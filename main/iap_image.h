/*
 * ESP IAP - 用户程序镜像管理接口
 *
 * 【设计说明】
 *
 * user_app 分区内是**纯 ESP 应用镜像**（无自定义文件头）。
 *
 * v6: 用户程序打包为从 0x140000 开始的**单一文件**（含分区表 B），
 *     IAP 用 iap_image_var_region_write() 整段写入。
 * 镜像合法性完全交给 bootloader 自校验（magic / 段表 / SHA256 / chip_id），
 * 校验失败时 bootloader 会自动回落到 factory (IAP)，不会变砖。
 *
 * 本模块只负责:
 *   - 分区定位
 *   - 读写 / 擦除
 *   - 判断分区内是否存在镜像（读首字节是否为 0xE9）
 *   - 启动分区切换
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_partition.h"
#include "iap_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/** ESP 应用镜像头 magic */
#define IAP_ESP_IMAGE_MAGIC   0xE9

/**
 * @brief 用户程序信息摘要
 *
 * 由于不再有自定义文件头，这里只提供存在性与配置区元数据。
 */
typedef struct {
    bool     present;                       /*!< 分区内是否存在镜像 */
    uint32_t app_version;                   /*!< 版本号 (来自配置区) */
    uint32_t image_size;                    /*!< 已烧录长度 (来自配置区，0=未知) */
    uint32_t image_crc32;                   /*!< 镜像 CRC32 (来自配置区，0=未知) */
} iap_image_info_t;

/**
 * @brief 初始化镜像模块，查找用户程序区与 IAP 区
 *
 * @return ESP_OK 成功
 *         ESP_ERR_NOT_FOUND 找不到必要分区
 */
esp_err_t iap_image_init(void);

/* -------------------------------------------------------------------------- */
/* bootloader 字段一致性检查                                                   */
/* -------------------------------------------------------------------------- */

/**
 * @brief bootloader 字段一致性检查结果
 */
typedef struct {
    bool     present;       /*!< flash 0x0 处有有效 bootloader 镜像 */
    bool     compatible;    /*!< 关键字段与本固件一致 */
    uint8_t  magic;         /*!< 读到的镜像 magic (0xE9 为有效) */
    uint8_t  segment_count; /*!< 段数量 */
    uint8_t  spi_mode;      /*!< flash 模式 */
    uint8_t  spi_speed;     /*!< flash 频率 (仅显示，不参与比对) */
    uint8_t  spi_size;      /*!< flash 容量 */
    uint16_t chip_id;       /*!< 芯片 ID */
    char     detail[160];   /*!< 字段不一致时的详细说明 */
} iap_bootloader_info_t;

/**
 * @brief 检查 flash 中的 bootloader 与本固件的字段是否一致
 *
 * 读取 0x0 处 bootloader 镜像头，比对 magic / chip_id / spi_mode / spi_size。
 *
 * 不比对项:
 *   - **spi_speed**: bootloader 该字段常为 0，运行时才按配置设置，比对会误报
 *   - **编译时间**: 每次构建都不同，不影响功能
 *
 * 典型用途: 只烧 app (0x20000) 时，确认设备上的 bootloader 来自何种构建配置。
 *
 * 注意: 本检查**仅作排查参考**。实测表明字段不一致不会导致启动失败
 *       —— bootloader 会在运行时探测实际 flash 容量并容忍该差异。
 *
 * @param out 输出信息 (可为 NULL)
 * @return ESP_OK 检查完成 (结果见 out->compatible)
 *         ESP_ERR_INVALID_ARG out 为 NULL
 *         ESP_FAIL 无法读取 flash
 */
esp_err_t iap_image_check_bootloader(iap_bootloader_info_t *out);

/**
 * @brief 获取用户程序分区句柄 (OTA 槽 0)
 */
const esp_partition_t *iap_image_get_user_partition(void);

/**
 * @brief 获取 IAP 分区句柄
 */
const esp_partition_t *iap_image_get_iap_partition(void);

/* -------------------------------------------------------------------------- */
/* 多 OTA 槽                                                                   */
/* -------------------------------------------------------------------------- */

/**
 * @brief 获取已发现的 OTA 槽数量 (至少 1)
 */
uint8_t iap_image_get_slot_count(void);

/**
 * @brief 按槽序号获取分区句柄
 *
 * @param slot 槽序号 (0 = ota_0)
 * @return 分区句柄，不存在返回 NULL
 */
const esp_partition_t *iap_image_get_slot(uint8_t slot);

/**
 * @brief 获取当前活动 OTA 槽序号
 */
uint8_t iap_image_get_active_slot(void);

/**
 * @brief 配置区的活动槽是否无效
 *
 * 场景 C: 配置的 active_slot 越界或对应分区不存在时为 true。
 * main.c 据此进入 IAP 下载模式（不静默回退到槽 0）。
 *
 * @return true 无效
 */
