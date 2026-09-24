/*
 * ESP IAP - XMODEM 协议接口
 *
 * 本模块与传输介质解耦: 调用者提供 read/write 回调，
 * 因此同一套实现可用于 UART、I2C 或其它通道。
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 字节读取回调
 *
 * @param user    用户上下文
 * @param buf     输出缓冲区
 * @param len     期望读取的字节数
 * @param timeout 超时 (毫秒)
 * @return 实际读取的字节数
 */
typedef int (*iap_xmodem_read_fn_t)(void *user, uint8_t *buf, size_t len,
                                    uint32_t timeout);

/**
 * @brief 字节写入回调
 *
 * @param user 用户上下文
 * @param buf  待发送数据
 * @param len  数据长度
 * @return 实际写入的字节数
 */
typedef int (*iap_xmodem_write_fn_t)(void *user, const uint8_t *buf, size_t len);

/**
 * @brief 接收数据回调
 *
 * @param user   用户上下文
 * @param data   数据指针
 * @param len    数据长度
 * @param offset 该数据在文件中的偏移
 * @return ESP_OK 表示已成功处理 (写入 flash)
 */
typedef esp_err_t (*iap_xmodem_data_cb_t)(void *user, const uint8_t *data,
                                          size_t len, uint32_t offset);

/**
 * @brief 读取数据回调 (发送时使用)
 *
 * @param user   用户上下文
 * @param buf    输出缓冲区
 * @param offset 文件偏移
 * @param len    需要读取的长度
 * @return ESP_OK 成功
 */
typedef esp_err_t (*iap_xmodem_read_cb_t)(void *user, uint8_t *buf,
                                          uint32_t offset, size_t len);

/**
 * @brief XMODEM 协议上下文
 */
typedef struct {
    iap_xmodem_read_fn_t  read;         /*!< 字节读取回调 */
    iap_xmodem_write_fn_t write;        /*!< 字节写入回调 */
    void                 *user;         /*!< 用户上下文，传给回调 */

    volatile bool        *cancel_flag;  /*!< 指向取消标志，可为 NULL */

    /* --- 以下为运行时输出 --- */
    uint8_t  last_seq;                  /*!< 最近收到的包序号 */
    uint32_t total_bytes;               /*!< 累计传输字节数 */
    uint32_t file_size;                 /*!< 文件总长度 (0 表示未知) */
    char     file_name[64];             /*!< 文件名 */
} iap_xmodem_ctx_t;

/**
 * @brief 以 XMODEM 协议接收文件
 *
 * 支持 XMODEM-CRC 与 XMODEM-1K。若发送方使用 YMODEM 风格的第 0 包
 * (包含文件名与长度)，本函数会自动解析并使用其中的长度信息。
 *
 * @param ctx     协议上下文 (需已设置回调)
 * @param on_data 数据回调，每收到一个数据包调用一次
 * @param user    传给 on_data 的用户上下文
 * @return ESP_OK 接收成功
 *         ESP_ERR_TIMEOUT 超时
 *         ESP_ERR_INVALID_STATE 被取消
 *         ESP_ERR_INVALID_CRC 校验失败
 */
esp_err_t iap_xmodem_receive(iap_xmodem_ctx_t *ctx,
                             iap_xmodem_data_cb_t on_data,
                             void *user);

/**
 * @brief 以 XMODEM 协议发送文件
 *
 * @param ctx       协议上下文 (需已设置回调)
 * @param file_name 文件名，用于第 0 包
 * @param file_size 文件长度
 * @param on_read   读取数据回调
 * @param user      传给 on_read 的用户上下文
 * @return ESP_OK 发送成功
 */
esp_err_t iap_xmodem_send(iap_xmodem_ctx_t *ctx,
                          const char *file_name,
                          uint32_t file_size,
                          iap_xmodem_read_cb_t on_read,
                          void *user);

#ifdef __cplusplus
}
#endif
