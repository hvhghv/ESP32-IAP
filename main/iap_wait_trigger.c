/*
 * ESP IAP - 等待期间进入 IAP 下载模式的触发检测
 *
 * 在启动等待窗口内，同时监听多个触发源。任一使能的触发源命中，
 * 即中止等待并进入 IAP 下载模式。
 *
 * 支持的触发源 (各自独立使能):
 *
 *   1. GPIO 引脚电平
 *      进入等待前将配置的引脚配置为输入，并按有效电平启用内部上拉/下拉。
 *      等待期间轮询引脚，检测到有效电平时触发。
 *      有效电平可配置: 低电平有效 (启用上拉) 或高电平有效 (启用下拉)。
 *
 *   2. I2C 进入命令
 *      等待期间启动 I2C 从机，监听 I2C_TYPE_BOOT_IAP 命令。
 *
 *   3. UART 进入命令
 *      等待期间监听串口。收到 "iap" 关键字加回车，或任意单字节
 *      (兼容旧行为) 即触发。
 *
 *   4. WiFi 客户端接入
 *      等待期间启动 WiFi AP，监听 WIFI_EVENT_AP_STACONNECTED 事件。
 *
 * 设计说明:
 *   - 检测在独立任务中轮询/事件驱动，主任务阻塞等待通知
 *   - 触发源按配置按需启动，未使能的不占用资源
 *   - 所有触发源共享一个"已触发"标志与原因记录
 */

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"

#include "iap_common.h"
#include "iap_config.h"
#include "iap_uart.h"        /* iap_usb_prepare / iap_usb_ready / iap_usb_read */
#include "iap_wait_trigger.h"

static const char *TAG = "iap_wait";

/** 轮询周期 (毫秒) */
#define WAIT_POLL_MS        50

/** 触发源位掩码 */
#define TRIG_SRC_GPIO       (1U << 0)
#define TRIG_SRC_I2C        (1U << 1)
#define TRIG_SRC_UART       (1U << 2)
#define TRIG_SRC_WIFI       (1U << 3)
#define TRIG_SRC_USB        (1U << 4)

/** 触发状态 */
static struct {
    volatile uint32_t fired_mask;       /*!< 已触发的源位掩码 */
    volatile iap_boot_reason_t reason;  /*!< 触发原因 (首个命中的源) */
    volatile bool     stop;             /*!< 请求停止所有检测 */
    uint32_t          enabled;          /*!< 本次使能的源位掩码 */
    uint8_t           gpio_num;         /*!< 触发引脚 */
    uint8_t           gpio_level;       /*!< 有效电平 */
    uart_port_t       uart_num;         /*!< 等待阶段监听的 UART */
    bool              uart_ready;       /*!< UART 驱动是否可用 */
    bool              usb_ready;        /*!< USB 驱动是否可用 */
    TaskHandle_t      main_task;        /*!< 主任务句柄 */
    iap_wait_start_fn_t start_i2c;      /*!< I2C 启动回调 (由检测任务调用) */
    iap_wait_start_fn_t start_wifi;     /*!< WiFi 启动回调 (由检测任务调用) */
    uint32_t          wait_ms;          /*!< 等待窗口 (毫秒) */
} s_trig;

/* -------------------------------------------------------------------------- */
/* GPIO 触发                                                                   */
/* -------------------------------------------------------------------------- */

/**
 * @brief 配置触发引脚为输入并启用内部上/下拉
 *
 * 低电平有效 -> 启用上拉 (默认高，按下拉低触发)
 * 高电平有效 -> 启用下拉 (默认低，上拉高触发)
 *
 * @param gpio_num 引脚号
 * @param level    有效电平
 * @return ESP_OK 成功
 */
static esp_err_t trig_gpio_setup(uint8_t gpio_num, uint8_t level)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << gpio_num),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = (level == IAP_CFG_GPIO_TRIG_ACTIVE_LOW)
                        ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = (level == IAP_CFG_GPIO_TRIG_ACTIVE_HIGH)
                        ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "配置触发引脚 GPIO%u 失败: %s",
                 gpio_num, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "触发引脚 GPIO%u 已配置为输入，%s有效 (内部%s)",
             gpio_num,
             (level == IAP_CFG_GPIO_TRIG_ACTIVE_LOW) ? "低电平" : "高电平",
             (level == IAP_CFG_GPIO_TRIG_ACTIVE_LOW) ? "上拉" : "下拉");
    return ESP_OK;
}

/**
 * @brief 读取触发引脚当前电平是否有效
 *
 * @return true 表示检测到有效电平
 */
