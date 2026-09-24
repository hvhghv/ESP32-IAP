# 用户程序 main 组件
#
# 若不需要 IAP 对接功能，可去掉 iap_user_api.c。
idf_component_register(
    SRCS
        "main.c"
        "iap_user_api.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        esp_partition
        spi_flash            # esp_flash_read/write/erase_region (v5 直访)
        esp_system
        esp_timer
        esp_rom
        bootloader_support   # rtc_retain_mem_t / bootloader_common_get_rtc_retain_mem (v5)
        nvs_flash
)
