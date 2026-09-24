/*
 * ESP IAP - XMODEM 协议实现
 *
 * 支持 XMODEM / XMODEM-CRC / XMODEM-1K 三种模式，带 YMODEM 风格的
 * 文件名信息包 (第 0 包) 与结束包。
 *
 * 协议要点:
 *   - 接收方发送 'C' (0x43) 表示使用 CRC16 校验
 *   - 发送方按 128 字节 (SOH) 或 1024 字节 (STX) 分包
 *   - 每包: <SOH|STX> <seq> <255-seq> <data...> <crc_hi> <crc_lo>
 *   - 接收方回 ACK(0x06) 或 NAK(0x15)
 *   - 传输结束发送方发 EOT(0x04)，接收方回 ACK
 *
 * 本模块与具体传输介质解耦，通过回调函数进行字节收发，
 * 因此可同时用于 UART 终端与其它通道。
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "iap_common.h"
#include "iap_crc.h"
#include "iap_xmodem.h"

static const char *TAG = "xmodem";

/* 协议控制字符 */
#define XM_SOH      0x01    /*!< 128 字节数据包起始 */
#define XM_STX      0x02    /*!< 1024 字节数据包起始 */
#define XM_EOT      0x04    /*!< 传输结束 */
#define XM_ACK      0x06    /*!< 确认 */
#define XM_NAK      0x15    /*!< 否认 */
#define XM_CAN      0x18    /*!< 取消 */
#define XM_CRC_CHAR 0x43    /*!< 'C'，请求 CRC 模式 */
#define XM_SUB      0x1A    /*!< 1K 模式下 Ctrl-Z 填充 */

/* 时序参数 */
#define XM_RETRY_MAX        10      /*!< 单包最大重试次数 */
#define XM_HANDSHAKE_SEC    90      /*!< 握手等待超时 (秒) */
#define XM_PACKET_TIMEOUT   3000    /*!< 单包接收超时 (毫秒) */

/*
 * 无进展超时 (秒): 连续这么久没有成功收到/发出任何数据包则放弃。
 *
 * 作用: 对端掉线/卡死时，避免 XMODEM 无限重试把终端卡住。
 * 与 XM_HANDSHAKE_SEC 的区别: 后者只管**首次握手**，本项管**传输中**。
 */
#define XM_STALL_SEC        30

/* -------------------------------------------------------------------------- */
/* 低级收发辅助                                                                */
/* -------------------------------------------------------------------------- */

/**
 * @brief 接收一个字节
 *
 * @param ctx     协议上下文
 * @param out     输出字节
 * @param timeout 超时 (毫秒)，0 表示不等待
 * @return 1  成功收到
 *         0  超时 (无数据)
 *        -1  已取消 (read 回调返回负值)
 */
static int xm_get_byte(iap_xmodem_ctx_t *ctx, uint8_t *out, uint32_t timeout)
{
    int n = ctx->read(ctx->user, out, 1, timeout);
    if (n < 0) {
        return -1;      /* 取消 */
    }
    return (n == 1) ? 1 : 0;
}

/**
 * @brief 是否已被请求取消
 */
static inline bool xm_cancelled(iap_xmodem_ctx_t *ctx)
{
    return ctx->cancel_flag != NULL && *ctx->cancel_flag;
}

/**
 * @brief 发送一个字节
 */
static bool xm_put_byte(iap_xmodem_ctx_t *ctx, uint8_t b)
{
    return ctx->write(ctx->user, &b, 1) == 1;
}

/**
 * @brief 丢弃输入缓冲中的残留数据
 */
static void xm_flush(iap_xmodem_ctx_t *ctx)
{
    uint8_t dummy;
    while (ctx->read(ctx->user, &dummy, 1, 0) == 1) {
        /* 持续丢弃 */
    }
}

/**
 * @brief 发送取消序列 (连续 2 个 CAN)
 */
static void xm_send_cancel(iap_xmodem_ctx_t *ctx)
{
    for (int i = 0; i < 3; i++) {
        xm_put_byte(ctx, XM_CAN);
    }
    xm_flush(ctx);
}