static bool trig_gpio_active(void)
{
    int level = gpio_get_level((gpio_num_t)s_trig.gpio_num);
    return (s_trig.gpio_level == IAP_CFG_GPIO_TRIG_ACTIVE_HIGH)
           ? (level != 0) : (level == 0);
}

/* -------------------------------------------------------------------------- */
/* UART 触发                                                                   */
/* -------------------------------------------------------------------------- */

/**
 * @brief 检查 UART 是否收到进入 IAP 的命令
 *
 * 识别规则:
 *   - 收到任意可打印字符即开始累积到行缓冲
 *   - 收到回车/换行时比较是否为 "iap" (不区分大小写)
 *   - 为兼容旧行为，收到单个非打印字符 (如 Ctrl+C) 也视为触发
 *
 * @return true 表示收到进入命令
 */
static bool trig_uart_check(void)
{
    if (!s_trig.uart_ready) {
        return false;
    }

    static char line[16];
    static size_t line_len = 0;

    uint8_t ch;
    while (uart_read_bytes(s_trig.uart_num, &ch, 1, 0) == 1) {
        if (ch == '\r' || ch == '\n') {
            if (line_len > 0) {
                line[line_len] = '\0';
                line_len = 0;

                /* 不区分大小写比较 "iap" */
                if (strcasecmp(line, "iap") == 0) {
                    ESP_LOGI(TAG, "收到 UART 进入命令: \"%s\"", line);
                    return true;
                }
                ESP_LOGD(TAG, "忽略 UART 输入: \"%s\"", line);
            }
            continue;
        }

        if (ch >= 0x20 && ch < 0x7F) {
            if (line_len < sizeof(line) - 1) {
                line[line_len++] = (char)ch;
            } else {
                /* 行过长，丢弃 */
                line_len = 0;
            }
        } else {
            /* 非打印字符 (如 Ctrl+C): 兼容旧行为，直接触发 */
            ESP_LOGI(TAG, "收到 UART 控制字符 (0x%02X)，视为进入命令", ch);
            return true;
        }
    }
    return false;
}

/* -------------------------------------------------------------------------- */
/* USB 触发                                                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief 检查 USB 串口是否收到进入 IAP 的命令
 *
 * 识别规则与 UART 完全一致:
 *   - 收到任意可打印字符即开始累积到行缓冲
 *   - 收到回车/换行时比较是否为 "iap" (不区分大小写)
 *   - 收到单个非打印字符 (如 Ctrl+C) 也视为触发
 *
 * @return true 表示收到进入命令
 */
static bool trig_usb_check(void)
{
    if (!s_trig.usb_ready || !iap_usb_ready()) {
        return false;
    }

    static char line[16];
    static size_t line_len = 0;

    uint8_t ch;
    while (iap_usb_read(&ch, 1) == 1) {
        if (ch == '\r' || ch == '\n') {
            if (line_len > 0) {
                line[line_len] = '\0';
                line_len = 0;

                /* 不区分大小写比较 "iap" */
                if (strcasecmp(line, "iap") == 0) {
                    ESP_LOGI(TAG, "收到 USB 进入命令: \"%s\"", line);
                    return true;
                }
                ESP_LOGD(TAG, "忽略 USB 输入: \"%s\"", line);
            }
            continue;
        }

        if (ch >= 0x20 && ch < 0x7F) {
            if (line_len < sizeof(line) - 1) {
                line[line_len++] = (char)ch;
            } else {
                line_len = 0;
            }
        } else {
            ESP_LOGI(TAG, "收到 USB 控制字符 (0x%02X)，视为进入命令", ch);
            return true;
        }
    }
    return false;
}

/* -------------------------------------------------------------------------- */
/* 检测任务                                                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief 子系统启动任务
 *
 * I2C/WiFi 的启动可能耗时较长 (WiFi 需初始化射频、PHY 校准)，
 * 若在检测任务中同步执行会阻塞轮询。因此放到独立任务中启动，
 * 检测任务继续轮询 GPIO/UART。
 */
static void subsys_start_task(void *arg)
{
    (void)arg;

    if (s_trig.start_i2c) {
        esp_err_t err = s_trig.start_i2c();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "等待阶段启动 I2C 失败: %s，I2C 触发已禁用",
                     esp_err_to_name(err));
            s_trig.enabled &= ~TRIG_SRC_I2C;
        }
    }

    if (s_trig.start_wifi) {
        esp_err_t err = s_trig.start_wifi();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "等待阶段启动 WiFi 失败: %s，WiFi 触发已禁用",
                     esp_err_to_name(err));
            s_trig.enabled &= ~TRIG_SRC_WIFI;
        }
    }

    vTaskDelete(NULL);
}

