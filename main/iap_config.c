/*
 * ESP IAP - 配置区管理
 *
 * 配置区位于独立分区 iap_cfg，采用双槽冗余 + 序号 + CRC 的方式存储，
 * 保证掉电安全。IAP 程序与用户程序均可读写该分区。
 *
 * 存储布局 (单个槽):
 *   offset 0                    : iap_cfg_header_t
 *   offset sizeof(header)       : iap_cfg_data_t
 *   其余                        : 0xFF (擦除态)
 *
 * 槽 A 位于 offset 0，槽 B 位于 offset IAP_CFG_SLOT_SIZE。
 */

#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_mac.h"
#include "spi_flash_mmap.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "iap_common.h"
#include "iap_crc.h"
#include "iap_config.h"

#include "esp_flash.h"
#include "esp_partition.h"   /* SPI_FLASH_SEC_SIZE */

static const char *TAG = "iap_cfg";

/*
 * 配置区采用**硬编码地址直访** (v5), 不依赖分区表。
 *
 * 原因: iap_cfg 不在任何分区表中 (IAP 与 APP 约定同一地址 0x008000),
 *       因此不能用 esp_partition_find_first() 查找。
 *
 * 地址与大小:
 *   基址 IAP_CFG_ADDR (0x008000), 共 IAP_CFG_SLOT_COUNT 个槽,
 *   每槽 IAP_CFG_SLOT_SIZE (4096) 字节。
 *
 * 详见 docs/FINAL-REPORT.md §2.1
 */

/** 配置区读写互斥锁 */
static SemaphoreHandle_t s_lock = NULL;

/** 当前生效的配置 (内存缓存) */
static iap_cfg_data_t s_cfg;

/** 当前生效配置所在的槽序号 (0 或 1) */
static int s_active_slot = -1;

/** 当前生效配置的写入序号 */
static uint32_t s_active_seq = 0;

/** 配置区是否已就绪 (init 成功后置位) */
static bool s_ready = false;

/* -------------------------------------------------------------------------- */
/* 内部辅助 —— 硬编码 flash 直访                                                */
/* -------------------------------------------------------------------------- */

/**
 * @brief 从配置区读取数据 (硬编码地址)
 *
 * @param offset 相对配置区基址的偏移
 * @param buf    输出缓冲
 * @param len    读取长度
 * @return ESP_OK 成功
 */
static esp_err_t cfg_flash_read(size_t offset, void *buf, size_t len)
{
    return esp_flash_read(NULL, buf, IAP_CFG_ADDR + offset, len);
}

/**
 * @brief 向配置区写入数据 (硬编码地址)
 */
static esp_err_t cfg_flash_write(size_t offset, const void *buf, size_t len)
{
    return esp_flash_write(NULL, buf, IAP_CFG_ADDR + offset, len);
}

/**
 * @brief 擦除配置区指定范围 (硬编码地址)
 *
 * 注意: esp_flash_erase_region() 要求地址与长度均按扇区对齐。
 */
static esp_err_t cfg_flash_erase(size_t offset, size_t len)
{
    return esp_flash_erase_region(NULL, IAP_CFG_ADDR + offset, len);
}

/**
 * @brief 将配置槽从 flash 读出并校验
 *
 * @param slot     槽序号 (0 或 1)
 * @param out_data 输出配置数据
 * @param out_seq  输出写入序号
 * @return ESP_OK 表示该槽有效
 */
static esp_err_t read_slot(int slot, iap_cfg_data_t *out_data, uint32_t *out_seq)
{
    uint8_t hdr_buf[sizeof(iap_cfg_header_t)];
    iap_cfg_header_t hdr;
    size_t slot_off = (size_t)slot * IAP_CFG_SLOT_SIZE;

    esp_err_t err = cfg_flash_read(slot_off, hdr_buf, sizeof(hdr_buf));
    if (err != ESP_OK) {
        return err;
    }
    memcpy(&hdr, hdr_buf, sizeof(hdr));

    /* 校验头部 */
    if (hdr.magic != IAP_CFG_MAGIC) {
        return ESP_ERR_INVALID_STATE;
    }
    if (hdr.version != IAP_CFG_VERSION) {
        ESP_LOGW(TAG, "槽 %d 版本不匹配: 0x%04X", slot, hdr.version);
        return ESP_ERR_INVALID_VERSION;
    }
    if (hdr.data_size != sizeof(iap_cfg_data_t)) {
        ESP_LOGW(TAG, "槽 %d 数据长度不匹配: %u", slot, (unsigned)hdr.data_size);
        return ESP_ERR_INVALID_SIZE;
    }

    /* 校验头部 CRC (不含 header_crc32 字段本身) */
    uint32_t hdr_crc_calc = iap_crc32(hdr_buf, offsetof(iap_cfg_header_t, header_crc32));
    if (hdr_crc_calc != hdr.header_crc32) {
        ESP_LOGW(TAG, "槽 %d 头部 CRC 错误", slot);
        return ESP_ERR_INVALID_CRC;
    }

    /* 读取数据区并校验 */
    static uint8_t data_buf[sizeof(iap_cfg_data_t)];
    err = cfg_flash_read(slot_off + sizeof(iap_cfg_header_t),
                         data_buf, sizeof(data_buf));
    if (err != ESP_OK) {
        return err;
    }

    uint32_t data_crc_calc = iap_crc32(data_buf, sizeof(data_buf));
    if (data_crc_calc != hdr.data_crc32) {
        ESP_LOGW(TAG, "槽 %d 数据 CRC 错误", slot);
        return ESP_ERR_INVALID_CRC;
    }

    memcpy(out_data, data_buf, sizeof(iap_cfg_data_t));
    if (out_seq) {
        *out_seq = hdr.seq;
    }
    return ESP_OK;
}

