# IAP 分区布局重构方案

> ⚠️ **历史文档** —— 本文描述 v1 设计阶段，**已完全过时**。
> 当前实现（v6）请以 [`README.md`](../README.md) 为准。

> 状态: **待审阅** — 请确认后再动代码
> 日期: 2026-09-22
> 目标: 固定区地址全部硬编码，配置区不依赖分区表，烧录简化为 2 个文件

---

## 1. 设计目标

| # | 目标 | 说明 |
|---|------|------|
| 1 | **固定区地址硬编码** | bootloader / 配置区 / 标志区 / 分区表 的地址在代码中写死，不通过分区表查找 |
| 2 | **配置区不依赖分区表** | 用 `esp_flash_read/write` 直接访问，bootloader 尚未解析分区表时也可用 |
| 3 | **烧录简化为 2 个文件** | ① `bootloader.bin`（含配置区+标志区+分区表）② `user.bin`（用户态） |
| 4 | **分区表前固定、后可追加** | `0x0 ~ 0x11FFF` 冻结，`0x12000` 之后可自由追加 |
| 5 | **每次上电必跑 IAP** | 删除 otadata，bootloader 走 `ota_info.offset == 0` 分支 → 永远启动 factory |

---

## 2. 新分区布局

### 2.1 固定区（地址硬编码，不依赖分区表）

| 偏移 | 结束 | 大小 | 名称 | 说明 | 谁写死 |
|------|------|------|------|------|--------|
| `0x000000` | `0x007FFF` | 32KB | **bootloader** | ROM 加载 | ROM（芯片硬编码） |
| `0x008000` | `0x00FFFF` | 32KB | **配置区** | IAP 配置（双槽，每槽 4KB） | `IAP_CFG_ADDR`（IAP 硬编码） |
| `0x010000` | `0x010FFF` | 4KB | **标志区** | 强制进入 IAP 的魔术标记 | `IAP_MARK_ADDR`（IAP 硬编码） |
| `0x011000` | `0x011FFF` | 4KB | **分区表** | ESP 分区表 | `CONFIG_PARTITION_TABLE_OFFSET`（编译期常量） |

> **为什么分区表必须在固定区**：
> ROM → 读 `0x0` bootloader → bootloader 用**编译期常量** `ESP_PARTITION_TABLE_OFFSET`
> `mmap` 分区表（见 `bootloader_utility.c:150`）。bootloader 在找到配置区**之前**
> 就必须知道分区表在哪，因此分区表位置**无法**由配置区指定（鸡生蛋）。

### 2.2 可变区（由分区表定义）

| 偏移 | 大小 | 名称 | 说明 |
|------|------|------|------|
| `0x012000` | 可变 | **用户态** (`user_app`) | 用户程序（纯 ESP 应用镜像） |
| 其后 | 可变 | **追加分区** | 用户可自由追加（文件系统等） |

### 2.3 布局图

```
┌────────────────────────────────────────────────────────────┐
│  固定区 (0x0 ~ 0x11FFF, 共 72KB)                           │
│  地址全部编译期硬编码，不查分区表                          │
│                                                            │
│  0x000000  ┌──────────────────────┐                        │
│            │  bootloader   (32KB) │  ROM 硬编码            │
│  0x008000  ├──────────────────────┤                        │
│            │  配置区       (32KB) │  IAP_CFG_ADDR          │
│  0x010000  ├──────────────────────┤                        │
│            │  标志区        (4KB) │  IAP_MARK_ADDR         │
│  0x011000  ├──────────────────────┤                        │
│            │  分区表        (4KB) │  CONFIG_PARTITION_...  │
│  0x012000  └──────────────────────┘                        │
├────────────────────────────────────────────────────────────┤
│  可变区 (0x12000 ~ 末尾)                                   │
│  由分区表定义，可自由追加                                  │
│                                                            │
│  0x012000  ┌──────────────────────┐                        │
│            │  用户态 (user_app)   │  大小 = flash - 0x12000│
│            ├──────────────────────┤                        │
│            │  追加分区 (可选)     │                        │
│            └──────────────────────┘                        │
└────────────────────────────────────────────────────────────┘
```

### 2.4 与当前布局对比

