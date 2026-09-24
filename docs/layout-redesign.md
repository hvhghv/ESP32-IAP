# IAP 启动框架重构方案（双分区表 + 自定义 bootloader）

> ⚠️ **历史文档** —— 本文描述 v5 设计阶段，**部分已过时**。
> 当前实现（v6）请以 [`README.md`](../README.md) 为准。
> 主要差异: 用户程序烧录地址已从 `0x210000` 改为 `0x200000`（单一合并镜像）。

> 状态: **待审阅** — 请确认后再动代码
> 日期: 2026-09-22
> 版本: **v5**（v4 的 flash 标志区方案已按用户建议改为 **RTC RAM**）
> 目标: bootloader 不读分区表 + **IAP/APP 各用独立分区表** + 全地址硬编码

---

## 0. v5 变更摘要（**先读这里**）

**v4 → v5 的核心变更**：**启动决策从 flash 标志区改为 RTC RAM**。

| 项 | v4（已废弃） | **v5（当前）** |
|---|-------------|---------------|
| 启动决策存放 | flash 标志区 `0xA000`（`iap_mark`） | **RTC RAM `custom[]`** |
| bootloader 读 | flash 标志区 | **RTC RAM** |
| bootloader 复制参数？ | ✅ 需要（读 iap_cfg → 写 RTC RAM） | ❌ **不需要**（IAP 已写好） |
| flash 写次数 | 每次切换写 flash | **0 次** |
| `iap_mark` 分区/区域 | 需要 | **取消** |
| `iap_flag.c/.h` | 需要 | **取消** |
| HTML「强制进入 IAP」按钮 | 需要 | **取消**（替代方案见 §3.3） |
| `marker` UART 命令 | 需要 | **取消** |
| `tools/verify_marker.py` | 需要 | **取消** |

**RTC RAM 访问方式**（IDF 官方机制）：
```c
rtc_retain_mem_t *mem = bootloader_common_get_rtc_retain_mem();
iap_boot_param_t *p = (iap_boot_param_t *)mem->custom;
```

**为什么不用 `0x50000000`**：那是 APP 的 `lp_ram_seg` 起始，会被 APP 使用。
正确位置是 LP RAM **末尾保留区** `bootloader_data_rtc_mem`。

> ⚠️ **本文档第 4 章（标志区设计）已过时**，保留仅作历史记录。
> 最新设计以 `docs/FINAL-REPORT.md` 为准。
>
> **注意**：标志区取消后，**「强制进入 IAP」能力改由「断电重上电」提供**
> （RTC RAM 断电失效 → 默认进 IAP）。详见 `FINAL-REPORT.md` §3.3。

---

## 1. 需求汇总

### 1.1 最终设计（v5 核心）

```
┌──────────────────────────────────────────────────────────────────┐
│  bootloader（自定义，**完全不读分区表，不读 flash**）             │
│                                                                  │
│  1. 读 RTC RAM (bootloader_common_get_rtc_retain_mem)            │
│  2. 按 boot_target 加载:                                         │
│       target=0 → IAP     @ 0x010000  (硬编码)                    │
│       target=1 → 用户态  @ param.user_addr (RTC RAM 提供)        │
│  3. 跳转（不读分区表、不传参、**不复制参数**）                    │
├──────────────────────────────────────────────────────────────────┤
│  IAP 程序（**独立编译**，用自己的分区表 A）                       │
│                                                                  │
│  编译期: CONFIG_PARTITION_TABLE_OFFSET = 0x00B000                │
│  → esp_partition_find_first() 读表 A                             │
│  → nvs_flash_init_partition("nvs_iap") ✅                        │
│  → WiFi 驱动正常工作                                             │
│                                                                  │
│  配置区: 硬编码 0x8000（不在分区表）                             │
│  ★ 切换启动目标时: 读 iap_cfg → 写 RTC RAM → esp_restart()       │
├──────────────────────────────────────────────────────────────────┤
│  APP 程序（**独立编译**，用自己的分区表 B）                       │
│                                                                  │
│  编译期: CONFIG_PARTITION_TABLE_OFFSET = 0x200000                │
│  → esp_partition_find_first() 读表 B                             │
│  → nvs_flash_init_partition("nvs_app") ✅                        │
│  → WiFi 驱动正常工作                                             │
│                                                                  │
│  ★ 读 RTC RAM 拿配置（IAP 写入的）                               │
└──────────────────────────────────────────────────────────────────┘
```

### 1.2 核心设计要点

| # | 设计 | 说明 |
|---|------|------|
| 1 | **bootloader 不读分区表** | 全硬编码 + **RTC RAM** 启动决策 |
| 2 | **IAP / APP 各用独立分区表** | 编译期 `ESP_PARTITION_TABLE_OFFSET` 不同 |
| 3 | **`iap_cfg` 硬编码** | **不在任何分区表**，两边约定同一地址 |
| 4 | **各自独立 nvs** | `nvs_iap`（表 A）/ `nvs_app`（表 B） |
| 5 | **隔离 + 安全** | APP 看不到 IAP 的分区（反之亦然） |
| 6 | **启动决策 + 配置均放 RTC RAM** | IAP 写，bootloader/APP 只读（v5 新增） |

### 1.3 已确认参数

| 项 | 值 |
|---|-----|
| 自定义 bootloader | **C1（基于 IDF 精简）** |
| `iap_cfg` | **`0x008000`**，8KB，**硬编码，不在分区表** |
| `iap_mark` | **`0x00A000`**，4KB，**硬编码，不在分区表** |
| 分区表 A（IAP） | `0x00B000`，4KB |
| `nvs_iap` | `0x00C000`，**12KB** |
| IAP 程序 | `0x010000`，~1.94MB，硬编码 |
| `nvs_app` | `0x201000`，**12KB** |
| 分区表 B（APP） | `0x200000`，4KB |
| `user_app` | `0x210000`，剩余 |
| bootloader 目录 | **`bootloader_components/main`**（IDF 官方覆盖机制） |
| bootloader 加载方式 | **手工构造 `bootloader_state_t`**，不读分区表 |
| 标志区计数上限 | **加**（防砖） |

### 1.4 关键分工原则

| 组件 | 读分区表？ | 读哪个表 | 配置区 | 标志区 |
|------|:---------:|:--------:|:------:|:------:|
| **bootloader** | ❌ **不读** | — | ❌ | ✅ 硬编码 |
| **IAP 程序** | ✅ 读 | **表 A** (`0xB000`) | ✅ 硬编码 | ✅ 硬编码 |
| **APP 程序** | ✅ 读 | **表 B** (`0x200000`) | ✅ 硬编码 | ✅ 硬编码 |

