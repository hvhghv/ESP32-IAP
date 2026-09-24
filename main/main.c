/*
 * ESP IAP - 主程序
 *
 * 启动流程:
 *
 *   1. 初始化 NVS (分区 "nvs")
 *   2. 初始化配置区模块，读取 IAP 配置
 *   3. 初始化镜像模块 (扫描 OTA 槽)
 *   4. 判断进入 IAP 下载模式还是用户程序:
 *        a. 配置区错误 / 镜像模块初始化失败 -> 进入 IAP 下载模式
 *        b. 配置区下载位被置位              -> 进入 IAP 下载模式
 *        c. 配置的活动槽无效                -> 进入 IAP 下载模式
 *        d. 配置区等待值为 n 秒             -> 等待 n 秒，期间有触发则进入 IAP，
 *                                              无触发则进入用户程序
 *        e. 其它情况                        -> 检查用户程序区后进入用户程序
 *   5. 用户程序区无镜像 (首字节非 0xE9) 则进入 IAP 下载模式
 *      (镜像合法性由 bootloader 启动时自校验，失败自动回落 IAP)
 *   6. IAP 下载模式下启动各下载通道 (UART / I2C / WiFi / USB) 与终端
 *   7. 跳转用户程序前释放 WiFi/HTTP 资源 (避免 WIFI_AP_DEF 网卡冲突)
 *
 * 强制进入 IAP 的方式 (优先级从高到低):
 *   1. 配置区错误 / 镜像模块不可用
 *   2. 配置区下载位    —— HTML 工具 / 用户程序可置位
 *   3. 配置的活动槽无效
 *   4. 等待窗口触发源  —— GPIO0 (BOOT 键) / UART / I2C / WiFi / USB
 *   5. 用户程序区无有效镜像
 *   6. 冷启动 (RTC RAM 无效)
 *
 * ⚠️ v5: 原 flash 魔术标记 (iap_mark) 已取消，
 *         启动决策改放 RTC RAM (见 main/iap_boot_param.h)。
 */

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"

#include "iap_common.h"
#include "iap_config.h"
#include "iap_image.h"
#include "iap_boot_param.h"     /* RTC RAM 启动参数 + 用户程序更新请求 (v6) */
#include "iap_uart.h"
#include "iap_i2c.h"
#include "iap_wifi.h"
#include "iap_http.h"
#include "iap_wait_trigger.h"

static const char *TAG = "iap_main";

/** 等待任务使用的 UART 端口 */
static uart_port_t s_wait_uart = UART_NUM_0;

/** 等待阶段的 UART 驱动是否已就绪 */
static bool s_wait_uart_ready = false;

/* ============================================================================
 * 等待检测
 * ========================================================================== */

/**
 * @brief 为等待阶段准备 UART
 *
 * 等待阶段早于 iap_uart_start()，此时 UART 驱动可能尚未安装。
 * 这里先安装驱动，使 uart_read_bytes() 可用；
 * 后续 iap_uart_start() 会复用同一驱动并配置参数。
 *
 * @return ESP_OK 驱动可用
 */
static esp_err_t wait_uart_prepare(void)
{
    /*
     * 安装驱动。
     *
     * 注意: tx_buffer_size 必须非 0。若传 0，驱动不会创建 TX 互斥量，
     * 但 uart_write_bytes() 仍会去获取该互斥量，导致断言失败。
     * 因此这里分配 TX 缓冲 (同时供后续终端复用)。
     */
    esp_err_t err = uart_driver_install(s_wait_uart, 1024, 1024, 0, NULL, 0);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "等待阶段已安装 UART 驱动");
        s_wait_uart_ready = true;
        return ESP_OK;
    }

    /* 检查驱动是否已可用 (已安装的情况) */
    size_t buffered = 0;
    if (uart_get_buffered_data_len(s_wait_uart, &buffered) == ESP_OK) {
        s_wait_uart_ready = true;
        return ESP_OK;
    }

    ESP_LOGW(TAG, "等待阶段 UART 不可用 (%s)", esp_err_to_name(err));
    s_wait_uart_ready = false;
    return err;
}

/* ============================================================================
 * 等待触发回调
 * ========================================================================== */

/**
 * @brief 等待阶段启动 I2C (供触发检测调用)
 *
 * I2C 启动较快，直接调用即可。
 */
static esp_err_t wait_start_i2c(void)
{
    return iap_i2c_start();
}