/**
 * @brief 将配置写入指定槽
 *
 * @param slot 槽序号 (0 或 1)
 * @param data 配置数据
 * @param seq  写入序号
 * @return ESP_OK 成功
 */
static esp_err_t write_slot(int slot, const iap_cfg_data_t *data, uint32_t seq)
{
    size_t slot_off = (size_t)slot * IAP_CFG_SLOT_SIZE;

    /*
     * 组装完整槽内容: 头部 + 数据 + 填充 0xFF。
     *
     * 缓冲区使用静态存储: 槽大小为一个 flash 扇区 (4096 字节)，
     * 若放在栈上会超出调用方任务栈 (终端任务仅 4096 字节) 导致溢出。
     * 本函数仅在持有 s_lock 时被调用，静态缓冲区不会并发访问。
     */
    static uint8_t buf[IAP_CFG_SLOT_SIZE];
    memset(buf, 0xFF, sizeof(buf));

    iap_cfg_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic       = IAP_CFG_MAGIC;
    hdr.version     = IAP_CFG_VERSION;
    hdr.header_size = sizeof(iap_cfg_header_t);
    hdr.seq         = seq;
    hdr.data_size   = sizeof(iap_cfg_data_t);
    hdr.data_crc32  = iap_crc32(data, sizeof(iap_cfg_data_t));
    hdr.header_crc32 = iap_crc32(&hdr, offsetof(iap_cfg_header_t, header_crc32));

    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), data, sizeof(iap_cfg_data_t));

    /*
     * 擦除并写入。
     *
     * 注意: esp_flash_erase_region() 要求擦除长度必须是
     * SPI_FLASH_SEC_SIZE (4096) 的整数倍，因此这里按扇区粒度擦除。
     * 由于 IAP_CFG_SLOT_SIZE 已对齐到扇区，两个槽各占一个扇区，
     * 擦除当前槽不会影响另一个槽。
     */
    size_t erase_len = (IAP_CFG_SLOT_SIZE + SPI_FLASH_SEC_SIZE - 1)
                     / SPI_FLASH_SEC_SIZE * SPI_FLASH_SEC_SIZE;

    esp_err_t err = cfg_flash_erase(slot_off, erase_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "擦除槽 %d 失败: %s", slot, esp_err_to_name(err));
        return err;
    }

    err = cfg_flash_write(slot_off, buf, sizeof(buf));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写入槽 %d 失败: %s", slot, esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

/**
 * @brief 用默认值填充配置
 */
static void fill_defaults(iap_cfg_data_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    cfg->flags = IAP_CFG_FLAG_WIFI_ENABLE |
                 IAP_CFG_FLAG_I2C_ENABLE |
                 IAP_CFG_FLAG_UART_ENABLE |
                 IAP_CFG_FLAG_VERIFY_USER_APP |
                 IAP_CFG_FLAG_WAIT_GPIO_TRIG |   /* GPIO0 (BOOT 键) 救援入口 */
                 IAP_CFG_FLAG_WAIT_UART_TRIG |
                 IAP_CFG_FLAG_WAIT_WIFI_TRIG |
                 IAP_CFG_DEFAULT_FLAGS_USB;   /* 支持 USB 的芯片默认开启 */

    cfg->wait_seconds     = IAP_CFG_DEFAULT_WAIT_SEC;
    cfg->user_app_size    = 0;
    cfg->user_app_crc32   = 0;
    cfg->user_app_version = 0;
    cfg->boot_count       = 0;
    cfg->last_boot_reason = IAP_BOOT_REASON_FIRST_BOOT;

    /* I2C */
    cfg->i2c_scl_gpio = IAP_CFG_DEFAULT_I2C_SCL;
    cfg->i2c_sda_gpio = IAP_CFG_DEFAULT_I2C_SDA;
    cfg->i2c_addr     = IAP_CFG_DEFAULT_I2C_ADDR;
    cfg->i2c_reserved_freq = 0;   /* 占位，保留布局 */

    /* WiFi AP */
    strncpy((char *)cfg->wifi_ssid, IAP_CFG_DEFAULT_WIFI_SSID,
            sizeof(cfg->wifi_ssid) - 1);
    strncpy((char *)cfg->wifi_password, IAP_CFG_DEFAULT_WIFI_PASS,
            sizeof(cfg->wifi_password) - 1);
    cfg->wifi_channel = IAP_CFG_DEFAULT_WIFI_CHANNEL;
    cfg->wifi_ip      = IAP_CFG_DEFAULT_IP_A |
                        (IAP_CFG_DEFAULT_IP_B << 8) |
                        (IAP_CFG_DEFAULT_IP_C << 16) |
                        ((uint32_t)IAP_CFG_DEFAULT_IP_D << 24);
    cfg->wifi_netmask = IAP_CFG_DEFAULT_NETMASK_A |
                        (IAP_CFG_DEFAULT_NETMASK_B << 8) |
                        (IAP_CFG_DEFAULT_NETMASK_C << 16) |
                        ((uint32_t)IAP_CFG_DEFAULT_NETMASK_D << 24);

    /* UART */
    cfg->uart_port     = IAP_CFG_DEFAULT_UART_PORT;
    cfg->uart_tx_gpio  = IAP_CFG_UART_PIN_DEFAULT;   /* 用芯片默认引脚 */
    cfg->uart_rx_gpio  = IAP_CFG_UART_PIN_DEFAULT;
    cfg->uart_baudrate = IAP_CFG_DEFAULT_UART_BAUD;

    /* 等待期间触发进入 IAP 的 GPIO 引脚: 默认 GPIO0 (BOOT 键, 救援入口) */
    cfg->trig_gpio       = IAP_CFG_DEFAULT_TRIG_GPIO;
    cfg->trig_gpio_level = IAP_CFG_DEFAULT_TRIG_LEVEL;

    /* USB: 无额外参数 (波特率由主机决定)，保留字段置 0 */
    cfg->usb_reserved0 = 0;

    /* 启动参数: 默认未设置 */
    memset(cfg->boot_param, 0, sizeof(cfg->boot_param));
    cfg->boot_param_len     = 0;
    cfg->boot_param_flags   = 0;
    cfg->boot_param_crc32   = 0;

    /* 多 OTA 槽: 默认启动槽 0，槽数 1 */
    cfg->active_slot    = 0;
    cfg->ota_slot_count = 1;
    cfg->ota_gpio       = IAP_OTA_GPIO_DISABLED;

    /*
     * 槽加载地址/大小 (v7): 默认只配置槽 0。
     *
     * 槽 0 默认指向标准的用户程序地址 (0x150000)，大小 = 到 flash 末尾。
     * 其余槽地址为 0 (无效)，用户可在「APP 启动槽」中手动添加。
     *
     * ⚠️ 这里只是**默认值**，实际以配置区为准。用户改了地址后，
     *    以配置区里的值为准，IAP 不再做任何推断。
     */
    cfg->slot_addr[0] = IAP_USER_APP_ADDR;              /* 0x150000 */
    cfg->slot_size[0] = 0x400000u - IAP_USER_APP_ADDR;  /* 到 4MB 末尾 */
    for (int i = 1; i < IAP_OTA_SLOT_MAX; i++) {
        cfg->slot_addr[i] = 0;
        cfg->slot_size[i] = 0;
    }

    /* 配置数据版本 */
    cfg->data_ver = IAP_CFG_DATA_VER;
}

