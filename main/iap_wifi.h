/*
 * ESP IAP - WiFi AP 接口
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 WiFi SoftAP
 *
 * SSID/密码/信道/IP 段从 IAP 配置区读取。
 *
 * @return ESP_OK 成功
 */
esp_err_t iap_wifi_start(void);

/**
 * @brief 启动 WiFi SoftAP (可指定是否阻塞等待就绪)
 *
 * @param[in] nonblocking true 表示不等待 AP 就绪立即返回
 *                        (供启动等待阶段使用，避免占用等待窗口)
 * @return ESP_OK 成功
 */
esp_err_t iap_wifi_start_ex(bool nonblocking);

/**
 * @brief 获取 AP 的 IP 地址字符串
 *
 * @return 静态字符串，如 "192.168.4.1"
 */
const char *iap_wifi_get_ip(void);

/**
 * @brief 获取 AP 网卡句柄
 */
esp_netif_t *iap_wifi_get_netif(void);

/**
 * @brief 停止并释放 WiFi / 网络栈资源
 *
 * 在跳转到用户程序之前调用。IAP 创建过 `WIFI_AP_DEF` 网卡后，
 * 用户程序再次调用 `esp_netif_create_default_wifi_ap()` 会因
 * 同名网卡已存在而失败，导致用户程序无法建立自己的 WiFi。
 *
 * 本函数会依次:
 *   1. 停止 DHCP 服务并销毁 AP 网卡 (`esp_netif_destroy`)
 *   2. 注销事件处理
 *   3. `esp_wifi_stop()` + `esp_wifi_deinit()`
 *   4. 删除事件组
 *
 * 未启动过 WiFi 时安全返回 ESP_OK (幂等)。
 *
 * @return ESP_OK 成功
 */
esp_err_t iap_wifi_stop(void);

#ifdef __cplusplus
}
#endif
