/*
 * ESP IAP - 公共定义
 *
 * 本文件定义 IAP 各模块共享的常量、数据结构与枚举。
 *
 * ============================================================================
 *  闪存布局 (v7 双分区表方案)
 * ============================================================================
 *
 *  固定区 (0x0 ~ 0x13FFFF, 1280KB) —— 全硬编码
 *   +------------------+ 0x000000
 *   |  bootloader      |  32KB (含配置区, 烧录文件 1)
 *   +------------------+ 0x008000
 *   |  iap_cfg         |  8KB  <- 硬编码, 不在任何分区表
 *   +------------------+ 0x00A000
 *   |  (保留)          |  4KB  (原 iap_mark 标志区, 已取消)
 *   +------------------+ 0x00B000
 *   |  分区表 A        |  4KB  <- CONFIG_PARTITION_TABLE_OFFSET
 *   +------------------+ 0x00C000
 *   |  nvs             |  12KB (表 A, 名字必须是 "nvs")
 *   +------------------+ 0x00F000
 *   |  (保留)          |  4KB
 *   +------------------+ 0x010000
 *   |  iap (factory)   |  1216KB (表 A, IAP 程序区)
 *   +------------------+ 0x140000
 *
 *  可变区 (0x140000 ~ 末尾) —— 分区表 B 定义
 *   +------------------+ 0x140000
 *   |  分区表 B        |  4KB  <- APP 的 CONFIG_PARTITION_TABLE_OFFSET
 *   +------------------+ 0x141000
 *   |  nvs             |  12KB (表 B)
 *   +------------------+ 0x144000
 *   |  (保留空隙)      |  48KB (使 user_app 64KB 对齐)
 *   +------------------+ 0x150000
 *   |  user_app (ota0) |  剩余 (表 B, 用户程序区)
 *   +------------------+ 0x400000 (4MB flash)
 *
 * v7 变更 (相对 v5):
 *   固定区 2MB → 1280KB，省下的 768KB 全部给用户程序。
 *   IAP 实际占用 ~998KB (C6)，1216KB 分区留有余量。
 *
 * 启动决策与参数传递:
 *   IAP 写 RTC RAM (rtc_retain_mem_t.custom[]) → esp_restart()
 *   bootloader 读 RTC RAM → 决定启动 IAP 还是 APP (不读 flash)
 *   APP 读 RTC RAM → 拿启动参数
 *   详见 main/iap_boot_param.h
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * 固定区地址常量 (硬编码, 与分区表无关)
 * ========================================================================== */

/** IAP 配置区地址 (8KB, 双槽冗余) */
#define IAP_CFG_ADDR          0x008000

/** IAP 程序区地址 (硬编码, bootloader 直接按此跳转) */
#define IAP_APP_ADDR          0x010000

/** IAP 程序区最大大小 (0x140000 - 0x10000 = 0x130000 = 1216KB) */
#define IAP_APP_MAX_SIZE      0x130000

/** 分区表 A 地址 (IAP 使用) */
#define IAP_PTABLE_A_ADDR     0x00B000

/** 分区表 B 地址 (APP 使用, 位于可变区起点) */
#define IAP_PTABLE_B_ADDR     0x140000

/** nvs 地址 (APP 使用) */
#define IAP_NVS_APP_ADDR      0x141000

/** 用户程序区地址 (APP 加载地址, 64KB 对齐) */
#define IAP_USER_APP_ADDR     0x150000

/** 可变区起点 (固定区结束) */
#define IAP_FIXED_REGION_END  0x140000

/* ============================================================================
 * 分区标签
 * ========================================================================== */

#define IAP_PARTITION_LABEL_CFG       "iap_cfg"    /*!< IAP 配置区 (硬编码, 不在表) */
#define IAP_PARTITION_LABEL_IAP       "iap"        /*!< IAP 程序区 (表 A) */
#define IAP_PARTITION_LABEL_USER_APP  "user_app"   /*!< 用户程序区 (表 B, OTA 槽 0) */
#define IAP_PARTITION_LABEL_NVS_IAP   "nvs"        /*!< IAP 的 NVS (表 A)
                                                         *   ⚠️ 必须叫 "nvs"：WiFi 协议栈内部
                                                         *   调用 nvs_flash_init() 查找该名字。 */
#define IAP_PARTITION_LABEL_NVS_APP   "nvs"        /*!< APP 的 NVS (表 B)
                                                         *   ⚠️ 同样必须叫 "nvs"：WiFi 协议栈
                                                         *   内部调用 nvs_flash_init() 查找它。 */

