/*
 * ESP IAP 用户程序模板 — 主入口 (v6)
 *
 * 演示用户程序如何与 IAP 程序对接:
 *   1. 读取 IAP 通过 RTC RAM 传来的启动参数
 *   2. 正常业务逻辑
 *   3. 需要升级时请求重启进入 IAP 下载模式
 *
 * ⚠️ v5: 用户程序**不允许访问配置区** (0x8000)。
 *     需要配置信息请通过启动参数 (RTC RAM) 获取。
 *
 * 编译 + 打包: python build_user_app.py --target esp32c6
 * 烧录: user_app_template_flash.bin (从 0x140000 开始的完整镜像)
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "iap_user_api.h"

static const char *TAG = "user_app";

/* ============================================================================
 * 应用配置（从启动参数或配置区读取）
 * ========================================================================== */

typedef struct {
    char     mode[16];          /*!< 运行模式: normal / debug / factory */
    char     reason[24];        /*!< 进入 IAP 的原因 */
    uint32_t boot_count;        /*!< 启动计数 */
    uint32_t baudrate;          /*!< 建议通信波特率 */
} app_config_t;

static app_config_t s_app_cfg = {
    .mode       = "normal",
    .reason     = "unknown",
    .boot_count = 0,
    .baudrate   = 115200,
};

/* 升级请求队列（示例：任何任务都可投递） */
static QueueHandle_t s_upgrade_req = NULL;

/* ============================================================================
 * 启动参数解析
 * ========================================================================== */

/**
 * @brief 从 IAP 启动参数中加载应用配置
 *
 * 启动参数格式: "mode=normal;reason=wait_timeout;boot=3;baud=921600"
 */
static void load_boot_params(void)
{
    /*
     * 用 static 避免占用栈空间。
     * 默认 main 任务栈仅 3584 字节，而本缓冲区 256 字节 +
     * iap_user_get_boot_param() 内部开销，叠加后极易溢出。
     * 本函数仅在启动时调用一次，无重入风险。
     */
    static char param[IAP_USER_BOOT_PARAM_SIZE];

    esp_err_t err = iap_user_get_boot_param(param, sizeof(param));
    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "无启动参数，使用默认配置");
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "读取启动参数失败: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "启动参数: %s", param);

    char val[32];

    if (iap_user_boot_param_get_value(param, "mode", val, sizeof(val)) == ESP_OK) {
        strlcpy(s_app_cfg.mode, val, sizeof(s_app_cfg.mode));
    }
    if (iap_user_boot_param_get_value(param, "reason", val, sizeof(val)) == ESP_OK) {
        strlcpy(s_app_cfg.reason, val, sizeof(s_app_cfg.reason));
    }
    if (iap_user_boot_param_get_value(param, "boot", val, sizeof(val)) == ESP_OK) {
        s_app_cfg.boot_count = (uint32_t)strtoul(val, NULL, 10);
    }
    if (iap_user_boot_param_get_value(param, "baud", val, sizeof(val)) == ESP_OK) {
        uint32_t b = (uint32_t)strtoul(val, NULL, 10);
        if (b >= 9600) {
            s_app_cfg.baudrate = b;
        }
    }

    ESP_LOGI(TAG, "模式=%s 原因=%s 启动计数=%" PRIu32 " 波特率=%" PRIu32,
             s_app_cfg.mode, s_app_cfg.reason,
             s_app_cfg.boot_count, s_app_cfg.baudrate);
}

/**
 * @brief 按运行模式初始化
 *
 * 这是"启动参数"最典型的用途：IAP 下载完成后可以指定一次性的
 * 运行模式（如 factory 自检、debug 详细日志），用户程序据此分支。
 */
static void init_by_mode(void)
{
    if (strcmp(s_app_cfg.mode, "debug") == 0) {
        esp_log_level_set("*", ESP_LOG_DEBUG);
        ESP_LOGI(TAG, "进入调试模式（详细日志）");
    } else if (strcmp(s_app_cfg.mode, "factory") == 0) {
        ESP_LOGI(TAG, "进入出厂自检模式");
        /* 执行自检流程 ... */
    } else {
        ESP_LOGI(TAG, "进入正常工作模式");
    }
}

/* ============================================================================
 * 升级请求
 * ========================================================================== */

/**
 * @brief 请求升级（线程安全，可从任意任务/中断投递）
 *
 * 实际重启在 upgrade_task 中执行，避免在中断上下文调用。
 */
void app_request_upgrade(void)
{
    if (s_upgrade_req != NULL) {
        uint8_t dummy = 1;
        xQueueSend(s_upgrade_req, &dummy, pdMS_TO_TICKS(10));
    }
}

