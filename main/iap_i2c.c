/*
 * ESP IAP - I2C 从机协议实现
 *
 * 帧格式 (与需求文档一致):
 *
 *   偏移   长度   字段
 *   0-1     2     magic
 *   2[0-4]  5bit  version (固定 0x01)
 *   2[5]    1bit  reserved (发送端必须为 0)
 *   2[6-7]  2bit  fragment: 单包 0b10, 多包非末包 0b01, 多包末包 0b11
 *   3       1     reserved (发送端必须为 0)
 *   4-7     4     message_id (uint32 LE)，同一 message_id 属于同一消息
 *   8       1     packet_index
 *   9       1     packet_count
 *   10-11   2     payload_length (uint16 LE)，记为 n
 *   12-13   2     reserved (发送端必须为 0)
 *   14-15   2     crc16 (多项式 0x1021, LE)
 *                 校验范围: [0..13] 与 [16..n+16)
 *   16..     n    负载
 *
 * 重组后的消息负载:
 *   TYPE(2) RESERVED(2) REQUEST_ID(4) CONTEXT(n-8)
 *
 * 本模块负责:
 *   - 帧的解析与校验
 *   - 多包重组
 *   - 命令分发 (TYPE)
 *   - 响应的分帧与发送
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "spi_flash_mmap.h"     /* SPI_FLASH_SEC_SIZE (v6) */
#include "esp_mac.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "driver/i2c_slave.h"
#include "driver/gpio.h"

#include "iap_common.h"
#include "iap_crc.h"
#include "iap_config.h"
#include "iap_image.h"
#include "iap_wait_trigger.h"
#include "iap_i2c.h"

static const char *TAG = "iap_i2c";

/*
 * 芯片能力检查: I2C 从机
 *
 * 部分芯片 (如 ESP32-C2) 只有 I2C 主机，没有从机硬件，
 * 此时 IDF 不会编译 i2c_slave.c，链接会失败。
 *
 * 处理方式: 整个模块退化为空实现，iap_i2c_start() 返回
 * ESP_ERR_NOT_SUPPORTED，等待触发中的 I2C 触发自动禁用。
 */
#if !SOC_I2C_SUPPORT_SLAVE

esp_err_t iap_i2c_start(void)
{
    ESP_LOGW(TAG, "本芯片不支持 I2C 从机，I2C 通道已禁用");
    return ESP_ERR_NOT_SUPPORTED;
}

#else  /* SOC_I2C_SUPPORT_SLAVE */

/* ============================================================================
 * 协议常量
 * ========================================================================== */

#define I2C_FRAME_MAGIC           0x4950U   /*!< "IP" 小端 */
#define I2C_FRAME_VERSION         0x01
#define I2C_FRAME_HEADER_SIZE     16        /*!< 帧头长度 */
#define I2C_FRAME_MAX_PAYLOAD     1024      /*!< 单帧最大负载 (受 I2C 缓冲限制) */
#define I2C_MSG_MAX_SIZE          (64 * 1024) /*!< 重组后消息最大长度 */

/* fragment 字段 */
#define I2C_FRAG_SINGLE           0x02      /*!< 0b10 单包 */
#define I2C_FRAG_FIRST_MID        0x01      /*!< 0b01 多包非末包 */
#define I2C_FRAG_LAST             0x03      /*!< 0b11 多包末包 */

/* 消息头部长度: TYPE(2) + RESERVED(2) + REQUEST_ID(4) */
#define I2C_MSG_HEADER_SIZE       8

/* ============================================================================
 * 命令类型 (TYPE)
 * ========================================================================== */

typedef enum {
    /* --- 系统信息 --- */
    I2C_TYPE_GET_SYS_INFO       = 0x0001,   /*!< 获取系统信息 */
    I2C_TYPE_GET_CFG_INFO       = 0x0002,   /*!< 获取 IAP 配置信息 */
    I2C_TYPE_SET_CFG_INFO       = 0x0003,   /*!< 设置 IAP 配置信息 */
    I2C_TYPE_GET_APP_INFO       = 0x0004,   /*!< 获取用户程序信息 */
    I2C_TYPE_GET_STATUS         = 0x0005,   /*!< 获取 IAP 状态 */
    I2C_TYPE_GET_BOOT_PARAM     = 0x0006,   /*!< 获取启动参数 */
    I2C_TYPE_SET_BOOT_PARAM     = 0x0007,   /*!< 设置启动参数 */

    /* --- 用户分区读写 --- */
    I2C_TYPE_FLASH_BEGIN        = 0x0010,   /*!< 开始烧录 (擦除) */
    I2C_TYPE_FLASH_DATA         = 0x0011,   /*!< 写入数据 */
    I2C_TYPE_FLASH_END          = 0x0012,   /*!< 结束烧录 (校验) */
    I2C_TYPE_FLASH_ABORT        = 0x0013,   /*!< 中止烧录 */

    I2C_TYPE_READ_PARTITION     = 0x0020,   /*!< 读取用户分区指定区域 */
    I2C_TYPE_WRITE_PARTITION    = 0x0021,   /*!< 写入用户分区指定区域 */
    I2C_TYPE_ERASE_PARTITION    = 0x0022,   /*!< 擦除用户分区指定区域 */
    I2C_TYPE_VERIFY_PARTITION   = 0x0023,   /*!< 校验用户分区 */

    /* --- 启动控制 --- */
    I2C_TYPE_BOOT_USER_APP      = 0x0030,   /*!< 重启进入用户程序 (当前活动槽) */
    I2C_TYPE_BOOT_IAP           = 0x0031,   /*!< 重启进入 IAP */
    I2C_TYPE_SET_DOWNLOAD_MODE  = 0x0032,   /*!< 设置下载模式标志 */
    I2C_TYPE_RESET_CFG          = 0x0033,   /*!< 恢复默认配置 */
    I2C_TYPE_REBOOT             = 0x0034,   /*!< 重启设备 */

    /* --- 多 OTA 槽 --- */
    I2C_TYPE_GET_SLOT_LIST      = 0x0040,   /*!< 获取 OTA 槽列表 */
    I2C_TYPE_GET_ACTIVE_SLOT    = 0x0041,   /*!< 获取当前活动槽 */
    I2C_TYPE_SET_ACTIVE_SLOT    = 0x0042,   /*!< 设置活动槽 */
    I2C_TYPE_BOOT_SLOT          = 0x0043,   /*!< 切换并启动指定槽 */

    /* --- 通用 --- */
    I2C_TYPE_PING               = 0x00F0,   /*!< 心跳 */
    I2C_TYPE_ACK                = 0x00F1,   /*!< 通用应答 */
    I2C_TYPE_NACK               = 0x00F2,   /*!< 通用否认 */
} iap_i2c_cmd_t;

/* ============================================================================
 * 状态码
 * ========================================================================== */

