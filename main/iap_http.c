/*
 * ESP IAP - HTTP 应用
 *
 * 通过 WiFi AP 访问的简易 HTTP 服务，提供:
 *
 *   GET  /                 简易 Web 界面
 *   GET  /api/info         系统信息 (JSON)
 *   GET  /api/cfg          IAP 配置 (JSON)
 *   POST /api/cfg          修改配置 (JSON)
 *   GET  /api/app          用户程序信息 (JSON)
 *   POST /api/upload       上传用户程序文件 (原始二进制)
 *   GET  /api/download     下载用户程序区内容
 *   GET  /api/verify       校验用户程序
 *   POST /api/erase        擦除用户程序区
 *   POST /api/boot         重启进入用户程序
 *   POST /api/reboot       重启设备
 *
 * 上传的固件文件为**从 0x140000 开始的完整镜像** (v6，含分区表 B + 应用镜像)。
 * IAP 会整段写入可变区，与 esptool 语义一致。
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "iap_common.h"
#include "iap_crc.h"
#include "iap_config.h"
#include "iap_image.h"
#include "iap_wifi.h"
#include "iap_http.h"

static const char *TAG = "iap_http";

/** HTTP 服务器句柄 */
static httpd_handle_t s_server = NULL;

/** 上传缓冲大小 */
#define HTTP_UPLOAD_CHUNK   4096

/* -------------------------------------------------------------------------- */
/* 简易 Web 界面                                                               */
/* -------------------------------------------------------------------------- */
/*
 * 页面内容来自 main/iap_index.html，构建时由 main/CMakeLists.txt 中的
 * target_add_binary_data(..., TEXT) 嵌入，符号如下 (末尾含 NUL):
 *     iap_index_html_start / iap_index_html_end
 */
extern const char iap_index_html_start[] asm("_binary_iap_index_html_start");
extern const char iap_index_html_end[]   asm("_binary_iap_index_html_end");

/* -------------------------------------------------------------------------- */
/* 辅助                                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief 发送 JSON 响应
 */
static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, HTTPD_TYPE_JSON);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

/**
 * @brief 发送纯文本响应
 *
 * @param status HTTP 状态字符串，NULL 表示 200
 */
static esp_err_t send_text(httpd_req_t *req, const char *text, const char *status)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    if (status) {
        httpd_resp_set_status(req, status);
    }
    return httpd_resp_sendstr(req, text);
}

/**
 * @brief 从请求体读取全部内容到缓冲区
 *
 * @param req     请求
 * @param buf     输出缓冲区
 * @param buf_len 缓冲区长度
 * @return 实际读取长度，负值表示错误
 */
static int read_body(httpd_req_t *req, char *buf, size_t buf_len)
{
    if (req->content_len == 0 || req->content_len >= buf_len) {
        return -1;
    }
    int received = 0;
    while (received < (int)req->content_len) {
        int n = httpd_req_recv(req, buf + received, req->content_len - received);
        if (n <= 0) {
            if (n == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return -1;
        }
        received += n;
    }
    buf[received] = '\0';
    return received;
}

/* -------------------------------------------------------------------------- */
/* 处理函数: 页面                                                              */
/* -------------------------------------------------------------------------- */

static esp_err_t handler_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, iap_index_html_start,
                           iap_index_html_end - iap_index_html_start);
}

/* -------------------------------------------------------------------------- */
/* 处理函数: 系统信息                                                          */
/* -------------------------------------------------------------------------- */

static esp_err_t handler_info(httpd_req_t *req)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    const char *model;
    switch (chip.model) {
    case CHIP_ESP32:   model = "ESP32";    break;
    case CHIP_ESP32S2: model = "ESP32-S2"; break;
    case CHIP_ESP32S3: model = "ESP32-S3"; break;
    case CHIP_ESP32C3: model = "ESP32-C3"; break;
    case CHIP_ESP32C6: model = "ESP32-C6"; break;
    case CHIP_ESP32H2: model = "ESP32-H2"; break;
    default:           model = "未知";      break;
    }

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    iap_cfg_data_t cfg;
    iap_config_get(&cfg);

    char json[768];
    snprintf(json, sizeof(json),
        "{"
        "\"IAP 版本\":\"%s\","
        "\"芯片型号\":\"%s\","
        "\"芯片版本\":%d,"
        "\"核心数\":%d,"
        "\"Flash 大小\":\"%" PRIu32 " MB\","
        "\"IDF 版本\":\"%s\","
        "\"MAC 地址\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
        "\"空闲堆\":%" PRIu32 ","
        "\"最小空闲堆\":%" PRIu32 ","
        "\"运行时间\":\"%" PRIu64 " ms\","
        "\"AP IP\":\"%s\","
        "\"WiFi SSID\":\"%s\","
        "\"下载模式\":\"%s\","
        "\"上次启动原因\":\"%s\""
        "}",
        IAP_VERSION_STRING, model, chip.revision, chip.cores,
        flash_size / (1024 * 1024), esp_get_idf_version(),
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
        (uint32_t)esp_get_free_heap_size(),
        (uint32_t)esp_get_minimum_free_heap_size(),
        esp_timer_get_time() / 1000,
        iap_wifi_get_ip(), (const char *)cfg.wifi_ssid,
        (cfg.flags & IAP_CFG_FLAG_DOWNLOAD_MODE) ? "开" : "关",
        iap_boot_reason_str((iap_boot_reason_t)cfg.last_boot_reason));

    return send_json(req, json);
}

/* -------------------------------------------------------------------------- */
/* 处理函数: 配置                                                              */
/* -------------------------------------------------------------------------- */

