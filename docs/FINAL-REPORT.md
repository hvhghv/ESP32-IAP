# IAP 启动框架重构 —— 最终方案报告

> ⚠️ **历史文档** —— 本文描述的是 v5 设计阶段的方案，**部分已过时**。
> 当前实现（v6）请以 [`README.md`](../README.md) 为准。
> 主要差异: 用户程序烧录地址已从 `0x210000` 改为 `0x200000`（单一合并镜像）。

> 日期: 2026-09-22
> 状态: **待批准实施**
> 详细设计: [`docs/layout-redesign.md`](layout-redesign.md)（v4，15 章）

---

## 一、需求与目标

### 1.1 用户需求（演进过程）

| # | 需求 | 状态 |
|---|------|:----:|
| 1 | IAP 必须每次上电都运行（不能被 otadata 绕过） | ✅ |
| 2 | IAP 程序移到**固定区** | ✅ |
| 3 | 配置区 8KB | ✅ |
| 4 | `nvs` / `phy_init` 处理 | ✅ |
| 5 | bootloader 查**启动决策**决定启动谁 | ✅ |
| 6 | **bootloader 完全不读分区表** | ✅ |
| 7 | **IAP / APP 各用独立分区表**（隔离 + 安全） | ✅ |
| 8 | `iap_cfg` 硬编码，IAP/APP 约定同一地址 | ✅ |
| 9 | **启动决策 + 配置均放 RTC RAM**（bootloader 不复制参数） | ✅ **用户建议** |

### 1.2 核心目标

```
┌──────────────────────────────────────────────────────────────┐
│  ① 启动可控    每次上电必先跑 bootloader，由 RTC RAM 决定后续 │
│  ② 完全隔离    IAP 与 APP 各用独立分区表，互不可见           │
│  ③ 地址硬编码  固定区地址编译期常量，不依赖分区表            │
│  ④ 参数共享    IAP 写 RTC RAM，bootloader/APP 只读           │
└──────────────────────────────────────────────────────────────┘
```

---

## 二、最终布局

### 2.1 完整地址表

```
┌────────────────────────────────────────────────────────────────────┐
│  固定区 (0x0 ~ 0x1FFFFF, 2MB) —— 全硬编码                          │
│                                                                    │
│  偏移        大小      名称           在分区表?   谁定义            │
│  ────────────────────────────────────────────────────────────────  │
│  0x000000    32KB     bootloader      —          ROM               │
│  0x008000     8KB     iap_cfg         ❌ 不在     IAP_CFG_ADDR      │
│  0x00A000     4KB     (保留)          —          —                 │
│  0x00B000     4KB     分区表 A        —          CONFIG_PARTITION_ │
│  0x00C000    12KB     nvs_iap         ✅ 表 A     分区表 A          │
│  0x00F000     4KB     (保留)          —          —                 │
│  0x010000  ~1.94MB    IAP 程序        ✅ 表 A     IAP_APP_ADDR      │
│  0x200000  ────────────                                            │
├────────────────────────────────────────────────────────────────────┤
│  可变区 (0x200000 ~ 末尾) —— 分区表定义                            │
│                                                                    │
│  0x200000    64KB     nvs_app         ✅ 表 B     分区表 B          │
│  0x201000     4KB     分区表 B        —          CONFIG_PARTITION_ │
│  0x210000    剩余     user_app        ✅ 表 B     分区表 B          │
│  其后        可变     追加分区         ✅ 表 B     可自由追加        │
└────────────────────────────────────────────────────────────────────┘
```

> **注 1**：`iap` 虽在分区表 A 中，但其**偏移 `0x10000` 是硬编码的** ——
> bootloader 直接按 `IAP_APP_ADDR` 跳转，不查分区表；
> 分区表 A 中的 `iap` 条目仅供 IAP 代码 `esp_partition_find_first()` 取自身句柄。

> **注 2（v5）**：**`iap_mark` 标志区已取消** ——
> 启动决策改放 RTC RAM（`bootloader_common_get_rtc_retain_mem()->custom`）。
> `0x00A000` 保留 4KB 空位（不分配，避免未来地址变动）。

> **注 3（v5）**：**HTML 工具的「强制进入 IAP」功能一并移除**。
> 替代方案见 §3.3。

### 2.2 两个分区表的内容

**分区表 A（IAP，`0x00B000`）**：
```csv
# Name,     Type,    SubType, Offset,   Size,     Flags
nvs_iap,    data,    nvs,     0x00C000, 0x3000,
iap,        app,     factory, 0x010000, 0x1F0000,
```

| 项 | 偏移 | 大小 | 谁用 | 必须？ |
|---|------|------|------|:------:|
| `nvs_iap` | `0xC000` | **12KB** | `nvs_flash_init_partition("nvs_iap")` / WiFi 驱动 | ✅ **必须** |
| `iap` | `0x10000` | `0x1F0000` | IAP 代码查 `"iap"` 分区<br>（`esp_partition_find_first(APP, FACTORY, "iap")`） | ✅ **必须** |

> **`0x1F0000` 计算**：`0x200000`（可变区起点）− `0x10000`（IAP 起点）= `0x1F0000`（约 1.94MB）

> **`nvs_iap` 为什么是 12KB 而不是 4KB**：IDF 的 `gen_esp32part.py` 强制要求
> 可读写 NVS 分区至少 `0x3000`（12KB）：
> ```
> NVS_RW_MIN_PARTITION_SIZE = 0x3000     # gen_esp32part.py:49
> if self.size < NVS_RW_MIN_PARTITION_SIZE and self.readonly is False:
>     raise ValidationError(...)          # gen_esp32part.py:599
> ```
> 标记 `readonly` 不可行 —— WiFi 驱动必须写 NVS（`esp_adapter.c:340`），
> 因此 `nvs_iap` 无法缩小到 4KB。

> **`iap` 为什么必须 64KB 对齐**：`app` 类型分区的 Offset 与 Size 必须按
> `0x10000`（64KB）对齐（`gen_esp32part.py:107,119-123,570-573`）。
> `0x10000` 与 `0x1F0000` 均为 64KB 倍数 ✅

> **为什么 `iap` 必须在分区表 A**：IAP 代码调用
> `esp_partition_find_first(APP, FACTORY, "iap")` 获取自身分区句柄
> （`iap_image.c:340`）。若不在表中，`s_iap_part == NULL`，
> `iap_image_boot_iap()` 会返回 `ESP_ERR_INVALID_STATE`。

> **为什么不需要 `phy_init`**：实测 `# CONFIG_ESP_PHY_INIT_DATA_IN_PARTITION is not set`，
> PHY 数据编译进固件、校准数据存 NVS（`CONFIG_ESP_PHY_CALIBRATION_AND_DATA_STORAGE=y`），
> **不使用 `phy_init` 分区**。

**分区表 B（APP，`0x200000`）**：
```csv
# Name,     Type,    SubType, Offset,   Size,     Flags
nvs_app,    data,    nvs,     0x201000, 0x3000,
user_app,   app,     ota_0,   0x210000, 0x1F0000,
# 追加分区示例 (需缩小 user_app 的 Size):
# storage,  data,    spiffs,  0x3F0000, 0x10000,
```

| 项 | 偏移 | 大小 | 谁用 | 必须？ |
|---|------|------|------|:------:|
| `nvs_app` | `0x201000` | **12KB** | APP 的 `nvs_flash_init_partition("nvs_app")` / WiFi 驱动 | ✅ **必须** |
| `user_app` | `0x210000` | `0x1F0000` | APP 自身运行分区 | ✅ **必须** |

> **`nvs_app` 为什么从 `0x201000` 开始**：`gen_esp32part.py` 会校验
> **分区不与分区表自身重叠**，且分区表**必须在所有分区之前**。
> 因此分区表 B 占 `0x200000`，`nvs_app` 从 `0x201000` 开始（12KB）。
> `0x204000`~`0x20FFFF` 为保留空隙，`user_app` 仍落在 `0x210000`（64KB 对齐）。

> **`nvs_app` 为什么是 12KB**：IDF 要求可读写 NVS 至少 `0x3000`（12KB）。
> 分区表 B 自身占 `0x200000` 一个扇区（必须在所有分区之前），
> 因此 `nvs_app` 从 `0x201000` 开始，12KB → `0x201000`~`0x203FFF`。
> `0x204000`~`0x20FFFF` 为保留空隙，使 `user_app` 仍落在 `0x210000`（64KB 对齐）。