> **为什么 IAP/APP 能各用独立分区表**：两者**分别编译**，
> 各自的 `CONFIG_PARTITION_TABLE_OFFSET` 可以不同。
> `esp_partition_find_first()` 内部用**本程序的**编译期常量
> （`partition.c:128`），因此互不干扰。

> **为什么 `iap_cfg` / `iap_mark` 不在分区表**：它们是**共享资源**，
> 由 IAP 与 APP **约定同一地址**（编译期常量）。放在分区表反而
> 破坏隔离性（两边都能看到）。

---

## 2. 为什么可以做到"bootloader 不读分区表"

### 2.1 标准 bootloader 为什么要读

| 原因 | 说明 |
|------|------|
| 不知道 app 在哪 | `bs->factory = partition->pos`（`bootloader_utility.c:175`） |
| 支持多 OTA 槽 | `bs->ota[N] = partition->pos`（`:191`） |
| 适配不同 flash 容量 | 分区表由用户配置 |

### 2.2 你的方案如何绕过

| 标准做法 | 你的做法 |
|---------|---------|
| 读分区表找 factory | **硬编码 `0x10000`** |
| 读分区表找 ota_0 | **读标志区的 `user_addr`** |
| 读 otadata 决定启动谁 | **读标志区的 `boot_target`** |

**代价**：改 flash 容量 / 改 IAP 位置 → 必须重编 bootloader。

### 2.3 IDF 官方预留的扩展点

```c
// components/bootloader/subproject/main/bootloader_start.c:130
// Customer implementation.
// if (gpio_pin_1 == true && ...){
//     boot_index = required_boot_partition;
// } ...
```

---

## 3. 新分区布局

### 3.1 固定区（IAP 侧，全硬编码）

| 偏移 | 结束 | 大小 | 名称 | 在分区表？ | 谁定义 |
|------|------|------|------|:---------:|--------|
| `0x000000` | `0x007FFF` | 32KB | **bootloader** | — | ROM |
| `0x008000` | `0x009FFF` | 8KB | **iap_cfg** | ❌ **不在** | `IAP_CFG_ADDR` |
| `0x00A000` | `0x00AFFF` | 4KB | (保留) | — | — |
| `0x00B000` | `0x00BFFF` | 4KB | **分区表 A** | — | `CONFIG_PARTITION_TABLE_OFFSET` |
| `0x00C000` | `0x00EFFF` | **12KB** | **nvs_iap** | ✅ 表 A | 分区表 A |
| `0x00F000` | `0x00FFFF` | 4KB | (保留) | — | — |
| `0x010000` | `0x1FFFFF` | ~1.94MB | **IAP 程序** | ✅ 表 A | `IAP_APP_ADDR` |

> ⚠️ **v5 变更**：原 `0x00A000` 的 `iap_mark` 标志区**已取消** ——
> 启动决策改放 RTC RAM（`bootloader_common_get_rtc_retain_mem()->custom`）。

> ⚠️ **`nvs_iap` 必须 ≥ 12KB**：IDF `gen_esp32part.py:49,599` 要求可读写 NVS
> 至少 `0x3000`（`NVS_RW_MIN_PARTITION_SIZE`）。标记 `readonly` 不可行
> （WiFi 驱动必须写 NVS，`esp_adapter.c:340`）。
>
> ⚠️ **`iap` 偏移必须 64KB 对齐**：`app` 类型分区的 Offset/Size 须为 `0x10000`
> 的倍数（`gen_esp32part.py:107,119-123,570-573`）。`0x10000` 与 `0x1F0000`
> 均为 64KB 倍数 ✅

### 3.2 可变区（APP 侧）

| 偏移 | 结束 | 大小 | 名称 | 在分区表？ | 说明 |
|------|------|------|------|:---------:|------|
| `0x200000` | `0x20FFFF` | **64KB** | **nvs_app** | ✅ 表 B | APP 的 nvs |
| `0x200000` | `0x200FFF` | 4KB | **分区表 B** | — | `CONFIG_PARTITION_TABLE_OFFSET` |
| `0x201000` | `0x203FFF` | **12KB** | **nvs_app** | ✅ 表 B | 分区表 B |
| `0x210000` | 剩余 | 可变 | **user_app** | ✅ 表 B | APP 自己 |
| 其后 | 可变 | 可变 | 追加分区 | ✅ 表 B | 可自由追加 |

> ⚠️ **`nvs_app` 取 12KB 且从 `0x201000` 开始的原因**：
> IDF 要求可读写 NVS ≥ 12KB；且 `gen_esp32part.py` 校验
> **分区不与分区表自身重叠**、分区表**必须在所有分区之前**。
> 因此分区表 B 占 `0x200000`，`nvs_app` 从 `0x201000` 开始。
> `0x204000`~`0x20FFFF` 为保留空隙，使 `user_app` 落在 `0x210000`（64KB 对齐）。

### 3.3 两个分区表的内容

**分区表 A（IAP，`0x00B000`）**：
```csv
# Name,     Type,    SubType, Offset,   Size,     Flags
nvs_iap,    data,    nvs,     0x00C000, 0x3000,
iap,        app,     factory, 0x010000, 0x1F0000,
```

| 项 | 偏移 | 大小 | 谁用 |
|---|------|------|------|
| `nvs_iap` | `0xC000` | 12KB | `nvs_flash_init_partition("nvs_iap")` / WiFi 驱动 |
| `iap` | `0x10000` | `0x1F0000` | IAP 代码 `esp_partition_find_first(APP, FACTORY, "iap")` 取自身句柄 |

**分区表 B（APP，`0x200000`）**：
```csv
# Name,     Type,    SubType, Offset,   Size,     Flags
nvs_app,    data,    nvs,     0x201000, 0x3000,
user_app,   app,     ota_0,   0x210000, 0x1F0000,
# 追加分区示例:
# storage,  data,    spiffs,  0x3F0000, 0x10000,
```

| 项 | 偏移 | 大小 | 谁用 |
|---|------|------|------|
| `nvs_app` | `0x201000` | 12KB | `nvs_flash_init_partition("nvs_app")` / WiFi 驱动 |
| `user_app` | `0x210000` | `0x1F0000` | APP 自身运行分区 |

> **注意**：两个表都**不含** `iap_cfg` —— 它是硬编码共享资源。
> （原 `iap_mark` 标志区已取消，启动决策改放 RTC RAM）
> 也**不含** `otadata` / `phy_init` —— 已删除（见第 9 章）。

> **校验**：两个表均已通过 `gen_esp32part.py --verify` ✅

### 3.4 布局图