/**
 * @brief 等待触发检测任务
 *
 * 轮询各已使能的触发源，任一命中即记录原因、置位 fired_mask，
 * 并通知主任务。
 */
static void wait_trigger_task(void *arg)
{
    /* 使用静态副本，避免依赖调用方的栈变量生命周期 */
    uint32_t wait_ms = s_trig.wait_ms;
    uint32_t elapsed = 0;

    (void)arg;

    ESP_LOGI(TAG, "触发检测已启动 (源掩码=0x%02" PRIx32 ", 窗口=%" PRIu32 " ms)",
             s_trig.enabled, wait_ms);

    /*
     * 异步启动 I2C 与 WiFi。
     *
     * 这两个子系统启动较慢 (WiFi 需射频初始化与 PHY 校准)，
     * 放到独立任务中执行，避免阻塞本任务的轮询。
     */
    if (s_trig.start_i2c || s_trig.start_wifi) {
        if (xTaskCreate(subsys_start_task, "iap_subsys", 4096, NULL, 4, NULL) != pdPASS) {
            ESP_LOGW(TAG, "创建子系统启动任务失败");
        }
    }

    while (elapsed < wait_ms && !s_trig.stop && s_trig.fired_mask == 0) {
        /* --- GPIO --- */
        if ((s_trig.enabled & TRIG_SRC_GPIO) && trig_gpio_active()) {
            ESP_LOGW(TAG, "GPIO%u 检测到有效电平，进入 IAP 下载模式", s_trig.gpio_num);
            s_trig.reason = IAP_BOOT_REASON_TRIG_GPIO;
            s_trig.fired_mask |= TRIG_SRC_GPIO;
            break;
        }

        /* --- UART --- */
        if ((s_trig.enabled & TRIG_SRC_UART) && trig_uart_check()) {
            s_trig.reason = IAP_BOOT_REASON_TRIG_UART;
            s_trig.fired_mask |= TRIG_SRC_UART;
            break;
        }

        /* --- USB --- */
        if ((s_trig.enabled & TRIG_SRC_USB) && trig_usb_check()) {
            s_trig.reason = IAP_BOOT_REASON_TRIG_USB;
            s_trig.fired_mask |= TRIG_SRC_USB;
            break;
        }

        /* I2C 与 WiFi 由各自的事件/命令处理回调置位 fired_mask */
        if (s_trig.fired_mask != 0) {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(WAIT_POLL_MS));
        elapsed += WAIT_POLL_MS;
    }

    if (s_trig.fired_mask == 0) {
        ESP_LOGI(TAG, "触发窗口结束，无触发");
    }

    /* 通知主任务 */
    if (s_trig.main_task) {
        xTaskNotifyGive(s_trig.main_task);
    }

    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------- */
/* 公共接口                                                                    */
/* -------------------------------------------------------------------------- */

esp_err_t iap_wait_trigger_start(const iap_cfg_data_t *cfg, uint32_t wait_ms,
                                 TaskHandle_t main_task,
                                 iap_wait_start_fn_t start_i2c,
                                 iap_wait_start_fn_t start_wifi)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_trig, 0, sizeof(s_trig));
    s_trig.uart_num  = (uart_port_t)cfg->uart_port;
    s_trig.main_task = main_task;
    s_trig.wait_ms   = wait_ms;

    /* 组装使能掩码 */
    if (cfg->flags & IAP_CFG_FLAG_WAIT_GPIO_TRIG) {
        s_trig.enabled |= TRIG_SRC_GPIO;
    }
    if (cfg->flags & IAP_CFG_FLAG_WAIT_I2C_TRIG) {
        s_trig.enabled |= TRIG_SRC_I2C;
    }
    if (cfg->flags & IAP_CFG_FLAG_WAIT_UART_TRIG) {
        s_trig.enabled |= TRIG_SRC_UART;
    }
    if (cfg->flags & IAP_CFG_FLAG_WAIT_WIFI_TRIG) {
        s_trig.enabled |= TRIG_SRC_WIFI;
    }
    if (cfg->flags & IAP_CFG_FLAG_WAIT_USB_TRIG) {
        s_trig.enabled |= TRIG_SRC_USB;
    }

    if (s_trig.enabled == 0) {
        ESP_LOGI(TAG, "所有等待触发源均已禁用");
        /*
         * 仍需通知主任务，否则主任务会白等满超时时间。
         * 此处直接返回，由调用方通过 iap_wait_trigger_fired() == false
         * 判定为无触发；同时主动通知一次让主任务立即继续。
         */
        if (main_task) {
            xTaskNotifyGive(main_task);
        }
        return ESP_OK;
    }

    s_trig.gpio_num   = cfg->trig_gpio;
    s_trig.gpio_level = cfg->trig_gpio_level;
    s_trig.reason     = IAP_BOOT_REASON_NONE;

    /* --- GPIO: 立即配置引脚 (在等待开始前完成) --- */
    if (s_trig.enabled & TRIG_SRC_GPIO) {
        if (trig_gpio_setup(s_trig.gpio_num, s_trig.gpio_level) != ESP_OK) {
            s_trig.enabled &= ~TRIG_SRC_GPIO;
        } else {
            /* 引脚在等待开始前就已是有效电平，立即触发 */
            if (trig_gpio_active()) {
                ESP_LOGW(TAG, "GPIO%u 在等待开始前已为有效电平", s_trig.gpio_num);
                s_trig.reason = IAP_BOOT_REASON_TRIG_GPIO;
                s_trig.fired_mask |= TRIG_SRC_GPIO;
            }
        }
    }

    /* --- UART: 驱动可能尚未安装，此处仅探测可用性 --- */
    if (s_trig.enabled & TRIG_SRC_UART) {
        size_t buffered = 0;
        if (uart_get_buffered_data_len(s_trig.uart_num, &buffered) == ESP_OK) {
            s_trig.uart_ready = true;
        } else {
            ESP_LOGW(TAG, "UART 驱动不可用，UART 触发已禁用");
            s_trig.enabled &= ~TRIG_SRC_UART;
        }
    }

    /* --- USB: 驱动由 main 在等待前提前安装 (iap_usb_prepare) --- */
    if (s_trig.enabled & TRIG_SRC_USB) {
        if (iap_usb_ready()) {
            s_trig.usb_ready = true;
        } else {
            ESP_LOGW(TAG, "USB 驱动不可用，USB 触发已禁用");
            s_trig.enabled &= ~TRIG_SRC_USB;
        }
    }

    /* --- I2C/WiFi: 记录回调，由检测任务异步启动 --- */
    /*
     * 注意: 不能在此处同步调用 start_i2c/start_wifi。
     * iap_wifi_start() 会阻塞等待 AP 启动完成 (最多 10 秒)，
     * 若在 app_main 中同步调用，会吃掉整个等待窗口。
     * 因此把启动动作放到检测任务里，与计时并行进行。
     */
    s_trig.start_i2c  = (s_trig.enabled & TRIG_SRC_I2C)  ? start_i2c  : NULL;
    s_trig.start_wifi = (s_trig.enabled & TRIG_SRC_WIFI) ? start_wifi : NULL;

    if ((s_trig.enabled & TRIG_SRC_I2C) && start_i2c == NULL) {
        ESP_LOGW(TAG, "未提供 I2C 启动回调，I2C 触发已禁用");
        s_trig.enabled &= ~TRIG_SRC_I2C;
    }
    if ((s_trig.enabled & TRIG_SRC_WIFI) && start_wifi == NULL) {
        ESP_LOGW(TAG, "未提供 WiFi 启动回调，WiFi 触发已禁用");
        s_trig.enabled &= ~TRIG_SRC_WIFI;
    }

    if (s_trig.enabled == 0) {
        ESP_LOGW(TAG, "所有触发源启动失败，退化为纯延时");
        if (main_task) {
            xTaskNotifyGive(main_task);
        }
        return ESP_OK;
    }

    /* 启动检测任务 */
    if (xTaskCreate(wait_trigger_task, "iap_wait", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "创建触发检测任务失败");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void iap_wait_trigger_stop(void)
{
    s_trig.stop = true;
}

bool iap_wait_trigger_fired(void)
{
    return s_trig.fired_mask != 0;
}

iap_boot_reason_t iap_wait_trigger_reason(void)
{
    return s_trig.reason;
}

void iap_wait_trigger_notify_i2c(void)
{
    if ((s_trig.enabled & TRIG_SRC_I2C) && s_trig.fired_mask == 0) {
        ESP_LOGW(TAG, "I2C 收到进入命令，进入 IAP 下载模式");
        s_trig.reason = IAP_BOOT_REASON_TRIG_I2C;
        s_trig.fired_mask |= TRIG_SRC_I2C;
    }
}

void iap_wait_trigger_notify_wifi(void)
{
    if ((s_trig.enabled & TRIG_SRC_WIFI) && s_trig.fired_mask == 0) {
        ESP_LOGW(TAG, "WiFi 客户端接入，进入 IAP 下载模式");
        s_trig.reason = IAP_BOOT_REASON_TRIG_WIFI;
        s_trig.fired_mask |= TRIG_SRC_WIFI;
    }
}
