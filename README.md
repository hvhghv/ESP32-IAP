# ESP IAP 程序

基于 ESP-IDF v6.0.3 的通用 IAP（In-Application Programming）程序，支持通过
**UART / I2C / WiFi** 三条通道对用户程序区进行烧录，兼容 **ESP32 / ESP32C6 / ESP32S3**
等 ESP 全系列芯片。

---

## 目录

- [1. 功能概览](#1-功能概览)
- [2. 闪存布局](#2-闪存布局)
- [3. 启动流程](#3-启动流程)
- [4. IAP 配置区](#4-iap-配置区)
- [5. 用户程序镜像](#5-用户程序镜像)
- [6. 等待触发源](#6-等待触发源)
- [7. 启动参数字符串](#7-启动参数字符串)
- [8. 烧录通道](#8-烧录通道)
  - [8.1 UART 终端](#81-uart-终端)
  - [8.2 I2C 从机](#82-i2c-从机)
  - [8.3 WiFi + HTTP](#83-wifi--http)
- [9. 编译与烧录](#9-编译与烧录)
  - [9.5 QEMU 仿真验证](#95-qemu-仿真验证)
- [9.6 出厂预置配置与单一镜像烧录](#96-出厂预置配置与单一镜像烧录)
- [9.7 浏览器配置与烧录工具](#97-浏览器配置与烧录工具)
- [9.8 烧录到设备（Web Serial）](#98-烧录到设备web-serial)
- [9.9 烧录用户程序](#99-烧录用户程序)
- [9.10 读取设备 Flash](#910-读取设备-flash)
- [9.11 自动化构建（GitHub Actions）](#911-自动化构建github-actions)
- [10. 用户程序集成](#10-用户程序集成)
  - [10.1 使用模板工程（推荐）](#101-使用模板工程推荐)
  - [10.2 手动集成](#102-手动集成)
  - [10.3 打包与烧录](#103-打包与烧录)
- [11. 源码结构](#11-源码结构)
- [12. 已知限制](#12-已知限制)

---

## 1. 功能概览

| 功能 | 说明 |
|------|------|
| 多通道烧录 | UART（XMODEM）、I2C（自定义分帧协议）、WiFi（HTTP 上传）、USB |
| 多芯片支持 | ESP32 / ESP32S2 / ESP32S3 / ESP32C3 / ESP32C5 / ESP32C6 / ESP32H2（**C2 不支持**） |
| 双分区表 | IAP 侧（表 A）+ APP 侧（表 B），互不干扰 |
| 配置区 | 硬编码地址 `0x8000`，双槽冗余 + 序号 + CRC，掉电安全 |
| 等待触发源 | GPIO0 (BOOT 键) / I2C 命令 / UART 命令 / WiFi 接入 / USB，任一命中即进入 IAP |
| 启动参数 | 配置区内置参数字符串，IAP 写入、用户程序读取 |
| 启动决策 | **RTC RAM**（`rtc_retain_mem_t.custom[]`），冷启动自动失效 |
| 多 OTA 槽 | 1~16 槽，支持 GPIO 电平选择启动槽 |
| 完整性校验 | 由 **bootloader 自校验**（magic / 段表 / SHA256 / chip_id），失败自动回落 IAP |
| 内置终端 | 系统信息、配置读写、XMODEM 收发、分区操作 |
| 内置 HTTP | Web 控制台、固件上传、分区下载、**配置区图形化编辑**（功能开关 / WiFi / UART / I2C / 触发 / OTA 槽 / 启动参数） |
| 安全回退 | 用户程序校验失败时 bootloader 自动回落 IAP 下载模式 |

---

## 2. 闪存布局

> **v5 双分区表方案**：IAP 侧与 APP 侧各有自己的分区表，互不干扰。
> 启动决策改放 **RTC RAM**（原 `iap_mark` flash 标志区已取消）。

### 2.1 固定区（IAP 侧，全硬编码 0x0 ~ 0x13FFFF）

| 偏移 | 大小 | 内容 | 说明 |
|------|------|------|------|
| `0x000000` | 32 KB | bootloader | 自定义，**不读分区表** |
| `0x008000` | 8 KB | `iap_cfg` | 配置区，**硬编码，不在分区表** |
| `0x00A000` | 4 KB | (保留) | 原 `iap_mark` 标志区，已取消 |
| `0x00B000` | 4 KB | **分区表 A** | IAP 用（`partitions_iap.csv`） |
| `0x00C000` | 12 KB | `nvs` | IAP 的 NVS（WiFi 用，名字必须为 `nvs`） |
| `0x00F000` | 4 KB | (保留) | |
| `0x010000` | 1216 KB | `iap` | IAP 程序区 (factory) |

```
0x000000  +------------------+
          |  bootloader      |  32KB
0x008000  +------------------+
          |  iap_cfg  (8KB)  |  <- 配置区 (硬编码)
0x00A000  +------------------+
          |  (保留)   (4KB)  |  <- 原 iap_mark
0x00B000  +------------------+
          |  分区表 A (4KB)  |  <- partitions_iap.csv
0x00C000  +------------------+
          |  nvs (12KB)      |
0x00F000  +------------------+
          |  (保留)   (4KB)  |
0x010000  +------------------+
          |  iap  (1216KB)   |  <- IAP 程序区 (factory)
0x13FFFF  +------------------+  <- 固定区末尾
```

> **v7 变更**：固定区从 2 MB 缩到 **1280 KB**，省下的 768 KB 全部给用户程序。
> IAP 实际占用 ~998 KB（C6），1216 KB 分区留有余量。

### 2.2 可变区（APP 侧，0x140000 起）

| 偏移 | 大小 | 内容 | 说明 |
|------|------|------|------|
| `0x140000` | 4 KB | **分区表 B** | 用户程序用（`partitions_user_app.csv`） |
| `0x141000` | 12 KB | `nvs` | 用户程序的 NVS（名字必须为 `nvs`） |
| `0x144000` | 48 KB | (空隙) | 使 `user_app` 64KB 对齐 |
| `0x150000` | ~2.69 MB | `user_app` | 用户程序区 (ota_0) |

```
0x140000  +------------------+
          |  分区表 B (4KB)  |  <- 属用户程序工程
0x141000  +------------------+
          |  nvs (12KB)      |
0x144000  +------------------+
          |  (空隙 48KB)     |
0x150000  +------------------+
          |  user_app        |  <- 用户程序 (ota_0), 2.69MB
0x400000  +------------------+
```

> **分区表 B 由用户程序工程维护**：`examples/user_app_template/partitions_user_app.csv`，
> `CONFIG_PARTITION_TABLE_OFFSET=0x140000`，随 `user.bin` 一起编译烧录。
> HTML 工具**只读取、不修改**它。

> **为什么 `iap_cfg` 不在分区表**：它是 **IAP 与 APP 共享的资源**，
> 由双方约定同一地址（`0x8000`）直接读写，不通过分区表查找 ——
> 避免两个分区表各自定义导致的不一致。

> **`nvs` 必须 ≥ 12 KB**：ESP-IDF 要求 `data/nvs` 分区
> 至少 `0x3000`（12 KB），否则 `gen_esp32part.py` 会拒绝。

> **`otadata` 不需要**：v5 的启动决策走 RTC RAM，不用 `esp_ota_set_boot_partition()`。

### 启动切换机制

利用 ESP-IDF 的 OTA 启动分区机制实现 IAP ↔ 用户程序切换：

- 进入用户程序：`esp_ota_set_boot_partition(user_app)` → `esp_restart()`
  - 向 `otadata` 写入“启动 ota_0”标记
- 返回 IAP：`esp_ota_set_boot_partition(iap)` → `esp_restart()`
  - `iap` 是 factory 分区，会**擦除 otadata**，回到默认启动 factory

因此 `user_app` 必须使用 `ota_0` 子类型，且必须有 `otadata` 分区。

### 2.4 适配不同 flash 容量 ⭐

**分区表 A（IAP 侧）与容量无关** —— 它只描述固定区布局
（`nvs` + `iap`），任何容量的模块都一样，已打进 `iap_side.bin`。

**分区表 B（APP 侧）随容量变化** —— 它描述可变区，`user_app` 大小
随 flash 容量自动最大化。由**用户程序工程**维护：

```bash
# 查看布局
python tools/gen_partitions.py --flash 8MB --info

# 生成分区表 B 的 CSV（供用户程序工程使用）
python tools/gen_partitions.py --flash 8MB --out-b partitions_app.csv

# 多 OTA 槽（分区表 B 生成 ota_0 ~ ota_N）
python tools/gen_partitions.py --flash 8MB --slots 3 --info
```

各容量的 `user_app` 大小（单槽）：

| Flash 容量 | user_app 分区 | 说明 |
|-----------|--------------|------|
| 4 MB | 2752 KB (0x2B0000) | 默认 |
| 8 MB | 6848 KB (0x6B0000) | |
| 16 MB | 15040 KB (0xEB0000) | |

多 OTA 槽时每槽大小 = 可用空间 / 槽数（对齐 64KB）：

| Flash | 1 槽 | 2 槽 | 4 槽 |
|-------|------|------|------|
| 4 MB | 2752 KB (0x2B0000) | 1344 KB (0x150000) | ❌ 640 KB < 1 MB 下限 |
| 8 MB | 6848 KB (0x6B0000) | 3392 KB (0x350000) | 1664 KB (0x1A0000) |
| 16 MB | 15040 KB (0xEB0000) | 7488 KB (0x750000) | 3712 KB (0x3A0000) |

> `gen_partitions.py` 会拒绝每槽 < 1 MB 的配置（如上表 4MB/4 槽）。

### 2.4.1 分区表修改规则 ⚠️

| 分区表 | 归属 | 允许修改 |
|--------|------|---------|
| **A** ([`partitions_iap.csv`](partitions_iap.csv)) | IAP 固定区 | 仅 `iap` 的 Size（一般不需要改） |
| **B** ([`examples/user_app_template/partitions_user_app.csv`](examples/user_app_template/partitions_user_app.csv)) | 用户程序工程 | ① `user_app` 的 Size　② 在 `user_app` 之后追加分区　③ 调整 OTA 槽数量 |

**`user_app` 起始地址（`0x150000`）之前的任何内容都禁止修改**：

| | 禁止的修改 |
|---|---|
| ❌ | 修改 `user_app` 的 Offset |
| ❌ | 修改 `user_app` 之前任何分区的 名称 / 类型 / 子类型 / 偏移 / 大小 |
| ❌ | 在 `user_app` 之前插入新分区 |
| ❌ | 删除 `user_app` 之前的任何分区 |

**原因**：IAP 固件里的地址常量是**写死的** ——
`0x8000`（配置区）、`0xB000`（分区表 A）、`0x10000`（IAP 程序）、
`0x140000`（分区表 B）、`0x150000`（用户程序区）。
改动这些偏移会导致 IAP 无法工作。

#### 为什么允许「在 user_app 之后追加分区」

用户程序若需要**小型文件系统**（LittleFS / SPIFFS / FAT），
可以在 `user_app` 之后追加一个 data 分区：

```csv
# 4MB flash 示例：缩小 user_app，追加 storage 分区
user_app,   app,     ota_0,   0x150000, 0x2A0000,
storage,    data,    spiffs,  0x3F0000, 0x10000,
```

要点：
1. 缩小 `user_app` 的 Size 腾出空间
2. 新分区偏移必须**紧接 `user_app` 末尾**（不能留空洞）
3. 新分区大小按 4KB（`0x1000`）扇区对齐
4. **只需改分区表 B** —— IAP 侧不关心用户程序的文件系统分区

**烧录方式（v6）**：用户程序打包为**从 `0x140000` 开始的单一文件**，
一次写入即可（含分区表 B）：

```bash
# 1. 打包（合并分区表 B）
cd examples/user_app_template
python build_user_app.py --target esp32c6
# → user_app_template_flash.bin (约 216 KB)

# 2. 烧录（单一文件）
esptool.py --chip esp32c6 -p COM18 write_flash \
    0x140000 user_app_template_flash.bin
```

> ⚠️ **不要用 `idf.py flash`** —— 它会把用户程序工程自己的 bootloader
> 写到 `0x0`，**覆盖 IAP 侧的 bootloader**。

> ✅ **v6 起用户程序只有一个文件**（`0x140000` 起，含分区表 B + 应用镜像），
> IAP 会整段写入，与 esptool 语义一致。

**安全保证**：若分区表与设备实际 flash 容量不匹配，**bootloader 会拒绝启动**


## 3. 启动流程

```
                    ┌─────────────┐
                    │  上电/复位  │
                    └──────┬──────┘
                           ▼
                 ┌─────────────────────┐
                 │ 初始化 NVS / 配置区 │
                 └──────────┬──────────┘
                            ▼
              ┌─────────────────────────────┐
              │ 配置区下载位被置位?         │
              └──────┬───────────────┬──────┘
                     │ 是            │ 否
                     ▼               ▼
              ┌────────────┐  ┌──────────────────────┐
              │ 进入 IAP   │  │ 等待值 n > 0 ?       │
              │ 下载模式   │  └────┬────────────┬────┘
              └────────────┘       │ 是         │ 否
                                   ▼            │
                        ┌──────────────────┐    │
                        │ 等待 n 秒        │    │
                        │ 期间有操作?      │    │
                        └───┬──────────┬───┘    │
                            │ 有       │ 无     │
                            ▼          ▼        ▼
                     ┌────────────┐  ┌───────────────────────┐
                     │ 进入 IAP   │  │ 用户程序区有镜像?     │
                     │ 下载模式   │  │ (首字节 == 0xE9)      │
                     └────────────┘  └────┬────────────┬─────┘
                                          │ 是         │ 否
                                          ▼            ▼
                              ┌───────────────────┐  ┌────────────┐
                              │ 切换启动分区      │  │ 进入 IAP   │
                              │ 交由 bootloader   │  │ 下载模式   │
                              │ 自校验并启动      │  └────────────┘
                              └───────────────────┘
```

**进入 IAP 的原因**会记录在配置区 `last_boot_reason` 字段，可通过终端或 HTTP 查询：

| 值 | 含义 |
|----|------|
| 0 | 未知 |
| 1 | 配置区下载位置位 |
| 2 | 等待超时无操作 |
| 3 | 用户程序主动请求 |
| 4 | 启动用户程序失败 |
| 5 | 用户程序区无有效程序 |
| 6 | 首次上电 |
| 7 | GPIO 引脚电平触发 |
| 8 | I2C 进入命令触发 |
| 9 | UART 进入命令触发 |
| 10 | WiFi 客户端接入触发 |
| 11 | GPIO0 电平触发（原 flash 魔术标记已取消） |
| 12 | 配置区错误 |

> **等待期间的"操作"检测**：等待阶段仅 UART 已初始化，因此通过**串口发送任意字符**
> 即可中断等待并进入 IAP 下载模式。I2C 与 WiFi 在等待阶段不启动。

### 3.x 救援进入 IAP（v5：已取消 flash 标志区）

> **v5 变更**：原 `iap_mark` flash 标志区**已取消**。
> 启动决策改放 **RTC RAM**（`rtc_retain_mem_t.custom[]`），
> 不再需要任何 flash 写入即可切换启动目标。

**四种救援方式**（按优先级）：

| # | 方式 | 操作 | 适用场景 |
|---|------|------|---------|
| A | **GPIO0 电平** | 上电时按住 **BOOT 键**（GPIO0 接地） | 设备能上电，无需工具 |
| B | **UART 命令** | 等待窗口内向串口发 `iap` 字符串 | 有串口连接 |
| C | **用户程序请求** | 用户程序写 RTC RAM `boot_target=0` → `esp_restart()` | 程序主动升级 |
| D | **断电重上电** | 冷启动 RTC RAM 无效 → 默认进 IAP | 兜底 |

**为什么取消 flash 标志区**：

| 问题 | 说明 |
|------|------|
| 原子性 | 「标志区 + 配置」若同扇区会有擦写冲突 |
| 额外写入 | 每次切换都要擦写 flash，寿命消耗 |
| 分区表依赖 | 需要 `iap_mark` 分区，双分区表下难以协调 |

**RTC RAM 方案**（`main/iap_boot_param.h`）：

```c
/* 100 字节，放在 LP RAM 末尾保留区 (rtc_retain_mem_t.custom[]) */
typedef struct {
    uint32_t magic;        /* "IAPB" */
    uint32_t version;
    uint32_t crc32;
    uint8_t  boot_target;  /* 0=IAP, 1=APP */
    ...
} iap_boot_param_t;
```

- bootloader 侧：`bootloader_common_get_rtc_retain_mem()->custom`
- APP 侧：链接段 `.bootloader_data_rtc_mem`
- **冷启动自动失效**（RTC RAM 掉电丢失）→ 天然兜底

**配置 GPIO0 触发**（默认已启用）：

```bash
# UART 终端
iap> cfg trig_gpio 0
iap> cfg trig_level 0     # 低电平触发
```

或 HTML 工具「触发引脚」设 `GPIO = 0`、`触发方式 = 上拉，低电平触发`。

---


## 4. IAP 配置区

### 4.1 存储结构

配置区位于 `iap_cfg` 分区（8 KB），采用**双槽冗余**设计：

```
偏移 0x000                 偏移 0x1000
+------------------+       +------------------+
|  槽 A (4096 B)   |       |  槽 B (4096 B)   |
|  header (32 B)   |       |  header (32 B)   |
|  data (352 B)    |       |  data (352 B)    |
|  0xFF 填充       |       |  0xFF 填充       |
+------------------+       +------------------+
```

> 槽大小为 4096 字节（一个 flash 扇区），以满足 `esp_partition_erase_range()`
> 对擦除长度必须是扇区大小整数倍的要求。

- **写入**：总是写入与当前有效槽不同的另一个槽，并将 `seq` 加 1
- **读取**：选择 CRC 校验通过且 `seq` 最大的槽
- **掉电安全**：写入过程中掉电，旧槽仍完好，下次启动可回退

### 4.1.1 配置数据版本与迁移 ⭐

配置有两个独立的版本号：

| 版本号 | 位置 | 含义 |
|--------|------|------|
| `IAP_CFG_VERSION` | 槽头部 | **头部格式**版本（magic/version/seq/crc 的布局） |
| `IAP_CFG_DATA_VER` | 数据区 `data_ver` 字段 | **数据内容**版本（字段语义） |

**头部格式不变但字段增删时，只需递增 `IAP_CFG_DATA_VER`。**

#### 迁移策略

旧固件写入的配置（`data_ver` 较小）会被新固件**自动迁移**：

```
1. 以「默认配置」为基准      ← 保证新增字段有合理初值
2. 逐项合并旧配置中仍然存在的字段
3. 已删除的字段自然被忽略   ← 默认值保留
4. 冲突/越界值由 sanitize() 修正
5. 更新 data_ver 并写回     ← 下次启动不再迁移
```

**迁移历史**：

| data_ver | 变更 |
|----------|------|
| `0x0000` | 旧版（无 `data_ver` 字段） |
| `0x0001` | 初版 |
| `0x0002` | 新增多 OTA 槽（`active_slot` / `ota_slot_count` / `ota_gpio`） |

#### 迁移日志示例

```
W iap_cfg:  配置数据版本迁移: 0 -> 2
I iap_cfg:   新增字段使用默认值: active_slot=0, ota_slot_count=1, ota_gpio=0xFF
I iap_cfg: 迁移完成，新版本 2
I iap_cfg: 迁移后配置已写回槽 1 (seq=2)
```

#### 手动迁移

```
iap> cfg                    ← 查看当前版本 (会提示"需要迁移")
iap> cfg migrate            ← 手动触发迁移
```

#### 配置区损坏时的保护

若配置区**读取失败或无法修复**（分区缺失 / CRC 全错）：

- **不卡死** —— 用默认配置继续启动
- **强制进入 IAP 下载模式** —— 避免用错误配置启动用户程序
- 启动原因记录为 `配置区错误`

```
E iap_main:  配置区初始化失败: ESP_ERR_INVALID_CRC
E iap_main:  将使用默认配置进入 IAP 下载模式
W iap_main:  原因: 配置区错误
```

### 4.2 配置字段

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `flags` | u32 | `0x79E` | 标志位，见下表 |
| `wait_seconds` | u16 | 3 | 启动等待秒数，0 表示不等待 |
| `user_app_size` | u32 | 0 | 用户程序镜像长度（字节） |
| `user_app_crc32` | u32 | 0 | 用户程序镜像 CRC32（由用户程序上报） |
| `user_app_version` | u32 | 0 | 用户程序版本 |
| `boot_count` | u32 | 0 | 用户程序启动计数 |
| `last_boot_reason` | u32 | 6 | 上次进入 IAP 的原因 |
| `i2c_scl_gpio` | u8 | 7 | I2C SCL 引脚 |
| `i2c_sda_gpio` | u8 | 6 | I2C SDA 引脚 |
| `i2c_addr` | u8 | 0x42 | I2C 从机地址（7 bit） |
| `i2c_reserved_freq` | u32 | 0 | 占位（原 `i2c_freq_hz`，从机模式无效） |
| `wifi_ssid` | u8[32] | `"ESP-IAP"` | AP SSID |
| `wifi_password` | u8[64] | `"12345678"` | AP 密码（< 8 字符则为开放网络） |
| `wifi_channel` | u8 | 1 | AP 信道（1~13） |
| `wifi_ip` | u32 | 192.168.4.1 | AP IPv4（小端） |
| `wifi_netmask` | u32 | 255.255.255.0 | AP 掩码（小端） |
| `uart_port` | u8 | 0 | UART 端口号（0/1/2） |
| `uart_tx_gpio` | u8 | `0xFF` | TX 引脚；`0xFF` = 用该端口默认引脚 |
| `uart_rx_gpio` | u8 | `0xFF` | RX 引脚；`0xFF` = 用该端口默认引脚 |
| `uart_baudrate` | u32 | 115200 | UART 波特率 |
| `trig_gpio` | u8 | 0xFF | **等待触发引脚**（0xFF = 未配置） |
| `trig_gpio_level` | u8 | 0 | 触发有效电平：0=低电平，1=高电平 |
| `boot_param` | u8[256] | `""` | **启动参数字符串**（见第 6 节） |
| `boot_param_len` | u16 | 0 | 有效字符数（不含 `\0`） |
| `boot_param_flags` | u8 | 0 | 启动参数标志（VALID / CONSUMED） |
| `boot_param_crc32` | u32 | 0 | `boot_param[0..len)` 的 CRC32 |
| `active_slot` | u8 | 0 | **当前活动 OTA 槽**（0 = `user_app`） |
| `ota_slot_count` | u8 | 1 | 已发现的 OTA 槽数量（启动时扫描填充） |
| `ota_gpio` | u8 | `0xFF` | **GPIO 选择 OTA 槽**的引脚（`0xFF` = 禁用） |
| `data_ver` | u16 | 2 | **配置数据版本**（用于跨版本迁移，见 4.1.1） |

**标志位**（默认 `0x79E` = bit1|2|3|4|7|8|9|10）：

| 位 | 名称 | 默认 | 说明 |
|----|------|:----:|------|
| 0 | `DOWNLOAD_MODE` | ⬜ | 置位则强制进入 IAP 下载模式 |
| 1 | `WIFI_ENABLE` | ✅ | 使能 WiFi AP 与 HTTP 服务 |
| 2 | `I2C_ENABLE` | ✅ | 使能 I2C 从机 |
| 3 | `UART_ENABLE` | ✅ | 使能 UART 终端 |
| 4 | `VERIFY_USER_APP` | ✅ | 启动时校验用户程序（由 bootloader 执行） |
| 5 | `WAIT_GPIO_TRIG` | ⬜ | 等待期间引脚为有效电平则进入 IAP |
| 6 | `WAIT_I2C_TRIG` | ⬜ | 等待期间收到 I2C 进入命令则进入 IAP |
| 7 | `WAIT_UART_TRIG` | ✅ | 等待期间收到 UART 进入命令则进入 IAP |
| 8 | `WAIT_WIFI_TRIG` | ✅ | 等待期间有 WiFi 客户端接入则进入 IAP |
| 9 | `USB_ENABLE` | ✅ | 使能 USB 串口终端与烧录（仅 C3/C6/H2/S3） |
| 10 | `WAIT_USB_TRIG` | ✅ | 等待期间收到 USB 进入命令则进入 IAP |

> **USB 标志位说明**：bit9/bit10 仅在支持 USB-Serial-JTAG 的芯片上生效
> （ESP32-C3/C5/C6/C61/H2/S3）。其他芯片上固件会自动清除这两个标志。
> 默认开启 UART 与 WiFi 触发，便于现场通过串口输入 `iap` 或连上 AP 直接进入下载模式。
> GPIO / I2C 触发默认关闭（GPIO 引脚默认未配置，I2C 触发需主机主动发命令）。

**启动参数标志位** (`boot_param_flags`)：

| 位 | 名称 | 说明 |
|----|------|------|
| 0 | `VALID` | 启动参数有效（已由 IAP 写入且 CRC 正确） |
| 1 | `CONSUMED` | 已被用户程序读取（用于"仅首次生效"语义） |

### 4.3 修改配置

三种方式，效果等价：

```bash
# 1. UART 终端
iap> cfg set ssid MyDevice
iap> cfg set pass 12345678
iap> cfg set ip 192.168.10.1
iap> cfg set wait 5

# 2. HTTP (查询字符串格式)
curl -X POST "http://192.168.4.1/api/cfg" -d "ssid=MyDevice&wait=5"

# 3. I2C 命令 SET_CFG_INFO (见 7.2 节)
```

---

## 5. 用户程序镜像

### 5.1 分区内布局

`user_app` 分区内就是**纯 ESP 应用镜像**（无任何附加数据）：

```
user_app 分区 (2752KB)
+---------------------------+ offset 0
|  ESP 应用镜像 (magic 0xE9)|  ← bootloader / OTA 直接可读
+---------------------------+ offset image_size
|  0xFF 填充 (未使用)       |
+---------------------------+ offset size
```

**镜像合法性完全交给 bootloader 自校验**（magic / 段表 / SHA256 / chip_id）。
校验失败时 bootloader 会自动回落到 factory (IAP)，**不会变砖**。

### 5.2 为什么不用自定义文件头

早期版本在镜像前附加 512 字节自定义文件头（magic `"USER"`），但存在
**阻断性缺陷**：

```
esp_ota_set_boot_partition()
  └─ image_validate()
       └─ esp_image_verify()  → 要求分区 offset 0 == 0xE9
```

自定义文件头占据 offset 0 → OTA 校验必然返回
`ESP_ERR_OTA_VALIDATE_FAILED` → **用户程序永远无法启动**。

而且 ESP 镜像的 segment 表用**相对偏移**串接，镜像必须从 offset 0
连续存放，中途无法插入任何字节。

因此改为**完全依赖 ESP-IDF 原生机制**：

| 项 | 做法 |
|----|------|
| 镜像合法性 | bootloader 自校验（magic / 段表 / SHA256 / chip_id） |
| 校验失败 | bootloader 自动回落 factory (IAP) |
| 版本号等元数据 | 存**配置区** `user_app_version`，由 IAP 侧维护（用户程序**不访问配置区**） |
| 打包 | **不需要**，`idf.py build` 产物直接烧 |

**额外好处**：分区内无位置敏感数据，**换 flash 容量 / 改分区大小零改动**。

### 5.3 用户程序如何获取启动参数

⚠️ **v5: 用户程序不允许访问配置区 (`0x8000`)**。

配置区位于 IDF 的 flash 写保护区 (`0x0 ~ 0xBFFF`) 内，用户程序直接写会
触发 `abort()`；且配置区是 IAP 私有数据，读写会破坏双槽冗余一致性。

用户程序通过 **RTC RAM 启动参数** 获取信息：

```c
#include "iap_user_api.h"

void app_main(void)
{
    /* 1. 读取启动参数字符串 (IAP 启动前写入 RTC RAM) */
    char param[IAP_USER_BOOT_PARAM_SIZE];
    if (iap_user_get_boot_param(param, sizeof(param)) == ESP_OK) {
        ESP_LOGI(TAG, "启动参数: %s", param);
    }

    /* 2. 读取自身加载信息 (地址/大小/版本) */
    iap_user_boot_param_t bp;
    if (iap_user_boot_param_read(&bp) == ESP_OK) {
        ESP_LOGI(TAG, "加载地址=0x%08" PRIX32 " 大小=%" PRIu32,
                 bp.user_addr, bp.user_size);
    }

    /* 3. 需要升级时: 写 RTC RAM → 重启进 IAP */
    // iap_user_request_download();

    /* ... 业务逻辑 ... */
}
```

**已删除的 API**（v5 起不再提供）：

| 旧 API | 说明 |
|--------|------|
| `iap_user_config_read()` | 读配置区 → 改用 `iap_user_get_boot_param()` |
| `iap_user_config_write()` | 写配置区 → 改用 `iap_user_request_apply()` (v6) |
| `iap_user_cfg_addr()` | 取配置区地址 → 不再暴露 |
| `iap_user_report_boot_ok()` | 写配置区启动计数 → IAP 侧自行维护 |
| `iap_user_report_version()` | 写配置区版本号 → IAP 侧自行维护 |
| `iap_user_cfg_t` | 配置结构体 → 已移除 |

**间接修改配置**（v6 起）：

```c
/* 用户程序侧：填写要修改的字段，然后提交并重启 */
iap_user_request_boot_param("mode=debug;server=192.168.1.10", 0);
iap_user_request_active_slot(1);
iap_user_request_apply();   /* 不会返回 */
```

IAP 启动时检测到请求后写入配置区持久化，再重启启动用户程序。
详见 §7.6.1。

### 5.4 烧录方式

⚠️ **v6: 用户程序必须打包为单一文件**（从 `0x140000` 开始，含分区表 B）。

```bash
# 1. 编译 + 打包（合并分区表 B）
cd my_user_app
python build_user_app.py --target esp32c6
# → user_app_template_flash.bin (约 216 KB, 从 0x140000 开始)

# 2. 通过 IAP 烧录（XMODEM / HTTP / 浏览器）
#    IAP 会整段写入 0x140000

# 3. 或用 esptool 直接烧
esptool.py --chip <chip> write_flash 0x140000 user_app_template_flash.bin
```

**为什么必须打包**：`user_app` 分区定义在**分区表 B**（`0x140000`），
而 IAP 运行在**分区表 A**（`0xB000`）下，`esp_partition_find_first()`
看不到表 B。打包后 IAP 直接整段写入，与 esptool 语义一致。

> **注意**：`esptool` 直烧后 IAP 的 `app info` 不会显示版本号
> （因为用户程序还没运行过、没上报）。启动一次后即可。

---

## 6. 等待触发源

在启动等待窗口内，IAP 可同时监听多个触发源。**任一使能的触发源命中，即中止等待并进入 IAP 下载模式。**

### 6.1 四个触发源

| 触发源 | 配置标志 | 触发条件 | 说明 |
|--------|----------|----------|------|
| **GPIO 引脚** | `WAIT_GPIO_TRIG` | 引脚电平等于配置的有效电平 | 等待开始前配置为输入并启用内部上/下拉 |
| **I2C 命令** | `WAIT_I2C_TRIG` | 收到 `BOOT_IAP` 命令 | 等待期间启动 I2C 从机 |
| **UART 命令** | `WAIT_UART_TRIG` | 收到 `iap` 回车 或 任意控制字符 | 等待期间监听串口 |
| **WiFi 接入** | `WAIT_WIFI_TRIG` | 有客户端接入 AP | 等待期间启动 WiFi AP |

### 6.2 GPIO 触发

配置两个字段：

| 字段 | 说明 |
|------|------|
| `trig_gpio` | 引脚号，`0xFF` 表示未配置（此时自动清除使能标志） |
| `trig_gpio_level` | 有效电平：`0` = 低电平有效，`1` = 高电平有效 |

**内部上/下拉自动配置**：

| 有效电平 | 内部上下拉 | 默认状态 | 触发方式 |
|----------|-----------|----------|----------|
| 低电平有效 (`0`) | **上拉** | 高 | 接地拉低触发 |
| 高电平有效 (`1`) | **下拉** | 低 | 接高电平触发 |

```
        VCC                     VCC
         │                       │
         ╱ 内部上拉              ╱ 外部上拉
         │                       │
   GPIO ─┼───┬─── 按钮 ─── GND   GPIO ───┬─── 信号源
         │   │                            │
       输入  低电平有效                   下拉(内部)
```

**行为**：等待开始前即完成引脚配置。若引脚在等待开始前就已是有效电平，立即触发。

配置示例：

```bash
iap> cfg set trig 5              # 使用 GPIO5，并使能 GPIO 触发
iap> cfg set triglevel low       # 低电平有效 (内部上拉)
iap> cfg set triglevel high      # 高电平有效 (内部下拉)
iap> cfg set trig off            # 禁用 GPIO 触发
```

### 6.3 I2C 触发

等待期间启动 I2C 从机，监听 `BOOT_IAP` 命令 (`TYPE = 0x0031`)。
主机发送该命令后，IAP 立即进入下载模式。

> 注意：`BOOT_IAP` 命令在正常 IAP 模式下会直接重启到 IAP 分区；
> 在等待窗口内则只中止等待，由主流程接管（避免重复重启）。

### 6.4 UART 触发

等待期间监听串口，识别两种输入：

| 输入 | 说明 |
|------|------|
| `iap` + 回车 | 推荐的显式命令（不区分大小写） |
| 任意控制字符（如 `Ctrl+C`） | 兼容旧行为，便于快速中断 |

```
iap> iap                      ← 输入 iap 回车，进入 IAP 下载模式
```

非命令的普通输入会被忽略，不会误触发。

### 6.5 WiFi 触发

等待期间启动 WiFi AP，监听 `WIFI_EVENT_AP_STACONNECTED` 事件。
任何客户端接入 AP 即触发进入 IAP 下载模式。

> WiFi 启动采用**非阻塞模式**：不等待 AP 就绪即返回，
> 避免射频初始化与 PHY 校准占用等待窗口。
> 客户端接入事件仍会正常上报。

### 6.6 配置方式

四种触发源可独立使能，互不影响：

```bash
# UART 终端
iap> cfg set trig_gpio on        # 使能 GPIO 触发
iap> cfg set trig_i2c on         # 使能 I2C 触发
iap> cfg set trig_uart on        # 使能 UART 触发
iap> cfg set trig_wifi on        # 使能 WiFi 触发
iap> cfg                          # 查看当前状态

# HTTP
curl -X POST -d "trig_uart=on&trig_wifi=on" http://192.168.4.1/api/cfg
```

**典型组合**：

| 场景 | 建议配置 |
|------|----------|
| 产线烧录 | GPIO 触发（接烧录夹具）+ UART 触发 |
| 远程升级 | WiFi 触发 + I2C 触发（主机控制） |
| 调试 | UART 触发（最方便） |

### 6.7 进入原因记录

触发进入 IAP 后，原因会记录在配置区 `last_boot_reason`：

| 值 | 含义 |
|----|------|
| 7 | GPIO 引脚电平触发 |
| 8 | I2C 进入命令触发 |
| 9 | UART 进入命令触发 |
| 10 | WiFi 客户端接入触发 |

可通过 `cfg` 命令的「上次原因」字段或 HTTP `/api/cfg` 查询。

### 6.8 实现说明

触发检测运行在独立任务中，轮询周期 50ms。I2C/WiFi 的启动放在**另一个独立任务**，
避免其较长的初始化时间（WiFi 射频初始化 + PHY 校准）阻塞轮询：

```
app_main                    检测任务              子系统任务
   │                            │                     │
   ├─ 配置 GPIO 引脚             │                     │
   ├─ 创建检测任务 ──────────────>│                     │
   │                            ├─ 创建子系统任务 ─────>│
   ├─ 等待通知 (阻塞)            │                     ├─ 启动 I2C
   │                            ├─ 轮询 GPIO/UART      ├─ 启动 WiFi
   │                            │  (每 50ms)           │
   │                            │                     │
   │                            │  ← I2C/WiFi 事件置位 fired_mask
   │                            │                     │
   │<──── 通知 ─────────────────┤                     │
   ├─ 读取触发原因               │                     │
   └─ 进入 IAP 下载模式          └─ 退出                └─ 退出
```

---

## 7. 启动参数字符串

启动参数字符串用于 **IAP 与用户程序之间的参数传递**：IAP 在启动用户程序前写入，
用户程序启动后读取该字符串完成初始化。

### 7.1 数据流

```
        ┌──────────────┐                        ┌──────────────┐
        │ IAP 程序     │                        │ 用户程序     │
        └──────┬───────┘                        └──────┬───────┘
               │                                       │
   1. 检查用户程序区存在镜像                            │
               │                                       │
   2. 组装启动参数                                      │
      "reason=0;boot=5;mode=normal"                     │
               │                                       │
   3. iap_config_set_boot_param()                       │
               │                                       │
               ▼                                       │
        ┌─────────────────────────┐                    │
        │  iap_cfg 分区           │                    │
        │  boot_param[256]        │                    │
        │  boot_param_len         │                    │
        │  boot_param_flags=VALID │                    │
        │  boot_param_crc32       │                    │
        └─────────────────────────┘                    │
               │                                       │
   4. esp_ota_set_boot_partition(user_app)             │
      esp_restart()                                    │
               │                                       │
               └──────────── 重启 ─────────────────────┤
                                                       │
                                        5. iap_user_get_boot_param()
                                           iap_user_boot_param_get_value()
                                                       │
                                        6. 根据参数初始化
```

### 7.2 字段说明

| 字段 | 长度 | 说明 |
|------|------|------|
| `boot_param` | 256 字节 | 参数字符串，以 `\0` 结尾 |
| `boot_param_len` | 2 字节 | 有效字符数（不含 `\0`），上限 255 |
| `boot_param_flags` | 1 字节 | bit0 = VALID（有效），bit1 = CONSUMED（已消费） |
| `boot_param_crc32` | 4 字节 | `boot_param[0..len)` 的 CRC32 |

> 读取时会校验 `boot_param_crc32`，不匹配则返回 `ESP_ERR_INVALID_CRC`，
> 避免参数被写坏后导致用户程序异常初始化。

### 7.3 参数格式约定

格式为 `key=value` 对，以 `;` 分隔：

```
key1=value1;key2=value2;key3=value3
```

约定规则：

- 键值对之间用 `;` 分隔
- 键与值之间用 `=` 连接
- 键与值首尾的空白会被自动忽略
- 值中不能包含 `;`（如需包含，请自行约定转义方式）

**IAP 默认写入的键**：

| 键 | 说明 | 示例 |
|----|------|------|
| `reason` | 进入用户程序的原因（对应 `iap_boot_reason_t`） | `reason=0` |
| `boot` | 用户程序启动计数 | `boot=5` |
| `mode` | 运行模式 | `mode=normal` |

用户可以自行扩展任意键，例如 `ssid`、`server`、`baud` 等。

### 7.4 写入启动参数

**方式一：IAP 自动写入**（默认行为）

IAP 在启动用户程序前自动写入：

```c
/* main.c 中 iap_main_write_boot_param() */
snprintf(param, sizeof(param),
         "reason=%u;boot=%" PRIu32 ";mode=normal",
         (unsigned)reason, cfg.boot_count);
iap_config_set_boot_param(param);
```

**方式二：UART 终端**

```bash
iap> bootparam set mode=debug;server=192.168.4.100:8080
iap> bootparam                    # 查看当前参数
iap> bootparam get mode           # 提取单个键
iap> bootparam clear              # 清除
```

**方式三：HTTP**

```bash
# 设置（请求体为参数字符串原文）
curl -X POST -d "mode=debug;baud=115200" http://192.168.4.1/api/bootparam

# 查询
curl http://192.168.4.1/api/bootparam
# -> {"param":"mode=debug;baud=115200","length":22,"consumed":"否"}

# 清除
curl -X DELETE http://192.168.4.1/api/bootparam
```

**方式四：I2C**

| TYPE | 名称 | CONTEXT（请求） | CONTEXT（响应） |
|------|------|-----------------|-----------------|
| `0x0006` | `GET_BOOT_PARAM` | 无 | `flags(1)` `length(2)` + 参数字符串 |
| `0x0007` | `SET_BOOT_PARAM` | 参数字符串（空则清除） | 无 |

### 7.5 读取启动参数（用户程序侧）

```c
#include "iap_user_api.h"

void app_main(void)
{
    char param[IAP_USER_BOOT_PARAM_SIZE];
    esp_err_t err = iap_user_get_boot_param(param, sizeof(param));

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "启动参数: %s", param);

        /* 提取单个键的值 */
        char val[32];

        if (iap_user_boot_param_get_value(param, "mode", val, sizeof(val)) == ESP_OK) {
            if (strcmp(val, "debug") == 0) {
                /* 调试模式初始化 */
            }
        }

        if (iap_user_boot_param_get_value(param, "server", val, sizeof(val)) == ESP_OK) {
            /* 使用 server 地址连接上位机 */
        }

    } else if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "无启动参数，使用默认配置");
    } else {
        ESP_LOGW(TAG, "读取启动参数失败: %s", esp_err_to_name(err));
    }
}
```

**API 一览**：

| 函数 | 说明 |
|------|------|
| `iap_user_get_boot_param(buf, len)` | 读取启动参数（来自 RTC RAM） |
| `iap_user_take_boot_param(buf, len)` | 同 `get`（v5 无需回写消费标志） |
| `iap_user_boot_param_get_value(param, key, out, out_len)` | 提取指定键的值 |
| `iap_user_has_boot_param()` | 查询参数是否有效 |
| `iap_user_boot_param_consumed()` | 保留接口，v5 始终返回 false |
| `iap_user_boot_param_read(out)` | 读取 RTC RAM 启动参数结构体 |
| `iap_user_request_download()` | 写 RTC RAM → 重启进 IAP 下载模式 |
| `iap_user_request_boot_param(s, len)` | 请求修改启动参数 (v6) |
| `iap_user_request_active_slot(slot)` | 请求修改 OTA 槽序号 (v6) |
| `iap_user_request_user_addr(addr)` | 请求修改加载地址 (v6) |
| `iap_user_request_boot_target(t)` | 请求修改启动目标 (v6) |
| `iap_user_request_commit()` | 提交请求（重算 CRC，不重启）(v6) |
| `iap_user_request_apply()` | 提交 + 重启进 IAP 应用配置 (v6) |
| `iap_user_has_update_request()` | 查询是否有待处理请求 (v6) |
| `iap_user_request_clear()` | 放弃更新请求 (v6) |

### 7.6 参数传递方式 (v5)

启动参数由 IAP 在跳转前写入 **RTC RAM**（`iap_boot_param_t.param`），
用户程序通过 `iap_user_get_boot_param()` 读取。

不再使用配置区传递（配置区对用户程序不开放）。RTC RAM 中的参数由 IAP
每次启动重写，因此**无需**用户程序回写消费标志。

### 7.6.1 用户程序间接修改配置 (v6)

用户程序**不能直接写配置区**，但可通过 RTC RAM 的 **update 区** 请求 IAP 代为修改：

```
用户程序                                IAP
   |                                     |
   | 1. iap_user_request_*()             |
   |    填写 RTC RAM update 区            |
   | 2. iap_user_request_apply()         |
   |    提交 + esp_restart()  ----------> bootloader 读 RTC RAM → 启动 IAP
   |                                     |
   |                                     | 3. iap_main_apply_update_request()
   |                                     |    检测 update_flags
   |                                     |    写入配置区持久化
   |                                     |    清除请求 + esp_restart()
   | <---- bootloader 启动用户程序 -------- |
```

**可修改字段白名单**（`IAP_PARAM_UPD_ALLOWED_MASK`）：

| 位 | 字段 | IAP 侧校验 |
|----|------|------------|
| `UPD_BOOT_PARAM` | 启动参数字符串 | 长度 <= 255，超长则拒绝 |
| `UPD_ACTIVE_SLOT` | OTA 槽序号 | 必须 < 实际槽数 |
| `UPD_USER_ADDR` | 加载地址 | 必须为 0 或 >= `IAP_FIXED_REGION_END` |
| `UPD_BOOT_TARGET` | 启动目标 | 必须为 IAP / APP |

IAP 启动时在**配置区初始化之后、启动决策之前**处理请求
（`main.c` 步骤 4.5），处理完立即重启应用新配置。

> 加载地址覆盖持久化到配置区 `ota_reserved1[0]`（0 = 用分区表地址），
> `iap_image_boot_slot()` 启动时读取并应用。

### 7.7 结构体兼容性 (v5/v6)

启动参数字符串通过 **RTC RAM** 的 `iap_boot_param_t` 传递，两侧布局必须一致：

| 侧 | 结构体 | 大小 |
|----|--------|------|
| IAP / bootloader | `iap_boot_param_t`（`iap_boot_param.h`） | 124 字节 |
| 用户程序 | `iap_user_boot_param_t`（`iap_user_api.h`） | 124 字节 |

两个头文件均包含编译期断言：

```c
_Static_assert(sizeof(iap_boot_param_t) <= 128, "...");       /* IAP 侧 */
_Static_assert(sizeof(iap_user_boot_param_t) <= 128, "...");  /* 用户程序侧 */
```

上限 128 字节来自 `CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE`。
若修改了任一侧的字段，编译会立即失败并提示同步。

> ⚠️ v5 起**用户程序不再定义配置结构体**。早期版本要求用户程序复制
> `iap_cfg_data_t` 为 `iap_user_cfg_t`（492 字节）以直读配置区，
> 现已全部移除 —— 配置区对用户程序不开放。

---

## 8. 烧录通道

### 8.1 UART 终端

终端默认与日志共用 **UART0**（115200 8N1），即 ESP 系列默认日志输出口。

#### UART 端口与引脚配置

端口与引脚均可通过配置区修改：

| 字段 | 默认 | 说明 |
|------|:----:|------|
| `uart_port` | `0` | UART 端口号（0/1/2） |
| `uart_tx_gpio` | `0xFF` | TX 引脚；`0xFF` = 用该端口默认引脚 |
| `uart_rx_gpio` | `0xFF` | RX 引脚；`0xFF` = 用该端口默认引脚 |

**各芯片 UART0 默认引脚**（即默认日志口）：

| 芯片 | TX | RX |
|------|:--:|:--:|
| ESP32 | GPIO1 | GPIO3 |
| ESP32-S2 / S3 | GPIO43 | GPIO44 |
| ESP32-C2 / C3 / C6 | GPIO21 | GPIO20 |
| ESP32-H2 | GPIO24 | GPIO23 |

**常见用法**：

```bash
# 1. 默认（UART0 + 芯片默认引脚，与 idf.py monitor 共用）
iap> cfg set uartport 0
iap> cfg set uarttx default
iap> cfg set uartrx default

# 2. 换到 UART1 并指定引脚（UART1/2 无默认引脚，必须显式指定）
iap> cfg set uartport 1
iap> cfg set uarttx 17
iap> cfg set uartrx 16

# 3. 仍用 UART0，但复用到其他 IO
iap> cfg set uartport 0
iap> cfg set uarttx 5
iap> cfg set uartrx 4
```

> **注意**：
> - UART1/2 在部分芯片上**没有默认引脚映射**，必须显式指定 TX/RX，
>   否则终端无法收发（启动日志会打印警告）。
> - 改到非 UART0 后，日志输出仍在 UART0，终端交互在新端口上。
> - 改引脚后需 `reboot` 生效。

**命令列表**：

| 命令 | 说明 |
|------|------|
| `help` | 显示帮助 |
| `info` | 系统信息（芯片、Flash、MAC、堆、运行时间） |
| `part` | 分区表与 flash 容量一致性 |
| `cfg` | 显示 IAP 配置 |
| `cfg set <key> <value>` | 修改配置项 |
| `cfg reset` | 恢复默认配置 |
| `cfg migrate` | 手动触发配置版本迁移 |
| `bootparam` | 显示启动参数 |
| `bootparam set <string>` | 设置启动参数 |
| `bootparam clear` | 清除启动参数 |
| `bootparam get <key>` | 提取启动参数中指定键的值 |
| `app info` | 用户程序信息（存在性 + 版本/大小/CRC） |
| `app erase` | 擦除用户程序区 |
| `app boot` | 重启进入用户程序（由 bootloader 校验） |
| `iap boot` | 重启进入 IAP |
| `iap download on\|off` | 设置下载模式标志 |
| `trig` | 显示触发配置（GPIO / 电平 / 窗口） |
| `trig gpio <n>` | 设置触发引脚（0 = GPIO0 BOOT 键） |
| `bootloader` | **检查 bootloader 与本固件的兼容性** |
| `xmodem recv [offset]` | **XMODEM 接收并写入可变区**（默认 `0x140000`，Ctrl+C 取消） |
| `xmodem send [offset] [len]` | **XMODEM 发送可变区内容**（默认 `0x140000` ~ flash 末尾，Ctrl+C 取消） |
| `reboot` | 重启设备 |

**`cfg set` 支持的 key**：

`wait` `ssid` `pass` `channel` `ip` `mask` `scl` `sda` `i2caddr`
`uartport` `uarttx` `uartrx` `baud` `download` `wifi` `i2c` `verify`
`trig` `triglevel` `trig_gpio` `trig_i2c` `trig_uart` `trig_wifi`

**等待触发源相关 key**：

| key | 取值 | 说明 |
|-----|------|------|
| `trig` | 引脚号 / `off` | 设置触发引脚并自动使能 GPIO 触发 |
| `triglevel` | `low` / `high` | 触发有效电平 |
| `trig_gpio` | `on` / `off` | 单独使能/禁用 GPIO 触发 |
| `trig_i2c` | `on` / `off` | 使能/禁用 I2C 触发 |
| `trig_uart` | `on` / `off` | 使能/禁用 UART 触发 |
| `trig_wifi` | `on` / `off` | 使能/禁用 WiFi 触发 |

#### XMODEM 烧录示例

```bash
# 使用 lrzsz 的 sx 命令发送（XMODEM-1K + CRC）
sx -k --xmodem user_app.bin < /dev/ttyUSB0 > /dev/ttyUSB0

# 或使用 Python 脚本
python tools/xmodem_send.py --port COM3 --file user_app.bin
```

终端侧：

```
iap> xmodem recv
擦除可变区 0x140000 ~ 0x400000 (2752 KB)...
请使用 XMODEM 协议发送镜像文件 (Ctrl+C 取消)...
文件名: user_app_template_flash.bin, 大小: 221024 字节
进度: 221024/221024 字节 (100%)
接收完成: 221024 字节, CRC32 0xCC219FCE
镜像校验通过 (应用镜像 155488 字节 @ 0x150000)
可用 app boot 启动
```

> **`offset` 省略时默认为 `0x140000`** —— 即「整段写入可变区」模式，
> 与 esptool 的 `write_flash 0x140000` 语义一致。
> 若显式给出其它偏移，则按旧流程从该偏移写入用户程序分区。

**`xmodem send` 的默认行为与之对称**：

```
iap> xmodem send
发送可变区 0x140000 ~ 0x400000 (2752 KB)...
等待接收方握手 (Ctrl+C 取消)...
```

> 不带参数时从 `0x140000` 发到 **flash 末尾**（按实际 flash 容量计算），
> 可直接把整段可变区备份出来。

> 支持 XMODEM / XMODEM-CRC / XMODEM-1K，并解析 YMODEM 风格的第 0 包
> （文件名 + 长度）以获得准确的文件总长。

**取消与超时**：

| 情况 | 行为 |
|------|------|
| 传输中按 **Ctrl+C** | 立即取消（向对端发 3 个 CAN），回到 `iap>` 提示符 |
| 等待握手超过 **90 秒** | 超时退出 |
| 传输中 **30 秒**无任何数据包 | 判定对端掉线，自动取消 |

> Ctrl+C 的检测在**底层读回调**中完成 —— 因为传输期间终端任务被
> XMODEM 阻塞，无法自行读取按键。

---

### 8.2 I2C 从机

#### 帧格式

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0-1 | 2 | `magic` | 魔数（`0x4950`，"IP" 小端） |
| 2[0-4] | 5 bit | `version` | 固定 `0x01` |
| 2[5] | 1 bit | `reserved` | 发送端必须为 0 |
| 2[6-7] | 2 bit | `fragment` | `0b10` 单包，`0b01` 多包非末包，`0b11` 多包末包 |
| 3 | 1 | `reserved` | 发送端必须为 0 |
| 4-7 | 4 | `message_id` | uint32 小端，同一消息共用 |
| 8 | 1 | `packet_index` | 包序号（从 0 开始） |
| 9 | 1 | `packet_count` | 包总数 |
| 10-11 | 2 | `payload_length` | uint16 小端，记为 n |
| 12-13 | 2 | `reserved` | 发送端必须为 0 |
| 14-15 | 2 | `crc16` | 多项式 `0x1021`，小端 |
| 16..16+n | n | `payload` | 负载 |

**CRC16 校验范围**：`[0..13]` 与 `[16..16+n)`
（即跳过 CRC 字段本身，其余字节全覆盖）

**接收端校验规则**：

- `magic` 必须匹配
- `version` 必须为 `0x01`
- byte2 的 `reserved` 位与 byte3 必须为 0
- `fragment` 必须为 `0b10` / `0b01` / `0b11` 之一
- 帧总长必须等于 `16 + payload_length`
- `packet_index < packet_count`
- 单包时 `packet_count == 1`；多包时 `packet_count >= 2`
- CRC16 必须匹配

> 注：按需求文档，接收端对 `reserved` 字段**不强制为 0**（仅要求发送端置 0）。
> 本实现为便于排查问题，对帧头的 `reserved` 位做了严格校验；
> 如需放宽，可修改 `iap_i2c.c` 中 `frame_parse()` 的对应判断。

#### 消息负载格式

重组后的完整消息：

```
TYPE(2)  RESERVED(2)  REQUEST_ID(4)  CONTEXT(n-8)
```

- `TYPE`：命令类型
- `RESERVED`：保留，发送时置 0，接收时不强制
- `REQUEST_ID`：请求号。请求-响应型命令组使用同一请求号；非请求响应型置 0
- `CONTEXT`：命令内容

#### 命令列表

**系统信息类**

| TYPE | 名称 | CONTEXT（请求） | CONTEXT（响应） |
|------|------|-----------------|-----------------|
| `0x0001` | `GET_SYS_INFO` | 无 | 见下 |
| `0x0002` | `GET_CFG_INFO` | 无 | `iap_cfg_data_t` |
| `0x0003` | `SET_CFG_INFO` | `iap_cfg_data_t` | 无 |
| `0x0004` | `GET_APP_INFO` | 无 | 见下 |
| `0x0005` | `GET_STATUS` | 无 | 见下 |
| `0x0006` | `GET_BOOT_PARAM` | 无 | `flags(1)` `length(2)` + 参数字符串 |
| `0x0007` | `SET_BOOT_PARAM` | 参数字符串（空则清除） | 无 |

`GET_SYS_INFO` 响应（64 字节）：
`chip_model(4)` `chip_rev(2)` `cores(1)` `flash_size(4)` `heap_free(4)`
`heap_min(4)` `uptime_ms(4)` `idf_ver(32)` `mac(6)`

`GET_APP_INFO` 响应（96 字节）：
`valid(1)` `image_size(4)` `total_size(4)` `crc32(4)` `version(4)` `timestamp(4)`
`project_name(32)` `version_string(32)` `chip_name(16)`

`GET_STATUS` 响应（16 字节）：
`session_active(1)` `session_offset(4)` `session_total(4)` `download_mode(1)` `boot_reason(4)`

**烧录会话类**

| TYPE | 名称 | CONTEXT（请求） | CONTEXT（响应） |
|------|------|-----------------|-----------------|
| `0x0010` | `FLASH_BEGIN` | `total_size(4)` `target(1)` | 无 |
| `0x0011` | `FLASH_DATA` | `offset(4)` + 数据 | `written(4)` |
| `0x0012` | `FLASH_END` | `total_size(4)` `crc32(4)` | 无 |
| `0x0013` | `FLASH_ABORT` | 无 | 无 |

推荐流程：

```
FLASH_BEGIN(total_size)   -> 擦除分区，建立会话
FLASH_DATA(0,   chunk0)   -> 顺序写入
FLASH_DATA(4096, chunk1)
...
FLASH_END(total_size, crc32) -> 校验整包 CRC 并写入配置区
```

**分区读写类**

| TYPE | 名称 | CONTEXT（请求） | CONTEXT（响应） |
|------|------|-----------------|-----------------|
| `0x0020` | `READ_PARTITION` | `offset(4)` `length(2)` | `offset(4)` + 数据 |
| `0x0021` | `WRITE_PARTITION` | `offset(4)` + 数据 | `end_offset(4)` |
| `0x0022` | `ERASE_PARTITION` | `offset(4)` `length(4)` | 无 |
| `0x0023` | `VERIFY_PARTITION` | 无 | `valid(1)` `crc32(4)` `size(4)` |

> `READ_PARTITION` / `WRITE_PARTITION` 用于**读写用户分区指定区域**，
> 不参与烧录会话的 CRC 统计，适合随机访问。

**启动控制类**

| TYPE | 名称 | CONTEXT（请求） | CONTEXT（响应） |
|------|------|-----------------|-----------------|
| `0x0030` | `BOOT_USER_APP` | 无 | 无（应答后重启） |
| `0x0031` | `BOOT_IAP` | 无 | 无（应答后重启） |
| `0x0032` | `SET_DOWNLOAD_MODE` | `enable(1)` | 无 |
| `0x0033` | `RESET_CFG` | 无 | 无 |
| `0x0034` | `REBOOT` | 无 | 无（应答后重启） |

**通用**

| TYPE | 名称 | 说明 |
|------|------|------|
| `0x00F0` | `PING` | 响应 `uptime_ms(4)` |
| `0x00F1` | `ACK` | 应答类型（响应中） |
| `0x00F2` | `NACK` | 否认类型（响应中） |

#### 状态码

应答的 CONTEXT 前 2 字节为状态码：

| 值 | 名称 | 含义 |
|----|------|------|
| `0x0000` | `OK` | 成功 |
| `0x0001` | `ERR_ARG` | 参数错误 |
| `0x0002` | `ERR_STATE` | 状态错误 |
| `0x0003` | `ERR_RANGE` | 越界 |
| `0x0004` | `ERR_FLASH` | Flash 操作失败 |
| `0x0005` | `ERR_CRC` | CRC 校验失败 |
| `0x0006` | `ERR_NO_MEM` | 内存不足 |
| `0x0007` | `ERR_NO_APP` | 无有效用户程序 |
| `0x0008` | `ERR_BUSY` | 忙 |
| `0x0009` | `ERR_UNSUPPORTED` | 不支持的命令 |

#### 主机侧参考代码（Python）

```python
import struct, time
from smbus2 import SMBus

MAGIC = 0x4950
ADDR = 0x42

def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 \
                  else (crc << 1) & 0xFFFF
    return crc

def build_frame(msg_id, payload, index=0, count=1, fragment=0b10):
    hdr = bytearray(16)
    struct.pack_into("<H", hdr, 0, MAGIC)
    hdr[2] = (0x01 << 3) | fragment
    hdr[3] = 0
    struct.pack_into("<I", hdr, 4, msg_id)
    hdr[8] = index
    hdr[9] = count
    struct.pack_into("<H", hdr, 10, len(payload))
    # 12-13 保留
    crc = crc16(bytes(hdr[:14]) + payload)
    struct.pack_into("<H", hdr, 14, crc)
    return bytes(hdr) + payload

def build_message(type_, request_id, context=b""):
    return struct.pack("<HHI", type_, 0, request_id) + context

# 示例: PING
with SMBus(1) as bus:
    frame = build_frame(1, build_message(0x00F0, 0x1234))
    bus.write_i2c_block_data(ADDR, 0, list(frame))
    time.sleep(0.05)
    resp = bus.read_i2c_block_data(ADDR, 0, 32)
    print("响应:", resp.hex())
```

---

### 8.3 USB 串口终端

支持 USB-Serial-JTAG 的芯片（**ESP32-C3 / C5 / C6 / C61 / H2 / S3**）默认开启
USB 串口，提供与 UART **完全等价**的终端与烧录能力。

#### 与 UART 的对比

| 项 | UART | USB 串口 |
|----|------|---------|
| 引脚 | 可配置（默认日志口） | 固定接芯片 D+/D- |
| 波特率 | 可配置 | 无（由主机决定） |
| 驱动 | CP210x / CH34x 等 | Windows 10+ 免驱（CDC-ACM） |
| 终端命令 | ✅ 全部 | ✅ **完全相同** |
| XMODEM 烧录 | ✅ | ✅ |
| 默认状态 | 开启 | **支持 USB 的芯片默认开启** |

#### 使用方式

```bash
# 1. 用 USB 线连接芯片的 USB 口（不是 UART 口）
#    Windows 会识别为 "USB 串行设备 (COMx)"

# 2. 打开串口（波特率任意，USB CDC 不受影响）
idf.py -p COMx monitor
# 或
python -m serial.tools.miniterm COMx 115200

# 3. 终端提示会标明后端
========================================
 ESP IAP v1.0.0 终端就绪 (USB)
 输入 help 查看命令列表
========================================
iap>
```

> **两个终端可同时使用**：UART 与 USB 各自独立运行，命令输出会同时
> 广播到两个通道（便于同时观察日志与操作）。

#### 触发进入 IAP

默认开启 `WAIT_USB_TRIG`（bit10），等待期间在 USB 终端输入 `iap` + 回车
即可进入下载模式，与 UART 触发行为一致。

#### 关闭 USB

```bash
iap> cfg set usb off      # 关闭 USB 终端
```

或通过配置区把 bit9 / bit10 清零。不支持的芯片上这两个标志会被固件自动清除。

---

### 8.4 WiFi + HTTP

#### AP 配置

默认 SSID `ESP-IAP`，密码 `12345678`，IP `192.168.4.1`。
可通过配置区修改（见 4.3 节）。

#### 接口列表

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/` | Web 控制台（上传/下载/操作/配置编辑） |
| GET | `/api/info` | 系统信息（JSON） |
| GET | `/api/cfg` | IAP 配置摘要（JSON，面向人阅读） |
| POST | `/api/cfg` | 修改配置（`key=value&key=value` 查询串格式） |
| GET | `/api/cfg/full` | **完整配置**（JSON，字段名与配置编辑器一致，可回写） |
| POST | `/api/cfg/full` | **写入完整配置**（JSON；仅覆盖出现的字段，支持 `"reboot":true`） |
| GET | `/api/bootparam` | **读取启动参数**（JSON） |
| POST | `/api/bootparam` | **设置启动参数**（请求体为参数字符串原文） |
| DELETE | `/api/bootparam` | **清除启动参数** |
| GET | `/api/app` | 用户程序信息（JSON） |
| POST | `/api/upload` | **上传固件并烧录**（原始二进制；`?addr=&length=`） |
| GET | `/api/download` | **下载 flash 内容**（`?addr=&length=`） |
| POST | `/api/verify` | 校验用户程序 |
| POST | `/api/erase` | 擦除用户程序区 |
| POST | `/api/boot` | 重启进入用户程序 |
| POST | `/api/reboot` | 重启设备 |

#### 上传 / 下载的地址与长度

`/api/upload` 与 `/api/download` 均支持 `addr`（别名 `address` / `offset`）与
`length`（别名 `size`）查询参数，数值支持 `0x` 前缀：

| 参数 | 上传缺省 | 下载缺省 |
|------|---------|---------|
| `addr` | `0x140000`（可变区起点） | `0x140000`（可变区起点） |
| `length` | 文件实际长度 | 到 flash 末尾（按 flash 容量自动计算） |

约束：

- 地址须 `>= 0x140000`（固定区 bootloader / 配置区 / 分区表 A / IAP 程序不可读写）
- 上传地址须 **4KB 对齐**（flash 扇区）
- 地址 + 长度不得越过 flash 容量
- 上传指定 `length` 时只写入前 N 字节（超出文件长度报错）
- **上传到默认地址 `0x140000`** 时按完整镜像处理：校验 `0x150000` 处 magic `0xE9`，
  并把镜像长度 / CRC32 记入配置区；**自定义地址**视为裸写，不做镜像校验。

#### 使用示例

```bash
# 上传固件（默认: 从 0x140000 起写入整个文件）
curl -X POST --data-binary @build/my_user_app.bin http://192.168.4.1/api/upload

# 上传到指定地址/长度
curl -X POST --data-binary @part.bin \
     "http://192.168.4.1/api/upload?addr=0x1A0000&length=0x10000"

# 下载整个可变区（默认: 0x140000 到 flash 末尾）
curl -o dump.bin http://192.168.4.1/api/download

# 下载指定区域
curl -o part.bin "http://192.168.4.1/api/download?addr=0x140000&length=0x1000"

# 查询系统信息
curl http://192.168.4.1/api/info

# 修改配置（查询串格式）
curl -X POST -d "ssid=MyDevice&wait=5" http://192.168.4.1/api/cfg

# 读取完整配置（可回写格式）
curl http://192.168.4.1/api/cfg/full

# 写入完整配置（仅覆盖出现的字段）
curl -X POST -H "Content-Type: application/json" \
     -d '{"wifi_ssid":"MyDevice","wait_seconds":5,"reboot":true}' \
     http://192.168.4.1/api/cfg/full

# 校验并启动用户程序
curl -X POST http://192.168.4.1/api/verify
curl -X POST http://192.168.4.1/api/boot
```

> 浏览器访问 `http://192.168.4.1/` 可获得图形化控制台，支持拖拽上传固件、
> 指定地址/长度上传下载，以及配置区编辑。

#### 配置区编辑（Web 控制台）

浏览器访问 `http://192.168.4.1/` 后，页面底部提供**完整的配置区编辑器**，
功能与 `esp_iap_tool.html` 的「读取配置区」一致：

- **功能开关** —— 11 个标志位（下载模式 / WiFi / I2C / UART / 触发源 / USB）
- **启动等待** —— 进入 IAP 后等待触发源的秒数
- **WiFi 热点** —— SSID / 密码 / 信道 / AP IP / 子网掩码
- **UART 终端** —— 端口 / TX·RX 引脚 / 波特率
- **I2C 从机** —— SCL / SDA 引脚 / 从机地址
- **触发引脚** —— GPIO 引脚与有效电平
- **APP 启动槽** —— 槽数、各槽加载地址/大小、活动槽、槽选择 GPIO
- **启动参数** —— IAP → 用户程序的参数字符串

操作流程：**读取配置** → 修改 → **写回设备**（或**写回并重启**，WiFi/UART 参数需重启生效）。
仅写入 8KB 配置区，不影响 IAP 程序与用户程序。另提供**恢复默认值**按钮。

> 设备端通过 `/api/cfg/full` 读写；所有字段经固件 `sanitize()` 做合法性修正
> （越界 GPIO、非法密码长度、槽地址越界等会被自动纠正）。

#### DHCP 不下发网关（重要）

本设备的 DHCP 服务器**刻意不下发默认网关（option 3）与 DNS（option 6）**：

- 若下发网关，客户端会安装默认路由 `0.0.0.0/0 → 192.168.4.1`，
  把本应发往互联网的流量全部送到本设备，导致**客户端断网**。
- 本设备只提供本地 HTTP 服务，无需充当网关。

客户端仍会正常获得 **IP 地址、子网掩码、租期、服务器标识、广播地址、MTU**，
因此**可以正常访问 `192.168.4.1`**，但不会劫持默认路由。

实现方式见 `main/iap_lwip_hooks.h`（两个 lwIP 钩子配合裁剪 DHCP 响应选项）。

> **注意**：由于不下发网关，部分操作系统（Windows / Android）会显示
> 「已连接，但无法访问互联网」，并可能**自动切换回原来的 WiFi**。
> 此时请在系统网络设置中**关闭「自动切换」**，或手动选择保持连接 `ESP-IAP`。

##### 故障排查：连上 WiFi 但 ping 不通 / 网页打不开

| 现象 | 原因与解决 |
|------|-----------|
| 反复弹出「无法访问互联网」，系统自动切回原 WiFi | 关闭系统的「自动切换网络」；或手动保持连接 |
| 串口日志反复出现 `收到 DHCP 请求 ... DISCOVER` + `发送 DHCP OFFER`，但**从不出现 REQUEST/ACK** | DHCP 响应报文格式错误（选项区末尾指针算错，尾部残留垃圾）→ 客户端拒绝 OFFER。已修复，回归测试见 `tools/test_dhcp_strip.py` |
| ping 不通但能打开网页 | 部分系统禁 ICMP；以浏览器为准 |
| 完全连不上 AP | 检查 `WIFI_ENABLE` 标志位、串口日志是否有 `WiFi AP 已就绪` |

> **诊断技巧**：DHCP 报文的详细日志（选项列表 + 完整 hexdump）**默认关闭**，
> 因为输出量极大（每条报文约 50 行）会淹没终端。
> 需要排查时用编译期定义临时开启：
>
> ```powershell
> idf.py -DIAP_DHCP_DIAG=1 build
> ```
>
> 开启后正常应看到 OFFER 后紧跟客户端的 REQUEST 与设备的 ACK；
> 若只有 OFFER 循环，即为上述报文错误。详见 `main/iap_lwip_hooks.h`。

---

## 9. 编译与烧录

### 9.1 环境准备

```powershell
# Windows PowerShell
& "C:\Espressif\tools\Microsoft.v6.0.3.PowerShell_profile.ps1"
```

### 9.2 编译

```bash
cd <项目目录>

# 选择目标芯片
idf.py set-target esp32s3     # 或 esp32 / esp32c6

# 编译
idf.py build

# 烧录并监视
idf.py -p COM3 flash monitor
```

> **重要提示**：ESP-IDF v6.0.3 自带的 CMake 4.0.3 在**路径包含非 ASCII 字符**
> （如中文目录名）时会崩溃（退出码 `0xC0000409`）。
> 请将项目放在纯 ASCII 路径下构建，例如 `C:\work\esp_iap`。

### 9.3 首次烧录

首次需要完整烧录（bootloader + 分区表 + IAP 程序）：

```bash
idf.py -p COM3 flash
```

之后升级用户程序**无需**串口连接，可通过 I2C / UART / WiFi 任一通道完成。

#### bootloader 字段一致性检查

ESP-IDF 的 bootloader 中固化了若干编译期配置：

| 内容 | 由什么决定 |
|------|-----------|
| 分区表偏移 | `CONFIG_PARTITION_TABLE_OFFSET` |
| app 镜像格式 / 校验规则 | IDF 版本 |
| flash 模式 / 容量 | `CONFIG_ESPTOOLPY_*` |

**排查方法**：IAP 终端执行 `bootloader` 命令，查看设备上 bootloader 的关键字段：

```
iap> bootloader

===== bootloader 检查 =====
位置       : 0x00000000
magic      : 0xE9 (有效)
段数量     : 3
芯片 ID    : 13
flash 模式 : 2 (dio)
flash 频率 : 0
flash 容量 : 2 (4MB)
-----------------------------
结果       : [OK] 与本固件一致
=============================
```

字段不一致时会列出具体差异，例如：

```
结果       : [!!] 与本固件字段不一致
  flash 容量不匹配: bootloader=2, 本固件=3 (8MB)
提示       : 设备上的 bootloader 可能来自其它构建配置
```

IAP 启动时也会自动做此检查并在日志中输出结果。

> **检查项**：`magic` / `chip_id` / `flash 模式` / `flash 容量`。
> **不检查**：`spi_speed`（bootloader 该字段常为 0，运行时才按配置设置，
> 比对会产生误报）、**编译时间**（每次构建都不同，不影响功能）。

> **实测结论**：app 与 bootloader 的 flash 容量字段不一致**不会导致启动失败**
> —— ESP-IDF bootloader 会在运行时探测实际容量并容忍该差异。
> 此检查仅用于**排查参考**，帮助确认设备上的 bootloader 是何时、用何种配置构建的。

> **注意**：esptool 烧录时会根据 `--flash-size` 参数**自动改写** bootloader
> 头部的 flash 容量字段（日志显示 `SHA digest in image updated`），
> 因此用 esptool 正常烧录通常不会产生此不匹配。

> **建议**：首次烧录或更换了 `sdkconfig`（flash 容量 / 分区表 / IDF 版本）时，
> 完整烧录 `iap_side.bin`（含 bootloader + 配置区 + 分区表 A + IAP）；
> 仅升级 IAP 程序时，用同一套配置编译的 app 烧到 `0x10000`。

### 9.4 验证构建

已在以下目标验证编译通过：

| 目标 | 结果 |
|------|------|
| esp32 | ✅ 通过 |
| esp32c6 | ✅ 通过 |
| esp32s3 | ✅ 通过 |

### 9.4.1 发布打包（CMake 目标）

编译完成后，用 IDF 自定义目标一键生成发布产物（替代旧的 `make_release.ps1`）：

```bash
idf.py release        # → build/iap_side_<target>.bin  (1280KB, 从 0x0 起)
idf.py release-user   # → build/user_<target>.bin     (从 0x140000 起)
idf.py release-tools  # 同步 html / js / py 到 build/
idf.py release-all    # 以上全部
idf.py release-embed  # 重建内置镜像（需先编译全部芯片；输出到 build/）
```

| 目标 | 产物 | 说明 |
|------|------|------|
| `release` | `build/iap_side_<target>.bin` | bootloader + 配置区 + 分区表 A + IAP 程序，固定区整片 1280KB |
| `release-user` | `build/user_<target>.bin` | 分区表 B + 用户应用镜像（单一文件，从 `0x140000` 起） |
| `release-tools` | `build/*.py` `*.html` `*.js` | 配套工具与网页编辑器 |
| `release-all` | 以上全部 | 推荐入口 |
| `release-embed` | `esp_iap_builtin.js` + `assets/iap_img_*.js` | 内置镜像（惰性加载，**构建产物**，输出到 `build/`） |

> `release-user` 会从 `build/`、`../user_app*/build/`、`C:/temp/user_app*/build/`
> 中自动查找最新的 `user_app_template.bin`。若未找到，会打印警告并跳过。

---

### 9.5 QEMU 仿真验证

无需真实硬件即可验证 IAP 的启动流程、配置区读写与终端命令。

### 支持的目标

| QEMU 可执行文件 | 支持的芯片 |
|-----------------|-----------|
| `qemu-system-xtensa` | esp32, esp32s3 |
| `qemu-system-riscv32` | esp32c3 |

> QEMU **不支持** ESP32C6，该目标仅能通过真实硬件验证。

### 步骤

**1. 构建固件**

```bash
idf.py set-target esp32s3
idf.py build
```

**2. 合并并填充为 4MB 镜像**

QEMU 要求 flash 镜像大小恰为 2/4/8/16 MB：

```bash
python -m esptool --chip esp32s3 merge-bin \
    -o build/merged.bin \
    --flash-mode dio --flash-freq 80m --flash-size 4MB \
    0x0     build/bootloader/bootloader.bin \
    0x8000  build/iap_cfg.bin \
    0xB000  build/partition_table/partition-table.bin \
    0x10000 build/esp_iap.bin

python -c "size=4*1024*1024; d=open('build/merged.bin','rb').read(); \
open('build/flash_4mb.bin','wb').write(d + b'\xff'*(size-len(d)))"
```

**3. 运行 QEMU**

```bash
qemu-system-xtensa -machine esp32s3 -nographic \
    -drive "file=build/flash_4mb.bin,if=mtd,format=raw" \
    -serial mon:stdio
```

退出：`Ctrl+A` 然后 `X`。

### 预期输出

```
I (281) iap_main: ========================================
I (281) iap_main:  ESP IAP v1.0.0
I (282) iap_main:  芯片: ESP32-S3 rev0, 2 核
I (282) iap_main:  Flash: 4 MB, IDF: v6.0.3
I (282) iap_main: ========================================
I (282) iap_cfg: 配置区 (硬编码): offset=0x00008000, 槽数=2, 槽大小=4096
W (285) iap_cfg: 配置区无有效数据，写入默认配置
I (287) iap_image: 发现 1 个 OTA 槽:
I (287) iap_image:   [0] ota_0: offset=0x00150000, size=2752 KB
I (287) iap_image: IAP 程序区: offset=0x00010000, size=1216 KB
I (288) iap_main: 配置: flags=0x0000079e, 等待=3 秒, 下载模式=否
I (288) iap_main: 等待 3 秒以检测触发源...
I (292) iap_main: 等待阶段已安装 UART 驱动
I (3306) iap_main: 等待窗口结束，无触发，尝试启动用户程序
W (3307) iap_main: OTA 槽 0 无镜像 (首字节非 0xE9)
W (3307) iap_main:  进入 IAP 下载模式
I (3336) iap_cfg: 配置已保存 (槽 1, seq=2)

========================================
 ESP IAP v1.0.0 终端就绪
 输入 help 查看命令列表
========================================
iap> I (3429) iap_term: UART 终端已启动: 端口 0, 波特率 115200
I (3638) iap_i2c: I2C 从机已启动: SCL=GPIO7, SDA=GPIO6, addr=0x42, 100000Hz
I (3656) iap_wifi: AP IP: 192.168.4.1
```

### 可验证项

| 项目 | 验证方式 |
|------|----------|
| 分区表解析 | 启动日志打印 `配置区 (硬编码): offset=0x00008000` |
| 配置区初始化 | 首次启动 `写入默认配置`，再次启动 `已加载配置 (槽 N, seq=M)` |
| 配置区掉电安全 | 反复 `cfg set` / `reboot`，序号递增且能正确回读 |
| 等待逻辑 | 3 秒后打印 `等待超时无操作` |
| 启动决策 | `用户程序区无有效程序` → 进入 IAP 下载模式 |
| UART 终端 | 输入 `help` / `info` / `cfg` / `bootparam` |
| 启动参数 | `bootparam set` → `bootparam` 回读 → `reboot` 后仍存在 |
| I2C 从机 | 日志确认 `I2C 从机已启动` |
| WiFi AP | 日志确认 `AP IP: 192.168.4.1` |

### 仿真环境限制

QEMU 的 `esp32s3` 机器**只模拟 CPU、内存、Flash、UART、定时器等核心外设**，
不包含射频与 I2C 控制器模型。以下项目无法在 QEMU 中验证：

| 限制 | 原因 | 替代验证方式 |
|------|------|--------------|
| WiFi 实际连接 | QEMU 的 `esp32s3` 机器**无网卡设备**（`-nic` 选项被忽略，无 `-netdev` 对端） | 真实硬件；或用 Wokwi 在线仿真 |
| I2C 总线通信 | 无 I2C 控制器模型（`i2c-ddc`/`tmp105` 属 PCI 平台，不挂在 ESP32 上） | 真实硬件 + 外部主机 |
| XMODEM 烧录 | 需真实串口对端 | 真实硬件；或 QEMU 串口接 socket |
| 用户程序跳转 | 需第二个可启动镜像 | 真实硬件 |
| MAC 地址 | 未模拟 eFuse MAC，读出全 0 | 真实硬件 |

**为什么不能模拟 WiFi**：ESP32 的 WiFi 由独立的射频硬件 + 闭源固件 blob 实现，
QEMU 只模拟 CPU 指令，无法执行射频固件。这是所有 ESP32 仿真器的共同限制。

**为什么不能模拟 I2C**：QEMU 官方 I2C 设备（`tmp105` 等）走 PCI 总线，
而 ESP32 的 I2C 是内存映射外设，两者不兼容。需要有人为 ESP32 编写
I2C 控制器模型才能模拟。

**可以模拟的替代方案**：

| 方案 | 覆盖范围 | 说明 |
|------|----------|------|
| QEMU（当前） | 启动流程、配置区、终端、分区、CRC | 已验证 |
| QEMU + 串口 socket | 上述 + XMODEM 烧录 | `-serial socket:...` 或 `-serial tcp:...` |
| Wokwi 在线仿真 | 上述 + WiFi、I2C、GPIO | 需将工程导入 wokwi.com |
| 真实硬件 | 全部 | 最终验收 |

#### QEMU 串口 socket 方案（可验证 XMODEM）

QEMU 支持把串口映射到 TCP，从而让 PC 侧工具与固件通信：

```bash
# 终端 A: 启动 QEMU，串口监听 5555 端口
qemu-system-xtensa -machine esp32s3 -nographic \
    -drive "file=build/flash_4mb.bin,if=mtd,format=raw" \
    -serial tcp:127.0.0.1:5555,server=on,wait=off

# 终端 B: 连接并发送 XMODEM
python tools/iap_xmodem_send.py --port <虚拟串口> --file user_app.bin
```

> 注意：TCP 不是串口，`pyserial` 无法直接打开。需要写一个
> socket 到 pty 的桥接脚本，或用 `socat` 创建虚拟串口对。
> 这条路径可行但配置繁琐，实际调试仍推荐真实硬件。

---

### 9.6 出厂预置配置与单一镜像烧录

#### 问题

默认的 `idf.py flash` **只烧 3 个区域**，不含 `iap_cfg`：

```
0x0      bootloader/bootloader.bin
0x8000   partition_table/partition-table.bin
0x10000  esp_iap.bin          <- 只覆盖 iap 分区
```

因此配置区是空的（全 `0xFF`），设备首次上电才会用默认值写入。
若要在出厂时预置 SSID/密码/IP/I2C 引脚等参数，需要额外烧录配置区。

#### 方案：生成配置镜像 + 合并为烧录镜像

工具配合，最终烧录**两个文件**（IAP 镜像 + 用户程序镜像）：

```
tools/gen_factory_cfg.py   生成 iap_cfg 分区镜像 (8192 字节)
tools/gen_partitions.py    按 flash 容量生成分区表 A/B
tools/gen_boot_embed.py    生成内置镜像索引与分芯片数据文件
tools/merge_bin.py         合成 iap_side.bin (固定区 0x0 ~ 0x13FFFF)
tools/merge_user_app.py    合成 user.bin (从 0x140000 开始, 含分区表 B)
```

#### 步骤 1：生成出厂配置

```bash
# 用默认值
python tools/gen_factory_cfg.py -o iap_cfg.bin

# 指定关键参数
python tools/gen_factory_cfg.py -o iap_cfg.bin \
    --ssid MyDevice-01 --password MyPass2026 \
    --ip 192.168.10.1 --netmask 255.255.0.0 --channel 6 \
    --scl 5 --sda 4 --i2c-addr 0x55 --baud 921600 --wait 5 \
    --trig-gpio 9 --trig-level 1 \
    --boot-param "mode=factory;site=A1"
```

输出示例：

```
已生成出厂配置镜像: iap_cfg.bin (8192 字节)

  配置槽 0: seq=1 (生效)
  配置槽 1: 擦除态

  flags           = 0x0000019E
    -> WiFi, I2C, UART, 校验用户程序
  wait_seconds    = 5
  WiFi SSID       = 'MyDevice-01'
  WiFi 密码       = 'MyPass2026'
  WiFi 信道       = 6
  AP IP           = 192.168.10.1
  AP 掩码         = 255.255.0.0
  I2C SCL/SDA     = GPIO5 / GPIO4
  I2C 地址/频率   = 0x55 / 100000 Hz
  UART 端口/波特率= 0 / 921600
  触发引脚        = GPIO9 (高电平有效)
  启动参数        = 'mode=factory;site=A1'

  data_crc32      = 0x5217E896
```

**全部可用参数**：

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `--flags` | 配置标志位（十六进制） | `0x79E` |
| `--wait` | 启动等待秒数 | 3 |
| `--ssid` | WiFi AP SSID（≤31 字节） | `ESP-IAP` |
| `--password` | WiFi 密码（8~63 字节） | `12345678` |
| `--channel` | WiFi 信道（1~14） | 1 |
| `--ip` | AP IPv4 地址 | `192.168.4.1` |
| `--netmask` | AP 子网掩码 | `255.255.255.0` |
| `--scl` / `--sda` | I2C 引脚 | GPIO7 / GPIO6 |
| `--i2c-addr` | I2C 从机地址 | `0x42` |
| `--uart-port` | UART 端口号 | 0 |
| `--baud` | UART 波特率 | 115200 |
| `--trig-gpio` | 触发引脚（`0xFF`=未配置） | `0xFF` |
| `--trig-level` | 触发有效电平（0=低, 1=高） | 0 |
| `--boot-param` | 启动参数字符串 | 空 |

**用 JSON 管理配置**（适合批量生产）：

```bash
# 导出模板
python tools/gen_factory_cfg.py --dump-template factory.json

# 编辑 factory.json 后生成
python tools/gen_factory_cfg.py -o iap_cfg.bin --json factory.json
```

`factory.json` 示例：

```json
{
  "flags": 30,
  "wait_seconds": 5,
  "wifi_ssid": "MyDevice-01",
  "wifi_password": "MyPass2026",
  "wifi_channel": 6,
  "wifi_ip": "192.168.10.1",
  "wifi_netmask": "255.255.0.0",
  "i2c_scl_gpio": 5,
  "i2c_sda_gpio": 4,
  "i2c_addr": 85,
  "uart_baudrate": 921600,
  "trig_gpio": 9,
  "trig_gpio_level": 1,
  "boot_param": "mode=factory;site=A1"
}
```

> 命令行参数会覆盖 JSON 中的同名字段，便于在通用配置上做小调整。

#### 步骤 2：生成分段烧录镜像

```bash
# 带配置合并，同时生成 4MB 分区表
python tools/merge_bin.py --build-dir build --cfg iap_cfg.bin \
    --flash 4MB --output iap_side.bin

# 一步到位：自动生成默认配置 + 分区表 A 并合并
python tools/merge_bin.py --build-dir build --default-cfg \
    --flash 4MB --output iap_side.bin

# 指定分区表文件
python tools/merge_bin.py --build-dir build --cfg iap_cfg.bin \
    --ptable partitions_iap_4mb.bin --output iap_side.bin
```

输出示例：

```
已生成 IAP 烧录文件 (单一连续文件, 0x000000 ~ 0x13FFFF):

  文件                     地址         大小         内容
  ----------------------------------------------------------------------------
  iap_side.bin           0x000000   0x140000   bootloader.bin + iap_cfg.bin + 分区表A + esp_iap.bin

  (可变区)                  0x140000   —          由 user.bin 单独烧录
```

镜像布局（**单一连续文件，固定区整片 1280KB**）：

```
iap_side.bin (0x000000 ~ 0x13FFFF)
0x000000  +------------------+
          |  bootloader      |
0x008000  +------------------+
          |  iap_cfg  (8KB)  |  <- 出厂配置
0x00A000  +------------------+
          |  (保留)           |
0x00B000  +------------------+
          |  分区表 A  (4KB)  |  <- IAP 用
0x00C000  +------------------+
          |  nvs  (12KB)     |
0x010000  +------------------+
          |  iap  (IAP程序)   |
0x13FFFF  +------------------+  <- 固定区末尾
```

#### 步骤 3：烧录（2 个文件，顺序无关）

```bash
python -m esptool --chip esp32c6 -p COM18 -b 460800 \
    --before default-reset --after hard-reset \
    write-flash \
    0x0      iap_side.bin \
    0x140000 user.bin
```

> **为什么顺序无关**：`iap_side.bin` 覆盖固定区 `0x0 ~ 0x13FFFF`，
> `user.bin` 从可变区起点 `0x140000` 开始（含分区表 B + 应用镜像），
> 两段地址互不重叠。
>
> 也可用浏览器工具：依次加载两个文件（`iap_side.bin` / `user.bin`），
> 工具自动组装为一次烧录。

#### 步骤 4：验证

启动日志应显示读到了烧录的配置（`seq >= 1`）：

```
I (342) iap_cfg: 配置分区: offset=0x00010000, size=8192
I (348) iap_cfg: 已加载配置 (槽 1, seq=2)          <- 读到出厂配置
I (364) iap_main: 配置: flags=0x0000001e, 等待=5 秒, 下载模式=否
I (730) iap_i2c: I2C 从机已启动: SCL=GPIO5, SDA=GPIO4, addr=0x55, 100000Hz
I (915) iap_wifi: WiFi AP 已就绪: SSID='MyDevice-01', 信道 6, IP 192.168.10.1
I (920) iap_term: UART 终端已启动: 端口 0, 波特率 921600
```

若配置区为空，日志会显示使用默认值（`SSID='ESP-IAP'`、`IP 192.168.4.1` 等）。

#### 批量生产建议

1. 用 `--dump-template` 导出一份基准 `factory.json`
2. 为每台设备生成独立配置（改 SSID/IP），或保持相同配置
3. 脚本化流程：

```bash
for sn in 001 002 003; do
    python tools/gen_factory_cfg.py -o cfg_$sn.bin \
        --json factory.json --ssid "Device-$sn" --ip "192.168.10.$((10#$sn))"
    python tools/merge_bin.py --build-dir build --cfg cfg_$sn.bin \
        -o factory_$sn.bin
    python -m esptool --chip esp32c6 -p COM18 write-flash 0x0 factory_$sn.bin
done
```

> **提示**：配置区采用双槽冗余，出厂镜像只写槽 0（`seq=1`），槽 1 保持擦除态。
> 设备首次启动后会更新配置（写入另一个槽，`seq` 递增），这是正常行为。

---

### 9.7 浏览器配置与烧录工具

`esp_iap_tool.html` 是一个**零依赖的单文件 Web 应用**（位于项目根目录），
支持四类操作：

| 功能 | 说明 |
|------|------|
| **编辑配置** | 读取镜像/配置区，修改后写回 |
| **烧录固件** | 通过 Web Serial 直接烧录到设备 |
| **读写 Flash** | 备份/克隆设备，或读回配置再编辑 |
| **烧录用户程序** | 直接写 Flash 或走 IAP 协议（XMODEM） |

所有处理在浏览器本地完成，**文件不会上传到任何服务器**。

#### 打开方式

**方式一：直接双击**（仅配置编辑，推荐 Chrome / Edge）

```bash
# Windows
start esp_iap_tool.html

# macOS
open esp_iap_tool.html

# Linux
xdg-open esp_iap_tool.html
```

> ⚠️ 若浏览器因安全策略禁止 `file://` 访问本地文件，或需要使用**烧录功能**，
> 请改用方式二 —— Web Serial API 要求安全上下文，`file://` 不满足。

**方式二：本地 HTTP 服务**（含烧录功能，推荐）

```bash
python tools/serve_editor.py
```

该脚本会：
1. 先校验 HTML 完整性（文件大小 / JS 语法 / 关键元素）
2. 在项目根目录启动 HTTP 服务（默认 8899，被占用时自动递增）
3. 自动打开浏览器

```
=== 校验 esp_iap_tool.html ===
  [PASS] 文件存在 (123135 字节)
  [PASS] 提取 JS (2643 行)
  [PASS] 关键元素齐全 (11 项)
  [PASS] JS 语法检查通过

校验通过
==============================================================
  ESP IAP 配置编辑器 — 本地服务
==============================================================

  地址: http://localhost:8899/esp_iap_tool.html
```

仅校验不启动服务：

```bash
python tools/serve_editor.py --check
```

> 也可用 `python -m http.server 8899` 手动启动，但需自行确认端口未被占用。

**方式三：在线使用**（GitHub Pages）

若仓库启用了 GitHub Pages，可直接访问：

```
https://<owner>.github.io/<repo>/esp_iap_tool.html
```

> 在线版本通过 HTTPS 提供，满足 Web Serial 的安全上下文要求，
> 可直接在浏览器里连接设备烧录（Chrome / Edge）。

#### 使用流程

**1. 打开文件** —— 点击选择或直接拖拽到页面
编辑器支持 **3 种文件类型**，按内容 magic 自动识别：

| 类型 | 文件 | 说明 |
|------|------|------|
| **IAP 烧录文件** | `iap_side.bin` | 1280KB 整片（bootloader + 配置区 + 分区表 A + IAP 程序） |
| **配置区** | `iap_cfg.bin` | 8KB，整文件即配置区 |
| **用户程序** | `user.bin` | **从 `0x140000` 开始的完整镜像**（分区表 B + 应用镜像，v6） |

加载后自动：
- 选择有效且 `seq` 最大的槽
- 显示两个槽的校验状态（magic / 头部 CRC / 数据 CRC）

> **加载用户程序时**：不显示配置编辑器，改为显示 ESP 镜像头详情
> （负载大小 / CRC32 / 工程名 / 版本 / 目标芯片 / 分区容量是否足够），
> 并自动滚动到「10. 烧录到设备」区块。

**2. 修改配置**（仅配置类文件）—— 9 个分组表单：

| 分组 | 内容 |
|------|------|
| 配置标志位 | 9 个下拉选择框（bit0~bit8，开启/关闭），实时显示 flags 十六进制值 |
| 启动与等待 | 等待秒数 |
| WiFi AP | SSID / 密码 / 信道 / IP / 子网掩码 |
| I2C 从机 | SCL / SDA 引脚 / 从机地址 / 总线频率 |
| UART 终端 | 端口号 / 波特率 |
| 触发引脚 | GPIO 号 / 触发方式（上拉低电平触发 / 下拉高电平触发） |
| 启动参数字符串 | 文本域，实时显示字节数 |

**3. 写回文件** —— 点击「下载修改后的镜像」
- 只覆盖配置区（`0x10000`），**镜像其余部分逐字节保持不变**
- 写回前自动做**字段往返校验**（写入后立即读回比对）
- 下载文件名：`<原名>_modified.bin`

#### 内置校验

编辑器在写回前会检查：

| 检查项 | 规则 |
|--------|------|
| 等待秒数 | 0 ~ 3600 |
| WiFi 信道 | 1 ~ 14 |
| SSID | ≤ 31 字节 |
| WiFi 密码 | ≤ 63 字节；非空时建议 ≥ 8 字节 |
| IP / 子网掩码 | 点分十进制，每段 0~255 |
| I2C 地址 | 0x08 ~ 0x77 |
| I2C 引脚 | 0 ~ 48，且 SCL ≠ SDA |
| I2C 频率 | ~~10 kHz ~ 1 MHz~~ 已废弃（从机模式频率由主机决定） |
| UART 端口 / 波特率 | 0~2 / 1200 ~ 5000000 |
| 触发引脚 | 0~48 或 255（未配置） |
| 启动参数 | ≤ 255 字节 |

#### 错误处理

| 情况 | 行为 |
|------|------|
| 文件过小 | 提示至少需要 8192 字节 |
| 找不到配置区 | 提示确认文件类型，并显示实际读到的 magic |
| 槽 CRC 校验失败 | 显示具体失败原因（magic / 头部 CRC / 数据 CRC），允许从默认值重建 |
| 表单校验失败 | 列出全部问题项，不生成文件 |

#### 与命令行工具的关系

三者使用**完全相同的二进制格式**，可任意混用：

```
tools/gen_factory_cfg.py     → 命令行生成配置区
tools/merge_bin.py           → 命令行合成 iap_side.bin (固定区)
tools/merge_user_app.py      → 命令行合成 user.bin (可变区, 含分区表 B)
esp_iap_tool.html            → 图形化编辑 + 烧录（浏览器）  ← 本工具
```

`tools/verify_html_editor.py` 会逐字节比对 HTML 编辑器与
`gen_factory_cfg.py` 的输出，确保两者一致：

```bash
python tools/verify_html_editor.py
```

#### 适用场景

- **产线快速改配置**：不用装 Python，浏览器打开即可改 SSID/IP
- **现场调试**：直接编辑已烧录的镜像文件，重新烧录
- **配置核对**：打开镜像查看当前出厂配置，确认无误

---

### 9.8 烧录到设备（Web Serial）

编辑器内置 **Web Serial 烧录功能**，基于 Espressif 官方
[esptool-js](https://github.com/espressif/esptool-js)，
**无需安装 Python / esptool / 驱动**，浏览器里点几下就能烧录。

> **统一入口**：配置类文件与用户程序共用「10. 烧录到设备」区块，
> 界面会**根据「1. 选择文件」的类型自动切换** —— 无需在两个区块之间来回找。

#### 环境要求

| 项目 | 要求 |
|------|------|
| 浏览器 | 桌面版 **Chrome / Edge 89+**（Firefox、Safari 不支持） |
| 访问方式 | `http://localhost` 或 `https://`（`file://` 无法使用） |
| 驱动 | Windows 10/11 通常免驱；旧系统可能需装 CP210x / CH34x 驱动 |
| 网络 | 首次需联网加载 esptool-js（约 218 KB，之后浏览器会缓存） |

> **为什么不能用 `file://`**：Web Serial API 要求安全上下文（Secure Context）。
> `localhost` 被浏览器视为安全，`file://` 不是。因此必须用本地 HTTP 服务打开。

#### 使用步骤

**1. 启动本地服务并打开页面**

```bash
python tools/serve_editor.py
```

**2. 连接设备**

点击「连接设备」→ 浏览器弹出串口选择框 → 选择 ESP32 对应的端口
（如 `USB JTAG/serial debug unit` 或 `CP210x`）。

连接成功后显示设备信息：

```
芯片         ESP32-C6 (QFN40) (revision v0.2)
MAC          d4:05:92:b0:d7:2c
Flash 大小   4MB
晶振         40 MHz
```

**3. 在「烧录到设备」中加载文件**

共 **2 个槽位**（v5 两文件方案）：

| 槽位 | 文件 | 烧录地址 | 内容 |
|------|------|---------|------|
| **0. IAP 烧录文件** | `iap_side.bin` | `0x000000` | 1280KB 整片 = bootloader + 配置区 + 分区表 A + IAP 程序 |
| **1. 用户程序** | `user.bin` | 启动槽地址 | 纯 ESP 应用镜像 |

**槽位 0 的两种来源**（单选按钮切换）：

| 模式 | 说明 |
|------|------|
| **使用内置 IAP 镜像**（默认） | 按检测到的芯片自动载入**完整** `iap_side.bin`（含 IAP 程序），无需上传文件 |
| **手动选择文件** | 手动上传 `iap_side.bin`（由 `tools/merge_bin.py` 生成） |

选择「使用内置 IAP 镜像」时会显示**「芯片类型」下拉框**：

| 选项 | 行为 |
|------|------|
| **自动**（默认） | 按连接设备检测到的芯片选择；未连接时提示「连接设备后识别」 |
| **ESP32 / ESP32-S2 / … / ESP32-H2** | 手动指定芯片，**忽略设备检测结果** |

> ✅ **内置镜像是完整的**（含 IAP 程序），**直接烧录即可启动**，无需额外烧 IAP。

### 内置镜像数据（v6，惰性加载）

内置数据拆为 **索引 + 分芯片数据**，默认只加载索引（几 KB）：

| 文件 | 大小 | 何时加载 |
|------|------|----------|
| `esp_iap_builtin.js` | 1.4 KB | 页面启动时（仅元数据） |
| `assets/iap_img_<芯片>.js` | ~800 KB | 首次需要该芯片时注入 `<script>` |

| 项 | 说明 |
|----|------|
| 内容 | 7 个芯片的完整 `iap_side.bin`（每个 raw 1280KB） |
| 压缩 | **zlib**（约 48%）+ Base64 |
| 解压 | 浏览器原生 `DecompressionStream("deflate")`，无需外部库 |
| 加载 | 惰性：`loadChipDataScript()` 按需注入 `<script src>` |
| 缓存 | 解压后结果缓存，重复使用不重新加载 |
| 缺失时 | 页面仍可用「手动选择文件」模式 |

| 芯片 | 压缩后 | 芯片 | 压缩后 |
|------|--------|------|--------|
| esp32 | 768 KB | esp32c5 | 823 KB |
| esp32s2 | 739 KB | esp32c6 | 827 KB |
| **esp32s3** | **760 KB** | esp32h2 | 296 KB |
| esp32c3 | 750 KB | | |

> `esp32c2` 无内置数据（硬件不支持 RTC FAST RAM，无法用 RTC RAM 传递启动参数）。

> ⚠️ **这两个文件是构建产物**，不在开发项目中生成/跟踪（见 `.gitignore`）。
> 由 **GitHub Actions 构建页面时自动生成**并放在 HTML 同目录。
> 若缺失（例如本地直接打开源码），页面仍可用「手动选择文件」模式。
> `esp_iap_builtin.js` 必须与 HTML 同目录；`assets/iap_img_*.js` 在其下的 `assets/` 子目录。

本地按需生成（输出到 `build/`，不污染源码树）：

```bash
idf.py release-embed
```

或直接调用脚本：

```bash
python tools/gen_boot_embed.py --out-js esp_iap_builtin.js --out-dir assets
```

**槽位 0 的「烧录范围」选项**（勾选表示**跳过**该区域）：

| 选项 | 跳过区域 | 默认 |
|------|---------|:----:|
| 不烧 bootloader | `0x0 ~ 0x7FFF` | ☐ |
| **不烧配置区** | `0x8000 ~ 0x9FFF` | **☑** |
| 不烧分区表 A | `0xB000 ~ 0xBFFF` | ☐ |
| 不烧 IAP 程序 | `0x10000 ~ 0x13FFFF` | ☐ |

> 默认跳过配置区 —— 保留设备上已有的 WiFi/引脚/触发设置，
> 避免每次烧录都重置为表单默认值。

**槽位 1 的烧录地址**：由**设备上的分区表 B** 启动槽决定，
连接设备时自动读回（`0x140000`，4KB）。

**4. 按需调整选项**

| 选项 | 说明 |
|------|------|
| **烧录波特率** | 连接时用 115200 握手，之后自动切换（推荐 460800） |
| **槽位勾选** | 只烧勾选的槽位；两个槽位可单独烧 |

操作按钮按目标显示：

- **用户程序** → 「擦除用户程序区」「查询用户程序信息」「启动用户程序」
- **读取设备 Flash** → 「读取并导出 bin」「读取配置区」（解析 + 编辑 + 写回）


**5. 点击「烧录」**

确认对话框 → 等待进度条完成 → 设备自动复位。

终端会实时输出 esptool-js 的日志：

```
=== 开始烧录 ===
地址 0x00010000，大小 8192 字节
esptool.js v0.6.1
Connecting...
Chip is ESP32-C6 (QFN40) (revision v0.2)
Changing baud rate to 460800
...
Compressed 8192 bytes to 156...
Wrote 8192 bytes ...
Hash of data verified.
烧录完成，复位设备
=== 烧录成功 ===
```

#### 附加功能

**擦除配置区** —— 点击「擦除配置区」可将 `0x10000` 的 8KB 清零。
设备下次启动会用内置默认值重新写入配置（相当于恢复出厂设置）。

#### 常见问题

| 现象 | 原因与解决 |
|------|-----------|
| 提示「当前浏览器不支持 Web Serial API」 | 换用桌面版 Chrome / Edge 89+ |
| 提示「Web Serial 需要安全上下文」 | 改用 `http://localhost` 访问，不要用 `file://` |
| 提示「无法加载 esptool-js」 | 检查网络；或改用命令行 `esptool.py` 烧录 |
| 点击连接后没有弹出串口选择框 | 浏览器需用户手势触发；确认点击的是页面按钮而非脚本调用 |
| 串口列表里找不到设备 | 检查 USB 线（需数据线）；确认驱动已装；关闭占用串口的其他程序 |
| 连接失败 / 握手超时 | 拔插 USB 后重试；按住 BOOT 键再点连接（部分板子需要） |
| 烧录中途失败 | 降低波特率到 115200 重试；检查 USB 线质量与供电 |
| 烧录后设备无输出 | 确认烧录范围选对了（整片 vs 仅配置区） |

> **注意**：烧录前请关闭其他占用串口的程序（`idf.py monitor`、
> 串口助手、本文档前面用到的 `monitor_c6.ps1` 等），否则浏览器无法打开端口。

#### 安全说明

- 所有数据在浏览器本地处理，**不上传任何服务器**
- 唯一的外部请求是从 CDN 加载 esptool-js 代码本身
- 串口访问需用户显式授权，网页无法静默打开串口

#### 与命令行烧录的对比

| 方式 | 优点 | 缺点 |
|------|------|------|
| **Web Serial（本工具）** | 免安装、跨平台、可视化、能改配置再烧 | 需 Chrome/Edge、需联网加载库 |
| `esptool.py` 命令行 | 可脚本化、支持 CI、功能最全 | 需装 Python 环境与驱动 |
| `idf.py flash` | 与构建流程集成 | 不含配置区，需另烧 |

两者烧录的**二进制格式完全一致**，可任意混用。

---

### 9.9 烧录用户程序

编辑器支持**两种方式**烧录用户程序，可在界面上切换：

| | **直接写 Flash**（默认，推荐） | **IAP 协议 / XMODEM** |
|---|---|---|
| 机制 | esptool-js 写 `0x140000` | 串口终端 + XMODEM 传输 |
| 速度 | **快**（1MB 约 8 秒） | 慢（1MB 约 90 秒 @115200） |
| 压缩 | ✅ esptool 压缩传输 | ❌ 无压缩，逐包确认 |
| 设备状态 | 复位进下载模式（自动） | 运行 IAP 即可，**无需复位** |
| 硬件流控 | 不需要 | 无，波特率过高易出错 |
| 依赖 | esptool-js | 自研 XMODEM 实现 |

> **v6: 用户程序必须打包为单一文件**（从 `0x140000` 开始，含分区表 B）。
> 用 `build_user_app.py` 或 `tools/merge_user_app.py` 生成。
> 合法性由 **bootloader 启动时自校验**（magic / 段表 / SHA256 / chip_id）。

#### 方式一：直接写 Flash（推荐）

**1. 准备文件**

```bash
cd my_app
python build_user_app.py --target esp32c6
# 产物 user_app_template_flash.bin (从 0x140000 开始)
```

**2. 连接设备** —— 在「10. 烧录到设备」点「连接设备」

**3. 加载文件** —— 在「1. 选择文件」选「**用户程序**」类型
（或保持「自动识别」），拖入 `build/my_app.bin`

编辑器会解析镜像：

```
✓ 用户程序 (完整镜像, 含分区表 B)
文件: user_app_template_flash.bin (221,040 字节)
入口: 0x4086B938   段数: 3
芯片: esp32c6
分区表 B: 已合并 @ 0x140000
应用镜像: @ 0x150000

若文件不是合法格式（分区表 magic 不是 0x50AA 或应用 magic 不是 0xE9），
会明确提示原因并禁用烧录按钮。

加载后「10. 烧录到设备」会自动切换为**用户程序模式**：

```
烧录目标  用户程序  0x00200000 (完整镜像)
          user_app_template_flash.bin (221,040 字节)
```

**4. 选择「直接写 Flash」→ 点「烧录用户程序」**

```
=== 直接写 Flash ===
地址 0x140000，大小 221040 字节
esptool.js v0.6.1
Compressed 221040 bytes to 158432...
Wrote 221040 bytes ...
Hash of data verified.
烧录完成，复位设备
=== 烧录成功 ===
```

> 设备会复位。若需启动用户程序，等 IAP 启动后在终端发 `app boot`。

#### 方式二：IAP 协议 / XMODEM

适合**不能复位设备**的场景（如设备正在执行关键任务）。

**1. 选择「IAP 协议 / XMODEM」** —— 波特率会自动降为 115200（更稳）

**2. 可选「烧录后启动用户程序」**

**3. 点「烧录用户程序」**

```
=== 开始烧录用户程序 (XMODEM) ===
断开 esptool 连接以释放串口...
发送命令: xmodem recv 0
等待设备进入 XMODEM 接收模式 (等待 'C')...
收到 'C'，开始传输 1003792 字节
...
发送 EOT 结束传输...
传输完成
校验通过: 长度 1003280, CRC32 0x0562E4ED
=== 烧录成功 ===
```

#### 其他操作按钮

| 按钮 | 直接写 Flash 方式 | XMODEM 方式 |
|------|------------------|-------------|
| 查询用户程序信息 | 需连接设备（走终端命令） | `app info` |
| 擦除用户程序区 | `eraseRegion(0x150000, 0x2B0000)` | `app erase` |
| 启动用户程序 | `app boot` | `app boot` |

#### XMODEM 协议实现细节

编辑器内置完整的 XMODEM-1K 发送端：

| 项目 | 实现 |
|------|------|
| 握手 | 等待接收方发 `'C'` (0x43) 请求 CRC 模式 |
| 分包 | 剩余 > 128 字节用 1K 包 (STX)，否则 128 字节包 (SOH) |
| 包格式 | `<SOH\|STX> <seq> <255-seq> <data> <crc_hi> <crc_lo>` |
| 填充 | 不足部分补 `0x1A` |
| 校验 | CRC16-CCITT (poly 0x1021, init 0xFFFF, 非反射) |
| 确认 | 收 ACK(0x06) 继续，收 NAK(0x15) 重传，最多 10 次 |
| 结束 | 发 EOT(0x04)，等 ACK |
| 取消 | 发 3 个 CAN(0x18) |

#### 常见问题

| 现象 | 原因与解决 |
|------|-----------|
| 提示「文件过大」 | 超出 user_app 分区容量（2752 KB） |
| 提示「请先连接设备」 | 需先在「10. 烧录到设备」连接 |
| 提示「设备校验失败」 | 文件不是合法 ESP 镜像（magic 非 0xE9），请用 `idf.py build` 产物 |
| 等待 `'C'` 超时 | 设备未进入 XMODEM 模式；确认 UART 终端已启用 |
| 传输中频繁 NAK | 降低波特率（改用 115200） |
| 串口被占用 | 关闭 `idf.py monitor`、串口助手等 |

#### 与命令行方式的对比

| 方式 | 命令 |
|------|------|
| 直接写 Flash | `esptool.py write_flash 0x140000 user_app_template_flash.bin` |
| XMODEM | 终端敲 `xmodem recv` + `python tools/iap_xmodem_send.py` |

浏览器版本把**发命令 + 传输 + 解析结果**合并成一键操作。

---

### 9.10 读取设备 Flash

编辑器还能**从设备读回 Flash 内容并导出为 bin 文件**，用于备份或克隆设备。

#### 使用步骤

**1. 连接设备** —— 在「10. 烧录到设备」点「连接设备」

**2. 选择读取范围**（「11. 读取设备 / 导出 bin」区块）

| 范围 | 地址 | 大小 |
|------|------|------|
| **整片 Flash** | `0x0` | 芯片容量（如 4MB） |
| **IAP 程序区** | `0x10000` | 1216 KB |
| **配置区** | `0x10000` | 8 KB |
| **用户程序区** | `0x150000` | 2752 KB |
| **自定义范围** | 手动填写 | 手动填写 |

自定义范围支持 `0x` 十六进制或十进制，长度自动对齐到 4KB。

**3. 点「读取并导出 bin」**

```
=== 读取 Flash ===
范围 配置区，地址 0x00010000，长度 8192 字节
已读取 8192 / 8192 字节 (100.0%)
读取完成: 8192 字节，耗时 0.8 秒
已导出: dump_esp32c6_00010000_配置区_d7-2c_2026-09-20-03-23-40.bin
```

读取完成后显示摘要：

| 项目 | 值 |
|------|-----|
| 范围 | 配置区 |
| 地址 | `0x00010000 ~ 0x00012000` |
| 长度 | 8,192 字节 (8.0 KB) |
| CRC32 | `0x6B88E2C6` |
| 非 0xFF 字节 | 520 (6.3%) |
| 耗时 | 0.8 秒 |

> **非 0xFF 字节占比**可判断区域是否为空 —— 若接近 0%，说明该区域是擦除态。

#### 读取配置区（解析 + 编辑 + 写回）

点「**读取配置区**」一步完成读取、解析并进入编辑模式：

```
读回 8KB 配置区 → 解析最优槽 → 显示槽状态 → 填入编辑表单
     ↓
用户修改字段 → 校验 → 生成新配置区 → 写回设备
     ↓
自动回读校验（逐字段比对）
```

读取后显示槽状态摘要：

| 项目 | 值 |
|------|-----|
| 读取地址 | `0x00010000 ~ 0x00012000` |
| 槽 0 | ✅ 有效 seq=25 |
| 槽 1 | ✅ 有效 seq=24 |
| 当前生效 | 槽 0 (seq=25) |

**编辑表单字段**：

| 分组 | 字段 |
|------|------|
| 启动 | 等待秒数 |
| WiFi | SSID / 密码 / 信道 / AP IP / 子网掩码 |
| I2C | SCL / SDA / 从机地址 / 总线频率 |
| UART | 端口号 / 波特率 |
| 触发 | 触发引脚 / 触发方式（上拉低电平 / 下拉高电平） |
| 其他 | flags（只读，含中文描述）/ 启动参数 |

**写回流程**：

```
=== 写回配置区 ===
写入 8192 / 8192 字节
写入完成，回读校验...
回读校验通过
=== 写回成功 ===
```

写回前会：
1. 用与配置编辑器**相同的校验规则**检查（IP 格式、SSID 长度、I2C 地址范围等）
2. 弹确认框显示将要写入的关键字段
3. 写回后**自动回读并逐字段比对**，不一致立即报错

**只写 8KB 配置区**，不影响 IAP 程序与用户程序。写回后需重启设备生效。

**其他按钮**：

| 按钮 | 说明 |
|------|------|
| 重新读取 | 丢弃修改，重新从设备读回 |
| 导出为 bin | 把编辑后的配置存成独立 8KB 镜像（可单独烧到 0x10000） |
| 取消编辑 | 关闭编辑表单 |

#### 导出的文件能做什么

| 用途 | 方法 |
|------|------|
| **备份设备** | 读整片 → 保存 bin，出问题时 `esptool write_flash 0x0` 恢复 |
| **克隆设备** | 读整片 → 烧到另一台（注意 MAC 会相同，需另改） |
| **查看配置** | 读配置区 → 用「1. 选择文件」加载，表单里查看/编辑 |
| **提取用户程序** | 读用户程序区 → 得到纯 ESP 应用镜像，可再烧到其他设备 |
| **对比固件** | 读 IAP 区 → 与本地构建的 bin 比对 CRC32 |
| **远程改配置** | 读回 → 改 SSID/IP → 写回，无需重新烧录整个固件 |

#### 常见问题

| 现象 | 原因与解决 |
|------|-----------|
| 读取很慢 | 读取无压缩，1MB @460800 约 30 秒；可提高波特率 |
| 读到的全是 0xFF | 该区域未烧录或已擦除 |
| 读取中途失败 | 降低波特率重试；检查 USB 线质量 |
| 导出文件打不开 | 整片 dump 不是标准镜像格式，需用「1. 选择文件」手动指定类型 |

> **注意**：读取整片 Flash（4MB）在 460800 波特率下约需 2 分钟。
> 若只需配置，用「读取配置区」最快（8KB，不到 1 秒）。

#### 与命令行方式的对比

```bash
# 命令行读取整片
esptool.py read_flash 0 0x400000 backup.bin

# 命令行读取配置区
esptool.py read_flash 0x10000 0x2000 cfg_backup.bin
```

浏览器版本的优势：**读回后立即解析**（配置区自动显示 SSID/IP/flags），
且导出文件可直接拖回「1. 选择文件」继续编辑。

---

### 9.11 自动化构建（GitHub Actions）

项目包含两个 workflow：

| Workflow | 文件 | 作用 |
|----------|------|------|
| **固件构建** | `.github/workflows/build.yml` | 7 芯片矩阵编译 + 合并镜像 + Release |
| **网页构建** | `.github/workflows/pages.yml` | 校验 HTML + 部署到 GitHub Pages |

#### 固件构建（build.yml）

**触发**：push / PR / tag `v*` / 手动（可指定目标子集）

**芯片支持矩阵**：

| 芯片 | WiFi | I2C 从机 | RTC RAM | 说明 |
|------|:----:|:--------:|:-------:|------|
| ESP32 | ✅ | ✅ | ✅ | 完整支持 |
| ESP32-S2 | ✅ | ✅ | ✅ | 完整支持 |
| ESP32-S3 | ✅ | ✅ | ✅ | 完整支持 |
| ESP32-C3 | ✅ | ✅ | ✅ | 完整支持 |
| ESP32-C5 | ✅ | ✅ | ✅ | 完整支持（WiFi 6 + USB-Serial-JTAG） |
| ESP32-C6 | ✅ | ✅ | ✅ | 完整支持 |
| ESP32-H2 | ❌ | ✅ | ✅ | 无 WiFi（`SOC_WIFI_SUPPORTED=0`） |
| ESP32-C2 | ✅ | ❌ | ❌ | **不支持**：无 RTC FAST RAM，无法传递启动参数 |

> 不支持的功能在源码中用 `#if` 守卫，对应接口返回 `ESP_ERR_NOT_SUPPORTED`，
> 不影响其余功能编译与运行。ESP32-C2 因缺少 RTC FAST RAM 被
> `main/iap_boot_param.h` 用 `#error` 明确拦截，未纳入构建矩阵。

**Job 结构**：

```
prepare ──→ build (7 芯片矩阵) ──→ embed-bootloader ──→ summary ──→ release (tag / 手动)
   │
   └─ 解析目标列表（支持手动指定子集）
```

**产物**（每个芯片**只发布一个烧录文件**）：

| 文件 | 说明 |
|------|------|
| `iap_side_<chip>.bin` | ★ **唯一烧录文件**（1280KB 整片，含 bootloader + 配置区 + 分区表 A + IAP） |
| `MANIFEST_<chip>.txt` | 清单（大小 + SHA256） |
| `user_<chip>.bin` | 用户程序（可选，来自 `examples/user_app_template`，单独烧到 `0x140000`） |

> **中间产物不再发布**：`esp_iap.bin` / `bootloader.bin` /
> `partition-table.bin` / `.elf` 都已被 `iap_side.bin` 完整包含，
> 仅作为内部 artifact（`iap-intermediates-*`，保留 7 天）供
> `embed-bootloader` 任务切分使用，**不随 Release 发布**，避免使用者误烧。

`embed-bootloader` 任务：用各芯片的 `bootloader.bin` + `esp_iap.bin` +
`partition-table.bin` 合成完整镜像，重新编码为 Base64 写入
`esp_iap_builtin.js`（HTML 惰性加载），并用 `gen_boot_embed.py --check`
校验一致性。

`iap_side_<chip>.bin` 布局：

```
0x000000  bootloader
0x008000  配置区（默认值，可用 esp_iap_tool.html 改后重烧）
0x00B000  分区表 A
0x00C000  nvs
0x010000  IAP 应用
0x13FFFF  ---- 固定区末尾 ----
```

> **为什么 CI 不直接产出 `idf.py build` 的结果**：`idf.py flash` 只烧
> bootloader / 分区表 / 应用，**不含配置区**（`iap_cfg` 是硬编码地址，
> IDF 不知道要写什么）。因此 CI 用 `tools/merge_bin.py --default-cfg`
> 额外把默认配置区合并进去，用户烧一次即可完成出厂配置。

**手动指定目标**：在 Actions 页面点 "Run workflow"，填入：

```
targets: esp32,esp32c6      # 只构建这两个
targets:                    # 留空 = 全部 7 个
```

**Release 内容**：所有芯片固件 + 配套工具（`esp_iap_tool.html`、
`gen_factory_cfg.py`、`gen_boot_embed.py`、`merge_bin.py`、
`merge_user_app.py` 等）+ `SHA256SUMS.txt`。

#### 网页构建（pages.yml）

**触发**：修改 `esp_iap_tool.html` / `tools/**` / 该 workflow 自身时

**校验步骤**：

| 步骤 | 内容 |
|------|------|
| 1. 文件检查 | 存在且 > 10KB |
| 2. JS 语法 | 提取 `<script type="module">` 用 `node --check` |
| 3. 交叉验证 | `tools/verify_html_editor.py` 逐字节比对 HTML 与 Python 工具 |
| 4. 关键元素 | 检查 11 个必需元素（按钮 ID、常量名） |

**部署**：校验通过后发布到 GitHub Pages

```
https://<owner>.github.io/<repo>/esp_iap_tool.html
```

站点内容：
- `index.html` / `esp_iap_tool.html` — 编辑器
- `README.md` — 文档
- `tools/*.py` — 配套脚本（可下载）

> **在线版本的优势**：通过 HTTPS 提供，满足 Web Serial 的安全上下文要求，
> 可直接在浏览器里连接设备烧录，无需本地起服务。

#### 本地复现 CI 检查

```bash
python tools/serve_editor.py --check
```

输出与 CI 中的校验步骤一致：

```
=== 校验 esp_iap_tool.html ===
  [PASS] 文件存在 (120242 字节)
  [PASS] 提取 JS (2571 行)
  [PASS] 关键元素齐全 (11 项)
  [PASS] JS 语法检查通过

校验通过
```

---

## 10. 用户程序集成

用户程序需要：

1. 使用**同一份分区表**（含 `iap_cfg` 分区）
2. 分区标签为 `user_app`，子类型 `ota_0`
3. （可选）集成 [`examples/user_app/iap_user_api.c`](examples/user_app/iap_user_api.c)
   以支持主动请求升级

### 10.1 使用模板工程（推荐）

`examples/user_app_template/` 是一个**可直接编译运行**的完整工程，
已经配好分区表、sdkconfig 与对接代码：

```
examples/user_app_template/
├── CMakeLists.txt              顶层工程
├── sdkconfig.defaults          关键配置（分区表等）
├── partitions_user_app.csv     分区表（与 IAP 一致）
├── build_user_app.py           一键编译 + 打包（合并分区表 B）
├── README.md                   模板说明
└── main/
    ├── CMakeLists.txt
    ├── main.c                  用户程序入口（含完整对接示例）
    └── iap_user_api.c/.h       IAP 对接接口
```

**使用步骤**：

```bash
# 1. 复制模板到你的工程目录
cp -r <IAP>/examples/user_app_template ~/my_project
cd ~/my_project

# 2. 一键编译 + 打包（合并分区表 B，输出从 0x140000 开始）
python build_user_app.py --target esp32c6
```

输出 `build/user_app_template.bin`（纯 ESP 应用镜像），可直接通过任一通道烧录。

**模板演示的关键流程** (v5/v6)：

| 步骤 | 调用 | 说明 |
|------|------|------|
| 1 | `iap_user_get_boot_param()` | 读取 IAP 传来的启动参数 (RTC RAM) |
| 2 | `iap_user_boot_param_get_value()` | 按 key 提取参数值（如 `mode`） |
| 3 | `iap_user_boot_param_read()` | 读取自身加载信息 (地址/大小/版本) |
| 4 | `iap_user_request_download()` | 请求重启进入 IAP 下载模式 |
| 5 | `iap_user_request_apply()` | 请求修改配置并重启进 IAP 持久化 (v6) |

`main.c` 中 `init_by_mode()` 演示了启动参数的典型用途：
IAP 下载完成后可指定一次性的运行模式（`normal` / `debug` / `factory`），
用户程序据此分支初始化。

> ⚠️ v5: 用户程序**不访问配置区**。早期版本的 `iap_user_report_boot_ok()` /
> `iap_user_report_version()` / `iap_user_config_read()` 已全部删除。
> v6: 需要修改配置时用 `iap_user_request_*()` + `iap_user_request_apply()`。

### 10.2 手动集成

如果不想用模板，只需把接口文件加入你的工程：

```cmake
# 用户程序 main/CMakeLists.txt
idf_component_register(
    SRCS "main.c" "iap_user_api.c"
    INCLUDE_DIRS "."
    REQUIRES esp_system esp_timer bootloader_support
)
```

```c
// 用户程序 main.c
#include "iap_user_api.h"

void app_main(void)
{
    // 读取 IAP 传来的启动参数
    char param[IAP_USER_BOOT_PARAM_SIZE];
    if (iap_user_get_boot_param(param, sizeof(param)) == ESP_OK) {
        ESP_LOGI(TAG, "启动参数: %s", param);
    }

    // ... 用户程序逻辑 ...

    // 收到升级指令时，请求重启进入 IAP 下载模式
    if (upgrade_requested) {
        iap_user_request_download();   // 不会返回
    }
}
```

> **注意**：`sdkconfig.defaults` 中必须把分区表指向与 IAP 一致的 CSV：
> ```
> CONFIG_PARTITION_TABLE_CUSTOM=y
> CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_user_app.csv"
> ```

### 10.3 编译与烧录

```bash
# 1. 编译 + 打包用户程序（合并分区表 B）
cd my_user_app
python build_user_app.py --target esp32c6
# → user_app_template_flash.bin (从 0x140000 开始)

# 2. 通过任一通道烧录 user_app_template_flash.bin
#    - UART:  iap> xmodem recv  然后发送该文件
#    - WiFi:  curl -X POST --data-binary @user_app_template_flash.bin http://192.168.4.1/api/upload
#    - I2C:   使用 FLASH_BEGIN / FLASH_DATA / FLASH_END 命令序列
#    - 浏览器: 打开 esp_iap_tool.html → 选择该文件 → 烧录到设备
#    - 直接写: esptool.py write_flash 0x140000 user_app_template_flash.bin

# 3. 启动用户程序
#    iap> app boot   或   curl -X POST http://192.168.4.1/api/boot
```

> 镜像合法性由 bootloader 启动时自校验，IAP 侧不重复校验。
> 用户程序**不访问配置区** (v5)，启动参数经 RTC RAM 传递。

> 用模板的 `build_user_app.py` 可把步骤 1-2 合并为一条命令。

---

## 11. 源码结构

```
IAP/
├── CMakeLists.txt              工程入口
├── partitions_iap.csv          分区表
├── sdkconfig.defaults          默认配置
├── README.md                   本文档
├── esp_iap_tool.html           浏览器配置编辑器 + 烧录工具（纯前端，183 KB）
├── esp_iap_builtin.js          内置镜像索引（1.4 KB，惰性加载入口）
├── assets/
│   └── iap_img_<芯片>.js        各芯片完整镜像数据（按需加载，~800 KB）
├── .github/
│   └── workflows/
│       ├── build.yml           固件多芯片矩阵构建 + Release
│       └── pages.yml           网页编辑器校验 + GitHub Pages 部署
├── main/
│   ├── CMakeLists.txt
│   ├── main.c                  主流程与启动决策
│   ├── iap_common.h            公共定义（配置布局、启动参数、命令枚举）
│   ├── iap_crc.c/.h            CRC32 / CRC16-CCITT
│   ├── iap_config.c/.h         IAP 配置区管理（双槽冗余）
│   ├── iap_image.c/.h          用户程序镜像管理（存在性检查、启动切换）
│   ├── iap_xmodem.c/.h         XMODEM 协议（介质无关）
│   ├── iap_uart.c/.h           UART 终端与命令
│   ├── iap_i2c.c/.h            I2C 从机协议与命令处理
│   ├── iap_wifi.c/.h           WiFi SoftAP
│   ├── iap_lwip_hooks.h        lwIP 钩子（DHCP 不下发网关）
│   ├── iap_wait_trigger.c/.h   等待触发源检测 (GPIO/I2C/UART/WiFi)
│   ├── iap_index.html          Web 控制台页面（构建时嵌入为二进制数据）
│   └── iap_http.c/.h           HTTP 服务与 Web 控制台├── tools/
│   ├── gen_factory_cfg.py      生成出厂配置镜像
│   ├── gen_partitions.py       按 flash 容量生成分区表 A/B (只改 user_app 大小)
│   ├── gen_boot_embed.py       生成内置镜像索引 + 分芯片数据 (惰性加载)
│   ├── merge_bin.py            合成 iap_side.bin (固定区 0x0 ~ 0x13FFFF)
│   ├── merge_user_app.py       合成 user.bin (从 0x140000 开始, 含分区表 B)
│   ├── iap_xmodem_send.py      命令行 XMODEM 发送
│   ├── serve_editor.py         本地预览 / 校验网页编辑器
│   ├── verify_html_editor.py   HTML 与 Python 工具交叉验证
│   └── test_crc.py             CRC 算法一致性测试
├── bootloader_components/
│   └── main/
│       ├── CMakeLists.txt
│       └── bootloader_start.c  ★ 自定义 bootloader（不读分区表）
└── examples/
    └── user_app_template/      ★ 可直接编译的用户程序模板工程
        ├── CMakeLists.txt
        ├── sdkconfig.defaults
        ├── partitions_user_app.csv  分区表 B（烧到 0x140000）
        ├── build_user_app.py   一键编译 + 打包（合并分区表 B）
        ├── README.md
        └── main/
            ├── CMakeLists.txt
            ├── main.c          用户程序入口（含完整对接示例）
            └── iap_user_api.c/.h
```

### 模块依赖

```
main.c
  ├── iap_config  ── iap_crc
  ├── iap_boot_param ── (RTC RAM 参数传递)
  ├── iap_image   ── iap_config, iap_crc, iap_boot_param
  ├── iap_wait_trigger ── iap_config (回调启动 iap_i2c / iap_wifi)
  ├── iap_uart    ── iap_config, iap_image, iap_xmodem
  ├── iap_i2c     ── iap_config, iap_image, iap_crc, iap_wait_trigger
  ├── iap_wifi    ── iap_config, iap_wait_trigger
  └── iap_http    ── iap_config, iap_image, iap_wifi

bootloader_start.c
  └── bootloader_common_get_rtc_retain_mem()  ← 读 RTC RAM 启动参数
```

---

## 12. 已知限制

| 限制 | 说明 |
|------|------|
| 等待期间仅检测 UART | 等待阶段 I2C/WiFi 未启动，需通过串口输入中断等待 |
| I2C 单帧负载上限 1024 字节 | 受 `I2C_FRAME_MAX_PAYLOAD` 限制，大文件需分帧 |
| I2C 消息重组上限 64 KB | 受 `I2C_MSG_MAX_SIZE` 限制 |
| HTTP 上传需合法镜像 | 非 ESP 应用镜像（首字节非 0xE9）会被拒绝 |
| 无加密/签名校验 | `sha256` 字段已预留，但当前未强制校验 |
| 启动参数上限 255 字符 | 受 `boot_param[256]` 缓冲区限制，值中不能含 `;` |
| GPIO 触发引脚需预先配置 | `trig_gpio` 为 `0xFF` 时自动禁用 GPIO 触发 |
| 等待期间仅启动已使能的子系统 | 未使能对应触发源时不启动 I2C/WiFi，不占用资源 |
| 分区表固定 | 修改分区表需同步更新 `iap_common.h` 中的标签与类型 |
| 配置区不可缩减 | `iap_cfg` 至少 8KB（双槽 × 1 扇区） |

### 后续可扩展方向

- 启用 SHA256 与签名校验，防止固件被篡改
- 支持压缩传输（esptool 协议自带）
- 增加 I2C 中断等待检测（等待阶段也响应 I2C 请求）
- 增加 OTA 回滚保护（`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`）

---

## 附录：CRC 算法说明

| 算法 | 多项式 | 初值 | 反射 | 输出异或 | 用途 |
|------|--------|------|------|---------|------|
| CRC-32/ISO-HDLC | `0xEDB88320`（反射） | `0xFFFFFFFF` | 是 | `0xFFFFFFFF` | 用户程序、配置区完整性 |
| CRC-16/CCITT-FALSE | `0x1021` | `0xFFFF` | 否 | `0x0000` | I2C 帧校验 |

两端（设备与 PC 工具）必须使用**完全一致**的算法参数，
否则校验会失败。Python 侧使用 `binascii.crc32()` 与
`iap_xmodem_send.crc16()`，与设备端 `iap_crc32()` / `iap_crc16()`
结果一致（`tools/test_crc.py` 已验证，含硬件/软件两条路径）。

### 硬件 CRC 加速

所有 ESP32 系列芯片的 ROM 中都带有硬件 CRC 加速器，可显著加快大块数据
（如 2MB 的用户程序分区）的校验速度，且省下查表所需的 flash 空间。

**等价关系**（已用主机端逐字节验证，见 `tools/test_crc.py`）：

| 软件实现 | 硬件 ROM 等价调用 | 验证 |
|---------|------------------|------|
| `iap_crc32(data, len)` | `esp_rom_crc32_le(0, data, len)` | ✅ |
| `iap_crc32_update(c, data, len)` | `esp_rom_crc32_le(c, data, len)` | ✅ |
| `iap_crc16(data, len)` | `~esp_rom_crc16_be(0, data, len)` | ✅ |
| `iap_crc16_update(c, data, len)` | `~esp_rom_crc16_be(~c, data, len)` | ✅ |

> **关键差异**：ROM 函数内部在进入时取反、返回时再取反（`crc = ~crc` … `return ~crc`）。
> - CRC32 的软件实现本身就带首尾取反，正好与 ROM 抵消 → **无需额外取反**
> - CRC16-CCITT-FALSE 不做任何取反 → **需要外部再取反**
> - 增量式尤其要注意：CRC16 是**双重取反**（入参也要取反）

**当前实现**：

| 算法 | 默认路径 | 回退开关 |
|------|---------|---------|
| CRC32 | `esp_rom_crc32_le` | `IAP_CRC32_SOFTWARE=1` |
| CRC16 | `esp_rom_crc16_be` | `IAP_CRC16_SOFTWARE=1` |

```bash
# 全部切回软件实现（便于交叉验证）
idf.py -DCMAKE_C_FLAGS="-DIAP_CRC32_SOFTWARE=1 -DIAP_CRC16_SOFTWARE=1" build
```

**协议兼容性**：CRC16 用于 I2C 帧校验，硬件路径与软件路径结果**完全一致**
（已验证），因此与 PC 工具、其他设备的 I2C 通信不受影响。

**用户程序侧**：v5 起用户程序**不访问配置区**，不再需要 CRC 校验配置数据，
因此 `iap_user_api.c` 中的软件/硬件 CRC 实现已移除。RTC RAM 启动参数
的 CRC 由 `iap_param_update_crc()`（IAP 侧）与 `iap_user_boot_param_valid()`
（用户程序侧）用 ROM 硬件 CRC 完成。

> **镜像完整性无需用户程序自行校验**：`user_app` 分区内是纯 ESP 应用镜像，
> bootloader 启动时会做完整校验，失败自动回落 IAP。

**效果**：

| 项目 | 改动前 | 改动后 | 变化 |
|------|--------|--------|------|
| 固件大小 | 1003280 B | 1002624 B | **−656 B** |
| CRC32 校验 | 4bit 查表 | ROM 硬件 | 快数倍 |
| CRC16 校验 | 按位循环 | ROM 硬件 | 快数倍 |

> ROM 的 `crc16_le` 变体不匹配任何标准算法，因此 CRC16 选用 `crc16_be`。