```
┌────────────────────────────────────────────────────────────┐
│  固定区 (0x0 ~ 0x1FFFFF, 2MB)                              │
│  地址编译期硬编码                                          │
│                                                            │
│  0x000000  ┌────────────────────────┐                      │
│            │  bootloader    (32KB)  │  ROM                 │
│  0x008000  ├────────────────────────┤                      │
│            │  iap_cfg        (8KB)  │  ★ 硬编码，不在表    │
│  0x00A000  ├────────────────────────┤                      │
│            │  (保留)         (4KB)  │  原 iap_mark         │
│  0x00B000  ├────────────────────────┤                      │
│            │  分区表 A       (4KB)  │  IAP 用              │
│  0x00C000  ├────────────────────────┤                      │
│            │  nvs_iap       (12KB)  │  表 A 定义           │
│  0x00F000  ├────────────────────────┤                      │
│            │  (保留)         (4KB)  │                      │
│  0x010000  ├────────────────────────┤                      │
│            │                        │                      │
│            │  IAP 程序    (~1.94MB) │  表 A 定义           │
│            │                        │                      │
│  0x200000  └────────────────────────┘                      │
├────────────────────────────────────────────────────────────┤
│  可变区 (0x200000 ~ 末尾)                                  │
│                                                            │
│  0x200000  ┌────────────────────────┐                      │
│            │  分区表 B       (4KB)  │  APP 用              │
│  0x201000  ├────────────────────────┤                      │
│            │  nvs_app       (12KB)  │  表 B 定义           │
│  0x210000  ├────────────────────────┤                      │
│            │  user_app              │  表 B 定义           │
│            ├────────────────────────┤                      │
│            │  追加分区 (可选)       │                      │
│            └────────────────────────┘                      │
└────────────────────────────────────────────────────────────┘
```

### 3.5 与当前布局对比

| 项 | 当前 | 新方案 | 变化 |
|----|------|--------|------|
| bootloader | `0x0` 32KB | `0x0` 32KB | — |
| 分区表 | `0x8000` 1 个 | **`0xB000` + `0x200000` 2 个** | ⚠️ 双表 |
| nvs | `0x9000` 24KB | **`0xC000` + `0x200000`** | ⚠️ 双 nvs |
| phy_init | `0xF000` 4KB | **删除**（无代码引用） | ⚠️ 删除 |
| 配置区 | `0x10000` 8KB | **`0x8000`** 8KB | ⚠️ 移动 |
| 标志区 | `0x14000` 4KB | **取消**（改用 RTC RAM） | ⚠️ 删除 |
| otadata | `0x12000` 8KB | **删除** | ⚠️ 删除 |
| IAP 程序 | `0x20000` | **`0x10000`** | ⚠️ 移到固定区 |
| 用户态 | `0x210000` | **`0x210000`** | — 不变 |

### 3.6 空间对比（4MB flash）

| 区域 | 当前 | 新方案 |
|------|------|--------|
| 固定区 | 56KB | **2MB**（含 IAP） |
| 用户态 | 1984KB | **~1.99MB** |

> **结论**：用户态空间基本不变。

---

## 4. ~~标志区设计~~（**v5 已废弃，改用 RTC RAM**）

> ⚠️ **本章为 v4 历史记录，已废弃**。
>
> **v5 变更**：启动决策改放 RTC RAM
> （`bootloader_common_get_rtc_retain_mem()->custom`），
> 不再使用 flash `0xA000` 标志区。详见 **第 5 章**。
>
> **废弃原因**（用户建议）：
> 1. 每次切换要写 flash（磨损）
> 2. bootloader 需读 flash + 写 RTC RAM（两步）
> 3. 与配置区若同扇区有原子性问题
>
> 以下内容仅作历史参考。

### 4.1 原结构（v4，已废弃）

```c
/* 标志区布局 (4KB = 1 个 flash 扇区) */
#define IAP_FLAG_MAGIC  0x49415046U   /* "IAPF" */

typedef struct {
    uint32_t magic;          /* IAP_FLAG_MAGIC */
    uint8_t  boot_target;    /* 0 = IAP, 1 = user_app */
    uint8_t  reserved0[3];
    uint32_t user_addr;      /* ★ 用户态加载地址 (bootloader 用它!) */
    uint32_t user_size;      /* ★ 用户态大小 */
    uint32_t boot_count;     /* 累计启动次数 (防砖用) */
    uint32_t last_reason;    /* 上次启动原因 (复用 iap_boot_reason_t) */
    uint32_t user_version;   /* 用户态版本号 (用户程序上报) */
    uint32_t crc32;          /* 以上字段的 CRC32 */
    uint8_t  reserved1[4096 - 32];
} iap_flag_t;
```

> **★ 关键字段 `user_addr`**：bootloader **不读分区表**，用户态加载地址
> 由 IAP 从配置区读取后写入标志区。这是"bootloader 不读分区表"的关键。

### 4.2 启动流程（最终版）

```
┌──────────────────────────────────────────────────────────────┐
│ 上电                                                         │
│   ↓                                                          │
│ ROM → 加载 bootloader (0x0)                                  │
│   ↓                                                          │
│ 自定义 bootloader（**不读分区表**）:                          │
│   1. 读标志区 (0x00A000, 硬编码)                             │
│   2. 校验 magic + CRC                                        │
│   3. 若无效 → 默认 boot_target=0 (IAP)                       │
│   4. 按 boot_target 决定加载地址:                            │
│        target=0 → addr = 0x010000 (IAP, 硬编码)              │
│        target=1 → addr = flag.user_addr (标志区提供)         │
│   5. 从 addr 加载镜像 → 跳转（不传参）                       │
│                                                              │
│ 【场景 A: IAP → 用户态】                                     │
│   IAP 运行 → 用户点"启动用户程序"                             │
│     → IAP 读配置区 (0x8000) 得到 user_addr                   │
│     → IAP 写标志区: boot_target=1, user_addr=0x200000        │
│     → IAP 调用 esp_restart()                                 │
│     → bootloader 读标志区 → 从 0x200000 加载 → 启动用户态    │
│                                                              │
│ 【场景 B: 用户态 → IAP】                                     │
│   用户程序写标志区: boot_target=0                             │
│     → 用户程序 esp_restart()                                 │
│     → bootloader 读标志区 → 从 0x10000 加载 → 启动 IAP       │
└──────────────────────────────────────────────────────────────┘
```

**关键**：
1. bootloader **完全不读分区表**，只读标志区
2. IAP **不再调用 `esp_ota_set_boot_partition()`**
3. 用户态地址**存在标志区**，由 IAP 从配置区读取后写入

### 4.3 防砖机制（启动计数上限）

```
bootloader 启动用户态前:
  1. boot_count++
  2. 写回标志区
  3. 若 boot_count > MAX (如 5):
       → 强制 boot_target = 0 (IAP)
       → 重置 boot_count
       → 日志告警

用户程序启动成功后:
  调用 iap_user_report_boot_ok() → 清零 boot_count
```

