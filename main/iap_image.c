/*
 * ESP IAP - 用户程序镜像管理
 *
 * 负责:
 *   1. 用户程序分区的擦除/写入/读取
 *   2. 镜像存在性检查（首字节是否为 0xE9）
 *   3. IAP <-> 用户程序 之间的启动分区切换
 *
 * 【设计说明】
 *
 * user_app 分区内是**纯 ESP 应用镜像**（无自定义文件头）:
 *   +---------------------------+ offset 0
 *   |  ESP 应用镜像 (magic 0xE9)|  ← bootloader / OTA 直接可读
 *   +---------------------------+ offset image_size
 *   |  0xFF 填充                |
 *   +---------------------------+ offset size
 *
 * 镜像合法性完全交给 bootloader 自校验（magic / 段表 / SHA256 / chip_id）。
 * 校验失败时 bootloader 会自动回落到 factory (IAP)，不会变砖。
 *
 * 【v6: 单一烧录文件】
 *
 * 用户程序打包为**从 0x140000 开始的单一文件**（分区表 B + 应用镜像），
 * IAP 整段写入可变区（见 iap_image_var_region_write）。
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "spi_flash_mmap.h"     /* SPI_FLASH_SEC_SIZE (v6) */
#include "soc/soc.h"
#include "rom/sha.h"
#include "esp_rom_md5.h"        /* 分区表 B MD5 校验 (v6, ROM 实现无依赖) */
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "iap_common.h"
#include "iap_crc.h"
#include "iap_config.h"
#include "iap_image.h"
#include "iap_boot_param.h"     /* RTC RAM 启动参数 (v5) */
#include "iap_wifi.h"
#include "iap_http.h"

static const char *TAG = "iap_image";

/**
 * OTA 槽分区句柄数组
 *
 * s_slots[0] = ota_0 (标签 "user_app"，兼容旧版单槽分区表)
 * s_slots[1] = ota_1 (标签 "user_app_1")
 * ...
 * 未找到的槽为 NULL。
 */
static const esp_partition_t *s_slots[IAP_OTA_SLOT_MAX] = { NULL };

/** 已发现的 OTA 槽数量 (>=1) */
static uint8_t s_slot_count = 0;

/** 当前活动的 OTA 槽序号 */
static uint8_t s_active_slot = 0;

/**
 * 配置区的活动槽是否无效 (越界或分区不存在)
 *
 * 由 iap_image_init() 设置。为 true 时 main.c 应进入 IAP 下载模式。
 */
static bool s_slot_invalid = false;

/** IAP 程序区分区句柄 */
static const esp_partition_t *s_iap_part = NULL;

/* v5: 魔术标记分区句柄 (s_mark_part) 已删除 —— 标志区已取消 */

/* -------------------------------------------------------------------------- */
/* 分区查找                                                                    */
/* -------------------------------------------------------------------------- */

/*
 * v7: IAP **不再解析任何分区表**。
 *
 * 槽的加载地址与大小来自**配置区** (slot_addr[] / slot_size[])，由用户在
 * 浏览器工具「APP 启动槽」中手动填写。这样 IAP 与用户程序的分区布局彻底
 * 解耦 —— 分区表 B 长什么样、放在哪，IAP 完全不关心。
 *
 * slot_addr[i] == 0 表示该槽无效。
 */

/**
 * @brief 取槽 i 的加载地址 (配置区)
 *
 * @return 非 0 地址；槽无效或配置读取失败返回 0
 */
static uint64_t slot_addr_of(uint8_t slot)
{
    if (slot >= IAP_OTA_SLOT_MAX) return 0;

    iap_cfg_data_t cfg;
    if (iap_config_get(&cfg) != ESP_OK) return 0;

    return cfg.slot_addr[slot];
}

/**
 * @brief 取槽 i 的可用大小 (配置区)
 *
 * @return 字节数；未配置返回 0
 */
static uint64_t slot_size_of(uint8_t slot)
{
    if (slot >= IAP_OTA_SLOT_MAX) return 0;

    iap_cfg_data_t cfg;
    if (iap_config_get(&cfg) != ESP_OK) return 0;

    return cfg.slot_size[slot];
}

/**
 * @brief 按 OTA 槽序号构造分区句柄 (地址来自配置区)
 *
 * 句柄数据存于静态区 (生命周期 = 程序运行期)，可直接返回指针。
 * 槽地址为 0 (未配置) 时返回 NULL。
 */
static const esp_partition_t *find_ota_slot(uint8_t slot)
{
    if (slot >= IAP_OTA_SLOT_MAX) {
        return NULL;
    }

    uint64_t addr64 = slot_addr_of(slot);
    if (addr64 == 0) {
        return NULL;   /* 该槽未配置 */
    }

    /*
     * ESP32 为 32 位架构，地址高 32 位必须为 0。
     * 若用户误填了超出 32 位的值，视为无效并告警。
     */
    if (addr64 > 0xFFFFFFFFULL) {
        ESP_LOGW(TAG, "槽 %u 地址 0x%08" PRIx32 "%08" PRIx32 " 超出 32 位，忽略",
                 (unsigned)slot,
                 (uint32_t)(addr64 >> 32), (uint32_t)(addr64 & 0xFFFFFFFFULL));
        return NULL;
    }

    uint32_t addr = (uint32_t)addr64;
    uint32_t size = (uint32_t)slot_size_of(slot);

    /* 大小未配置时，用"到固定区末尾"兜底 (仅用于边界校验) */
    if (size == 0) {
        size = (addr < IAP_FIXED_REGION_END)
             ? (IAP_FIXED_REGION_END - addr)
             : (0x400000u - addr);   /* 4MB 兜底 */
    }

    static esp_partition_t parts[IAP_OTA_SLOT_MAX];
    static bool used[IAP_OTA_SLOT_MAX] = { false };

    esp_partition_t *p = &parts[slot];
    if (!used[slot]) {
        memset(p, 0, sizeof(*p));
        p->flash_chip = esp_flash_default_chip;
        p->type       = ESP_PARTITION_TYPE_APP;
        p->subtype    = (esp_partition_subtype_t)(IAP_OTA_SUBTYPE_BASE + slot);
        p->erase_size = SPI_FLASH_SEC_SIZE;
        snprintf(p->label, sizeof(p->label), "slot_%u", (unsigned)slot);
        used[slot] = true;
    }

    p->address = addr;
    p->size    = size;

    ESP_LOGI(TAG, "槽 %u: 配置区地址 0x%08" PRIx32 " (%" PRIu32 " KB)",
             (unsigned)slot, addr, size / 1024);
    return p;
}

/* -------------------------------------------------------------------------- */
/* bootloader 字段一致性检查                                                   */
/* -------------------------------------------------------------------------- */