/* 自定义分区类型/子类型 (见 partitions_iap.csv)
 *
 * 注意: v5 起 iap_cfg 不再作为分区存在 (改为硬编码地址),
 *       但保留这些定义供旧分区表兼容检测使用。 */
#define IAP_PARTITION_TYPE_CFG        0x40
#define IAP_PARTITION_SUBTYPE_CFG     0x00

/* ============================================================================
 * bootloader 字段一致性检查
 *
 * 背景:
 *   ESP-IDF 的 bootloader 中固化了若干**编译期配置**:
 *     - 分区表偏移 (CONFIG_PARTITION_TABLE_OFFSET)
 *     - app 镜像格式与校验规则 (IDF 版本决定)
 *     - flash 模式/容量 (CONFIG_ESPTOOLPY_*)
 *
 *   若只烧 app (0x20000) 而设备上是用**不同配置编译的 bootloader**，
 *   两者字段会不一致。
 *
 *   实测结论 (2026-09-22 受控实验):
 *     app 与 bootloader 的 flash 容量字段不一致**不会导致启动失败**
 *     —— ESP-IDF bootloader 会在运行时探测实际容量并容忍该差异。
 *     因此本检查仅作**排查参考**，用于确认设备上的 bootloader
 *     是何时、用何种配置构建的，而非启动阻断条件。
 *
 * 检查策略:
 *   读取 flash 0x0 处 bootloader 镜像头，比对以下**关键字段**:
 *     1. magic == 0xE9 (bootloader 存在)
 *     2. chip_id 与本固件一致
 *     3. spi_mode / spi_size 与本固件一致
 *
 *   注意:
 *     - **不比对 spi_speed**: bootloader 该字段常为 0 (40MHz)，
 *       实际频率在运行时由 esp_flash_init() 按配置设置，比对会误报。
 *     - **不比对编译时间**: bootloader 内嵌编译日期
 *       (CONFIG_BOOTLOADER_COMPILE_TIME_DATE=y) 每次构建都不同，
 *       但不影响功能，比对会导致误报。
 * ========================================================================== */

/* bootloader 镜像头在 flash 中的偏移 (固定 0x0) */
#define IAP_BOOTLOADER_OFFSET         0x0000

/* ESP 镜像头长度 (与 bootloader 一致) */
#define IAP_IMAGE_HEADER_SIZE         24

/* ESP 镜像 magic */
#define IAP_IMAGE_MAGIC               0xE9

/* 本固件编译时使用的 flash 配置 (由 IDF 宏提供) */
#ifndef CONFIG_ESPTOOLPY_FLASHMODE
#define CONFIG_ESPTOOLPY_FLASHMODE    "dio"
#endif
#ifndef CONFIG_ESPTOOLPY_FLASHFREQ
#define CONFIG_ESPTOOLPY_FLASHFREQ    "80m"
#endif
#ifndef CONFIG_ESPTOOLPY_FLASHSIZE
#define CONFIG_ESPTOOLPY_FLASHSIZE    "4MB"
#endif

/* ============================================================================
 * 多 OTA 槽 (方案 B: 多应用并存)
 *
 * ESP-IDF 支持 app 类型的 ota_0 ~ ota_15 共 16 个子类型 (0x10 ~ 0x1F)。
 * 本工程取前 8 个 (ota_0 ~ ota_7)，在配置区里为每槽保存加载地址与大小。
 *
 * IAP **不解析分区表** —— 启动时只读配置区的 slot_addr[active_slot]，
 * 直接跳转。分区表 B 的布局完全由用户程序工程决定，IAP 不关心。
 *
 * 槽 0 的标签约定为 "user_app"，槽 1~7 为 "user_app_1" ~ "user_app_7"
 * (仅用于用户程序工程侧的命名参考，IAP 不依赖)。
 * ========================================================================== */

/** OTA 槽最大数量 (对应 ota_0 ~ ota_7) */
#define IAP_OTA_SLOT_MAX              8

/** OTA 槽 0 的子类型 (ota_0) */
#define IAP_OTA_SUBTYPE_BASE          0x10

/** GPIO 选择 OTA 槽: 禁用值 */
#define IAP_OTA_GPIO_DISABLED         0xFF

/* ============================================================================
 * 版本信息
 * ========================================================================== */

#define IAP_VERSION_MAJOR             1
#define IAP_VERSION_MINOR             0
#define IAP_VERSION_PATCH             0

