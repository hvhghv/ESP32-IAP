# ESP IAP 主组件
#
# 按芯片能力条件编译:
#   - I2C 从机: 仅 SOC_I2C_SUPPORT_SLAVE 的芯片支持 (ESP32-C2 不支持)
#   - WiFi    : 仅 SOC_WIFI_SUPPORTED 的芯片支持 (ESP32-H2 不支持)
#
# 不支持时对应源文件内的实现会被 #if 置空 (返回 ESP_ERR_NOT_SUPPORTED)。

# 按芯片能力组装依赖列表
set(iap_requires
    esp_partition
    app_update
    esp_app_format
    driver
    esp_driver_uart
    esp_driver_i2c
    esp_driver_gpio
    esp_netif
    esp_event
    esp_http_server
    nvs_flash
    lwip
    vfs
    esp_timer
    esp_system
    spi_flash
    bootloader_support
    mbedtls
    soc
    esp_rom
)

# USB-Serial-JTAG 组件仅在支持 USB 的芯片上引入
#   C3 / C6 / H2 / S3 内置 USB PHY，可用 USB 串口终端
#   ESP32 / S2 / C2 无 USB-Serial-JTAG
if(NOT IDF_TARGET STREQUAL "esp32" AND
   NOT IDF_TARGET STREQUAL "esp32s2" AND
   NOT IDF_TARGET STREQUAL "esp32c2")
    list(APPEND iap_requires esp_driver_usb_serial_jtag)
endif()

# WiFi 组件仅在支持 WiFi 的芯片上引入 (ESP32-H2 无 WiFi)
#
# 说明: 这里用 IDF_TARGET 判断而非 CONFIG_SOC_WIFI_SUPPORTED，
# 因为后者是 soc_caps.h 中的宏，不是 Kconfig 选项，
# 在 CMake 阶段不可用。
if(NOT IDF_TARGET STREQUAL "esp32h2")
    list(APPEND iap_requires esp_wifi)
endif()

idf_component_register(
    SRCS
        "main.c"
        "iap_config.c"
        "iap_image.c"
        "iap_xmodem.c"
        "iap_uart.c"
        "iap_usb.c"
        "iap_i2c.c"
        "iap_wifi.c"
        "iap_http.c"
        "iap_wait_trigger.c"
        "iap_crc.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        ${iap_requires}
)

# ---------------------------------------------------------------------------
# 嵌入 Web 控制台页面 (main/iap_index.html)
# ---------------------------------------------------------------------------
# 页面源码独立存放，编辑 HTML 无需改动 C 代码；构建时由 ESP-IDF 的
# target_add_binary_data 生成目标文件，导出的符号为:
#     _binary_iap_index_html_start / _binary_iap_index_html_end
# 在 iap_http.c 中通过 iap_index_html_start / iap_index_html_end 访问。
target_add_binary_data(${COMPONENT_LIB} "iap_index.html" TEXT)

# ---------------------------------------------------------------------------
# 注入自定义 lwIP 钩子 (DHCP 不下发网关/DNS)
# ---------------------------------------------------------------------------
# 原理:
#   lwIP 组件中的 lwip_default_hooks.h 会包含由 ESP_IDF_LWIP_HOOK_FILENAME
#   指定的文件。我们在 main/iap_lwip_hooks.h 中定义了
#   LWIP_HOOK_DHCPS_POST_STATE，用于在 DHCP 响应发送前删除
#   option 3 (Router) 与 option 6 (DNS)。
#
# 注意:
#   必须直接对 lwip 组件设置编译选项/宏 (官方推荐做法)，
#   仅在自己的组件上设置无法影响 lwip 组件的编译。
#   参考: ESP-IDF 文档 api-guides/lwip.rst "Customized lwIP Hooks"
#
# 仅在有 WiFi 的芯片上启用 (ESP32-H2 无 WiFi，也就没有 DHCP 服务器)。
if(NOT IDF_TARGET STREQUAL "esp32h2")
    idf_component_get_property(lwip_lib lwip COMPONENT_LIB)
    target_compile_options(${lwip_lib} PRIVATE "-I${CMAKE_CURRENT_LIST_DIR}")
    target_compile_definitions(${lwip_lib} PRIVATE
        "-DESP_IDF_LWIP_HOOK_FILENAME=\"iap_lwip_hooks.h\"")
endif()