**作用**：用户程序反复崩溃 → boot_count 累积 → 自动回 IAP 救援。

### 4.4 标志区 vs otadata

| | otadata | 标志区（新） |
|---|---------|-------------|
| 位置 | 分区表定义 | **硬编码 `0xA000`** |
| 谁写 | `esp_ota_set_boot_partition()` | IAP / 用户程序直接写 |
| 谁读 | 标准 bootloader | **自定义 bootloader** |
| 内容 | ota_seq + state | 启动目标 + 计数 + 原因 + 版本 |
| 可扩展 | ❌ 格式固定 | ✅ 自定义 |

---

## 5. 参数传递 + 启动决策（**v5：全部放 RTC RAM**）

### 5.1 需求

**两个需求合并到 RTC RAM**：

| 需求 | 原因 |
|------|------|
| ① APP 需要知道配置 | `iap_cfg` **不在分区表 B**，APP 找不到 |
| ② bootloader 需要知道启动谁 | **不读分区表**，也不读 flash |

**v5 优化**（用户建议）：**由 IAP 统一写入 RTC RAM**，bootloader 只读不复制。

```
IAP:        读 iap_cfg → 写 RTC RAM（boot_target + user_addr + 配置副本）→ esp_restart()
bootloader: 读 RTC RAM → 决定启动谁 → 跳转（★ 不复制参数）
APP:        读 RTC RAM → 拿配置
```

### 5.2 ✅ 采用 IDF 官方 `custom[]` 区（**不是 `0x50000000`**）

**IDF 官方结构**（`esp_image_format.h:58`）：
```c
typedef struct {
    esp_partition_pos_t partition;
    uint16_t reboot_counter;
    union { uint8_t factory_reset_state:1, reserve:7; uint8_t val; } flags;
    uint8_t reserve;
#ifdef CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC
    uint8_t custom[CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE];  /* ★ 官方留给用户 */
#endif
    uint32_t crc;
} rtc_retain_mem_t;
```

**访问 API**（bootloader 与 APP 通用）：
```c
rtc_retain_mem_t *mem = bootloader_common_get_rtc_retain_mem();
iap_boot_param_t *p = (iap_boot_param_t *)mem->custom;
```

**物理位置**（ESP32-C6，`esp32c6/memory.ld.in:97-133`）：
```
0x50000000                          LP RAM 起始
  lp_ram_seg (RW)                   len = 0x4000 - RESERVE_RTC_MEM
0x50004000 - RESERVE_RTC_MEM        lp_reserved_seg
  ├── bootloader_data_rtc_mem       ← rtc_retain_mem_t（含 custom[]）
  ├── rtc_timer_data_in_rtc_mem
  └── ...
0x50004000                          LP RAM 末尾
```

> ⚠️ **为什么不用 `0x50000000`**：那是 APP 的 `lp_ram_seg` 起始，
> **会被 APP 使用**（冲突）。`custom[]` 在 LP RAM **末尾保留区**，IDF 保证不被占用。

**Kconfig 配置**：
```ini
CONFIG_BOOTLOADER_RESERVE_RTC_MEM=y
CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC=y
CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE=512   # >= sizeof(iap_boot_param_t)
```

### 5.3 两侧地址一致性依据

`bootloader_common_loader.c:268`：
```c
rtc_retain_mem_t* bootloader_common_get_rtc_retain_mem(void)
{
#ifdef BOOTLOADER_BUILD
    /* bootloader 侧：硬编码地址 */
    #define RTC_RETAIN_MEM_ADDR (SOC_RTC_DRAM_LOW)
    static rtc_retain_mem_t *const s_bootloader_retain_mem =
        (rtc_retain_mem_t *)(RTC_RETAIN_MEM_ADDR - CONFIG_SECURE_BOOT_ROM_FAST_WAKE_RESERVE_SIZE);
    return s_bootloader_retain_mem;
#else
    /* APP 侧：链接器放到 .bootloader_data_rtc_mem 段（同一物理地址） */
    static __attribute__((section(".bootloader_data_rtc_mem"))) rtc_retain_mem_t s_bootloader_retain_mem;
    return &s_bootloader_retain_mem;
#endif
}
```

> **官方验证**：IDF 自带测试 `bootloader_support/test_apps/rtc_custom_section/`
> 专门验证 `custom[]` 跨 `esp_restart()` 保持 —— 正是我们的用法。

### 5.4 bootloader 的 `.bss` 清零不会影响 RTC RAM

`bootloader/subproject/main/ld/esp32c6/bootloader.memory.ld.in` 的 `MEMORY`：
```
iram_seg          org = 0x4086xxxx    ← 内部 SRAM
iram_loader_seg   org = 0x4086xxxx    ← 内部 SRAM
dram_seg          org = 0x4086xxxx    ← 内部 SRAM
```

**不含 `0x50000000`** → bootloader 的 `.bss` 清零**不会碰 RTC RAM** ✅

### 5.5 共享头文件 `iap_boot_param.h`

```c
/* 通过 IDF 官方 API 访问，不硬编码地址 */
#define IAP_PARAM_MAGIC   0x49415042U   /* "IAPB" */

typedef struct {
    uint32_t magic;          /* IAP_PARAM_MAGIC */
    uint32_t crc32;          /* 以下数据的 CRC32 */
    uint32_t layout_ver;     /* 布局版本 */
    uint32_t cfg_addr;       /* 0x8000（配置区地址） */
    uint32_t cfg_seq;        /* 配置槽序号 */

    /* --- 启动决策（v5：原 flash 标志区的职责） --- */
    uint8_t  boot_target;    /* 0 = IAP, 1 = APP */
    uint8_t  reserved0[3];
    uint32_t user_addr;      /* APP 加载地址（0x210000） */
    uint32_t user_size;
    uint32_t user_version;

    /* --- APP 需要的配置子集（自定义） --- */
    uint32_t flags;
    uint8_t  wifi_ssid[32];
    /* ... 按需添加 ... */
} iap_boot_param_t;

/* 访问辅助（bootloader 与 APP 通用） */
static inline iap_boot_param_t *iap_param_get(void)
{
    rtc_retain_mem_t *mem = bootloader_common_get_rtc_retain_mem();
    return (iap_boot_param_t *)mem->custom;
}
```

