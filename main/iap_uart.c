/*
 * ESP IAP - UART 终端
 *
 * 提供一个简单的行编辑终端，支持以下命令:
 *
 *   help                         显示帮助
 *   info                         显示系统信息
 *   cfg                          显示 IAP 配置
 *   cfg set <key> <value>        修改配置项
 *   cfg save                     保存配置
 *   cfg reset                    恢复默认配置
 *   app info                     显示用户程序信息
 *   app erase                    擦除用户程序区
 *   app boot                     重启进入用户程序
 *   iap boot                     重启进入 IAP
 *   iap download on|off          设置下载模式标志
 *   bootloader                   检查 bootloader 与本固件的字段一致性
 *   xmodem recv [offset]         以 XMODEM 接收镜像并写入可变区 (默认 0x140000, Ctrl+C 取消)
 *   xmodem send [offset] [len]   以 XMODEM 发送可变区内容 (默认 0x140000 ~ flash 末尾, Ctrl+C 取消)
 *   reboot                       重启设备
 *
 * 终端使用 UART0 (与日志共用)，通过命令行输入区分。
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_app_desc.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"

#include "iap_common.h"
#include "iap_crc.h"
#include "iap_config.h"
#include "iap_image.h"
#include "iap_xmodem.h"
#include "iap_boot_param.h"     /* IAP_BOOT_MAX_RETRY (防砖计数上限) */
#include "iap_uart.h"

static const char *TAG = "iap_term";

/**
 * 终端 UART 端口与引脚
 *
 * 端口号与引脚均可通过配置区修改:
 *   - uart_port    : 0 / 1 / 2
 *   - uart_tx_gpio : 0xFF = 用该端口默认引脚，否则复用为指定 GPIO
 *   - uart_rx_gpio : 同上
 *
 * 默认 (uart_port=0, tx=rx=0xFF) 即 ESP 系列默认日志输出口:
 *   ESP32/S2/S3        = TX GPIO1  / RX GPIO3
 *   ESP32-C2/C3/C6/H2  = TX GPIO21 / RX GPIO20
 *
 * 注意: UART1/2 在部分芯片上没有默认引脚映射（uart_get_pin 返回 -1），
 *       此时必须显式指定 TX/RX 引脚，否则终端无法工作。
 */
static uart_port_t s_uart_num = UART_NUM_0;

/** 实际生效的 TX/RX 引脚 (-1 表示未绑定) */
static int s_tx_pin = -1;
static int s_rx_pin = -1;

/**
 * @brief 查询指定 UART 端口的芯片默认 TX 引脚
 *
 * 只有 UART0 在 ESP 全系列都有默认引脚（即默认日志输出口）:
 *   ESP32/S2/S3        = GPIO1  (ESP32-S2/S3 为 GPIO43)
 *   ESP32-C2/C3/C6/H2  = GPIO21 (ESP32-H2 为 GPIO24)
 *
 * UART1/2 无默认映射，返回 -1（必须显式配置引脚）。
 */
static int uart_default_tx_pin(uart_port_t port)
{
    if (port != UART_NUM_0) {
        return -1;
    }
#if CONFIG_IDF_TARGET_ESP32
    return 1;
#elif defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32S3)
    return 43;
#elif defined(CONFIG_IDF_TARGET_ESP32C2) || defined(CONFIG_IDF_TARGET_ESP32C3) || \
      defined(CONFIG_IDF_TARGET_ESP32C6)
    return 21;
#elif defined(CONFIG_IDF_TARGET_ESP32H2)
    return 24;
#else
    return -1;   /* 未知芯片，保守返回未绑定 */
#endif
}

/**
 * @brief 查询指定 UART 端口的芯片默认 RX 引脚
 *
 * @see uart_default_tx_pin
 */
static int uart_default_rx_pin(uart_port_t port)
{
    if (port != UART_NUM_0) {
        return -1;
    }
#if CONFIG_IDF_TARGET_ESP32
    return 3;
#elif defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32S3)
    return 44;
#elif defined(CONFIG_IDF_TARGET_ESP32C2) || defined(CONFIG_IDF_TARGET_ESP32C3) || \
      defined(CONFIG_IDF_TARGET_ESP32C6)
    return 20;
#elif defined(CONFIG_IDF_TARGET_ESP32H2)
    return 23;
#else
    return -1;
#endif
}

/** 取消标志 (用于中断 XMODEM 传输) */
static volatile bool s_cancel = false;

/** 终端读取任务 (实现见文件末尾) */
static void term_task(void *arg);

/* -------------------------------------------------------------------------- */
/* 终端后端注册表                                                              */
/* -------------------------------------------------------------------------- */

/** 最多同时挂载的后端数 (UART + USB) */
#define IAP_TERM_MAX_BACKENDS   2

static const iap_term_backend_t *s_backends[IAP_TERM_MAX_BACKENDS];
static int s_backend_count = 0;

/*
 * 当前正在执行命令的终端后端。
 *
 * 每个后端有独立的读取任务 (见 term_task)，命令在哪个后端的任务里
 * 被解析执行，XMODEM 等需要独占通道的操作就必须用**同一个后端** ——
 * 否则会出现「终端在 USB、XMODEM 却在 UART0」的错配，
 * 表现为握手字符发到了没人接的串口上，传输永远无法开始。
 */
static const iap_term_backend_t *s_active_backend = NULL;

/** 取得当前执行命令的后端 (可能为 NULL，如从非终端上下文调用) */
static const iap_term_backend_t *term_active_backend(void)
{
    return s_active_backend;
}

esp_err_t iap_term_attach(const iap_term_backend_t *backend, const char *task_name)
{
    if (backend == NULL || s_backend_count >= IAP_TERM_MAX_BACKENDS) {
        return ESP_ERR_NO_MEM;
    }
    if (backend->ready && !backend->ready()) {
        ESP_LOGW(TAG, "%s 后端未就绪，跳过", backend->name);
        return ESP_OK;
    }

    s_backends[s_backend_count++] = backend;

    BaseType_t ret = xTaskCreate(term_task, task_name, 6144,
                                 (void *)backend, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "创建 %s 终端任务失败", backend->name);
        s_backend_count--;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "终端后端已挂载: %s", backend->name);
    return ESP_OK;
}

bool iap_term_any_ready(void)
{
    for (int i = 0; i < s_backend_count; i++) {
        if (!s_backends[i]->ready || s_backends[i]->ready()) {
            return true;
        }
    }
    return false;
}

/* -------------------------------------------------------------------------- */
/* 输出辅助                                                                    */
/* -------------------------------------------------------------------------- */

/*
 * 输出静默开关
 *
 * XMODEM 传输期间必须**完全静默**终端输出:
 *   日志与提示文本会与 XMODEM 数据 (ACK/NAK/包体) 混在同一字节流里,
 *   发送方逐字节解析时会先读到日志字符 (如 'I'、中文 UTF-8 字节),
 *   判定不是 ACK 而重传, 造成「设备已正确收包但发送方永远等不到 ACK」
 *   的死循环。
 *
 * ⚠️ 必须同时拦截两条输出路径:
 *   1. iap_term_write / iap_term_printf  (终端引擎, 走所有后端)
 *   2. ESP_LOGx                          (IDF 日志, 直接写控制台设备)
 *      实测 ESP32-C6 上 IDF 日志直接写到 USB-Serial-JTAG,
 *      绕过终端引擎, 因此必须用 esp_log_set_vprintf 单独拦截。
 */
static volatile bool s_term_mute = false;

/** 原始 IDF 日志输出函数 (被我们替换前的那个) */
static vprintf_like_t s_log_vprintf_orig = NULL;

/** 替换后的日志输出: 静默期间直接丢弃 */
static int log_vprintf_muted(const char *fmt, va_list ap)
{
    if (s_term_mute) {
        return 0;
    }
    if (s_log_vprintf_orig) {
        return s_log_vprintf_orig(fmt, ap);
    }
    return vprintf(fmt, ap);
}

/** 安装日志拦截 (幂等) */
static void log_hook_install(void)
{
    if (s_log_vprintf_orig == NULL) {
        s_log_vprintf_orig = esp_log_set_vprintf(log_vprintf_muted);
    }
}

/** 设置终端输出静默 (供 XMODEM 等独占通道的会话使用) */
void iap_term_set_mute(bool mute)
{
    log_hook_install();
    s_term_mute = mute;
}

/**
 * @brief 等待所有后端的发送缓冲排空
 *
 * ⚠️ USB-Serial-JTAG 的 write 是**异步**的: 数据先进入驱动缓冲,
 *    再由硬件在主机轮询时发出。若在提示文本尚未发完时就开始
 *    XMODEM 会话, 这些残留字节会被发送方当作协议数据 (ACK/NAK) 读走,
 *    造成「设备已正确收包但发送方判定 ACK 无效」的假象。
 *
 *    因此进入独占会话前必须调用本函数。后端若实现了 flush
 *    (如 USB 的 wait_tx_done) 则调用之, 否则退化为短暂延时。
 */
void iap_term_flush(void)
{
    for (int i = 0; i < s_backend_count; i++) {
        if (s_backends[i]->flush) {
            s_backends[i]->flush();
        }
    }
    /* 兜底延时: 覆盖无 flush 实现的通道 (UART 为同步写, 无需等待) */
    vTaskDelay(pdMS_TO_TICKS(20));
}

/**
 * @brief 向所有已注册后端输出字符串
 */