> **`0x1F0000` 计算**（4MB flash）：`0x400000`（4MB）− `0x210000` = `0x1F0000`（约 1.99MB）

> **两个表都不含** `iap_cfg` —— 它是**硬编码共享资源**，
> 由 IAP 与 APP 约定同一地址（`0x8000`），不通过分区表访问。
> （原 `iap_mark` 标志区已取消，启动决策改放 RTC RAM）

### 2.2.1 两表对比

| 分区 | 表 A (IAP) | 表 B (APP) | 说明 |
|------|:----------:|:----------:|------|
| `nvs_iap` | ✅ 12KB | ❌ | IAP 专用 nvs |
| `iap` (factory) | ✅ `0x10000` | ❌ | IAP 自身 |
| `nvs_app` | ❌ | ✅ 12KB | APP 专用 nvs |
| `user_app` (ota_0) | ❌ | ✅ `0x210000` | APP 自身 |
| `iap_cfg` | ❌ | ❌ | **硬编码 `0x8000`** |
| `otadata` | ❌ | ❌ | **已删除** |
| `phy_init` | ❌ | ❌ | **已删除** |
| ~~`iap_mark`~~ | ❌ | ❌ | **已取消**（改用 RTC RAM） |

### 2.2.2 校验结果

两个分区表均已通过 IDF 官方工具 `gen_esp32part.py` 校验：

```
$ python gen_esp32part.py --verify partitions_iap.csv
Parsing CSV input...
Verifying table...          ← 无错误

$ python gen_esp32part.py --verify partitions_app.csv
Parsing CSV input...
Verifying table...          ← 无错误
```

解析后的二进制内容（各 3072 字节）：

| 表 | Name | Type | SubType | Offset | Size | End |
|---|------|------|---------|--------|------|-----|
| A | `nvs_iap` | `0x01` | `0x02` | `0x00C000` | `0x003000` | `0x00F000` |
| A | `iap` | `0x00` | `0x00` | `0x010000` | `0x1F0000` | `0x200000` |
| B | `nvs_app` | `0x01` | `0x02` | `0x201000` | `0x003000` | `0x204000` |
| B | `user_app` | `0x00` | `0x10` | `0x210000` | `0x1F0000` | `0x400000` |

### 2.2.3 完整 flash 布局（4MB）

```
0x000000  32KB     bootloader      (含配置区, 烧录文件 1)
0x008000   8KB     iap_cfg         (硬编码, 不在任何分区表)
0x00A000   4KB     (保留, 原 iap_mark)
0x00B000   4KB     分区表 A        (IAP 用)
0x00C000  12KB     nvs_iap         (表 A)
0x00F000   4KB     (保留)
0x010000  ~1.94MB  iap             (表 A, IAP 程序)
0x200000   4KB     分区表 B        (APP 用)
0x201000  12KB     nvs_app         (表 B)
0x210000  ~1.99MB  user_app        (表 B, 用户态)
0x400000           flash 末尾
```

### 2.3 与当前布局对比

| 项 | 当前 | 新方案 | 变化 |
|----|------|--------|------|
| bootloader | `0x0` 32KB | `0x0` 32KB | — |
| 分区表 | `0x8000`（1 个） | **`0xB000` + `0x200000`（2 个）** | ⚠️ 双表 |
| nvs | `0x9000` 24KB | **`0xC000` 12KB + `0x201000` 12KB** | ⚠️ 双 nvs |
| phy_init | `0xF000` 4KB | **删除** | ⚠️ 删除 |
| iap_cfg | `0x10000` 8KB | **`0x8000`** 8KB | ⚠️ 移动 |
| iap_mark | `0x14000` 4KB | **取消**（改用 RTC RAM） | ⚠️ 删除 |
| otadata | `0x12000` 8KB | **删除** | ⚠️ 删除 |
| IAP 程序 | `0x20000` | **`0x10000`** | ⚠️ 移到固定区 |
| user_app | `0x210000` | **`0x210000`** | — 不变 |
| 启动决策 | flash `iap_mark` | **RTC RAM** | ⚠️ 改介质 |

---

## 三、启动流程

### 3.1 完整流程（**已按用户建议优化：启动决策也放 RTC RAM**）

```
┌─────────────────────────────────────────────────────────────────┐
│ ① 上电 / 复位                                                  │
│      ROM → 加载 bootloader (0x0)                               │
├─────────────────────────────────────────────────────────────────┤
│ ② bootloader（自定义，不读分区表，**不读 flash 标志区**）        │
│      读 RTC RAM (bootloader_common_get_rtc_retain_mem)         │
│        ├─ boot_target=0 → IAP  @ 0x10000  (硬编码)            │
│        └─ boot_target=1 → APP  @ param.user_addr (RTC RAM)    │
│      ★ 不复制任何参数 —— 参数已在 RTC RAM 里                    │
│      跳转 entry()（不传参）                                     │
├─────────────────────────────────────────────────────────────────┤
│ ③ IAP 运行（读表 A）                                            │
│      读配置区 (0x8000, 硬编码)                                  │
│      用户点"启动用户程序"                                        │
│        → ★ IAP 读 iap_cfg → 写入 RTC RAM                       │
│           (boot_target=1, user_addr=0x210000, 配置副本)        │
│        → esp_restart()                                          │
├─────────────────────────────────────────────────────────────────┤
│ ④ bootloader 再次运行 → 读 RTC RAM → 启动 APP（见 ②）          │
├─────────────────────────────────────────────────────────────────┤
│ ⑤ APP 运行（读表 B）                                            │
│      读 RTC RAM → 得到配置（IAP 写入的）                        │
│      若要回 IAP: ★ 写 RTC RAM boot_target=0 → esp_restart()    │
└─────────────────────────────────────────────────────────────────┘
```

**与之前方案的关键差异**：

| 项 | 之前 | **优化后** |
|---|------|-----------|
| 启动决策存放 | flash 标志区 `0xA000` | **RTC RAM** |
| 谁写启动决策 | IAP / APP 写 flash | **IAP / APP 写 RTC RAM** |
| bootloader 读什么 | flash 标志区 | **RTC RAM** |
| bootloader 复制参数？ | ✅ 需要（读 iap_cfg → 写 RTC RAM） | ❌ **不需要**（IAP 已写好） |
| flash 写次数 | 每次切换写 flash | **0 次**（不磨损 flash） |

### 3.1.0 为什么这个优化更好

**① bootloader 极简**：只读 RTC RAM 一个结构体，不碰 flash。

**② 无 flash 磨损**：切换启动目标不再写 flash（RTC RAM 写入无寿命限制）。

**③ 消除原子性问题**：之前"标志区 + 配置"若同扇区会有擦写冲突，现在都在 RAM 里。

**④ 职责更清晰**：
```
IAP  = 唯一写 RTC RAM 的一方（准备启动参数）
bootloader = 只读 RTC RAM（决定启动谁）
APP = 只读 RTC RAM（拿配置）
```

**⑤ 仍满足"每次上电必跑 IAP"**：见 §3.1.3 的冷启动处理。

### 3.1.1 RTC RAM 结构（**用 IDF 官方机制**）

**重要修正**：不再自定义 `0x50000000` 处结构，而是用 IDF 官方保留区。