| 项 | 当前 | 新方案 | 变化 |
|----|------|--------|------|
| bootloader | `0x0` 32KB | `0x0` 32KB | — |
| 分区表 | `0x8000` 4KB | **`0x11000`** 4KB | ⚠️ 移动 |
| nvs | `0x9000` 24KB | **删除**（bootloader 写死） | ⚠️ 删除 |
| phy_init | `0xF000` 4KB | **删除**（bootloader 写死） | ⚠️ 删除 |
| 配置区 | `0x10000` 8KB | **`0x8000`** 32KB | ⚠️ 移动+扩大 |
| otadata | `0x12000` 8KB | **删除** | ⚠️ 删除 |
| 标志区 | `0x14000` 4KB | **`0x10000`** 4KB | ⚠️ 移动 |
| IAP 程序 | `0x20000` 1984KB | **删除**（并入用户态） | ⚠️ 删除 |
| 用户态 | `0x210000` 1984KB | **`0x12000`** | ⚠️ 移动 |

> **关于 IAP 程序区**：新方案中 IAP 程序**不再是独立分区**，而是作为
> **factory 应用**存在于分区表中（`app/factory`）。用户态是 `ota_0`。
> 若 IAP 也不在分区表里，bootloader 无法通过分区表找到它 —— 但 bootloader
> 需要 factory 分区来启动。因此 **IAP 程序必须在分区表中**（作为 `app/factory`）。

### 2.5 修正后的完整布局

```
固定区 (地址硬编码):
  0x000000  bootloader        32KB
  0x008000  配置区            32KB   ← IAP_CFG_ADDR
  0x010000  标志区             4KB   ← IAP_MARK_ADDR
  0x011000  分区表             4KB   ← CONFIG_PARTITION_TABLE_OFFSET

可变区 (分区表定义):
  0x012000  iap       (factory)  1984KB  ← IAP 程序 (bootloader 启动它)
  0x210000  user_app  (ota_0)    剩余    ← 用户态
            追加分区    (可选)
```

> **注意**：IAP 程序仍需保留在 `0x20000`（或其它分区表定义的位置），
> 因为 bootloader 需要从分区表找到 factory 应用来启动。

---

## 3. 烧录文件

### 3.1 两个文件

| 文件 | 内容 | 烧录地址 | 大小 |
|------|------|---------|------|
| **`bootloader.bin`** | bootloader + 配置区 + 标志区 + 分区表 | `0x0` | `0x12000` (72KB) |
| **`user.bin`** | 用户态 | `0x12000` | 可变 |

### 3.2 烧录命令

```bash
esptool.py --chip esp32c6 -p COM18 write_flash \
    0x0     bootloader.bin \
    0x12000 user.bin
```

> **顺序无关**：两段地址不重叠。

### 3.3 与当前对比

| | 当前 | 新方案 |
|---|------|--------|
| 文件数 | 3 个（bootloader 段 / 应用段 / 分区表） | **2 个** |
| 分段原因 | 跳过分区表区避免覆盖 | 固定区整体烧录，无重叠问题 |
| 用户态地址 | `0x210000` | `0x12000` |

---

## 4. 配置区新结构

### 4.1 新增字段

配置区存放**固定区之外的元信息**，供 IAP 与用户态共享：

| 字段 | 类型 | 说明 |
|------|------|------|
| `ptable_addr` | u32 | 分区表地址（**冗余**，实际固定 `0x11000`，便于工具校验） |
| `ptable_size` | u32 | 分区表大小（固定 `0x1000`） |
| `user_addr` | u32 | 用户态起始地址（`0x12000`） |
| `user_size` | u32 | 用户态大小（= flash 容量 - `0x12000`） |
| `layout_ver` | u16 | 布局版本号（用于未来迁移） |

> **`ptable_addr` 的意义**：虽然分区表位置是编译期常量，但把它写进配置区
> 可让**外部工具**（HTML / 脚本）无需硬编码即可知道布局，也便于未来校验一致性。

### 4.2 配置区大小

| 项 | 当前 | 新方案 |
|----|------|--------|
| 大小 | 8KB（双槽 × 4KB） | **32KB**（双槽 × 16KB） |
| 槽大小 | 4096 | **16384** |
| 数据可用 | 4064 字节/槽 | 16352 字节/槽 |

> **扩大原因**：为未来预留（如多套配置、启动参数扩展）。若不需要，可保持 8KB。

---

## 5. 代码改动清单

### 5.1 分区表 CSV（2 个）

| 文件 | 改动 |
|------|------|
| `partitions_iap.csv` | 删除 nvs/phy_init/otadata；配置区移到 `0x8000`；标志区移到 `0x10000`；分区表 `0x11000`；user_app 移到 `0x12000` |
| `examples/user_app_template/partitions_user_app.csv` | 同步 |