/**
 * @brief 升级请求处理任务
 *
 * 收到请求后置位下载标志并重启，IAP 启动时会进入下载模式。
 */
static void upgrade_task(void *arg)
{
    (void)arg;
    uint8_t dummy;

    while (1) {
        if (xQueueReceive(s_upgrade_req, &dummy, portMAX_DELAY) == pdTRUE) {
            ESP_LOGW(TAG, "收到升级请求，%d 秒后重启进入 IAP 下载模式...", 2);
            vTaskDelay(pdMS_TO_TICKS(2000));

            /* 置位下载标志并重启 — 正常情况不会返回 */
            esp_err_t err = iap_user_request_download();
            ESP_LOGE(TAG, "请求下载失败: %s", esp_err_to_name(err));
        }
    }
}

/* ============================================================================
 * 业务逻辑（示例）
 * ========================================================================== */

/**
 * @brief 示例: 请求修改配置并持久化 (v6)
 *
 * 用户程序不能直接写配置区。把要修改的字段写入 RTC RAM update 区，
 * 重启进 IAP 后由 IAP 写入配置区，再重启启动用户程序。
 *
 * 只允许修改 IAP 暴露的白名单字段:
 *   启动参数字符串 / OTA 槽序号 / 加载地址 / 启动目标
 */
void app_request_config_update(void)
{
    /* 1. 填写要修改的字段 (可只填其中一部分) */
    iap_user_request_boot_param("mode=debug;server=192.168.1.10", 0);
    iap_user_request_active_slot(1);

    /* 2. 提交 + 重启 (不会返回) */
    iap_user_request_apply();
}

/**
 * @brief 模拟业务任务
 *
 * 真实项目中替换为你的应用逻辑。
 * 这里演示：运行 30 秒后自动请求升级（便于测试闭环）。
 */
static void business_task(void *arg)
{
    (void)arg;
    int64_t start = esp_timer_get_time() / 1000000;
    int tick = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        tick++;

        int64_t uptime = esp_timer_get_time() / 1000000 - start;
        ESP_LOGI(TAG, "[%d] 运行中... 已运行 %lld 秒, 空闲堆 %" PRIu32,
                 tick, uptime, (uint32_t)esp_get_free_heap_size());

        /* 示例：运行 30 秒后自动请求升级
         * 实际项目中应改为由外部指令（串口命令/网络请求/按键）触发。
         * 测试时取消下面注释即可验证"用户程序 → IAP"的完整闭环。 */
        // if (uptime > 30) {
        //     ESP_LOGW(TAG, "测试：自动请求升级");
        //     app_request_upgrade();
        // }
    }
}

/* ============================================================================
 * 入口
 * ========================================================================== */

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  ESP IAP 用户程序模板 v1.0.0 (v5)");
    ESP_LOGI(TAG, "========================================");

    /* --- 1. 读取并应用启动参数 (来自 RTC RAM) ---------------------------- */
    load_boot_params();
    init_by_mode();

    /* --- 2. 初始化 NVS（若业务需要）-------------------------------------- */
    /*
     * v5: 用户程序使用自己的 NVS 分区 "nvs" (见 partitions_user_app.csv)，
     *     与 IAP 的 "nvs" 位于不同地址、互不影响。
     *
     * ⚠️ 名字必须是 "nvs"：WiFi 协议栈内部调用 nvs_flash_init() 查找它，
     *    改成其它名字（如 "nvs_app"）会导致 WiFi 无法启动。
     *
     * 注意: 用户程序**不访问配置区** (0x8000)，需要配置请用启动参数。
     */
    esp_err_t nvs_err = nvs_flash_init_partition("nvs");
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase_partition("nvs");
        nvs_err = nvs_flash_init_partition("nvs");
    }
    if (nvs_err != ESP_OK) {
        ESP_LOGW(TAG, "NVS (nvs) 初始化失败: %s", esp_err_to_name(nvs_err));
    }

    /* --- 3. 创建升级请求队列与任务 --------------------------------------- */
    s_upgrade_req = xQueueCreate(4, sizeof(uint8_t));
    if (s_upgrade_req == NULL) {
        ESP_LOGE(TAG, "创建升级队列失败");
    } else {
        xTaskCreate(upgrade_task, "upgrade", 3072, NULL, 5, NULL);
    }

    /* --- 7. 启动业务任务 ------------------------------------------------- */
    xTaskCreate(business_task, "business", 4096, NULL, 4, NULL);

    ESP_LOGI(TAG, "初始化完成，进入主循环");
}