**IAP 侧（写）**：
```c
iap_boot_param_t *bp = iap_param_get();
bp->magic       = IAP_PARAM_MAGIC;
bp->layout_ver  = IAP_LAYOUT_VER;
bp->cfg_addr    = IAP_CFG_ADDR;
bp->cfg_seq     = seq;
bp->boot_target = 1;                 /* ★ 启动 APP */
bp->user_addr   = 0x210000;
bp->user_size   = 0x1F0000;
memcpy(bp->wifi_ssid, cfg.wifi_ssid, sizeof(bp->wifi_ssid));
bp->crc32       = iap_crc32(bp, offsetof(iap_boot_param_t, crc32));
```

**bootloader 侧（读，不写）**：
```c
iap_boot_param_t *bp = iap_param_get();
if (bp->magic == IAP_PARAM_MAGIC && iap_param_crc_ok(bp)) {
    /* 按 bp->boot_target 决定 */
}
```

**APP 侧（读）**：
```c
iap_boot_param_t *bp = iap_param_get();
if (bp->magic == IAP_PARAM_MAGIC && iap_param_crc_ok(bp)) {
    /* 使用 bp->... */
}
```

### 5.6 保留性验证

| 场景 | RTC RAM 保留？ | 行为 |
|------|:-------------:|------|
| `esp_restart()`（软复位） | ✅ **保留** | 按 `boot_target` 启动 |
| 看门狗复位 | ✅ 保留 | 按 `boot_target` 启动 |
| 深睡眠唤醒 | ✅ 保留 | 按 `boot_target` 启动 |
| **掉电重启** | ❌ 丢失 | **magic 失败 → 启动 IAP** ✅ |

**"每次上电必跑 IAP"仍然满足** —— 冷启动时 RTC RAM 无效，默认走 IAP。

### 5.7 堆空间影响

| 项 | 当前 | 禁用 RTC FAST 后 |
|----|------|-----------------|
| 空闲堆 | 141,520 字节 (138KB) | **~125KB** |
| 最小空闲堆 | 131,056 字节 (128KB) | **~115KB** |

> **实测**：损失 16KB 堆，剩余仍然充足。
> **注意**：`custom[]` 位于 LP RAM **末尾保留区**，本就**不在堆范围内**，
> 与 `CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP` 无直接冲突；
> 但禁用该选项可进一步避免混淆。

---

## 6. 配置区新结构

### 6.1 新增字段

| 字段 | 类型 | 说明 | 谁读 |
|------|------|------|------|
| `layout_ver` | u16 | 布局版本（用于未来迁移） | 工具 |
| `ptable_addr` | u32 | 分区表地址（`0xB000`，**记录用**） | 工具 |
| `ptable_size` | u32 | 分区表大小（`0x1000`） | 工具 |
| `flag_addr` | u32 | 标志区地址（`0xA000`） | 工具 |
| **`user_addr`** | u32 | **用户态加载地址（`0x200000`）** | **★ IAP** |
| **`user_size`** | u32 | **用户态大小** | **★ IAP** |

> **★ 关键**：`user_addr` / `user_size` 是 **IAP 写入标志区的数据源**。
> 流程：IAP 读配置区 → 得 `user_addr` → 写标志区 → bootloader 用。
> 这样 bootloader **无需读分区表**即可知道用户态在哪。

### 6.2 配置区访问方式变更

**当前**（依赖分区表）：
```c
s_cfg_part = esp_partition_find_first(0x40, 0x00, "iap_cfg");
esp_partition_read(s_cfg_part, off, buf, len);
```

**新方案**（硬编码直访）：
```c
#define IAP_CFG_ADDR  0x008000
#define IAP_CFG_SIZE  0x00002000

esp_flash_read(NULL, buf, IAP_CFG_ADDR + off, len);
```

**优势**：bootloader 也能用同样方式读配置区（不依赖分区表）。

---

## 7. 自定义 bootloader 实现

### 7.1 实现方式：覆盖整个 bootloader（IDF 官方机制）

**做法**：在项目根建 `bootloader_components/main/`，定义 `call_start_cpu0()`。
**组件名为 `main` 时会覆盖整个二级 bootloader**，无需改 IDF 源码。

**核心思路**：**不调用 `bootloader_utility_load_partition_table()`**，
改为**手工构造 `bootloader_state_t`** 后调用 `bootloader_utility_load_boot_image()`。

| 步骤 | 标准 bootloader | 我们的 bootloader |
|------|----------------|------------------|
| 硬件初始化 | `bootloader_init()` | 同 |
| 读分区表 | `load_partition_table(&bs)` | ❌ **不调用** |
| 选启动分区 | `get_selected_boot_partition(&bs)` 读 otadata | ✅ **读 RTC RAM** |
| 加载镜像 | `load_boot_image(&bs, idx)` | 同（但 `bs` 是手工填的） |

### 7.2 核心改动代码（**v5：不覆盖 IDF 函数，只写 `call_start_cpu0`**）

> **v5 简化**：不再覆盖 `bootloader_utility_load_partition_table()` 等函数，
> 而是**直接写自己的 `call_start_cpu0()`**，内部手工构造 `bootloader_state_t`。

```c
/* ========== 硬编码地址 ========== */
#define IAP_CFG_ADDR     0x008000
#define IAP_APP_ADDR     0x010000   /* ★ IAP 在固定区 */
#define IAP_APP_MAX_SIZE 0x1F0000   /* 0x200000 - 0x10000 ≈ 1.94MB */

/* ========== 启动决策（RTC RAM，v5） ========== */
void __attribute__((noreturn)) call_start_cpu0(void)
{
    if (bootloader_init() != ESP_OK) bootloader_reset();

    /* ★ 不读分区表，也不读 flash —— 只读 RTC RAM */
    iap_boot_param_t *p = iap_param_get();
    bootloader_state_t bs = {0};

    if (p->magic == IAP_PARAM_MAGIC && iap_param_crc_ok(p)
        && p->boot_target == 1) {
        /* 防砖：reboot_counter++ */
        bootloader_common_update_rtc_retain_mem(NULL, true);
        if (bootloader_common_get_rtc_retain_mem_reboot_counter() > 5) {
            p->boot_target = 0;                       /* 强制回 IAP */
            bootloader_common_reset_rtc_retain_mem();
        } else {
            /* ★ 手工构造 bs，绕过分区表 */
            bs.factory.offset = p->user_addr;         /* 0x210000 */
            bs.factory.size   = p->user_size;
            bs.app_count      = 0;
            bootloader_utility_load_boot_image(&bs, FACTORY_INDEX);
        }
    }

    /* 启动 IAP（冷启动 / 数据无效 / 强制回 IAP） */
    bs.factory.offset = IAP_APP_ADDR;                 /* 0x10000 */
    bs.factory.size   = IAP_APP_MAX_SIZE;             /* 0x1F0000 */
    bs.app_count      = 0;
    bootloader_utility_load_boot_image(&bs, FACTORY_INDEX);
    /* 不会返回 */
}
```