/**
 * @brief 对配置做合法性修正
 *
 * 防止配置区被写坏后导致程序异常 (如 SSID 无终止符、GPIO 越界)。
 */
static void sanitize(iap_cfg_data_t *cfg)
{
    /* 保证字符串以 0 结尾 */
    cfg->wifi_ssid[sizeof(cfg->wifi_ssid) - 1] = '\0';
    cfg->wifi_password[sizeof(cfg->wifi_password) - 1] = '\0';

    /* SSID 为空则用默认值 */
    if (cfg->wifi_ssid[0] == '\0' || cfg->wifi_ssid[0] == 0xFF) {
        strncpy((char *)cfg->wifi_ssid, IAP_CFG_DEFAULT_WIFI_SSID,
                sizeof(cfg->wifi_ssid) - 1);
    }

    /* 密码长度检查: WPA2 要求 8~63 字符，否则退化为开放网络 */
    size_t pass_len = strnlen((const char *)cfg->wifi_password,
                              sizeof(cfg->wifi_password));
    if (pass_len != 0 && (pass_len < 8 || pass_len > 63)) {
        ESP_LOGW(TAG, "WiFi 密码长度非法 (%u)，AP 将不加密", (unsigned)pass_len);
        cfg->wifi_password[0] = '\0';
    }

    /* 信道范围 1~13 */
    if (cfg->wifi_channel < 1 || cfg->wifi_channel > 13) {
        cfg->wifi_channel = IAP_CFG_DEFAULT_WIFI_CHANNEL;
    }

    /* I2C 地址必须为有效 7bit 地址 */
    if (cfg->i2c_addr == 0 || cfg->i2c_addr > 0x7F) {
        cfg->i2c_addr = IAP_CFG_DEFAULT_I2C_ADDR;
    }

    /* 等待时间上限 (避免长时间无法启动用户程序) */
    if (cfg->wait_seconds > 300) {
        cfg->wait_seconds = 300;
    }

    /* IP 不能为 0 */
    if (cfg->wifi_ip == 0) {
        cfg->wifi_ip = IAP_CFG_DEFAULT_IP_A |
                       (IAP_CFG_DEFAULT_IP_B << 8) |
                       (IAP_CFG_DEFAULT_IP_C << 16) |
                       ((uint32_t)IAP_CFG_DEFAULT_IP_D << 24);
    }
    if (cfg->wifi_netmask == 0) {
        cfg->wifi_netmask = 0x00FFFFFFU;  /* 255.255.255.0 */
    }

    /* 波特率范围 */
    if (cfg->uart_baudrate < 9600 || cfg->uart_baudrate > 3000000) {
        cfg->uart_baudrate = IAP_CFG_DEFAULT_UART_BAUD;
    }

    /* UART 端口仅支持 0/1/2 */
    if (cfg->uart_port > 2) {
        cfg->uart_port = IAP_CFG_DEFAULT_UART_PORT;
    }

    /*
     * UART 引脚: 0xFF 表示"用默认引脚"，属合法值。
     * 其余值必须是有效 GPIO 号 (0~48)，否则回退为默认。
     */
    if (cfg->uart_tx_gpio != IAP_CFG_UART_PIN_DEFAULT &&
        cfg->uart_tx_gpio > 48) {
        cfg->uart_tx_gpio = IAP_CFG_UART_PIN_DEFAULT;
    }
    if (cfg->uart_rx_gpio != IAP_CFG_UART_PIN_DEFAULT &&
        cfg->uart_rx_gpio > 48) {
        cfg->uart_rx_gpio = IAP_CFG_UART_PIN_DEFAULT;
    }
    /* TX/RX 不能是同一引脚 (除非都是默认) */
    if (cfg->uart_tx_gpio == cfg->uart_rx_gpio &&
        cfg->uart_tx_gpio != IAP_CFG_UART_PIN_DEFAULT) {
        ESP_LOGW(TAG, "UART TX/RX 引脚相同 (GPIO%u)，回退为默认引脚",
                 cfg->uart_tx_gpio);
        cfg->uart_tx_gpio = IAP_CFG_UART_PIN_DEFAULT;
        cfg->uart_rx_gpio = IAP_CFG_UART_PIN_DEFAULT;
    }

    /* --- 触发 GPIO 引脚 --- */
    /* 有效电平只允许 0/1 */
    if (cfg->trig_gpio_level > IAP_CFG_GPIO_TRIG_ACTIVE_HIGH) {
        cfg->trig_gpio_level = IAP_CFG_GPIO_TRIG_ACTIVE_LOW;
    }
    /* 引脚号越界则视为未配置 (ESP32 系列 GPIO 最大 48) */
    if (cfg->trig_gpio != IAP_CFG_DEFAULT_TRIG_GPIO && cfg->trig_gpio > 48) {
        ESP_LOGW(TAG, "触发引脚号非法 (%u)，已禁用 GPIO 触发", cfg->trig_gpio);
        cfg->trig_gpio = IAP_CFG_DEFAULT_TRIG_GPIO;
    }
    /* 未配置引脚时清除使能标志，避免无效检测 */
    if (cfg->trig_gpio == IAP_CFG_DEFAULT_TRIG_GPIO) {
        cfg->flags &= ~IAP_CFG_FLAG_WAIT_GPIO_TRIG;
    }

    /* --- USB --- */
    /* 不支持的芯片: 强制清除 USB 相关标志 (配置可能来自其他芯片的镜像) */
    if (!IAP_USB_SUPPORTED) {
        if (cfg->flags & (IAP_CFG_FLAG_USB_ENABLE | IAP_CFG_FLAG_WAIT_USB_TRIG)) {
            ESP_LOGW(TAG, "该芯片不支持 USB，已清除 USB 相关标志");
            cfg->flags &= ~(IAP_CFG_FLAG_USB_ENABLE | IAP_CFG_FLAG_WAIT_USB_TRIG);
        }
    }

    /* --- 启动参数 --- */
    /* 保证以 '\0' 结尾 */
    cfg->boot_param[sizeof(cfg->boot_param) - 1] = '\0';

    /* 长度字段与实际内容对齐 */
    size_t param_len = strnlen((const char *)cfg->boot_param,
                               sizeof(cfg->boot_param));
    if (param_len > IAP_CFG_BOOT_PARAM_MAX_LEN) {
        param_len = IAP_CFG_BOOT_PARAM_MAX_LEN;
        cfg->boot_param[param_len] = '\0';
    }
    cfg->boot_param_len = (uint16_t)param_len;

    /* 空串或长度字段不一致时，视为未设置 */
    if (param_len == 0) {
        cfg->boot_param_flags &= ~IAP_CFG_BOOT_PARAM_FLAG_VALID;
        cfg->boot_param_crc32 = 0;
    } else {
        /* 校验 CRC；不匹配则重新计算 (容忍字段被单独修改) */
        uint32_t crc = iap_crc32(cfg->boot_param, param_len);
        if (cfg->boot_param_crc32 != crc) {
            if (cfg->boot_param_flags & IAP_CFG_BOOT_PARAM_FLAG_VALID) {
                ESP_LOGW(TAG, "启动参数 CRC 不匹配，已重新计算");
            }
            cfg->boot_param_crc32 = crc;
        }
        cfg->boot_param_flags |= IAP_CFG_BOOT_PARAM_FLAG_VALID;
    }

    /* --- 多 OTA 槽 --- */
    /* 槽数至少 1，且不超上限 */
    if (cfg->ota_slot_count == 0 || cfg->ota_slot_count > IAP_OTA_SLOT_MAX) {
        cfg->ota_slot_count = 1;
    }
    /* 活动槽必须在有效范围内 */
    if (cfg->active_slot >= cfg->ota_slot_count) {
        ESP_LOGW(TAG, "active_slot=%u 越界 (槽数=%u)，修正为 0",
                 (unsigned)cfg->active_slot, (unsigned)cfg->ota_slot_count);
        cfg->active_slot = 0;
    }
    /* GPIO 编号: 0xFF 表示禁用，其余须为有效引脚 */
    if (cfg->ota_gpio != IAP_OTA_GPIO_DISABLED && cfg->ota_gpio > 48) {
        ESP_LOGW(TAG, "OTA 选择引脚号非法 (%u)，已禁用", (unsigned)cfg->ota_gpio);
        cfg->ota_gpio = IAP_OTA_GPIO_DISABLED;
    }

    /* --- 槽加载地址 (v7) --- */
    for (int i = 0; i < IAP_OTA_SLOT_MAX; i++) {
        uint64_t a = cfg->slot_addr[i];

        /* 0 = 未配置 (合法); 非 0 必须落在可变区且不超 32 位 */
        if (a == 0) continue;

        if (a > 0xFFFFFFFFULL || a < IAP_FIXED_REGION_END) {
            ESP_LOGW(TAG, "槽 %d 地址 0x%08X%08X 非法 (须为 0 或 >= 0x%06X)，已清除",
                     i, (unsigned)(a >> 32), (unsigned)(a & 0xFFFFFFFFu),
                     (unsigned)IAP_FIXED_REGION_END);
            cfg->slot_addr[i] = 0;
            cfg->slot_size[i] = 0;
            continue;
        }

        /* 大小未配置时，用"到 flash 末尾"兜底 (4MB) */
        if (cfg->slot_size[i] == 0) {
            cfg->slot_size[i] = 0x400000ULL - a;
        }
    }

    /* --- 配置数据版本 --- */
    if (cfg->data_ver == 0) {
        cfg->data_ver = IAP_CFG_DATA_VER_LEGACY;
    }
}