/* -------------------------------------------------------------------------- */
/* 接收实现                                                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief 等待数据包起始字符
 *
 * @param ctx    上下文
 * @param out_len 输出包长度 (128 或 1024)
 * @return 起始字符 (SOH/STX/EOT)
 *         0  超时
 *         0xFF 已取消 (调用方据此返回 ESP_ERR_INVALID_STATE)
 */
#define XM_WAIT_CANCELLED   0xFF

static uint8_t xm_wait_start(iap_xmodem_ctx_t *ctx, size_t *out_len)
{
    uint8_t c;
    uint32_t start = xTaskGetTickCount();

    while (1) {
        /* 每轮都检查取消 —— 不能只在读失败时检查，
         * 否则收到噪声字节时会一直循环而忽略 Ctrl+C。 */
        if (xm_cancelled(ctx)) {
            return XM_WAIT_CANCELLED;
        }

        if (xTaskGetTickCount() - start > pdMS_TO_TICKS(XM_HANDSHAKE_SEC * 1000)) {
            return 0;
        }

        int r = xm_get_byte(ctx, &c, 1000);
        if (r < 0) {
            return XM_WAIT_CANCELLED;   /* read 回调报告取消 */
        }
        if (r == 0) {
            continue;                   /* 超时: 回到循环顶部重新检查取消 */
        }

        if (c == XM_SOH) {
            *out_len = 128;
            return c;
        }
        if (c == XM_STX) {
            *out_len = 1024;
            return c;
        }
        if (c == XM_EOT) {
            return c;
        }
        if (c == XM_CAN) {
            ESP_LOGW(TAG, "收到 CAN，传输被取消");
            return XM_WAIT_CANCELLED;
        }
        /* 其它字符忽略 (可能是上一轮的噪声) */
    }
}

/**
 * @brief 接收一个完整数据包
 *
 * @param ctx     上下文
 * @param data    输出数据缓冲区 (至少 1024 字节)
 * @param data_len 输出有效数据长度
 * @return ESP_OK 成功
 */
/**
 * @brief 循环读取直到读满 len 字节
 *
 * ⚠️ 必须循环, 不能依赖单次 read 读满。
 *
 * 底层介质 (USB-Serial-JTAG / UART) 都有硬件 FIFO 与分包限制,
 * 单次 read 常常**短读** (例如 USB-Serial-JTAG 一次最多 64 字节,
 * 而 XMODEM-1K 包体为 1024 字节)。若按「一次读满」判断,
 * 会永远读不满而超时, 表现为「握手成功但一发数据就失败」。
 *
 * @param ctx       协议上下文
 * @param buf       输出缓冲区
 * @param len       需要读取的总长度
 * @param timeout_ms 单次读取的超时 (毫秒)
 * @return ESP_OK 读满
 *         ESP_ERR_INVALID_STATE 被取消
 *         ESP_ERR_TIMEOUT 超时
 */
static esp_err_t xm_read_exact(iap_xmodem_ctx_t *ctx, uint8_t *buf,
                               size_t len, uint32_t timeout_ms)
{
    size_t got = 0;
    uint32_t last_progress = xTaskGetTickCount();

    while (got < len) {
        if (xm_cancelled(ctx)) {
            return ESP_ERR_INVALID_STATE;
        }

        int n = ctx->read(ctx->user, buf + got, len - got, timeout_ms);
        if (n < 0) {
            return ESP_ERR_INVALID_STATE;   /* 介质层报告取消 */
        }
        if (n > 0) {
            got += (size_t)n;
            last_progress = xTaskGetTickCount();
            continue;
        }

        /* 本次无数据: 若长时间无进展则超时 */
        if (xTaskGetTickCount() - last_progress >
            pdMS_TO_TICKS(XM_PACKET_TIMEOUT)) {
            return ESP_ERR_TIMEOUT;
        }
    }
    return ESP_OK;
}

