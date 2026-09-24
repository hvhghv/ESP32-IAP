/*
 * ESP IAP - WiFi AP 模块
 *
 * 以 SoftAP 模式启动 WiFi，SSID/密码/信道/IP 段均从 IAP 配置区读取。
 * 启动成功后 HTTP 服务可通过 AP IP 访问。
 */

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*
 * WiFi 头文件仅在支持 WiFi 的芯片上包含。
 * ESP32-H2 等无 WiFi 芯片的 esp_wifi.h 会因缺少配置宏而编译失败。
 * SOC_WIFI_SUPPORTED 由 soc/soc_caps.h 提供。
 */
#if SOC_WIFI_SUPPORTED
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_event.h"
#include "freertos/event_groups.h"
#endif

#include "iap_common.h"
#include "iap_config.h"
#include "iap_wait_trigger.h"
#include "iap_wifi.h"

static const char *TAG = "iap_wifi";

/*
 * 芯片能力检查: WiFi
 *
 * 部分芯片 (如 ESP32-H2) 没有 WiFi 硬件 (仅 802.15.4/Thread)，
 * 此时 esp_wifi.h 中的配置宏不存在，编译会失败。
 *
 * 处理方式: 整个模块退化为空实现，iap_wifi_start() 返回
 * ESP_ERR_NOT_SUPPORTED，HTTP 服务也随之跳过。
 */
#if !SOC_WIFI_SUPPORTED

esp_err_t iap_wifi_start(void)
{
    ESP_LOGW(TAG, "本芯片不支持 WiFi，WiFi/HTTP 通道已禁用");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t iap_wifi_start_ex(bool nonblocking)
{
    (void)nonblocking;
    return iap_wifi_start();
}

const char *iap_wifi_get_ip(void)
{
    return "0.0.0.0";
}

esp_netif_t *iap_wifi_get_netif(void)
{
    return NULL;
}

esp_err_t iap_wifi_stop(void)
{
    return ESP_OK;
}

#else  /* SOC_WIFI_SUPPORTED */

/** AP 网卡句柄 */
static esp_netif_t *s_ap_netif = NULL;

/** 事件组: 用于等待 AP 启动完成 */
static EventGroupHandle_t s_wifi_events = NULL;

#define WIFI_AP_STARTED_BIT   BIT0

/** 当前 AP 的 IP 信息 (启动后填充) */
static char s_ap_ip[16] = "0.0.0.0";

/** 是否已启动 (用于 iap_wifi_stop() 幂等与资源释放) */
static bool s_wifi_started = false;

/** 已注册的事件处理器实例 (注销时需要) */
static esp_event_handler_instance_t s_wifi_evt_inst = NULL;

/* -------------------------------------------------------------------------- */
/* 事件处理                                                                    */
/* -------------------------------------------------------------------------- */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_AP_START:
            ESP_LOGI(TAG, "AP 已启动");
            if (s_wifi_events) {
                xEventGroupSetBits(s_wifi_events, WIFI_AP_STARTED_BIT);
            }
            break;

        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *evt =
                (wifi_event_ap_staconnected_t *)event_data;
            ESP_LOGI(TAG, "客户端接入: " MACSTR ", AID=%d",
                     MAC2STR(evt->mac), evt->aid);

            /* 若处于启动等待阶段，通知等待触发模块 */
            iap_wait_trigger_notify_wifi();
            break;
        }

        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *evt =
                (wifi_event_ap_stadisconnected_t *)event_data;
            ESP_LOGI(TAG, "客户端断开: " MACSTR ", AID=%d",
                     MAC2STR(evt->mac), evt->aid);
            break;
        }

        default:
            break;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* 初始化                                                                      */
/* -------------------------------------------------------------------------- */

esp_err_t iap_wifi_start(void)
{
    return iap_wifi_start_ex(false);
}