**新 `partitions_iap.csv`**：
```csv
# Name,     Type,    SubType, Offset,   Size,     Flags
iap_cfg,    0x40,    0x00,    0x8000,   0x8000,
iap_mark,   0x41,    0x00,    0x10000,  0x1000,
iap,        app,     factory, 0x12000,  0x1F0000,
user_app,   app,     ota_0,   0x210000, 0x1F0000,
```

> **注意**：分区表自身**不列入分区表**（它是元数据）。`0x11000` 处是分区表，
> 但 CSV 中不写它 —— ESP-IDF 的 `gen_esp32part.py` 允许分区表位置有"空洞"。

### 5.2 IAP C 代码

| 文件 | 改动 |
|------|------|
| `main/iap_common.h` | 新增 `IAP_CFG_ADDR` / `IAP_MARK_ADDR` / `IAP_PTABLE_ADDR` / `IAP_USER_ADDR` 常量；更新布局注释 |
| `main/iap_config.c` | **不再用 `esp_partition_find_first()`**，改为 `esp_flash_read/write(IAP_CFG_ADDR, ...)`；`fill_defaults()` 加新字段 |
| `main/iap_image.c` | `USER_APP_ADDR` 改 `0x12000`；`iap_image_boot_slot_direct()` 用新地址；删除 otadata 相关逻辑 |
| `main/iap_image.h` | 更新地址常量声明 |
| `main/main.c` | 启动决策：删除 otadata 相关分支 |
| `main/iap_uart.c` | `part` 命令显示新布局 |

### 5.3 工具脚本

| 文件 | 改动 |
|------|------|
| `tools/gen_partitions.py` | `FIXED_PARTITIONS` 重写；`PARTITION_TABLE_OFFSET` 改 `0x11000`；`USER_APP_OFFSET` 改 `0x12000` |
| `tools/merge_bin.py` | 地址常量重写；输出改为 2 个文件 |
| `tools/gen_factory_cfg.py` | 配置区地址改 `0x8000`；新增字段 |
| `tools/verify_html_editor.py` | 常量同步 |
| `tools/verify_marker.py` | 标志区地址改 `0x10000` |
| `tools/test_bootloader_check.py` | 无改动 |

### 5.4 HTML 工具

| 项 | 改动 |
|----|------|
| `CFG_OFFSET_IN_IMAGE` | `0x10000` → `0x8000` |
| `IAP_APP_OFFSET` | `0x20000` → `0x12000`（用户态） |
| `PARTITION_TABLE_OFFSET` | `0x8000` → `0x11000` |
| `USER_APP_ADDR` | `0x210000` → `0x12000` |
| `IAP_MARKER_ADDR` | `0x14000` → `0x10000` |
| `FIXED_PARTITIONS` | 重写 |
| 槽位定义 | 4 个槽位 → 2 个（bootloader / 用户态） |
| 识别逻辑 | `identifyFile()` 判据更新 |

### 5.5 其他

| 文件 | 改动 |
|------|------|
| `sdkconfig.defaults` | `CONFIG_PARTITION_TABLE_OFFSET=0x11000` |
| `README.md` | 布局图、烧录说明、地址常量表 |
| `.github/workflows/build.yml` | 产物分段说明 |
| `examples/user_app_template/main/iap_user_api.h` | 结构体同步（新字段） |

---

## 6. 迁移步骤

### 6.1 一次性迁移（已烧录设备）

**必须完整重烧**（分区布局变了）：

```bash
# 1. 擦除整个 flash（清除旧布局）
esptool.py --chip esp32c6 -p COM18 erase_flash

# 2. 烧录新布局
esptool.py --chip esp32c6 -p COM18 write_flash \
    0x0     bootloader.bin \
    0x12000 user.bin
```

### 6.2 实施顺序（建议）

| 阶段 | 内容 | 验证 |
|------|------|------|
| 1 | 改分区表 CSV + `sdkconfig.defaults` | `idf.py build` 通过 |
| 2 | 改 IAP C 代码（常量 + 配置区直访） | 编译通过 |
| 3 | 改工具脚本（gen_partitions / merge_bin） | 生成的二进制正确 |
| 4 | 改 HTML 工具 | `verify_html_editor.py` 通过 |
| 5 | 更新文档 | — |
| 6 | 实机验证 | 烧录 + 启动 + 用户态跳转 |