/**
 * @brief 等待阶段启动 WiFi (供触发检测调用)
 *
 * 使用非阻塞模式: 不等待 AP 就绪，避免占用等待窗口。
 * 客户端接入事件仍会正常上报，不影响触发检测。
 */
static esp_err_t wait_start_wifi(void)
{
    return iap_wifi_start_ex(true);
}

/* ============================================================================
 * 用户程序配置更新请求 (v6)
 * ========================================================================== */

/**
 * @brief 处理用户程序通过 RTC RAM 提交的配置更新请求
 *
 * 用户程序不能直接写配置区 (0x8000 在 IDF 写保护区内)。需要持久化修改时:
 *   1. 用户程序调 iap_param_request_*() 填写 RTC RAM update 区
 *   2. 用户程序 esp_restart()
 *   3. bootloader 读 RTC RAM → 启动 IAP
 *   4. **本函数** 检测 update_flags，把白名单内的字段写入配置区
 *   5. 清除请求，重启启动用户程序
 *
 * 只允许修改白名单字段 (IAP_PARAM_UPD_ALLOWED_MASK):
 *   启动参数字符串 / OTA 槽序号 / 加载地址 / 启动目标
 *
 * @return true 表示处理了更新请求 (调用方应重启)
 */
static bool iap_main_apply_update_request(void)
{
    if (!iap_param_has_update_request()) {
        return false;
    }

    iap_boot_param_t *p = iap_param_get();
    uint8_t flags = p->update_flags & IAP_PARAM_UPD_ALLOWED_MASK;

    ESP_LOGW(TAG, "========================================");
    ESP_LOGW(TAG, " 检测到用户程序配置更新请求 (flags=0x%02X)", (unsigned)flags);
    ESP_LOGW(TAG, "========================================");

    iap_cfg_data_t cfg;
    if (iap_config_get(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "读取配置区失败，放弃更新请求");
        iap_param_clear_update_request();
        return false;
    }

    bool changed = false;

    /* --- 1. 启动参数字符串 --- */
    if (flags & IAP_PARAM_UPD_BOOT_PARAM) {
        if (p->upd_param_len == 0) {
            if (iap_config_clear_boot_param() == ESP_OK) {
                ESP_LOGI(TAG, "  启动参数: 已清空");
                changed = true;
            }
        } else {
            if (iap_config_set_boot_param((const char *)p->upd_param) == ESP_OK) {
                ESP_LOGI(TAG, "  启动参数: %s", (const char *)p->upd_param);
                changed = true;
            } else {
                ESP_LOGW(TAG, "  启动参数写入失败 (可能超长)");
            }
        }
    }

    /* --- 2. OTA 槽序号 --- */
    if (flags & IAP_PARAM_UPD_ACTIVE_SLOT) {
        uint8_t slot_count = iap_image_get_slot_count();
        if (p->upd_active_slot < slot_count) {
            if (iap_config_set_active_slot(p->upd_active_slot) == ESP_OK) {
                ESP_LOGI(TAG, "  OTA 槽序号: %u (共 %u 槽)",
                         (unsigned)p->upd_active_slot, (unsigned)slot_count);
                changed = true;
            }
        } else {
            ESP_LOGW(TAG, "  OTA 槽序号 %u 越界 (有效 0~%u)，已忽略",
                     (unsigned)p->upd_active_slot,
                     slot_count ? (unsigned)(slot_count - 1) : 0);
        }
    }

    /* --- 3. 加载地址 --- */
    if (flags & IAP_PARAM_UPD_USER_ADDR) {
        /*
         * 加载地址必须落在可变区 (>= IAP_FIXED_REGION_END)，
         * 否则会覆盖固定区 (bootloader / 配置区 / 分区表 / IAP)。
         *
         * v7: 写入 cfg.slot_addr[active_slot] —— 用户程序请求"下次从
         *     该地址加载"。0 表示清除该槽地址 (槽失效)。
         */
        uint8_t slot = cfg.active_slot;
        if (slot >= IAP_OTA_SLOT_MAX) slot = 0;

        if (p->upd_user_addr == 0 || p->upd_user_addr >= IAP_FIXED_REGION_END) {
            cfg.slot_addr[slot] = p->upd_user_addr;
            if (p->upd_user_addr != 0 && cfg.slot_size[slot] == 0) {
                /* 未配置大小时，用"到固定区末尾"兜底 */
                cfg.slot_size[slot] = IAP_FIXED_REGION_END - p->upd_user_addr;
            }
            if (iap_config_set(&cfg) == ESP_OK) {
                ESP_LOGI(TAG, "  槽 %u 加载地址: 0x%08" PRIx32 " (%s)",
                         (unsigned)slot, p->upd_user_addr,
                         p->upd_user_addr ? "设置" : "清除");
                changed = true;
            }
        } else {
            ESP_LOGW(TAG, "  加载地址 0x%08" PRIx32 " 非法 (须为 0 或 >= 0x%06X)，已忽略",
                     p->upd_user_addr, (unsigned)IAP_FIXED_REGION_END);
        }
    }

    /* --- 4. 启动目标 --- */
    if (flags & IAP_PARAM_UPD_BOOT_TARGET) {
        if (p->upd_boot_target == IAP_BOOT_TARGET_IAP) {
            iap_config_set_download_mode(true);
            ESP_LOGI(TAG, "  启动目标: IAP (置位下载模式)");
            changed = true;
        } else if (p->upd_boot_target == IAP_BOOT_TARGET_APP) {
            iap_config_set_download_mode(false);
            ESP_LOGI(TAG, "  启动目标: APP (清除下载模式)");
            changed = true;
        } else {
            ESP_LOGW(TAG, "  启动目标 %u 非法，已忽略", (unsigned)p->upd_boot_target);
        }
    }

    /* 清除请求 (无论是否全部成功，避免死循环) */
    iap_param_clear_update_request();

    if (changed) {
        ESP_LOGI(TAG, "配置更新完成，重启以应用新配置...");
    } else {
        ESP_LOGW(TAG, "配置更新无有效字段，继续正常启动");
    }

    return changed;
}