> **注意**：bootloader 里**没有 `memcpy` 复制参数** —— 参数由 IAP 提前写好，
> bootloader 只读。这正是 v5 的优化点。

### 7.3 目录结构

**IDF 官方约定**：项目根目录下的 `bootloader_components/main/` 会**覆盖整个二级 bootloader**。
组件名必须为 `main`。无需 `EXTRA_COMPONENT_DIRS`。

```
project/
├── bootloader_components/
│   └── main/                       ← 新增（IDF 约定目录名）
│       ├── CMakeLists.txt
│       └── bootloader_start.c      ← 定义 call_start_cpu0()
├── main/
└── CMakeLists.txt
```

**`bootloader_components/main/CMakeLists.txt`**：
```cmake
idf_component_register(SRCS "bootloader_start.c"
                    REQUIRES bootloader bootloader_support)

# 使用默认链接脚本
idf_build_get_property(scripts BOOTLOADER_LINKER_SCRIPT)
target_linker_script(${COMPONENT_LIB} INTERFACE "${scripts}")
```

> **依据**：`examples/custom_bootloader/bootloader_override/` 与
> `examples/custom_bootloader/bootloader_multiboot/` 两个官方示例。
> 后者（多启动菜单）与本项目场景最接近。

---

## 8. 烧录文件

### 8.1 两个文件（v5 最终方案）

| 文件 | 内容 | 烧录地址 | 大小 |
|------|------|---------|------|
| **`iap_side.bin`** | bootloader + iap_cfg + **分区表 A** + nvs_iap + **IAP 程序** | `0x0` | **2MB**（固定区整片） |
| **`user.bin`** | user_app（+ 随工程烧录的分区表 B / nvs_app） | `0x210000` | 可变 |

> **v5 简化**：分区表 A 已并入 `iap_side.bin`（单一连续文件 `0x0 ~ 0x1FFFFF`），
> 不再单独烧录。分区表 B 由用户程序工程管理（`PARTITION_TABLE_OFFSET=0x200000`），
> 随 `user.bin` 一起烧录。

### 8.2 烧录命令

```bash
esptool.py --chip esp32c6 -p COM18 write_flash \
    0x0      iap_side.bin \
    0x210000 user.bin
```

> **升级 IAP 时**：只重烧 `iap_side.bin`，APP 侧不受影响。
> **升级 APP 时**：只重烧 `user.bin`（含表 B），IAP 侧不受影响。

---

## 9. nvs / phy_init 说明

### 9.1 谁读写？

| 分区 | 谁读 | 谁写 | 必需？ |
|------|------|------|:------:|
| **nvs_iap** | IAP 的 WiFi 驱动 | IAP 的 WiFi 驱动 | ✅ **必需** |
| **nvs_app** | APP 的 WiFi 驱动 | APP 的 WiFi 驱动 | ✅ **必需** |
| **phy_init** | — | — | ❌ **已删除** |

### 9.2 为什么各自一个 nvs

```c
// nvs_api.cpp:204
esp_err_t nvs_flash_init(void) {
    return nvs_flash_init_partition(NVS_DEFAULT_PART_NAME);  /* "nvs" */
}
// → 内部通过分区名查找 → 读本程序的 ESP_PARTITION_TABLE_OFFSET
```

| 程序 | `ESP_PARTITION_TABLE_OFFSET` | 找到的 nvs |
|------|:---------------------------:|-----------|
| **IAP** | `0x00B000` | `nvs_iap` (`0xC000`, 12KB) |
| **APP** | `0x200000` | `nvs_app` (`0x201000`, 12KB) |

**两者互不干扰** —— 因为各自编译时 offset 不同。

### 9.3 phy_init 为何删除

实测搜索 IDF 源码，`PART_SUBTYPE_DATA_PHY` **零引用**。
PHY 校准数据实际存在 **NVS**（`esp_phy/src/phy_init.c:742`
的 `esp_phy_load_cal_data_from_nvs`）。

---

## 10. 代码改动清单

### 10.1 新增文件

| 文件 | 说明 |
|------|------|
| `bootloader_components/main/CMakeLists.txt` | 自定义 bootloader 组件（IDF 约定目录名） |
| `bootloader_components/main/bootloader_start.c` | 启动决策（覆盖 `call_start_cpu0()`） |
| `main/iap_boot_param.h` | RTC RAM 参数结构（IAP/bootloader/APP 共享） |
| `main/iap_boot_param.h` | RTC RAM 参数结构（IAP/APP 共享） |
| `partitions_app.csv` | 分区表 B |
| `docs/layout-redesign.md` | 本文档 |

### 10.2 修改文件

| 文件 | 改动 |
|------|------|
| `partitions_iap.csv` | 删除 otadata/phy_init；重排固定区；nvs_iap 取 12KB |
| `examples/user_app_template/partitions_user_app.csv` | 同步 |
| `sdkconfig.defaults` | `CONFIG_PARTITION_TABLE_OFFSET=0xB000` |
| `main/iap_common.h` | 新增地址常量；更新布局注释 |
| `main/iap_config.c` | 改用 `esp_flash_read/write` 直访；新增布局字段 |
| `main/iap_image.c` | 地址常量更新；跳转逻辑改**写 RTC RAM** |
| `main/iap_image.h` | 常量声明 |
| `main/main.c` | 启动决策简化（不再处理 otadata） |
| `main/iap_uart.c` | `part` 命令显示新布局；新增 `flag` 命令 |
| `tools/gen_partitions.py` | `FIXED_PARTITIONS` 重写 |
| `tools/merge_bin.py` | 地址常量重写；2 文件输出 |
| `tools/gen_factory_cfg.py` | 配置区地址 `0x8000`；新字段 |
| `tools/verify_html_editor.py` | 常量同步 |
| `tools/verify_marker.py` | **删除**（不再有 flash 标志区） |
| `main/iap_uart.c` | **删除 `marker` / `marker set` / `marker clear` 命令** |
| `esp_iap_tool.html` | **删除「强制进入 IAP」按钮 + `forceEnterIap()`** |
| `README.md` | **删除「强制进入 IAP —— flash 魔术标记」章节** |
| `esp_iap_tool.html` | 所有地址常量 + 槽位定义 + 识别逻辑 |
| `README.md` | 布局图 + 烧录说明 |
| `.github/workflows/build.yml` | 产物分段说明 |
| `examples/user_app_template/main/iap_user_api.h` | 结构体同步 |

### 10.3 删除内容