static esp_err_t handler_cfg_get(httpd_req_t *req)
{
    iap_cfg_data_t cfg;
    if (iap_config_get(&cfg) != ESP_OK) {
        return send_text(req, "读取配置失败", HTTPD_500);
    }

    char ip[16], mask[16];
    iap_config_get_ip_string(ip, sizeof(ip));
    iap_config_get_netmask_string(mask, sizeof(mask));

    /* 缓冲区需容纳启动参数全文 (最多 255 字符) 及其它配置项 */
    char json[IAP_CFG_BOOT_PARAM_SIZE + 768];
    snprintf(json, sizeof(json),
        "{"
        "\"标志\":\"0x%08" PRIx32 "\","
        "\"下载模式\":\"%s\","
        "\"WiFi\":\"%s\","
        "\"I2C\":\"%s\","
        "\"UART\":\"%s\","
        "\"启动校验\":\"%s\","
        "\"等待时间\":%u,"
        "\"用户程序长度\":%" PRIu32 ","
        "\"用户程序CRC\":\"0x%08" PRIx32 "\","
        "\"启动计数\":%" PRIu32 ","
        "\"上次原因\":\"%s\","
        "\"I2C SCL\":%u,"
        "\"I2C SDA\":%u,"
        "\"I2C 地址\":\"0x%02X\","
        "\"WiFi SSID\":\"%s\","
        "\"WiFi 信道\":%u,"
        "\"AP IP\":\"%s\","
        "\"AP 掩码\":\"%s\","
        "\"UART 端口\":%u,"
        "\"UART TX\":%u,"
        "\"UART RX\":%u,"
        "\"UART 波特率\":%" PRIu32 ","
        "\"启动参数\":\"%s\","
        "\"启动参数长度\":%u,"
        "\"启动参数已消费\":\"%s\""
        "}",
        cfg.flags,
        (cfg.flags & IAP_CFG_FLAG_DOWNLOAD_MODE) ? "开" : "关",
        (cfg.flags & IAP_CFG_FLAG_WIFI_ENABLE) ? "开" : "关",
        (cfg.flags & IAP_CFG_FLAG_I2C_ENABLE) ? "开" : "关",
        (cfg.flags & IAP_CFG_FLAG_UART_ENABLE) ? "开" : "关",
        (cfg.flags & IAP_CFG_FLAG_VERIFY_USER_APP) ? "开(bootloader)" : "关",
        cfg.wait_seconds, cfg.user_app_size, cfg.user_app_crc32,
        cfg.boot_count,
        iap_boot_reason_str((iap_boot_reason_t)cfg.last_boot_reason),
        cfg.i2c_scl_gpio, cfg.i2c_sda_gpio, cfg.i2c_addr,
        (const char *)cfg.wifi_ssid, cfg.wifi_channel,
        ip, mask, cfg.uart_port, cfg.uart_tx_gpio, cfg.uart_rx_gpio,
        cfg.uart_baudrate,
        (cfg.boot_param_flags & IAP_CFG_BOOT_PARAM_FLAG_VALID)
            ? (const char *)cfg.boot_param : "",
        (unsigned)cfg.boot_param_len,
        (cfg.boot_param_flags & IAP_CFG_BOOT_PARAM_FLAG_CONSUMED) ? "是" : "否");

    return send_json(req, json);
}

/**
 * @brief 修改配置
 *
 * 请求体为查询字符串格式: key=value&key=value
 * 支持 key: wait, ssid, pass, channel, ip, mask, scl, sda, i2caddr,
 *           uartport, uarttx, uartrx, baud, download, wifi, i2c, verify
 */
