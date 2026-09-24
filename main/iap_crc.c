/*
 * ESP IAP - CRC 计算
 *
 * 提供 CRC32 (IEEE 802.3, 反射多项式 0xEDB88320) 与
 * CRC16-CCITT (多项式 0x1021) 两种校验算法。
 *
 * CRC32 用于用户程序与配置区的完整性校验。
 * CRC16 用于 I2C 帧校验 (见 I2C 帧格式定义)。
 *
 * 实现说明:
 *   - CRC32 优先使用 ROM 硬件 CRC 加速器 (esp_rom_crc32_le)，速度远快于查表。
 *     定义 IAP_CRC32_SOFTWARE=1 可强制使用软件查表 (便于交叉验证)。
 *   - CRC16 无硬件对应 (ROM 的 crc16_be 语义需取反)，保留按位软件实现。
 */

#include "iap_crc.h"
#include <string.h>

#if !IAP_CRC32_SOFTWARE
#include "esp_rom_crc.h"
#endif

/* ============================================================================
 * CRC32 (IEEE 802.3)
 *
 * 硬件路径: ROM 的 esp_rom_crc32_le 内部在进入时取反、返回时再取反，
 *           因此首次传 0 即等价于标准 init=0xFFFFFFFF / xorout=0xFFFFFFFF。
 *           等价关系已用主机端逐字节验证:
 *             iap_crc32(data,len) == esp_rom_crc32_le(0, data, len)
 *
 * 软件路径: 4bit 查表法，兼顾速度与代码体积，适合嵌入式环境。
 * ========================================================================== */

/** 置 1 强制使用软件 CRC32 (默认 0，使用 ROM 硬件 CRC) */
#ifndef IAP_CRC32_SOFTWARE
#define IAP_CRC32_SOFTWARE 0
#endif

#if IAP_CRC32_SOFTWARE
/** 半字节查表: 索引为 4bit 数据，值为对应的 CRC 贡献 */
static const uint32_t s_crc32_nibble_table[16] = {
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC,
    0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
    0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
};
#endif

uint32_t iap_crc32_update(uint32_t crc, const void *data, size_t len)
{
#if IAP_CRC32_SOFTWARE
    const uint8_t *p = (const uint8_t *)data;

    while (len--) {
        crc ^= *p++;
        /* 低 4 bit */
        crc = (crc >> 4) ^ s_crc32_nibble_table[crc & 0x0F];
        /* 高 4 bit */
        crc = (crc >> 4) ^ s_crc32_nibble_table[crc & 0x0F];
    }
    return crc;
#else
    return esp_rom_crc32_le(crc, (const uint8_t *)data, (uint32_t)len);
#endif
}

uint32_t iap_crc32(const void *data, size_t len)
{
#if IAP_CRC32_SOFTWARE
    return iap_crc32_update(0xFFFFFFFFU, data, len) ^ 0xFFFFFFFFU;
#else
    /* ROM 版内部已处理取反，传 0 即标准语义 */
    return esp_rom_crc32_le(0, (const uint8_t *)data, (uint32_t)len);
#endif
}

/* ============================================================================
 * CRC16-CCITT-FALSE (多项式 0x1021, 初值 0xFFFF, 不反射, 无输出异或)
 *
 * 用于 I2C 帧校验，帧格式要求多项式 0x1021。
 *
 * 硬件路径: ROM 的 esp_rom_crc16_be 内部在进入时取反、返回时再取反，
 *           而 CCITT-FALSE 本身不做任何取反，因此需要外部再取反一次。
 *
 *           等价关系 (已用主机端逐字节验证):
 *             一次性: iap_crc16(d,len)        == ~esp_rom_crc16_be(0, d, len)
 *             增量  : iap_crc16_update(c,d,len) == ~esp_rom_crc16_be(~c, d, len)
 *
 *           注意增量式是**双重取反**：ROM 会对入参取反一次，所以传入前
 *           要先自行取反，才能让 ROM 内部拿到正确的原始中间值。
 *
 * 软件路径: 按位实现，语义与硬件完全一致 (已验证 6/6 相同)。
 * ========================================================================== */

