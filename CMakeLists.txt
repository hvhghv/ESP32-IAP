# ESP IAP (In-Application Programming) 工程
#
# 支持 ESP32 / ESP32S2 / ESP32S3 / ESP32C3 / ESP32C5 / ESP32C6 / ESP32H2
# 通过 I2C / UART / WiFi 对用户程序区进行烧录
#
# 使用方式:
#   idf.py set-target esp32c6
#   idf.py build              # 编译
#   idf.py release            # 打包发布固件到 build/
#   idf.py -p COMx flash monitor
#
# 自定义目标:
#   release       生成 build/iap_side_<target>.bin (固定区 1280KB)
#   release-user  生成 build/user_<target>.bin    (从 0x140000 开始)
#   release-all   以上两者 + 同步配套工具到 build/

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(esp_iap)

# ============================================================================
# 发布打包 (release) —— 替代原 make_release.ps1
# ============================================================================
#
# 产物 (输出到 <项目根>/build/):
#   iap_side_<target>.bin   固定区整片 (0x0 ~ 0x13FFFF, 1280KB)
#                           = bootloader + 出厂配置 + 分区表 A + IAP 程序
#   user_<target>.bin       用户程序 (从 0x140000 开始, 含分区表 B)
#   esp_iap_tool.html       浏览器烧录工具
#   esp_iap_builtin.js      内置镜像索引 (惰性加载)
#   assets/iap_img_<chip>.js  各芯片内置镜像数据
#   *.py                    配套 Python 工具
#
# 前置条件:
#   - 本工程已 idf.py build
#   - 用户程序工程已编译 (examples/user_app_template/build 或 C:\temp\*\build)
#
# 用法:
#   idf.py release              # 仅生成 iap_side_<target>.bin
#   idf.py release-user         # 仅生成 user_<target>.bin
#   idf.py release-all          # 全部 + 同步工具

# --- 路径 ---
set(_iap_root     "${CMAKE_CURRENT_LIST_DIR}")
set(_iap_outdir   "${CMAKE_CURRENT_LIST_DIR}/build")
set(_iap_merge    "${_iap_root}/tools/merge_bin.py")
set(_iap_mergeapp "${_iap_root}/tools/merge_user_app.py")
set(_iap_embed    "${_iap_root}/tools/gen_boot_embed.py")

# 用户程序产物: 优先 examples 内构建目录, 回退到常见临时构建目录
#
# ⚠️ 临时目录里可能有多个 user_app_template.bin (不同芯片/不同时间),
#    因此按**修改时间**取最新的, 并优先匹配含 "user_app" 的目录名。
set(_iap_user_bin  "${_iap_root}/examples/user_app_template/build/user_app_template.bin")
set(_iap_user_pt   "${_iap_root}/examples/user_app_template/build/partition_table/partition-table.bin")

if(NOT EXISTS "${_iap_user_bin}")
    file(GLOB _iap_alt
         "${_iap_root}/../*/build/user_app_template.bin"
         "C:/temp/user_app*/build/user_app_template.bin"
         "C:/temp/iap_*/build/user_app_template.bin")

    # 过滤: 优先含 "user_app" 的目录, 其余作为备选
    set(_iap_pref "")
    set(_iap_rest "")
    foreach(_f ${_iap_alt})
        if(_f MATCHES "user_app")
            list(APPEND _iap_pref "${_f}")
        else()
            list(APPEND _iap_rest "${_f}")
        endif()
    endforeach()

    if(_iap_pref)
        set(_iap_cands ${_iap_pref})
    else()
        set(_iap_cands ${_iap_rest})
    endif()

    # 按修改时间降序取第一个
    set(_iap_best "")
    set(_iap_best_t 0)
    foreach(_f ${_iap_cands})
        file(TIMESTAMP "${_f}" _t "%Y%m%d%H%M%S")
        if(_t GREATER _iap_best_t)
            set(_iap_best_t "${_t}")
            set(_iap_best "${_f}")
        endif()
    endforeach()

    if(_iap_best)
        set(_iap_user_bin "${_iap_best}")
        get_filename_component(_iap_user_dir "${_iap_best}" DIRECTORY)
        set(_iap_user_pt "${_iap_user_dir}/partition_table/partition-table.bin")
        message(STATUS "release: 用户程序 = ${_iap_best}")
    else()
        message(WARNING "release: 未找到 user_app_template.bin，release-user 将失败")
    endif()
endif()

# 分区表 B: 找不到时让 merge_user_app.py 按 flash 容量自动生成
if(EXISTS "${_iap_user_pt}")
    set(_iap_pt_args --ptable "${_iap_user_pt}")
else()
    set(_iap_pt_args --flash "${CONFIG_ESPTOOLPY_FLASHSIZE}")
    message(STATUS "release: 未找到分区表 B，将按 flash 容量自动生成")
endif()