/*
 * 把 IDF 配置字符串转为 ESP 镜像头中的编码值
 *
 * 镜像头 byte3 的位域 (见 esp_app_format.h):
 *     uint8_t spi_speed : 4;   // 低 4 位 (bits 0-3)
 *     uint8_t spi_size  : 4;   // 高 4 位 (bits 4-7)
 * 即 byte3 = (spi_size << 4) | spi_speed
 *
 *   spi_size  (esp_image_flash_size_t): 0=1MB 1=2MB 2=4MB 3=8MB 4=16MB ...
 *   spi_speed (esp_image_spi_freq_t)  : 0=DIV_2(40M) 1=DIV_3(26M)
 *                                       2=DIV_4(20M) 0xF=DIV_1(80M)
 */
static uint8_t iap_flashsize_to_code(const char *s)
{
    if (s == NULL) return 0;
    if (strcmp(s, "1MB")   == 0) return 0;
    if (strcmp(s, "2MB")   == 0) return 1;
    if (strcmp(s, "4MB")   == 0) return 2;
    if (strcmp(s, "8MB")   == 0) return 3;
    if (strcmp(s, "16MB")  == 0) return 4;
    if (strcmp(s, "32MB")  == 0) return 5;
    if (strcmp(s, "64MB")  == 0) return 6;
    if (strcmp(s, "128MB") == 0) return 7;
    return 0;
}

static uint8_t iap_flashmode_to_code(const char *s)
{
    if (s == NULL) return 0;
    if (strcmp(s, "qio")  == 0) return 0;
    if (strcmp(s, "qout") == 0) return 1;
    if (strcmp(s, "dio")  == 0) return 2;
    if (strcmp(s, "dout") == 0) return 3;
    return 2;
}

/* 仅用于日志显示 */
static const char *iap_speed_code_to_str(uint8_t code)
{
    switch (code) {
    case 0:   return "40MHz";
    case 1:   return "26MHz";
    case 2:   return "20MHz";
    case 0xF: return "80MHz";
    default:  return "?";
    }
}

/* 仅用于日志显示 */
static const char *iap_mode_code_to_str(uint8_t code)
{
    switch (code) {
    case 0: return "QIO";
    case 1: return "QOUT";
    case 2: return "DIO";
    case 3: return "DOUT";
    default: return "?";
    }
}

esp_err_t iap_image_check_bootloader(iap_bootloader_info_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->detail[0] = '\0';

    /* 读取 bootloader 镜像头 (24 字节) */
    uint8_t hdr[IAP_IMAGE_HEADER_SIZE];
    esp_err_t err = esp_flash_read(NULL, hdr, IAP_BOOTLOADER_OFFSET,
                                   sizeof(hdr));
    if (err != ESP_OK) {
        snprintf(out->detail, sizeof(out->detail),
                 "读取 flash 0x%X 失败: %s", IAP_BOOTLOADER_OFFSET,
                 esp_err_to_name(err));
        return err;
    }

    out->magic         = hdr[0];
    out->segment_count = hdr[1];
    out->spi_mode      = hdr[2];
    /*
     * byte3 位域: spi_speed 在低 4 位, spi_size 在高 4 位
     * (见 esp_app_format.h 的 esp_image_header_t)
     */
    out->spi_speed     = hdr[3] & 0x0F;
    out->spi_size      = (hdr[3] >> 4) & 0x0F;
    out->chip_id       = (uint16_t)(hdr[12] | (hdr[13] << 8));

    /* 1. magic */
    if (out->magic != IAP_IMAGE_MAGIC) {
        out->present    = false;
        out->compatible = false;
        snprintf(out->detail, sizeof(out->detail),
                 "flash 0x%X 处无有效 bootloader (magic=0x%02X, 期望 0x%02X)",
                 IAP_BOOTLOADER_OFFSET, out->magic, IAP_IMAGE_MAGIC);
        return ESP_OK;
    }
    out->present = true;

    /* 2. chip_id */
    uint16_t expect_chip = (uint16_t)CONFIG_IDF_FIRMWARE_CHIP_ID;
    if (out->chip_id != expect_chip) {
        out->compatible = false;
        snprintf(out->detail, sizeof(out->detail),
                 "芯片 ID 不匹配: bootloader=%u, 本固件=%u",
                 (unsigned)out->chip_id, (unsigned)expect_chip);
        return ESP_OK;
    }

    /* 3. flash 模式/容量
     *
     * 注意: **不比对 spi_speed**。
     *   ESP-IDF 构建的 bootloader 该字段常为 0 (DIV_2/40MHz)，
     *   与 CONFIG_ESPTOOLPY_FLASHFREQ 无关 —— bootloader 会在运行时
     *   通过 esp_flash_init() 按配置重新设置实际频率。
     *   比对它会产生误报。
     */
    uint8_t exp_mode = iap_flashmode_to_code(CONFIG_ESPTOOLPY_FLASHMODE);
    uint8_t exp_size = iap_flashsize_to_code(CONFIG_ESPTOOLPY_FLASHSIZE);

    if (out->spi_mode != exp_mode) {
        out->compatible = false;
        snprintf(out->detail, sizeof(out->detail),
                 "flash 模式不匹配: bootloader=%u, 本固件=%u (%s)",
                 (unsigned)out->spi_mode, (unsigned)exp_mode,
                 CONFIG_ESPTOOLPY_FLASHMODE);
        return ESP_OK;
    }
    if (out->spi_size != exp_size) {
        out->compatible = false;
        snprintf(out->detail, sizeof(out->detail),
                 "flash 容量不匹配: bootloader=%u, 本固件=%u (%s)",
                 (unsigned)out->spi_size, (unsigned)exp_size,
                 CONFIG_ESPTOOLPY_FLASHSIZE);
        return ESP_OK;
    }

    out->compatible = true;
    snprintf(out->detail, sizeof(out->detail),
             "bootloader 与本固件兼容 (chip_id=%u, mode=%u, size=%u)",
             (unsigned)out->chip_id, (unsigned)out->spi_mode,
             (unsigned)out->spi_size);
    return ESP_OK;
}