bool iap_image_slot_invalid(void);

/**
 * @brief 设置活动 OTA 槽 (写入配置区，下次启动生效)
 *
 * @param slot 槽序号
 * @return ESP_OK 成功
 *         ESP_ERR_INVALID_ARG 槽不存在
 */
esp_err_t iap_image_set_active_slot(uint8_t slot);

/**
 * @brief 按 GPIO 电平选择 OTA 槽
 *
 * 读取配置的 ota_gpio 引脚电平，映射为槽序号。
 * 未配置引脚时返回当前活动槽。
 *
 * @return 选中的槽序号
 */
uint8_t iap_image_slot_from_gpio(void);

/**
 * @brief 判断指定 OTA 槽是否存在镜像
 *
 * @param slot 槽序号
 * @return true 存在
 */
bool iap_image_slot_present(uint8_t slot);

/**
 * @brief 擦除指定 OTA 槽
 *
 * @param slot 槽序号
 * @param size 擦除长度，0 或超范围时擦除整个分区
 * @return ESP_OK 成功
 */
esp_err_t iap_image_slot_erase(uint8_t slot, uint32_t size);

/**
 * @brief 向指定 OTA 槽写入数据
 *
 * @param slot   槽序号
 * @param offset 相对分区起始的偏移
 * @param data   数据指针
 * @param len    数据长度
 * @return ESP_OK 成功
 */
esp_err_t iap_image_slot_write(uint8_t slot, uint32_t offset,
                               const void *data, size_t len);

/**
 * @brief 从指定 OTA 槽读取数据
 *
 * @param slot   槽序号
 * @param offset 相对分区起始的偏移
 * @param data   输出缓冲区
 * @param len    数据长度
 * @return ESP_OK 成功
 */
esp_err_t iap_image_slot_read(uint8_t slot, uint32_t offset,
                              void *data, size_t len);

/**
 * @brief 启动指定 OTA 槽的用户程序 (重启)
 *
 * 由 bootloader 校验镜像。**槽不存在 / 无镜像 / 校验失败时，
 * 自动回落到 IAP (factory) 并重启**，不会卡死或变砖。
 * 回落前会把活动槽重置为 0，避免下次启动重复失败。
 *
 * @param slot 槽序号
 * @return 仅在回落也失败时返回；正常情况已重启，不会返回
 */
esp_err_t iap_image_boot_slot(uint8_t slot);

/* -------------------------------------------------------------------------- */
/* 直接跳转启动 (不使用 otadata)                                               */
/* -------------------------------------------------------------------------- */

/**
 * @brief 镜像校验结果
 */
typedef struct {
    bool     ok;            /*!< 全部校验通过 */
    char     reason[96];    /*!< 失败原因 */
    uint32_t image_size;    /*!< 镜像总长度 (含段表与尾部校验) */
    uint16_t chip_id;       /*!< 镜像声明的芯片 ID */
    uint32_t entry_addr;    /*!< 入口地址 */
    uint8_t  segment_count; /*!< 段数量 */
} iap_image_verify_t;

/**
 * @brief 校验 OTA 槽内的 ESP 应用镜像 (IAP 侧自校验)
 *
 * 校验项:
 *   1. magic == 0xE9
 *   2. segment_count 合法 (1 ~ 16)
 *   3. 每段 load_addr / data_len 合法 (不越界、不覆盖 IAP 自身)
 *   4. chip_id 与本芯片一致
 *   5. 尾部 SHA256 (若镜像含 appended hash) 或 checksum 比对
 *
 * 用途: 直接跳转启动前的自校验 (不再依赖 bootloader)。
 *
 * @param slot 槽序号
 * @param out  输出结果 (可为 NULL)
 * @return ESP_OK 校验流程完成 (结果见 out->ok)
 *         ESP_ERR_INVALID_ARG / ESP_ERR_NOT_FOUND 槽不存在
 */
esp_err_t iap_image_verify_slot(uint8_t slot, iap_image_verify_t *out);

/**
 * @brief 直接跳转启动指定 OTA 槽的用户程序 (**不写 otadata**)
 *
 * 与 iap_image_boot_slot() 的区别:
 *   - boot_slot()  : 写 otadata + esp_restart() -> ROM 直接加载 ota_x
 *                    **下次上电会绕过 IAP**
 *   - boot_slot_direct(): IAP 自己把镜像段拷到 RAM 并跳转
 *                    **otadata 保持擦除态 -> 每次上电都先跑 IAP**
 *
 * 流程:
 *   1. iap_image_verify_slot() 完整校验 (magic/段表/chip_id/SHA256)
 *   2. 释放 IAP 占用的资源 (HTTP / WiFi / I2C / 终端)
 *   3. 逐段拷贝到 load_addr
 *   4. 跳转到 entry_addr (不返回)
 *
 * 校验失败时**不跳转**，返回错误并进入 IAP 下载模式。
 *
 * @param slot 槽序号
 * @return 仅在失败时返回 (成功跳转后不返回)
 */