typedef enum {
    I2C_STATUS_OK               = 0x0000,   /*!< 成功 */
    I2C_STATUS_ERR_ARG          = 0x0001,   /*!< 参数错误 */
    I2C_STATUS_ERR_STATE        = 0x0002,   /*!< 状态错误 */
    I2C_STATUS_ERR_RANGE        = 0x0003,   /*!< 越界 */
    I2C_STATUS_ERR_FLASH        = 0x0004,   /*!< Flash 操作失败 */
    I2C_STATUS_ERR_CRC          = 0x0005,   /*!< CRC 校验失败 */
    I2C_STATUS_ERR_NO_MEM       = 0x0006,   /*!< 内存不足 */
    I2C_STATUS_ERR_NO_APP       = 0x0007,   /*!< 无有效用户程序 */
    I2C_STATUS_ERR_BUSY         = 0x0008,   /*!< 忙 */
    I2C_STATUS_ERR_UNSUPPORTED  = 0x0009,   /*!< 不支持的命令 */
} iap_i2c_status_t;

/* ============================================================================
 * 全局状态
 * ========================================================================== */

/** I2C 从机句柄 */
static i2c_slave_dev_handle_t s_slave = NULL;

/** 接收队列: ISR 收到数据后投递到队列，由任务处理 */
static QueueHandle_t s_rx_queue = NULL;

/** 发送缓冲: 主设备读取时从此缓冲取数据 */
static uint8_t *s_tx_buf = NULL;
static size_t   s_tx_len = 0;
static size_t   s_tx_pos = 0;
static SemaphoreHandle_t s_tx_lock = NULL;

/** 重组缓冲 */
static uint8_t *s_msg_buf = NULL;
static size_t   s_msg_len = 0;
static uint32_t s_msg_id = 0;
static uint8_t  s_msg_next_index = 0;
static uint8_t  s_msg_packet_count = 0;

/** 烧录会话状态 */
static struct {
    bool     active;            /*!< 是否处于烧录会话中 */
    uint32_t offset;            /*!< 当前写入偏移 */
    uint32_t total_size;        /*!< 期望总长度 */
    uint32_t max_size;          /*!< 允许的最大长度 */
    iap_target_t target;        /*!< 目标区域 */
    uint32_t base_offset;       /*!< 目标区域内的基准偏移 */
    iap_crc32_ctx_t crc;        /*!< 增量 CRC */
} s_session;

/* ============================================================================
 * 帧解析
 * ========================================================================== */

/**
 * @brief 校验并解析帧头
 *
 * @param frame 帧数据
 * @param len   帧长度
 * @param hdr   输出解析结果
 * @return ESP_OK 成功
 */