#define IAP_STR_HELPER(x)             #x
#define IAP_STR(x)                    IAP_STR_HELPER(x)
#define IAP_VERSION_STRING            IAP_STR(IAP_VERSION_MAJOR) "." \
                                      IAP_STR(IAP_VERSION_MINOR) "." \
                                      IAP_STR(IAP_VERSION_PATCH)

/* ============================================================================
 * IAP 配置区布局
 *
 * 配置区位于独立分区 iap_cfg，长度 4096 字节。
 * 采用双份冗余 (slot A / slot B) + 序号 + CRC 的方式保证掉电安全:
 *   - 写入时总是写序号更大的那个槽，并更新序号
 *   - 读取时选择 CRC 正确且序号最大的槽
 * ========================================================================== */

#define IAP_CFG_MAGIC                 0x49415043U  /*!< "IAPC" */
#define IAP_CFG_VERSION               0x0001
#define IAP_CFG_SLOT_SIZE             4096          /*!< 单个配置槽大小 (须为 flash 扇区 4096 的整数倍) */
#define IAP_CFG_SLOT_COUNT            2             /*!< 冗余槽数量 */

/* ============================================================================
 * 配置数据版本 (用于跨版本迁移)
 *
 * 【与 IAP_CFG_VERSION 的区别】
 *
 *   IAP_CFG_VERSION   —— 配置槽**头部格式**版本 (magic/version/seq/crc 的布局)
 *   IAP_CFG_DATA_VER  —— 配置**数据内容**版本 (iap_cfg_data_t 的字段语义)
 *
 * 头部格式不变但数据字段增删时，只需递增 IAP_CFG_DATA_VER。
 * 旧固件写入的配置 (data_ver 较小) 会被新固件**自动迁移**:
 *   1. 以默认配置为基准
 *   2. 逐项合并旧配置 (已删除的项忽略，冲突项修正为合理值)
 *   3. 更新 data_ver 后写回
 *
 * 迁移历史:
 *   0x0001  初版 (无 data_ver 字段，视为 0x0000)
 *   0x0002  新增多 OTA 槽 (active_slot / ota_slot_count / ota_gpio)
 *   0x0003  新增槽加载地址/大小 (slot_addr / slot_size)，IAP 不再解析分区表
 * ========================================================================== */

#define IAP_CFG_DATA_VER              0x0003        /*!< 当前配置数据版本 */
#define IAP_CFG_DATA_VER_LEGACY       0x0000        /*!< 旧版无 data_ver 字段 */

/* 配置标志位 */
#define IAP_CFG_FLAG_DOWNLOAD_MODE    (1U << 0)  /*!< 置位则强制进入 IAP 下载模式 */
#define IAP_CFG_FLAG_WIFI_ENABLE      (1U << 1)  /*!< 使能 WiFi AP */
#define IAP_CFG_FLAG_I2C_ENABLE       (1U << 2)  /*!< 使能 I2C 从机 */
#define IAP_CFG_FLAG_UART_ENABLE      (1U << 3)  /*!< 使能 UART 终端 */
/*
 * bit4: 启动前要求用户程序镜像合法。
 *
 * 注意: 镜像合法性实际由 **bootloader** 在启动时自校验
 * (magic / 段表 / SHA256 / chip_id)，IAP 侧不做重复校验。
 * 该校验不可关闭 —— 关闭后 bootloader 仍会校验并在失败时回落 IAP。
 * 本标志仅作为"是否允许尝试启动用户程序"的意图记录。
 */
#define IAP_CFG_FLAG_VERIFY_USER_APP  (1U << 4)

/* 等待期间进入 IAP 下载模式的触发源开关 */
#define IAP_CFG_FLAG_WAIT_GPIO_TRIG   (1U << 5)  /*!< 使能 GPIO 触发: 等待期间引脚为有效电平则进入 IAP */
#define IAP_CFG_FLAG_WAIT_I2C_TRIG    (1U << 6)  /*!< 使能 I2C 触发: 等待期间收到 I2C 进入命令则进入 IAP */
#define IAP_CFG_FLAG_WAIT_UART_TRIG   (1U << 7)  /*!< 使能 UART 触发: 等待期间收到 UART 进入命令则进入 IAP */
#define IAP_CFG_FLAG_WAIT_WIFI_TRIG   (1U << 8)  /*!< 使能 WiFi 触发: 等待期间有客户端接入则进入 IAP */

/* USB 相关标志位 */
#define IAP_CFG_FLAG_USB_ENABLE       (1U << 9)  /*!< 使能 USB 串口 (终端 + 烧录) */
#define IAP_CFG_FLAG_WAIT_USB_TRIG    (1U << 10) /*!< 使能 USB 触发: 等待期间收到 USB 进入命令则进入 IAP */