esp_err_t iap_image_boot_slot_direct(uint8_t slot);

/**
 * @brief 判断用户程序区是否存在镜像
 *
 * 读取分区首字节，若为 ESP 镜像头 magic (0xE9) 则认为存在。
 * 注意: 这只是**快速存在性检查**，不做完整校验 ——
 *       完整校验由 bootloader 在启动时完成。
 *
 * @return true 存在
 */
bool iap_image_user_app_present(void);

/**
 * @brief 获取用户程序信息 (存在性 + 配置区元数据)
 *
 * @param[out] info 输出信息
 * @return ESP_OK 成功
 */
esp_err_t iap_image_get_info(iap_image_info_t *info);

/**
 * @brief 擦除用户程序区
 *
 * @param size 需要擦除的长度，0 或超范围时擦除整个分区
 * @return ESP_OK 成功
 */
esp_err_t iap_image_erase(uint32_t size);

/**
 * @brief 向用户程序区写入数据
 *
 * @param offset 相对分区起始的偏移
 * @param data   数据指针
 * @param len    数据长度
 * @return ESP_OK 成功
 */
esp_err_t iap_image_write(uint32_t offset, const void *data, size_t len);

/**
 * @brief 从用户程序区读取数据
 *
 * @param offset 相对分区起始的偏移
 * @param data   输出缓冲区
 * @param len    数据长度
 * @return ESP_OK 成功
 */
esp_err_t iap_image_read(uint32_t offset, void *data, size_t len);

/* ==========================================================================
 * 可变区整段写入 (v6)
 * ==========================================================================
 *
 * v6 烧录方案: 用户程序打包为**从 0x140000 开始的单一文件**
 *   (分区表 B + 0xFF 填充 + 应用镜像)，IAP 整段写入可变区。
 *
 * 与 esptool 语义一致: 把文件字节原样写到指定地址，不解析内容。
 */

/**
 * @brief 整段擦除并写入可变区
 *
 * @param addr 起始地址 (必须 >= IAP_FIXED_REGION_END = 0x140000)
 * @param data 数据指针
 * @param len  数据长度
 * @return ESP_OK 成功; ESP_ERR_INVALID_ARG 地址落在固定区
 */
esp_err_t iap_image_var_region_write(uint32_t addr, const void *data, size_t len);

/**
 * @brief 从可变区镜像中提取应用镜像长度
 *
 * 输入文件 (从 0x140000 开始):
 *   0x0000  分区表 B (4KB)
 *   0x1000  0xFF 填充
 *   0x10000 应用镜像 (0xE9 开头)
 *
 * @param data 文件数据
 * @param len  文件长度
 * @return 应用镜像长度 (字节)，0 表示格式不符
 */
size_t iap_image_parse_var_image(const uint8_t *data, size_t len);

/**
 * @brief 切换到用户程序启动分区 (由 bootloader 校验镜像)
 *
 * 调用 esp_ota_set_boot_partition() —— 它内部会用
 * esp_image_verify() 做完整校验（magic / 段表 / SHA256 / chip_id）。
 * 校验失败返回 ESP_ERR_OTA_VALIDATE_FAILED。
 *
 * @return 仅在失败时返回；成功时已重启，不会返回
 */
esp_err_t iap_image_boot_user_app(void);

/**
 * @brief 切换回 IAP 启动分区并重启
 *
 * @return 仅在失败时返回；成功时已重启，不会返回
 */
esp_err_t iap_image_boot_iap(void);

/**
 * @brief 请求进入 IAP 下载模式并重启
 *
 * @param reason 进入原因
 * @return 仅在失败时返回；成功时不会返回 (已重启)
 */
esp_err_t iap_image_request_iap(iap_boot_reason_t reason);

/* -------------------------------------------------------------------------- */
/* 强制进入 IAP —— 已改为 GPIO 电平检测 (v5)                                    */
/*                                                                            */
/* v4 及以前的 flash iap_mark 标志区与 marker API 已删除。                      */
/* 替代方案 (详见 docs/FINAL-REPORT.md §3.3):                                  */
/*   A. GPIO0 电平检测  —— iap_wait_trigger.c (上电按住 BOOT 键)                */
/*   B. UART 命令       —— 等待窗口内发 "iap" 字符串                            */
/*   C. 用户程序主动请求 —— iap_param_set_boot_iap() → esp_restart()            */
/*   D. 断电重上电      —— 冷启动 RTC RAM 无效 → 默认进 IAP                     */
/* -------------------------------------------------------------------------- */

#ifdef __cplusplus
}
#endif
