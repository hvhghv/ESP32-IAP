/*
 * ESP IAP - HTTP 应用接口
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 HTTP 服务
 *
 * 需要先启动 WiFi AP。提供系统信息查询、配置读写、
 * 固件上传烧录、用户分区下载等功能。
 *
 * @return ESP_OK 成功
 */
esp_err_t iap_http_start(void);

/**
 * @brief 停止 HTTP 服务
 *
 * @return ESP_OK 成功
 */
esp_err_t iap_http_stop(void);

#ifdef __cplusplus
}
#endif
