idf_component_register(SRCS "bootloader_start.c"
                    REQUIRES bootloader bootloader_support
                    INCLUDE_DIRS "."
                    PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../main")

# 使用默认链接脚本
idf_build_get_property(scripts BOOTLOADER_LINKER_SCRIPT)
target_linker_script(${COMPONENT_LIB} INTERFACE "${scripts}")