static esp_err_t frame_parse(const uint8_t *frame, size_t len, iap_i2c_frame_t *hdr)
{
    if (len < I2C_FRAME_HEADER_SIZE) {
        ESP_LOGW(TAG, "帧太短: %u", (unsigned)len);
        return ESP_ERR_INVALID_SIZE;
    }

    /* magic */
    uint16_t magic = iap_rd_le16(frame + 0);
    if (magic != I2C_FRAME_MAGIC) {
        ESP_LOGW(TAG, "magic 错误: 0x%04X", magic);
        return ESP_ERR_INVALID_ARG;
    }

    /* byte 2: version(5bit) | reserved(1bit) | fragment(2bit) */
    uint8_t b2 = frame[2];
    uint8_t version  = (b2 >> 3) & 0x1F;
    uint8_t reserved = (b2 >> 2) & 0x01;
    uint8_t fragment = b2 & 0x03;

    if (version != I2C_FRAME_VERSION) {
        ESP_LOGW(TAG, "版本不支持: 0x%02X", version);
        return ESP_ERR_INVALID_VERSION;
    }
    if (reserved != 0) {
        ESP_LOGW(TAG, "byte2 保留位非 0");
        return ESP_ERR_INVALID_ARG;
    }
    if (fragment != I2C_FRAG_SINGLE &&
        fragment != I2C_FRAG_FIRST_MID &&
        fragment != I2C_FRAG_LAST) {
        ESP_LOGW(TAG, "fragment 非法: 0x%02X", fragment);
        return ESP_ERR_INVALID_ARG;
    }

    /* byte 3: reserved */
    if (frame[3] != 0) {
        ESP_LOGW(TAG, "byte3 保留位非 0");
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t message_id   = iap_rd_le32(frame + 4);
    uint8_t  packet_index = frame[8];
    uint8_t  packet_count = frame[9];
    uint16_t payload_len  = iap_rd_le16(frame + 10);

    /* 帧长度必须匹配 */
    if (len != (size_t)(I2C_FRAME_HEADER_SIZE + payload_len)) {
        ESP_LOGW(TAG, "帧长度不匹配: 实际 %u, 声明 %u",
                 (unsigned)len, (unsigned)(I2C_FRAME_HEADER_SIZE + payload_len));
        return ESP_ERR_INVALID_SIZE;
    }

    if (payload_len > I2C_FRAME_MAX_PAYLOAD) {
        ESP_LOGW(TAG, "负载超限: %u", payload_len);
        return ESP_ERR_INVALID_SIZE;
    }

    /* 分片字段与包序号一致性检查 */
    if (fragment == I2C_FRAG_SINGLE && packet_count != 1) {
        ESP_LOGW(TAG, "单包但 packet_count=%u", packet_count);
        return ESP_ERR_INVALID_ARG;
    }
    if (fragment != I2C_FRAG_SINGLE && packet_count < 2) {
        ESP_LOGW(TAG, "多包但 packet_count=%u", packet_count);
        return ESP_ERR_INVALID_ARG;
    }
    if (packet_index >= packet_count) {
        ESP_LOGW(TAG, "packet_index %u >= packet_count %u", packet_index, packet_count);
        return ESP_ERR_INVALID_ARG;
    }

    /* CRC16: 校验 [0..13] 与 [16..16+n) */
    uint16_t crc_recv = iap_rd_le16(frame + 14);
    uint16_t crc_calc = iap_crc16(frame, 14);
    if (payload_len > 0) {
        crc_calc = iap_crc16_update(crc_calc, frame + I2C_FRAME_HEADER_SIZE, payload_len);
    }

    if (crc_recv != crc_calc) {
        ESP_LOGW(TAG, "帧 CRC 错误: recv=0x%04X calc=0x%04X", crc_recv, crc_calc);
        return ESP_ERR_INVALID_CRC;
    }

    /* 输出 */
    hdr->magic        = magic;
    hdr->version      = version;
    hdr->fragment     = fragment;
    hdr->message_id   = message_id;
    hdr->packet_index = packet_index;
    hdr->packet_count = packet_count;
    hdr->payload_len  = payload_len;
    hdr->payload      = frame + I2C_FRAME_HEADER_SIZE;

    return ESP_OK;
}

/**
 * @brief 组装一个待发送的帧
 *
 * @param out       输出缓冲区 (至少 16 + payload_len)
 * @param message_id 消息号
 * @param fragment  分片标志
 * @param index     包序号
 * @param count     包总数
 * @param payload   负载
 * @param payload_len 负载长度
 * @return 帧总长度
 */
static size_t frame_build(uint8_t *out, uint32_t message_id, uint8_t fragment,
                          uint8_t index, uint8_t count,
                          const uint8_t *payload, uint16_t payload_len)
{
    memset(out, 0, I2C_FRAME_HEADER_SIZE);

    iap_wr_le16(out + 0, I2C_FRAME_MAGIC);
    out[2] = (uint8_t)((I2C_FRAME_VERSION << 3) | (fragment & 0x03));
    out[3] = 0;
    iap_wr_le32(out + 4, message_id);
    out[8] = index;
    out[9] = count;
    iap_wr_le16(out + 10, payload_len);
    /* 12-13 已由 memset 置 0 */

    if (payload_len > 0 && payload != NULL) {
        memcpy(out + I2C_FRAME_HEADER_SIZE, payload, payload_len);
    }

    uint16_t crc = iap_crc16(out, 14);
    if (payload_len > 0) {
        crc = iap_crc16_update(crc, out + I2C_FRAME_HEADER_SIZE, payload_len);
    }
    iap_wr_le16(out + 14, crc);

    return I2C_FRAME_HEADER_SIZE + payload_len;
}

/* ============================================================================
 * 响应发送
 * ========================================================================== */

/**
 * @brief 将一段数据放入发送缓冲 (供主设备读取)
 *
 * 若数据超过单帧容量，自动分帧。
 *
 * @param message_id 消息号
 * @param data       数据
 * @param len        数据长度
 * @return ESP_OK 成功
 */
static esp_err_t tx_prepare(uint32_t message_id, const uint8_t *data, size_t len)
{
    const size_t frame_max = I2C_FRAME_MAX_PAYLOAD;

    /* 计算所需帧数 (最多 255 帧) */
    size_t count_sz = (len + frame_max - 1) / frame_max;
    if (count_sz == 0) {
        count_sz = 1;
    }
    if (count_sz > 255) {
        ESP_LOGE(TAG, "响应过长: %u", (unsigned)len);
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t count = (uint8_t)count_sz;

    size_t total = 0;
    for (uint8_t i = 0; i < count; i++) {
        size_t off = (size_t)i * frame_max;
        size_t remain = len - off;
        uint16_t chunk = (uint16_t)((remain > frame_max) ? frame_max : remain);

        uint8_t fragment;
        if (count == 1) {
            fragment = I2C_FRAG_SINGLE;
        } else if (i == count - 1) {
            fragment = I2C_FRAG_LAST;
        } else {
            fragment = I2C_FRAG_FIRST_MID;
        }

        total += frame_build(s_tx_buf + total, message_id, fragment, i, count,
                             (chunk > 0) ? (data + off) : NULL, chunk);
    }

    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    s_tx_len = total;
    s_tx_pos = 0;
    xSemaphoreGive(s_tx_lock);

    ESP_LOGD(TAG, "响应已就绪: %u 帧, %u 字节", count, (unsigned)total);
    return ESP_OK;
}

/**
 * @brief 发送一个命令应答
 *
 * 应答负载格式: TYPE(2) RESERVED(2) REQUEST_ID(4) CONTEXT(...)
 * 其中 TYPE 为 I2C_TYPE_ACK / I2C_TYPE_NACK，
 * CONTEXT 前 2 字节为状态码。
 *
 * @param request_id 请求号 (回显)
 * @param status     状态码
 * @param data       附加数据，可为 NULL
 * @param len        附加数据长度
 * @return ESP_OK 成功
 */
static esp_err_t send_response(uint32_t request_id, iap_i2c_status_t status,
                               const uint8_t *data, size_t len)
{
    size_t ctx_len = 2 + len;   /* 状态码 (2) + 附加数据 */
    size_t msg_len = I2C_MSG_HEADER_SIZE + ctx_len;

    uint8_t *msg = malloc(msg_len);
    if (msg == NULL) {
        return ESP_ERR_NO_MEM;
    }

    uint16_t type = (status == I2C_STATUS_OK) ? I2C_TYPE_ACK : I2C_TYPE_NACK;
    iap_wr_le16(msg + 0, type);
    iap_wr_le16(msg + 2, 0);
    iap_wr_le32(msg + 4, request_id);
    iap_wr_le16(msg + 8, (uint16_t)status);
    if (len > 0 && data != NULL) {
        memcpy(msg + 10, data, len);
    }

    esp_err_t err = tx_prepare(request_id, msg, msg_len);
    free(msg);
    return err;
}

/* ============================================================================
 * 命令处理
 * ========================================================================== */

/**
 * @brief 处理 GET_SYS_INFO
 *
 * 返回: chip_model(4) chip_rev(2) cores(1) flash_size(4)
 *       heap_free(4) heap_min(4) uptime_ms(4) idf_ver(32) mac(6)
 */
static esp_err_t cmd_get_sys_info(uint32_t request_id,
                                  const uint8_t *ctx, size_t ctx_len)
{
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));

    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    iap_wr_le32(buf + 0, (uint32_t)chip.model);
    iap_wr_le16(buf + 4, (uint16_t)chip.revision);
    buf[6] = (uint8_t)chip.cores;
    iap_wr_le32(buf + 7, flash_size);

    iap_wr_le32(buf + 11, (uint32_t)esp_get_free_heap_size());
    iap_wr_le32(buf + 15, (uint32_t)esp_get_minimum_free_heap_size());
    iap_wr_le32(buf + 19, (uint32_t)(esp_timer_get_time() / 1000));

    strncpy((char *)buf + 23, esp_get_idf_version(), 31);

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    memcpy(buf + 55, mac, 6);

    return send_response(request_id, I2C_STATUS_OK, buf, sizeof(buf));
}

/**
 * @brief 处理 GET_CFG_INFO
 *
 * 返回完整 iap_cfg_data_t
 */
static esp_err_t cmd_get_cfg_info(uint32_t request_id,
                                  const uint8_t *ctx, size_t ctx_len)
{
    iap_cfg_data_t cfg;
    esp_err_t err = iap_config_get(&cfg);
    if (err != ESP_OK) {
        return send_response(request_id, I2C_STATUS_ERR_STATE, NULL, 0);
    }
    return send_response(request_id, I2C_STATUS_OK,
                         (const uint8_t *)&cfg, sizeof(cfg));
}

/**
 * @brief 处理 SET_CFG_INFO
 *
 * CONTEXT: iap_cfg_data_t
 */