/* 启动参数标志位 */
#define IAP_CFG_BOOT_PARAM_FLAG_VALID     (1U << 0)  /*!< 启动参数有效 (已由 IAP 写入) */
#define IAP_CFG_BOOT_PARAM_FLAG_CONSUMED  (1U << 1)  /*!< 已被用户程序读取 (可用于单次生效语义) */

/* 触发引脚配置 */
#define IAP_CFG_GPIO_TRIG_ACTIVE_LOW   0         /*!< 低电平有效 (启用内部上拉) */
#define IAP_CFG_GPIO_TRIG_ACTIVE_HIGH  1         /*!< 高电平有效 (启用内部下拉) */

/* ============================================================================
 * USB 支持
 *
 * ESP32 系列的 USB 能力分两类:
 *   - USB-Serial-JTAG (内置 USB PHY): C3 / C6 / H2 / S3
 *       固定接芯片的 D+/D- 引脚，可配置为 CDC 串口
 *   - USB-OTG (需外部 PHY 或内置): S2 / S3 / P4 / H4
 *       需外接 USB PHY，本工程不使用
 *
 * 本工程使用 USB-Serial-JTAG 的 CDC 模式，提供与 UART 等价的终端与烧录能力。
 * ========================================================================== */

#if defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32C6) || \
    defined(CONFIG_IDF_TARGET_ESP32H2) || defined(CONFIG_IDF_TARGET_ESP32S3) || \
    defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C61)
#define IAP_HAS_USB_SERIAL_JTAG   1
#else
#define IAP_HAS_USB_SERIAL_JTAG   0
#endif

/** 该芯片是否支持 USB 串口 (终端 + 烧录) */
#define IAP_USB_SUPPORTED         IAP_HAS_USB_SERIAL_JTAG

/* 默认值 */
#define IAP_CFG_DEFAULT_WIFI_SSID     "ESP-IAP"
#define IAP_CFG_DEFAULT_WIFI_PASS     "12345678"
#define IAP_CFG_DEFAULT_WIFI_CHANNEL  1
#define IAP_CFG_DEFAULT_IP_A          192
#define IAP_CFG_DEFAULT_IP_B          168
#define IAP_CFG_DEFAULT_IP_C          4
#define IAP_CFG_DEFAULT_IP_D          1
#define IAP_CFG_DEFAULT_NETMASK_A     255
#define IAP_CFG_DEFAULT_NETMASK_B     255
#define IAP_CFG_DEFAULT_NETMASK_C     255
#define IAP_CFG_DEFAULT_NETMASK_D     0
#define IAP_CFG_DEFAULT_I2C_SCL       7
#define IAP_CFG_DEFAULT_I2C_SDA       6
#define IAP_CFG_DEFAULT_I2C_ADDR      0x42
#define IAP_CFG_DEFAULT_WAIT_SEC      3
#define IAP_CFG_DEFAULT_UART_PORT     0
#define IAP_CFG_DEFAULT_UART_BAUD     115200
/*
 * UART 引脚: 0xFF 表示"使用该端口的芯片默认引脚"。
 * UART0 的默认引脚即 ESP 系列默认日志输出口
 * (ESP32/S2/S3 = TX GPIO1 / RX GPIO3; ESP32-C2/C3/C6/H2 = TX GPIO21 / RX GPIO20)。
 */
#define IAP_CFG_UART_PIN_DEFAULT      0xFF

/*
 * USB 串口默认使能 (仅支持 USB 的芯片)。
 * 支持 USB-Serial-JTAG 的芯片 (C3/C6/H2/S3) 默认开启 USB 终端与触发。
 */
#if IAP_USB_SUPPORTED
#define IAP_CFG_DEFAULT_FLAGS_USB     (IAP_CFG_FLAG_USB_ENABLE | \
                                       IAP_CFG_FLAG_WAIT_USB_TRIG)
#else
#define IAP_CFG_DEFAULT_FLAGS_USB     0
#endif
/*
 * 默认触发引脚: GPIO0 (ESP32-C6 的 BOOT 按键)。
 *
 * 上电时按住 BOOT 键 → 等待窗口内检测到低电平 → 进入 IAP 下载模式。
 * 这是"不用断电、不用设备能跑"的救援入口 (详见 docs/FINAL-REPORT.md §3.3)。
 *
 * 注意: 检测为**纯电平检测**, 不做长按计时。
 *       0xFF 表示禁用 GPIO 触发。
 */