/* ============================================================================
 * 启动参数
 * ========================================================================== */

/**
 * @brief 组装并写入启动参数字符串
 *
 * 在启动用户程序前调用，将 IAP 侧的信息以 "key=value;..." 形式写入配置区，
 * 用户程序启动后可通过 iap_config_get_boot_param() 读取。
 *
 * 当前写入的键:
 *   reason  进入用户程序的原因 (对应 iap_boot_reason_t 数值)
 *   boot    启动计数
 *   mode    运行模式 (normal / factory)
 *
 * @param reason 进入用户程序的原因
 * @return ESP_OK 成功
 */
static esp_err_t iap_main_write_boot_param(iap_boot_reason_t reason)
{
    iap_cfg_data_t cfg;
    if (iap_config_get(&cfg) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }

    char param[IAP_CFG_BOOT_PARAM_SIZE];
    snprintf(param, sizeof(param),
             "reason=%u;boot=%" PRIu32 ";mode=normal",
             (unsigned)reason, cfg.boot_count);

    esp_err_t err = iap_config_set_boot_param(param);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "写入启动参数失败: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "启动参数: %s", param);
    }
    return err;
}

/* ============================================================================
 * 主程序
 * ========================================================================== */

void app_main(void)
{
    /* ---- 1. 初始化 NVS ---- */
    /*
     * ⚠️ 分区表 A 中 NVS 分区的标签**必须**是 "nvs"。
     *    WiFi 协议栈内部会调用 nvs_flash_init() 查找名为 "nvs" 的分区，
     *    若改名（如 "nvs_iap"）会导致 WiFi 初始化失败。
     *    因此这里直接用标准的 nvs_flash_init()。
     */
    esp_err_t err = nvs_flash_init_partition(IAP_PARTITION_LABEL_NVS_IAP);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要重新初始化");
        nvs_flash_erase_partition(IAP_PARTITION_LABEL_NVS_IAP);
        err = nvs_flash_init_partition(IAP_PARTITION_LABEL_NVS_IAP);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败 (%s): %s",
                 IAP_PARTITION_LABEL_NVS_IAP, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "NVS 已初始化: %s", IAP_PARTITION_LABEL_NVS_IAP);
    }

    /* ---- 2. 打印启动横幅 ---- */
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

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " ESP IAP v%s", IAP_VERSION_STRING);
    ESP_LOGI(TAG, " 芯片: %s rev%d, %d 核", model, chip.revision, chip.cores);
    ESP_LOGI(TAG, " Flash: %" PRIu32 " MB, IDF: %s",
             flash_size / (1024 * 1024), esp_get_idf_version());
    ESP_LOGI(TAG, "========================================");

    /* ---- 3. 初始化配置区 ---- */
    bool cfg_error = false;
    err = iap_config_init();
    if (err != ESP_OK) {
        /*
         * 配置区初始化失败 (分区缺失 / 读写错误 / CRC 无法修复)。
         *
         * 不直接 return —— 那会让设备卡死无输出。
         * 改为: 用默认配置继续启动，并强制进入 IAP 下载模式，
         *       让用户能通过终端发现并修复问题。
         */
        ESP_LOGE(TAG, "========================================");
        ESP_LOGE(TAG, " 配置区初始化失败: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, " 将使用默认配置进入 IAP 下载模式");
        ESP_LOGE(TAG, "========================================");
        cfg_error = true;
    }

    /* ---- 4. 初始化镜像模块 ---- */
    err = iap_image_init();
    bool image_error = false;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "镜像模块初始化失败: %s", esp_err_to_name(err));
        /*
         * 用户程序区不可用 (分区表 B 缺失 / 无 user_app 分区)。
         *
         * 不直接 return —— 那会让设备卡死无输出。
         * 改为: 标记 image_error，强制进入 IAP 下载模式，
         *       让用户能通过终端发现并修复问题。
         *
         * ⚠️ 关键: 此时 iap_image_* 系列函数都不可用
         *    (s_slot_count == 0)，后续必须跳过所有镜像操作，
         *    否则会触发 assert / 越界。
         */
        image_error = true;
    }

    iap_cfg_data_t cfg;
    iap_config_get(&cfg);

    ESP_LOGI(TAG, "配置: flags=0x%08" PRIx32 ", 等待=%u 秒, 下载模式=%s",
             cfg.flags, cfg.wait_seconds,
             (cfg.flags & IAP_CFG_FLAG_DOWNLOAD_MODE) ? "是" : "否");

    /* ---- 4.5 处理用户程序提交的配置更新请求 (v6) ---- */
    /*
     * 用户程序不能直接写配置区，需要持久化修改时把目标值写入 RTC RAM
     * 的 update 区并重启。本函数检测到请求后写入配置区，然后重启应用。
     *
     * 必须在配置区初始化成功 (无 cfg_error) 且镜像模块可用后调用。
     */
    if (!cfg_error && !image_error) {
        if (iap_main_apply_update_request()) {
            ESP_LOGW(TAG, "配置已更新，重启以应用...");
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
            /* 不会执行到这里 */
        }
    }

    /* ---- 5. 决定进入 IAP 还是用户程序 ---- */
    bool enter_download = false;
    iap_boot_reason_t reason = IAP_BOOT_REASON_NONE;

    /*
     * 5.-2 最高优先级: RTC RAM 明确要求启动 IAP
     *
     * 场景: 用户在终端执行 `iap boot` (或用户程序请求进入 IAP)，
     *       写 RTC RAM boot_target=IAP 后重启。bootloader 按此启动 IAP，
     *       该标志**保留**到 IAP 运行期。
     *
     * 此时必须**直接停在 IAP 终端**，不再等 3 秒、不再启动用户程序 ——
     * 否则 `iap boot` 命令形同虚设 (重启后又被跳回 APP)。
     */
    {
        iap_boot_param_t *bp = iap_param_get();
        if (iap_param_valid() && bp->boot_target == IAP_BOOT_TARGET_IAP) {
            ESP_LOGW(TAG, "RTC RAM 请求启动 IAP，直接进入终端 (跳过等待)");
            enter_download = true;
            reason = IAP_BOOT_REASON_USER_REQUEST;
        }
    }

    /*
     * 5.-1 配置区错误
     *
     * 配置区无法读取或修复时，强制进入 IAP 下载模式，
     * 避免用错误的配置启动用户程序。
     */
    if (!enter_download) {
    if (cfg_error) {
        ESP_LOGW(TAG, "配置区错误，强制进入 IAP 下载模式");
        enter_download = true;
        reason = IAP_BOOT_REASON_CFG_ERROR;
    } else
    /*
     * 5.-0.5 镜像模块初始化失败 (分区表 B 缺失 / 无 user_app 分区)
     *
     * 此时没有任何可用 OTA 槽，必须直接进入 IAP 下载模式，
     * 且**不得**调用任何 iap_image_* 函数 (会越界/assert)。
     */
    if (image_error) {
        ESP_LOGW(TAG, "用户程序区不可用，强制进入 IAP 下载模式");
        enter_download = true;
        reason = IAP_BOOT_REASON_NO_VALID_APP;
    } else
    /*
     * 5.0 次高优先级: 配置的活动槽无效 (越界或分区不存在)
     *
     * v5: flash 魔术标记检查已删除 —— 「强制进入 IAP」改由
     *     GPIO0 电平检测 (iap_wait_trigger.c) 提供。
     *     详见 docs/FINAL-REPORT.md §3.3
     */
    if (iap_image_slot_invalid()) {
        /*
         * 5a'. 配置区的活动槽无效 (越界或分区不存在)
         *
         * 场景 C: 用户设置了 active_slot = 5，但设备只有 2 个槽。
         * 不静默回退，直接进入 IAP 下载模式，让用户修正配置。
         */
        ESP_LOGW(TAG, "配置的活动槽无效，进入 IAP 下载模式");
        enter_download = true;
        reason = IAP_BOOT_REASON_NO_VALID_APP;
    } else if (cfg.flags & IAP_CFG_FLAG_DOWNLOAD_MODE) {
        /* 5b. 下载位被置位: 强制进入 IAP 下载 */
        ESP_LOGW(TAG, "配置区下载位已置位，进入 IAP 下载模式");
        enter_download = true;
        reason = IAP_BOOT_REASON_DOWNLOAD_FLAG;
    } else if (cfg.wait_seconds > 0) {
        /* 5c. 等待 n 秒，期间任一使能的触发源命中则进入 IAP */
        ESP_LOGI(TAG, "等待 %u 秒以检测触发源...", cfg.wait_seconds);

        uint32_t wait_ms = (uint32_t)cfg.wait_seconds * 1000;

        /* 准备等待阶段的 UART (驱动可能尚未安装) */
        wait_uart_prepare();

        /*
         * 准备等待阶段的 USB (驱动可能尚未安装)。
         *
         * 与 UART 同理: USB 终端在 step 7 才启动，但等待窗口需要读 USB
         * 输入来判断是否收到 "iap" 进入命令。这里提前安装驱动
         * (仅当 USB 使能或 USB 触发使能时)。
         */
        iap_usb_prepare();

        /* 清空可能残留的通知 */
        ulTaskNotifyTake(pdTRUE, 0);

        /*
         * 启动触发检测。
         *
         * I2C 与 WiFi 通过回调在等待阶段按需启动，
         * 未使能对应触发源时不会启动，不占用资源。
         */
        err = iap_wait_trigger_start(&cfg, wait_ms,
                                     xTaskGetCurrentTaskHandle(),
                                     wait_start_i2c, wait_start_wifi);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "启动触发检测失败: %s，退化为纯延时", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(wait_ms));
        } else {
            /* 阻塞等待检测任务通知 (最多等待 wait_ms + 1 秒) */
            uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms + 1000));
            ESP_LOGD(TAG, "等待结束 (通知计数=%" PRIu32 ")", notified);
            iap_wait_trigger_stop();
            /* 留出时间让检测任务清理 */
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        if (iap_wait_trigger_fired()) {
            iap_boot_reason_t trig_reason = iap_wait_trigger_reason();
            ESP_LOGW(TAG, "等待期间触发进入 IAP (原因: %s)",
                     iap_boot_reason_str(trig_reason));
            enter_download = true;
            reason = trig_reason;
        } else {
            ESP_LOGI(TAG, "等待窗口结束，无触发，尝试启动用户程序");
        }
    } else {
        ESP_LOGI(TAG, "等待时间为 0，直接尝试启动用户程序");
    }
    }   /* end: if (!enter_download) —— RTC RAM 要求进 IAP 时跳过全部判定 */

    /* ---- 6. 非下载模式: 检查用户程序是否存在 ---- */
    if (!enter_download && !image_error) {
        /*
         * 6a. 按 GPIO 电平选择 OTA 槽
         *
         * 若配置了 ota_gpio，读取引脚电平并映射为槽序号，
         * 覆盖配置区的 active_slot。用于产线/现场快速切换。
         */
        uint8_t slot_count = iap_image_get_slot_count();
        uint8_t slot = iap_image_get_active_slot();

        if (slot_count > 1) {
            uint8_t gpio_slot = iap_image_slot_from_gpio();
            if (gpio_slot != slot) {
                ESP_LOGW(TAG, "GPIO 选择 OTA 槽 %u (原活动槽 %u)",
                         (unsigned)gpio_slot, (unsigned)slot);
                if (iap_image_set_active_slot(gpio_slot) == ESP_OK) {
                    slot = gpio_slot;
                }
            }
        }

        ESP_LOGI(TAG, "准备启动 OTA 槽 %u / 共 %u 槽",
                 (unsigned)slot, (unsigned)slot_count);

        /*
         * 6b. 活动槽不存在时回落到槽 0
         *
         * 场景: 分区表被替换（槽数变少），但配置区仍记录着旧的
         *       active_slot。此时不直接进 IAP，而是尝试槽 0 ——
         *       因为槽 0 通常有可用的用户程序。
         */
        if (iap_image_get_slot(slot) == NULL) {
            ESP_LOGW(TAG, "活动槽 %u 不存在，回落到槽 0", (unsigned)slot);
            slot = 0;
            iap_image_set_active_slot(0);
        }

        bool app_ok = false;

        if (iap_image_get_slot(slot) == NULL) {
            ESP_LOGW(TAG, "OTA 槽 %u 分区不可用", (unsigned)slot);
        } else if (!iap_image_slot_present(slot)) {
            ESP_LOGW(TAG, "OTA 槽 %u 无镜像 (首字节非 0xE9)", (unsigned)slot);
        } else {
            /*
             * 镜像完整性由 bootloader 在启动时校验
             * (magic / 段表 / SHA256 / chip_id)，失败会自动回落到
             * factory (IAP)，不会变砖。此处只做存在性检查。
             */
            ESP_LOGI(TAG, "OTA 槽 %u 存在镜像，交由 bootloader 校验",
                     (unsigned)slot);
            app_ok = true;
        }

        if (app_ok) {
            /* 更新配置区并启动用户程序 */
            iap_config_inc_boot_count();
            iap_config_set_boot_reason(IAP_BOOT_REASON_NONE);

            /* 写入启动参数字符串，供用户程序初始化使用 */
            iap_main_write_boot_param(IAP_BOOT_REASON_NONE);

            ESP_LOGI(TAG, "启动 OTA 槽 %u 的用户程序...", (unsigned)slot);
            err = iap_image_boot_slot(slot);
            /* 正常情况下不会返回 */
            ESP_LOGE(TAG, "启动用户程序失败: %s", esp_err_to_name(err));
            enter_download = true;
            reason = IAP_BOOT_REASON_CRC_FAILED;
        } else {
            /* 校验失败 -> 进入 IAP 下载模式 */
            enter_download = true;
            reason = iap_image_slot_present(slot)
                   ? IAP_BOOT_REASON_CRC_FAILED
                   : IAP_BOOT_REASON_NO_VALID_APP;
        }
    }

    /* ---- 7. 进入 IAP 下载模式 ---- */
    ESP_LOGW(TAG, "========================================");
    ESP_LOGW(TAG, " 进入 IAP 下载模式");
    ESP_LOGW(TAG, " 原因: %s", iap_boot_reason_str(reason));
    ESP_LOGW(TAG, "========================================");

    iap_config_set_boot_reason(reason);

    /* 7a. UART 终端 (最先启动，便于用户交互) */
    err = iap_uart_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART 终端启动失败: %s", esp_err_to_name(err));
    }

    /* 7a2. USB 串口终端 (仅支持 USB 的芯片，与 UART 共享命令表) */
    err = iap_usb_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB 终端启动失败: %s", esp_err_to_name(err));
    }

    /* 给终端一点时间输出提示 */
    vTaskDelay(pdMS_TO_TICKS(200));

    /* 7b. I2C 从机 */
    err = iap_i2c_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C 从机启动失败: %s", esp_err_to_name(err));
    }

    /* 7c. WiFi AP */
    err = iap_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi AP 启动失败: %s", esp_err_to_name(err));
    }

    /* 7d. HTTP 服务 */
    err = iap_http_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 服务启动失败: %s", esp_err_to_name(err));
    }

    /* ---- 8. 进入主循环 ---- */
    ESP_LOGI(TAG, "IAP 已就绪");
    ESP_LOGI(TAG, "  UART 终端: 输入 help 查看命令");
    ESP_LOGI(TAG, "  I2C 从机  : 地址 0x%02X", cfg.i2c_addr);
    if (cfg.flags & IAP_CFG_FLAG_WIFI_ENABLE) {
        ESP_LOGI(TAG, "  WiFi AP   : SSID '%s'", cfg.wifi_ssid);
        ESP_LOGI(TAG, "  HTTP      : http://%s/", iap_wifi_get_ip());
    }

    while (1) {
        /* 主循环仅做心跳与内存监控 */
        vTaskDelay(pdMS_TO_TICKS(10000));

        ESP_LOGD(TAG, "空闲堆: %" PRIu32 " 字节",
                 (uint32_t)esp_get_free_heap_size());
    }
}
