/*
 * ESP IAP - I2C 从机协议接口
 *
 * 帧格式与命令定义详见 iap_i2c.c 顶部注释。
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 单个 I2C 接收块的最大长度 */
#define I2C_RX_BLOCK_SIZE     1024

/** 发送缓冲大小 (需容纳多帧响应) */
#define I2C_TX_BUF_SIZE       (16 * 1024)

/** 接收队列长度 */
#define I2C_RX_QUEUE_LEN      16

/**
 * @brief 解析后的 I2C 帧头
 */
typedef struct {
    uint16_t magic;         /*!< 魔数，应为 0x4950 ("IP" 小端) */
    uint8_t  version;       /*!< 协议版本，固定 0x01 */
    uint8_t  fragment;      /*!< 分片标志: 0b10 单包, 0b01 非末包, 0b11 末包 */
    uint32_t message_id;    /*!< 消息号 */
    uint8_t  packet_index;  /*!< 包序号 */
    uint8_t  packet_count;  /*!< 包总数 */
    uint16_t payload_len;   /*!< 负载长度 */
    const uint8_t *payload; /*!< 指向负载数据 (在原始帧缓冲内) */
} iap_i2c_frame_t;

/**
 * @brief 接收队列元素
 */
typedef struct {
    uint16_t len;                       /*!< 数据长度 */
    uint8_t  data[I2C_RX_BLOCK_SIZE];   /*!< 数据内容 */
} i2c_rx_item_t;

/**
 * @brief 启动 I2C 从机
 *
 * 从配置区读取 SCL/SDA/地址/频率并初始化 I2C 从机，
 * 注册回调并启动命令处理任务。
 *
 * @return ESP_OK 成功
 */
esp_err_t iap_i2c_start(void);

#ifdef __cplusplus
}
#endif