#define IAP_CFG_DEFAULT_TRIG_GPIO     0     /*!< 触发引脚: GPIO0 (BOOT 键) */
#define IAP_CFG_DEFAULT_TRIG_LEVEL    IAP_CFG_GPIO_TRIG_ACTIVE_LOW

/* ============================================================================
 * 启动参数字符串
 *
 * 用于 IAP 与用户程序之间传递初始化参数。IAP 在启动用户程序前写入，
 * 用户程序启动后读取该字符串完成初始化。
 *
 * 字符串以 '\0' 结尾，长度上限为 IAP_CFG_BOOT_PARAM_SIZE - 1 个字符。
 * 约定格式 (由使用者自行定义，以下为建议格式):
 *
 *   key1=value1;key2=value2;...
 *
 * 示例:
 *   "mode=normal;baud=115200;ssid=MyAP;server=192.168.4.100:8080"
 *
 * IAP 内置的保留键 (供参考，用户程序可自行扩展):
 *   mode      运行模式 (normal / factory / debug)
 *   reason    进入用户程序的原因 (对应 iap_boot_reason_t)
 *   boot      启动计数
 * ========================================================================== */

/** 启动参数字符串缓冲区长度 (含结尾 '\0') */
#define IAP_CFG_BOOT_PARAM_SIZE       256

/** 启动参数字符串最大可写字符数 (不含结尾 '\0') */
#define IAP_CFG_BOOT_PARAM_MAX_LEN    (IAP_CFG_BOOT_PARAM_SIZE - 1)

/** 启动参数字符串默认值 (空字符串表示未设置) */
#define IAP_CFG_DEFAULT_BOOT_PARAM    ""

/**
 * @brief IAP 配置内容 (不含头部)
 *
 * 该结构体在配置槽内紧跟在 iap_cfg_header_t 之后。
 * 字段全部使用固定宽度类型，保证跨编译器和跨芯片一致。
 */