static esp_err_t cmd_set_cfg_info(uint32_t request_id,
                                  const uint8_t *ctx, size_t ctx_len)
{
    if (ctx_len < sizeof(iap_cfg_data_t)) {
        return send_response(request_id, I2C_STATUS_ERR_ARG, NULL, 0);
    }

    esp_err_t err = iap_config_set((const iap_cfg_data_t *)ctx);
    if (err != ESP_OK) {
        return send_response(request_id, I2C_STATUS_ERR_FLASH, NULL, 0);
    }
    return send_response(request_id, I2C_STATUS_OK, NULL, 0);
}

/**
 * @brief 处理 GET_APP_INFO
 *
 * 返回: valid(1) image_size(4) total_size(4) crc32(4) version(4)
 *       timestamp(4) project_name(32) version_string(32) chip_name(16)
 */
static esp_err_t cmd_get_app_info(uint32_t request_id,
                                  const uint8_t *ctx, size_t ctx_len)
{
    iap_image_info_t info;
    iap_image_get_info(&info);

    uint8_t buf[96];
    memset(buf, 0, sizeof(buf));

    /* 无自定义文件头后，只返回存在性与配置区元数据 */
    buf[0] = info.present ? 1 : 0;
    iap_wr_le32(buf + 1, info.image_size);
    iap_wr_le32(buf + 5, 0);                 /* total_size: 已废弃，填 0 */
    iap_wr_le32(buf + 9, info.image_crc32);
    iap_wr_le32(buf + 13, info.app_version);
    iap_wr_le32(buf + 17, 0);                /* timestamp: 已废弃，填 0 */
    /* project_name / version_string / chip_name 已废弃，保持全 0 */

    return send_response(request_id, I2C_STATUS_OK, buf, sizeof(buf));
}

/**
 * @brief 处理 GET_STATUS
 *
 * 返回: session_active(1) session_offset(4) session_total(4)
 *       download_mode(1) boot_reason(4)
 */
static esp_err_t cmd_get_status(uint32_t request_id,
                                const uint8_t *ctx, size_t ctx_len)
{
    iap_cfg_data_t cfg;
    iap_config_get(&cfg);

    uint8_t buf[16];
    memset(buf, 0, sizeof(buf));

    buf[0] = s_session.active ? 1 : 0;
    iap_wr_le32(buf + 1, s_session.offset);
    iap_wr_le32(buf + 5, s_session.total_size);
    buf[9] = (cfg.flags & IAP_CFG_FLAG_DOWNLOAD_MODE) ? 1 : 0;
    iap_wr_le32(buf + 10, cfg.last_boot_reason);

    return send_response(request_id, I2C_STATUS_OK, buf, sizeof(buf));
}

/**
 * @brief 处理 GET_BOOT_PARAM
 *
 * 响应 CONTEXT: flags(1) length(2, 小端) + 参数字符串 (不含 '\0')
 */
static esp_err_t cmd_get_boot_param(uint32_t request_id,
                                    const uint8_t *ctx, size_t ctx_len)
{
    /* 缓冲区较大 (256+3)，使用静态存储避免占用任务栈 */
    static char param[IAP_CFG_BOOT_PARAM_SIZE];
    static uint8_t buf[3 + IAP_CFG_BOOT_PARAM_SIZE];

    esp_err_t err = iap_config_get_boot_param(param, sizeof(param));

    iap_cfg_data_t cfg;
    iap_config_get(&cfg);

    buf[0] = cfg.boot_param_flags;

    if (err == ESP_OK) {
        size_t len = strlen(param);
        iap_wr_le16(buf + 1, (uint16_t)len);
        memcpy(buf + 3, param, len);
        return send_response(request_id, I2C_STATUS_OK, buf, 3 + len);
    }

    /* 未设置或读取失败: 返回空参数 */
    iap_wr_le16(buf + 1, 0);
    return send_response(request_id,
                         (err == ESP_ERR_NOT_FOUND) ? I2C_STATUS_OK
                                                    : I2C_STATUS_ERR_STATE,
                         buf, 3);
}

/**
 * @brief 处理 SET_BOOT_PARAM
 *
 * CONTEXT: 参数字符串 (不含 '\0')
 */
static esp_err_t cmd_set_boot_param(uint32_t request_id,
                                    const uint8_t *ctx, size_t ctx_len)
{
    if (ctx_len == 0) {
        /* 空 CONTEXT 表示清除 */
        esp_err_t err = iap_config_clear_boot_param();
        return send_response(request_id,
                             (err == ESP_OK) ? I2C_STATUS_OK : I2C_STATUS_ERR_FLASH,
                             NULL, 0);
    }

    if (ctx_len > IAP_CFG_BOOT_PARAM_MAX_LEN) {
        ESP_LOGW(TAG, "启动参数过长: %u", (unsigned)ctx_len);
        return send_response(request_id, I2C_STATUS_ERR_RANGE, NULL, 0);
    }

    /* 复制到临时缓冲区并补 '\0' (静态存储，避免占用任务栈) */
    static char param[IAP_CFG_BOOT_PARAM_SIZE];
    memcpy(param, ctx, ctx_len);
    param[ctx_len] = '\0';

    esp_err_t err = iap_config_set_boot_param(param);
    return send_response(request_id,
                         (err == ESP_OK) ? I2C_STATUS_OK : I2C_STATUS_ERR_FLASH,
                         NULL, 0);
}

/**
 * @brief 处理 FLASH_BEGIN
 *
 * CONTEXT: total_size(4) target(1)
 *
 * v6: 烧录**从 0x140000 开始的完整镜像**（分区表 B + 应用镜像），
 *     整段写入可变区（与 esptool 语义一致）。
 */
static esp_err_t cmd_flash_begin(uint32_t request_id,
                                 const uint8_t *ctx, size_t ctx_len)
{
    if (ctx_len < 5) {
        return send_response(request_id, I2C_STATUS_ERR_ARG, NULL, 0);
    }
    if (s_session.active) {
        return send_response(request_id, I2C_STATUS_ERR_BUSY, NULL, 0);
    }

    uint32_t total_size = iap_rd_le32(ctx);
    uint8_t  target     = ctx[4];

    /* 可变区容量 */
    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK || flash_size == 0) {
        return send_response(request_id, I2C_STATUS_ERR_STATE, NULL, 0);
    }
    uint32_t max_size = flash_size - IAP_FIXED_REGION_END;

    if (total_size == 0 || total_size > max_size) {
        ESP_LOGW(TAG, "烧录长度非法: %" PRIu32 " (最大 %" PRIu32 ")",
                 total_size, max_size);
        return send_response(request_id, I2C_STATUS_ERR_RANGE, NULL, 0);
    }

    /* 擦除可变区 */
    uint32_t erase_len = (total_size + SPI_FLASH_SEC_SIZE - 1)
                       / SPI_FLASH_SEC_SIZE * SPI_FLASH_SEC_SIZE;
    if (erase_len > max_size) {
        erase_len = max_size;
    }
    ESP_LOGI(TAG, "擦除可变区 0x%06X (%" PRIu32 " 字节)...",
             (unsigned)IAP_FIXED_REGION_END, erase_len);
    esp_err_t err = esp_flash_erase_region(NULL, IAP_FIXED_REGION_END, erase_len);
    if (err != ESP_OK) {
        return send_response(request_id, I2C_STATUS_ERR_FLASH, NULL, 0);
    }

    /* 初始化会话 */
    memset(&s_session, 0, sizeof(s_session));
    s_session.active     = true;
    s_session.offset     = 0;
    s_session.total_size = total_size;
    s_session.max_size   = max_size;
    s_session.target     = (iap_target_t)target;
    s_session.base_offset = IAP_FIXED_REGION_END;   /* v6: 可变区基址 */
    iap_crc32_ctx_init(&s_session.crc);

    ESP_LOGI(TAG, "烧录会话开始: 长度 %" PRIu32 ", 目标 %u (可变区整段写)",
             total_size, target);
    return send_response(request_id, I2C_STATUS_OK, NULL, 0);
}