esp_err_t iap_image_init(void)
{
    /* ---- bootloader 字段一致性检查 ---- */
    /*
     * 只烧 app (0x20000) 时，设备上的 bootloader 可能是用不同 flash 配置
     * 编译的。此处提前检查，便于确认设备上的 bootloader 来源。
     *
     * 注意: 这里**仅作排查参考**，不阻断启动。
     *       实测表明字段不一致不会导致启动失败 —— bootloader 会在运行时
     *       探测实际 flash 容量并容忍该差异。
     */
    iap_bootloader_info_t bl;
    if (iap_image_check_bootloader(&bl) == ESP_OK) {
        if (!bl.present) {
            ESP_LOGE(TAG, "========================================");
            ESP_LOGE(TAG, " bootloader 检查失败!");
            ESP_LOGE(TAG, "   %s", bl.detail);
            ESP_LOGE(TAG, "========================================");
        } else if (!bl.compatible) {
            ESP_LOGW(TAG, "========================================");
            ESP_LOGW(TAG, " bootloader 与本固件字段不一致");
            ESP_LOGW(TAG, "   %s", bl.detail);
            ESP_LOGW(TAG, " 提示: 设备上的 bootloader 可能来自其它构建配置");
            ESP_LOGW(TAG, "========================================");
        } else {
            ESP_LOGI(TAG, "%s", bl.detail);
            ESP_LOGI(TAG, "  bootloader: mode=%s, speed=%s, size=%u",
                     iap_mode_code_to_str(bl.spi_mode),
                     iap_speed_code_to_str(bl.spi_speed),
                     (unsigned)bl.spi_size);
        }
    }

    /* ---- 扫描所有 OTA 槽 ---- */
    s_slot_count = 0;
    for (uint8_t i = 0; i < IAP_OTA_SLOT_MAX; i++) {
        s_slots[i] = find_ota_slot(i);
        if (s_slots[i] != NULL) {
            s_slot_count = i + 1;   /* 记录最大连续槽号 */
        }
    }

    if (s_slots[0] == NULL) {
        ESP_LOGE(TAG, "配置区未配置槽 0 的加载地址 (slot_addr[0] = 0)");
        ESP_LOGE(TAG, "  请在浏览器工具「APP 启动槽」中填写各槽的加载地址");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "发现 %u 个 OTA 槽:", (unsigned)s_slot_count);
    for (uint8_t i = 0; i < s_slot_count; i++) {
        if (s_slots[i] == NULL) {
            ESP_LOGW(TAG, "  [%u] (缺失)", (unsigned)i);
            continue;
        }
        ESP_LOGI(TAG, "  [%u] ota_%u: offset=0x%08" PRIx32
                 ", size=%" PRIu32 " KB",
                 (unsigned)i, (unsigned)i,
                 s_slots[i]->address, s_slots[i]->size / 1024);
    }

    s_iap_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP,
        ESP_PARTITION_SUBTYPE_APP_FACTORY,
        IAP_PARTITION_LABEL_IAP);

    if (s_iap_part == NULL) {
        s_iap_part = esp_partition_find_first(
            ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY,
            IAP_PARTITION_LABEL_IAP);
    }

    if (s_iap_part) {
        ESP_LOGI(TAG, "IAP 程序区: offset=0x%08" PRIx32 ", size=%" PRIu32,
                 s_iap_part->address, s_iap_part->size);
    }

    /*
     * v5: 魔术标记区 (iap_mark) 已取消 —— 启动决策改放 RTC RAM,
     *     「强制进入 IAP」改由 GPIO0 电平检测提供 (iap_wait_trigger.c)。
     *     详见 docs/FINAL-REPORT.md §3.3
     */

    /* ---- 同步槽信息到配置区 ---- */
    iap_config_set_slot_count(s_slot_count);

    uint8_t cfg_slot = iap_config_get_active_slot();
    s_active_slot = cfg_slot;

    /*
     * 配置区的活动槽越界 / 分区不存在 -> 标记需要进入 IAP 下载模式
     *
     * 场景 C: 用户设置了 active_slot = 5，但设备只有 2 个槽。
     * 此时**不静默回退到槽 0**，而是进入 IAP 下载模式，
     * 让用户通过终端修正配置或重新烧录。
     *
     * 注意: 这里只置标志，实际进入 IAP 由 main.c 的启动决策处理，
     *       以便统一走「进入 IAP 下载模式」的完整流程。
     */
    s_slot_invalid = false;
    if (cfg_slot >= s_slot_count || s_slots[cfg_slot] == NULL) {
        ESP_LOGE(TAG, "========================================");
        ESP_LOGE(TAG, " 配置的活动槽无效!");
        ESP_LOGE(TAG, "   配置值   : %u", (unsigned)cfg_slot);
        if (s_slot_count == 0) {
            ESP_LOGE(TAG, "   有效范围 : (无可用槽)");
        } else {
            ESP_LOGE(TAG, "   有效范围 : 0 ~ %u", (unsigned)(s_slot_count - 1));
        }
        ESP_LOGE(TAG, "   将进入 IAP 下载模式以便修正");
        ESP_LOGE(TAG, "========================================");
        s_slot_invalid = true;
        s_active_slot = 0;   /* 内部暂用槽 0，避免越界访问 */
    } else {
        ESP_LOGI(TAG, "当前活动槽: %u (ota_%u)",
                 (unsigned)s_active_slot, (unsigned)s_active_slot);
    }

    /*
     * flash 容量一致性检查
     *
     * 分区表按目标 flash 容量生成 (见 tools/gen_partitions.py)，
     * 最后一个 OTA 槽的末尾应等于 flash 总容量。
     *
     * 若实际 flash 容量与分区表不匹配:
     *   - 实际 < 分区表: 分区表尾部越界，bootloader 通常会拒绝启动；
     *                    若侥幸启动，写用户程序会越界损坏数据
     *   - 实际 > 分区表: 浪费空间 (不致命，仅提示)
     *
     * 这里做运行时校验并告警，便于现场快速定位烧错模块的问题。
     */
    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK && flash_size > 0) {
        /* 取所有槽中最靠后的结束地址 */
        uint32_t part_end = 0;
        for (uint8_t i = 0; i < s_slot_count; i++) {
            if (s_slots[i] == NULL) continue;
            uint32_t e = s_slots[i]->address + s_slots[i]->size;
            if (e > part_end) part_end = e;
        }

        if (part_end > flash_size) {
            ESP_LOGE(TAG, "========================================");
            ESP_LOGE(TAG, " 分区表与 flash 容量不匹配!");
            ESP_LOGE(TAG, "   实际 flash : %" PRIu32 " KB",
                     flash_size / 1024);
            ESP_LOGE(TAG, "   分区表末尾 : %" PRIu32 " KB (超出 %" PRIu32 " KB)",
                     part_end / 1024, (part_end - flash_size) / 1024);
            ESP_LOGE(TAG, "   请用 tools/gen_partitions.py 按实际容量重新生成");
            ESP_LOGE(TAG, "   分区表并单独烧录到 0x8000");
            ESP_LOGE(TAG, "========================================");
        } else {
            uint32_t unused = flash_size - part_end;
            ESP_LOGI(TAG, "flash 容量校验通过: 实际 %" PRIu32 " KB, "
                     "未使用 %" PRIu32 " KB",
                     flash_size / 1024, unused / 1024);
            if (unused >= 0x100000) {
                ESP_LOGW(TAG, "有 %" PRIu32 " KB flash 未分配，"
                         "可用 gen_partitions.py 扩大分区",
                         unused / 1024);
            }
        }
    }

    return ESP_OK;
}

const esp_partition_t *iap_image_get_user_partition(void)
{
    return s_slots[0];
}

const esp_partition_t *iap_image_get_iap_partition(void)
{
    return s_iap_part;
}

