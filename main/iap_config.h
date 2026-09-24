/*
 * ESP IAP - 配置区管理接口
 *
 * 配置区位于独立分区 iap_cfg，可由 IAP 程序区与用户程序区共同读写。
 * 本模块提供线程安全的读写接口，并保证掉电安全。
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "iap_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化配置模块
 *
 * 查找 iap_cfg 分区，读取有效配置槽。
 * 若两个槽都无效，则写入默认配置。
 *
 * @return ESP_OK 成功
 *         ESP_ERR_NOT_FOUND 找不到配置分区
 */
esp_err_t iap_config_init(void);

/**
 * @brief 获取当前配置的副本
 *
 * @param[out] out 输出配置，不可为 NULL
 * @return ESP_OK 成功
 */
esp_err_t iap_config_get(iap_cfg_data_t *out);

/**
 * @brief 保存配置
 *
 * 写入冗余槽并更新序号，保证掉电安全。
 * 写入前会做合法性修正。
 *
 * @param[in] cfg 待保存配置
 * @return ESP_OK 成功
 */
esp_err_t iap_config_set(const iap_cfg_data_t *cfg);

/**
 * @brief 按位更新配置标志
 *
 * @param set_mask 需要置位的标志
 * @param clr_mask 需要清除的标志
 * @return ESP_OK 成功
 */
esp_err_t iap_config_update_flags(uint32_t set_mask, uint32_t clr_mask);

/**
 * @brief 查询是否处于 IAP 下载模式
 *
 * @return true 表示配置区下载位被置位
 */
bool iap_config_is_download_mode(void);

/**
 * @brief 设置或清除 IAP 下载模式标志
 *
 * @param enable true 置位，false 清除
 * @return ESP_OK 成功
 */
esp_err_t iap_config_set_download_mode(bool enable);

/**
 * @brief 记录本次进入 IAP 的原因
 *
 * @param reason 原因枚举
 * @return ESP_OK 成功
 */
esp_err_t iap_config_set_boot_reason(iap_boot_reason_t reason);

/**
 * @brief 用户程序启动计数 +1
 *
 * @return ESP_OK 成功
 */
esp_err_t iap_config_inc_boot_count(void);

/**
 * @brief 连续启动失败计数 +1
 *
 * IAP 在**准备启动用户程序前**调用。若用户程序成功运行，应调用
 * iap_config_clear_boot_fail() 清零；否则下次 IAP 启动时该值仍保留，
 * 累计超过 IAP_BOOT_MAX_RETRY 后 IAP 将停止尝试启动并停在下载模式，
 * 避免「启动失败 → 回落 IAP → 又启动」的无限重启循环。
 *
 * @return ESP_OK 成功
 */
esp_err_t iap_config_inc_boot_fail(void);

/**
 * @brief 清零连续启动失败计数
 *
 * 用户程序启动成功后调用 (表示本次启动成功)。
 * 也可由用户在终端执行 `app clearfail` 手动清零。
 *
 * @return ESP_OK 成功
 */
esp_err_t iap_config_clear_boot_fail(void);

/**
 * @brief 读取连续启动失败计数
 *
 * @return 当前计数
 */
uint16_t iap_config_get_boot_fail(void);

/**
 * @brief 恢复默认配置
 *
 * @return ESP_OK 成功
 */
esp_err_t iap_config_reset(void);

/**
 * @brief 手动触发配置数据版本迁移
 *
 * 正常情况下 iap_config_init() 会在启动时自动迁移。
 * 本接口用于手动重试 (如自动迁移写回失败后)。
 *
 * 迁移策略: 以默认配置为基准，逐项合并旧配置中仍然存在的字段，
 *           已删除的字段忽略，冲突值由 sanitize() 修正。
 *
 * @return ESP_OK 成功 (含"无需迁移")
 */
esp_err_t iap_config_migrate(void);

/**
 * @brief 获取配置数据版本
 *
 * @return 当前配置的 data_ver
 */
uint16_t iap_config_get_data_ver(void);

/* ============================================================================
 * 多 OTA 槽配置
 * ========================================================================== */

/**
 * @brief 获取当前活动 OTA 槽序号
 *
 * @return 槽序号 (0 = ota_0)
 */
uint8_t iap_config_get_active_slot(void);

/**
 * @brief 设置活动 OTA 槽序号
 *
 * 下次启动用户程序时使用该槽。若槽序号超出 ota_slot_count，
 * 会被截断到有效范围。
 *
 * @param slot 槽序号 (0 ~ IAP_OTA_SLOT_MAX-1)
 * @return ESP_OK 成功
 */
esp_err_t iap_config_set_active_slot(uint8_t slot);

/**
 * @brief 获取已发现的 OTA 槽数量
 *
 * 由 IAP 启动扫描分区表后填充 (见 iap_image_init)。
 *
 * @return 槽数量 (至少 1)
 */
uint8_t iap_config_get_slot_count(void);