/** 置 1 强制使用软件 CRC16 (默认 0，使用 ROM 硬件 CRC) */
#ifndef IAP_CRC16_SOFTWARE
#define IAP_CRC16_SOFTWARE 0
#endif

#if IAP_CRC16_SOFTWARE
uint16_t iap_crc16_update(uint16_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    while (len--) {
        crc ^= (uint16_t)(*p++) << 8;
        for (int i = 0; i < 8; i++) {
            if (crc & 0x8000) {
                crc = (uint16_t)((crc << 1) ^ 0x1021);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}
#else
uint16_t iap_crc16_update(uint16_t crc, const void *data, size_t len)
{
    /* ROM 内部会取反入参，故先自行取反以传入正确的原始中间值 */
    return (uint16_t)(~esp_rom_crc16_be((uint16_t)~crc,
                                        (const uint8_t *)data, (uint32_t)len));
}
#endif

uint16_t iap_crc16(const void *data, size_t len)
{
    /* 初值 0xFFFF；硬件路径下 ~0xFFFF == 0，正好等价于 ~esp_rom_crc16_be(0,...) */
    return iap_crc16_update(0xFFFF, data, len);
}

/* ============================================================================
 * XMODEM 标准 CRC16 (初值 0x0000)
 *
 * ⚠️ 与 iap_crc16() 的唯一区别是**初值**:
 *      iap_crc16()      → 0xFFFF (CCITT-FALSE，I2C 帧校验用)
 *      iap_crc16_xmodem → 0x0000 (XMODEM 协议标准)
 *
 * 标准 XMODEM 规定 init=0x0000，若误用 0xFFFF 则与所有标准工具
 * (lrzsz sx/rx、Tera Term 等) 不兼容 —— 每包 CRC 校验都失败。
 *
 * 测试向量: "123456789" → 0x31C3
 *
 * 实现: 纯软件按位 (硬件 ROM 的 crc16_be 语义不便拆出 init=0 的路径)。
 * ========================================================================== */

uint16_t iap_crc16_xmodem_update(uint16_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    while (len--) {
        crc ^= (uint16_t)(*p++) << 8;
        for (int i = 0; i < 8; i++) {
            if (crc & 0x8000) {
                crc = (uint16_t)((crc << 1) ^ 0x1021);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

uint16_t iap_crc16_xmodem(const void *data, size_t len)
{
    return iap_crc16_xmodem_update(0x0000, data, len);
}

/* ============================================================================
 * 增量 CRC32 上下文
 *
 * 注意: 硬件与软件路径的"中间值"语义不同:
 *   - 软件路径: 中间值就是原始 CRC 寄存器值，初值 0xFFFFFFFF，末尾需取反
 *   - ROM 路径: 函数内部自行取反，链式调用时把上次返回值直接传回即可，
 *              因此初值用 0 (等价于软件路径的 0xFFFFFFFF)
 * 这里通过 ctx->crc 统一保存"可直接喂给下一次调用"的值。
 * ========================================================================== */

void iap_crc32_ctx_init(iap_crc32_ctx_t *ctx)
{
#if IAP_CRC32_SOFTWARE
    ctx->crc = 0xFFFFFFFFU;
#else
    ctx->crc = 0;   /* ROM 语义: 首次传 0 */
#endif
    ctx->total = 0;
}

void iap_crc32_ctx_update(iap_crc32_ctx_t *ctx, const void *data, size_t len)
{
    ctx->crc = iap_crc32_update(ctx->crc, data, len);
    ctx->total += len;
}

uint32_t iap_crc32_ctx_final(iap_crc32_ctx_t *ctx)
{
#if IAP_CRC32_SOFTWARE
    return ctx->crc ^ 0xFFFFFFFFU;
#else
    return ctx->crc;   /* ROM 版已在返回时取反 */
#endif
}