/**
 * @brief 处理 FLASH_DATA
 *
 * CONTEXT: offset(4) + 数据
 */
static esp_err_t cmd_flash_data(uint32_t request_id,
                                const uint8_t *ctx, size_t ctx_len)
{
    if (!s_session.active) {
        return send_response(request_id, I2C_STATUS_ERR_STATE, NULL, 0);
    }
    if (ctx_len < 4) {
        return send_response(request_id, I2C_STATUS_ERR_ARG, NULL, 0);
    }

    uint32_t offset = iap_rd_le32(ctx);
    const uint8_t *data = ctx + 4;
    size_t data_len = ctx_len - 4;

    if (data_len == 0) {
        return send_response(request_id, I2C_STATUS_ERR_ARG, NULL, 0);
    }
    if (offset > s_session.max_size || data_len > s_session.max_size - offset) {
        ESP_LOGW(TAG, "写入越界: 0x%" PRIx32 " + %u", offset, (unsigned)data_len);
        return send_response(request_id, I2C_STATUS_ERR_RANGE, NULL, 0);
    }

    /* v6: 写入可变区绝对地址 (base_offset = 0x140000) */
    esp_err_t err = esp_flash_write(NULL, data,
                                    s_session.base_offset + offset, data_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写入失败: %s", esp_err_to_name(err));
        return send_response(request_id, I2C_STATUS_ERR_FLASH, NULL, 0);
    }

    /* 顺序写入时更新 CRC */
    if (offset == s_session.offset) {
        iap_crc32_ctx_update(&s_session.crc, data, data_len);
        s_session.offset += data_len;
    }

    /* 返回当前进度 */
    uint8_t buf[4];
    iap_wr_le32(buf, s_session.offset);
    return send_response(request_id, I2C_STATUS_OK, buf, sizeof(buf));
}

/**
 * @brief 处理 FLASH_END
 *
 * CONTEXT: total_size(4) crc32(4)
 * 校验整包 CRC 并更新配置区
 */
static esp_err_t cmd_flash_end(uint32_t request_id,
                               const uint8_t *ctx, size_t ctx_len)
{
    if (!s_session.active) {
        return send_response(request_id, I2C_STATUS_ERR_STATE, NULL, 0);
    }
    if (ctx_len < 8) {
        return send_response(request_id, I2C_STATUS_ERR_ARG, NULL, 0);
    }

    uint32_t expect_size = iap_rd_le32(ctx);
    uint32_t expect_crc  = iap_rd_le32(ctx + 4);

    uint32_t calc_crc = iap_crc32_ctx_final(&s_session.crc);

    ESP_LOGI(TAG, "烧录结束: 已写 %" PRIu32 "/%" PRIu32 ", CRC 计算=0x%08" PRIx32 " 期望=0x%08" PRIx32,
             s_session.offset, expect_size, calc_crc, expect_crc);

    if (s_session.offset != expect_size) {
        s_session.active = false;
        return send_response(request_id, I2C_STATUS_ERR_STATE, NULL, 0);
    }
    if (calc_crc != expect_crc) {
        s_session.active = false;
        return send_response(request_id, I2C_STATUS_ERR_CRC, NULL, 0);
    }

    /* 更新配置区中的用户程序信息 */
    iap_cfg_data_t cfg;
    if (iap_config_get(&cfg) == ESP_OK) {
        cfg.user_app_size    = expect_size;
        cfg.user_app_crc32   = expect_crc;
        iap_config_set(&cfg);
    }

    s_session.active = false;
    ESP_LOGI(TAG, "烧录成功");
    return send_response(request_id, I2C_STATUS_OK, NULL, 0);
}

/**
 * @brief 处理 FLASH_ABORT
 */
static esp_err_t cmd_flash_abort(uint32_t request_id,
                                 const uint8_t *ctx, size_t ctx_len)
{
    s_session.active = false;
    s_session.offset = 0;
    ESP_LOGW(TAG, "烧录会话被中止");
    return send_response(request_id, I2C_STATUS_OK, NULL, 0);
}

/**
 * @brief 处理 READ_PARTITION
 *
 * CONTEXT: offset(4) length(2)
 * 响应 CONTEXT: offset(4) + 数据
 */
static esp_err_t cmd_read_partition(uint32_t request_id,
                                    const uint8_t *ctx, size_t ctx_len)
{
    if (ctx_len < 6) {
        return send_response(request_id, I2C_STATUS_ERR_ARG, NULL, 0);
    }

    uint32_t offset = iap_rd_le32(ctx);
    uint16_t length = iap_rd_le16(ctx + 4);

    if (length == 0) {
        return send_response(request_id, I2C_STATUS_ERR_ARG, NULL, 0);
    }

    const esp_partition_t *part = iap_image_get_user_partition();
    if (part == NULL) {
        return send_response(request_id, I2C_STATUS_ERR_STATE, NULL, 0);
    }
    if (offset + length > part->size) {
        return send_response(request_id, I2C_STATUS_ERR_RANGE, NULL, 0);
    }

    uint8_t *buf = malloc(4 + length);
    if (buf == NULL) {
        return send_response(request_id, I2C_STATUS_ERR_NO_MEM, NULL, 0);
    }

    iap_wr_le32(buf, offset);
    esp_err_t err = iap_image_read(offset, buf + 4, length);
    if (err != ESP_OK) {
        free(buf);
        return send_response(request_id, I2C_STATUS_ERR_FLASH, NULL, 0);
    }

    err = send_response(request_id, I2C_STATUS_OK, buf, 4 + length);
    free(buf);
    return err;
}

/**
 * @brief 处理 WRITE_PARTITION
 *
 * CONTEXT: offset(4) + 数据 (直接写入，不参与会话 CRC)
 */
