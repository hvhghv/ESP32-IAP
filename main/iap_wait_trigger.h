/*
 * ESP IAP - 等待期间进入 IAP 下载模式的触发检测接口
 *
 * 在启动等待窗口内监听多个触发源 (GPIO / I2C / UART / WiFi)，
 * 任一使能的触发源命中即中止等待并进入 IAP 下载模式。
 *
 * 典型用法 (main.c):
 *
 *   iap_wait_trigger_start(&cfg, wait_ms, s_main_task);
 *   ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms + 1000));
 *   iap_wait_trigger_stop();
 *
 *   if (iap_wait_trigger_fired()) {
 *       reason = iap_wait_trigger_reason();
 *       enter_download = true;
 *   }
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "iap_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 触发源启动回调
 *
 * 由 main.c 提供，用于在等待阶段启动 I2C/WiFi 等子系统。
 * 返回 ESP_OK 表示启动成功。
 */
typedef esp_err_t (*iap_wait_start_fn_t)(void);

/**
 * @brief 启动等待触发检测
 *
 * 按配置启动各触发源并创建检测任务:
 *   - GPIO: 立即配置引脚为输入并按有效电平启用内部上/下拉
 *   - I2C : 调用 start_i2c 回调启动 I2C 从机，监听进入命令
 *   - UART: 探测驱动可用性，监听进入命令
 *   - WiFi: 调用 start_wifi 回调启动 AP，监听客户端接入事件
 *
 * 未使能的触发源不启动，不占用资源。
 *
 * @param[in] cfg        当前 IAP 配置
 * @param[in] wait_ms    等待窗口 (毫秒)
 * @param[in] main_task  主任务句柄 (检测任务结束时通知)
 * @param[in] start_i2c  启动 I2C 的回调，可为 NULL (则禁用 I2C 触发)
 * @param[in] start_wifi 启动 WiFi 的回调，可为 NULL (则禁用 WiFi 触发)
 * @return ESP_OK 成功 (即使所有触发源都禁用/失败)
 */
esp_err_t iap_wait_trigger_start(const iap_cfg_data_t *cfg, uint32_t wait_ms,
                                 TaskHandle_t main_task,
                                 iap_wait_start_fn_t start_i2c,
                                 iap_wait_start_fn_t start_wifi);

/**
 * @brief 请求停止触发检测
 *
 * 检测任务会在下一个轮询周期退出。
 */
void iap_wait_trigger_stop(void);

/**
 * @brief 查询是否有触发源命中
 *
 * @return true 表示应进入 IAP 下载模式
 */
bool iap_wait_trigger_fired(void);

/**
 * @brief 获取触发原因
 *
 * @return 触发原因枚举；无触发时返回 IAP_BOOT_REASON_NONE
 */
iap_boot_reason_t iap_wait_trigger_reason(void);

/**
 * @brief 由 I2C 模块调用: 收到进入 IAP 命令
 *
 * 仅当 I2C 触发已使能且尚未触发时生效。
 */
void iap_wait_trigger_notify_i2c(void);

/**
 * @brief 由 WiFi 模块调用: 有客户端接入
 *
 * 仅当 WiFi 触发已使能且尚未触发时生效。
 */
void iap_wait_trigger_notify_wifi(void);

#ifdef __cplusplus
}
#endif