# --- 1. 生成 IAP 烧录文件 (含出厂配置 + 分区表 A) ---
add_custom_target(release
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_iap_outdir}"
    COMMAND ${CMAKE_COMMAND} -E echo "=== 生成 IAP 烧录文件 (${IDF_TARGET}) ==="
    COMMAND ${PYTHON}
            "${_iap_merge}"
            --build-dir "${CMAKE_BINARY_DIR}"
            --default-cfg
            --flash "${CONFIG_ESPTOOLPY_FLASHSIZE}"
            -o "${_iap_outdir}/iap_side_${IDF_TARGET}.bin"
    COMMAND ${CMAKE_COMMAND} -E echo "产物: build/iap_side_${IDF_TARGET}.bin"
    USES_TERMINAL
    COMMENT "打包 IAP 烧录文件 -> build/iap_side_${IDF_TARGET}.bin"
)

# --- 2. 生成用户程序烧录文件 (合并分区表 B) ---
add_custom_target(release-user
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_iap_outdir}"
    COMMAND ${CMAKE_COMMAND} -E echo "=== 打包用户程序 (${IDF_TARGET}) ==="
    COMMAND ${CMAKE_COMMAND} -E echo "用户程序: ${_iap_user_bin}"
    COMMAND ${PYTHON}
            "${_iap_mergeapp}"
            --app "${_iap_user_bin}"
            ${_iap_pt_args}
            -o "${_iap_outdir}/user_${IDF_TARGET}.bin"
    COMMAND ${CMAKE_COMMAND} -E echo "产物: build/user_${IDF_TARGET}.bin"
    USES_TERMINAL
    COMMENT "打包用户程序 -> build/user_${IDF_TARGET}.bin"
)

# --- 3. 同步配套工具到 build/ ---
add_custom_target(release-tools
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_iap_outdir}"
    COMMAND ${CMAKE_COMMAND} -E echo "=== 同步配套工具 ==="
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_iap_root}/esp_iap_tool.html" "${_iap_outdir}/esp_iap_tool.html"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_iap_root}/esp_iap_builtin.js" "${_iap_outdir}/esp_iap_builtin.js"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_iap_merge}"    "${_iap_outdir}/merge_bin.py"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_iap_mergeapp}" "${_iap_outdir}/merge_user_app.py"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_iap_root}/tools/gen_partitions.py"  "${_iap_outdir}/gen_partitions.py"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_iap_root}/tools/gen_factory_cfg.py" "${_iap_outdir}/gen_factory_cfg.py"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_iap_embed}" "${_iap_outdir}/gen_boot_embed.py"
    COMMAND ${CMAKE_COMMAND} -E echo "工具已同步到 build/"
    USES_TERMINAL
    COMMENT "同步配套工具 -> build/"
)

# --- 4. 一键: 全部生成 + 同步 ---
add_custom_target(release-all
    COMMAND ${CMAKE_COMMAND} -E echo "=== release-all: 生成全部发布产物 ==="
    USES_TERMINAL
    COMMENT "生成全部发布产物"
)
add_dependencies(release-all release release-user release-tools)

# --- 5. 内置镜像数据 (供浏览器工具惰性加载) ---
#
# ⚠️ esp_iap_builtin.js 与 assets/iap_img_*.js 是**构建产物**，
#    默认**不写入开发项目** (见 .gitignore)。由 GitHub Actions 在构建
#    页面时生成。本地如需预览，用下面的目标显式生成到 build/ 目录。
#
# 需要各芯片的 iap_side.bin 已就绪; 查找顺序见 gen_boot_embed.py:
#   <build-root>/iap_<target>/build/iap_side.bin
#   <build-root>/iap_<target>/build/bootloader/bootloader.bin (+ 自动合成)
# 可用 -DIAP_EMBED_BUILD_ROOT=<dir> 覆盖默认的 C:\temp。
if(NOT DEFINED IAP_EMBED_BUILD_ROOT)
    set(IAP_EMBED_BUILD_ROOT "C:/temp")
endif()

# 输出目录: 默认 build/ (不污染源码树); 可用 -DIAP_EMBED_OUT_DIR=<dir> 覆盖
if(NOT DEFINED IAP_EMBED_OUT_DIR)
    set(IAP_EMBED_OUT_DIR "${_iap_outdir}")
endif()

add_custom_target(release-embed
    COMMAND ${CMAKE_COMMAND} -E make_directory "${IAP_EMBED_OUT_DIR}/assets"
    COMMAND ${PYTHON} "${_iap_embed}"
            --build-root "${IAP_EMBED_BUILD_ROOT}"
            --out-js "${IAP_EMBED_OUT_DIR}/esp_iap_builtin.js"
            --out-dir "${IAP_EMBED_OUT_DIR}/assets"
    USES_TERMINAL
    COMMENT "重建内置镜像索引 + 分芯片数据 -> ${IAP_EMBED_OUT_DIR}"
)