| 项 | 说明 |
|----|------|
| `otadata` 分区 | 由 RTC RAM 取代 |
| `esp_ota_set_boot_partition()` 调用 | 改为写 RTC RAM |
| `esp_ota_get_boot_partition()` 调用 | 改为读 RTC RAM |
| **flash 标志区 `iap_mark`** | **由 RTC RAM 取代**（v5） |
| **`iap_image_check_marker()` 等 marker API** | **随标志区一并删除** |
| **HTML「强制进入 IAP」按钮** | **随标志区一并删除** |
| **`marker` UART 命令** | **随标志区一并删除** |

---

## 11. 迁移步骤

### 11.1 一次性迁移（破坏性）

分区布局完全变了，**必须完整重烧**：

```bash
# 1. 擦除整个 flash
esptool.py --chip esp32c6 -p COM18 erase_flash

# 2. 烧录新布局
esptool.py --chip esp32c6 -p COM18 write_flash \
    0x0      bootloader.bin \
    0x10000  iap.bin \
    0x200000 user.bin
```

### 11.2 实施顺序

| 阶段 | 内容 | 验证 |
|------|------|------|
| 1 | 分区表 CSV + `sdkconfig.defaults` | `idf.py build` 通过 |
| 2 | 新增 `iap_boot_param.h` + RTC RAM 读写验证 | 实机验证跨复位保持 |
| 3 | 改 `iap_config.c`（硬编码直访） | 编译 + 实机读配置 |
| 4 | 改 `iap_image.c`（跳转改写 RTC RAM） | 编译 |
| 5 | **自定义 bootloader 组件** | `idf.py build` 生成新 bootloader |
| 6 | 改工具脚本 | 生成的二进制正确 |
| 7 | 改 HTML 工具 | `verify_html_editor.py` 通过 |
| 8 | 更新文档 | — |
| 9 | 实机验证 | 烧录 + 启动 + 双向切换 |

---

## 12. 风险与取舍

### 12.1 风险

| 风险 | 等级 | 缓解 |
|------|------|------|
| 破坏性变更（需重烧所有设备） | 🔴 高 | 一次性迁移，文档说明 |
| **自定义 bootloader 维护成本** | 🟡 中 | 只改 2 个函数；IDF 升级时需同步 |
| **bootloader 崩溃无日志** | 🟡 中 | 保留 `ESP_LOGI`；必要时 JTAG |
| 地址常量散落 15+ 文件 | 🟡 中 | 加 `verify_layout.py` 交叉校验 |
| nvs_iap 取 12KB 可能不够 | 🟢 低 | 报错则改大（IDF 下限 12KB） |
| RTC RAM 数据损坏 | 🟢 低 | magic/CRC 校验失败 → 默认启动 IAP |

### 12.2 关键取舍

| 取舍 | 选择 | 理由 |
|------|------|------|
| 标准 vs 自定义 bootloader | **自定义** | 第 4/5/6 条必须 |
| bootloader 读分区表？ | **不读** | 全硬编码，极简 |
| 启动决策载体 | **RTC RAM** | 无 flash 磨损、bootloader 无需复制参数 |
| 用户态地址来源 | **RTC RAM**（IAP 从配置区读后写入） | bootloader 无需读分区表 |
| 配置区 | **8KB** | 你的确认 |
| IAP 位置 | **`0x10000`**（硬编码） | 你的第 1 条 |

### 12.3 备选方案

| 方案 | 改动 | 能否满足 |
|------|------|---------|
| **A. 完整重构**（本文档） | 大 | ✅ 全部 6 条 |
| **B. 标准 bootloader + 删 otadata** | 小 | ❌ 第 4/5/6 条 |
| **C. 标准 bootloader + IAP 直跳** | 中 | ❌ 第 4/5/6 条 |

---

## 13. 确认状态

### 13.1 全部已确认 ✅

| # | 问题 | 结论 |
|---|------|------|
| 1 | `iap_cfg` 8KB 够吗？ | ✅ **够**（双槽 × 4KB） |
| 2 | nvs 大小？ | ✅ **nvs_iap 12KB / nvs_app 12KB**（IDF 强制约束） |
| 3 | IAP 放固定区？ | ✅ **可以**（`0x10000`，硬编码） |
| 4 | bootloader 读分区表？ | ✅ **完全不读** |
| 5 | 用户态地址来源？ | ✅ **RTC RAM**（IAP 从配置区读后写入） |
| 6 | `iap_cfg` 位置 | ✅ `0x008000`，**硬编码，不在分区表** |
| 7 | `iap_mark` 位置 | ✅ **取消**（v5：改用 RTC RAM） |
| 8 | 分区表 A（IAP） | ✅ `0x00B000` |
| 9 | 分区表 B（APP） | ✅ `0x200000` |
| 10 | nvs 安排 | ✅ **各自一个**（`nvs_iap` / `nvs_app`） |
| 11 | `phy_init` | ✅ **删除**（无代码引用） |
| 12 | 自定义 bootloader 目录名 | ✅ **`bootloader_components/main`**（IDF 官方覆盖机制） |
| 13 | 启动计数上限防砖？ | ✅ **加**（复用 `rtc_retain_mem_t.reboot_counter`） |
| 14 | bootloader 如何绕过分区表加载？ | ✅ **手工构造 `bootloader_state_t`**（见 14.3） |

**方案已全部确认，可以开始实施。**

---

## 14. 附：关键源码路径

### 14.1 标准 bootloader 的硬限制

```c
// components/bootloader_support/src/bootloader_utility.c:150
partitions = bootloader_mmap(ESP_PARTITION_TABLE_OFFSET, ESP_PARTITION_TABLE_MAX_LEN);
//                        ^^^^^^^^^^^^^^^^^^^^^^^^^^ 编译期常量

// components/bootloader_support/src/bootloader_utility.c:378
int bootloader_utility_get_selected_boot_partition(const bootloader_state_t *bs)
{
    int boot_index = FACTORY_INDEX;
    if (bs->ota_info.offset == 0) {
        return FACTORY_INDEX;      // 无 otadata → factory
    }
    ...
    int active_otadata = bootloader_common_get_active_otadata(otadata);
    ...
}
```

### 14.2 IDF 预留的扩展点

```c
// components/bootloader/subproject/main/bootloader_start.c:130
// Customer implementation.
// if (gpio_pin_1 == true && ...){
//     boot_index = required_boot_partition;
// } ...
```

### 14.3 自定义 bootloader 的挂载点

**IDF 官方机制（推荐，不改 IDF 源码）**：

```c
// components/bootloader/subproject/main/bootloader_start.c:52
static int select_partition_number(bootloader_state_t *bs)
{
    if (!bootloader_utility_load_partition_table(bs)) {   // ← 我们不调用
        return INVALID_INDEX;
    }
    return selected_boot_partition(bs);                    // ← 改读 RTC RAM
}
```