---

## 7. 风险与取舍

### 7.1 风险

| 风险 | 等级 | 缓解 |
|------|------|------|
| 破坏性变更（需重烧所有设备） | 🔴 高 | 一次性迁移，文档说明 |
| 配置区扩大 4 倍（8KB→32KB） | 🟡 中 | 可保持 8KB，仅改位置 |
| 地址常量散落 10+ 文件，易漏改 | 🟡 中 | 加 `verify_*.py` 交叉校验 |
| 删除 nvs 后 WiFi 校准数据无处存 | 🟡 中 | WiFi 已用 `WIFI_STORAGE_RAM`，不写 NVS |
| 删除 otadata 后无法用 OTA 机制 | 🟢 低 | 改用 IAP 直接跳转（已实现） |

### 7.2 关于 nvs / phy_init

你提到"**在 bootloader 写死**"。需要说明：

| 分区 | 能否"写死在 bootloader" | 说明 |
|------|----------------------|------|
| `phy_init` | ✅ 可以 | PHY 校准数据，若不持久化则每次启动重新校准（慢几 ms） |
| `nvs` | ⚠️ 需确认 | ESP-IDF 的 WiFi 驱动默认会写 NVS（`config NVS flash: enabled`）。本工程已设 `WIFI_STORAGE_RAM`，理论上不需要 NVS。但**其它组件**（如 `esp_netif`）可能仍会尝试打开 NVS |

**建议**：先删除 `nvs`/`phy_init` 分区，若启动报错再恢复。或者保留一个极小的 nvs（4KB）。

### 7.3 替代方案（改动更小）

若觉得风险太大，可考虑**渐进方案**：

| 方案 | 改动 | 效果 |
|------|------|------|
| **A. 完整重构**（本文档） | 大 | 2 文件烧录，配置区不依赖分区表 |
| **B. 仅删 otadata** | 小（1 个 CSV） | 每次上电必跑 IAP |
| **C. B + 配置区硬编码** | 中 | 配置区不依赖分区表，但布局不变 |

---

## 8. 待确认问题

| # | 问题 | 选项 |
|---|------|------|
| 1 | 配置区大小：32KB 还是保持 8KB？ | 32KB（预留）/ 8KB（够用） |
| 2 | IAP 程序区位置：保持 `0x20000`？ | 保持 / 移到别处 |
| 3 | nvs / phy_init：删除还是保留极小？ | 删除 / 保留 4KB |
| 4 | 用户态起始：`0x12000`？ | 是 / 其他 |
| 5 | 是否保留 otadata 分区（不读但存在）？ | 删除 / 保留 |
| 6 | 配置区是否真的需要存 `ptable_addr`？ | 需要（工具用）/ 不需要 |

---

## 9. 附：关键代码路径

### 9.1 bootloader 读取分区表（不可改）

```c
// components/bootloader_support/src/bootloader_utility.c:150
partitions = bootloader_mmap(ESP_PARTITION_TABLE_OFFSET, ESP_PARTITION_TABLE_MAX_LEN);
//                        ^^^^^^^^^^^^^^^^^^^^^^^^^^ 编译期常量
```

### 9.2 bootloader 选择启动分区（删除 otadata 后走此分支）

```c
// components/bootloader_support/src/bootloader_utility.c:378
int bootloader_utility_get_selected_boot_partition(const bootloader_state_t *bs)
{
    int boot_index = FACTORY_INDEX;
    if (bs->ota_info.offset == 0) {
        return FACTORY_INDEX;      // ← 无 otadata 分区 → 永远启动 factory
    }
    ...
}
```

### 9.3 配置区直访（新方案）

```c
// 不再用 esp_partition_find_first()
#define IAP_CFG_ADDR   0x008000
#define IAP_CFG_SIZE   0x008000

esp_err_t iap_config_init(void)
{
    // 直接读 flash，不依赖分区表
    esp_err_t err = esp_flash_read(NULL, buf, IAP_CFG_ADDR + slot_off, len);
    ...
}
```

---

## 10. 下一步

请审阅本方案，确认或修改以下内容：

1. **第 8 节的 6 个待确认问题**
2. **第 2.5 节的完整布局**（尤其 IAP 程序区位置）
3. **第 7.3 节**：是否采用完整重构（A）还是渐进方案（B/C）

确认后我将按第 6.2 节的顺序实施。