static esp_err_t xm_recv_packet(iap_xmodem_ctx_t *ctx, uint8_t *data, size_t *data_len)
{
    size_t pkt_len = 0;
    uint8_t start = xm_wait_start(ctx, &pkt_len);
    if (start == XM_WAIT_CANCELLED) {
        return ESP_ERR_INVALID_STATE;   /* 已取消 */
    }
    if (start == 0) {
        return ESP_ERR_TIMEOUT;
    }
    if (start == XM_EOT) {
        /* EOT 不是错误 —— 用独立返回值让调用方正确回 ACK */
        return ESP_ERR_NOT_FINISHED;
    }

    /*
     * 进入二进制数据阶段: 后续字节是序号/包体/CRC，可能包含任意值
     * (含 0x03)。必须通知介质层停止 Ctrl+C 检测，否则固件镜像中的
     * 0x03 会被误判为取消请求而中断传输。
     */
    ctx->raw_mode = true;
    iap_uart_xmodem_set_raw_mode(true);

    esp_err_t err = ESP_OK;

    /* 读取序号、反序号、数据、CRC (均循环读满, 容忍介质短读) */
    uint8_t hdr[2];
    err = xm_read_exact(ctx, hdr, 2, XM_PACKET_TIMEOUT);
    if (err != ESP_OK) {
        if (err == ESP_ERR_TIMEOUT && xm_cancelled(ctx)) {
            err = ESP_ERR_INVALID_STATE;
        }
        goto done;
    }

    uint8_t seq  = hdr[0];
    uint8_t nseq = hdr[1];

    if ((uint8_t)(seq + nseq) != 0xFF) {
        ESP_LOGW(TAG, "序号校验失败: seq=%u nseq=%u", seq, nseq);
        err = ESP_ERR_INVALID_CRC;
        goto done;
    }

    err = xm_read_exact(ctx, data, pkt_len, XM_PACKET_TIMEOUT);
    if (err != ESP_OK) {
        if (err == ESP_ERR_TIMEOUT && xm_cancelled(ctx)) {
            err = ESP_ERR_INVALID_STATE;
        }
        goto done;
    }

    uint8_t crc_bytes[2];
    err = xm_read_exact(ctx, crc_bytes, 2, XM_PACKET_TIMEOUT);
    if (err != ESP_OK) {
        if (err == ESP_ERR_TIMEOUT && xm_cancelled(ctx)) {
            err = ESP_ERR_INVALID_STATE;
        }
        goto done;
    }

    uint16_t crc_recv = (uint16_t)((crc_bytes[0] << 8) | crc_bytes[1]);
    uint16_t crc_calc = iap_crc16_xmodem(data, pkt_len);

    if (crc_recv != crc_calc) {
        ESP_LOGW(TAG, "CRC 错误: seq=%u recv=0x%04X calc=0x%04X",
                 seq, crc_recv, crc_calc);
        err = ESP_ERR_INVALID_CRC;
        goto done;
    }

    ESP_LOGI(TAG, "[diag] 收到包 seq=%u len=%u (累计 %" PRIu32 ")",
             seq, (unsigned)pkt_len, ctx->total_bytes);

    ctx->last_seq = seq;
    *data_len = pkt_len;

done:
    /* 退出二进制阶段: 恢复 Ctrl+C 检测 (等待下一个包起始字符时用) */
    ctx->raw_mode = false;
    iap_uart_xmodem_set_raw_mode(false);
    return err;
}