static esp_err_t handler_cfg_post(httpd_req_t *req)
{
    char body[512];
    if (read_body(req, body, sizeof(body)) < 0) {
        return send_text(req, "请求体无效", HTTPD_400);
    }

    iap_cfg_data_t cfg;
    if (iap_config_get(&cfg) != ESP_OK) {
        return send_text(req, "读取配置失败", HTTPD_500);
    }

    int changed = 0;
    char *saveptr = NULL;
    char *pair = strtok_r(body, "&", &saveptr);

    while (pair) {
        char *eq = strchr(pair, '=');
        if (eq) {
            *eq = '\0';
            const char *key = pair;
            const char *val = eq + 1;

            if (strcmp(key, "wait") == 0) {
                cfg.wait_seconds = (uint16_t)strtoul(val, NULL, 0);
                changed++;
            } else if (strcmp(key, "ssid") == 0) {
                memset(cfg.wifi_ssid, 0, sizeof(cfg.wifi_ssid));
                strncpy((char *)cfg.wifi_ssid, val, sizeof(cfg.wifi_ssid) - 1);
                changed++;
            } else if (strcmp(key, "pass") == 0) {
                memset(cfg.wifi_password, 0, sizeof(cfg.wifi_password));
                strncpy((char *)cfg.wifi_password, val, sizeof(cfg.wifi_password) - 1);
                changed++;
            } else if (strcmp(key, "channel") == 0) {
                cfg.wifi_channel = (uint8_t)strtoul(val, NULL, 0);
                changed++;
            } else if (strcmp(key, "ip") == 0) {
                unsigned a, b, c, d;
                if (sscanf(val, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
                    cfg.wifi_ip = a | (b << 8) | (c << 16) | ((uint32_t)d << 24);
                    changed++;
                }
            } else if (strcmp(key, "mask") == 0) {
                unsigned a, b, c, d;
                if (sscanf(val, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
                    cfg.wifi_netmask = a | (b << 8) | (c << 16) | ((uint32_t)d << 24);
                    changed++;
                }
            } else if (strcmp(key, "scl") == 0) {
                cfg.i2c_scl_gpio = (uint8_t)strtoul(val, NULL, 0);
                changed++;
            } else if (strcmp(key, "sda") == 0) {
                cfg.i2c_sda_gpio = (uint8_t)strtoul(val, NULL, 0);
                changed++;
            } else if (strcmp(key, "i2caddr") == 0) {
                cfg.i2c_addr = (uint8_t)strtoul(val, NULL, 0);
                changed++;
            } else if (strcmp(key, "uartport") == 0) {
                cfg.uart_port = (uint8_t)strtoul(val, NULL, 0);
                changed++;
            } else if (strcmp(key, "uarttx") == 0) {
                cfg.uart_tx_gpio = (strcmp(val, "default") == 0)
                                   ? IAP_CFG_UART_PIN_DEFAULT
                                   : (uint8_t)strtoul(val, NULL, 0);
                changed++;
            } else if (strcmp(key, "uartrx") == 0) {
                cfg.uart_rx_gpio = (strcmp(val, "default") == 0)
                                   ? IAP_CFG_UART_PIN_DEFAULT
                                   : (uint8_t)strtoul(val, NULL, 0);
                changed++;
            } else if (strcmp(key, "baud") == 0) {
                cfg.uart_baudrate = strtoul(val, NULL, 0);
                changed++;
            } else if (strcmp(key, "download") == 0) {
                if (strcmp(val, "on") == 0 || strcmp(val, "1") == 0) {
                    cfg.flags |= IAP_CFG_FLAG_DOWNLOAD_MODE;
                } else {
                    cfg.flags &= ~IAP_CFG_FLAG_DOWNLOAD_MODE;
                }
                changed++;
            } else if (strcmp(key, "wifi") == 0) {
                if (strcmp(val, "on") == 0 || strcmp(val, "1") == 0) {
                    cfg.flags |= IAP_CFG_FLAG_WIFI_ENABLE;
                } else {
                    cfg.flags &= ~IAP_CFG_FLAG_WIFI_ENABLE;
                }
                changed++;
            } else if (strcmp(key, "i2c") == 0) {
                if (strcmp(val, "on") == 0 || strcmp(val, "1") == 0) {
                    cfg.flags |= IAP_CFG_FLAG_I2C_ENABLE;
                } else {
                    cfg.flags &= ~IAP_CFG_FLAG_I2C_ENABLE;
                }
                changed++;
            } else if (strcmp(key, "verify") == 0) {
                if (strcmp(val, "on") == 0 || strcmp(val, "1") == 0) {
                    cfg.flags |= IAP_CFG_FLAG_VERIFY_USER_APP;
                } else {
                    cfg.flags &= ~IAP_CFG_FLAG_VERIFY_USER_APP;
                }
                changed++;
            }
        }
        pair = strtok_r(NULL, "&", &saveptr);
    }

    if (changed == 0) {
        return send_text(req, "没有可修改的配置项", HTTPD_400);
    }

    if (iap_config_set(&cfg) != ESP_OK) {
        return send_text(req, "保存配置失败", HTTPD_500);
    }

    char msg[64];
    snprintf(msg, sizeof(msg), "已更新 %d 项配置", changed);
    return send_text(req, msg, NULL);
}

/* -------------------------------------------------------------------------- */
/* 处理函数: 完整配置读写 (Web 控制台配置编辑器)                               */
/* -------------------------------------------------------------------------- */

/**
 * @brief 在 JSON 字符串中查找字段的整数值
 *
 * 支持 "key":123 与 "key":"0x1F" 两种形式 (后者按 16 进制解析)。
 * 仅做轻量解析，避免引入完整 JSON 库。
 *
 * @param json  JSON 文本
 * @param key   字段名
 * @param[out] out 解析结果
 * @return true 找到并解析成功
 */
static bool json_get_u32(const char *json, const char *key, uint32_t *out)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (p == NULL) {
        return false;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == ':') {
        p++;
    }
    if (*p == '"') {
        p++;
        *out = (uint32_t)strtoul(p, NULL, 0);
    } else {
        *out = (uint32_t)strtoul(p, NULL, 10);
    }
    return true;
}

/**
 * @brief 在 JSON 中查找字段的字符串值 (写入 buf)
 *
 * 处理 \" 与 \\ 转义。返回值不含引号。
 *
 * @return true 找到并复制成功
 */
static bool json_get_str(const char *json, const char *key,
                         char *buf, size_t buf_len)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (p == NULL) {
        return false;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == ':') {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    p++;

    size_t n = 0;
    while (*p && *p != '"' && n + 1 < buf_len) {
        if (*p == '\\' && (p[1] == '"' || p[1] == '\\')) {
            p++;
        }
        buf[n++] = *p++;
    }
    buf[n] = '\0';
    return true;
}

/**
 * @brief 把 JSON 字符串转义后写入缓冲区
 *
 * @return 写入的字符数 (不含结尾 '\0')
 */
static int json_escape(const char *src, char *dst, size_t dst_len)
{
    size_t n = 0;
    for (const char *p = src; *p && n + 2 < dst_len; p++) {
        if (*p == '"' || *p == '\\') {
            dst[n++] = '\\';
        }
        dst[n++] = *p;
    }
    dst[n] = '\0';
    return (int)n;
}

/**
 * @brief GET /api/cfg/full —— 返回完整配置 (JSON，字段名与工具一致)
 *
 * 相比 /api/cfg (面向人的中文摘要)，本接口返回**可回写**的原始字段，
 * 供 Web 控制台配置编辑器直接读取/修改/回写。
 */
static esp_err_t handler_cfg_full_get(httpd_req_t *req)
{
    iap_cfg_data_t cfg;
    if (iap_config_get(&cfg) != ESP_OK) {
        return send_text(req, "读取配置失败", HTTPD_500);
    }

    char ip[16], mask[16];
    iap_config_get_ip_string(ip, sizeof(ip));
    iap_config_get_netmask_string(mask, sizeof(mask));

    /* 字符串字段转义后需额外空间 */
    static char ssid[80], pass[144], bparam[IAP_CFG_BOOT_PARAM_SIZE * 2 + 8];
    json_escape((const char *)cfg.wifi_ssid, ssid, sizeof(ssid));
    json_escape((const char *)cfg.wifi_password, pass, sizeof(pass));
    json_escape((const char *)cfg.boot_param, bparam, sizeof(bparam));

    static char json[2048];
    int n = snprintf(json, sizeof(json),
        "{"
        "\"flags\":%" PRIu32 ","
        "\"wait_seconds\":%u,"
        "\"wifi_ssid\":\"%s\","
        "\"wifi_password\":\"%s\","
        "\"wifi_channel\":%u,"
        "\"wifi_ip\":\"%s\","
        "\"wifi_netmask\":\"%s\","
        "\"i2c_scl_gpio\":%u,"
        "\"i2c_sda_gpio\":%u,"
        "\"i2c_addr\":%u,"
        "\"uart_port\":%u,"
        "\"uart_tx_gpio\":%u,"
        "\"uart_rx_gpio\":%u,"
        "\"uart_baudrate\":%" PRIu32 ","
        "\"trig_gpio\":%u,"
        "\"trig_gpio_level\":%u,"
        "\"boot_param\":\"%s\","
        "\"active_slot\":%u,"
        "\"ota_slot_count\":%u,"
        "\"ota_gpio\":%u,"
        "\"slot_addr\":[",
        cfg.flags, cfg.wait_seconds,
        ssid, pass, cfg.wifi_channel, ip, mask,
        cfg.i2c_scl_gpio, cfg.i2c_sda_gpio, cfg.i2c_addr,
        cfg.uart_port, cfg.uart_tx_gpio, cfg.uart_rx_gpio, cfg.uart_baudrate,
        cfg.trig_gpio, cfg.trig_gpio_level, bparam,
        cfg.active_slot, cfg.ota_slot_count, cfg.ota_gpio);

    for (int i = 0; i < IAP_OTA_SLOT_MAX && n > 0 && n < (int)sizeof(json) - 32; i++) {
        n += snprintf(json + n, sizeof(json) - n, "%s%" PRIu32,
                      i ? "," : "", (uint32_t)cfg.slot_addr[i]);
    }
    if (n > 0 && n < (int)sizeof(json) - 2) {
        n += snprintf(json + n, sizeof(json) - n, "],\"slot_size\":[");
    }
    for (int i = 0; i < IAP_OTA_SLOT_MAX && n > 0 && n < (int)sizeof(json) - 32; i++) {
        n += snprintf(json + n, sizeof(json) - n, "%s%" PRIu32,
                      i ? "," : "", (uint32_t)cfg.slot_size[i]);
    }
    if (n > 0 && n < (int)sizeof(json) - 2) {
        n += snprintf(json + n, sizeof(json) - n, "]}");
    }

    return send_json(req, json);
}

/**
 * @brief POST /api/cfg/full —— 写入完整配置 (JSON)
 *
 * 请求体字段与 GET 响应一致，**仅覆盖出现的字段**，其余保持原值。
 * 写入后由 iap_config_set() 内部的 sanitize() 做合法性修正。
 *
 * 特例: 传 "reboot":true 时保存后重启设备 (WiFi 参数变更需重启生效)。
 */
static esp_err_t handler_cfg_full_post(httpd_req_t *req)
{
    static char body[1536];
    int len = read_body(req, body, sizeof(body));
    if (len <= 0) {
        return send_text(req, "请求体无效", HTTPD_400);
    }

    iap_cfg_data_t cfg;
    if (iap_config_get(&cfg) != ESP_OK) {
        return send_text(req, "读取配置失败", HTTPD_500);
    }

    uint32_t v;
    char tmp[IAP_CFG_BOOT_PARAM_SIZE];

    if (json_get_u32(body, "flags", &v))            cfg.flags = v;
    if (json_get_u32(body, "wait_seconds", &v))     cfg.wait_seconds = (uint16_t)v;
    if (json_get_str(body, "wifi_ssid", tmp, sizeof(tmp))) {
        memset(cfg.wifi_ssid, 0, sizeof(cfg.wifi_ssid));
        strncpy((char *)cfg.wifi_ssid, tmp, sizeof(cfg.wifi_ssid) - 1);
    }
    if (json_get_str(body, "wifi_password", tmp, sizeof(tmp))) {
        memset(cfg.wifi_password, 0, sizeof(cfg.wifi_password));
        strncpy((char *)cfg.wifi_password, tmp, sizeof(cfg.wifi_password) - 1);
    }
    if (json_get_u32(body, "wifi_channel", &v))     cfg.wifi_channel = (uint8_t)v;
    if (json_get_str(body, "wifi_ip", tmp, sizeof(tmp))) {
        unsigned a, b, c, d;
        if (sscanf(tmp, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
            cfg.wifi_ip = a | (b << 8) | (c << 16) | ((uint32_t)d << 24);
        }
    }
    if (json_get_str(body, "wifi_netmask", tmp, sizeof(tmp))) {
        unsigned a, b, c, d;
        if (sscanf(tmp, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
            cfg.wifi_netmask = a | (b << 8) | (c << 16) | ((uint32_t)d << 24);
        }
    }
    if (json_get_u32(body, "i2c_scl_gpio", &v))     cfg.i2c_scl_gpio = (uint8_t)v;
    if (json_get_u32(body, "i2c_sda_gpio", &v))     cfg.i2c_sda_gpio = (uint8_t)v;
    if (json_get_u32(body, "i2c_addr", &v))         cfg.i2c_addr = (uint8_t)v;
    if (json_get_u32(body, "uart_port", &v))        cfg.uart_port = (uint8_t)v;
    if (json_get_u32(body, "uart_tx_gpio", &v))     cfg.uart_tx_gpio = (uint8_t)v;
    if (json_get_u32(body, "uart_rx_gpio", &v))     cfg.uart_rx_gpio = (uint8_t)v;
    if (json_get_u32(body, "uart_baudrate", &v))    cfg.uart_baudrate = v;
    if (json_get_u32(body, "trig_gpio", &v))        cfg.trig_gpio = (uint8_t)v;
    if (json_get_u32(body, "trig_gpio_level", &v))  cfg.trig_gpio_level = (uint8_t)v;
    if (json_get_str(body, "boot_param", tmp, sizeof(tmp))) {
        memset(cfg.boot_param, 0, sizeof(cfg.boot_param));
        strncpy((char *)cfg.boot_param, tmp, sizeof(cfg.boot_param) - 1);
    }
    if (json_get_u32(body, "active_slot", &v))      cfg.active_slot = (uint8_t)v;
    if (json_get_u32(body, "ota_slot_count", &v))   cfg.ota_slot_count = (uint8_t)v;
    if (json_get_u32(body, "ota_gpio", &v))         cfg.ota_gpio = (uint8_t)v;

    /* 槽地址/大小数组: "slot_addr":[a0,a1,...] */
    const char *pa = strstr(body, "\"slot_addr\"");
    if (pa != NULL) {
        pa = strchr(pa, '[');
        if (pa != NULL) {
            pa++;
            for (int i = 0; i < IAP_OTA_SLOT_MAX && *pa; i++) {
                while (*pa == ' ' || *pa == ',') pa++;
                if (*pa == ']') break;
                char *end = NULL;
                cfg.slot_addr[i] = strtoull(pa, &end, 10);
                if (end == pa) break;   /* 非数字，停止 */
                pa = end;
            }
        }
    }
    const char *ps = strstr(body, "\"slot_size\"");
    if (ps != NULL) {
        ps = strchr(ps, '[');
        if (ps != NULL) {
            ps++;
            for (int i = 0; i < IAP_OTA_SLOT_MAX && *ps; i++) {
                while (*ps == ' ' || *ps == ',') ps++;
                if (*ps == ']') break;
                char *end = NULL;
                cfg.slot_size[i] = strtoull(ps, &end, 10);
                if (end == ps) break;
                ps = end;
            }
        }
    }

    if (iap_config_set(&cfg) != ESP_OK) {
        return send_text(req, "保存配置失败", HTTPD_500);
    }

    /* 可选: 保存后重启 (WiFi/UART 等参数需重启生效) */
    /* 支持 "reboot":true 与 "reboot":1 两种写法 */
    bool do_reboot = (strstr(body, "\"reboot\":true") != NULL);
    if (!do_reboot) {
        uint32_t rb = 0;
        if (json_get_u32(body, "reboot", &rb) && rb != 0) {
            do_reboot = true;
        }
    }
    if (do_reboot) {
        send_text(req, "配置已保存，正在重启...", HTTPD_200);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }

    return send_text(req, "配置已保存", NULL);
}

/* -------------------------------------------------------------------------- */
/* 处理函数: 启动参数                                                          */
/* -------------------------------------------------------------------------- */

/**
 * @brief 读取启动参数
 *
 * 响应 JSON: { "param": "...", "length": N, "consumed": "是/否" }
 */
static esp_err_t handler_bootparam_get(httpd_req_t *req)
{
    static char param[IAP_CFG_BOOT_PARAM_SIZE];
    esp_err_t err = iap_config_get_boot_param(param, sizeof(param));

    /* 缓冲区需容纳参数全文 + JSON 包装 */
    static char json[IAP_CFG_BOOT_PARAM_SIZE + 128];
    if (err == ESP_OK) {
        snprintf(json, sizeof(json),
            "{\"param\":\"%s\",\"length\":%u,\"consumed\":\"%s\"}",
            param, (unsigned)strlen(param),
            iap_config_boot_param_consumed() ? "是" : "否");
    } else if (err == ESP_ERR_NOT_FOUND) {
        snprintf(json, sizeof(json),
            "{\"param\":\"\",\"length\":0,\"consumed\":\"否\",\"状态\":\"未设置\"}");
    } else {
        snprintf(json, sizeof(json),
            "{\"param\":\"\",\"length\":0,\"状态\":\"读取失败: %s\"}",
            esp_err_to_name(err));
        return send_json(req, json);
    }

    return send_json(req, json);
}

/**
 * @brief 设置启动参数
 *
 * 请求体为参数字符串原文，例如:
 *   mode=debug;baud=115200
 *
 * 也支持查询参数形式: /api/bootparam?set=mode%3Ddebug
 */
static esp_err_t handler_bootparam_set(httpd_req_t *req)
{
    static char body[IAP_CFG_BOOT_PARAM_SIZE];
    int len = read_body(req, body, sizeof(body));

    if (len <= 0) {
        /* 退化: 尝试从查询参数读取 */
        static char query[IAP_CFG_BOOT_PARAM_SIZE + 32];
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
            static char val[IAP_CFG_BOOT_PARAM_SIZE];
            if (httpd_query_key_value(query, "set", val, sizeof(val)) == ESP_OK) {
                snprintf(body, sizeof(body), "%s", val);
                len = (int)strlen(body);
            }
        }
    }

    if (len <= 0) {
        return send_text(req, "请求体为空", HTTPD_400);
    }

    esp_err_t err = iap_config_set_boot_param(body);
    if (err == ESP_ERR_INVALID_SIZE) {
        static char msg[96];
        snprintf(msg, sizeof(msg), "启动参数过长 (上限 %d 字符)",
                 IAP_CFG_BOOT_PARAM_MAX_LEN);
        return send_text(req, msg, HTTPD_400);
    }
    if (err != ESP_OK) {
        return send_text(req, "设置启动参数失败", HTTPD_500);
    }

    static char msg[IAP_CFG_BOOT_PARAM_SIZE + 32];
    snprintf(msg, sizeof(msg), "启动参数已设置: %s", body);
    return send_text(req, msg, NULL);
}

/**
 * @brief 清除启动参数
 */
static esp_err_t handler_bootparam_clear(httpd_req_t *req)
{
    esp_err_t err = iap_config_clear_boot_param();
    if (err != ESP_OK) {
        return send_text(req, "清除启动参数失败", HTTPD_500);
    }
    return send_text(req, "启动参数已清除", NULL);
}

/* -------------------------------------------------------------------------- */
/* 处理函数: 用户程序信息                                                      */
/* -------------------------------------------------------------------------- */

static esp_err_t handler_app(httpd_req_t *req)
{
    iap_image_info_t info;
    iap_image_get_info(&info);

    uint8_t slot = iap_image_get_active_slot();
    const esp_partition_t *part = iap_image_get_slot(slot);

    char json[512];
    if (!info.present) {
        snprintf(json, sizeof(json),
            "{"
            "\"状态\":\"无镜像\","
            "\"活动槽\":%u,"
            "\"槽总数\":%u,"
            "\"分区地址\":\"0x%08" PRIx32 "\","
            "\"分区大小\":%" PRIu32
            "}",
            (unsigned)slot, (unsigned)iap_image_get_slot_count(),
            part ? part->address : 0, part ? part->size : 0);
    } else {
        snprintf(json, sizeof(json),
            "{"
            "\"状态\":\"存在镜像\","
            "\"活动槽\":%u,"
            "\"槽总数\":%u,"
            "\"版本号\":%" PRIu32 ","
            "\"已记录长度\":%" PRIu32 ","
            "\"已记录CRC32\":\"0x%08" PRIx32 "\","
            "\"分区大小\":%" PRIu32 ","
            "\"说明\":\"完整性由 bootloader 启动时校验\""
            "}",
            (unsigned)slot, (unsigned)iap_image_get_slot_count(),
            info.app_version, info.image_size, info.image_crc32,
            part ? part->size : 0);
    }

    return send_json(req, json);
}

/**
 * @brief GET /api/slots —— 列出所有 OTA 槽
 */
static esp_err_t handler_slots(httpd_req_t *req)
{
    uint8_t count = iap_image_get_slot_count();
    uint8_t active = iap_image_get_active_slot();

    char json[1024];
    int n = snprintf(json, sizeof(json),
                     "{\"槽总数\":%u,\"活动槽\":%u,\"槽\":[",
                     (unsigned)count, (unsigned)active);

    for (uint8_t i = 0; i < count && n > 0 && n < (int)sizeof(json) - 128; i++) {
        const esp_partition_t *p = iap_image_get_slot(i);
        if (p == NULL) {
            n += snprintf(json + n, sizeof(json) - n,
                          "%s{\"序号\":%u,\"状态\":\"缺失\"}",
                          i ? "," : "", (unsigned)i);
            continue;
        }
        n += snprintf(json + n, sizeof(json) - n,
                      "%s{\"序号\":%u,\"地址\":\"0x%08" PRIx32 "\","
                      "\"大小\":%" PRIu32 ",\"有镜像\":%s,\"活动\":%s}",
                      i ? "," : "", (unsigned)i, p->address, p->size,
                      iap_image_slot_present(i) ? "true" : "false",
                      (i == active) ? "true" : "false");
    }

    if (n > 0 && n < (int)sizeof(json) - 2) {
        n += snprintf(json + n, sizeof(json) - n, "]}");
    }
    return send_json(req, json);
}

/**
 * @brief POST /api/slot —— 切换活动 OTA 槽
 *
 * 请求体: {"slot":1} 或 {"slot":1,"boot":true}
 */
static esp_err_t handler_slot_set(httpd_req_t *req)
{
    char body[128];
    int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof(body)) {
        return send_text(req, "请求体无效", HTTPD_400);
    }
    if (httpd_req_recv(req, body, len) <= 0) {
        return send_text(req, "读取请求体失败", HTTPD_400);
    }
    body[len] = '\0';

    /* 解析 slot 字段 */
    const char *p = strstr(body, "\"slot\"");
    if (p == NULL) {
        return send_text(req, "缺少 slot 字段", HTTPD_400);
    }
    p = strchr(p, ':');
    if (p == NULL) {
        return send_text(req, "slot 字段格式错误", HTTPD_400);
    }
    long slot = strtol(p + 1, NULL, 10);

    uint8_t count = iap_image_get_slot_count();
    if (slot < 0 || slot >= count) {
        char msg[64];
        snprintf(msg, sizeof(msg), "槽号超出范围 (0 ~ %u)", (unsigned)(count - 1));
        return send_text(req, msg, HTTPD_400);
    }

    esp_err_t err = iap_image_set_active_slot((uint8_t)slot);
    if (err != ESP_OK) {
        return send_text(req, "设置活动槽失败", HTTPD_500);
    }

    /* 是否需要立即重启进入该槽 */
    bool do_boot = (strstr(body, "\"boot\"") != NULL &&
                    strstr(body, "true") != NULL);

    if (do_boot) {
        /*
         * 槽不存在或无镜像时，iap_image_boot_slot() 会自动回落到 IAP
         * (factory)，不会卡死。这里如实告知调用方。
         */
        bool ok = iap_image_slot_present((uint8_t)slot);
        if (ok) {
            send_text(req, "已切换活动槽，正在重启...", HTTPD_200);
        } else {
            send_text(req,
                      "该槽无镜像，将回落到 IAP 下载模式并重启...",
                      HTTPD_200);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
        iap_image_boot_slot((uint8_t)slot);
        return ESP_OK;
    }

    char msg[96];
    snprintf(msg, sizeof(msg), "活动槽已切换为 %ld (重启后生效)", slot);
    return send_text(req, msg, HTTPD_200);
}

/* -------------------------------------------------------------------------- */
/* 处理函数: 上传                                                              */
/* -------------------------------------------------------------------------- */

/**
 * @brief 从查询串中解析地址/长度参数
 *
 * 支持 addr / address / offset 三个别名 (地址) 与 length / size (长度)。
 * 数值支持 0x 前缀十进制/十六进制。
 *
 * @param[in]  req       请求
 * @param[out] addr      起始地址 (未提供时置 0)
 * @param[out] has_addr  是否提供了地址
 * @param[out] length    长度 (未提供时置 0)
 * @param[out] has_len   是否提供了长度
 */
static void parse_addr_len_query(httpd_req_t *req,
                                 uint32_t *addr, bool *has_addr,
                                 uint32_t *length, bool *has_len)
{
    *addr = 0; *has_addr = false;
    *length = 0; *has_len = false;

    char query[160];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return;
    }

    char val[32];
    const char *addr_keys[] = { "addr", "address", "offset" };
    for (size_t i = 0; i < sizeof(addr_keys) / sizeof(addr_keys[0]); i++) {
        if (httpd_query_key_value(query, addr_keys[i], val, sizeof(val)) == ESP_OK) {
            *addr = (uint32_t)strtoul(val, NULL, 0);
            *has_addr = true;
            break;
        }
    }

    const char *len_keys[] = { "length", "size" };
    for (size_t i = 0; i < sizeof(len_keys) / sizeof(len_keys[0]); i++) {
        if (httpd_query_key_value(query, len_keys[i], val, sizeof(val)) == ESP_OK) {
            *length = (uint32_t)strtoul(val, NULL, 0);
            *has_len = true;
            break;
        }
    }
}

static esp_err_t handler_upload(httpd_req_t *req)
{
    uint32_t total = req->content_len;
    if (total == 0) {
        return send_text(req, "请求体为空", HTTPD_400);
    }

    /* --- 容量校验 --- */
    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK || flash_size == 0) {
        return send_text(req, "读取 flash 容量失败", HTTPD_500);
    }

    /*
     * 解析写入地址与长度:
     *   - addr 缺省: 0x140000 (可变区起点)
     *   - length 缺省: 文件实际长度 (total)
     *   - length 提供: 只写入前 length 字节 (超出文件长度则报错)
     */
    uint32_t addr = 0, want_len = 0;
    bool has_addr = false, has_len = false;
    parse_addr_len_query(req, &addr, &has_addr, &want_len, &has_len);

    if (!has_addr) {
        addr = IAP_FIXED_REGION_END;
    }
    if (!has_len) {
        want_len = total;
    }

    /* 地址必须落在可变区且 4KB 对齐 */
    if (addr < IAP_FIXED_REGION_END) {
        char m[128];
        snprintf(m, sizeof(m),
                 "写入地址 0x%08" PRIx32 " 非法（须 >= 0x%06X，避免破坏固定区）",
                 addr, (unsigned)IAP_FIXED_REGION_END);
        return send_text(req, m, HTTPD_400);
    }
    if (addr & 0xFFF) {
        return send_text(req, "写入地址须 4KB 对齐", HTTPD_400);
    }
    if (addr >= flash_size) {
        return send_text(req, "写入地址超出 flash 容量", HTTPD_400);
    }
    if (want_len == 0) {
        return send_text(req, "写入长度不能为 0", HTTPD_400);
    }
    if (want_len > total) {
        char m[128];
        snprintf(m, sizeof(m),
                 "写入长度 %" PRIu32 " 超出文件长度 %" PRIu32, want_len, total);
        return send_text(req, m, HTTPD_400);
    }
    if (addr + want_len > flash_size) {
        char m[128];
        snprintf(m, sizeof(m),
                 "写入范围 0x%08" PRIx32 " + %" PRIu32 " 超出 flash 容量",
                 addr, want_len);
        return send_text(req, m, HTTPD_400);
    }

    ESP_LOGI(TAG, "开始接收上传: %" PRIu32 " 字节 → 写入 0x%08" PRIx32
             "，长度 %" PRIu32 " 字节", total, addr, want_len);

    /* --- 擦除目标区域 (按扇区对齐, 覆盖到实际写入范围) --- */
    uint32_t erase_len = (want_len + 0xFFF) & ~0xFFFu;
    if (addr + erase_len > flash_size) {
        erase_len = flash_size - addr;
    }
    if (esp_flash_erase_region(NULL, addr, erase_len) != ESP_OK) {
        return send_text(req, "擦除目标区域失败", HTTPD_500);
    }

    uint8_t *buf = malloc(HTTP_UPLOAD_CHUNK);
    if (buf == NULL) {
        return send_text(req, "内存不足", HTTPD_500);
    }

    /*
     * 上传流默认是**从 0x140000 开始的完整镜像**（分区表 B + 应用镜像），
     * 从 addr 起顺序写入。镜像完整性由 bootloader 在启动时校验。
     */
    iap_crc32_ctx_t crc;
    iap_crc32_ctx_init(&crc);

    uint32_t received = 0;   /* 已从 socket 接收的字节数 */
    uint32_t written = 0;    /* 已写入 flash 的字节数 */
    bool failed = false;

    while (received < total) {
        uint32_t remain = total - received;
        size_t want = (remain > HTTP_UPLOAD_CHUNK) ? HTTP_UPLOAD_CHUNK : remain;

        int n = httpd_req_recv(req, (char *)buf, want);
        if (n <= 0) {
            if (n == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            ESP_LOGE(TAG, "接收数据失败: %d", n);
            failed = true;
            break;
        }
        received += n;

        /* 只写入 want_len 字节，其余丢弃 (但仍需读空 socket) */
        if (written >= want_len) {
            continue;
        }
        size_t to_write = n;
        if (written + to_write > want_len) {
            to_write = want_len - written;
        }

        if (esp_flash_write(NULL, buf, addr + written, to_write) != ESP_OK) {
            ESP_LOGE(TAG, "写入 flash 失败 @0x%" PRIx32, addr + written);
            failed = true;
            break;
        }

        iap_crc32_ctx_update(&crc, buf, to_write);
        written += to_write;

        /* 每 64KB 让出 CPU */
        if ((written & 0xFFFF) < HTTP_UPLOAD_CHUNK) {
            vTaskDelay(1);
        }
    }

    free(buf);

    if (failed) {
        return send_text(req, "上传失败", HTTPD_500);
    }
    if (written != want_len) {
        char m[128];
        snprintf(m, sizeof(m),
                 "写入不完整: 期望 %" PRIu32 " 字节，实际 %" PRIu32 " 字节",
                 want_len, written);
        return send_text(req, m, HTTPD_500);
    }

    uint32_t calc_crc = iap_crc32_ctx_final(&crc);
    ESP_LOGI(TAG, "上传完成: 写入 %" PRIu32 " 字节 @0x%08" PRIx32
             ", CRC32 0x%08" PRIx32, written, addr, calc_crc);

    /*
     * 默认地址 (0x140000) 且为完整镜像时，校验应用镜像位置并记录元数据。
     * 自定义地址视为「裸写」，不做镜像校验，也不更新配置区。
     */
    char msg[288];
    if (addr == IAP_FIXED_REGION_END) {
        const uint32_t app_off = IAP_USER_APP_ADDR - IAP_FIXED_REGION_END; /* 0x10000 */

        if (written < app_off + 1) {
            snprintf(msg, sizeof(msg),
                     "已写入 %" PRIu32 " 字节，但文件过短（应 >= %" PRIu32 "）。"
                     "请上传从 0x%06X 开始的完整镜像（含分区表 B）。",
                     written, app_off + 1, (unsigned)IAP_FIXED_REGION_END);
            ESP_LOGW(TAG, "%s", msg);
            return send_text(req, msg, HTTPD_400);
        }

        uint8_t magic = 0;
        esp_flash_read(NULL, &magic, IAP_USER_APP_ADDR, 1);

        if (magic != IAP_ESP_IMAGE_MAGIC) {
            snprintf(msg, sizeof(msg),
                     "已写入 %" PRIu32 " 字节，但 0x%06X 处首字节为 0x%02X，"
                     "不是 ESP 镜像 magic (0xE9)。"
                     "请用 build_user_app.py 打包（会合并分区表 B）。",
                     written, (unsigned)IAP_USER_APP_ADDR, magic);
            ESP_LOGW(TAG, "%s", msg);
            return send_text(req, msg, HTTPD_400);
        }

        /* 记录元数据到配置区 (版本号由用户程序启动后自行上报) */
        iap_cfg_data_t cfg;
        if (iap_config_get(&cfg) == ESP_OK) {
            cfg.user_app_size  = written - app_off;
            cfg.user_app_crc32 = calc_crc;
            iap_config_set(&cfg);
        }

        snprintf(msg, sizeof(msg),
                 "上传成功: %" PRIu32 " 字节 (应用镜像 %" PRIu32 " 字节 @ 0x%06X), "
                 "CRC32 0x%08" PRIx32 "。镜像完整性由 bootloader 启动时校验。",
                 written, written - app_off, (unsigned)IAP_USER_APP_ADDR, calc_crc);
    } else {
        snprintf(msg, sizeof(msg),
                 "上传成功: 已写入 %" PRIu32 " 字节 @0x%08" PRIx32
                 ", CRC32 0x%08" PRIx32 "（自定义地址，未做镜像校验）",
                 written, addr, calc_crc);
    }

    ESP_LOGI(TAG, "%s", msg);
    return send_text(req, msg, NULL);
}

/* -------------------------------------------------------------------------- */
/* 处理函数: 下载                                                              */
/* -------------------------------------------------------------------------- */

/**
 * @brief GET /api/download —— 下载 flash 内容
 *
 * 查询参数 (均可省略):
 *   addr   / offset  起始地址，缺省 0x140000 (可变区起点)
 *   length / size    读取长度，缺省 = flash 末尾 - addr (按 flash 容量)
 *
 * 地址与长度按字节计，支持 0x 前缀。读取范围不得越过 flash 末尾，
 * 也不得进入固定区 (0x0 ~ 0x13FFFF)。
 */
static esp_err_t handler_download(httpd_req_t *req)
{
    /* --- flash 容量 --- */
    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK || flash_size == 0) {
        return send_text(req, "读取 flash 容量失败", HTTPD_500);
    }

    /* --- 解析地址与长度 --- */
    uint32_t offset = 0, length = 0;
    bool has_addr = false, has_len = false;
    parse_addr_len_query(req, &offset, &has_addr, &length, &has_len);

    if (!has_addr) {
        offset = IAP_FIXED_REGION_END;   /* 缺省: 0x140000 */
    }
    if (offset < IAP_FIXED_REGION_END) {
        char m[128];
        snprintf(m, sizeof(m),
                 "读取地址 0x%08" PRIx32 " 非法（须 >= 0x%06X，固定区不可下载）",
                 offset, (unsigned)IAP_FIXED_REGION_END);
        return send_text(req, m, HTTPD_400);
    }
    if (offset >= flash_size) {
        return send_text(req, "读取地址超出 flash 容量", HTTPD_400);
    }

    /* 缺省长度: 从 offset 读到 flash 末尾 */
    if (!has_len || length == 0) {
        length = flash_size - offset;
    }
    if (offset + length > flash_size) {
        length = flash_size - offset;
    }

    /*
     * 未显式指定长度且从默认地址开始时，若配置区记录了有效镜像长度，
     * 只下载有效部分 (避免下载整个区域大部分是 0xFF 的填充)。
     */
    if (!has_len && offset == IAP_FIXED_REGION_END) {
        iap_cfg_data_t cfg;
        if (iap_config_get(&cfg) == ESP_OK &&
            cfg.user_app_size > 0 &&
            cfg.user_app_size + (IAP_USER_APP_ADDR - IAP_FIXED_REGION_END) <= length) {
            length = cfg.user_app_size + (IAP_USER_APP_ADDR - IAP_FIXED_REGION_END);
        }
    }

    ESP_LOGI(TAG, "下载 flash: offset=0x%08" PRIx32 ", length=%" PRIu32,
             offset, length);

    char disp[80];
    snprintf(disp, sizeof(disp),
             "attachment; filename=\"flash_0x%08" PRIx32 ".bin\"", offset);
    httpd_resp_set_type(req, HTTPD_TYPE_OCTET);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    uint8_t *buf = malloc(HTTP_UPLOAD_CHUNK);
    if (buf == NULL) {
        return send_text(req, "内存不足", HTTPD_500);
    }

    esp_err_t result = ESP_OK;

    /* 顺序下载 */
    uint32_t sent = 0;
    while (sent < length) {
        uint32_t remain = length - sent;
        size_t chunk = (remain > HTTP_UPLOAD_CHUNK) ? HTTP_UPLOAD_CHUNK : remain;

        if (esp_flash_read(NULL, buf, offset + sent, chunk) != ESP_OK) {
            ESP_LOGE(TAG, "读取 flash 失败 @%" PRIu32, offset + sent);
            result = ESP_FAIL;
            break;
        }

        if (httpd_resp_send_chunk(req, (const char *)buf, chunk) != ESP_OK) {
            ESP_LOGW(TAG, "发送数据失败");
            result = ESP_FAIL;
            break;
        }

        sent += chunk;
    }

    free(buf);

    /* 结束分块响应 */
    httpd_resp_send_chunk(req, NULL, 0);
    return result;
}