static esp_err_t cmd_write_partition(uint32_t request_id,
                                     const uint8_t *ctx, size_t ctx_len)
{
    if (ctx_len < 5) {
        return send_response(request_id, I2C_STATUS_ERR_ARG, NULL, 0);
    }

    uint32_t offset = iap_rd_le32(ctx);
    const uint8_t *data = ctx + 4;
    size_t data_len = ctx_len - 4;

    const esp_partition_t *part = iap_image_get_user_partition();
    if (part == NULL) {
        return send_response(request_id, I2C_STATUS_ERR_STATE, NULL, 0);
    }
    if (offset + data_len > part->size) {
        return send_response(request_id, I2C_STATUS_ERR_RANGE, NULL, 0);
    }

    esp_err_t err = iap_image_write(offset, data, data_len);
    if (err != ESP_OK) {
        return send_response(request_id, I2C_STATUS_ERR_FLASH, NULL, 0);
    }

    uint8_t buf[4];
    iap_wr_le32(buf, offset + (uint32_t)data_len);
    return send_response(request_id, I2C_STATUS_OK, buf, sizeof(buf));
}

/**
 * @brief 处理 ERASE_PARTITION
 *
 * CONTEXT: offset(4) length(4)
 */
static esp_err_t cmd_erase_partition(uint32_t request_id,
                                     const uint8_t *ctx, size_t ctx_len)
{
    if (ctx_len < 8) {
        return send_response(request_id, I2C_STATUS_ERR_ARG, NULL, 0);
    }

    uint32_t offset = iap_rd_le32(ctx);
    uint32_t length = iap_rd_le32(ctx + 4);

    const esp_partition_t *part = iap_image_get_user_partition();
    if (part == NULL) {
        return send_response(request_id, I2C_STATUS_ERR_STATE, NULL, 0);
    }
    if (length == 0 || offset + length > part->size) {
        return send_response(request_id, I2C_STATUS_ERR_RANGE, NULL, 0);
    }

    esp_err_t err = esp_partition_erase_range(part, offset, length);
    if (err != ESP_OK) {
        return send_response(request_id, I2C_STATUS_ERR_FLASH, NULL, 0);
    }
    return send_response(request_id, I2C_STATUS_OK, NULL, 0);
}

/**
 * @brief 处理 VERIFY_PARTITION
 *
 * 无自定义文件头后，只检查镜像是否存在（首字节 0xE9）。
 * 返回: valid(1) crc32(4) size(4)
 */
static esp_err_t cmd_verify_partition(uint32_t request_id,
                                      const uint8_t *ctx, size_t ctx_len)
{
    iap_image_info_t info;
    iap_image_get_info(&info);

    uint8_t buf[9];
    buf[0] = info.present ? 1 : 0;
    iap_wr_le32(buf + 1, info.image_crc32);
    iap_wr_le32(buf + 5, info.image_size);

    return send_response(request_id,
                         info.present ? I2C_STATUS_OK : I2C_STATUS_ERR_NO_APP,
                         buf, sizeof(buf));
}

/**
 * @brief 处理 GET_SLOT_LIST
 *
 * 返回: count(1) active(1) 然后每个槽 8 字节:
 *         address(4) size(4)
 *       最后附加 count 个字节的 present 标志。
 *
 * 简化布局: count(1) active(1) present[count](各 1 字节) addr/size[count](各 8 字节)
 */
static esp_err_t cmd_get_slot_list(uint32_t request_id,
                                   const uint8_t *ctx, size_t ctx_len)
{
    uint8_t count = iap_image_get_slot_count();
    uint8_t active = iap_image_get_active_slot();

    uint8_t buf[2 + IAP_OTA_SLOT_MAX * 9];
    size_t n = 0;
    buf[n++] = count;
    buf[n++] = active;

    for (uint8_t i = 0; i < count && n + 9 <= sizeof(buf); i++) {
        const esp_partition_t *p = iap_image_get_slot(i);
        buf[n++] = iap_image_slot_present(i) ? 1 : 0;
        iap_wr_le32(buf + n, p ? p->address : 0); n += 4;
        iap_wr_le32(buf + n, p ? p->size : 0);    n += 4;
    }

    return send_response(request_id, I2C_STATUS_OK, buf, n);
}

/**
 * @brief 处理 GET_ACTIVE_SLOT
 *
 * 返回: active(1) count(1)
 */
static esp_err_t cmd_get_active_slot(uint32_t request_id,
                                     const uint8_t *ctx, size_t ctx_len)
{
    uint8_t buf[2];
    buf[0] = iap_image_get_active_slot();
    buf[1] = iap_image_get_slot_count();
    return send_response(request_id, I2C_STATUS_OK, buf, sizeof(buf));
}

/**
 * @brief 处理 SET_ACTIVE_SLOT
 *
 * 请求: slot(1)
 */
static esp_err_t cmd_set_active_slot(uint32_t request_id,
                                     const uint8_t *ctx, size_t ctx_len)
{
    if (ctx_len < 1) {
        return send_response(request_id, I2C_STATUS_ERR_ARG, NULL, 0);
    }

    uint8_t slot = ctx[0];
    if (slot >= iap_image_get_slot_count() || iap_image_get_slot(slot) == NULL) {
        return send_response(request_id, I2C_STATUS_ERR_RANGE, NULL, 0);
    }

    esp_err_t err = iap_image_set_active_slot(slot);
    return send_response(request_id,
                         err == ESP_OK ? I2C_STATUS_OK : I2C_STATUS_ERR_FLASH,
                         NULL, 0);
}

/**
 * @brief 处理 BOOT_SLOT
 *
 * 请求: slot(1)
 *
 * 槽不存在或无镜像时，iap_image_boot_slot() 会自动回落到 IAP (factory)，
 * 因此这里仍返回 OK，由调用方通过 GET_SLOT_LIST 查询实际状态。
 */