typedef struct __attribute__((packed)) {
    uint32_t flags;                     /*!< IAP_CFG_FLAG_* 位掩码 */

    /* --- 启动等待 --- */
    uint16_t wait_seconds;              /*!< 进入 IAP 后等待秒数，0 表示不等待 */
    /*
     * 保留字段 (原 boot_fail_count)。
     *
     * 旧版用于「连续启动失败计数」防砖，现已移除 —— IAP 不再对用户程序
     * 做任何启动重试限制，启动决策完全由配置区与 RTC RAM 控制。
     *
     * 保留 2 字节以维持 iap_cfg_data_t 的 612 字节布局不变，
     * 避免破坏已烧录设备的配置区兼容性。
     */
    uint16_t boot_fail_reserved;

    /* --- 用户程序信息 (由 IAP 或用户程序更新) --- */
    uint32_t user_app_size;             /*!< 用户程序镜像长度 (字节)，0 表示未知 */
    uint32_t user_app_crc32;            /*!< 用户程序镜像 CRC32 (由用户程序上报) */
    uint32_t user_app_version;          /*!< 用户程序版本号 */
    uint32_t boot_count;                /*!< 用户程序启动计数 */
    uint32_t last_boot_reason;          /*!< 上次进入 IAP 的原因 */

    /* --- I2C 从机配置 --- */
    uint8_t  i2c_scl_gpio;              /*!< I2C SCL 引脚 */
    uint8_t  i2c_sda_gpio;              /*!< I2C SDA 引脚 */
    uint8_t  i2c_addr;                  /*!< I2C 从机地址 (7bit) */
    uint8_t  i2c_reserved;
    /*
     * 原 i2c_freq_hz (总线频率) 已废弃。
     * I2C 从机模式下时钟完全由主机产生 (主机驱动 SCL，从机只做采样)，
     * i2c_slave_config_t 中也没有频率字段，该配置项从未生效。
     * 此处保留 4 字节占位，以维持 iap_cfg_data_t 的 492 字节布局不变，
     * 避免破坏已烧录设备的配置区兼容性。
     */
    uint32_t i2c_reserved_freq;
    uint32_t i2c_reserved1[2];

    /* --- WiFi AP 配置 --- */
    uint8_t  wifi_ssid[32];             /*!< AP SSID (null 结尾) */
    uint8_t  wifi_password[64];         /*!< AP 密码 (null 结尾) */
    uint8_t  wifi_channel;              /*!< AP 信道 */
    uint8_t  wifi_reserved[3];
    uint32_t wifi_ip;                   /*!< AP IPv4 地址，小端 (a|b<<8|c<<16|d<<24) */
    uint32_t wifi_netmask;              /*!< AP 子网掩码，小端 */
    uint32_t wifi_reserved1[2];

    /* --- UART 终端配置 --- */
    uint8_t  uart_port;                 /*!< UART 端口号 (0/1/2) */
    uint8_t  uart_tx_gpio;              /*!< TX 引脚；0xFF = 用该端口默认引脚 */
    uint8_t  uart_rx_gpio;              /*!< RX 引脚；0xFF = 用该端口默认引脚 */
    uint8_t  uart_reserved;
    uint32_t uart_baudrate;             /*!< UART 波特率 */
    uint32_t uart_reserved1[2];

    /* --- 等待期间触发进入 IAP 的 GPIO 引脚 --- */
    uint8_t  trig_gpio;                 /*!< 触发引脚号 (0xFF 表示未配置) */
    uint8_t  trig_gpio_level;           /*!< 有效电平: 0=低, 1=高 */
    uint8_t  trig_gpio_reserved[2];
    uint32_t trig_gpio_reserved1[2];

    /* --- USB 串口配置 --- */
    uint32_t usb_reserved0;             /*!< 保留 (USB 无需额外参数，波特率由主机决定) */
    uint32_t usb_reserved1[2];

    /* --- 启动参数字符串 (IAP -> 用户程序) --- */
    uint8_t  boot_param[IAP_CFG_BOOT_PARAM_SIZE]; /*!< 启动参数，'\0' 结尾 */
    uint16_t boot_param_len;            /*!< 有效字符数 (不含 '\0') */
    uint8_t  boot_param_flags;          /*!< 启动参数标志，见 IAP_CFG_BOOT_PARAM_FLAG_* */
    uint8_t  boot_param_reserved;
    uint32_t boot_param_crc32;          /*!< boot_param[0..len) 的 CRC32，0 表示未设置 */
    uint32_t boot_param_reserved1[2];

    /* --- 多 OTA 槽 (方案 B: 多应用并存) --- */
    uint8_t  active_slot;               /*!< 当前要启动的 OTA 槽序号 (0=ota_0, 1=ota_1, ...) */
    uint8_t  ota_slot_count;            /*!< 已配置的 OTA 槽数量 (由工具/用户填写) */
    uint8_t  ota_reserved0[2];
    /*
     * GPIO 选择 OTA 槽: 使能后，启动时读取 ota_gpio 引脚的电平组合，
     * 映射为槽序号 (电平值 & (ota_slot_count-1))，覆盖 active_slot。
     */
    uint8_t  ota_gpio;                  /*!< 选择 OTA 槽的 GPIO 编号，0xFF = 禁用 */
    uint8_t  ota_gpio_reserved[3];

    /*
     * ── 槽加载地址 / 大小 (v7 新增) ──────────────────────────────────
     *
     * IAP **不解析任何分区表** —— 它只读这两个数组决定跳到哪、跳多远。
     * 地址与大小由用户在「APP 启动槽」中手动填写 (工具写配置区)。
     *
     *   slot_addr[i] == 0  →  槽 i 无效，不可启动
     *   slot_addr[i] != 0  →  直接跳转到该绝对地址
     *   slot_size[i]       →  该槽可用空间 (字节)，用于 bootloader 边界校验
     *
     * 这样 IAP 与用户程序的分区布局彻底解耦: 分区表 B 长什么样、放在哪，
     * IAP 完全不关心。
     *
     * 用 uint64_t 保存，便于将来扩展到 >4GB 地址空间 (当前 ESP32 为 32 位，
     * 高 32 位恒为 0，写入/读取时按 32 位截断校验)。
     */
    uint64_t slot_addr[IAP_OTA_SLOT_MAX];   /*!< 各槽加载地址 (0 = 无效) */
    uint64_t slot_size[IAP_OTA_SLOT_MAX];   /*!< 各槽可用大小 (字节) */

    /* --- 配置数据版本 (用于跨版本迁移) --- */
    uint16_t data_ver;                  /*!< IAP_CFG_DATA_VER，0 = 旧版无此字段 */
    uint8_t  data_ver_reserved[2];

    /* --- 保留扩展 --- */
    uint8_t  reserved2[0];              /*!< 占位 */
} iap_cfg_data_t;