/* -------------------------------------------------------------------------- */
/* 处理函数: 操作                                                              */
/* -------------------------------------------------------------------------- */

static esp_err_t handler_verify(httpd_req_t *req)
{
    iap_image_info_t info;
    iap_image_get_info(&info);

    char msg[192];
    if (!info.present) {
        return send_text(req, "用户程序区无镜像 (首字节非 0xE9)", HTTPD_400);
    }

    snprintf(msg, sizeof(msg),
             "镜像存在。已记录长度 %" PRIu32 ", CRC32 0x%08" PRIx32
             "。完整性由 bootloader 启动时校验。",
             info.image_size, info.image_crc32);
    return send_text(req, msg, NULL);
}

static esp_err_t handler_erase(httpd_req_t *req)
{
    esp_err_t err = iap_image_erase(0);
    if (err != ESP_OK) {
        return send_text(req, "擦除失败", HTTPD_500);
    }
    return send_text(req, "用户程序区已擦除", NULL);
}

static esp_err_t handler_boot(httpd_req_t *req)
{
    if (!iap_image_user_app_present()) {
        return send_text(req, "用户程序区无镜像 (首字节非 0xE9)", HTTPD_400);
    }

    send_text(req, "即将重启进入用户程序...", NULL);
    vTaskDelay(pdMS_TO_TICKS(500));
    iap_image_boot_user_app();
    return ESP_OK;
}