/* -------------------------------------------------------------------------- */
/* 配置迁移                                                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief 把旧版配置迁移到当前版本
 *
 * 策略 (以默认配置为基准，逐项合并旧配置):
 *   1. 从默认配置出发 (保证新增字段有合理初值)
 *   2. 把旧配置中**仍然存在**的字段覆盖过来
 *   3. 已删除的字段自然被忽略 (默认值保留)
 *   4. 冲突/越界值由 sanitize() 修正
 *   5. 更新 data_ver 并写回
 *
 * @param old 旧版配置 (in/out: 迁移后原地更新)
 * @return true  发生了迁移 (调用方应写回 flash)
 *         false 无需迁移
 */
static bool migrate(iap_cfg_data_t *old)
{
    uint16_t from = old->data_ver;

    if (from == IAP_CFG_DATA_VER) {
        return false;   /* 已是最新 */
    }

    if (from > IAP_CFG_DATA_VER) {
        ESP_LOGW(TAG, "配置数据版本 %u 高于固件支持的 %u，"
                 "按当前版本处理 (可能丢失新字段)",
                 (unsigned)from, (unsigned)IAP_CFG_DATA_VER);
        return false;
    }

    ESP_LOGW(TAG, "========================================");
    ESP_LOGW(TAG, " 配置数据版本迁移: %u -> %u",
             (unsigned)from, (unsigned)IAP_CFG_DATA_VER);
    ESP_LOGW(TAG, "========================================");

    /*
     * 以默认配置为基准。
     *
     * 这样新增字段自动获得合理初值，已删除字段不会残留旧数据。
     */
    iap_cfg_data_t merged;
    fill_defaults(&merged);

    /* ---- 逐项合并旧配置中仍然存在的字段 ---- */

    /* 标志位: 保留旧值，但清除已废弃的位 (由 sanitize 处理) */
    merged.flags        = old->flags;
    merged.wait_seconds = old->wait_seconds;

    /* 用户程序元数据 (保留，避免丢失版本号记录) */
    merged.user_app_size    = old->user_app_size;
    merged.user_app_crc32   = old->user_app_crc32;
    merged.user_app_version = old->user_app_version;
    merged.boot_count       = old->boot_count;
    merged.last_boot_reason = old->last_boot_reason;

    /* I2C */
    merged.i2c_scl_gpio = old->i2c_scl_gpio;
    merged.i2c_sda_gpio = old->i2c_sda_gpio;
    merged.i2c_addr     = old->i2c_addr;

    /* WiFi */
    memcpy(merged.wifi_ssid, old->wifi_ssid, sizeof(merged.wifi_ssid));
    memcpy(merged.wifi_password, old->wifi_password, sizeof(merged.wifi_password));
    merged.wifi_channel = old->wifi_channel;
    merged.wifi_ip      = old->wifi_ip;
    merged.wifi_netmask = old->wifi_netmask;

    /* UART */
    merged.uart_port     = old->uart_port;
    merged.uart_tx_gpio  = old->uart_tx_gpio;
    merged.uart_rx_gpio  = old->uart_rx_gpio;
    merged.uart_baudrate = old->uart_baudrate;

    /* 触发引脚 */
    merged.trig_gpio       = old->trig_gpio;
    merged.trig_gpio_level = old->trig_gpio_level;

    /* 启动参数 */
    memcpy(merged.boot_param, old->boot_param, sizeof(merged.boot_param));
    merged.boot_param_len   = old->boot_param_len;
    merged.boot_param_flags = old->boot_param_flags;
    merged.boot_param_crc32 = old->boot_param_crc32;

    /*
     * 多 OTA 槽 (v0x0002 新增)
     *
     * 旧版无此字段，merged 已从 fill_defaults 拿到默认值。
     * 仅当旧版确实有该字段 (data_ver >= 0x0002) 时才覆盖。
     */
    if (from >= 0x0002) {
        merged.active_slot    = old->active_slot;
        merged.ota_slot_count = old->ota_slot_count;
        merged.ota_gpio       = old->ota_gpio;
    } else {
        ESP_LOGI(TAG, "  新增字段使用默认值: active_slot=0, "
                 "ota_slot_count=1, ota_gpio=0x%02X",
                 (unsigned)IAP_OTA_GPIO_DISABLED);
    }

    /*
     * 槽加载地址/大小 (v0x0003 新增)
     *
     * 旧版无此字段，merged 已从 fill_defaults 拿到默认值
     * (槽 0 = 0x150000，其余为 0)。仅当旧版确实有该字段时才覆盖。
     */
    if (from >= 0x0003) {
        memcpy(merged.slot_addr, old->slot_addr, sizeof(merged.slot_addr));
        memcpy(merged.slot_size, old->slot_size, sizeof(merged.slot_size));
    } else {
        ESP_LOGI(TAG, "  槽加载地址使用默认值: slot_addr[0]=0x%06X",
                 (unsigned)IAP_USER_APP_ADDR);
    }

    /* 标记为最新版本 */
    merged.data_ver = IAP_CFG_DATA_VER;

    /* 修正冲突/越界值 */
    sanitize(&merged);

    *old = merged;
    ESP_LOGI(TAG, "迁移完成，新版本 %u", (unsigned)merged.data_ver);
    return true;
}