/* -------------------------------------------------------------------------- */
/* 多 OTA 槽查询                                                               */
/* -------------------------------------------------------------------------- */

uint8_t iap_image_get_slot_count(void)
{
    return s_slot_count ? s_slot_count : 1;
}

const esp_partition_t *iap_image_get_slot(uint8_t slot)
{
    if (slot >= IAP_OTA_SLOT_MAX) {
        return NULL;
    }
    return s_slots[slot];
}

uint8_t iap_image_get_active_slot(void)
{
    return s_active_slot;
}

bool iap_image_slot_invalid(void)
{
    return s_slot_invalid;
}

esp_err_t iap_image_set_active_slot(uint8_t slot)
{
    if (slot >= s_slot_count || s_slots[slot] == NULL) {
        /*
         * ⚠️ s_slot_count 可能为 0 (init 失败提前返回)，此时
         *    s_slot_count - 1 会下溢为 4294967295 —— 必须防住。
         */
        if (s_slot_count == 0) {
            ESP_LOGE(TAG, "无效的 OTA 槽: %u (无可用槽，镜像模块未初始化)",
                     (unsigned)slot);
        } else {
            ESP_LOGE(TAG, "无效的 OTA 槽: %u (有效范围 0~%u)",
                     (unsigned)slot, (unsigned)(s_slot_count - 1));
        }
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = iap_config_set_active_slot(slot);
    if (err == ESP_OK) {
        s_active_slot = slot;
        ESP_LOGI(TAG, "活动槽已切换为 %u (ota_%u)", (unsigned)slot, (unsigned)slot);
    }
    return err;
}

/**
 * @brief 按 GPIO 电平选择 OTA 槽
 *
 * 读取 ota_gpio 引脚的电平，映射为槽序号:
 *   槽序号 = 电平值 & (slot_count - 1)
 *
 * 仅当 slot_count 是 2 的幂时映射才均匀；否则用取模。
 * 引脚未配置或读取失败时返回当前活动槽。
 *
 * @return 选中的槽序号
 */
uint8_t iap_image_slot_from_gpio(void)
{
    uint8_t gpio = iap_config_get_ota_gpio();
    if (gpio == IAP_OTA_GPIO_DISABLED) {
        return s_active_slot;
    }

    uint8_t count = iap_image_get_slot_count();
    if (count <= 1) {
        return 0;
    }

    /* 配置为输入，启用内部上拉 (悬空时读到 1) */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << gpio),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&io) != ESP_OK) {
        ESP_LOGW(TAG, "配置 OTA 选择引脚 GPIO%u 失败", (unsigned)gpio);
        return s_active_slot;
    }

    int level = gpio_get_level((gpio_num_t)gpio);
    uint8_t slot = (uint8_t)((level < 0 ? 0 : level) % count);

    ESP_LOGI(TAG, "GPIO%u 电平=%d -> OTA 槽 %u (共 %u 槽)",
             (unsigned)gpio, level, (unsigned)slot, (unsigned)count);
    return slot;
}

/* -------------------------------------------------------------------------- */
/* 镜像存在性检查                                                              */
/* -------------------------------------------------------------------------- */

bool iap_image_user_app_present(void)
{
    return iap_image_slot_present(s_active_slot);
}

bool iap_image_slot_present(uint8_t slot)
{
    const esp_partition_t *p = iap_image_get_slot(slot);
    if (p == NULL) {
        return false;
    }

    uint8_t magic = 0;
    if (esp_partition_read(p, 0, &magic, 1) != ESP_OK) {
        return false;
    }
    return (magic == IAP_ESP_IMAGE_MAGIC);
}

/* -------------------------------------------------------------------------- */
/* 写入与擦除                                                                  */
/* -------------------------------------------------------------------------- */

esp_err_t iap_image_erase(uint32_t size)
{
    return iap_image_slot_erase(s_active_slot, size, NULL);
}

/*
 * 单次擦除的分块大小。
 *
 * esp_partition_erase_range 一次性擦除 2.7MB 需十几秒且无任何输出,
 * 用户会误以为卡死。改为按块擦除并在块间回调进度。
 * 取 256KB: 对 2.7MB 分区约 11 块, 进度刷新足够细腻,
 * 又不会因回调过于频繁而拖慢整体速度。
 */
#define IAP_ERASE_CHUNK_BYTES   (256u * 1024u)

esp_err_t iap_image_slot_erase(uint8_t slot, uint32_t size,
                               iap_image_progress_fn_t progress)
{
    const esp_partition_t *p = iap_image_get_slot(slot);
    if (p == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (size == 0 || size > p->size) {
        size = p->size;
    }

    /* 擦除长度向上对齐到扇区 */
    uint32_t erase_size = (size + p->erase_size - 1)
                        / p->erase_size * p->erase_size;
    if (erase_size > p->size) {
        erase_size = p->size;
    }

    ESP_LOGI(TAG, "擦除 OTA 槽 %u %" PRIu32 " 字节...",
             (unsigned)slot, erase_size);

    /* 分块擦除, 每块完成后回调进度 */
    uint32_t done = 0;
    while (done < erase_size) {
        uint32_t chunk = erase_size - done;
        if (chunk > IAP_ERASE_CHUNK_BYTES) {
            chunk = IAP_ERASE_CHUNK_BYTES;
        }

        esp_err_t err = esp_partition_erase_range(p, done, chunk);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "擦除失败 @0x%" PRIx32 ": %s",
                     done, esp_err_to_name(err));
            return err;
        }

        done += chunk;
        if (progress != NULL) {
            progress(done, erase_size);
        }
    }

    return ESP_OK;
}

esp_err_t iap_image_write(uint32_t offset, const void *data, size_t len)
{
    return iap_image_slot_write(s_active_slot, offset, data, len);
}

