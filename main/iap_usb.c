/*
 * ESP IAP - USB 串口终端 (USB-Serial-JTAG CDC)
 *
 * 支持的芯片 (内置 USB PHY，无需外部器件):
 *   ESP32-C3 / C5 / C6 / C61 / H2 / S3
 *
 * 作用:
 *   - 提供与 UART 等价的交互终端 (命令、XMODEM 烧录)
 *   - 与 UART 终端共享同一套命令表，两个终端可同时使用
 *
 * 与 UART 的差异:
 *   - 无波特率概念 (USB CDC 由主机决定速率)，配置区无对应字段
 *   - 引脚固定接芯片 D+/D-，不可配置
 *   - 首次连接需主机侧驱动支持 (Windows 10+ 免驱，走 CDC-ACM)
 *
 * 注意: 若 IDF 控制台也启用了 USB Serial/JTAG
 *       (CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)，日志会与本终端争用同一
 *       接口。本工程默认控制台走 UART，USB 仅供 IAP 终端使用。
 */

#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "iap_common.h"
#include "iap_config.h"
#include "iap_uart.h"

static const char *TAG = "iap_usb";

#if IAP_USB_SUPPORTED

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

/** USB 驱动是否已安装 */
static bool s_usb_installed = false;

/* -------------------------------------------------------------------------- */
/* 后端原语                                                                    */
/* -------------------------------------------------------------------------- */

static int usb_backend_read(uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    if (!s_usb_installed) {
        vTaskDelay(pdMS_TO_TICKS(timeout_ms));
        return 0;
    }
    return usb_serial_jtag_read_bytes(buf, (uint32_t)len,
                                      pdMS_TO_TICKS(timeout_ms));
}

static int usb_backend_write(const uint8_t *buf, size_t len)
{
    if (!s_usb_installed) {
        return 0;
    }
    return usb_serial_jtag_write_bytes(buf, len, pdMS_TO_TICKS(100));
}

/**
 * @brief 等待 USB 发送缓冲排空
 *
 * ⚠️ usb_serial_jtag_write_bytes 是**异步**的: 数据先进驱动环形缓冲,
 *    再由 USB 外设在主机轮询时发出。若在缓冲尚未排空时就开始
 *    XMODEM 会话, 残留的提示文本会被发送方当作协议数据读走。
 *
 *    这里用固定延时等待硬件发出 (USB CDC 的 1KB 缓冲在 115200 下
 *    约需 90ms; 取 200ms 留足余量)。主机未连接时数据发不出去,
 *    但那种情况下 XMODEM 本身也无法工作, 延时不会造成额外问题。
 */
static void usb_backend_flush(void)
{
    if (!s_usb_installed) {
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
}

static bool usb_backend_ready(void)
{
    return s_usb_installed;
}

static const iap_term_backend_t s_usb_backend = {
    .name  = "USB",
    .read  = usb_backend_read,
    .write = usb_backend_write,
    .ready = usb_backend_ready,
    .flush = usb_backend_flush,
};

/* -------------------------------------------------------------------------- */
/* 初始化                                                                      */
/* -------------------------------------------------------------------------- */

esp_err_t iap_usb_start(void)
{
    iap_cfg_data_t cfg;
    esp_err_t err = iap_config_get(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    if (!(cfg.flags & IAP_CFG_FLAG_USB_ENABLE)) {
        ESP_LOGI(TAG, "USB 串口未使能，跳过初始化");
        return ESP_OK;
    }

    /* 安装驱动 (幂等) */
    err = iap_usb_prepare();
    if (err != ESP_OK) {
        return err;
    }
    if (!s_usb_installed) {
        return ESP_OK;   /* 未使能/不支持 */
    }

    /* 挂载为终端后端 (内部创建独立读取任务) */
    err = iap_term_attach(&s_usb_backend, "iap_term_usb");
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "USB 串口终端已启动 (USB-Serial-JTAG CDC)");
    return ESP_OK;
}

esp_err_t iap_usb_prepare(void)
{
    if (s_usb_installed) {
        return ESP_OK;   /* 幂等 */
    }

    iap_cfg_data_t cfg;
    esp_err_t err = iap_config_get(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    /*
     * 等待触发窗口需要读 USB 输入，因此只要 USB 使能**或** USB 触发使能，
     * 就提前安装驱动。
     */
    if (!(cfg.flags & (IAP_CFG_FLAG_USB_ENABLE | IAP_CFG_FLAG_WAIT_USB_TRIG))) {
        return ESP_OK;   /* 两者都未使能，不安装 */
    }

    /*
     * 安装 USB-Serial-JTAG 驱动。
     * 若 IDF 控制台已占用该接口，安装会失败并提示，此时复用已有驱动。
     */
    usb_serial_jtag_driver_config_t usb_cfg = {
        .tx_buffer_size = 1024,
        .rx_buffer_size = 1024,
    };

    esp_err_t drv_err = usb_serial_jtag_driver_install(&usb_cfg);
    if (drv_err == ESP_OK) {
        s_usb_installed = true;
        ESP_LOGI(TAG, "USB-Serial-JTAG 驱动已安装");
    } else if (drv_err == ESP_ERR_INVALID_STATE) {
        /* 已被控制台安装，直接复用 */
        s_usb_installed = true;
        ESP_LOGI(TAG, "USB-Serial-JTAG 驱动已存在，复用现有驱动");
    } else {
        ESP_LOGE(TAG, "USB-Serial-JTAG 驱动安装失败: %s", esp_err_to_name(drv_err));
        return drv_err;
    }

    return ESP_OK;
}

bool iap_usb_ready(void)
{
    return s_usb_installed;
}

int iap_usb_read(uint8_t *buf, size_t len)
{
    return usb_backend_read(buf, len, 0);   /* 非阻塞 */
}

#else  /* !IAP_USB_SUPPORTED */

esp_err_t iap_usb_start(void)
{
    ESP_LOGI(TAG, "该芯片不支持 USB-Serial-JTAG，跳过 USB 初始化");
    return ESP_OK;
}

esp_err_t iap_usb_prepare(void)
{
    return ESP_OK;   /* 不支持，静默跳过 */
}

bool iap_usb_ready(void)
{
    return false;
}

int iap_usb_read(uint8_t *buf, size_t len)
{
    (void)buf; (void)len;
    return 0;
}

#endif  /* IAP_USB_SUPPORTED */