/* -------------------------------------------------------------------------- */
/* 公共接口                                                                    */
/* -------------------------------------------------------------------------- */

esp_err_t iap_config_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    /*
     * v5: 配置区使用**硬编码地址** (IAP_CFG_ADDR = 0x008000), 不查分区表。
     *
     * iap_cfg 不在任何分区表中 (IAP 与 APP 约定同一地址),
     * 因此这里只做地址合法性检查, 不做分区查找。
     */
    ESP_LOGI(TAG, "配置区 (硬编码): offset=0x%08X, 槽数=%d, 槽大小=%d",
             (unsigned)IAP_CFG_ADDR, IAP_CFG_SLOT_COUNT, IAP_CFG_SLOT_SIZE);

    /* 地址必须按扇区对齐 */
    if ((IAP_CFG_ADDR % SPI_FLASH_SEC_SIZE) != 0) {
        ESP_LOGE(TAG, "配置区地址未按扇区对齐: 0x%08X", (unsigned)IAP_CFG_ADDR);
        return ESP_ERR_INVALID_ARG;
    }

    /* 读取两个槽，选择有效且序号更大的 */
    iap_cfg_data_t data_a, data_b;
    uint32_t seq_a = 0, seq_b = 0;
    esp_err_t err_a = read_slot(0, &data_a, &seq_a);
    esp_err_t err_b = read_slot(1, &data_b, &seq_b);

    if (err_a == ESP_OK && err_b == ESP_OK) {
        if (seq_a >= seq_b) {
            s_cfg = data_a;
            s_active_slot = 0;
            s_active_seq = seq_a;
        } else {
            s_cfg = data_b;
            s_active_slot = 1;
            s_active_seq = seq_b;
        }
        ESP_LOGI(TAG, "已加载配置 (槽 %d, seq=%" PRIu32 ")", s_active_slot, s_active_seq);
    } else if (err_a == ESP_OK) {
        s_cfg = data_a;
        s_active_slot = 0;
        s_active_seq = seq_a;
        ESP_LOGW(TAG, "槽 1 无效，使用槽 0");
    } else if (err_b == ESP_OK) {
        s_cfg = data_b;
        s_active_slot = 1;
        s_active_seq = seq_b;
        ESP_LOGW(TAG, "槽 0 无效，使用槽 1");
    } else {
        /* 两个槽都无效，写入默认配置 */
        ESP_LOGW(TAG, "配置区无有效数据，写入默认配置");
        fill_defaults(&s_cfg);
        s_active_slot = 0;
        s_active_seq = 1;
        sanitize(&s_cfg);
        esp_err_t err = write_slot(0, &s_cfg, s_active_seq);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "写入默认配置失败: %s", esp_err_to_name(err));
            return err;
        }
    }

    /*
     * 配置数据版本迁移
     *
     * 旧固件写入的配置 (data_ver 较小) 在这里自动升级:
     * 以默认配置为基准，逐项合并旧值，然后写回。
     */
    if (migrate(&s_cfg)) {
        s_active_seq++;
        esp_err_t err = write_slot(s_active_slot == 0 ? 1 : 0, &s_cfg, s_active_seq);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "写回迁移后配置失败: %s", esp_err_to_name(err));
            /* 不致命: 内存中已是迁移后的配置，下次启动会重试 */
        } else {
            s_active_slot = (s_active_slot == 0) ? 1 : 0;
            ESP_LOGI(TAG, "迁移后配置已写回槽 %d (seq=%" PRIu32 ")",
                     s_active_slot, s_active_seq);
        }
    }

    sanitize(&s_cfg);

    /* 标记配置区就绪 (后续所有 API 据此判断可用性) */
    s_ready = true;

    return ESP_OK;
}