esp_err_t iap_image_slot_write(uint8_t slot, uint32_t offset,
                               const void *data, size_t len)
{
    const esp_partition_t *p = iap_image_get_slot(slot);
    if (p == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (offset + len > p->size) {
        ESP_LOGE(TAG, "写入越界: slot=%u offset=0x%" PRIx32 " len=%u",
                 (unsigned)slot, offset, (unsigned)len);
        return ESP_ERR_INVALID_SIZE;
    }
    return esp_partition_write(p, offset, data, len);
}

esp_err_t iap_image_read(uint32_t offset, void *data, size_t len)
{
    return iap_image_slot_read(s_active_slot, offset, data, len);
}

esp_err_t iap_image_slot_read(uint8_t slot, uint32_t offset,
                              void *data, size_t len)
{
    const esp_partition_t *p = iap_image_get_slot(slot);
    if (p == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (offset + len > p->size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return esp_partition_read(p, offset, data, len);
}

/* -------------------------------------------------------------------------- */
/* 可变区整段写入 (v6)                                                         */
/* -------------------------------------------------------------------------- */
/*
 * v6 烧录方案: 用户程序打包为**从 0x140000 开始的单一文件**
 *   (分区表 B + 0xFF 填充 + 应用镜像)，IAP 整段写入可变区。
 *
 * 与 esptool 语义一致: 把文件字节原样写到指定地址，不解析内容。
 *
 * 安全约束:
 *   - 起始地址必须 >= IAP_FIXED_REGION_END (0x140000)，禁止写固定区
 *   - 长度不得超过 flash 末尾
 *   - 写入前整段擦除
 */

/** 可变区整段擦除 + 写入 */
esp_err_t iap_image_var_region_write(uint32_t addr, const void *data, size_t len)
{
    if (addr < IAP_FIXED_REGION_END) {
        ESP_LOGE(TAG, "写入地址 0x%08" PRIx32 " 落在固定区 (< 0x%06X)，拒绝",
                 addr, (unsigned)IAP_FIXED_REGION_END);
        return ESP_ERR_INVALID_ARG;
    }
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK || flash_size == 0) {
        ESP_LOGE(TAG, "读取 flash 容量失败");
        return ESP_ERR_INVALID_STATE;
    }
    if (addr + len > flash_size) {
        ESP_LOGE(TAG, "写入越界: 0x%08" PRIx32 " + %u > flash %" PRIu32,
                 addr, (unsigned)len, flash_size);
        return ESP_ERR_INVALID_SIZE;
    }

    /* 擦除长度向上对齐到扇区 */
    uint32_t erase_len = (uint32_t)((len + SPI_FLASH_SEC_SIZE - 1)
                                    / SPI_FLASH_SEC_SIZE * SPI_FLASH_SEC_SIZE);

    ESP_LOGI(TAG, "擦除可变区 0x%08" PRIx32 " ~ 0x%08" PRIx32 " (%" PRIu32 " KB)...",
             addr, addr + erase_len, erase_len / 1024);
    esp_err_t err = esp_flash_erase_region(NULL, addr, erase_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "擦除失败: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "写入 %u 字节 @ 0x%08" PRIx32 "...", (unsigned)len, addr);
    err = esp_flash_write(NULL, data, addr, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写入失败: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "可变区写入完成");
    return ESP_OK;
}

/**
 * @brief 从可变区镜像中提取应用镜像大小
 *
 * 输入文件 (从 0x140000 开始) 的结构:
 *   0x0000  分区表 B (4KB)
 *   0x1000  0xFF 填充
 *   0x10000 应用镜像 (0xE9 开头)
 *
 * 返回应用镜像的实际长度 (扫描到 0xFF 填充为止)，失败返回 0。
 *
 * @param data 文件数据
 * @param len  文件长度
 * @return 应用镜像长度 (字节)，0 表示格式不符
 */
size_t iap_image_parse_var_image(const uint8_t *data, size_t len)
{
    const size_t app_off = IAP_USER_APP_ADDR - IAP_FIXED_REGION_END;

    if (len < app_off + 1) {
        ESP_LOGW(TAG, "镜像过短 (%u < %u)", (unsigned)len, (unsigned)(app_off + 1));
        return 0;
    }
    if (data[app_off] != IAP_ESP_IMAGE_MAGIC) {
        ESP_LOGW(TAG, "偏移 0x%X 处首字节 0x%02X 不是 ESP 镜像 magic (0xE9)",
                 (unsigned)app_off, data[app_off]);
        return 0;
    }

    /* 从末尾向前跳过 0xFF 填充，得到实际长度 */
    size_t end = len;
    while (end > app_off && data[end - 1] == 0xFF) {
        end--;
    }
    return end - app_off;
}

/* -------------------------------------------------------------------------- */
/* 启动分区切换                                                                */
/* -------------------------------------------------------------------------- */

/**
 * @brief 启动槽不可用时回落到 IAP
 *
 * 当请求启动的 OTA 槽不存在、无镜像或校验失败时调用:
 *   1. 把活动槽重置为 0（避免下次启动又失败）
 *   2. 设置启动分区为 IAP (factory) 并重启
 *
 * 注意: factory 分区会擦除 otadata，bootloader 默认启动 factory，
 *       因此这是最可靠的回落路径。
 *
 * @param reason 回落原因（用于日志与配置区记录）
 * @return 仅在失败时返回；成功时已重启，不会返回
 */
static esp_err_t boot_fallback_iap(const char *reason)
{
    ESP_LOGW(TAG, "========================================");
    ESP_LOGW(TAG, " 启动槽不可用，回落到 IAP");
    ESP_LOGW(TAG, " 原因: %s", reason ? reason : "未知");
    ESP_LOGW(TAG, "========================================");

    /* 重置活动槽，避免下次启动再次失败 */
    if (s_active_slot != 0) {
        iap_config_set_active_slot(0);
        s_active_slot = 0;
    }

    /* 记录回落原因 */
    iap_config_set_boot_reason(IAP_BOOT_REASON_NO_VALID_APP);

    return iap_image_boot_iap();
}

/* ========================================================================== */
/* 直接跳转启动 (不使用 otadata)                                               */
/* ========================================================================== */

/*
 * 【为什么需要直接跳转】
 *
 * ESP-IDF 的 otadata 机制**就是为了绕过 factory 直接启动 ota_x**。
 * bootloader_utility_get_selected_boot_partition() 的逻辑:
 *
 *   if (otadata 全 0xFF)  -> 启动 factory (IAP)
 *   else                  -> 启动 otadata 指向的 ota_x (user_app)
 *
 * 因此只要调用过 esp_ota_set_boot_partition(user_app)，
 * **下次上电 bootloader 会直接加载 user_app，IAP 完全不执行**。
 *
 * 若需求是「每次上电都必须先跑 IAP」，就必须**永不写 otadata**，
 * 改为由 IAP 自己把镜像段拷贝到 RAM 并跳转。
 */

/* ESP 镜像头结构 (与 esp_app_format.h 一致, 仅取所需字段) */
typedef struct __attribute__((packed)) {
    uint8_t  magic;             /*!< 0xE9 */
    uint8_t  segment_count;     /*!< 段数量 */
    uint8_t  spi_mode;
    uint8_t  spi_speed: 4;
    uint8_t  spi_size:  4;
    uint32_t entry_addr;        /*!< 入口地址 */
    uint8_t  wp_pin;
    uint8_t  spi_pin_drv[3];
    uint16_t chip_id;           /*!< 芯片 ID */
    uint8_t  min_chip_rev;
    uint16_t min_chip_rev_full;
    uint16_t max_chip_rev_full;
    uint8_t  reserved[4];
    uint8_t  hash_appended;     /*!< 尾部是否附有 SHA256 */
} iap_esp_image_header_t;

/* 段头 (8 字节) */
typedef struct __attribute__((packed)) {
    uint32_t load_addr;         /*!< 目标地址 */
    uint32_t data_len;          /*!< 数据长度 */
} iap_esp_image_segment_t;

#define IAP_ESP_IMAGE_MAX_SEGMENTS  16
#define IAP_ESP_IMAGE_HASH_LEN      32
#define IAP_ESP_IMAGE_CHECKSUM_LEN  1

/* 逐段拷贝用的分片缓冲 (静态, 避免占用任务栈) */
#define IAP_JUMP_CHUNK_SIZE         4096
static uint8_t s_jump_chunk[IAP_JUMP_CHUNK_SIZE];

/**
 * @brief 校验镜像尾部 checksum (ESP 镜像格式的简单校验)
 *
 * 镜像末尾 1 字节 = (0xEF ^ 所有数据字节之和) & 0xFF
 *
 * @param part     分区
 * @param img_len  镜像长度 (不含 checksum 字节)
 * @return true 通过
 */
static bool verify_image_checksum(const esp_partition_t *part, uint32_t img_len)
{
    uint32_t sum = 0xEF;
    uint32_t off = 0;
    uint8_t tail = 0;

    while (off < img_len) {
        uint32_t n = img_len - off;
        if (n > sizeof(s_jump_chunk)) {
            n = sizeof(s_jump_chunk);
        }
        if (esp_partition_read(part, off, s_jump_chunk, n) != ESP_OK) {
            return false;
        }
        for (uint32_t i = 0; i < n; i++) {
            sum += s_jump_chunk[i];
        }
        off += n;
    }

    if (esp_partition_read(part, img_len, &tail, 1) != ESP_OK) {
        return false;
    }
    return (((sum & 0xFF) ^ 0xFF) == tail);
}

/**
 * @brief 校验镜像尾部 SHA256 (若存在)
 *
 * ESP 镜像可在末尾附加 32 字节 SHA256 (hash_appended=1)。
 * 用 ROM 的 SHA256 硬件加速计算整个镜像的摘要并比对。
 *
 * 【跨目标兼容性】
 *
 * ROM 的 ets_sha_* API 在不同目标上签名不同:
 *
 *   ESP32 (老 ROM):
 *     void        ets_sha_init(SHA_CTX *ctx);
 *     void        ets_sha_update(SHA_CTX *ctx, SHA_TYPE type, const uint8_t *in, size_t bits);
 *     void        ets_sha_finish(SHA_CTX *ctx, SHA_TYPE type, uint8_t *out);
 *     (无 ets_sha_starts; type 在 update/finish 时传入; 长度单位是 **bit**)
 *
 *   ESP32-S2/S3/C3/C5/C6/H2 (新 ROM):
 *     ets_status_t ets_sha_init(SHA_CTX *ctx, SHA_TYPE type);
 *     ets_status_t ets_sha_starts(SHA_CTX *ctx, uint16_t sha512_t);
 *     void         ets_sha_update(SHA_CTX *ctx, const uint8_t *in, uint32_t bytes, bool update_ctx);
 *     ets_status_t ets_sha_finish(SHA_CTX *ctx, uint8_t *out);
 *     (长度单位是 **byte**)
 *
 * 下面用一组静态内联包装函数统一两种 API。
 *
 * @param part     分区
 * @param img_len  镜像长度 (不含 32 字节摘要)
 * @param expected 期望的摘要 (从镜像尾部读出)
 * @return true 通过
 */

#if defined(CONFIG_IDF_TARGET_ESP32)

/* --- ESP32: 老 ROM API --- */
static inline bool sha_begin(SHA_CTX *ctx)
{
    ets_sha_init(ctx);
    return true;    /* 老 API 无返回值, 视为成功 */
}

static inline void sha_update(SHA_CTX *ctx, const uint8_t *data, uint32_t len)
{
    /* 老 API 长度单位为 bit */
    ets_sha_update(ctx, SHA2_256, data, (size_t)len * 8);
}

static inline bool sha_finish(SHA_CTX *ctx, uint8_t *digest)
{
    ets_sha_finish(ctx, SHA2_256, digest);
    return true;
}

#else

/* --- 新 ROM API (S2/S3/C3/C5/C6/H2) --- */
static inline bool sha_begin(SHA_CTX *ctx)
{
    if (ets_sha_init(ctx, SHA2_256) != ETS_OK) {
        return false;
    }
    return ets_sha_starts(ctx, 0) == ETS_OK;
}

static inline void sha_update(SHA_CTX *ctx, const uint8_t *data, uint32_t len)
{
    ets_sha_update(ctx, data, len, false);
}

static inline bool sha_finish(SHA_CTX *ctx, uint8_t *digest)
{
    return ets_sha_finish(ctx, digest) == ETS_OK;
}

#endif /* CONFIG_IDF_TARGET_ESP32 */

static bool verify_image_sha256(const esp_partition_t *part, uint32_t img_len,
                                const uint8_t *expected)
{
    uint8_t digest[IAP_ESP_IMAGE_HASH_LEN];
    uint32_t off = 0;
    SHA_CTX ctx;

    if (!sha_begin(&ctx)) {
        return false;
    }

    while (off < img_len) {
        uint32_t n = img_len - off;
        if (n > sizeof(s_jump_chunk)) {
            n = sizeof(s_jump_chunk);
        }
        if (esp_partition_read(part, off, s_jump_chunk, n) != ESP_OK) {
            return false;
        }
        sha_update(&ctx, s_jump_chunk, n);
        off += n;
    }

    if (!sha_finish(&ctx, digest)) {
        return false;
    }
    return (memcmp(digest, expected, IAP_ESP_IMAGE_HASH_LEN) == 0);
}

esp_err_t iap_image_verify_slot(uint8_t slot, iap_image_verify_t *out)
{
    iap_image_verify_t local;
    if (out == NULL) {
        out = &local;
    }
    memset(out, 0, sizeof(*out));
    out->reason[0] = '\0';

    const esp_partition_t *part = iap_image_get_slot(slot);
    if (part == NULL) {
        snprintf(out->reason, sizeof(out->reason), "槽 %u 分区不存在", (unsigned)slot);
        return ESP_ERR_NOT_FOUND;
    }

    /* --- 1. 镜像头 --- */
    iap_esp_image_header_t hdr;
    if (esp_partition_read(part, 0, &hdr, sizeof(hdr)) != ESP_OK) {
        snprintf(out->reason, sizeof(out->reason), "读取镜像头失败");
        return ESP_FAIL;
    }
    if (hdr.magic != IAP_ESP_IMAGE_MAGIC) {
        snprintf(out->reason, sizeof(out->reason),
                 "镜像 magic 无效 (0x%02X, 期望 0x%02X)", hdr.magic,
                 IAP_ESP_IMAGE_MAGIC);
        return ESP_OK;
    }
    if (hdr.segment_count == 0 ||
        hdr.segment_count > IAP_ESP_IMAGE_MAX_SEGMENTS) {
        snprintf(out->reason, sizeof(out->reason),
                 "段数量非法 (%u)", (unsigned)hdr.segment_count);
        return ESP_OK;
    }

    out->chip_id       = hdr.chip_id;
    out->entry_addr    = hdr.entry_addr;
    out->segment_count = hdr.segment_count;

    /* --- 2. chip_id --- */
    uint16_t expect_chip = (uint16_t)CONFIG_IDF_FIRMWARE_CHIP_ID;
    if (hdr.chip_id != expect_chip) {
        snprintf(out->reason, sizeof(out->reason),
                 "芯片 ID 不匹配 (镜像=%u, 本机=%u)",
                 (unsigned)hdr.chip_id, (unsigned)expect_chip);
        return ESP_OK;
    }

    /* --- 3. 遍历段表 --- */
    uint32_t off = sizeof(hdr);
    for (uint8_t i = 0; i < hdr.segment_count; i++) {
        iap_esp_image_segment_t seg;
        if (esp_partition_read(part, off, &seg, sizeof(seg)) != ESP_OK) {
            snprintf(out->reason, sizeof(out->reason), "读取段 %u 头失败", i);
            return ESP_FAIL;
        }
        off += sizeof(seg);

        /* 段数据必须在分区内 */
        if (seg.data_len == 0 ||
            (uint64_t)off + seg.data_len > part->size) {
            snprintf(out->reason, sizeof(out->reason),
                     "段 %u 越界 (off=0x%X len=0x%X)", i,
                     (unsigned)off, (unsigned)seg.data_len);
            return ESP_OK;
        }

        /* 段目标地址不能覆盖 IAP 自身 (flash 映射区 0x42000000 起为 IROM) */
        if (seg.load_addr >= 0x3F000000 && seg.load_addr < 0x40000000) {
            snprintf(out->reason, sizeof(out->reason),
                     "段 %u 目标地址非法 (0x%08X)", i, (unsigned)seg.load_addr);
            return ESP_OK;
        }

        off += seg.data_len;
    }

    /* --- 4. 尾部校验 --- */
    uint32_t img_len = off;                 /* 段数据结束位置 */

    if (hdr.hash_appended) {
        /* 有 SHA256: [镜像][32 字节 SHA256] */
        uint8_t expected[IAP_ESP_IMAGE_HASH_LEN];
        if ((uint64_t)img_len + IAP_ESP_IMAGE_HASH_LEN > part->size) {
            snprintf(out->reason, sizeof(out->reason), "SHA256 位置越界");
            return ESP_OK;
        }
        if (esp_partition_read(part, img_len, expected,
                               IAP_ESP_IMAGE_HASH_LEN) != ESP_OK) {
            snprintf(out->reason, sizeof(out->reason), "读取 SHA256 失败");
            return ESP_FAIL;
        }
        if (!verify_image_sha256(part, img_len, expected)) {
            snprintf(out->reason, sizeof(out->reason), "SHA256 校验失败");
            return ESP_OK;
        }
        out->image_size = img_len + IAP_ESP_IMAGE_HASH_LEN;
    } else {
        /* 无 SHA256: 用 checksum 字节 */
        if ((uint64_t)img_len + IAP_ESP_IMAGE_CHECKSUM_LEN > part->size) {
            snprintf(out->reason, sizeof(out->reason), "checksum 位置越界");
            return ESP_OK;
        }
        if (!verify_image_checksum(part, img_len)) {
            snprintf(out->reason, sizeof(out->reason), "checksum 校验失败");
            return ESP_OK;
        }
        out->image_size = img_len + IAP_ESP_IMAGE_CHECKSUM_LEN;
    }

    out->ok = true;
    snprintf(out->reason, sizeof(out->reason),
             "校验通过 (chip_id=%u, %u 段, %u 字节%s)",
             (unsigned)hdr.chip_id, (unsigned)hdr.segment_count,
             (unsigned)out->image_size,
             hdr.hash_appended ? ", SHA256" : ", checksum");
    return ESP_OK;
}

esp_err_t iap_image_boot_slot_direct(uint8_t slot)
{
    char reason[160];

    const esp_partition_t *part = iap_image_get_slot(slot);
    if (part == NULL) {
        snprintf(reason, sizeof(reason), "OTA 槽 %u 不存在", (unsigned)slot);
        ESP_LOGE(TAG, "%s", reason);
        return boot_fallback_iap(reason);
    }

    /* --- 1. 完整校验 --- */
    iap_image_verify_t v;
    esp_err_t err = iap_image_verify_slot(slot, &v);
    if (err != ESP_OK) {
        snprintf(reason, sizeof(reason), "校验流程失败 (%s)", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", reason);
        return boot_fallback_iap(reason);
    }
    if (!v.ok) {
        snprintf(reason, sizeof(reason), "镜像校验失败: %s", v.reason);
        ESP_LOGE(TAG, "========================================");
        ESP_LOGE(TAG, " OTA 槽 %u 镜像校验失败", (unsigned)slot);
        ESP_LOGE(TAG, "   %s", v.reason);
        ESP_LOGE(TAG, "========================================");
        return boot_fallback_iap(reason);
    }
    ESP_LOGI(TAG, "OTA 槽 %u %s", (unsigned)slot, v.reason);

    /* --- 2. 读取镜像头 (跳转需要 entry_addr / segment_count) --- */
    iap_esp_image_header_t hdr;
    if (esp_partition_read(part, 0, &hdr, sizeof(hdr)) != ESP_OK) {
        snprintf(reason, sizeof(reason), "读取镜像头失败");
        return boot_fallback_iap(reason);
    }

    /*
     * --- 3. 释放 IAP 占用的资源 ---
     *
     * 必须在跳转前完成, 否则用户程序无法重新初始化这些外设:
     *   - HTTP 服务
     *   - WiFi 网卡 (WIFI_AP_DEF 同名冲突 -> 用户程序建不了自己的 AP)
     *
     * 注意: UART/USB 终端与 I2C 从机不在此处停止 —— 它们不占用
     *       用户程序需要的独占资源, 且停止后无法恢复日志输出。
     */
    iap_http_stop();
    iap_wifi_stop();

    ESP_LOGW(TAG, "========================================");
    ESP_LOGW(TAG, " 直接跳转到用户程序 (不写 otadata)");
    ESP_LOGW(TAG, "   槽     : %u", (unsigned)slot);
    ESP_LOGW(TAG, "   入口   : 0x%08" PRIX32, hdr.entry_addr);
    ESP_LOGW(TAG, "   段数量 : %u", (unsigned)hdr.segment_count);
    ESP_LOGW(TAG, "   下次上电仍会先运行 IAP");
    ESP_LOGW(TAG, "========================================");

    /* 给日志输出留时间 */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* --- 4. 逐段拷贝到 load_addr --- */
    uint32_t off = sizeof(hdr);
    for (uint8_t i = 0; i < hdr.segment_count; i++) {
        iap_esp_image_segment_t seg;
        esp_partition_read(part, off, &seg, sizeof(seg));
        off += sizeof(seg);

        uint32_t dst = seg.load_addr;
        uint32_t remain = seg.data_len;
        uint32_t src = off;

        while (remain > 0) {
            uint32_t n = (remain > sizeof(s_jump_chunk))
                       ? sizeof(s_jump_chunk) : remain;
            esp_partition_read(part, src, s_jump_chunk, n);
            memcpy((void *)dst, s_jump_chunk, n);
            src    += n;
            dst    += n;
            remain -= n;
        }
        off += seg.data_len;
    }

    /* --- 5. 跳转 (不返回) --- */
    typedef void (*iap_entry_t)(void);
    iap_entry_t entry = (iap_entry_t)hdr.entry_addr;

    ESP_LOGW(TAG, "跳转中...");

    /* 关闭中断, 避免跳转后 IAP 的中断处理干扰用户程序 */
    portDISABLE_INTERRUPTS();
    entry();

    /* 不会执行到这里 */
    return ESP_OK;
}

esp_err_t iap_image_boot_user_app(void)
{
    return iap_image_boot_slot(s_active_slot);
}

esp_err_t iap_image_boot_slot(uint8_t slot)
{
    char reason[80];

    const esp_partition_t *p = iap_image_get_slot(slot);
    if (p == NULL) {
        snprintf(reason, sizeof(reason), "OTA 槽 %u 不存在", (unsigned)slot);
        ESP_LOGE(TAG, "%s", reason);
        return boot_fallback_iap(reason);
    }

    if (!iap_image_slot_present(slot)) {
        snprintf(reason, sizeof(reason),
                 "OTA 槽 %u 无镜像 (首字节非 0xE9)", (unsigned)slot);
        ESP_LOGE(TAG, "%s", reason);
        return boot_fallback_iap(reason);
    }

    /*
     * v5: 改用 RTC RAM 传递启动决策 (不再用 esp_ota_set_boot_partition)。
     *
     * 写入 iap_boot_param_t 后 esp_restart(), bootloader 读 RTC RAM
     * 得知应加载 user_addr 处的镜像 —— 全程不读分区表。
     *
     * v7: 加载地址/大小**直接来自配置区** (slot_addr / slot_size)，
     *     find_ota_slot() 已把它们填入 esp_partition_t 句柄，
     *     这里无需再做任何覆盖判断。
     */
    uint32_t load_addr = p->address;
    uint32_t load_size = p->size;

    iap_param_set_boot_app(load_addr, load_size, 0, IAP_CFG_ADDR, 0);

    /*
     * v5: 把配置区的启动参数字符串复制到 RTC RAM。
     *
     * 用户程序**不允许访问配置区** (0x8000 在 IDF 写保护区内)，
     * 因此 IAP 在跳转前把 boot_param 放进 RTC RAM，用户程序通过
     * iap_user_get_boot_param() 读取。
     */
    {
        iap_cfg_data_t cfg;
        if (iap_config_get(&cfg) == ESP_OK &&
            (cfg.boot_param_flags & IAP_CFG_BOOT_PARAM_FLAG_VALID) &&
            cfg.boot_param_len > 0) {
            iap_param_set_string((const char *)cfg.boot_param, cfg.boot_param_len);
            ESP_LOGI(TAG, "启动参数已传入 RTC RAM: %s", (const char *)cfg.boot_param);
        } else {
            iap_param_set_string(NULL, 0);
        }
    }

    /*
     * 跳转前释放 IAP 占用的网络资源。
     *
     * 关键: IAP 通过 esp_netif_create_default_wifi_ap() 创建了名为
     * "WIFI_AP_DEF" 的网卡。若不销毁，用户程序再调用同一 API 会因
     * 同名网卡已存在而返回 NULL，导致**用户程序无法建立自己的 WiFi**。
     *
     * 同时停止 HTTP 服务并释放事件循环/网络栈，让用户程序从干净状态起步。
     */
    iap_http_stop();
    iap_wifi_stop();

    ESP_LOGI(TAG, "即将启动 OTA 槽 %u 的用户程序 @ 0x%08" PRIx32 "，重启中...",
             (unsigned)slot, p->address);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return ESP_OK;  /* 不会执行到这里 */
}

esp_err_t iap_image_boot_iap(void)
{
    /*
     * v7: 不再清除下载标志。
     *
     * 旧实现先调 iap_config_set_download_mode(false)，导致 `iap boot`
     * 命令重启后 IAP 看到"下载模式=否"，等 3 秒无触发又跳回用户程序 ——
     * 命令形同虚设。
     *
     * 正确做法: 只写 RTC RAM boot_target = IAP。bootloader 会按它启动
     * IAP，IAP 启动时检测到该标志便**直接停在终端**，不再等待/启动 APP。
     * 这样既满足"iap boot 留在 IAP"，也不会死循环 (下次正常上电时
     * RTC RAM 无效，boot_target 自然不再是 IAP)。
     */
    iap_param_set_boot_iap();

    ESP_LOGI(TAG, "即将重启进入 IAP @ 0x%08X，重启中...", (unsigned)IAP_APP_ADDR);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return ESP_OK;  /* 不会执行到这里 */
}

esp_err_t iap_image_request_iap(iap_boot_reason_t reason)
{
    iap_config_set_download_mode(true);
    iap_config_set_boot_reason(reason);
    return iap_image_boot_iap();
}

/* -------------------------------------------------------------------------- */
/* 强制进入 IAP —— 已改为 GPIO 电平检测 (v5)                                    */
/*                                                                            */
/* v4 及以前: flash iap_mark 标志区 + 魔术标记                                  */
/* v5 起已取消, 替代方案 (详见 docs/FINAL-REPORT.md §3.3):                      */
/*   A. GPIO0 电平检测  —— iap_wait_trigger.c (上电按住 BOOT 键)                */
/*   B. UART 命令       —— 等待窗口内发 "iap" 字符串                            */
/*   C. 用户程序主动请求 —— iap_param_set_boot_iap() → esp_restart()            */
/*   D. 断电重上电      —— 冷启动 RTC RAM 无效 → 默认进 IAP                     */
/* -------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------- */
/* 信息查询                                                                    */
/* -------------------------------------------------------------------------- */

esp_err_t iap_image_get_info(iap_image_info_t *info)
{
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(info, 0, sizeof(*info));

    /* 存在性: 读分区首字节是否为 ESP 镜像 magic */
    info->present = iap_image_user_app_present();

    /* 元数据来自配置区 (由用户程序启动后写入) */
    iap_cfg_data_t cfg;
    if (iap_config_get(&cfg) == ESP_OK) {
        info->app_version  = cfg.user_app_version;
        info->image_size   = cfg.user_app_size;
        info->image_crc32  = cfg.user_app_crc32;
    }

    return ESP_OK;
}