static esp_err_t handler_reboot(httpd_req_t *req)
{
    send_text(req, "即将重启...", NULL);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* 路由注册                                                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief 兜底处理器: 未匹配的路径
 *
 * 浏览器常会请求 /favicon.ico、/index.html 等路径。
 * 对于非 /api/ 开头的请求，统一返回主页面，避免出现 404；
 * 对于 /api/ 开头的请求，返回 404 以便调用方区分。
 *
 * 注意: 该处理器注册在最后，且使用通配符匹配，
 * 因此需要 uri_match_fn = httpd_uri_match_wildcard。
 */
static esp_err_t handler_catch_all(httpd_req_t *req)
{
    /* API 路径返回 404，便于调用方识别 */
    if (strncmp(req->uri, "/api/", 5) == 0) {
        httpd_resp_set_status(req, HTTPD_404);
        return httpd_resp_sendstr(req, "Not Found");
    }

    /* 其它路径返回主页面 (兼容 /index.html、/favicon.ico 等) */
    ESP_LOGI(TAG, "未匹配路径 %s，返回主页面", req->uri);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, iap_index_html_start,
                           iap_index_html_end - iap_index_html_start);
}

static const httpd_uri_t s_uris[] = {
    { .uri = "/",              .method = HTTP_GET,  .handler = handler_index },
    { .uri = "/api/info",      .method = HTTP_GET,  .handler = handler_info },
    { .uri = "/api/cfg",       .method = HTTP_GET,  .handler = handler_cfg_get },
    { .uri = "/api/cfg",       .method = HTTP_POST, .handler = handler_cfg_post },
    { .uri = "/api/cfg/full",  .method = HTTP_GET,  .handler = handler_cfg_full_get },
    { .uri = "/api/cfg/full",  .method = HTTP_POST, .handler = handler_cfg_full_post },
    { .uri = "/api/bootparam", .method = HTTP_GET,  .handler = handler_bootparam_get },
    { .uri = "/api/bootparam", .method = HTTP_POST, .handler = handler_bootparam_set },
    { .uri = "/api/bootparam", .method = HTTP_DELETE, .handler = handler_bootparam_clear },
    { .uri = "/api/app",       .method = HTTP_GET,  .handler = handler_app },
    { .uri = "/api/slots",     .method = HTTP_GET,  .handler = handler_slots },
    { .uri = "/api/slot",      .method = HTTP_POST, .handler = handler_slot_set },
    { .uri = "/api/upload",    .method = HTTP_POST, .handler = handler_upload },
    { .uri = "/api/download",  .method = HTTP_GET,  .handler = handler_download },
    { .uri = "/api/verify",    .method = HTTP_POST, .handler = handler_verify },
    { .uri = "/api/erase",     .method = HTTP_POST, .handler = handler_erase },
    { .uri = "/api/boot",      .method = HTTP_POST, .handler = handler_boot },
    { .uri = "/api/reboot",    .method = HTTP_POST, .handler = handler_reboot },
    /* 兜底路由必须放在最后 */
    { .uri = "/*",             .method = HTTP_GET,  .handler = handler_catch_all },
};