esp_err_t iap_xmodem_receive(iap_xmodem_ctx_t *ctx,
                             iap_xmodem_data_cb_t on_data,
                             void *user)
{
    if (ctx == NULL || ctx->read == NULL || ctx->write == NULL || on_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t *pkt_buf = malloc(1024);
    if (pkt_buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = ESP_OK;
    uint8_t expected_seq = 1;
    uint32_t total_bytes = 0;
    uint32_t file_size = 0;
    bool got_first = false;
    bool finished = false;
    bool pending_packet = false;   /* 握手阶段已收到第 1 包 */
    size_t pend_len = 0;           /* 待处理包的有效数据长度 */

    /* 初始为控制字符阶段: 允许介质层检测 Ctrl+C */
    ctx->raw_mode = false;
    iap_uart_xmodem_set_raw_mode(false);

    ESP_LOGI(TAG, "开始 XMODEM 接收，等待发送方...");

    /* 握手: 发送 'C' 请求 CRC 模式 */
    int handshake_retry = 0;
    while (!got_first) {
        if (xm_cancelled(ctx)) {
            result = ESP_ERR_INVALID_STATE;
            goto out;
        }

        xm_put_byte(ctx, XM_CRC_CHAR);

        size_t data_len = 0;
        esp_err_t err = xm_recv_packet(ctx, pkt_buf, &data_len);

        /* 取消: 立即退出，不重试 */
        if (err == ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "握手期间收到取消请求");
            result = ESP_ERR_INVALID_STATE;
            goto out;
        }

        /* 握手阶段直接收到 EOT (0 字节传输): 回 ACK 后结束 */
        if (err == ESP_ERR_NOT_FINISHED) {
            xm_put_byte(ctx, XM_ACK);
            finished = true;
            break;
        }

        if (err != ESP_OK) {
            if (++handshake_retry > XM_HANDSHAKE_SEC) {
                ESP_LOGE(TAG, "握手超时");
                result = ESP_ERR_TIMEOUT;
                goto out;
            }
            continue;
        }

        /* 第 0 包 (YMODEM 风格): 包含文件名与长度 */
        if (ctx->last_seq == 0) {
            /* 包内容: 文件名\0 长度\0 ... */
            const char *name = (const char *)pkt_buf;
            size_t name_len = strnlen(name, 128);

            if (name_len == 0) {
                /* 空包，表示发送方结束 */
                xm_put_byte(ctx, XM_ACK);
                xm_put_byte(ctx, XM_CRC_CHAR);
                continue;
            }

            if (name_len + 1 < data_len) {
                const char *size_str = (const char *)pkt_buf + name_len + 1;
                file_size = (uint32_t)strtoul(size_str, NULL, 10);
            }

            snprintf(ctx->file_name, sizeof(ctx->file_name), "%s", name);
            ctx->file_size = file_size;

            ESP_LOGI(TAG, "文件名: %s, 大小: %" PRIu32 " 字节", ctx->file_name, file_size);

            xm_put_byte(ctx, XM_ACK);
            xm_put_byte(ctx, XM_CRC_CHAR);

            expected_seq = 1;
            got_first = true;
            continue;
        }

        /* 没有第 0 包，直接进入数据包 */
        if (ctx->last_seq == 1) {
            got_first = true;
            /*
             * 第 1 包已在 xm_recv_packet 中收到，标记为待处理。
             *
             * ⚠️ 必须把**长度**一并带出循环 —— 否则主循环里 data_len
             *    重新初始化为 0，第 1 包会被当成空包丢弃。
             */
            pending_packet = true;
            pend_len = data_len;
            break;
        }
    }

    /* 主接收循环 */
    uint32_t last_progress = xTaskGetTickCount();   /* 上次成功收包的时刻 */
    while (!finished) {
        if (xm_cancelled(ctx)) {
            xm_send_cancel(ctx);
            result = ESP_ERR_INVALID_STATE;
            goto out;
        }

        /* 无进展超时: 对端掉线/卡死时放弃，避免终端被卡住 */
        if (xTaskGetTickCount() - last_progress >
            pdMS_TO_TICKS(XM_STALL_SEC * 1000)) {
            ESP_LOGE(TAG, "传输停滞 %d 秒，放弃接收", XM_STALL_SEC);
            xm_send_cancel(ctx);
            result = ESP_ERR_TIMEOUT;
            goto out;
        }

        size_t data_len = 0;
        esp_err_t err = ESP_OK;

        if (pending_packet) {
            /* 第 1 包已在握手阶段收到，直接处理 (长度也要恢复) */
            pending_packet = false;
            data_len = pend_len;
            err = ESP_OK;
        } else {
            err = xm_recv_packet(ctx, pkt_buf, &data_len);
        }

        /*
         * EOT: 传输结束，回 ACK 后退出。
         *
         * ⚠️ 必须单独处理 —— 若按普通错误回 NAK，发送方会一直重发 EOT，
         *    形成死循环 (空文件时尤其明显: 没有数据包，直接就是 EOT)。
         */
        if (err == ESP_ERR_NOT_FINISHED) {
            xm_put_byte(ctx, XM_ACK);
            finished = true;
            break;
        }

        if (err != ESP_OK) {
            /* 出错则请求重传 */
            ESP_LOGW(TAG, "[diag] 收包失败 (%s)，回 NAK 请求重传",
                     esp_err_to_name(err));
            xm_put_byte(ctx, XM_NAK);
            continue;
        }

        last_progress = xTaskGetTickCount();   /* 收到包，刷新进展 */

        if (ctx->last_seq == expected_seq) {
            /* 序号正确，交付数据 */
            uint32_t payload = (uint32_t)data_len;
            if (file_size > 0 && total_bytes + payload > file_size) {
                payload = file_size - total_bytes;
            }

            if (payload > 0) {
                esp_err_t cb_err = on_data(user, pkt_buf, payload, total_bytes);
                if (cb_err != ESP_OK) {
                    ESP_LOGE(TAG, "数据回调失败: %s", esp_err_to_name(cb_err));
                    xm_send_cancel(ctx);
                    result = cb_err;
                    goto out;
                }
                total_bytes += payload;
            }

            if (!xm_put_byte(ctx, XM_ACK)) {
                ESP_LOGW(TAG, "[diag] ACK 发送失败 (seq=%u)", expected_seq);
            }
            expected_seq++;

            /* 进度输出 */
            if (file_size > 0) {
                ESP_LOGI(TAG, "进度: %" PRIu32 "/%" PRIu32 " 字节 (%" PRIu32 "%%)",
                         total_bytes, file_size, total_bytes * 100 / file_size);
            } else {
                ESP_LOGI(TAG, "进度: %" PRIu32 " 字节", total_bytes);
            }
        } else if (ctx->last_seq == (uint8_t)(expected_seq - 1)) {
            /* 重复包，重新 ACK */
            xm_put_byte(ctx, XM_ACK);
        } else {
            /* 序号错乱 */
            ESP_LOGW(TAG, "序号错乱: 收到 %u, 期望 %u", ctx->last_seq, expected_seq);
            xm_put_byte(ctx, XM_NAK);
        }

        /* 检查是否已收完 (依据文件长度) */
        if (file_size > 0 && total_bytes >= file_size) {
            /*
             * 等待 EOT 并回 ACK。
             *
             * ⚠️ 超时必须 >= 发送方的 EOT 重试间隔 (XM_PACKET_TIMEOUT)，
             *    否则接收方会在发送方发出 EOT 之前就超时退出，
             *    导致发送方一直等不到 ACK 而报超时。
             */
            uint8_t c;
            uint32_t wait_start = xTaskGetTickCount();
            while (xTaskGetTickCount() - wait_start < pdMS_TO_TICKS(10000)) {
                if (xm_cancelled(ctx)) {
                    break;
                }
                int r = xm_get_byte(ctx, &c, XM_PACKET_TIMEOUT);
                if (r < 0) {
                    break;      /* 取消 */
                }
                if (r > 0) {
                    if (c == XM_EOT) {
                        xm_put_byte(ctx, XM_ACK);
                        finished = true;
                        break;
                    }
                    if (c == XM_SOH || c == XM_STX) {
                        /* 还有额外数据，继续接收 */
                        break;
                    }
                    /* 'C' 等噪声: 忽略，继续等 EOT */
                }
            }
            if (!finished) {
                finished = true;
            }
        }
    }

    /* 等待 EOT 并确认 */
    if (file_size == 0) {
        uint8_t c;
        int r = xm_get_byte(ctx, &c, XM_PACKET_TIMEOUT);
        if (r > 0 && c == XM_EOT) {
            xm_put_byte(ctx, XM_ACK);
        }
    }

    ctx->total_bytes = total_bytes;
    ESP_LOGI(TAG, "XMODEM 接收完成，共 %" PRIu32 " 字节", total_bytes);

out:
    /* 退出时务必恢复: 否则终端后续输入中的 0x03 会被误判 */
    ctx->raw_mode = false;
    iap_uart_xmodem_set_raw_mode(false);
    free(pkt_buf);
    return result;
}

/* -------------------------------------------------------------------------- */
/* 发送实现                                                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief 发送一个数据包
 *
 * @param ctx  上下文
 * @param seq  包序号
 * @param data 数据 (不足 pkt_len 时用 0x1A 填充)
 * @param len  有效数据长度
 * @param pkt_len 包长度 (128 或 1024)
 * @return ESP_OK 收到 ACK
 */
static esp_err_t xm_send_packet(iap_xmodem_ctx_t *ctx, uint8_t seq,
                                const uint8_t *data, size_t len, size_t pkt_len)
{
    uint8_t start = (pkt_len == 1024) ? XM_STX : XM_SOH;
    uint8_t buf[1024 + 5];
    size_t idx = 0;

    buf[idx++] = start;
    buf[idx++] = seq;
    buf[idx++] = (uint8_t)(0xFF - seq);

    memcpy(&buf[idx], data, len);
    idx += len;
    /* 填充 */
    while (idx < pkt_len + 3) {
        buf[idx++] = XM_SUB;
    }

    uint16_t crc = iap_crc16_xmodem(&buf[3], pkt_len);
    buf[idx++] = (uint8_t)(crc >> 8);
    buf[idx++] = (uint8_t)(crc & 0xFF);

    for (int retry = 0; retry < XM_RETRY_MAX; retry++) {
        if (ctx->write(ctx->user, buf, idx) != (int)idx) {
            return ESP_ERR_TIMEOUT;
        }

        /*
         * 读取响应。
         *
         * 'C' (0x43): 接收方在第 0 包后按 YMODEM 约定会再发一个 'C'
         * 请求数据。它**不是**重传信号 —— 必须忽略并继续读，
         * 且**不能重发数据包** (否则每包多发一次，接收方会收到重复包)。
         */
        for (;;) {
            uint8_t resp;
            int r = xm_get_byte(ctx, &resp, XM_PACKET_TIMEOUT);
            if (r < 0) {
                return ESP_ERR_INVALID_STATE;   /* 用户取消 */
            }
            if (r == 0) {
                break;              /* 超时: 外层重发 */
            }
            if (resp == XM_ACK) {
                return ESP_OK;
            }
            if (resp == XM_CAN) {
                ESP_LOGW(TAG, "发送方取消");
                return ESP_ERR_INVALID_STATE;
            }
            if (resp == XM_CRC_CHAR) {
                continue;           /* 忽略 'C'，继续等真正的响应 */
            }
            break;                  /* NAK 或其它: 外层重发 */
        }
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t iap_xmodem_send(iap_xmodem_ctx_t *ctx,
                          const char *file_name,
                          uint32_t file_size,
                          iap_xmodem_read_cb_t on_read,
                          void *user)
{
    if (ctx == NULL || ctx->read == NULL || ctx->write == NULL || on_read == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t *buf = malloc(1024);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = ESP_OK;

    /*
     * 发送方向只读**单字节响应** (ACK/NAK/CAN)，均属控制字符阶段，
     * 因此全程允许介质层检测 Ctrl+C (raw_mode = false)。
     */
    ctx->raw_mode = false;
    iap_uart_xmodem_set_raw_mode(false);

    /* 握手: 等待接收方发送 'C' 或 NAK */
    ESP_LOGI(TAG, "等待接收方握手...");
    uint8_t c = 0;
    uint32_t start = xTaskGetTickCount();
    bool handshake_ok = false;

    while (xTaskGetTickCount() - start < pdMS_TO_TICKS(XM_HANDSHAKE_SEC * 1000)) {
        if (xm_cancelled(ctx)) {
            result = ESP_ERR_INVALID_STATE;
            goto out;
        }
        int r = xm_get_byte(ctx, &c, 1000);
        if (r < 0) {
            result = ESP_ERR_INVALID_STATE;
            goto out;
        }
        if (r > 0) {
            if (c == XM_CRC_CHAR || c == XM_NAK) {
                handshake_ok = true;
                break;
            }
        }
    }

    if (!handshake_ok) {
        ESP_LOGE(TAG, "握手超时");
        result = ESP_ERR_TIMEOUT;
        goto out;
    }

    /* 发送第 0 包: 文件名 + 长度 */
    {
        char info[128];
        memset(info, 0, sizeof(info));
        snprintf(info, sizeof(info), "%s", file_name ? file_name : "user_app.bin");

        size_t name_len = strlen(info) + 1;
        snprintf(info + name_len, sizeof(info) - name_len, "%" PRIu32, file_size);

        esp_err_t err = xm_send_packet(ctx, 0, (const uint8_t *)info,
                                       name_len + strlen(info + name_len) + 1, 128);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "发送文件信息包失败");
            result = err;
            goto out;
        }
    }

    /* 发送数据包 */
    uint32_t offset = 0;
    uint8_t seq = 1;
    uint32_t last_progress = xTaskGetTickCount();   /* 上次成功发包的时刻 */

    while (offset < file_size) {
        if (xm_cancelled(ctx)) {
            xm_send_cancel(ctx);
            result = ESP_ERR_INVALID_STATE;
            goto out;
        }

        /* 无进展超时: 对端掉线/卡死时放弃，避免终端被卡住 */
        if (xTaskGetTickCount() - last_progress >
            pdMS_TO_TICKS(XM_STALL_SEC * 1000)) {
            ESP_LOGE(TAG, "传输停滞 %d 秒，放弃发送", XM_STALL_SEC);
            xm_send_cancel(ctx);
            result = ESP_ERR_TIMEOUT;
            goto out;
        }

        uint32_t remaining = file_size - offset;
        size_t chunk = (remaining > 1024) ? 1024 : remaining;
        size_t pkt_len = (chunk > 128) ? 1024 : 128;

        if (chunk > pkt_len) {
            chunk = pkt_len;
        }

        esp_err_t err = on_read(user, buf, offset, chunk);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "读取数据失败 @%" PRIu32, offset);
            xm_send_cancel(ctx);
            result = err;
            goto out;
        }

        err = xm_send_packet(ctx, seq, buf, chunk, pkt_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "发送包 %u 失败", seq);
            xm_send_cancel(ctx);
            result = err;
            goto out;
        }

        offset += chunk;
        seq++;
        last_progress = xTaskGetTickCount();   /* 发出包，刷新进展 */

        if ((offset & 0x3FFF) < pkt_len) {
            ESP_LOGI(TAG, "进度: %" PRIu32 "/%" PRIu32 " 字节 (%" PRIu32 "%%)",
                     offset, file_size, offset * 100 / file_size);
        }
    }

    /* 发送 EOT */
    for (int retry = 0; retry < XM_RETRY_MAX; retry++) {
        if (xm_cancelled(ctx)) {
            result = ESP_ERR_INVALID_STATE;
            goto out;
        }
        xm_put_byte(ctx, XM_EOT);
        uint8_t resp;
        int r = xm_get_byte(ctx, &resp, XM_PACKET_TIMEOUT);
        if (r < 0) {
            result = ESP_ERR_INVALID_STATE;
            goto out;
        }
        if (r > 0 && resp == XM_ACK) {
            break;
        }
    }

    ESP_LOGI(TAG, "XMODEM 发送完成，共 %" PRIu32 " 字节", file_size);

out:
    free(buf);
    return result;
}
