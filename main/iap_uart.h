/*
 * ESP IAP - 终端接口
 *
 * 终端引擎与传输后端解耦:
 *   - 引擎 (本文件实现): 行编辑、命令解析、XMODEM 调度
 *   - 后端 (iap_uart.c / iap_usb.c): 提供读/写字节的原语
 *
 * 这样 UART 与 USB 串口可共用同一套命令与交互逻辑。
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 终端命令行最大长度 */
#define IAP_TERM_LINE_MAX     256

/**
 * @brief 终端后端接口
 *
 * 每种物理通道 (UART / USB) 实现这组原语，终端引擎据此收发。
 */
typedef struct {
    const char *name;       /*!< 后端名称 (用于日志) */

    /** 读取至多 len 字节，返回实际读取数 (0 表示超时无数据) */
    int (*read)(uint8_t *buf, size_t len, uint32_t timeout_ms);

    /** 写入 len 字节，返回实际写入数 */
    int (*write)(const uint8_t *buf, size_t len);

    /** 后端是否就绪 (未就绪时引擎不启动) */
    bool (*ready)(void);

    /**
     * 等待发送缓冲真正排空 (可选, 可为 NULL)。
     *
     * USB-Serial-JTAG 等异步通道必须实现: 进入 XMODEM 等独占会话前
     * 调用, 确保此前的提示文本已真正送出, 否则残留字节会被发送方
     * 当作协议数据 (ACK/NAK) 读走。
     */
    void (*flush)(void);
} iap_term_backend_t;

/* -------------------------------------------------------------------------- */
/* 后端注册                                                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief 启动 UART 终端
 *
 * 从配置区读取端口/引脚/波特率，安装驱动并把 UART 注册为终端后端。
 *
 * @return ESP_OK 成功
 */
esp_err_t iap_uart_start(void);

/**
 * @brief 启动 USB 串口终端 (USB-Serial-JTAG CDC)
 *
 * 仅支持 USB 的芯片可用；不支持的芯片直接返回 ESP_OK (不启动)。
 * 无波特率概念 (由主机决定)，默认 115200 仅为日志显示。
 *
 * @return ESP_OK 成功
 */
esp_err_t iap_usb_start(void);

/**
 * @brief 仅安装 USB-Serial-JTAG 驱动 (不挂终端)
 *
 * 供**等待触发窗口**提前调用: 窗口期间需要读 USB 输入来判断是否
 * 收到 "iap" 进入命令，但此时终端尚未启动。
 *
 * 幂等: 重复调用安全 (已安装则直接返回 ESP_OK)。
 * 未使能 USB (IAP_CFG_FLAG_USB_ENABLE) 或芯片不支持时返回 ESP_OK 但不安装。
 *
 * @return ESP_OK 成功
 */
esp_err_t iap_usb_prepare(void);

/**
 * @brief USB 驱动是否已就绪 (可读)
 */
bool iap_usb_ready(void);

/**
 * @brief 从 USB 读取字节 (非阻塞)
 *
 * @param buf 输出缓冲区
 * @param len 期望长度
 * @return 实际读到的字节数 (0 = 无数据/未就绪)
 */
int iap_usb_read(uint8_t *buf, size_t len);

/* -------------------------------------------------------------------------- */
/* 终端引擎 (供各后端调用)                                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief 向所有已注册的后端输出字符串
 *
 * 用于命令回显与日志，UART 与 USB 会同时收到。
 */
void iap_term_write(const char *s);

/**
 * @brief 格式化输出到所有已注册的后端
 */
void iap_term_printf(const char *fmt, ...);

/**
 * @brief 设置终端输出静默
 *
 * 置位后 iap_term_write / iap_term_printf (含 ESP_LOG 输出) 全部丢弃。
 *
 * ⚠️ XMODEM 等**独占通道**的会话必须使用:
 *    日志文本会与协议数据混在同一字节流中, 发送方逐字节解析时
 *    会先读到日志字符而误判, 导致「设备已收包但发送方等不到 ACK」。
 *    会话结束 (含所有错误路径) 后必须清除, 否则终端不再有任何输出。
 *
 * @param mute true 静默, false 恢复
 */
void iap_term_set_mute(bool mute);

/**
 * @brief 等待所有后端的发送缓冲排空
 *
 * USB-Serial-JTAG 的 write 是异步的 (先进驱动缓冲, 再由硬件发出)。
 * 进入 XMODEM 等独占会话前必须调用, 否则尚未发完的提示文本会被
 * 发送方当作协议数据 (ACK/NAK) 读走。
 */
void iap_term_flush(void);

/**
 * @brief 注册一个终端后端并启动对应的读取任务
 *
 * 每个后端独立一个任务，共享同一套命令表与行缓冲 (各自独立行缓冲)。
 *
 * @param backend 后端接口 (须为静态存储)
 * @param task_name 任务名
 * @return ESP_OK 成功
 */
esp_err_t iap_term_attach(const iap_term_backend_t *backend, const char *task_name);

/** 当前是否有任一后端就绪 */
bool iap_term_any_ready(void);

#ifdef __cplusplus
}
#endif
