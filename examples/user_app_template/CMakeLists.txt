# ESP IAP 用户程序模板
#
# 编译: idf.py set-target <chip> && idf.py build
# 烧录: build/user_app_template.bin 直接可用（无需打包）

cmake_minimum_required(VERSION 3.16)

# 工程名 (决定产物 bin 文件名)
set(PROJECT_NAME "user_app_template")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(${PROJECT_NAME})
