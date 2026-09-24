/*
 * ESP IAP - CRC 计算接口
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

/*
 * 置 1 强制使用软件 CRC（便于与硬件路径交叉验证）。
 * 默认 0：使用 ROM 硬件 CRC 加速器。
 *   CRC32 -> esp_rom_crc32_le
 *   CRC16 -> esp_rom_crc16_be (需外部取反)
 */
#ifndef IAP_CRC32_SOFTWARE
#define IAP_CRC32_SOFTWARE 0
#endif

#ifndef IAP_CRC16_SOFTWARE
#define IAP_CRC16_SOFTWARE 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 计算 CRC32 (IEEE 802.3, 反射多项式 0xEDB88320)
 *
 * @param data 数据指针，可为 NULL (此时 len 必须为 0)
 * @param len  数据长度
 * @return CRC32 值
 */
uint32_t iap_crc32(const void *data, size_t len);

/**
 * @brief 增量计算 CRC32
 *
 * 首次调用时 crc 传入 0xFFFFFFFF，返回中间值；
 * 全部数据更新完成后需对结果取反 (^ 0xFFFFFFFF)。
 *
 * @param crc  当前 CRC 中间值
 * @param data 数据指针
 * @param len  数据长度
 * @return 更新后的 CRC 中间值
 */
uint32_t iap_crc32_update(uint32_t crc, const void *data, size_t len);

/**
 * @brief 计算 CRC16-CCITT-FALSE (多项式 0x1021, 初值 0xFFFF, 不反射)
 *
 * 默认走 ROM 硬件 CRC (esp_rom_crc16_be)，定义 IAP_CRC16_SOFTWARE=1 可切回软件。
 *
 * @param data 数据指针
 * @param len  数据长度
 * @return CRC16 值
 */
uint16_t iap_crc16(const void *data, size_t len);

/**
 * @brief 增量计算 CRC16-CCITT-FALSE
 *
 * @param crc  当前 CRC 中间值 (首次传入 0xFFFF)
 * @param data 数据指针
 * @param len  数据长度
 * @return 更新后的 CRC 中间值
 */
uint16_t iap_crc16_update(uint16_t crc, const void *data, size_t len);

/**
 * @brief 计算 XMODEM 标准 CRC16 (多项式 0x1021, **初值 0x0000**, 不反射)
 *
 * ⚠️ 与 iap_crc16() 的区别仅在**初值**:
 *      iap_crc16()      → init 0xFFFF (CCITT-FALSE，用于 I2C 帧校验)
 *      iap_crc16_xmodem → init 0x0000 (**XMODEM 协议标准**)
 *
 * 标准 XMODEM 使用 init=0x0000，若误用 0xFFFF 会与所有标准工具
 * (lrzsz 的 sx/rx、Tera Term、SecureCRT 等) 不兼容 —— 每包 CRC 都错。
 *
 * 测试向量: "123456789" → 0x31C3
 *
 * @param data 数据指针
 * @param len  数据长度
 * @return CRC16 值
 */
uint16_t iap_crc16_xmodem(const void *data, size_t len);

/**
 * @brief 增量计算 XMODEM 标准 CRC16 (初值 0x0000)
 *
 * @param crc  当前 CRC 中间值 (首次传入 0x0000)
 * @param data 数据指针
 * @param len  数据长度
 * @return 更新后的 CRC 中间值
 */
uint16_t iap_crc16_xmodem_update(uint16_t crc, const void *data, size_t len);

/**
 * @brief 增量 CRC32 上下文
 *
 * 用于对大块数据 (如整个用户程序分区) 分片计算 CRC，
 * 避免一次性分配大缓冲区。
 */
typedef struct {
    uint32_t crc;       /*!< CRC 中间值 */
    uint32_t total;     /*!< 已累计的字节数 */
} iap_crc32_ctx_t;

/**
 * @brief 初始化增量 CRC32 上下文
 */
void iap_crc32_ctx_init(iap_crc32_ctx_t *ctx);

/**
 * @brief 追加数据到增量 CRC32 上下文
 */
void iap_crc32_ctx_update(iap_crc32_ctx_t *ctx, const void *data, size_t len);

/**
 * @brief 结束增量 CRC32 计算并返回最终值
 */
uint32_t iap_crc32_ctx_final(iap_crc32_ctx_t *ctx);

#ifdef __cplusplus
}
#endif