在项目根建 `bootloader_components/main/`，定义 `call_start_cpu0()` 覆盖整个 bootloader。
参考官方示例 `examples/custom_bootloader/bootloader_multiboot/`。

### 14.3.1 ★ RTC RAM 官方机制（v5 新增）

**IDF 官方结构**（`bootloader_support/include/esp_image_format.h:58`）：
```c
typedef struct {
    esp_partition_pos_t partition;
    uint16_t reboot_counter;
    union { uint8_t factory_reset_state:1, reserve:7; uint8_t val; } flags;
    uint8_t reserve;
#ifdef CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC
    uint8_t custom[CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE];  /* ★ 用户自定义 */
#endif
    uint32_t crc;
} rtc_retain_mem_t;
```

**访问函数**（`bootloader_support/src/bootloader_common_loader.c:268`）：
```c
rtc_retain_mem_t* bootloader_common_get_rtc_retain_mem(void)
{
#ifdef BOOTLOADER_BUILD
    #define RTC_RETAIN_MEM_ADDR (SOC_RTC_DRAM_LOW)   /* bootloader: 硬编码 */
    static rtc_retain_mem_t *const s_bootloader_retain_mem =
        (rtc_retain_mem_t *)(RTC_RETAIN_MEM_ADDR - CONFIG_SECURE_BOOT_ROM_FAST_WAKE_RESERVE_SIZE);
    return s_bootloader_retain_mem;
#else
    /* APP: 链接器放到 .bootloader_data_rtc_mem 段（同一物理地址） */
    static __attribute__((section(".bootloader_data_rtc_mem"))) rtc_retain_mem_t s_bootloader_retain_mem;
    return &s_bootloader_retain_mem;
#endif
}
```

**C6 LP RAM 布局**（`esp_system/ld/esp32c6/memory.ld.in:97-133`）：
```
0x50000000                          LP RAM 起始
  lp_ram_seg (RW)                   len = 0x4000 - RESERVE_RTC_MEM
0x50004000 - RESERVE_RTC_MEM        lp_reserved_seg
  ├── bootloader_data_rtc_mem       ← rtc_retain_mem_t（含 custom[]）
  ├── rtc_timer_data_in_rtc_mem
  └── ...
0x50004000                          LP RAM 末尾
```

**bootloader 链接脚本不含 RTC RAM**（`bootloader/subproject/main/ld/esp32c6/bootloader.memory.ld.in`）：
```
MEMORY {
  iram_seg (RWX) :        org = bootloader_iram_seg_start, ...        /* 0x4086xxxx */
  iram_loader_seg (RWX) : org = bootloader_iram_loader_seg_start, ... /* 0x4086xxxx */
  dram_seg (RW) :         org = bootloader_dram_seg_start, ...        /* 0x4086xxxx */
}
```
→ bootloader 的 `.bss` 清零**不会碰 RTC RAM** ✅

**官方验证测试**：`bootloader_support/test_apps/rtc_custom_section/main/test_main.c`
```c
rtc_retain_mem_t* mem = bootloader_common_get_rtc_retain_mem();
uint32_t* _rtc_vars = (uint32_t*) mem->custom;
/* 第一次启动写入，第二次启动读出 → 验证跨 esp_restart() 保持 */
```

### 14.4 ★ 绕过分区表加载镜像（已核对源码）

**`bootloader_utility_load_boot_image_from_pos()` 不存在！**
正确做法是**手工构造 `bootloader_state_t`**：

```c
bootloader_state_t bs = {0};
bs.factory.offset = IAP_APP_ADDR;      /* 0x10000 */
bs.factory.size   = IAP_APP_MAX_SIZE;  /* 0x1F0000 */
bs.app_count      = 0;
bootloader_utility_load_boot_image(&bs, FACTORY_INDEX);   /* 直接加载 */
```

**源码依据**（`components/bootloader_support/src/bootloader_utility.c`）：

```c
/* :578 主循环 —— 只从 bs 取分区 */
void bootloader_utility_load_boot_image(const bootloader_state_t *bs, int start_index)
{
    for (index = start_index; index >= FACTORY_INDEX; index--) {
        part = index_to_partition(bs, index);   /* ← 只读 bs */
        if (part.size == 0) continue;
        if (check_anti_rollback(&part) && try_load_partition(&part, &image_data)) {
            load_image(&image_data);            /* 真正加载 */
        }
    }
    ...
}

/* :275 index_to_partition —— 仅从 bs 结构体取 */
static esp_partition_pos_t index_to_partition(const bootloader_state_t *bs, int index)
{
    if (index == FACTORY_INDEX) return bs->factory;   /* ← 仅此而已 */
    if (index == TEST_APP_INDEX) return bs->test;
    if (index >= 0 && index < MAX_OTA_SLOTS && index < (int)bs->app_count)
        return bs->ota[index];
    esp_partition_pos_t invalid = { 0 };
    return invalid;
}

/* :472 try_load_partition —— 只用 offset/size */
static bool try_load_partition(const esp_partition_pos_t *partition, esp_image_metadata_t *data)
{
    if (partition->size == 0) return false;
    if (bootloader_load_image(partition, data) == ESP_OK) return true;  /* 只用 offset/size */
    return false;
}
```

**结论**：`bs` 只是 `{offset, size}` 的容器，**内容由我们填**。
因此**完全不读 flash 分区表**即可加载任意地址的镜像 ✅

**`bootloader_state_t` 定义**（`bootloader_support/private_include/bootloader_config.h:25`）：

```c
typedef struct {
    esp_partition_pos_t ota_info;
    esp_partition_pos_t factory;
    esp_partition_pos_t test;
    esp_partition_pos_t ota[MAX_OTA_SLOTS];   /* MAX_OTA_SLOTS = 16 */
    uint32_t app_count;
    uint32_t selected_subtype;
} bootloader_state_t;

#define FACTORY_INDEX (-1)
#define TEST_APP_INDEX (-2)
#define INVALID_INDEX (-99)
```

### 14.5 IDF 官方 bootloader 钩子

```c
// components/bootloader/subproject/main/bootloader_hooks.h:28,34
void __attribute__((weak)) bootloader_before_init(void);
void __attribute__((weak)) bootloader_after_init(void);
```

`bootloader/subproject/main/CMakeLists.txt:7`：
```cmake
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u bootloader_hooks_include")
```

---

## 15. 下一步

请审阅本方案，确认或修改：

1. **第 11 节的待确认问题**（尤其 `iap_boot_param_t` 字段清单）
2. **第 3 节的完整布局**
3. **第 14.3 / 14.4 节的自定义 bootloader 实现方式**
4. **第 10.3 节**：是否采用完整重构（A）

确认后我将按第 9.2 节的顺序实施。