```c
/* IDF 官方结构（esp_image_format.h:58） */
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

> ⚠️ **修正**：之前方案写 `IAP_PARAM_ADDR = 0x50000000` **是错的** ——
> 那会与 APP 的 `lp_ram_seg` 冲突（APP 启动后可能用该区域）。
> 必须用 `bootloader_common_get_rtc_retain_mem()->custom`，
> 它在 **LP RAM 末尾的保留区**，两侧地址一致。

**需要的 Kconfig**：
```ini
CONFIG_BOOTLOADER_RESERVE_RTC_MEM=y
CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC=y
CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE=128   # >= sizeof(iap_boot_param_t)=100
```

**两侧地址一致性依据**（`bootloader_common_loader.c:268`）：
```c
rtc_retain_mem_t* bootloader_common_get_rtc_retain_mem(void)
{
#ifdef BOOTLOADER_BUILD
    #define RTC_RETAIN_MEM_ADDR (SOC_RTC_DRAM_LOW)   /* bootloader: 硬编码地址 */
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

> **官方验证**：IDF 自带测试 `bootloader_support/test_apps/rtc_custom_section/`
> 专门验证 `custom[]` 跨 `esp_restart()` 保持 —— 正是我们的用法。

### 3.1.2 三种状态的保持能力

| 区域 | 介质 | 复位后 | 断电后 | 用途 |
|------|------|:------:|:------:|------|
| `.bss` / `.data` | SRAM | ❌ 清零/重载 | ❌ | 普通变量 |
| RTC RAM `custom[]` | LP RAM | ✅ | ❌ | **启动决策 + 配置** |
| ~~flash 标志区~~ | ~~flash~~ | — | — | **已取消** |

> ⚠️ **`.bss` 会被清零**：bootloader 启动时有 `_bss_start`/`_bss_end` 清零逻辑。
> 但 **RTC RAM 不在 bootloader 链接脚本中**（`bootloader.memory.ld.in` 只有
> `iram_seg` / `iram_loader_seg` / `dram_seg` 三个内部 SRAM 段），
> 因此 bootloader 的 `.bss` 清零**不会影响 RTC RAM** ✅

### 3.1.3 冷启动（断电后）如何处理

**问题**：RTC RAM 断电丢失，冷启动时 `custom[]` 无效。

**处理**：bootloader 检查 `custom[]` 的 magic/CRC：
```c
rtc_retain_mem_t *mem = bootloader_common_get_rtc_retain_mem();
iap_boot_param_t *p = (iap_boot_param_t *)mem->custom;

if (p->magic == IAP_PARAM_MAGIC && iap_crc_ok(p)) {
    /* 有效 → 按 p->boot_target 启动 */
} else {
    /* 冷启动 / 数据无效 → 默认启动 IAP ✅ 满足"每次上电必跑 IAP" */
}
```

| 场景 | RTC RAM | 行为 |
|------|:-------:|------|
| 冷启动（断电后） | 无效 | **启动 IAP** ✅ |
| 复位（IAP 要求启动 APP） | 有效 | 启动 APP |
| 复位（APP 要求回 IAP） | 有效 | 启动 IAP |
| 数据损坏（CRC 错） | 无效 | **启动 IAP**（安全兜底） |

**结论**：**"每次上电必跑 IAP"的要求仍然满足** —— 冷启动时 RTC RAM 无效，默认走 IAP。

### 3.1.4 防砖机制（已移除）

> **v8 已移除防砖计数机制。**
>
> 早期设计曾用 `rtc_retain_mem_t.reboot_counter` 或配置区的
> `boot_fail_count` 做「连续启动失败则停止重试」的保护。但该机制
> 存在误报问题：计数器只增不减（用户程序无法访问配置区，无法清零），
> 设备正常重启若干次后即被误判为「启动失败」，导致界面显示
> “用户程序 CRC 校验失败”并停止启动。
>
> 现在 IAP **不再对用户程序做任何启动重试限制**：
>   - 镜像合法性完全由 bootloader 启动时自校验
>     (magic / 段表 / SHA256 / chip_id)
>   - 校验失败时 bootloader 自动回落到 IAP (factory)，不会变砖
>   - 启动决策完全由配置区与 RTC RAM 控制，无隐式计数
>
> 若需“启动失败后停止重试”的行为，应由用户程序自行实现。

### 3.2 为什么不能"IAP 直跳 APP"

| 原因 | 说明 |
|------|------|
| FreeRTOS 状态 | IAP 的任务/定时器/队列仍在，APP 初始化冲突 |
| lwIP 协议栈 | `esp_netif` 任务仍在跑，APP 重新初始化失败 |
| 堆状态 | IAP 的堆碎片与 APP 不一致 |
| 中断状态 | 无法安全交接 |

**结论**：每次切换**必须经复位**（`esp_restart()`），这是 IDF 唯一可靠的方式。

### 3.3 「强制进入 IAP」能力如何保留（**v5 重新设计**）

**问题**：取消 flash 标志区后，原「HTML 工具写魔术标记 → 强制进 IAP」的入口消失。
但**这个能力本身仍然必要**（救援场景：用户程序死循环 / 崩溃，需要回到 IAP）。

**新方案：GPIO0 电平检测放在 IAP 内，bootloader 不管**

```
bootloader: 只读 RTC RAM 决定启动谁（不做任何 GPIO 检测）
IAP:        启动后在等待窗口内检测 GPIO0 电平 → 有效则进入下载模式
APP:        正常运行
```

**为什么放 IAP 而不放 bootloader**：

| 理由 | 说明 |
|------|------|
| **职责单一** | bootloader 只管"启动谁"，不掺入人机交互 |
| **已有现成实现** | IAP 的 `main/iap_wait_trigger.c` 已实现 GPIO 触发 |
| **可配置** | 引脚号/电平/使能位都在 `iap_cfg` 里，用户可改 |
| **无需改 bootloader** | bootloader 代码更简单，风险更低 |
| **有等待窗口** | IAP 启动后有 `wait_seconds` 窗口，天然适合检测 |

**IAP 现有 GPIO 触发机制**（`main/iap_wait_trigger.c`，**已存在，无需新写**）：

```c
/* 引脚配置（:85） */
static esp_err_t trig_gpio_setup(uint8_t gpio_num, uint8_t level)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << gpio_num),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = (level == IAP_CFG_GPIO_TRIG_ACTIVE_LOW)
                        ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = (level == IAP_CFG_GPIO_TRIG_ACTIVE_HIGH)
                        ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    return gpio_config(&io);
}

/* 电平判定（:116） */
static bool trig_gpio_active(void)
{
    int level = gpio_get_level((gpio_num_t)s_trig.gpio_num);
    return (s_trig.gpio_level == IAP_CFG_GPIO_TRIG_ACTIVE_HIGH)
           ? (level != 0) : (level == 0);
}

/* 等待窗口内轮询（:246） */
while (elapsed < wait_ms && !s_trig.stop && s_trig.fired_mask == 0) {
    if ((s_trig.enabled & TRIG_SRC_GPIO) && trig_gpio_active()) {
        ESP_LOGW(TAG, "GPIO%u 检测到有效电平，进入 IAP 下载模式", s_trig.gpio_num);
        s_trig.reason = IAP_BOOT_REASON_TRIG_GPIO;
        s_trig.fired_mask |= TRIG_SRC_GPIO;
        break;
    }
    ...
}
```

**配置项**（`iap_cfg` 已有字段）：

| 字段 | 说明 | 默认值（v5 建议） |
|------|------|------------------|
| `trig_gpio` | 触发引脚号（`0xFF` = 禁用） | **`0`**（GPIO0 = BOOT 键） |
| `trig_gpio_level` | 有效电平（0=低，1=高） | `0`（低电平有效） |
| `IAP_CFG_FLAG_WAIT_GPIO_TRIG` | 使能位 | 默认**使能** |
| `wait_seconds` | 等待窗口 | 3 秒 |

**建议默认值调整**（v5）：

```c
/* iap_common.h */
#define IAP_CFG_DEFAULT_TRIG_GPIO     0     /* GPIO0 = BOOT 按键（原 0xFF） */
#define IAP_CFG_DEFAULT_TRIG_LEVEL    IAP_CFG_GPIO_TRIG_ACTIVE_LOW
/* 默认使能 GPIO 触发 */
#define IAP_CFG_DEFAULT_FLAGS         (IAP_CFG_FLAG_WAIT_GPIO_TRIG)
```

> **GPIO0 是 ESP32-C6 的 BOOT 按键**，板载已有，无需额外硬件。
> 低电平有效 + 内部上拉 → 按下即触发。
> （C6 共 31 个 GPIO，`SOC_GPIO_PIN_COUNT=31`，GPIO0 有效）

**检测语义：纯电平检测（不做长按）**

| 场景 | 行为 |
|------|------|
| 上电时 GPIO0 已为低电平 | 等待窗口内检测到 → 进 IAP ✅ |
| 上电后 3 秒内按下 GPIO0 | 同上 ✅ |
| 超过等待窗口才按 | 不触发（IAP 已启动用户程序） |

> **实现即"电平检测"**：`trig_gpio_active()` 只读一次引脚电平，
> **不做计时/长按判定**。逻辑简单，满足救援需求。

**救援手段汇总**（按推荐度）：

| 手段 | 说明 | 需硬件 | 需设备能跑 |
|------|------|:------:|:----------:|
| **A. GPIO0 电平** | IAP 内检测，上电时按住 BOOT 键 | ❌ 板载 | ❌ |
| **B. UART 命令** | 等待窗口内发 `iap` 字符串 | ❌ | ❌ |
| **C. 用户程序主动请求** | 写 RTC RAM `boot_target=0` → 复位 | ❌ | ✅ |
| **D. 断电重上电** | 冷启动 → RTC RAM 无效 → 进 IAP | ❌ | ❌ |

> **★ 方案 A + B 都在 IAP 的等待窗口内工作**，且**不需要设备运行用户程序**，
> 是真正的"救援"入口。**bootloader 完全不参与**。

> **因此 HTML 工具不再需要「强制进入 IAP」按钮** ——
> 该能力由 GPIO0 电平 / UART 命令 / 断电重上电覆盖。

---

## 四、关键设计决策

### 4.1 决策汇总

| # | 决策 | 理由 | 依据 |
|---|------|------|------|
| 1 | **bootloader 不读分区表** | 全硬编码，极简 | `bootloader_utility.c:150` 用编译期常量 |
| 2 | **双分区表** | IAP/APP 隔离 | 两程序独立编译，`ESP_PARTITION_TABLE_OFFSET` 可不同 |
| 3 | **`iap_cfg` 不在分区表** | 共享资源，硬编码约定 | 避免破坏隔离性 |
| 4 | **各自 nvs** | WiFi 驱动强制需要 | `nvs_api.cpp:204` 靠分区名查找 |
| 5 | **删除 `phy_init`** | 无代码引用 | 实测 `PART_SUBTYPE_DATA_PHY` 零引用 |
| 6 | **删除 `otadata`** | 由 RTC RAM 取代 | 避免 bootloader 绕过 IAP |
| 7 | **启动决策 + 参数均放 RTC RAM** | 用户建议：bootloader 不复制参数 | 见 §3.1.0 |
| 8 | **用 IDF 官方 `custom[]` 区** | 地址自动一致 + 自带 CRC | `esp_image_format.h:58` |
| 9 | **禁用 RTC FAST 作堆** | 防止 malloc 覆盖 | Kconfig 官方选项 |
| 10 | **GPIO0 电平检测放 IAP，bootloader 不管** | 职责单一；IAP 已有实现 | 见 §3.3 |
| 11 | **删除 flash 标志区 + HTML 强制进 IAP** | 用户要求；能力由 GPIO0/断电覆盖 | 见 §3.3 |
| 12 | **纯电平检测，不做长按** | 逻辑简单，已满足救援需求 | 见 §3.3 |
| 13 | **`iap_boot_param_t` 只传启动参数** | 配置字段保留在 `iap_cfg`；预留 `reserved1[64]` | 见 §5.5 |

### 4.2 访问方式分工

| 组件 | 读分区表 | 读配置区 | 读/写 RTC RAM |
|------|:--------:|:--------:|:-------------:|
| **bootloader** | ❌ | ❌ | ✅ **只读** |
| **IAP** | ✅ 表 A | ✅ | ✅ **写** |
| **APP** | ✅ 表 B | ❌ | ✅ **读** |

> **关键**：只有 IAP 写 RTC RAM（准备启动参数）；
> bootloader 与 APP 都**只读**。职责单一，无竞争。

---

## 五、RTC RAM 参数传递

### 5.1 官方文档依据

来源：`docs/zh_CN/api-guides/memory-types.rst`

| 官方原文 | 含义 |
|---------|------|
| "对于 **ESP32-C6/H2**，RTC 存储器已被**重新命名为 LP（低功耗）存储器**" | C6 上叫 LP RAM |
| "`RTC_NOINIT_ATTR`...放入此类型存储器的值**从深度睡眠模式中醒来后会保持值不变**" | 跨复位保留 |
| ⚠️ "**除非禁用 `CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP`，否则剩余的 RTC FAST memory 会被添加到堆中**" | **堆冲突风险** |

### 5.2 实测数据

```
ESP32-C6 RTC RAM (0x50000000 ~ 0x50004000, 16KB)
  lp_ram_seg       0x50000000   16360 字节   ← 可用
  lp_reserved_seg  0x50003fe8      24 字节   ← IDF esp_clk 占用

当前 IAP 静态占用: 0 字节
```

### 5.3 关键改动

```ini
# sdkconfig.defaults
CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP=n
```

> **bootloader 是子项目，继承主项目 SDKCONFIG**（`subproject/CMakeLists.txt:3`），
> 无需单独配置。

### 5.4 堆空间影响（实测）

| 项 | 当前 | 禁用后 | 评估 |
|----|------|--------|:----:|
| 空闲堆 | 141,520 字节 (138KB) | ~125,000 字节 (122KB) | 🟢 充足 |
| 最小空闲堆 | 131,056 字节 (128KB) | ~115,000 字节 (112KB) | 🟢 充足 |

### 5.5 共享结构（**只传启动参数**）

> **v5 决定**：`iap_boot_param_t` **只传启动参数**，
> 配置字段（WiFi / I2C / 触发源等）**先保留在 `iap_cfg`**，暂不复制到 RTC RAM。

> ⚠️ **目标芯片限制（实测）**：本方案依赖 `SOC_RTC_FAST_MEM_SUPPORTED`。
>
> | 目标 | RTC FAST RAM | 支持 |
> |------|:------------:|:----:|
> | esp32 / s2 / s3 | ✅ | ✅ |
> | **esp32c2** | ❌ **无** | ❌ **不支持** |
> | c3 / c5 / c6 / h2 | ✅ | ✅ |
>
> `esp32c2` 无 RTC FAST RAM → `CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC` 不可用
> → `rtc_retain_mem_t.custom[]` 不存在。`iap_boot_param.h` 已加 `#error` 明确拦截。

```c
/* iap_boot_param.h —— IAP / bootloader / APP 三方共享 */
#define IAP_PARAM_MAGIC   0x49415042U  /* "IAPB" */
#define IAP_PARAM_VERSION 0x0001U      /* 结构版本 */

typedef struct {
    /* --- 头部 --- */
    uint32_t magic;          /* IAP_PARAM_MAGIC */
    uint32_t version;        /* IAP_PARAM_VERSION */
    uint32_t crc32;          /* 以下数据的 CRC32 */

    /* --- 启动决策（原 flash 标志区的职责） --- */
    uint8_t  boot_target;    /* 0 = IAP, 1 = APP */
    uint8_t  reserved0[3];
    uint32_t user_addr;      /* APP 加载地址（0x210000） */
    uint32_t user_size;      /* APP 大小 */
    uint32_t user_version;   /* APP 版本号（可选） */

    /* --- 布局信息（供 APP 定位共享资源） --- */
    uint32_t cfg_addr;       /* iap_cfg 地址（0x8000） */
    uint32_t cfg_seq;        /* 配置槽序号（IAP 写入时的快照） */

    /* --- 预留（配置子集以后再加） --- */
    uint8_t  reserved1[64];
} iap_boot_param_t;
```

**大小（实测验证）**：`sizeof(iap_boot_param_t) = 100` 字节
（packed 与自然对齐结果一致，无填充；`offsetof(crc32) = 8`）
→ `CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE = 128`（4 字节对齐，留 28 字节余量）

**字段说明**：

| 字段 | 用途 | 写入者 | 读取者 |
|------|------|--------|--------|
| `magic` / `version` / `crc32` | 有效性校验 | IAP | bootloader / APP |
| `boot_target` | 决定启动谁 | IAP / APP | **bootloader** |
| `user_addr` / `user_size` | APP 加载地址与大小 | IAP | **bootloader** |
| `user_version` | APP 版本（显示用） | IAP | APP |
| `cfg_addr` / `cfg_seq` | 供 APP 定位 `iap_cfg` | IAP | APP |
| `reserved1[64]` | **配置子集预留** | — | — |

> **★ 为什么预留 64 字节**：以后若要传配置子集，可直接用 `reserved1`，
> **无需改结构大小**（避免 `CUSTOM_RESERVE_RTC_SIZE` 变动导致两侧不一致）。

**APP 如何拿配置**（当前方案）：

```c
/* APP 侧：通过 iap_param 拿到 iap_cfg 地址，再自行读 flash */
iap_boot_param_t *p = iap_param_get();
if (p->magic == IAP_PARAM_MAGIC && iap_param_crc_ok(p)) {
    /* 方式 A: 直接用 p->cfg_addr 读 flash（APP 自己解析 iap_cfg） */
    uint32_t cfg_addr = p->cfg_addr;   /* 0x8000 */

    /* 方式 B: 等以后 reserved1 里加了配置子集，直接用 */
}
```

> **后续扩展**：若 APP 频繁需要某些配置项，再往 `reserved1` 里加字段，
> 由 IAP 在写 RTC RAM 时一并填入。**当前先不做**。

**访问辅助函数**（bootloader 与 APP 通用）：

```c
/* iap_boot_param.h */
#include "esp_image_format.h"          /* rtc_retain_mem_t */
#include "bootloader_common.h"         /* bootloader_common_get_rtc_retain_mem */

static inline iap_boot_param_t *iap_param_get(void)
{
    rtc_retain_mem_t *mem = bootloader_common_get_rtc_retain_mem();
    return (iap_boot_param_t *)mem->custom;
}

static inline bool iap_param_crc_ok(const iap_boot_param_t *p)
{
    return esp_rom_crc32_le(UINT32_MAX, (const uint8_t *)p,
                            offsetof(iap_boot_param_t, crc32)) == p->crc32;
}

static inline void iap_param_update_crc(iap_boot_param_t *p)
{
    p->crc32 = esp_rom_crc32_le(UINT32_MAX, (const uint8_t *)p,
                                offsetof(iap_boot_param_t, crc32));
}
```

**Kconfig 配置**：
```ini
CONFIG_BOOTLOADER_RESERVE_RTC_MEM=y
CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC=y
CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE=128   # >= sizeof(iap_boot_param_t)=100，4 字节对齐
```

> **为什么用 `custom[]` 而不是自定义 `0x50000000` 结构**：
> 1. `0x50000000` 是 APP 的 `lp_ram_seg` 起始，**会被 APP 使用**（冲突）
> 2. `custom[]` 位于 LP RAM **末尾保留区**，IDF 保证不被占用
> 3. bootloader 与 APP 通过同一 API 访问，**地址自动一致**
> 4. 自带 CRC 维护（`update_rtc_retain_mem_crc`）

---

## 六、启动决策（原"标志区"章节 —— **已改为 RTC RAM**）

### 6.1 原 flash 标志区方案（**已废弃**）

> ❌ **原方案**：在 flash `0xA000` 建 `iap_mark` 标志区，存 `boot_target` 等。
>
> **废弃原因**（用户建议）：
> 1. 每次切换要写 flash（磨损）
> 2. bootloader 需读 flash + 写 RTC RAM（两步）
> 3. 与配置区若同扇区有原子性问题
>
> **新方案**：全部放 RTC RAM，见 §3.1.1 / §5.5。

### 6.2 新方案：RTC RAM 承载启动决策

| 字段 | 位置 | 写入者 | 读取者 |
|------|------|--------|--------|
| `magic` / `version` / `crc32` | `custom[]` | IAP | bootloader / APP |
| `boot_target` | `custom[]` | IAP / APP | **bootloader** |
| `user_addr` / `user_size` | `custom[]` | IAP | **bootloader** |
| `user_version` | `custom[]` | IAP | APP |
| `cfg_addr` / `cfg_seq` | `custom[]` | IAP | APP |
| `reserved1[64]` | `custom[]` | — | — |
| `reboot_counter` | `rtc_retain_mem_t` 本体 | bootloader | bootloader |

> **注意**：`iap_boot_param_t` **只传启动参数**（v5 决定），
> 配置字段（WiFi / I2C / 触发源等）**仍在 `iap_cfg` 里**，
> APP 通过 `p->cfg_addr` 自行读 flash。见 §5.5。

### 6.3 防砖机制（已移除）

> **v8 已移除。** 详见 §3.1.4。
>
> 现在 bootloader 按 RTC RAM 请求直接加载用户程序，不做任何重试计数；
> 镜像校验与失败回落完全由 IDF 的 `bootloader_utility_load_boot_image()`
> 负责。

---

## 七、烧录方案

### 7.1 两个文件

| 文件 | 内容 | 地址 | 大小 |
|------|------|------|------|
| `iap_side.bin` | **IAP 烧录文件** = bootloader + 配置区 + 表 A + IAP | `0x0` | 2MB（固定区整片） |
| `user.bin` | user_app | `0x210000` | 可变 |

> **v5 简化**：分区表 A 已并入 `iap_side.bin`（单一连续文件 `0x0 ~ 0x1FFFFF`），
> 不再单独烧录。分区表 B 由用户程序工程随 `user.bin` 一起烧录
> （用户程序工程的 `PARTITION_TABLE_OFFSET=0x200000`）。

### 7.2 烧录命令

```bash
esptool.py --chip esp32c6 -p COM18 write_flash \
    0x0      iap_side.bin \
    0x210000 user.bin
```

### 7.3 升级隔离

| 升级对象 | 重烧文件 | 影响 |
|---------|---------|------|
| IAP | `iap_side.bin` | APP 侧不受影响 |
| APP | `user.bin`（含表 B） | IAP 侧不受影响 |

---

## 八、改动清单

### 8.1 新增文件（4 个）

| 文件 | 说明 |
|------|------|
| `bootloader_components/main/CMakeLists.txt` | 自定义 bootloader 组件 |
| `bootloader_components/main/bootloader_start.c` | 启动决策（**IDF 官方支持**，见 8.2） |
| `main/iap_boot_param.h` | RTC RAM 参数结构（**只传启动参数**，100 字节，见 §5.5） |
| `partitions_app.csv` | 分区表 B |

> ⚠️ **目录名是 `bootloader_components/main`**（不是 `components/custom_bootloader`）——
> 这是 IDF 约定的特殊目录，见 8.2。
>
> ⚠️ **`iap_flag.c/.h` 已取消** —— 启动决策改用 RTC RAM，
> 不再需要 flash 标志区读写 API。

### 8.2 自定义 bootloader 的 IDF 官方机制（重要修正）

**不需要修改 IDF 源码**。IDF 已提供两种官方扩展方式：

#### 方式 A：覆盖整个 bootloader（推荐，`bootloader_multiboot` 示例即此法）

在项目根目录建 `bootloader_components/main/`：
```
bootloader_components/
└── main/
    ├── CMakeLists.txt
    └── bootloader_start.c      ← 定义 call_start_cpu0()
```

`CMakeLists.txt`：
```cmake
idf_component_register(SRCS "bootloader_start.c"
                    REQUIRES bootloader bootloader_support)
idf_build_get_property(scripts BOOTLOADER_LINKER_SCRIPT)
target_linker_script(${COMPONENT_LIB} INTERFACE "${scripts}")
```

**当 bootloader 组件命名为 `main` 时，会覆盖整个二级 bootloader。**

我们的 `bootloader_start.c` 骨架（**已依据 IDF 源码验证**）：

```c
void __attribute__((noreturn)) call_start_cpu0(void)
{
    if (bootloader_init() != ESP_OK) bootloader_reset();

    /* ★ 不读分区表，也不读 flash 标志区 —— 只读 RTC RAM */
    rtc_retain_mem_t *mem = bootloader_common_get_rtc_retain_mem();
    iap_boot_param_t *p = (iap_boot_param_t *)mem->custom;

    bootloader_state_t bs = {0};

    if (p->magic == IAP_PARAM_MAGIC && iap_param_crc_ok(p) && p->boot_target == 1) {
        /* --- 启动 APP（参数已在 RTC RAM，无需复制） --- */
        /* 防砖：reboot_counter++ */
        bootloader_common_update_rtc_retain_mem(NULL, true);
        if (bootloader_common_get_rtc_retain_mem_reboot_counter() > 5) {
            p->boot_target = 0;                       /* 强制回 IAP */
            bootloader_common_reset_rtc_retain_mem();
        } else {
            bs.factory.offset = p->user_addr;         /* 0x210000 */
            bs.factory.size   = p->user_size;
            bs.app_count      = 0;
            bootloader_utility_load_boot_image(&bs, FACTORY_INDEX);
        }
    }

    /* --- 启动 IAP（冷启动 / 数据无效 / 强制回 IAP） --- */
    bs.factory.offset = IAP_APP_ADDR;                 /* 0x10000 */
    bs.factory.size   = IAP_APP_MAX_SIZE;             /* 0x1F0000 */
    bs.app_count      = 0;
    bootloader_utility_load_boot_image(&bs, FACTORY_INDEX);
    /* 不会返回 */
}
```

> **注意 1**：bootloader 里**没有 `memcpy` 复制参数** —— 参数由 IAP 提前写好，
> bootloader 只读。这正是用户建议的优化点。
>
> **注意 2**：bootloader **不做任何 GPIO 检测** ——
> GPIO0 电平检测放在 **IAP 内**（`iap_wait_trigger.c`，已有实现），
> 见 §3.3。bootloader 职责保持单一：只读 RTC RAM 决定启动谁。

#### 为什么"手工构造 `bs`"可行（源码依据）

`bootloader_utility_load_boot_image()`（`bootloader_utility.c:578`）：

```c
for (index = start_index; index >= FACTORY_INDEX; index--) {
    part = index_to_partition(bs, index);   /* 只从 bs 读 offset/size */
    if (part.size == 0) continue;
    if (check_anti_rollback(&part) && try_load_partition(&part, &image_data)) {
        load_image(&image_data);            /* 真正加载 */
    }
}
```

`index_to_partition()`（`:275`）：
```c
if (index == FACTORY_INDEX) return bs->factory;   /* ← 仅此而已 */
```

`try_load_partition()`（`:472`）：
```c
if (partition->size == 0) return false;
if (bootloader_load_image(partition, data) == ESP_OK) return true;  /* 只用 offset/size */
```

**结论**：`bs` 只是 `{offset, size}` 的容器，**内容由我们填**。
只要 `bs.factory = {user_addr, user_size}` 且 `start_index = FACTORY_INDEX`，
`load_boot_image()` 就会直接加载该地址 —— **全程不触碰 flash 分区表** ✅

**`bootloader_state_t` 定义**（`bootloader_config.h:25`）：
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

#### 方式 B：只加钩子（更轻量）

IDF 提供两个 `weak` 函数（`bootloader_hooks.h`）：
```c
void __attribute__((weak)) bootloader_before_init(void);
void __attribute__((weak)) bootloader_after_init(void);
```
在项目里定义即可，无需覆盖整个 bootloader。
`bootloader/subproject/main/CMakeLists.txt:7` 用
`-u bootloader_hooks_include` 强制链接这两个符号。

#### 方式 C：IDF 预留的"客户扩展点"

`bootloader_start.c:130`（IDF 源码内）留有注释：
```c
// Customer implementation.
// if (gpio_pin_1 == true && ...){
//     boot_index = required_boot_partition;
// } ...
```
这是 IDF 官方标注的插入位置，但需改 IDF 源码，**不推荐**。

**结论**：采用**方式 A**（`bootloader_components/main`），完全在项目内实现，
不触碰 IDF 源码，升级 IDF 不受影响。

### 8.3 关键 API 确认（**均已核对 IDF 源码**）

| API | 位置 | 用途 | 我们是否使用 |
|-----|------|------|:------------:|
| `bootloader_init()` | `bootloader_init.h` | 硬件初始化 | ✅ |
| `bootloader_utility_load_boot_image(&bs, index)` | `bootloader_utility.c:578` | 按 `bs` 加载镜像 | ✅ |
| `bootloader_utility_load_partition_table(&bs)` | `bootloader_utility.c:143` | 读 flash 分区表 | ❌ **不调用** |
| `try_load_partition()` | `bootloader_utility.c:472` | 内部：按 `{offset,size}` 加载 | (内部调用) |
| `index_to_partition()` | `bootloader_utility.c:275` | 内部：从 `bs` 取分区 | (内部调用) |
| `esp_image_verify()` | `esp_image_format.h` | 镜像校验 | (内部调用) |
| `bootloader_reset()` | `bootloader_utility.c:1159` | 复位 | ✅ 失败时 |

> ⚠️ **重要修正**：`bootloader_utility_load_boot_image_from_pos()` **不存在**。
> 正确做法是**手工构造 `bootloader_state_t`** 后调用
> `bootloader_utility_load_boot_image(&bs, FACTORY_INDEX)`。
> 详见 8.2 的源码依据。

> **依据**：`examples/custom_bootloader/bootloader_multiboot/` 与
> `examples/custom_bootloader/bootloader_override/` 两个官方示例。

### 8.4 修改文件（18 个）

| 文件 | 改动 |
|------|------|
| `partitions_iap.csv` | 重写为表 A |
| `partitions_app.csv` | 新建（表 B） |
| `examples/user_app_template/partitions_user_app.csv` | 重写为表 B |
| `sdkconfig.defaults` | `PARTITION_TABLE_OFFSET=0xB000` + 禁用 RTC FAST 堆 + `BOOTLOADER_RESERVE_RTC_MEM` |
| `examples/user_app_template/sdkconfig.defaults` | `PARTITION_TABLE_OFFSET=0x200000` |
| `main/iap_common.h` | 新增地址常量；**删除 `IAP_PARTITION_LABEL_MARK` / `IAP_MAGIC_MARKER*`** |
| `main/iap_config.c` | 改用 `esp_flash_read/write` 硬编码直访 |
| `main/iap_image.c` | 地址常量 + 跳转改**写 RTC RAM**；**删除 `s_mark_part` / `iap_image_check_marker()` / `iap_image_set_marker()` / `iap_image_marker_addr()`** |
| `main/iap_image.h` | 常量声明；**删除 marker API** |
| `main/main.c` | 启动决策简化；**删除 `iap_image_check_marker()` 调用** |
| `main/iap_uart.c` | `part` 命令；**删除 `marker` / `marker set` / `marker clear` 命令** |
| `main/iap_wait_trigger.c` | **已有 GPIO 触发实现，无需新写**；仅调整默认值（见 §3.3） |
| `tools/gen_partitions.py` | 双表生成；**删除 `iap_mark` 条目** |
| `tools/merge_bin.py` | 3 文件输出；**删除 `ADDR_IAP_MARK`** |
| `tools/gen_factory_cfg.py` | 配置区地址 `0x8000` |
| `tools/verify_html_editor.py` | 常量同步 |
| `tools/verify_marker.py` | **删除整个文件** |
| `esp_iap_tool.html` | 全部地址常量 + 槽位；**删除「强制进入 IAP」按钮及 `forceEnterIap()`** |
| `README.md` | 布局 + 烧录说明；**删除「强制进入 IAP —— flash 魔术标记」章节** |
| `.github/workflows/build.yml` | 产物分段；**删除 `Verify magic marker addresses` 步骤** |

### 8.5 删除内容

| 项 | 说明 |
|----|------|
| `otadata` 分区 | 由 RTC RAM 取代 |
| `phy_init` 分区 | 无代码引用 |
| **flash 标志区 `iap_mark`** | **由 RTC RAM 取代**（用户建议） |
| **`iap_image_check_marker()` 等 marker API** | **随标志区一并删除** |
| **`marker` UART 命令** | **随标志区一并删除** |
| **HTML「强制进入 IAP」按钮** | **随标志区一并删除**（替代方案见 §3.3） |
| **`tools/verify_marker.py`** | **随标志区一并删除** |
| `esp_ota_set_boot_partition()` | 改为写 RTC RAM |

---

## 九、实施步骤

| 阶段 | 内容 | 验证 |
|------|------|------|
| 1 | `sdkconfig.defaults`（RTC FAST 堆禁用 + `BOOTLOADER_RESERVE_RTC_MEM`） | 编译通过 + 堆实测 |
| 2 | 分区表 A/B + `PARTITION_TABLE_OFFSET` | `idf.py build` 通过 |
| 3 | `iap_boot_param.h` + RTC RAM 读写验证 | 实机验证跨复位保持 |
| 4 | `iap_config.c` 硬编码直访 | 实机读配置 |
| 5 | `iap_image.c` 跳转改**写 RTC RAM** | 编译 |
| 6 | **自定义 bootloader 组件** | 生成新 bootloader |
| 7 | 工具脚本（gen_partitions / merge_bin） | 二进制正确 |
| 8 | HTML 工具 | `verify_html_editor.py` 通过 |
| 9 | 文档 | — |
| 10 | 实机验证 | 烧录 + 双向切换 |

---

## 十、风险与缓解

| 风险 | 等级 | 缓解 |
|------|:----:|------|
| 破坏性变更（需重烧） | 🔴 高 | 一次性迁移，`erase_flash` |
| 自定义 bootloader 维护 | 🟡 中 | 只改 1 个函数（`call_start_cpu0`） |
| bootloader 崩溃无日志 | 🟡 中 | 保留 `ESP_LOGI`；JTAG 备用 |
| 地址常量散落 20+ 文件 | 🟡 中 | 加 `verify_layout.py` 交叉校验 |
| 堆减少 16KB | 🟢 低 | 实测仍 122KB |
| **RTC RAM 数据损坏** | 🟢 低 | magic/CRC 失败 → 默认启动 IAP |
| **冷启动 RTC RAM 无效** | 🟢 低 | magic 失败 → 默认启动 IAP ✅ |
| **ESP32-C2 不支持** | 🟡 中 | **硬件限制**：C2 无 RTC FAST RAM。<br>`iap_boot_param.h` 加 `#error` 明确拦截。<br>其余 7 个目标均支持。 |

---

## 十一点五、实施结果（**已完成**）

### 11.1 编译验证（8 目标）

| 目标 | 主固件 | bootloader | 自定义 hook | 备注 |
|------|:------:|:----------:|:-----------:|------|
| **esp32** | ✅ | 0x58c0 | ✅ | 修复了 ROM SHA API 差异 |
| **esp32s2** | ✅ | 0x4920 | ✅ | |
| **esp32s3** | ✅ | 0x44c0 | ✅ | |
| **esp32c2** | ❌ | — | — | **硬件不支持 RTC FAST RAM** |
| **esp32c3** | ✅ | 0x4340 | ✅ | |
| **esp32c5** | ✅ | 0x47e0 | ✅ | |
| **esp32c6** | ✅ | 0x4790 | ✅ | 主目标 |
| **esp32h2** | ✅ | 0x4600 | ✅ | |

**7/8 目标编译通过**（C2 为硬件限制）。

### 11.2 关键验证点（esp32c6）

**烧录布局**（`flasher_args.json`）：
```
0x0      bootloader/bootloader.bin
0xb000   partition_table/partition-table.bin    ← 分区表 A
0x10000  esp_iap.bin                            ← IAP 程序
```

**Kconfig 生效**：
```
CONFIG_BOOTLOADER_RESERVE_RTC_MEM        1
CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC     1
CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE 0x128
CONFIG_PARTITION_TABLE_OFFSET            0xB000
```

**自定义 bootloader 生效**（符号表）：
```
4086b938 T call_start_cpu0      ← 我们的实现
4086b910 t boot_from_addr
```

**RTC RAM 布局**（linker map）：
```
lp_ram_seg       0x50000000  len=0x3eb0   ← APP 可用
lp_reserved_seg  0x50003eb0  len=0x0150   ← 含 rtc_retain_mem_t
_bootloader_data_rtc_mem_end = 0x50004000 ← LP RAM 末尾
```
`0x3eb0 + 0x150 = 0x4000` ✅ 正好填满 16KB，**不在 0x50000000**（避免冲突）。

**IAP 固件大小**：`0xf8820` (1,017,888 字节)，
分区 `0x1F0000` → **50% 空闲** ✅

### 11.3 实施中发现并修复的问题

| # | 问题 | 根因 | 修复 |
|---|------|------|------|
| 1 | bootloader 找不到 `iap_boot_param.h` | 头文件在 `main/`，不在 bootloader include 路径 | `CMakeLists.txt` 加 `PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../main"` |
| 2 | **esp32 编译失败**（`ets_sha_init` 参数错误） | ROM SHA API 跨目标签名不同（ESP32 老 API vs 新 API） | 加 `sha_begin/sha_update/sha_finish` 静态内联包装 |
| 3 | **esp32c2 编译失败**（`custom` 成员不存在） | C2 无 RTC FAST RAM | `iap_boot_param.h` 加 `#error` 明确拦截 |

**问题 2 详情**（既有缺陷，非本次引入）：

| 目标 | `ets_sha_init` | `ets_sha_update` |
|------|---------------|------------------|
| ESP32 | `(ctx)` | `(ctx, type, in, **bits**)` |
| S2/S3/C3/C5/C6/H2 | `(ctx, type)` | `(ctx, in, **bytes**, bool)` |

**问题 3 详情**：C2 的 `soc_caps.h` 无 `SOC_RTC_FAST_MEM_SUPPORTED`，
导致 `CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC` 依赖不满足 → `custom[]` 不存在。

---

## 十一、待确认事项

| # | 事项 | 建议 | 状态 |
|---|------|------|:----:|
| 1 | ~~`iap_boot_param_t` 传哪些配置字段~~ | ✅ **只传启动参数**，配置字段保留在 `iap_cfg` | ✅ **已定**（§5.5） |
| 2 | `iap_boot_param_t` 总大小 | **100 字节**（含 `reserved1[64]` 预留） | ✅ **已定** |
| 3 | `CUSTOM_RESERVE_RTC_SIZE` | **128**（4 字节对齐，留余量） | ✅ **已定** |
| 4 | `0x00A000`（4KB 原 iap_mark 位置）用途 | 保留空位（不分配） | ⏳ 待定 |
| 5 | `0x00F000`（4KB 保留）用途 | 暂留空 | ⏳ 待定 |
| 6 | 是否保留 `iap` 在分区表 A | 保留（IAP 代码需 `esp_partition_find_first` 取自身句柄） | ✅ **已定** |
| 7 | 启动决策存放位置 | **RTC RAM**（用户建议，取消 flash 标志区） | ✅ **已定**（§3.1.0） |
| 8 | 自定义 bootloader 实现方式 | `bootloader_components/main` + 手工构造 `bs` | ✅ **已定**（§8.2） |
| 9 | **去掉标志区 / HTML 不再支持改标志区** | ✅ **已采纳**，替代方案见 §3.3 | ✅ **已定** |
| 10 | **GPIO0 电平检测放 IAP**（bootloader 不管） | ✅ **已定**（§3.3） | ✅ **已定** |
| 11 | **纯电平检测，不做长按** | ✅ **已定**（§3.3） | ✅ **已定** |
| 12 | GPIO 触发默认值（`trig_gpio=0` + 默认使能） | 建议改默认值，见 §3.3 | ⏳ 待定 |

---

## 十二、已完成的准备工作

| 项 | 状态 |
|---|------|
| 方案文档 v4（15 章） | ✅ `docs/layout-redesign.md` |
| RTC RAM 分析（官方文档 + 实测） | ✅ |
| 双分区表可行性验证 | ✅ |
| **分区表 A/B 编写 + 官方工具校验** | ✅ `partitions_iap.csv` / `partitions_app.csv` |
| **bootloader 加载机制核查（IDF 源码）** | ✅ 确认可绕过分区表 |
| `sdkconfig.defaults` 改动（RTC FAST 堆禁用） | ⚠️ **已改，待编译验证** |
| 代码改动 | ⏳ **未开始** |

### 12.1 分区表编写过程中发现的 IDF 硬约束

| 约束 | 值 | 来源 | 影响 |
|------|-----|------|------|
| 可读写 NVS 最小尺寸 | `0x3000` (12KB) | `gen_esp32part.py:49,599` | `nvs_iap` 不能是 4KB |
| `app` 分区 Offset 对齐 | `0x10000` (64KB) | `gen_esp32part.py:107,119-123,570` | `iap`/`user_app` 偏移须 64KB 对齐 |
| `app` 分区 Size 对齐 | `0x10000` (64KB) | `gen_esp32part.py:573` | `iap`/`user_app` 大小须 64KB 对齐 |
| `data` 分区对齐 | `0x1000` (4KB) | `gen_esp32part.py:123` | 普通 data 分区按 4KB |

**由此产生的布局调整**：
- `nvs_iap`：4KB → **12KB**（`0xC000`–`0xF000`），IAP 起点 `0xE000` → **`0x10000`**
- `nvs_app`：4KB → **12KB**（`0x201000`–`0x204000`），`user_app` 起点 `0x202000` → **`0x210000`**
  （分区表 B 占 `0x200000`，必须在所有分区之前）

### 12.2 关于"启动决策放 RTC RAM"的优化（用户建议）

**用户建议**："决定**启动谁**的职责也交给了 RTC RAM，而且如果这样的话，
boot 不需要复制参数了，交给 iap 去复制"

**采纳并验证**：

| 项 | 原方案 | **优化后** |
|---|--------|-----------|
| 启动决策存放 | flash 标志区 `0xA000` | **RTC RAM `custom[]`** |
| bootloader 读 | flash 标志区 | **RTC RAM** |
| bootloader 复制参数？ | ✅ 需要 | ❌ **不需要** |
| flash 写次数 | 每次切换写 | **0 次** |

**核查中发现的 2 个技术要点**：

**① bootloader 的 `.bss` 清零**不会影响 RTC RAM ——
`bootloader.memory.ld.in` 的 `MEMORY` 只有 `iram_seg` / `iram_loader_seg` /
`dram_seg`（均在 `0x4086xxxx` 内部 SRAM），**不含 `0x50000000`**。

**② 必须用 IDF 官方 `custom[]`，不能用 `0x50000000`** ——
`0x50000000` 是 APP 的 `lp_ram_seg` 起始（`esp32c6/memory.ld.in:123`），
会被 APP 使用。正确位置是 LP RAM **末尾保留区**：
```
0x50004000 - RESERVE_RTC_MEM  lp_reserved_seg
  └── bootloader_data_rtc_mem  ← rtc_retain_mem_t（含 custom[]）
```

**③ 冷启动仍满足"每次上电必跑 IAP"** ——
RTC RAM 断电丢失，冷启动时 magic 校验失败 → 默认启动 IAP ✅

### 12.2.1 关于"`iap_boot_param_t` 只传启动参数"（用户要求）

**用户要求**："**`iap_boot_param_t`** 目前只传启动参数先，其他的先保留"

**采纳结果**：

| 项 | 决定 |
|----|------|
| 传什么 | **只传启动参数**（`boot_target` / `user_addr` / `user_size` / `user_version`） |
| 配置字段 | **保留在 `iap_cfg`**，暂不复制到 RTC RAM |
| APP 如何拿配置 | 通过 `p->cfg_addr` 定位 `iap_cfg`，自行读 flash |
| 预留 | `reserved1[64]` —— 以后加配置子集**无需改结构大小** |
| 结构大小 | **100 字节** → `CUSTOM_RESERVE_RTC_SIZE = 128` |

**最终结构**（见 §5.5）：

```c
typedef struct {
    uint32_t magic;          /* IAP_PARAM_MAGIC */
    uint32_t version;        /* IAP_PARAM_VERSION */
    uint32_t crc32;

    uint8_t  boot_target;    /* 0 = IAP, 1 = APP */
    uint8_t  reserved0[3];
    uint32_t user_addr;      /* 0x210000 */
    uint32_t user_size;
    uint32_t user_version;

    uint32_t cfg_addr;       /* 0x8000 */
    uint32_t cfg_seq;

    uint8_t  reserved1[64];  /* 配置子集预留 */
} iap_boot_param_t;
```

### 12.3 关于"去掉标志区 + HTML 不再支持改标志区"（用户要求）

**用户要求**："可以去掉标志区，html 也不再支持改标志区进入 IAP"

**采纳范围**：

| 项 | 处理 |
|----|------|
| flash 标志区 `iap_mark` | **删除** |
| `IAP_PARTITION_LABEL_MARK` / `IAP_MAGIC_MARKER*` | **删除** |
| `iap_image_check_marker()` 等 API | **删除** |
| `marker` UART 命令 | **删除** |
| HTML「强制进入 IAP」按钮 + `forceEnterIap()` | **删除** |
| `tools/verify_marker.py` | **删除** |
| `build.yml` 的 `Verify magic marker addresses` 步骤 | **删除** |
| README「强制进入 IAP —— flash 魔术标记」章节 | **删除** |

**能力保留方案**：见 §3.3 —— **GPIO0 电平检测（IAP 内）+ 断电重上电**。

### 12.3.1 关于"GPIO0 检测放 IAP + 纯电平检测"（用户要求）

**用户要求**：
1. "**GPIO0 长按**是唯一'不用断电、不用设备能跑'的救援方式交给 IAP，bootloader 不管"
2. "没必要长按，简单检测电平就行"

**采纳结果**：

| 项 | 决定 |
|----|------|
| 检测位置 | **IAP 内**（`iap_wait_trigger.c`，已有实现） |
| bootloader 职责 | **不做任何 GPIO 检测**，只读 RTC RAM |
| 检测方式 | **纯电平检测**（`trig_gpio_active()` 单次读电平） |
| 是否计时/长按 | ❌ **不做** |
| 引脚 | GPIO0（板载 BOOT 键，低电平有效 + 内部上拉） |
| 窗口 | `wait_seconds`（默认 3 秒） |

**代码影响**：**零新增** —— 现有实现即为电平检测，只需调整默认值（§3.3）。

**已确认**：ESP32-C6 `SOC_GPIO_PIN_COUNT = 31`，GPIO0 有效。

### 12.4 关于自定义 bootloader 机制的修正（核查 IDF 源码）

**核查中发现的问题**：

| 项 | 原方案 | 修正后 |
|---|--------|--------|
| bootloader 目录 | `components/custom_bootloader/` | **`bootloader_components/main/`**（IDF 约定） |
| 加载 API | `bootloader_utility_load_boot_image_from_pos()` ❌ **不存在** | 手工构造 `bootloader_state_t` + `bootloader_utility_load_boot_image(&bs, FACTORY_INDEX)` ✅ |
| 是否改 IDF 源码 | 未明确 | **完全不需要** —— 用 IDF 官方覆盖机制 |

**关键源码依据**（均已核对）：
- `bootloader_utility.c:275` `index_to_partition()` 只从 `bs` 读 `{offset, size}`
- `bootloader_utility.c:472` `try_load_partition()` 只用 `partition->offset/size`
- `bootloader_utility.c:578` `load_boot_image()` 主循环

→ **`bs` 内容由我们填，因此可完全绕过分区表** ✅

---

## 附：关键源码依据

| 结论 | 源码位置 |
|------|---------|
| bootloader 用编译期常量读分区表 | `bootloader_utility.c:150` |
| bootloader 启动决策走 otadata | `bootloader_utility.c:378` |
| IDF 预留客户扩展点 | `bootloader_start.c:130` |
| `esp_partition_find_first` 用编译期常量 | `partition.c:128` |
| 分区表只加载一次（全局链表） | `partition.c:262` |
| `nvs_flash_init` 靠分区名查找 | `nvs_api.cpp:204` |
| WiFi 驱动强制用 NVS | `esp_adapter.c:340` |
| bootloader 跳转不传参 | `bootloader_utility.c:1150` |
| `esp_restart` 走完整复位 | `esp_system.c:40` |
| RTC FAST 默认加入堆 | `esp_system/Kconfig:117` |
| **`bootloader_state_t` 定义** | `bootloader_config.h:25` |
| **`index_to_partition()` 只从 `bs` 取** | `bootloader_utility.c:275` |
| **`try_load_partition()` 只用 offset/size** | `bootloader_utility.c:472` |
| **`load_boot_image()` 主循环** | `bootloader_utility.c:578` |
| **IDF 官方 bootloader 钩子** | `bootloader_hooks.h:28,34` |
| **`bootloader_components/main` 覆盖机制** | `bootloader/subproject/main/CMakeLists.txt:7` |
| **官方多启动示例** | `examples/custom_bootloader/bootloader_multiboot/` |
| **官方覆盖示例** | `examples/custom_bootloader/bootloader_override/` |
| **`rtc_retain_mem_t` 定义（含 `custom[]`）** | `esp_image_format.h:58` |
| **`bootloader_common_get_rtc_retain_mem()`** | `bootloader_common_loader.c:268` |
| **`reboot_counter` 维护** | `bootloader_common_loader.c:207,248` |
| **C6 LP RAM 布局（bootloader_data_rtc_mem）** | `esp_system/ld/esp32c6/memory.ld.in:97-133` |
| **官方 RTC custom 测试** | `bootloader_support/test_apps/rtc_custom_section/` |
| **bootloader 链接脚本不含 RTC RAM** | `bootloader/subproject/main/ld/esp32c6/bootloader.memory.ld.in` |
| NVS 最小 12KB | `gen_esp32part.py:49,599` |
| app 分区 64KB 对齐 | `gen_esp32part.py:107,119-123,570-573` |
| PHY 数据来源配置 | `phy_init.c:39` |
| RTC_NOINIT 语义 | `docs/zh_CN/api-guides/memory-types.rst:157` |

---

**报告完毕。请批准后开始实施。**