esp_err_t iap_config_get(iap_cfg_data_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t iap_config_set(const iap_cfg_data_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    iap_cfg_data_t new_cfg = *cfg;
    sanitize(&new_cfg);

    /* 写入另一个槽，序号 +1 */
    int target_slot = (s_active_slot == 0) ? 1 : 0;
    uint32_t new_seq = s_active_seq + 1;

    esp_err_t err = write_slot(target_slot, &new_cfg, new_seq);
    if (err != ESP_OK) {
        xSemaphoreGive(s_lock);
        return err;
    }

    s_cfg = new_cfg;
    s_active_slot = target_slot;
    s_active_seq = new_seq;

    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "配置已保存 (槽 %d, seq=%" PRIu32 ")", target_slot, new_seq);
    return ESP_OK;
}

esp_err_t iap_config_update_flags(uint32_t set_mask, uint32_t clr_mask)
{
    iap_cfg_data_t cfg;
    esp_err_t err = iap_config_get(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    cfg.flags |= set_mask;
    cfg.flags &= ~clr_mask;
    return iap_config_set(&cfg);
}

bool iap_config_is_download_mode(void)
{
    iap_cfg_data_t cfg;
    if (iap_config_get(&cfg) != ESP_OK) {
        return false;
    }
    return (cfg.flags & IAP_CFG_FLAG_DOWNLOAD_MODE) != 0;
}

esp_err_t iap_config_set_download_mode(bool enable)
{
    return iap_config_update_flags(enable ? IAP_CFG_FLAG_DOWNLOAD_MODE : 0,
                                   enable ? 0 : IAP_CFG_FLAG_DOWNLOAD_MODE);
}

esp_err_t iap_config_set_boot_reason(iap_boot_reason_t reason)
{
    iap_cfg_data_t cfg;
    esp_err_t err = iap_config_get(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    cfg.last_boot_reason = (uint32_t)reason;
    return iap_config_set(&cfg);
}

esp_err_t iap_config_inc_boot_count(void)
{
    iap_cfg_data_t cfg;
    esp_err_t err = iap_config_get(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    cfg.boot_count++;
    return iap_config_set(&cfg);
}

esp_err_t iap_config_reset(void)
{
    iap_cfg_data_t cfg;
    fill_defaults(&cfg);
    return iap_config_set(&cfg);
}

esp_err_t iap_config_migrate(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    iap_cfg_data_t cfg;
    esp_err_t err = iap_config_get(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    if (!migrate(&cfg)) {
        return ESP_OK;   /* 已是最新 */
    }

    /* 写回 (iap_config_set 会自动选槽并递增 seq) */
    return iap_config_set(&cfg);
}

uint16_t iap_config_get_data_ver(void)
{
    iap_cfg_data_t cfg;
    if (iap_config_get(&cfg) != ESP_OK) {
        return 0;
    }
    return cfg.data_ver;
}

/* -------------------------------------------------------------------------- */
/* 多 OTA 槽配置                                                               */
/* -------------------------------------------------------------------------- */

uint8_t iap_config_get_active_slot(void)
{
    iap_cfg_data_t cfg;
    if (iap_config_get(&cfg) != ESP_OK) {
        return 0;
    }
    return cfg.active_slot;
}

esp_err_t iap_config_set_active_slot(uint8_t slot)
{
    iap_cfg_data_t cfg;
    esp_err_t err = iap_config_get(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    /* 截断到有效范围 */
    uint8_t count = cfg.ota_slot_count ? cfg.ota_slot_count : 1;
    if (slot >= count) {
        slot = count - 1;
    }

    if (cfg.active_slot == slot) {
        return ESP_OK;   /* 无变化，省一次 flash 写入 */
    }
    cfg.active_slot = slot;
    return iap_config_set(&cfg);
}

uint8_t iap_config_get_slot_count(void)
{
    iap_cfg_data_t cfg;
    if (iap_config_get(&cfg) != ESP_OK) {
        return 1;
    }
    return cfg.ota_slot_count ? cfg.ota_slot_count : 1;
}

esp_err_t iap_config_set_slot_count(uint8_t count)
{
    if (count == 0) {
        count = 1;
    }
    if (count > IAP_OTA_SLOT_MAX) {
        count = IAP_OTA_SLOT_MAX;
    }

    iap_cfg_data_t cfg;
    esp_err_t err = iap_config_get(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    if (cfg.ota_slot_count == count) {
        return ESP_OK;
    }
    cfg.ota_slot_count = count;

    /* 槽数变少时，活动槽也要跟着收敛 */
    if (cfg.active_slot >= count) {
        cfg.active_slot = count - 1;
    }
    return iap_config_set(&cfg);
}

uint8_t iap_config_get_ota_gpio(void)
{
    iap_cfg_data_t cfg;
    if (iap_config_get(&cfg) != ESP_OK) {
        return IAP_OTA_GPIO_DISABLED;
    }
    return cfg.ota_gpio;
}

esp_err_t iap_config_set_ota_gpio(uint8_t gpio)
{
    iap_cfg_data_t cfg;
    esp_err_t err = iap_config_get(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    if (cfg.ota_gpio == gpio) {
        return ESP_OK;
    }
    cfg.ota_gpio = gpio;
    return iap_config_set(&cfg);
}

void iap_config_get_ip_string(char *buf, size_t buf_len)
{
    iap_cfg_data_t cfg;
    if (iap_config_get(&cfg) != ESP_OK) {
        snprintf(buf, buf_len, "0.0.0.0");
        return;
    }
    snprintf(buf, buf_len, "%u.%u.%u.%u",
             (unsigned)(cfg.wifi_ip & 0xFF),
             (unsigned)((cfg.wifi_ip >> 8) & 0xFF),
             (unsigned)((cfg.wifi_ip >> 16) & 0xFF),
             (unsigned)((cfg.wifi_ip >> 24) & 0xFF));
}

void iap_config_get_netmask_string(char *buf, size_t buf_len)
{
    iap_cfg_data_t cfg;
    if (iap_config_get(&cfg) != ESP_OK) {
        snprintf(buf, buf_len, "0.0.0.0");
        return;
    }
    snprintf(buf, buf_len, "%u.%u.%u.%u",
             (unsigned)(cfg.wifi_netmask & 0xFF),
             (unsigned)((cfg.wifi_netmask >> 8) & 0xFF),
             (unsigned)((cfg.wifi_netmask >> 16) & 0xFF),
             (unsigned)((cfg.wifi_netmask >> 24) & 0xFF));
}

const char *iap_boot_reason_str(iap_boot_reason_t reason)
{
    switch (reason) {
    case IAP_BOOT_REASON_NONE:           return "未知";
    case IAP_BOOT_REASON_DOWNLOAD_FLAG:  return "配置区下载位置位";
    case IAP_BOOT_REASON_WAIT_TIMEOUT:   return "等待超时无操作";
    case IAP_BOOT_REASON_USER_REQUEST:   return "用户程序请求";
    case IAP_BOOT_REASON_CRC_FAILED:     return "用户程序 CRC 校验失败";
    case IAP_BOOT_REASON_NO_VALID_APP:   return "用户程序区无有效程序";
    case IAP_BOOT_REASON_FIRST_BOOT:     return "首次上电";
    case IAP_BOOT_REASON_TRIG_GPIO:      return "GPIO 引脚电平触发";
    case IAP_BOOT_REASON_TRIG_I2C:       return "I2C 进入命令触发";
    case IAP_BOOT_REASON_TRIG_UART:      return "UART 进入命令触发";
    case IAP_BOOT_REASON_TRIG_USB:       return "USB 进入命令触发";
    case IAP_BOOT_REASON_TRIG_WIFI:      return "WiFi 客户端接入触发";
    case IAP_BOOT_REASON_CFG_ERROR:      return "配置区错误";
    case IAP_BOOT_REASON_BOOTLOADER_MISMATCH: return "bootloader 字段不一致";
    default:                             return "保留";
    }
}

/* -------------------------------------------------------------------------- */
/* 启动参数字符串                                                              */
/* -------------------------------------------------------------------------- */

esp_err_t iap_config_set_boot_param(const char *param)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t len = (param != NULL) ? strlen(param) : 0;
    if (len > IAP_CFG_BOOT_PARAM_MAX_LEN) {
        ESP_LOGE(TAG, "启动参数过长: %u > %d", (unsigned)len, IAP_CFG_BOOT_PARAM_MAX_LEN);
        return ESP_ERR_INVALID_SIZE;
    }

    iap_cfg_data_t cfg;
    esp_err_t err = iap_config_get(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    memset(cfg.boot_param, 0, sizeof(cfg.boot_param));
    if (len > 0) {
        memcpy(cfg.boot_param, param, len);
        cfg.boot_param_len   = (uint16_t)len;
        cfg.boot_param_crc32 = iap_crc32(cfg.boot_param, len);
        cfg.boot_param_flags |= IAP_CFG_BOOT_PARAM_FLAG_VALID;
        cfg.boot_param_flags &= ~IAP_CFG_BOOT_PARAM_FLAG_CONSUMED;
    } else {
        /* 空串表示清除 */
        cfg.boot_param_len   = 0;
        cfg.boot_param_crc32 = 0;
        cfg.boot_param_flags &= ~(IAP_CFG_BOOT_PARAM_FLAG_VALID |
                                  IAP_CFG_BOOT_PARAM_FLAG_CONSUMED);
    }

    err = iap_config_set(&cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "启动参数已写入 (%u 字符): %s",
                 (unsigned)len, (len > 0) ? param : "(已清除)");
    }
    return err;
}

esp_err_t iap_config_get_boot_param(char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    iap_cfg_data_t cfg;
    esp_err_t err = iap_config_get(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    /* 未设置 */
    if (!(cfg.boot_param_flags & IAP_CFG_BOOT_PARAM_FLAG_VALID) ||
        cfg.boot_param_len == 0) {
        buf[0] = '\0';
        return ESP_ERR_NOT_FOUND;
    }

    /* 长度字段越界保护 */
    if (cfg.boot_param_len > IAP_CFG_BOOT_PARAM_MAX_LEN) {
        ESP_LOGW(TAG, "启动参数长度字段非法: %u", cfg.boot_param_len);
        buf[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }

    /* CRC 校验 */
    uint32_t crc = iap_crc32(cfg.boot_param, cfg.boot_param_len);
    if (crc != cfg.boot_param_crc32) {
        ESP_LOGW(TAG, "启动参数 CRC 校验失败: 计算=0x%08" PRIx32 " 存储=0x%08" PRIx32,
                 crc, cfg.boot_param_crc32);
        buf[0] = '\0';
        return ESP_ERR_INVALID_CRC;
    }

    if (buf_len <= cfg.boot_param_len) {
        ESP_LOGE(TAG, "缓冲区不足: %u <= %u", (unsigned)buf_len, cfg.boot_param_len);
        buf[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(buf, cfg.boot_param, cfg.boot_param_len);
    buf[cfg.boot_param_len] = '\0';
    return ESP_OK;
}

esp_err_t iap_config_take_boot_param(char *buf, size_t buf_len)
{
    esp_err_t err = iap_config_get_boot_param(buf, buf_len);
    if (err != ESP_OK) {
        return err;
    }

    /* 标记为已消费 */
    iap_cfg_data_t cfg;
    if (iap_config_get(&cfg) == ESP_OK) {
        cfg.boot_param_flags |= IAP_CFG_BOOT_PARAM_FLAG_CONSUMED;
        iap_config_set(&cfg);
    }
    return ESP_OK;
}

esp_err_t iap_config_clear_boot_param(void)
{
    return iap_config_set_boot_param(NULL);
}

bool iap_config_has_boot_param(void)
{
    iap_cfg_data_t cfg;
    if (iap_config_get(&cfg) != ESP_OK) {
        return false;
    }
    if (!(cfg.boot_param_flags & IAP_CFG_BOOT_PARAM_FLAG_VALID) ||
        cfg.boot_param_len == 0 ||
        cfg.boot_param_len > IAP_CFG_BOOT_PARAM_MAX_LEN) {
        return false;
    }
    return iap_crc32(cfg.boot_param, cfg.boot_param_len) == cfg.boot_param_crc32;
}

bool iap_config_boot_param_consumed(void)
{
    iap_cfg_data_t cfg;
    if (iap_config_get(&cfg) != ESP_OK) {
        return false;
    }
    return (cfg.boot_param_flags & IAP_CFG_BOOT_PARAM_FLAG_CONSUMED) != 0;
}

esp_err_t iap_config_boot_param_get_value(const char *param, const char *key,
                                          char *out, size_t out_len)
{
    if (param == NULL || key == NULL || out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    out[0] = '\0';

    size_t key_len = strlen(key);
    if (key_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *p = param;

    while (*p != '\0') {
        /* 跳过前导分隔符与空白 */
        while (*p == ';' || *p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        /* 当前键值对的结束位置 */
        const char *end = strchr(p, ';');
        if (end == NULL) {
            end = p + strlen(p);
        }

        /* 查找 '=' */
        const char *eq = memchr(p, '=', (size_t)(end - p));
        if (eq != NULL) {
            size_t cur_key_len = (size_t)(eq - p);

            /* 去除键尾部空白 */
            while (cur_key_len > 0 &&
                   (p[cur_key_len - 1] == ' ' || p[cur_key_len - 1] == '\t')) {
                cur_key_len--;
            }

            if (cur_key_len == key_len && strncmp(p, key, key_len) == 0) {
                /* 命中: 复制值 */
                const char *val = eq + 1;
                size_t val_len = (size_t)(end - val);

                /* 去除值首尾空白 */
                while (val_len > 0 && (*val == ' ' || *val == '\t')) {
                    val++;
                    val_len--;
                }
                while (val_len > 0 &&
                       (val[val_len - 1] == ' ' || val[val_len - 1] == '\t')) {
                    val_len--;
                }

                if (val_len >= out_len) {
                    return ESP_ERR_INVALID_SIZE;
                }
                memcpy(out, val, val_len);
                out[val_len] = '\0';
                return ESP_OK;
            }
        }

        p = end;
        if (*p == ';') {
            p++;
        }
    }

    return ESP_ERR_NOT_FOUND;
}