static esp_err_t cmd_boot_slot(uint32_t request_id,
                               const uint8_t *ctx, size_t ctx_len)
{
    if (ctx_len < 1) {
        return send_response(request_id, I2C_STATUS_ERR_ARG, NULL, 0);
    }

    uint8_t slot = ctx[0];
    if (slot >= iap_image_get_slot_count() || iap_image_get_slot(slot) == NULL) {
        return send_response(request_id, I2C_STATUS_ERR_RANGE, NULL, 0);
    }

    /* 先应答，再重启 (无镜像时 boot_slot 内部会回落到 IAP) */
    send_response(request_id, I2C_STATUS_OK, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
    iap_image_set_active_slot(slot);
    iap_image_boot_slot(slot);
    return ESP_OK;
}

/**
 * @brief 处理 BOOT_USER_APP
 */
static esp_err_t cmd_boot_user_app(uint32_t request_id,
                                   const uint8_t *ctx, size_t ctx_len)
{
    if (!iap_image_user_app_present()) {
        return send_response(request_id, I2C_STATUS_ERR_NO_APP, NULL, 0);
    }

    /* 先应答，再重启 */
    send_response(request_id, I2C_STATUS_OK, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(200));

    iap_image_boot_user_app();
    return ESP_OK;
}

/**
 * @brief 处理 BOOT_IAP
 */
static esp_err_t cmd_boot_iap(uint32_t request_id,
                              const uint8_t *ctx, size_t ctx_len)
{
    send_response(request_id, I2C_STATUS_OK, NULL, 0);

    /* 若处于启动等待阶段，通知等待触发模块，由主流程决定后续动作 */
    iap_wait_trigger_notify_i2c();

    vTaskDelay(pdMS_TO_TICKS(200));

    iap_image_boot_iap();
    return ESP_OK;
}

/**
 * @brief 处理 SET_DOWNLOAD_MODE
 *
 * CONTEXT: enable(1)
 */
static esp_err_t cmd_set_download_mode(uint32_t request_id,
                                       const uint8_t *ctx, size_t ctx_len)
{
    if (ctx_len < 1) {
        return send_response(request_id, I2C_STATUS_ERR_ARG, NULL, 0);
    }
    iap_config_set_download_mode(ctx[0] != 0);
    return send_response(request_id, I2C_STATUS_OK, NULL, 0);
}

/**
 * @brief 处理 RESET_CFG
 */
static esp_err_t cmd_reset_cfg(uint32_t request_id,
                               const uint8_t *ctx, size_t ctx_len)
{
    esp_err_t err = iap_config_reset();
    return send_response(request_id,
                         (err == ESP_OK) ? I2C_STATUS_OK : I2C_STATUS_ERR_FLASH,
                         NULL, 0);
}

/**
 * @brief 处理 REBOOT
 */
static esp_err_t cmd_reboot(uint32_t request_id,
                            const uint8_t *ctx, size_t ctx_len)
{
    send_response(request_id, I2C_STATUS_OK, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}

/**
 * @brief 处理 PING
 */
static esp_err_t cmd_ping(uint32_t request_id,
                          const uint8_t *ctx, size_t ctx_len)
{
    uint8_t buf[4];
    iap_wr_le32(buf, (uint32_t)(esp_timer_get_time() / 1000));
    return send_response(request_id, I2C_STATUS_OK, buf, sizeof(buf));
}

/**
 * @brief 分发消息到具体命令处理函数
 *
 * @param msg 重组后的完整消息
 * @param len 消息长度
 * @return ESP_OK 成功
 */
static esp_err_t dispatch_message(const uint8_t *msg, size_t len)
{
    if (len < I2C_MSG_HEADER_SIZE) {
        ESP_LOGW(TAG, "消息太短: %u", (unsigned)len);
        return ESP_ERR_INVALID_SIZE;
    }

    uint16_t type       = iap_rd_le16(msg + 0);
    uint32_t request_id = iap_rd_le32(msg + 4);
    const uint8_t *ctx  = msg + I2C_MSG_HEADER_SIZE;
    size_t ctx_len      = len - I2C_MSG_HEADER_SIZE;

    ESP_LOGD(TAG, "命令 TYPE=0x%04X REQ=%" PRIu32 " CTX=%u",
             type, request_id, (unsigned)ctx_len);

    switch (type) {
    case I2C_TYPE_GET_SYS_INFO:      return cmd_get_sys_info(request_id, ctx, ctx_len);
    case I2C_TYPE_GET_CFG_INFO:      return cmd_get_cfg_info(request_id, ctx, ctx_len);
    case I2C_TYPE_SET_CFG_INFO:      return cmd_set_cfg_info(request_id, ctx, ctx_len);
    case I2C_TYPE_GET_APP_INFO:      return cmd_get_app_info(request_id, ctx, ctx_len);
    case I2C_TYPE_GET_STATUS:        return cmd_get_status(request_id, ctx, ctx_len);
    case I2C_TYPE_GET_BOOT_PARAM:    return cmd_get_boot_param(request_id, ctx, ctx_len);
    case I2C_TYPE_SET_BOOT_PARAM:    return cmd_set_boot_param(request_id, ctx, ctx_len);

    case I2C_TYPE_FLASH_BEGIN:       return cmd_flash_begin(request_id, ctx, ctx_len);
    case I2C_TYPE_FLASH_DATA:        return cmd_flash_data(request_id, ctx, ctx_len);
    case I2C_TYPE_FLASH_END:         return cmd_flash_end(request_id, ctx, ctx_len);
    case I2C_TYPE_FLASH_ABORT:       return cmd_flash_abort(request_id, ctx, ctx_len);

    case I2C_TYPE_READ_PARTITION:    return cmd_read_partition(request_id, ctx, ctx_len);
    case I2C_TYPE_WRITE_PARTITION:   return cmd_write_partition(request_id, ctx, ctx_len);
    case I2C_TYPE_ERASE_PARTITION:   return cmd_erase_partition(request_id, ctx, ctx_len);
    case I2C_TYPE_VERIFY_PARTITION:  return cmd_verify_partition(request_id, ctx, ctx_len);

    case I2C_TYPE_BOOT_USER_APP:     return cmd_boot_user_app(request_id, ctx, ctx_len);
    case I2C_TYPE_BOOT_IAP:          return cmd_boot_iap(request_id, ctx, ctx_len);
    case I2C_TYPE_SET_DOWNLOAD_MODE: return cmd_set_download_mode(request_id, ctx, ctx_len);
    case I2C_TYPE_RESET_CFG:         return cmd_reset_cfg(request_id, ctx, ctx_len);
    case I2C_TYPE_REBOOT:            return cmd_reboot(request_id, ctx, ctx_len);

    case I2C_TYPE_GET_SLOT_LIST:     return cmd_get_slot_list(request_id, ctx, ctx_len);
    case I2C_TYPE_GET_ACTIVE_SLOT:   return cmd_get_active_slot(request_id, ctx, ctx_len);
    case I2C_TYPE_SET_ACTIVE_SLOT:   return cmd_set_active_slot(request_id, ctx, ctx_len);
    case I2C_TYPE_BOOT_SLOT:         return cmd_boot_slot(request_id, ctx, ctx_len);

    case I2C_TYPE_PING:              return cmd_ping(request_id, ctx, ctx_len);

    default:
        ESP_LOGW(TAG, "不支持的命令: 0x%04X", type);
        return send_response(request_id, I2C_STATUS_ERR_UNSUPPORTED, NULL, 0);
    }
}

/* ============================================================================
 * 多包重组
 * ========================================================================== */

/**
 * @brief 处理一个已解析的帧，必要时重组
 *
 * @param hdr 帧头信息
 * @return ESP_OK 成功
 */
static esp_err_t handle_frame(const iap_i2c_frame_t *hdr)
{
    if (hdr->fragment == I2C_FRAG_SINGLE) {
        /* 单包: 直接分发 */
        return dispatch_message(hdr->payload, hdr->payload_len);
    }

    /* 多包: 重组 */
    if (hdr->packet_index == 0) {
        /* 新消息起始 */
        s_msg_id = hdr->message_id;
        s_msg_len = 0;
        s_msg_next_index = 0;
        s_msg_packet_count = hdr->packet_count;
    } else {
        /* 后续包: 校验一致性 */
        if (hdr->message_id != s_msg_id ||
            hdr->packet_index != s_msg_next_index ||
            hdr->packet_count != s_msg_packet_count) {
            ESP_LOGW(TAG, "重组序号错乱: id=%" PRIu32 "/%" PRIu32
                     " idx=%u/%u count=%u/%u",
                     hdr->message_id, s_msg_id,
                     hdr->packet_index, s_msg_next_index,
                     hdr->packet_count, s_msg_packet_count);
            s_msg_len = 0;
            s_msg_next_index = 0;
            return ESP_ERR_INVALID_STATE;
        }
    }

    if (s_msg_len + hdr->payload_len > I2C_MSG_MAX_SIZE) {
        ESP_LOGW(TAG, "重组缓冲溢出");
        s_msg_len = 0;
        return ESP_ERR_NO_MEM;
    }

    memcpy(s_msg_buf + s_msg_len, hdr->payload, hdr->payload_len);
    s_msg_len += hdr->payload_len;
    s_msg_next_index++;

    /* 末包: 分发 */
    if (hdr->fragment == I2C_FRAG_LAST) {
        esp_err_t err = dispatch_message(s_msg_buf, s_msg_len);
        s_msg_len = 0;
        s_msg_next_index = 0;
        return err;
    }

    return ESP_OK;
}

/* ============================================================================
 * I2C 驱动回调与任务
 * ========================================================================== */

/**
 * @brief I2C 从机接收完成回调 (ISR 上下文)
 *
 * 将收到的数据投递到队列，由任务处理，避免在 ISR 中做耗时操作。
 */
static bool IRAM_ATTR i2c_on_receive(i2c_slave_dev_handle_t slave,
                                     const i2c_slave_rx_done_event_data_t *evt,
                                     void *arg)
{
    BaseType_t hp_task_woken = pdFALSE;

    /* 将数据指针与长度打包投递 (数据缓冲由驱动管理，需立即复制) */
    i2c_rx_item_t item;
    item.len = (evt->length > sizeof(item.data)) ? sizeof(item.data) : evt->length;
    memcpy(item.data, evt->buffer, item.len);

    if (s_rx_queue) {
        xQueueSendFromISR(s_rx_queue, &item, &hp_task_woken);
    }
    return hp_task_woken == pdTRUE;
}

/**
 * @brief I2C 从机请求回调: 主设备要读取数据
 *
 * 从发送缓冲取数据写入 FIFO。
 */
static bool IRAM_ATTR i2c_on_request(i2c_slave_dev_handle_t slave,
                                     const i2c_slave_request_event_data_t *evt,
                                     void *arg)
{
    BaseType_t hp_task_woken = pdFALSE;

    if (s_tx_pos < s_tx_len) {
        size_t remain = s_tx_len - s_tx_pos;
        uint32_t written = 0;
        /* 在 ISR 中直接写 FIFO，超时设为 0 */
        i2c_slave_write(slave, s_tx_buf + s_tx_pos, remain, &written, 0);
        s_tx_pos += written;
    }

    return hp_task_woken == pdTRUE;
}

/**
 * @brief I2C 命令处理任务
 */
static void i2c_task(void *arg)
{
    i2c_rx_item_t item;
    iap_i2c_frame_t hdr;

    ESP_LOGI(TAG, "I2C 处理任务已启动");

    while (1) {
        if (xQueueReceive(s_rx_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        esp_err_t err = frame_parse(item.data, item.len, &hdr);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "帧解析失败: %s", esp_err_to_name(err));
            continue;
        }

        err = handle_frame(&hdr);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "帧处理失败: %s", esp_err_to_name(err));
        }
    }
}