/**
 * @brief 设置已发现的 OTA 槽数量
 *
 * @param count 槽数量
 * @return ESP_OK 成功
 */
esp_err_t iap_config_set_slot_count(uint8_t count);

/**
 * @brief 获取 GPIO 选择 OTA 槽的引脚编号
 *
 * @return GPIO 编号，IAP_OTA_GPIO_DISABLED (0xFF) 表示禁用
 */
uint8_t iap_config_get_ota_gpio(void);

/**
 * @brief 设置 GPIO 选择 OTA 槽的引脚编号
 *
 * 使能后，启动时读取该引脚电平组合，映射为槽序号
 * (电平值 & (slot_count-1))，覆盖配置区的 active_slot。
 *
 * @param gpio GPIO 编号，IAP_OTA_GPIO_DISABLED 表示禁用
 * @return ESP_OK 成功
 */
esp_err_t iap_config_set_ota_gpio(uint8_t gpio);

/* ============================================================================
 * 启动参数字符串
 *
 * 用于 IAP 与用户程序之间的参数传递:
 *   - IAP 在启动用户程序前调用 iap_config_set_boot_param() 写入
 *   - 用户程序启动后调用 iap_config_get_boot_param() 读取并初始化
 *
 * 字符串以 '\0' 结尾，长度上限 IAP_CFG_BOOT_PARAM_MAX_LEN 个字符。
 * 建议格式: "key1=value1;key2=value2"
 * ========================================================================== */

/**
 * @brief 写入启动参数字符串
 *
 * 同时更新 boot_param_len、boot_param_crc32 并置位 VALID 标志、
 * 清除 CONSUMED 标志。
 *
 * @param[in] param 参数字符串，NULL 或空串表示清除启动参数
 * @return ESP_OK 成功
 *         ESP_ERR_INVALID_SIZE 字符串超出 IAP_CFG_BOOT_PARAM_MAX_LEN
 */
esp_err_t iap_config_set_boot_param(const char *param);

/**
 * @brief 读取启动参数字符串
 *
 * 会校验 CRC32，CRC 不匹配时返回 ESP_ERR_INVALID_CRC。
 *
 * @param[out] buf     输出缓冲区
 * @param[in]  buf_len 缓冲区长度，建议 IAP_CFG_BOOT_PARAM_SIZE
 * @return ESP_OK 成功
 *         ESP_ERR_NOT_FOUND 启动参数未设置
 *         ESP_ERR_INVALID_SIZE 缓冲区不足
 *         ESP_ERR_INVALID_CRC CRC 校验失败
 */
esp_err_t iap_config_get_boot_param(char *buf, size_t buf_len);

/**
 * @brief 读取启动参数字符串并标记为已消费
 *
 * 语义与 iap_config_get_boot_param() 相同，但读取成功后置位 CONSUMED 标志，
 * 便于实现"仅首次启动生效"的参数。
 *
 * @param[out] buf     输出缓冲区
 * @param[in]  buf_len 缓冲区长度
 * @return ESP_OK 成功
 */
esp_err_t iap_config_take_boot_param(char *buf, size_t buf_len);

/**
 * @brief 清除启动参数字符串
 *
 * @return ESP_OK 成功
 */
esp_err_t iap_config_clear_boot_param(void);

/**
 * @brief 查询启动参数是否有效
 *
 * @return true 表示已由 IAP 写入且 CRC 正确
 */
bool iap_config_has_boot_param(void);

/**
 * @brief 查询启动参数是否已被用户程序消费
 *
 * @return true 表示已消费
 */
bool iap_config_boot_param_consumed(void);

/**
 * @brief 从启动参数中提取指定键的值
 *
 * 参数格式为 "key1=value1;key2=value2"，本函数查找 `key` 并复制其值。
 *
 * @param[in]  param    参数字符串
 * @param[in]  key      键名
 * @param[out] out      输出值缓冲区
 * @param[in]  out_len  缓冲区长度
 * @return ESP_OK 成功
 *         ESP_ERR_NOT_FOUND 未找到该键
 *         ESP_ERR_INVALID_SIZE 缓冲区不足
 */
esp_err_t iap_config_boot_param_get_value(const char *param, const char *key,
                                          char *out, size_t out_len);

/**
 * @brief 获取 AP IP 字符串
 *
 * @param[out] buf     输出缓冲区
 * @param[in]  buf_len 缓冲区长度 (建议 >= 16)
 */
void iap_config_get_ip_string(char *buf, size_t buf_len);

/**
 * @brief 获取 AP 子网掩码字符串
 *
 * @param[out] buf     输出缓冲区
 * @param[in]  buf_len 缓冲区长度 (建议 >= 16)
 */
void iap_config_get_netmask_string(char *buf, size_t buf_len);

/**
 * @brief 将进入 IAP 原因转换为可读字符串
 *
 * @param reason 原因枚举
 * @return 静态字符串
 */
const char *iap_boot_reason_str(iap_boot_reason_t reason);

#ifdef __cplusplus
}
#endif