/*
 * 编译期检查: 配置数据结构必须能放入配置槽。
 *
 * 布局 (总长 612 字节):
 *   flags(4) off=0, wait_seconds(2) off=4, boot_fail_reserved(2) off=6,
 *   user_app_size(4) off=8, user_app_crc32(4) off=12, user_app_version(4) off=16,
 *   boot_count(4) off=20, last_boot_reason(4) off=24,
 *   i2c_scl_gpio(1) off=28, i2c_sda_gpio(1) off=29, i2c_addr(1) off=30,
 *   i2c_reserved(1) off=31, i2c_reserved_freq(4) off=32, i2c_reserved1(8) off=36,
 *   wifi_ssid(32) off=44, wifi_password(64) off=76, wifi_channel(1) off=140,
 *   wifi_reserved(3) off=141, wifi_ip(4) off=144, wifi_netmask(4) off=148,
 *   wifi_reserved1(8) off=152,
 *   uart_port(1) off=160, uart_tx_gpio(1) off=161, uart_rx_gpio(1) off=162,
 *   uart_reserved(1) off=163, uart_baudrate(4) off=164,
 *   uart_reserved1(8) off=168,
 *   trig_gpio(1) off=176, trig_gpio_level(1) off=177, trig_gpio_reserved(2) off=178,
 *   trig_gpio_reserved1(8) off=180,
 *   usb_reserved0(4) off=188, usb_reserved1(8) off=192,
 *   boot_param(256) off=200, boot_param_len(2) off=456, boot_param_flags(1) off=458,
 *   boot_param_reserved(1) off=459, boot_param_crc32(4) off=460,
 *   boot_param_reserved1(8) off=464,
 *   active_slot(1) off=472, ota_slot_count(1) off=473, ota_reserved0(2) off=474,
 *   ota_gpio(1) off=476, ota_gpio_reserved(3) off=477,
 *   slot_addr(64) off=480, slot_size(64) off=544,
 *   data_ver(2) off=608, data_ver_reserved(2) off=610
 *
 * 注意: 用户程序侧**不再镜像**该结构体 (v5 起已移除 iap_user_cfg_t)，
 *       用户程序如需配置请通过 RTC RAM 启动参数传递。
 */
_Static_assert(sizeof(iap_cfg_data_t) == 612,
               "iap_cfg_data_t 布局已变更，请同步更新 tools/gen_factory_cfg.py");

/**
 * @brief 配置槽头部
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;                     /*!< IAP_CFG_MAGIC */
    uint16_t version;                   /*!< IAP_CFG_VERSION */
    uint16_t header_size;               /*!< 头部长度 */
    uint32_t seq;                       /*!< 写入序号，越大越新 */
    uint32_t data_size;                 /*!< iap_cfg_data_t 长度 */
    uint32_t data_crc32;                /*!< iap_cfg_data_t 的 CRC32 */
    uint32_t header_crc32;              /*!< 头部 (不含本字段) 的 CRC32 */
    uint32_t reserved;
} iap_cfg_header_t;

_Static_assert(sizeof(iap_cfg_data_t) <= IAP_CFG_SLOT_SIZE - sizeof(iap_cfg_header_t),
               "iap_cfg_data_t 超出配置槽容量");

/* 进入 IAP 的原因 */
typedef enum {
    IAP_BOOT_REASON_NONE = 0,           /*!< 未知 */
    IAP_BOOT_REASON_DOWNLOAD_FLAG,      /*!< 配置区下载位置位 */
    IAP_BOOT_REASON_WAIT_TIMEOUT,       /*!< 等待超时无操作 */
    IAP_BOOT_REASON_USER_REQUEST,       /*!< 用户程序主动请求进入 IAP */
    IAP_BOOT_REASON_BOOT_FAIL,          /*!< 启动用户程序失败 (跳转未成功) */
    IAP_BOOT_REASON_NO_VALID_APP,       /*!< 用户程序区无有效程序 */
    IAP_BOOT_REASON_FIRST_BOOT,         /*!< 首次上电 */
    IAP_BOOT_REASON_TRIG_GPIO,          /*!< 等待期间 GPIO 引脚电平触发 */
    IAP_BOOT_REASON_TRIG_I2C,           /*!< 等待期间收到 I2C 进入命令 */
    IAP_BOOT_REASON_TRIG_UART,          /*!< 等待期间收到 UART 进入命令 */
    IAP_BOOT_REASON_TRIG_WIFI,          /*!< 等待期间有 WiFi 客户端接入 */
    IAP_BOOT_REASON_TRIG_USB,           /*!< 等待期间收到 USB 进入命令 */
    IAP_BOOT_REASON_CFG_ERROR,          /*!< 配置区错误 (读取/校验失败) */
    IAP_BOOT_REASON_BOOTLOADER_MISMATCH,/*!< bootloader 与本固件字段不一致 (保留) */
} iap_boot_reason_t;