/* ============================================================================
 * 初始化
 * ========================================================================== */

esp_err_t iap_i2c_start(void)
{
    iap_cfg_data_t cfg;
    esp_err_t err = iap_config_get(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    if (!(cfg.flags & IAP_CFG_FLAG_I2C_ENABLE)) {
        ESP_LOGI(TAG, "I2C 从机未使能，跳过初始化");
        return ESP_OK;
    }

    /*
     * 幂等保护: I2C 可能已在**等待触发阶段**启动过
     * (iap_wait_trigger 调用 iap_i2c_start())。
     * 重复初始化会重复分配缓冲 (内存泄漏) 并重复创建从机设备。
     */
    if (s_slave != NULL) {
        ESP_LOGI(TAG, "I2C 从机已启动，复用现有实例");
        return ESP_OK;
    }

    /* 分配缓冲 */
    s_tx_buf  = malloc(I2C_TX_BUF_SIZE);
    s_msg_buf = malloc(I2C_MSG_MAX_SIZE);
    if (s_tx_buf == NULL || s_msg_buf == NULL) {
        ESP_LOGE(TAG, "缓冲分配失败");
        return ESP_ERR_NO_MEM;
    }

    s_tx_lock = xSemaphoreCreateMutex();
    s_rx_queue = xQueueCreate(I2C_RX_QUEUE_LEN, sizeof(i2c_rx_item_t));
    if (s_tx_lock == NULL || s_rx_queue == NULL) {
        ESP_LOGE(TAG, "同步对象创建失败");
        return ESP_ERR_NO_MEM;
    }

    /* 配置 I2C 从机 */
    i2c_slave_config_t slave_cfg = {
        .i2c_port          = -1,            /* 自动选择端口 */
        .sda_io_num        = cfg.i2c_sda_gpio,
        .scl_io_num        = cfg.i2c_scl_gpio,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .send_buf_depth    = I2C_TX_BUF_SIZE,
        .receive_buf_depth = 2048,
        .slave_addr        = cfg.i2c_addr,
        .addr_bit_len      = I2C_ADDR_BIT_LEN_7,
        .intr_priority     = 0,
        .flags = {
            .enable_internal_pullup = 1,
        },
    };

    err = i2c_new_slave_device(&slave_cfg, &s_slave);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "创建 I2C 从机失败: %s", esp_err_to_name(err));
        return err;
    }

    /* 注册回调 */
    i2c_slave_event_callbacks_t cbs = {
        .on_receive = i2c_on_receive,
        .on_request = i2c_on_request,
    };
    err = i2c_slave_register_event_callbacks(s_slave, &cbs, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "注册 I2C 回调失败: %s", esp_err_to_name(err));
        return err;
    }

    /* 启动处理任务 */
    BaseType_t ret = xTaskCreate(i2c_task, "iap_i2c", 6144, NULL, 6, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "创建 I2C 任务失败");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "I2C 从机已启动: SCL=GPIO%d, SDA=GPIO%d, addr=0x%02X",
             cfg.i2c_scl_gpio, cfg.i2c_sda_gpio, cfg.i2c_addr);

    return ESP_OK;
}

#endif  /* SOC_I2C_SUPPORT_SLAVE */
