/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file bootloader_start.c
 * @brief 自定义二级 bootloader —— 不读分区表, 只读 RTC RAM 决定启动谁
 *
 * ============================================================================
 *  设计要点 (详见 docs/FINAL-REPORT.md §3 / §8.2)
 * ============================================================================
 *
 *  1. **完全不读分区表** —— 不调用 bootloader_utility_load_partition_table()
 *  2. **完全不读 flash** —— 启动决策来自 RTC RAM (IAP 预先写入)
 *  3. **手工构造 bootloader_state_t** —— 绕过 index_to_partition() 的分区查找
 *  4. **不做任何 GPIO 检测** —— GPIO0 救援入口在 IAP 内 (iap_wait_trigger.c)
 *
 * 启动决策流程:
 *
 *   RTC RAM (rtc_retain_mem_t.custom[])
 *     ├─ magic/CRC 有效 且 boot_target == APP
 *     │    → 从 p->user_addr 加载用户程序
 *     └─ 其他 (冷启动 / 数据无效 / boot_target == IAP)
 *          → 从 IAP_APP_ADDR (0x10000) 加载 IAP
 *
 * 为什么可行 (源码依据):
 *
 *   bootloader_utility_load_boot_image() (bootloader_utility.c:578)
 *     → index_to_partition(bs, index)   (:275)
 *         → if (index == FACTORY_INDEX) return bs->factory;   ← 只从 bs 读
 *     → try_load_partition(&part, ...)   (:472)
 *         → 只用 partition->offset / partition->size
 *
 *   bs 的内容由调用者填充, 因此可完全绕过分区表。
 *
 * ============================================================================
 *  目录约定 (IDF 官方机制)
 * ============================================================================
 *
 *   项目根/bootloader_components/main/
 *     ├── CMakeLists.txt
 *     └── bootloader_start.c     ← 本文件, 定义 call_start_cpu0()
 *
 *   组件名为 main 时, 会**覆盖整个二级 bootloader**。
 *   参考官方示例: examples/custom_bootloader/bootloader_multiboot/
 *
 * ============================================================================
 *  注意
 * ============================================================================
 *
 *   - 本文件在 **bootloader 环境** 编译 (BOOTLOADER_BUILD 已定义)
 *   - 可用 API 有限: 无完整 printf (用 esp_rom_printf), 无 malloc
 *   - 所有被调函数必须在 iram_loader_seg 中 (IDF 链接脚本已处理)
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/reent.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_image_format.h"
#include "esp_rom_crc.h"
#include "esp_rom_sys.h"

#include "bootloader_init.h"
#include "bootloader_utility.h"
#include "bootloader_common.h"

/* 共享的启动参数结构 (RTC RAM) */
#include "iap_boot_param.h"

/* ============================================================================
 * 硬编码地址 (与 main/iap_common.h 保持一致)
 *
 * 注意: bootloader 不能包含 iap_common.h (它依赖 esp_err.h 等 APP 侧头文件),
 *       因此这里重复定义这几个关键常量。
 *       修改时务必同步 main/iap_common.h。
 * ========================================================================== */

/** IAP 程序区地址 (固定区) */
#define IAP_APP_ADDR          0x010000

/** IAP 程序区最大大小 (0x140000 - 0x10000 = 0x130000 = 1216KB) */
#define IAP_APP_MAX_SIZE      0x130000

static const char *TAG = "boot";

/* ============================================================================
 * 辅助函数
 * ========================================================================== */

/**
 * @brief 判断 RTC RAM 中的启动参数是否有效
 */
static bool iap_param_is_valid(const iap_boot_param_t *p)
{
    if (p->magic != IAP_PARAM_MAGIC) {
        return false;
    }
    uint32_t crc = esp_rom_crc32_le(UINT32_MAX, (const uint8_t *)p,
                                    offsetof(iap_boot_param_t, crc32));
    return crc == p->crc32;
}

/**
 * @brief 从指定地址加载并启动镜像
 *
 * 手工构造 bootloader_state_t, 使 bootloader_utility_load_boot_image()
 * 直接加载该地址 —— 全程不触碰 flash 分区表。
 *
 * @param offset 镜像在 flash 中的偏移
 * @param size   镜像可用大小
 */
static void __attribute__((noreturn)) boot_from_addr(uint32_t offset, uint32_t size)
{
    bootloader_state_t bs = {0};

    bs.factory.offset = offset;
    bs.factory.size   = size;
    bs.app_count      = 0;      /* 无 OTA 槽 */

    ESP_LOGI(TAG, "加载镜像 @ 0x%08" PRIx32 " (size=0x%" PRIx32 ")", offset, size);

    /* 不会返回 */
    bootloader_utility_load_boot_image(&bs, FACTORY_INDEX);

    /* 理论上不可达; 兜底复位 */
    ESP_LOGE(TAG, "load_boot_image 意外返回, 复位");
    bootloader_reset();
}

/* ============================================================================
 * 入口
 * ========================================================================== */

/*
 * ROM bootloader 加载完二级 bootloader 后进入此处。
 * 硬件大部分未初始化, flash cache 未开启, APP CPU 处于复位状态。
 * 有栈可用, 因此可以在 C 中做初始化。
 */
void __attribute__((noreturn)) call_start_cpu0(void)
{
    /* 1. 硬件初始化 */
    if (bootloader_init() != ESP_OK) {
        bootloader_reset();
    }

#ifdef CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP
    /* 深睡眠唤醒的快速路径 (保持 IDF 行为) */
    bootloader_utility_load_boot_image_from_deep_sleep();
#endif

    /*
     * 2. 读 RTC RAM 决定启动谁
     *
     * ★ 不调用 bootloader_utility_load_partition_table() —— 完全不读分区表
     * ★ 不读任何 flash —— 启动决策来自 IAP 预先写入的 RTC RAM
     */
    rtc_retain_mem_t *mem = bootloader_common_get_rtc_retain_mem();
    iap_boot_param_t *p = (iap_boot_param_t *)mem->custom;

    if (iap_param_is_valid(p) && p->boot_target == IAP_BOOT_TARGET_APP) {
        /* --- 请求启动用户程序 --- */

        /*
         * 直接按 RTC RAM 请求启动用户程序。
         *
         * 镜像合法性由 bootloader_utility_load_boot_image() 内部校验
         * (magic / 段表 / SHA256 / chip_id)；校验失败时它会自动尝试
         * 下一个启动项，最终回落到 IAP (factory)。
         *
         * 不做任何重试计数 —— 启动决策完全由 IAP 侧控制。
         */
        ESP_LOGI(TAG, "RTC RAM: 启动用户程序 @ 0x%08" PRIx32,
                 p->user_addr);
        boot_from_addr(p->user_addr, p->user_size);
    }

    /* --- 启动 IAP (冷启动 / 数据无效 / 强制回 IAP) --- */
    ESP_LOGI(TAG, "RTC RAM: 启动 IAP @ 0x%08X", (unsigned)IAP_APP_ADDR);
    boot_from_addr(IAP_APP_ADDR, IAP_APP_MAX_SIZE);
}

#if CONFIG_LIBC_NEWLIB
/* 若 bootloader 链接了 newlib 函数, 需提供 _reent */
struct _reent *__getreent(void)
{
    return _GLOBAL_REENT;
}
#endif