esp_err_t iap_http_start(void)
{
    iap_cfg_data_t cfg;
    esp_err_t err = iap_config_get(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    if (!(cfg.flags & IAP_CFG_FLAG_WIFI_ENABLE)) {
        ESP_LOGI(TAG, "WiFi 未使能，跳过 HTTP 服务");
        return ESP_OK;
    }

    /* 幂等保护: 避免重复启动 (httpd_start 会返回 ESP_ERR_INVALID_STATE) */
    if (s_server != NULL) {
        ESP_LOGI(TAG, "HTTP 服务已启动，复用现有实例");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers   = sizeof(s_uris) / sizeof(s_uris[0]) + 2;
    config.stack_size         = 8192;
    config.lru_purge_enable   = true;
    config.recv_wait_timeout  = 30;   /* 上传大文件需要较长超时 */
    config.send_wait_timeout  = 30;
    config.max_open_sockets   = 4;
    /*
     * 使用通配符匹配以支持兜底路由。
     *
     * 注意: 精确路径 (如 "/" 与 "/api/info") 仍按字面匹配，
     * 通配符匹配器对不含 '*' / '?' 的模板要求长度精确相等，
     * 因此不会误匹配。兜底路由放在数组最后，
     * IDF 按注册顺序查找，先命中的精确路由优先。
     */
    config.uri_match_fn       = httpd_uri_match_wildcard;

    err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "启动 HTTP 服务失败: %s", esp_err_to_name(err));
        return err;
    }

    for (size_t i = 0; i < sizeof(s_uris) / sizeof(s_uris[0]); i++) {
        err = httpd_register_uri_handler(s_server, &s_uris[i]);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "注册路由 %s 失败: %s", s_uris[i].uri, esp_err_to_name(err));
        }
    }

    ESP_LOGI(TAG, "HTTP 服务已启动: http://%s/", iap_wifi_get_ip());
    return ESP_OK;
}

esp_err_t iap_http_stop(void)
{
    if (s_server == NULL) {
        return ESP_OK;
    }
    esp_err_t err = httpd_stop(s_server);
    s_server = NULL;
    return err;
}