/* ============================================================================
 * 强制进入 IAP —— 已改为 GPIO 电平检测 (v5)
 *
 * 【设计变更说明】
 *
 * v4 及以前: 在 flash 0xA000 建 iap_mark 标志区, esptool / HTML 工具
 *            写入魔术标记, IAP 启动时检测并擦除。
 *
 * v5 起**已取消** flash 标志区, 原因:
 *   1. 每次切换要写 flash (磨损)
 *   2. bootloader 需读 flash + 写 RTC RAM (两步)
 *   3. 与配置区若同扇区有原子性问题
 *
 * 替代方案 (详见 docs/FINAL-REPORT.md §3.3):
 *   A. GPIO0 电平检测  —— IAP 内 iap_wait_trigger.c 已实现
 *      (上电时按住 BOOT 键即可进 IAP)
 *   B. UART 命令       —— 等待窗口内发 "iap" 字符串
 *   C. 用户程序主动请求 —— 写 RTC RAM boot_target=0 → esp_restart()
 *   D. 断电重上电      —— 冷启动 RTC RAM 无效 → 默认进 IAP
 *
 * 因此不再需要: iap_mark 分区 / IAP_MAGIC_MARKER 常量 /
 *               marker UART 命令 / HTML「强制进入 IAP」按钮
 * ========================================================================== */


/* ============================================================================
 * 用户程序镜像 —— 无自定义文件头 + 单一烧录文件 (v6)
 *
 * 【设计变更说明】
 *
 * 早期版本在用户程序前附加 512 字节自定义文件头（magic "USER"），
 * 但该设计存在**阻断性缺陷**:
 *
 *   esp_ota_set_boot_partition() 内部 esp_image_verify() 要求
 *   user_app 分区 offset 0 必须是 ESP 镜像头 (0xE9)。自定义文件头
 *   占据 offset 0 会导致 ESP_ERR_OTA_VALIDATE_FAILED，用户程序
 *   永远无法启动。
 *
 * 现改为**完全依赖 ESP-IDF 原生机制**:
 *
 *   1. user_app 分区内就是**纯 ESP 应用镜像**（无任何附加数据）
 *      - offset 0 = ESP 镜像头 (0xE9) —— bootloader / OTA 直接可用
 *      - 与 flash 容量、分区大小完全无关（无位置敏感数据）
 *
 *   2. 镜像合法性由 **bootloader 自校验**（magic / 段表 / SHA256 / chip_id）
 *      - 校验失败时 bootloader 自动回落到 factory (IAP)，不会变砖
 *
 *   3. 用户程序元数据（版本号）存于**配置区** `user_app_version` 字段
 *
 * 【v6: 单一烧录文件】
 *
 * ⚠️ IAP 运行在**分区表 A** (0xB000)，而 user_app 定义在**分区表 B**
 *    (0x140000)。esp_partition_find_first() 只能看到当前生效的表 (A)，
 *    因此**找不到 user_app**。
 *
 * 解决: 用户程序打包为**从 0x140000 开始的单一文件**，IAP 整段写入:
 *
 *     0x140000  分区表 B     (4KB)
 *     0x141000  0xFF 填充    (nvs 位置)
 *     0x150000  应用镜像     (0xE9 开头)
 *
 * 与 esptool 的 `write_flash 0x140000 file.bin` 语义一致。
 * 打包工具: examples/user_app_template/build_user_app.py
 *           tools/merge_user_app.py
 *
 * 因此:
 *   - 用户程序**必须打包**（合并分区表 B），不能直接烧 idf.py 产物
 *   - 无需 IAP 侧 CRC 校验（bootloader 会校验）
 * ========================================================================== */

/* ============================================================================
 * IAP 下载目标
 * ========================================================================== */

typedef enum {
    IAP_TARGET_USER_APP = 0,            /*!< 用户程序区 */
    IAP_TARGET_IAP_CFG,                 /*!< IAP 配置区 */
    IAP_TARGET_RAW_OFFSET,              /*!< 用户程序区指定偏移 (裸写) */
} iap_target_t;

/* ============================================================================
 * 通用工具
 * ========================================================================== */

/** @brief 小端读取/写入辅助 */
static inline uint16_t iap_rd_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t iap_rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void iap_wr_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static inline void iap_wr_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/* ============================================================================
 * 模块初始化入口
 * ========================================================================== */

esp_err_t iap_config_init(void);
esp_err_t iap_image_init(void);
esp_err_t iap_uart_start(void);
esp_err_t iap_i2c_start(void);
esp_err_t iap_wifi_start(void);
esp_err_t iap_http_start(void);
esp_err_t iap_terminal_start(void);

#ifdef __cplusplus
}
#endif