esp_err_t iap_wifi_start_ex(bool nonblocking)
{
    iap_cfg_data_t cfg;
    esp_err_t err = iap_config_get(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    if (!(cfg.flags & IAP_CFG_FLAG_WIFI_ENABLE)) {
        ESP_LOGI(TAG, "WiFi 未使能，跳过初始化");
        return ESP_OK;
    }

    /*
     * 幂等保护: WiFi 可能已在**等待触发阶段**启动过
     * (iap_wait_trigger 调用 iap_wifi_start_ex(true))。
     *
     * 此时若再次执行 esp_netif_create_default_wifi_ap()，会因
     * "WIFI_AP_DEF" 网卡已存在而触发 assert:
     *   assert failed: esp_netif_create_default_wifi_ap wifi_default.c:408
     *
     * 典型场景: 等待超时 -> 尝试启动用户程序 -> 启动失败
     *           (如 ESP_ERR_NOT_FOUND) -> 回落到 IAP 下载模式
     *           -> 再次 iap_wifi_start() -> 崩溃
     *
     * 因此这里检测到已初始化时直接复用，只补做"等待就绪"。
     */
    if (s_wifi_started && s_ap_netif != NULL) {
        ESP_LOGI(TAG, "WiFi 已启动，复用现有实例");
        if (nonblocking) {
            return ESP_OK;
        }
        if (s_wifi_events) {
            EventBits_t bits = xEventGroupWaitBits(
                s_wifi_events, WIFI_AP_STARTED_BIT, pdFALSE, pdTRUE,
                pdMS_TO_TICKS(10000));
            if (!(bits & WIFI_AP_STARTED_BIT)) {
                ESP_LOGW(TAG, "等待 AP 启动超时");
            }
        }
        return ESP_OK;
    }

    s_wifi_events = xEventGroupCreate();
    if (s_wifi_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* --- 网络栈初始化 --- */
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init 失败: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "创建事件循环失败: %s", esp_err_to_name(err));
        return err;
    }

    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_ap_netif == NULL) {
        ESP_LOGE(TAG, "创建 AP 网卡失败");
        return ESP_FAIL;
    }

    /* --- 设置静态 IP --- */
    esp_netif_ip_info_t ip_info = {0};
    ip_info.ip.addr      = cfg.wifi_ip;
    ip_info.gw.addr      = cfg.wifi_ip;
    ip_info.netmask.addr = cfg.wifi_netmask;

    /* 必须先停止 DHCP 服务才能修改 IP */
    esp_netif_dhcps_stop(s_ap_netif);
    err = esp_netif_set_ip_info(s_ap_netif, &ip_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "设置 AP IP 失败: %s", esp_err_to_name(err));
        return err;
    }

    /*
     * 启动 DHCP 服务器。
     *
     * 关于"不下发网关"的实现方式:
     *   这里刻意不使用 esp_netif_dhcps_option() 来关闭网关下发 ——
     *   该 API 在本场景下不可用:
     *     1) 启动前设置会被 dhcps_start() 重置为默认值 (含 OFFER_ROUTER);
     *     2) 启动后设置会返回 ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED。
     *
     *   实际由 main/lwipopts.h 中的 LWIP_HOOK_DHCPS_POST_STATE 钩子完成:
     *   在 DHCP 响应报文生成后、发送前，直接删除 option 3 (Router)
     *   与 option 6 (DNS)，客户端因此不会安装默认路由，
     *   也就不会把发往其他网络的流量错误地送到本设备。
     *
     *   客户端仍会正常获得 IP 与子网掩码 (option 1)，可访问本设备 HTTP 服务。
     */
    err = esp_netif_dhcps_start(s_ap_netif);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "启动 DHCP 服务失败: %s", esp_err_to_name(err));
    }

    iap_config_get_ip_string(s_ap_ip, sizeof(s_ap_ip));
    ESP_LOGI(TAG, "AP IP: %s (DHCP 不下发网关/DNS)", s_ap_ip);

    /* --- WiFi 初始化 --- */
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init 失败: %s", esp_err_to_name(err));
        return err;
    }

    /* 配置不写入 NVS，避免占用空间 */
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    /* 注册事件 */
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &wifi_event_handler, NULL,
                                              &s_wifi_evt_inst);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "注册 WiFi 事件失败: %s", esp_err_to_name(err));
        return err;
    }

    /* --- AP 配置 --- */
    wifi_config_t wifi_cfg = {0};
    memcpy(wifi_cfg.ap.ssid, cfg.wifi_ssid, sizeof(wifi_cfg.ap.ssid));
    wifi_cfg.ap.ssid_len = (uint8_t)strnlen((const char *)cfg.wifi_ssid,
                                            sizeof(wifi_cfg.ap.ssid));

    size_t pass_len = strnlen((const char *)cfg.wifi_password,
                              sizeof(cfg.wifi_password));
    if (pass_len >= 8) {
        memcpy(wifi_cfg.ap.password, cfg.wifi_password,
               sizeof(wifi_cfg.ap.password));
        wifi_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        wifi_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }

    wifi_cfg.ap.channel        = cfg.wifi_channel;
    wifi_cfg.ap.max_connection = 4;
    wifi_cfg.ap.beacon_interval = 100;
    wifi_cfg.ap.pmf_cfg.required = false;

    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "设置 WiFi 模式失败: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "设置 AP 配置失败: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "启动 WiFi 失败: %s", esp_err_to_name(err));
        return err;
    }

    s_wifi_started = true;

    /* 等待 AP 启动完成 */
    if (nonblocking) {
        /*
         * 非阻塞模式 (等待触发路径使用):
         * 不阻塞等待 AP 就绪，立即返回。
         * AP 启动完成后仍会触发 WIFI_EVENT_AP_START，
         * 客户端接入事件也会正常上报，不影响触发检测。
         */
        ESP_LOGI(TAG, "WiFi AP 启动中 (非阻塞模式)");
        return ESP_OK;
    }

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_AP_STARTED_BIT,
                                           pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(10000));
    if (!(bits & WIFI_AP_STARTED_BIT)) {
        ESP_LOGW(TAG, "等待 AP 启动超时");
    }

    ESP_LOGI(TAG, "WiFi AP 已就绪: SSID='%s', 信道 %u, IP %s",
             cfg.wifi_ssid, cfg.wifi_channel, s_ap_ip);

    return ESP_OK;
}