void iap_term_write(const char *s)
{
    if (s_term_mute) {
        return;
    }
    size_t len = strlen(s);
    for (int i = 0; i < s_backend_count; i++) {
        if (s_backends[i]->write) {
            s_backends[i]->write((const uint8_t *)s, len);
        }
    }
}

/**
 * @brief 格式化输出到所有已注册后端
 */
void iap_term_printf(const char *fmt, ...)
{
    if (s_term_mute) {
        return;
    }
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        if (n > (int)sizeof(buf) - 1) {
            n = sizeof(buf) - 1;
        }
        for (int i = 0; i < s_backend_count; i++) {
            if (s_backends[i]->write) {
                s_backends[i]->write((const uint8_t *)buf, (size_t)n);
            }
        }
    }
}

/** 兼容旧调用名 (内部实现见 iap_term_write / iap_term_printf) */
#define term_puts(s)     iap_term_write(s)
#define term_printf(...) iap_term_printf(__VA_ARGS__)

/* -------------------------------------------------------------------------- */
/* 命令实现: 系统信息                                                          */
/* -------------------------------------------------------------------------- */

static void cmd_info(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    const char *model;
    switch (chip.model) {
    case CHIP_ESP32:   model = "ESP32";   break;
    case CHIP_ESP32S2: model = "ESP32-S2"; break;
    case CHIP_ESP32S3: model = "ESP32-S3"; break;
    case CHIP_ESP32C3: model = "ESP32-C3"; break;
    case CHIP_ESP32C6: model = "ESP32-C6"; break;
    case CHIP_ESP32H2: model = "ESP32-H2"; break;
    default:           model = "未知";     break;
    }

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    term_printf("\r\n===== 系统信息 =====\r\n");
    term_printf("IAP 版本     : %s\r\n", IAP_VERSION_STRING);
    term_printf("芯片型号     : %s (rev %d, %d 核)\r\n", model, chip.revision, chip.cores);
    term_printf("Flash 大小   : %" PRIu32 " MB\r\n", flash_size / (1024 * 1024));
    term_printf("IDF 版本     : %s\r\n", esp_get_idf_version());
    term_printf("MAC 地址     : %02X:%02X:%02X:%02X:%02X:%02X\r\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    term_printf("空闲堆       : %" PRIu32 " 字节\r\n", (uint32_t)esp_get_free_heap_size());
    term_printf("最小空闲堆   : %" PRIu32 " 字节\r\n", (uint32_t)esp_get_minimum_free_heap_size());
    term_printf("运行时间     : %" PRIu64 " ms\r\n", esp_timer_get_time() / 1000);
    term_printf("====================\r\n\r\n");
}

/**
 * @brief 显示分区表与 flash 容量一致性 (part 命令)
 *
 * 便于现场确认模块的 flash 容量与分区表是否匹配。
 */
static void cmd_part(void)
{
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    const esp_partition_t *user = iap_image_get_user_partition();
    const esp_partition_t *iap  = iap_image_get_iap_partition();

    term_printf("\r\n===== 分区信息 =====\r\n");
    term_printf("Flash 总容量 : %" PRIu32 " KB (0x%08" PRIX32 ")\r\n",
                flash_size / 1024, flash_size);

    if (iap) {
        term_printf("IAP 程序区   : 0x%08" PRIX32 " ~ 0x%08" PRIX32
                    "  (%" PRIu32 " KB)\r\n",
                    iap->address, iap->address + iap->size, iap->size / 1024);
    }

    if (user) {
        uint32_t end = user->address + user->size;
        term_printf("用户程序区   : 0x%08" PRIX32 " ~ 0x%08" PRIX32
                    "  (%" PRIu32 " KB)\r\n",
                    user->address, end, user->size / 1024);

        if (flash_size >= end) {
            uint32_t unused = flash_size - end;
            term_printf("未使用空间   : %" PRIu32 " KB\r\n", unused / 1024);
            if (unused >= 0x100000) {
                term_printf("提示         : 有 %" PRIu32 " KB 未分配，"
                            "可用 tools/gen_partitions.py 扩大 user_app\r\n",
                            unused / 1024);
            }
        } else {
            term_printf("!! 错误      : 分区表末尾 (0x%08" PRIX32
                        ") 超出 flash 容量 %" PRIu32 " KB\r\n",
                        end, flash_size / 1024);
            term_printf("   请用 tools/gen_partitions.py 按实际容量重新生成分区表，\r\n");
            term_printf("   并单独烧录到 0x8000 (无需重烧整个固件)\r\n");
        }
    } else {
        term_puts("用户程序区   : 未找到\r\n");
    }

    term_printf("====================\r\n\r\n");
}

static void cmd_cfg_show(void)
{
    iap_cfg_data_t cfg;
    if (iap_config_get(&cfg) != ESP_OK) {
        term_puts("读取配置失败\r\n");
        return;
    }

    char ip[16], mask[16];
    iap_config_get_ip_string(ip, sizeof(ip));
    iap_config_get_netmask_string(mask, sizeof(mask));

    term_printf("\r\n===== IAP 配置 =====\r\n");
    term_printf("标志         : 0x%08" PRIx32 "\r\n", cfg.flags);
    term_printf("  下载模式   : %s\r\n", (cfg.flags & IAP_CFG_FLAG_DOWNLOAD_MODE) ? "开" : "关");
    term_printf("  WiFi       : %s\r\n", (cfg.flags & IAP_CFG_FLAG_WIFI_ENABLE) ? "开" : "关");
    term_printf("  I2C        : %s\r\n", (cfg.flags & IAP_CFG_FLAG_I2C_ENABLE) ? "开" : "关");
    term_printf("  UART       : %s\r\n", (cfg.flags & IAP_CFG_FLAG_UART_ENABLE) ? "开" : "关");
    term_printf("  启动校验   : %s (由 bootloader 执行)\r\n", (cfg.flags & IAP_CFG_FLAG_VERIFY_USER_APP) ? "开" : "关");

    /* 等待期间触发源 */
    term_puts("等待触发源   :\r\n");
    if (cfg.flags & IAP_CFG_FLAG_WAIT_GPIO_TRIG) {
        term_printf("  GPIO       : 开 (GPIO%u, %s有效)\r\n",
                    cfg.trig_gpio,
                    (cfg.trig_gpio_level == IAP_CFG_GPIO_TRIG_ACTIVE_HIGH)
                        ? "高电平" : "低电平");
    } else {
        term_puts("  GPIO       : 关\r\n");
    }
    term_printf("  I2C        : %s\r\n",
                (cfg.flags & IAP_CFG_FLAG_WAIT_I2C_TRIG) ? "开" : "关");
    term_printf("  UART       : %s\r\n",
                (cfg.flags & IAP_CFG_FLAG_WAIT_UART_TRIG) ? "开" : "关");
    term_printf("  WiFi       : %s\r\n",
                (cfg.flags & IAP_CFG_FLAG_WAIT_WIFI_TRIG) ? "开" : "关");

    term_printf("等待时间     : %u 秒\r\n", cfg.wait_seconds);
    term_printf("用户程序长度 : %" PRIu32 "\r\n", cfg.user_app_size);
    term_printf("用户程序 CRC : 0x%08" PRIx32 "\r\n", cfg.user_app_crc32);
    term_printf("启动计数     : %" PRIu32 "\r\n", cfg.boot_count);
    term_printf("上次原因     : %s\r\n", iap_boot_reason_str((iap_boot_reason_t)cfg.last_boot_reason));
    term_printf("I2C SCL/SDA  : GPIO%u / GPIO%u\r\n", cfg.i2c_scl_gpio, cfg.i2c_sda_gpio);
    term_printf("I2C 地址     : 0x%02X\r\n", cfg.i2c_addr);
    term_printf("WiFi SSID    : %s\r\n", cfg.wifi_ssid);
    term_printf("WiFi 密码    : %s\r\n", cfg.wifi_password[0] ? (const char *)cfg.wifi_password : "(无)");
    term_printf("WiFi 信道    : %u\r\n", cfg.wifi_channel);
    term_printf("AP IP        : %s\r\n", ip);
    term_printf("AP 掩码      : %s\r\n", mask);

    /* UART: 显示端口、引脚与波特率 */
    char tx_str[16], rx_str[16];
    if (s_tx_pin < 0) snprintf(tx_str, sizeof(tx_str), "(未绑定)");
    else              snprintf(tx_str, sizeof(tx_str), "GPIO%d", s_tx_pin);
    if (s_rx_pin < 0) snprintf(rx_str, sizeof(rx_str), "(未绑定)");
    else              snprintf(rx_str, sizeof(rx_str), "GPIO%d", s_rx_pin);

    term_printf("UART         : UART%u, TX=%s, RX=%s, 波特率 %" PRIu32 "\r\n",
                (unsigned)s_uart_num, tx_str, rx_str, cfg.uart_baudrate);
    term_printf("  引脚来源   : TX=%s, RX=%s\r\n",
                cfg.uart_tx_gpio == IAP_CFG_UART_PIN_DEFAULT ? "默认" : "配置区",
                cfg.uart_rx_gpio == IAP_CFG_UART_PIN_DEFAULT ? "默认" : "配置区");

    /* 启动参数 */
    if (cfg.boot_param_flags & IAP_CFG_BOOT_PARAM_FLAG_VALID) {
        term_printf("启动参数     : %s\r\n", (const char *)cfg.boot_param);
        term_printf("  长度       : %u 字符\r\n", cfg.boot_param_len);
        term_printf("  CRC32      : 0x%08" PRIx32 "\r\n", cfg.boot_param_crc32);
        term_printf("  已消费     : %s\r\n",
                    (cfg.boot_param_flags & IAP_CFG_BOOT_PARAM_FLAG_CONSUMED) ? "是" : "否");
    } else {
        term_puts("启动参数     : (未设置)\r\n");
    }

    /* 多 OTA 槽 */
    term_printf("活动 OTA 槽  : %u / %u\r\n",
                (unsigned)cfg.active_slot, (unsigned)cfg.ota_slot_count);
    if (cfg.ota_gpio == IAP_OTA_GPIO_DISABLED) {
        term_puts("OTA 选择引脚 : (禁用)\r\n");
    } else {
        term_printf("OTA 选择引脚 : GPIO%u\r\n", (unsigned)cfg.ota_gpio);
    }

    /* 配置数据版本 */
    term_printf("配置数据版本 : %u (当前固件 %u)%s\r\n",
                (unsigned)cfg.data_ver, (unsigned)IAP_CFG_DATA_VER,
                (cfg.data_ver == IAP_CFG_DATA_VER) ? "" : "  <- 需要迁移");

    term_printf("====================\r\n\r\n");
}

/**
 * @brief 手动触发配置迁移 (cfg migrate)
 *
 * 正常情况下 iap_config_init() 会在启动时自动迁移。
 * 本命令用于手动重试或验证迁移结果。
 */
static void cmd_cfg_migrate(void)
{
    iap_cfg_data_t cfg;
    if (iap_config_get(&cfg) != ESP_OK) {
        term_puts("读取配置失败\r\n");
        return;
    }

    term_printf("当前配置数据版本: %u (固件支持 %u)\r\n",
                (unsigned)cfg.data_ver, (unsigned)IAP_CFG_DATA_VER);

    if (cfg.data_ver == IAP_CFG_DATA_VER) {
        term_puts("已是最新版本，无需迁移\r\n");
        return;
    }

    term_puts("开始迁移...\r\n");
    esp_err_t err = iap_config_migrate();
    if (err == ESP_OK) {
        term_puts("迁移完成，重启后生效\r\n");
        term_puts("可用 cfg 查看结果，或 reboot 重启\r\n");
    } else {
        term_printf("迁移失败: %s\r\n", esp_err_to_name(err));
    }
}

/**
 * @brief 解析配置项并修改
 *
 * 用法: cfg set <key> <value>
 */
static void cmd_cfg_set(const char *key, const char *value)
{
    iap_cfg_data_t cfg;
    if (iap_config_get(&cfg) != ESP_OK) {
        term_puts("读取配置失败\r\n");
        return;
    }

    bool ok = true;

    if (strcmp(key, "wait") == 0) {
        cfg.wait_seconds = (uint16_t)strtoul(value, NULL, 0);
    } else if (strcmp(key, "ssid") == 0) {
        memset(cfg.wifi_ssid, 0, sizeof(cfg.wifi_ssid));
        strncpy((char *)cfg.wifi_ssid, value, sizeof(cfg.wifi_ssid) - 1);
    } else if (strcmp(key, "pass") == 0) {
        memset(cfg.wifi_password, 0, sizeof(cfg.wifi_password));
        strncpy((char *)cfg.wifi_password, value, sizeof(cfg.wifi_password) - 1);
    } else if (strcmp(key, "channel") == 0) {
        cfg.wifi_channel = (uint8_t)strtoul(value, NULL, 0);
    } else if (strcmp(key, "ip") == 0) {
        unsigned a, b, c, d;
        if (sscanf(value, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
            cfg.wifi_ip = a | (b << 8) | (c << 16) | ((uint32_t)d << 24);
        } else {
            ok = false;
        }
    } else if (strcmp(key, "mask") == 0) {
        unsigned a, b, c, d;
        if (sscanf(value, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
            cfg.wifi_netmask = a | (b << 8) | (c << 16) | ((uint32_t)d << 24);
        } else {
            ok = false;
        }
    } else if (strcmp(key, "scl") == 0) {
        cfg.i2c_scl_gpio = (uint8_t)strtoul(value, NULL, 0);
    } else if (strcmp(key, "sda") == 0) {
        cfg.i2c_sda_gpio = (uint8_t)strtoul(value, NULL, 0);
    } else if (strcmp(key, "i2caddr") == 0) {
        cfg.i2c_addr = (uint8_t)strtoul(value, NULL, 0);
    } else if (strcmp(key, "uartport") == 0) {
        cfg.uart_port = (uint8_t)strtoul(value, NULL, 0);
    } else if (strcmp(key, "uarttx") == 0) {
        /* 0xFF (或 "default") 表示使用该端口的芯片默认引脚 */
        cfg.uart_tx_gpio = (strcmp(value, "default") == 0)
                           ? IAP_CFG_UART_PIN_DEFAULT
                           : (uint8_t)strtoul(value, NULL, 0);
    } else if (strcmp(key, "uartrx") == 0) {
        cfg.uart_rx_gpio = (strcmp(value, "default") == 0)
                           ? IAP_CFG_UART_PIN_DEFAULT
                           : (uint8_t)strtoul(value, NULL, 0);
    } else if (strcmp(key, "baud") == 0) {
        cfg.uart_baudrate = strtoul(value, NULL, 0);
    } else if (strcmp(key, "download") == 0) {
        if (strcmp(value, "on") == 0 || strcmp(value, "1") == 0) {
            cfg.flags |= IAP_CFG_FLAG_DOWNLOAD_MODE;
        } else {
            cfg.flags &= ~IAP_CFG_FLAG_DOWNLOAD_MODE;
        }
    } else if (strcmp(key, "usb") == 0) {
        if (strcmp(value, "on") == 0 || strcmp(value, "1") == 0) {
            cfg.flags |= IAP_CFG_FLAG_USB_ENABLE;
        } else {
            cfg.flags &= ~IAP_CFG_FLAG_USB_ENABLE;
        }
    } else if (strcmp(key, "usbtrig") == 0) {
        if (strcmp(value, "on") == 0 || strcmp(value, "1") == 0) {
            cfg.flags |= IAP_CFG_FLAG_WAIT_USB_TRIG;
        } else {
            cfg.flags &= ~IAP_CFG_FLAG_WAIT_USB_TRIG;
        }
    } else if (strcmp(key, "wifi") == 0) {
        if (strcmp(value, "on") == 0 || strcmp(value, "1") == 0) {
            cfg.flags |= IAP_CFG_FLAG_WIFI_ENABLE;
        } else {
            cfg.flags &= ~IAP_CFG_FLAG_WIFI_ENABLE;
        }
    } else if (strcmp(key, "i2c") == 0) {
        if (strcmp(value, "on") == 0 || strcmp(value, "1") == 0) {
            cfg.flags |= IAP_CFG_FLAG_I2C_ENABLE;
        } else {
            cfg.flags &= ~IAP_CFG_FLAG_I2C_ENABLE;
        }
    } else if (strcmp(key, "verify") == 0) {
        if (strcmp(value, "on") == 0 || strcmp(value, "1") == 0) {
            cfg.flags |= IAP_CFG_FLAG_VERIFY_USER_APP;
        } else {
            cfg.flags &= ~IAP_CFG_FLAG_VERIFY_USER_APP;
        }
    } else if (strcmp(key, "trig") == 0) {
        /* 触发引脚号; "off" 或 "none" 表示禁用 */
        if (strcmp(value, "off") == 0 || strcmp(value, "none") == 0) {
            cfg.trig_gpio = IAP_CFG_DEFAULT_TRIG_GPIO;
            cfg.flags &= ~IAP_CFG_FLAG_WAIT_GPIO_TRIG;
        } else {
            cfg.trig_gpio = (uint8_t)strtoul(value, NULL, 0);
            cfg.flags |= IAP_CFG_FLAG_WAIT_GPIO_TRIG;
        }
    } else if (strcmp(key, "triglevel") == 0) {
        /* 有效电平: low / high */
        cfg.trig_gpio_level = (strcmp(value, "high") == 0 ||
                               strcmp(value, "1") == 0)
                              ? IAP_CFG_GPIO_TRIG_ACTIVE_HIGH
                              : IAP_CFG_GPIO_TRIG_ACTIVE_LOW;
    } else if (strcmp(key, "trig_gpio") == 0) {
        if (strcmp(value, "on") == 0 || strcmp(value, "1") == 0) {
            cfg.flags |= IAP_CFG_FLAG_WAIT_GPIO_TRIG;
        } else {
            cfg.flags &= ~IAP_CFG_FLAG_WAIT_GPIO_TRIG;
        }
    } else if (strcmp(key, "trig_i2c") == 0) {
        if (strcmp(value, "on") == 0 || strcmp(value, "1") == 0) {
            cfg.flags |= IAP_CFG_FLAG_WAIT_I2C_TRIG;
        } else {
            cfg.flags &= ~IAP_CFG_FLAG_WAIT_I2C_TRIG;
        }
    } else if (strcmp(key, "trig_uart") == 0) {
        if (strcmp(value, "on") == 0 || strcmp(value, "1") == 0) {
            cfg.flags |= IAP_CFG_FLAG_WAIT_UART_TRIG;
        } else {
            cfg.flags &= ~IAP_CFG_FLAG_WAIT_UART_TRIG;
        }
    } else if (strcmp(key, "trig_wifi") == 0) {
        if (strcmp(value, "on") == 0 || strcmp(value, "1") == 0) {
            cfg.flags |= IAP_CFG_FLAG_WAIT_WIFI_TRIG;
        } else {
            cfg.flags &= ~IAP_CFG_FLAG_WAIT_WIFI_TRIG;
        }
    } else {
        term_printf("未知配置项: %s\r\n", key);
        term_puts("可用项: wait ssid pass channel ip mask scl sda i2caddr\r\n");
        term_puts("        uartport uarttx uartrx baud download usb usbtrig\r\n");
        term_puts("        wifi i2c verify\r\n");
        term_puts("        download wifi i2c verify\r\n");
        term_puts("        trig triglevel trig_gpio trig_i2c trig_uart trig_wifi\r\n");
        return;
    }

    if (!ok) {
        term_printf("值格式错误: %s\r\n", value);
        return;
    }

    if (iap_config_set(&cfg) != ESP_OK) {
        term_puts("保存配置失败\r\n");
        return;
    }
    term_printf("已设置 %s = %s\r\n", key, value);
}

/**
 * @brief 显示或设置启动参数
 *
 * 用法:
 *   bootparam                      显示当前启动参数
 *   bootparam set <string>         设置启动参数
 *   bootparam clear                清除启动参数
 *   bootparam get <key>            提取指定键的值
 */
static void cmd_bootparam(int argc, char **argv)
{
    /* 参数缓冲区较大 (256B)，使用静态存储避免占用任务栈 */
    static char buf[IAP_CFG_BOOT_PARAM_SIZE];

    if (argc == 1) {
        esp_err_t err = iap_config_get_boot_param(buf, sizeof(buf));

        term_puts("\r\n===== 启动参数 =====\r\n");
        if (err == ESP_OK) {
            term_printf("内容     : %s\r\n", buf);
            term_printf("长度     : %u 字符\r\n", (unsigned)strlen(buf));
            term_printf("已消费   : %s\r\n",
                        iap_config_boot_param_consumed() ? "是" : "否");
        } else if (err == ESP_ERR_NOT_FOUND) {
            term_puts("状态     : 未设置\r\n");
        } else {
            term_printf("状态     : 读取失败 (%s)\r\n", esp_err_to_name(err));
        }
        term_puts("====================\r\n\r\n");
        return;
    }

    if (strcmp(argv[1], "set") == 0) {
        if (argc < 3) {
            term_puts("用法: bootparam set <string>\r\n");
            return;
        }
        esp_err_t err = iap_config_set_boot_param(argv[2]);
        if (err == ESP_OK) {
            term_puts("启动参数已设置\r\n");
        } else if (err == ESP_ERR_INVALID_SIZE) {
            term_printf("设置失败: 参数过长 (上限 %d 字符)\r\n",
                        IAP_CFG_BOOT_PARAM_MAX_LEN);
        } else {
            term_printf("设置失败: %s\r\n", esp_err_to_name(err));
        }
        return;
    }

    if (strcmp(argv[1], "clear") == 0) {
        if (iap_config_clear_boot_param() == ESP_OK) {
            term_puts("启动参数已清除\r\n");
        } else {
            term_puts("清除失败\r\n");
        }
        return;
    }

    if (strcmp(argv[1], "get") == 0) {
        if (argc < 3) {
            term_puts("用法: bootparam get <key>\r\n");
            return;
        }
        static char val[IAP_CFG_BOOT_PARAM_SIZE];
        esp_err_t err = iap_config_get_boot_param(buf, sizeof(buf));
        if (err != ESP_OK) {
            term_printf("读取启动参数失败: %s\r\n", esp_err_to_name(err));
            return;
        }
        err = iap_config_boot_param_get_value(buf, argv[2], val, sizeof(val));
        if (err == ESP_OK) {
            term_printf("%s = %s\r\n", argv[2], val);
        } else {
            term_printf("未找到键 '%s'\r\n", argv[2]);
        }
        return;
    }

    term_puts("用法: bootparam [set <string> | clear | get <key>]\r\n");
}

/* -------------------------------------------------------------------------- */
/* 命令实现: 用户程序                                                          */
/* -------------------------------------------------------------------------- */

static void cmd_app_info(void)
{
    iap_image_info_t info;
    if (iap_image_get_info(&info) != ESP_OK) {
        term_puts("读取用户程序信息失败\r\n");
        return;
    }

    uint8_t slot = iap_image_get_active_slot();
    const esp_partition_t *part = iap_image_get_slot(slot);

    term_printf("\r\n===== 用户程序信息 (槽 %u) =====\r\n", (unsigned)slot);
    if (part) {
        term_printf("分区地址     : 0x%08" PRIx32 "\r\n", part->address);
        term_printf("分区大小     : %" PRIu32 " 字节\r\n", part->size);
    }
    if (!info.present) {
        term_puts("状态         : 无镜像 (首字节非 0xE9)\r\n");
        term_puts("========================\r\n\r\n");
        return;
    }
    term_printf("状态         : 存在镜像\r\n");
    term_printf("版本号       : 0x%06" PRIx32 "%s\r\n", info.app_version,
                info.app_version ? "" : " (未上报)");
    if (info.image_size) {
        term_printf("已记录长度   : %" PRIu32 " 字节\r\n", info.image_size);
        term_printf("已记录 CRC32 : 0x%08" PRIx32 "\r\n", info.image_crc32);
    }
    /*
     * 连续启动失败计数: 达到上限时 IAP 会停止自动启动 (防砖)。
     * 用户重新烧录正确的用户程序后可用 app clearfail 清零。
     */
    uint16_t fails = iap_config_get_boot_fail();
    if (fails > 0) {
        term_printf("连续启动失败 : %u 次%s\r\n", (unsigned)fails,
                    fails > IAP_BOOT_MAX_RETRY
                        ? " (已超限，自动启动已停止；app clearfail 可清零)"
                        : "");
    }
    term_puts("说明         : 镜像完整性由 bootloader 启动时校验\r\n");
    term_puts("========================\r\n\r\n");
}

static void cmd_app_erase(void)
{
    uint8_t slot = iap_image_get_active_slot();
    term_printf("正在擦除 OTA 槽 %u...\r\n", (unsigned)slot);
    esp_err_t err = iap_image_slot_erase(slot, 0);
    if (err == ESP_OK) {
        term_puts("擦除完成\r\n");
    } else {
        term_printf("擦除失败: %s\r\n", esp_err_to_name(err));
    }
}

/**
 * @brief 清零连续启动失败计数 (app clearfail)
 *
 * 用途:
 *   用户程序反复启动失败时 IAP 会停止自动启动并停在下载模式
 *   (防止无限重启)。重新烧录正确的用户程序后，可用本命令
 *   清除计数，让设备恢复自动启动。
 */
static void cmd_app_clearfail(void)
{
    uint16_t before = iap_config_get_boot_fail();
    esp_err_t err = iap_config_clear_boot_fail();
    if (err != ESP_OK) {
        term_printf("清零失败: %s\r\n", esp_err_to_name(err));
        return;
    }
    term_printf("连续启动失败计数已清零 (原值 %u)\r\n", (unsigned)before);
}

static void cmd_app_boot(void)
{
    term_puts("校验并启动用户程序...\r\n");
    esp_err_t err = iap_image_boot_user_app();
    term_printf("启动失败: %s\r\n", esp_err_to_name(err));
}

/**
 * @brief 列出所有 OTA 槽 (app list)
 */
static void cmd_app_list(void)
{
    uint8_t count = iap_image_get_slot_count();
    uint8_t active = iap_image_get_active_slot();

    term_printf("\r\n===== OTA 槽列表 (共 %u) =====\r\n", (unsigned)count);
    term_puts("  槽  地址        大小      状态      说明\r\n");
    term_puts("  ------------------------------------------------\r\n");

    for (uint8_t i = 0; i < count; i++) {
        const esp_partition_t *p = iap_image_get_slot(i);
        if (p == NULL) {
            term_printf("  %-3u %-10s %-9s %-9s %s\r\n",
                        (unsigned)i, "-", "-", "缺失", "");
            continue;
        }

        bool present = iap_image_slot_present(i);
        const char *mark = (i == active) ? "<- 活动" : "";

        term_printf("  %-3u 0x%08" PRIx32 "  %-9" PRIu32 " %-9s %s\r\n",
                    (unsigned)i, p->address, p->size / 1024,
                    present ? "有镜像" : "空", mark);
    }

    term_puts("  ------------------------------------------------\r\n");
    term_printf("  切换: app boot <槽号>   擦除: app erase <槽号>\r\n");
    term_puts("==============================\r\n\r\n");
}

/**
 * @brief 启动指定的 OTA 槽 (app boot <n>)
 *
 * 槽不存在或无镜像时不报错退出，而是重启并回落到 IAP 下载模式，
 * 便于现场快速恢复。
 */
static void cmd_app_boot_n(uint32_t slot)
{
    uint8_t count = iap_image_get_slot_count();

    if (slot >= count) {
        term_printf("槽号超出范围 (0 ~ %u)\r\n", (unsigned)(count - 1));
        return;
    }
    if (iap_image_get_slot((uint8_t)slot) == NULL) {
        term_printf("槽 %u 不存在，将回落到 IAP\r\n", (unsigned)slot);
    } else if (!iap_image_slot_present((uint8_t)slot)) {
        term_printf("槽 %u 无镜像，将回落到 IAP\r\n", (unsigned)slot);
    }

    term_printf("切换活动槽为 %u 并重启...\r\n", (unsigned)slot);
    iap_image_set_active_slot((uint8_t)slot);
    esp_err_t err = iap_image_boot_slot((uint8_t)slot);
    term_printf("启动失败: %s\r\n", esp_err_to_name(err));
}

/**
 * @brief 擦除指定的 OTA 槽 (app erase <n>)
 */
static void cmd_app_erase_n(uint32_t slot)
{
    uint8_t count = iap_image_get_slot_count();

    if (slot >= count) {
        term_printf("槽号超出范围 (0 ~ %u)\r\n", (unsigned)(count - 1));
        return;
    }
    if (iap_image_get_slot((uint8_t)slot) == NULL) {
        term_printf("槽 %u 不存在\r\n", (unsigned)slot);
        return;
    }

    term_printf("正在擦除 OTA 槽 %u...\r\n", (unsigned)slot);
    esp_err_t err = iap_image_slot_erase((uint8_t)slot, 0);
    if (err == ESP_OK) {
        term_puts("擦除完成\r\n");
    } else {
        term_printf("擦除失败: %s\r\n", esp_err_to_name(err));
    }
}

/**
 * @brief 设置 GPIO 选择 OTA 槽 (app gpio <n|off>)
 */
static void cmd_app_gpio(int argc, char **argv)
{
    if (argc < 3) {
        uint8_t g = iap_config_get_ota_gpio();
        if (g == IAP_OTA_GPIO_DISABLED) {
            term_puts("OTA 选择引脚: 未配置\r\n");
        } else {
            term_printf("OTA 选择引脚: GPIO%u\r\n", (unsigned)g);
        }
        term_puts("用法: app gpio <引脚号|off>\r\n");
        return;
    }

    uint8_t gpio;
    if (strcmp(argv[2], "off") == 0 || strcmp(argv[2], "none") == 0) {
        gpio = IAP_OTA_GPIO_DISABLED;
    } else {
        int v = (int)strtol(argv[2], NULL, 0);
        if (v < 0 || v > 48) {
            term_puts("引脚号无效\r\n");
            return;
        }
        gpio = (uint8_t)v;
    }

    if (iap_config_set_ota_gpio(gpio) == ESP_OK) {
        if (gpio == IAP_OTA_GPIO_DISABLED) {
            term_puts("已禁用 GPIO 选择 OTA 槽\r\n");
        } else {
            term_printf("已设置 GPIO%u 为 OTA 槽选择引脚 (重启后生效)\r\n",
                        (unsigned)gpio);
        }
    } else {
        term_puts("设置失败\r\n");
    }
}

/* -------------------------------------------------------------------------- */
/* 命令实现: XMODEM                                                            */
/* -------------------------------------------------------------------------- */

/** XMODEM 写回调: 写入用户程序区 */
typedef struct {
    uint32_t offset;        /*!< 起始偏移 */
    uint32_t written;       /*!< 已写入字节数 */
    iap_crc32_ctx_t crc;    /*!< 增量 CRC (仅用于显示) */
} xm_write_ctx_t;

/**
 * @brief XMODEM 数据回调
 *
 * 直接写入分区 —— 传入的就是**纯 ESP 应用镜像**（无自定义文件头）。
 * 镜像完整性由 bootloader 在启动时校验。
 */
static esp_err_t xm_on_data(void *user, const uint8_t *data, size_t len, uint32_t offset)
{
    xm_write_ctx_t *ctx = (xm_write_ctx_t *)user;
    const esp_partition_t *part = iap_image_get_user_partition();
    if (part == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t img_off = ctx->offset + ctx->written;

    /* 边界检查 */
    if (img_off + len > part->size) {
        term_puts("\r\n错误: 镜像超出分区容量\r\n");
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = iap_image_write(img_off, data, len);
    if (err != ESP_OK) {
        return err;
    }

    iap_crc32_ctx_update(&ctx->crc, data, len);
    ctx->written += len;
    return ESP_OK;
}

/**
 * @brief XMODEM 数据回调 (v6: 可变区整段写)
 *
 * 与 xm_on_data 的区别: 数据写到**可变区绝对地址** (从 0x140000 起)，
 * 而非 user_app 分区内偏移。用于接收含分区表 B 的完整镜像文件。
 */
static esp_err_t xm_on_data_var(void *user, const uint8_t *data, size_t len,
                                uint32_t offset)
{
    xm_write_ctx_t *ctx = (xm_write_ctx_t *)user;

    /* 绝对地址 = 可变区基址 + 已写字节数 */
    uint32_t addr = IAP_FIXED_REGION_END + ctx->written;

    esp_err_t err = esp_flash_write(NULL, data, addr, len);
    if (err != ESP_OK) {
        return err;
    }

    iap_crc32_ctx_update(&ctx->crc, data, len);
    ctx->written += len;
    return ESP_OK;
}

/** XMODEM 读回调: 从用户程序区读取 */
typedef struct {
    uint32_t offset;        /*!< 起始偏移 */
} xm_read_ctx_t;

static esp_err_t xm_on_read(void *user, uint8_t *buf, uint32_t offset, size_t len)
{
    xm_read_ctx_t *ctx = (xm_read_ctx_t *)user;
    return iap_image_read(ctx->offset + offset, buf, len);
}

/** XMODEM 读回调: 直接读 flash 绝对地址 (可变区整段发送) */
typedef struct {
    uint32_t base;          /*!< 起始绝对地址 (如 0x140000) */
} xm_raw_read_ctx_t;

static esp_err_t xm_on_read_raw(void *user, uint8_t *buf, uint32_t offset, size_t len)
{
    xm_raw_read_ctx_t *ctx = (xm_raw_read_ctx_t *)user;
    return esp_flash_read(NULL, buf, ctx->base + offset, len);
}

/*
 * XMODEM 通道选择
 *
 * ⚠️ 必须使用**执行本命令的终端后端**，不能硬编码 UART0。
 *
 * 设备可能同时挂载 UART 与 USB 两个终端后端 (见 term_task)，
 * 用户从哪个通道敲的 `xmodem recv`，数据就必须走哪个通道。
 * 若固定用 UART0，而终端实际在 USB 上，握手字符会发到无人接线的
 * UART0，发送方永远等不到 'C' —— 表现为「XMODEM 完全无效」。
 */

/** 数据阶段标志 (由协议层切换, 见 iap_uart_xmodem_set_raw_mode) */
static volatile bool s_xm_raw_mode = false;

/** 由 XMODEM 协议层调用: 标记当前是否处于二进制数据读取阶段 */
void iap_uart_xmodem_set_raw_mode(bool raw)
{
    s_xm_raw_mode = raw;
}

/** XMODEM 读回调: 从当前终端后端读取
 *
 * 返回值约定 (与 iap_xmodem_read_fn_t 一致):
 *   > 0  实际读取的字节数
 *   = 0  超时 (无数据)
 *   < 0  已取消 (检测到 Ctrl+C)
 *
 * ⚠️ 仅在**非数据阶段**扫描 0x03: XMODEM 包体是任意二进制，
 *    固件镜像中 0x03 极为常见，数据阶段扫描会误判为取消。
 */
static int xm_uart_read(void *user, uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    (void)user;
    const iap_term_backend_t *be = term_active_backend();
    if (be == NULL || be->read == NULL) {
        return 0;
    }

    int n = be->read(buf, len, timeout_ms);
    if (n <= 0) {
        return 0;
    }

    /* 数据阶段: 原样返回, 绝不扫描 0x03 */
    if (s_xm_raw_mode) {
        return n;
    }

    /* 握手/控制字符阶段: 扫描 Ctrl+C, 命中则立即返回负值 */
    for (int i = 0; i < n; i++) {
        if (buf[i] == 0x03) {
            s_cancel = true;
            return -1;      /* 取消优先，丢弃本批剩余数据 */
        }
    }
    return n;
}

/** XMODEM 写回调: 写入当前终端后端 */
static int xm_uart_write(void *user, const uint8_t *buf, size_t len)
{
    (void)user;
    const iap_term_backend_t *be = term_active_backend();
    if (be == NULL || be->write == NULL) {
        return 0;
    }
    int n = be->write(buf, len);
    return (n < 0) ? 0 : n;
}

/**
 * @brief XMODEM 接收并整段写入可变区 (v6)
 *
 * 流程:
 *   1. 擦除可变区 (0x140000 ~ flash 末尾)
 *   2. 接收文件，从 0x140000 起连续写入
 *   3. 校验: 偏移 0x10000 处应为 ESP 镜像 magic (0xE9)
 *   4. 更新配置区元数据
 *
 * 与 esptool 的 `write_flash 0x140000 file.bin` 语义一致。
 */
static void cmd_xmodem_recv_var(void)
{
    /* --- 1. 擦除可变区 --- */
    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK || flash_size == 0) {
        term_puts("读取 flash 容量失败\r\n");
        return;
    }
    uint32_t var_size = flash_size - IAP_FIXED_REGION_END;

    term_printf("擦除可变区 0x%06X ~ 0x%06X (%" PRIu32 " KB)...\r\n",
                IAP_FIXED_REGION_END, flash_size, var_size / 1024);
    if (esp_flash_erase_region(NULL, IAP_FIXED_REGION_END, var_size) != ESP_OK) {
        term_puts("擦除失败\r\n");
        return;
    }

    /* --- 2. 接收 --- */
    xm_write_ctx_t wctx = { .offset = 0, .written = 0 };
    iap_crc32_ctx_init(&wctx.crc);

    s_cancel = false;
    iap_xmodem_ctx_t xctx = {
        .read        = xm_uart_read,
        .write       = xm_uart_write,
        .user        = NULL,   /* 读写回调改用当前终端后端 */
        .cancel_flag = &s_cancel,
    };

    /*
     * 先等此前的输出排空, 再静默。
     *
     * ⚠️ 提示文本必须放在静默**之后**打印 —— 否则它会残留在 USB
     *    发送缓冲中, 与握手字符 'C' / ACK 混在同一字节流里,
     *    发送方逐字节解析时会读到这些文本字节而误判,
     *    导致「设备已收包但发送方等不到有效 ACK」。
     *    用户仍可从命令回显看到操作已开始。
     */
    iap_term_flush();
    iap_term_set_mute(true);
    term_puts("请使用 XMODEM 协议发送镜像文件 (Ctrl+C 取消)...\r\n");
    term_puts("提示: 文件应为 build_user_app.py 打包的 <name>_flash.bin\r\n");

    esp_err_t err = iap_xmodem_receive(&xctx, xm_on_data_var, &wctx);
    iap_term_set_mute(false);

    if (err != ESP_OK) {
        if (err == ESP_ERR_INVALID_STATE) {
            term_puts("\r\n已取消接收\r\n");
        } else if (err == ESP_ERR_TIMEOUT) {
            term_puts("\r\n接收超时 (对端无响应)，已取消\r\n");
        } else {
            term_printf("\r\n接收失败: %s\r\n", esp_err_to_name(err));
        }
        return;
    }

    uint32_t crc = iap_crc32_ctx_final(&wctx.crc);
    term_printf("\r\n接收完成: %" PRIu32 " 字节\r\n", wctx.written);
    term_printf("镜像 CRC32: 0x%08" PRIx32 "\r\n", crc);

    if (wctx.written == 0) {
        term_puts("警告: 未收到任何数据\r\n");
        return;
    }

    /* --- 3. 校验应用镜像位置 --- */
    const uint32_t app_off = IAP_USER_APP_ADDR - IAP_FIXED_REGION_END;   /* 0x10000 */

    if (wctx.written < app_off + 1) {
        term_printf("警告: 文件过短 (%" PRIu32 " < %" PRIu32 ")\r\n",
                    wctx.written, app_off + 1);
        term_puts("提示: 请发送从 0x140000 开始的完整镜像 (含分区表 B)\r\n");
        return;
    }

    uint8_t magic = 0;
    if (esp_flash_read(NULL, &magic, IAP_USER_APP_ADDR, 1) != ESP_OK ||
        magic != IAP_ESP_IMAGE_MAGIC) {
        term_printf("警告: 0x%06X 处首字节为 0x%02X，不是 ESP 镜像 magic (0xE9)\r\n",
                    IAP_USER_APP_ADDR, magic);
        term_puts("提示: 请用 build_user_app.py 打包 (会合并分区表 B)\r\n");
        return;
    }

    /* --- 4. 更新配置区元数据 --- */
    iap_cfg_data_t cfg;
    if (iap_config_get(&cfg) == ESP_OK) {
        cfg.user_app_size  = wctx.written - app_off;
        cfg.user_app_crc32 = crc;
        iap_config_set(&cfg);
    }

    term_printf("镜像校验通过 (应用镜像 %" PRIu32 " 字节 @ 0x%06X)\r\n",
                wctx.written - app_off, IAP_USER_APP_ADDR);
    term_puts("可用 app boot 启动\r\n");
    term_puts("(完整性由 bootloader 启动时校验)\r\n");
}

/**
 * @brief XMODEM 接收: 将文件写入用户程序区
 *
 * 用法: xmodem recv [offset]
 *
 * offset 语义:
 *   省略 / 0x140000  → 整段写入**可变区** (含分区表 B + 应用镜像)
 *   其它值           → 从该偏移写入用户程序分区 (旧流程)
 */
static void cmd_xmodem_recv(uint32_t offset)
{
    /* ---- v6+: offset == 可变区起点时走整段写 ---- */
    if (offset == 0 || offset == IAP_FIXED_REGION_END) {
        cmd_xmodem_recv_var();
        return;
    }

    const esp_partition_t *part = iap_image_get_user_partition();
    if (part == NULL) {
        term_puts("用户程序分区不可用\r\n");
        return;
    }

    if (offset >= part->size) {
        term_puts("偏移超出分区范围\r\n");
        return;
    }

    /* 擦除目标区域 */
    term_printf("擦除用户程序区 (从 0x%" PRIx32 ")...\r\n", offset);
    uint32_t erase_len = part->size - offset;
    uint32_t erase_aligned = (erase_len + part->erase_size - 1)
                           / part->erase_size * part->erase_size;
    if (erase_aligned > part->size - offset) {
        erase_aligned = part->size - offset;
    }
    if (esp_partition_erase_range(part, offset, erase_aligned) != ESP_OK) {
        term_puts("擦除失败\r\n");
        return;
    }

    xm_write_ctx_t wctx = {
        .offset  = offset,
        .written = 0,
    };
    iap_crc32_ctx_init(&wctx.crc);

    s_cancel = false;
    iap_xmodem_ctx_t xctx = {
        .read        = xm_uart_read,
        .write       = xm_uart_write,
        .user        = NULL,   /* 读写回调改用当前终端后端 */
        .cancel_flag = &s_cancel,
    };

    iap_term_flush();
    iap_term_set_mute(true);
    term_puts("请使用 XMODEM 协议发送文件 (Ctrl+C 取消)...\r\n");
    esp_err_t err = iap_xmodem_receive(&xctx, xm_on_data, &wctx);
    iap_term_set_mute(false);

    if (err != ESP_OK) {
        term_printf("\r\n接收失败: %s\r\n", esp_err_to_name(err));
        return;
    }

    uint32_t crc = iap_crc32_ctx_final(&wctx.crc);
    term_printf("\r\n接收完成: %" PRIu32 " 字节\r\n", wctx.written);
    term_printf("镜像 CRC32: 0x%08" PRIx32 "\r\n", crc);

    /* 若写在分区起始，记录元数据并检查镜像头 */
    if (offset == 0) {
        if (wctx.written == 0) {
            term_puts("警告: 未收到任何数据\r\n");
            return;
        }

        /* 检查首字节是否为 ESP 镜像 magic */
        uint8_t magic = 0;
        if (iap_image_read(0, &magic, 1) != ESP_OK || magic != IAP_ESP_IMAGE_MAGIC) {
            term_printf("警告: 首字节为 0x%02X，不是 ESP 镜像 magic (0xE9)\r\n", magic);
            term_puts("提示: 请用 build_user_app.py 打包（会合并分区表 B）\r\n");
            return;
        }

        /* 更新配置区元数据 (版本号由用户程序启动后自行上报) */
        iap_cfg_data_t cfg;
        if (iap_config_get(&cfg) == ESP_OK) {
            cfg.user_app_size  = wctx.written;
            cfg.user_app_crc32 = crc;
            iap_config_set(&cfg);
        }

        term_puts("镜像头检查通过，可用 app boot 启动\r\n");
        term_puts("(完整性由 bootloader 启动时校验)\r\n");
    }
}

/**
 * @brief XMODEM 发送: 从 flash 读出并发送
 *
 * 用法: xmodem send [offset] [length]
 *
 * offset 语义:
 *   省略 / 0x140000  → 从**可变区起点**发到 **flash 末尾**
 *                      (与 `xmodem recv` 的默认行为对称)
 *   其它值           → 从用户程序分区的该偏移发送 (旧流程)
 */
static void cmd_xmodem_send(uint32_t offset, uint32_t length)
{
    /* ---- 默认/0x140000: 整段发送可变区 (0x140000 ~ flash 末尾) ---- */
    if (offset == 0 || offset == IAP_FIXED_REGION_END) {
        uint32_t flash_size = 0;
        if (esp_flash_get_size(NULL, &flash_size) != ESP_OK || flash_size == 0) {
            term_puts("读取 flash 容量失败\r\n");
            return;
        }
        if (flash_size <= IAP_FIXED_REGION_END) {
            term_puts("flash 容量异常 (小于可变区起点)\r\n");
            return;
        }

        uint32_t var_size = flash_size - IAP_FIXED_REGION_END;
        if (length == 0 || length > var_size) {
            length = var_size;
        }

        xm_raw_read_ctx_t rctx = { .base = IAP_FIXED_REGION_END };

        s_cancel = false;
        iap_xmodem_ctx_t xctx = {
            .read        = xm_uart_read,
            .write       = xm_uart_write,
            .user        = NULL,   /* 读写回调改用当前终端后端 */
            .cancel_flag = &s_cancel,
        };

        term_printf("发送可变区 0x%06X ~ 0x%06X (%" PRIu32 " KB)...\r\n",
                    IAP_FIXED_REGION_END, IAP_FIXED_REGION_END + length,
                    length / 1024);
        iap_term_flush();
        iap_term_set_mute(true);
        term_puts("等待接收方握手 (Ctrl+C 取消)...\r\n");

        esp_err_t err = iap_xmodem_send(&xctx, "var_region.bin", length,
                                        xm_on_read_raw, &rctx);
        iap_term_set_mute(false);

        if (err == ESP_OK) {
            term_puts("\r\n发送完成\r\n");
        } else if (err == ESP_ERR_INVALID_STATE) {
            term_puts("\r\n已取消发送\r\n");
        } else if (err == ESP_ERR_TIMEOUT) {
            term_puts("\r\n发送超时 (对端无响应)，已取消\r\n");
        } else {
            term_printf("\r\n发送失败: %s\r\n", esp_err_to_name(err));
        }
        return;
    }

    /* ---- 旧流程: 从用户程序分区的指定偏移发送 ---- */
    const esp_partition_t *part = iap_image_get_user_partition();
    if (part == NULL) {
        term_puts("用户程序分区不可用\r\n");
        return;
    }

    if (offset >= part->size) {
        term_puts("偏移超出分区范围\r\n");
        return;
    }
    if (length == 0 || offset + length > part->size) {
        length = part->size - offset;
    }

    xm_read_ctx_t rctx = { .offset = offset };

    s_cancel = false;
    iap_xmodem_ctx_t xctx = {
        .read        = xm_uart_read,
        .write       = xm_uart_write,
        .user        = NULL,   /* 读写回调改用当前终端后端 */
        .cancel_flag = &s_cancel,
    };

    term_printf("开始发送 %" PRIu32 " 字节 (从 0x%" PRIx32 ")...\r\n", length, offset);

    iap_term_flush();
    iap_term_set_mute(true);
    term_puts("等待接收方握手 (Ctrl+C 取消)...\r\n");
    esp_err_t err = iap_xmodem_send(&xctx, "user_app.bin", length, xm_on_read, &rctx);
    iap_term_set_mute(false);

    if (err == ESP_OK) {
        term_puts("\r\n发送完成\r\n");
    } else if (err == ESP_ERR_INVALID_STATE) {
        term_puts("\r\n已取消发送\r\n");
    } else if (err == ESP_ERR_TIMEOUT) {
        term_puts("\r\n发送超时 (对端无响应)，已取消\r\n");
    } else {
        term_printf("\r\n发送失败: %s\r\n", esp_err_to_name(err));
    }
}

/* -------------------------------------------------------------------------- */
/* 命令解析                                                                    */
/* -------------------------------------------------------------------------- */

/*
 * v5: marker 命令已删除 —— flash 魔术标记 (iap_mark) 已取消。
 *
 * 「强制进入 IAP」的替代方案 (详见 docs/FINAL-REPORT.md §3.3):
 *   A. GPIO0 电平检测  —— 上电时按住 BOOT 键
 *   B. UART 命令       —— 等待窗口内发 "iap" 字符串
 *   C. 用户程序主动请求 —— 写 RTC RAM boot_target=0 → esp_restart()
 *   D. 断电重上电      —— 冷启动 RTC RAM 无效 → 默认进 IAP
 */

/**
 * bootloader [check]
 *
 * 检查 flash 0x0 处的 bootloader 与本固件的字段是否一致。
 *
 * 比对字段: magic / chip_id / flash 模式 / flash 容量。
 * 不比对项:
 *   - spi_speed: bootloader 该字段常为 0，运行时才按配置设置
 *   - 编译时间: 每次构建都不同，不影响功能
 *
 * 注意: 本检查**仅作排查参考**，字段不一致不会导致启动失败。
 */
static void cmd_bootloader(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    iap_bootloader_info_t bl;
    esp_err_t err = iap_image_check_bootloader(&bl);

    term_puts("\r\n===== bootloader 检查 =====\r\n");

    if (err != ESP_OK) {
        term_printf("检查失败: %s\r\n", esp_err_to_name(err));
        term_printf("  %s\r\n", bl.detail);
        term_puts("=============================\r\n\r\n");
        return;
    }

    term_printf("位置       : 0x%08X\r\n", IAP_BOOTLOADER_OFFSET);
    term_printf("magic      : 0x%02X %s\r\n", bl.magic,
               bl.magic == IAP_IMAGE_MAGIC ? "(有效)" : "(无效)");

    if (!bl.present) {
        term_printf("状态       : 无有效 bootloader\r\n");
        term_printf("  %s\r\n", bl.detail);
        term_puts("=============================\r\n\r\n");
        return;
    }

    term_printf("段数量     : %u\r\n", (unsigned)bl.segment_count);
    term_printf("芯片 ID    : %u\r\n", (unsigned)bl.chip_id);
    term_printf("flash 模式 : %u (%s)\r\n", (unsigned)bl.spi_mode,
               CONFIG_ESPTOOLPY_FLASHMODE);
    term_printf("flash 频率 : %u\r\n", (unsigned)bl.spi_speed);
    term_printf("flash 容量 : %u (%s)\r\n", (unsigned)bl.spi_size,
               CONFIG_ESPTOOLPY_FLASHSIZE);
    term_puts("-----------------------------\r\n");

    if (bl.compatible) {
        term_puts("结果       : [OK] 与本固件一致\r\n");
    } else {
        term_puts("结果       : [!!] 与本固件字段不一致\r\n");
        term_printf("  %s\r\n", bl.detail);
        term_puts("提示       : 设备上的 bootloader 可能来自其它构建配置\r\n");
    }

    term_puts("=============================\r\n\r\n");
}

static void print_help(void)
{
    term_puts("\r\n===== ESP IAP 终端命令 =====\r\n");
    term_puts("help                          显示本帮助\r\n");
    term_puts("info                          显示系统信息\r\n");
    term_puts("part                          显示分区表与 flash 容量一致性\r\n");
    term_puts("cfg                           显示 IAP 配置\r\n");
    term_puts("cfg set <key> <value>         修改配置项\r\n");
    term_puts("cfg reset                     恢复默认配置\r\n");
    term_puts("cfg migrate                   手动触发配置版本迁移\r\n");
    term_puts("bootparam                     显示启动参数\r\n");
    term_puts("bootparam set <string>        设置启动参数\r\n");
    term_puts("bootparam clear               清除启动参数\r\n");
    term_puts("bootparam get <key>           提取启动参数中指定键的值\r\n");
    term_puts("app info                      显示用户程序信息 (当前活动槽)\r\n");
    term_puts("app list                      列出所有 OTA 槽\r\n");
    term_puts("app erase [n]                 擦除用户程序区 (指定槽或当前槽)\r\n");
    term_puts("app clearfail                 清零连续启动失败计数 (防砖解锁)\r\n");
    term_puts("app boot [n]                  重启进入用户程序 (指定槽或当前槽)\r\n");
    term_puts("app gpio <引脚号|off>         设置 GPIO 选择 OTA 槽\r\n");
    term_puts("iap boot                      重启进入 IAP\r\n");
    term_puts("iap download on|off           设置下载模式标志\r\n");
    term_puts("bootloader                    检查 bootloader 与本固件的字段一致性\r\n");
    term_puts("xmodem recv [offset]          XMODEM 接收镜像并写入可变区 (默认 0x140000, Ctrl+C 取消)\r\n");
    term_puts("xmodem send [offset] [len]    XMODEM 发送可变区内容 (默认 0x140000 ~ flash 末尾, Ctrl+C 取消)\r\n");
    term_puts("reboot                        重启设备\r\n");
    term_puts("============================\r\n\r\n");
}

/**
 * @brief 将命令行按空格切分为参数
 *
 * @param line  输入行 (会被修改)
 * @param argv  输出参数数组
 * @param max   最大参数个数
 * @return 参数个数
 */
static int tokenize(char *line, char **argv, int max)
{
    int argc = 0;
    char *p = line;

    while (*p && argc < max) {
        while (*p == ' ' || *p == '\t') {
            *p++ = '\0';
        }
        if (*p == '\0') {
            break;
        }
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') {
            p++;
        }
    }
    return argc;
}

/**
 * @brief 执行一行命令
 */
static void exec_line(char *line)
{
    char *argv[8];
    int argc = tokenize(line, argv, 8);

    if (argc == 0) {
        return;
    }

    const char *cmd = argv[0];

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        print_help();
    } else if (strcmp(cmd, "info") == 0) {
        cmd_info();
    } else if (strcmp(cmd, "part") == 0) {
        cmd_part();
    } else if (strcmp(cmd, "cfg") == 0) {
        if (argc == 1) {
            cmd_cfg_show();
        } else if (strcmp(argv[1], "set") == 0 && argc >= 4) {
            cmd_cfg_set(argv[2], argv[3]);
        } else if (strcmp(argv[1], "reset") == 0) {
            if (iap_config_reset() == ESP_OK) {
                term_puts("已恢复默认配置\r\n");
            } else {
                term_puts("恢复失败\r\n");
            }
        } else if (strcmp(argv[1], "migrate") == 0) {
            cmd_cfg_migrate();
        } else {
            term_puts("用法: cfg [set <key> <value> | reset | migrate]\r\n");
        }
    } else if (strcmp(cmd, "bootparam") == 0) {
        cmd_bootparam(argc, argv);
    } else if (strcmp(cmd, "app") == 0) {
        if (argc < 2) {
            term_puts("用法: app [info|list|erase [n]|boot [n]|gpio <n|off>|clearfail]\r\n");
        } else if (strcmp(argv[1], "info") == 0) {
            cmd_app_info();
        } else if (strcmp(argv[1], "list") == 0) {
            cmd_app_list();
        } else if (strcmp(argv[1], "erase") == 0) {
            if (argc >= 3) {
                cmd_app_erase_n((uint32_t)strtoul(argv[2], NULL, 0));
            } else {
                cmd_app_erase();
            }
        } else if (strcmp(argv[1], "boot") == 0) {
            if (argc >= 3) {
                cmd_app_boot_n((uint32_t)strtoul(argv[2], NULL, 0));
            } else {
                cmd_app_boot();
            }
        } else if (strcmp(argv[1], "gpio") == 0) {
            cmd_app_gpio(argc, argv);
        } else if (strcmp(argv[1], "clearfail") == 0) {
            cmd_app_clearfail();
        } else {
            term_puts("未知子命令\r\n");
        }
    } else if (strcmp(cmd, "iap") == 0) {
        if (argc < 2) {
            term_puts("用法: iap [boot|download on|off]\r\n");
        } else if (strcmp(argv[1], "boot") == 0) {
            term_puts("重启进入 IAP...\r\n");
            iap_image_boot_iap();
        } else if (strcmp(argv[1], "download") == 0 && argc >= 3) {
            bool on = (strcmp(argv[2], "on") == 0 || strcmp(argv[2], "1") == 0);
            iap_config_set_download_mode(on);
            term_printf("下载模式: %s\r\n", on ? "开" : "关");
        } else {
            term_puts("未知子命令\r\n");
        }
    } else if (strcmp(cmd, "xmodem") == 0) {
        if (argc < 2) {
            term_puts("用法: xmodem [recv [offset] | send [offset] [len]]\r\n");
            term_puts("  recv 不带 offset 时默认 0x140000 (可变区整段写入)\r\n");
            term_puts("  send 不带 offset 时默认 0x140000 ~ flash 末尾\r\n");
        } else if (strcmp(argv[1], "recv") == 0) {
            /*
             * 不带 offset → 默认 IAP_FIXED_REGION_END (0x140000)，
             * 即"整段写入可变区"模式 (含分区表 B + 应用镜像)。
             */
            uint32_t offset = (argc >= 3)
                            ? (uint32_t)strtoul(argv[2], NULL, 0)
                            : (uint32_t)IAP_FIXED_REGION_END;
            cmd_xmodem_recv(offset);
        } else if (strcmp(argv[1], "send") == 0) {
            /*
             * 不带 offset → 默认 IAP_FIXED_REGION_END (0x140000)，
             * 即"整段发送可变区"模式 (0x140000 ~ flash 末尾)。
             */
            uint32_t offset = (argc >= 3)
                            ? (uint32_t)strtoul(argv[2], NULL, 0)
                            : (uint32_t)IAP_FIXED_REGION_END;
            uint32_t len    = (argc >= 4) ? (uint32_t)strtoul(argv[3], NULL, 0) : 0;
            cmd_xmodem_send(offset, len);
        } else {
            term_puts("未知子命令\r\n");
        }
    } else if (strcmp(cmd, "reboot") == 0) {
        term_puts("重启中...\r\n");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    } else if (strcmp(cmd, "bootloader") == 0) {
        cmd_bootloader(argc, argv);
    } else {
        term_printf("未知命令: %s (输入 help 查看帮助)\r\n", cmd);
    }
}

/* -------------------------------------------------------------------------- */
/* 终端任务                                                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief 终端读取任务 (每个后端一个实例)
 *
 * 逐字节读取，处理回车/退格/删除，遇到回车执行命令。
 * 行缓冲按后端隔离 (通过 task 局部变量)，避免两个终端互相干扰。
 */
static void term_task(void *arg)
{
    const iap_term_backend_t *be = (const iap_term_backend_t *)arg;
    char line[IAP_TERM_LINE_MAX];
    size_t len = 0;
    uint8_t ch;

    iap_term_write("\r\n");
    iap_term_write("========================================\r\n");
    iap_term_printf(" ESP IAP v%s 终端就绪 (%s)\r\n",
                    IAP_VERSION_STRING, be ? be->name : "?");
    iap_term_write(" 输入 help 查看命令列表\r\n");
    iap_term_write("========================================\r\n");
    iap_term_write("iap> ");

    while (1) {
        int n = be->read(&ch, 1, 100);
        if (n != 1) {
            continue;
        }

        /* Ctrl+C: 取消当前操作 */
        if (ch == 0x03) {
            s_cancel = true;
            iap_term_write("^C\r\niap> ");
            len = 0;
            continue;
        }

        /* 回车 */
        if (ch == '\r' || ch == '\n') {
            iap_term_write("\r\n");
            if (len > 0) {
                line[len] = '\0';
                /* 记录命令来自哪个后端: XMODEM 等独占操作需沿用同一通道 */
                s_active_backend = be;
                exec_line(line);
                s_active_backend = NULL;
                len = 0;
            }
            iap_term_write("iap> ");
            continue;
        }

        /* 退格 */
        if (ch == 0x08 || ch == 0x7F) {
            if (len > 0) {
                len--;
                iap_term_write("\b \b");
            }
            continue;
        }

        /* 可打印字符 */
        if (ch >= 0x20 && ch < 0x7F) {
            if (len < sizeof(line) - 1) {
                line[len++] = (char)ch;
                be->write(&ch, 1);
            }
            continue;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* UART 后端                                                                   */
/* -------------------------------------------------------------------------- */

static int uart_backend_read(uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    return uart_read_bytes(s_uart_num, buf, len, pdMS_TO_TICKS(timeout_ms));
}

static int uart_backend_write(const uint8_t *buf, size_t len)
{
    return uart_write_bytes(s_uart_num, (const char *)buf, len);
}

static bool uart_backend_ready(void)
{
    return s_uart_num >= 0;
}

static const iap_term_backend_t s_uart_backend = {
    .name  = "UART",
    .read  = uart_backend_read,
    .write = uart_backend_write,
    .ready = uart_backend_ready,
};

/* -------------------------------------------------------------------------- */
/* 初始化                                                                      */
/* -------------------------------------------------------------------------- */

esp_err_t iap_uart_start(void)
{
    iap_cfg_data_t cfg;
    esp_err_t err = iap_config_get(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    if (!(cfg.flags & IAP_CFG_FLAG_UART_ENABLE)) {
        ESP_LOGI(TAG, "UART 终端未使能，跳过初始化");
        return ESP_OK;
    }

    /* 端口与引脚均来自配置区（默认 UART0 + 芯片默认引脚） */
    s_uart_num = (uart_port_t)cfg.uart_port;

    /*
     * 尝试安装驱动。若已安装 (等待阶段或控制台已安装)，
     * 驱动会打印 "UART driver already installed" 并返回失败，
     * 这属于正常情况，直接复用已有驱动。
     *
     * 注意: tx_buffer_size 必须非 0，否则 uart_write_bytes() 会因
     * 缺少 TX 互斥量而断言失败。
     */
    esp_err_t drv_err = uart_driver_install(s_uart_num, 2048, 2048, 0, NULL, 0);
    if (drv_err == ESP_OK) {
        ESP_LOGI(TAG, "UART 驱动已安装");
    } else {
        size_t buffered = 0;
        if (uart_get_buffered_data_len(s_uart_num, &buffered) == ESP_OK) {
            ESP_LOGI(TAG, "UART 驱动已存在，复用现有驱动");
        } else {
            ESP_LOGW(TAG, "UART 驱动不可用: %s", esp_err_to_name(drv_err));
        }
    }

    uart_config_t uart_cfg = {
        .baud_rate  = (int)cfg.uart_baudrate,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(s_uart_num, &uart_cfg);

    /*
     * 引脚: 0xFF (IAP_CFG_UART_PIN_DEFAULT) 映射为 UART_PIN_NO_CHANGE，
     * 即保持该端口的芯片默认映射；否则复用为配置的 GPIO。
     */
    int tx_pin = (cfg.uart_tx_gpio == IAP_CFG_UART_PIN_DEFAULT)
                 ? UART_PIN_NO_CHANGE : (int)cfg.uart_tx_gpio;
    int rx_pin = (cfg.uart_rx_gpio == IAP_CFG_UART_PIN_DEFAULT)
                 ? UART_PIN_NO_CHANGE : (int)cfg.uart_rx_gpio;

    esp_err_t pin_err = uart_set_pin(s_uart_num, tx_pin, rx_pin,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (pin_err != ESP_OK) {
        ESP_LOGE(TAG, "设置 UART 引脚失败 (TX=%d, RX=%d): %s",
                 tx_pin, rx_pin, esp_err_to_name(pin_err));
        return pin_err;
    }

    /*
     * 记录实际生效的引脚。
     * IDF v6 没有 uart_get_pin()，因此:
     *   - 配置了具体引脚时，直接用配置值
     *   - 用默认引脚时，按芯片的 UART0 默认映射填入（仅 UART0 有默认映射）
     */
    s_tx_pin = (tx_pin == UART_PIN_NO_CHANGE) ? uart_default_tx_pin(s_uart_num) : tx_pin;
    s_rx_pin = (rx_pin == UART_PIN_NO_CHANGE) ? uart_default_rx_pin(s_uart_num) : rx_pin;

    if (s_tx_pin < 0 || s_rx_pin < 0) {
        ESP_LOGW(TAG, "UART%u 未绑定有效引脚 (TX=%d, RX=%d)，终端将无法收发。"
                      "请在配置区指定 uart_tx_gpio / uart_rx_gpio",
                 (unsigned)s_uart_num, s_tx_pin, s_rx_pin);
    }

    /* 挂载为终端后端 (内部创建独立读取任务) */
    err = iap_term_attach(&s_uart_backend, "iap_term_uart");
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "UART 终端已启动: UART%u, TX=%s, RX=%s, 波特率 %" PRIu32,
             (unsigned)s_uart_num,
             s_tx_pin < 0 ? "(未绑定)" : "GPIO",
             s_rx_pin < 0 ? "(未绑定)" : "GPIO",
             cfg.uart_baudrate);
    if (s_tx_pin >= 0) {
        ESP_LOGI(TAG, "  实际引脚: TX=GPIO%d, RX=GPIO%d", s_tx_pin, s_rx_pin);
    }
    return ESP_OK;
}