const char *iap_wifi_get_ip(void)
{
    return s_ap_ip;
}

esp_netif_t *iap_wifi_get_netif(void)
{
    return s_ap_netif;
}

esp_err_t iap_wifi_stop(void)
{
    if (!s_wifi_started && s_ap_netif == NULL) {
        /* 从未启动过，幂等返回 */
        return ESP_OK;
    }

    ESP_LOGI(TAG, "正在释放 WiFi / 网络栈资源...");

    /* 1. 停止 WiFi 协议栈 */
    if (s_wifi_started) {
        esp_wifi_stop();
        esp_wifi_deinit();
        s_wifi_started = false;
    }

    /* 2. 注销事件处理器 */
    if (s_wifi_evt_inst) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              s_wifi_evt_inst);
        s_wifi_evt_inst = NULL;
    }

    /* 3. 销毁 AP 网卡 —— 关键!
     *
     * esp_netif_create_default_wifi_ap() 创建的网卡被命名为 "WIFI_AP_DEF"。
     * 若不在跳转用户程序前销毁，用户程序再调用同一 API 会因网卡已存在
     * 而返回 NULL，导致用户程序无法建立自己的 WiFi。
     *
     * 销毁前需先停止 DHCP 服务，否则 esp_netif_destroy() 会残留
     * dhcps 任务与 socket。
     */
    if (s_ap_netif) {
        esp_netif_dhcps_stop(s_ap_netif);
        esp_netif_destroy(s_ap_netif);
        s_ap_netif = NULL;
    }

    /* 4. 删除事件组 */
    if (s_wifi_events) {
        vEventGroupDelete(s_wifi_events);
        s_wifi_events = NULL;
    }

    strncpy(s_ap_ip, "0.0.0.0", sizeof(s_ap_ip));

    ESP_LOGI(TAG, "WiFi 资源已释放");
    return ESP_OK;
}

#endif  /* SOC_WIFI_SUPPORTED */
