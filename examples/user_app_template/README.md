# ESP IAP 用户程序模板

这是一个**可直接编译运行**的用户程序模板，演示如何与 IAP 程序对接。

## 目录结构

```
user_app_template/
├── CMakeLists.txt              # 顶层工程
├── sdkconfig.defaults          # 关键配置（分区表等）
├── partitions_user_app.csv     # 分区表（必须与 IAP 一致）
├── main/
│   ├── CMakeLists.txt
│   ├── main.c                  # 用户程序入口
│   └── iap_user_api.c/.h       # IAP 对接接口（从 IAP 工程复制）
└── README.md
```

## 快速开始

### 1. 复制接口文件

把 IAP 工程的 `examples/user_app/iap_user_api.c` 和 `iap_user_api.h`
复制到本模板的 `main/` 目录下：

```bash
cp <IAP目录>/examples/user_app/iap_user_api.* main/
```

### 2. 编译 + 打包

**推荐：一键脚本**（编译 + 合并分区表 B）

```bash
python build_user_app.py --target esp32c6
```

输出 `user_app_template_flash.bin`（约 221 KB，**从 `0x140000` 开始**）：

```
0x140000  分区表 B     (4KB)
0x141000  0xFF 填充    (nvs 位置)
0x150000  应用镜像     (155KB)
```

或手动：

```bash
idf.py set-target esp32c6      # 换成你的芯片
idf.py build
```

### 3. 烧录

⚠️ **v6 起用户程序需打包**（合并分区表 B），IAP 会整段写入 `0x140000`。

任选一种通道：

| 通道 | 命令 |
|------|------|
| **UART** | `iap> xmodem recv` 然后发送 `user_app_template_flash.bin` |
| **WiFi** | `curl -X POST --data-binary @user_app_template_flash.bin http://192.168.4.1/api/upload` |
| **I2C** | FLASH_BEGIN / FLASH_DATA / FLASH_END 命令序列 |
| **浏览器** | 打开 `esp_iap_tool.html` → 「选择文件」选 `user_app_template_flash.bin` → 「烧录到设备」 |
| **直接写 Flash** | `esptool.py write_flash 0x140000 user_app_template_flash.bin` |

> **为什么需要打包**：`user_app` 分区定义在**分区表 B**（`0x140000`），
> 而 IAP 运行在**分区表 A** 下，无法用 `esp_partition_find_first()` 找到它。
> 打包后 IAP 直接整段写入，与 esptool 语义一致。

**兼容旧流程**（纯应用镜像，需 IAP 解析分区表）：

```bash
python build_user_app.py --no-build --no-merge -o app_only.bin
# 然后用 xmodem recv 发送（IAP 会解析分区表 B 找到 user_app 地址）
```

### 4. 启动用户程序

```
iap> app boot
```

或 `curl -X POST http://192.168.4.1/api/boot`

---

## 关键约束

| 约束 | 说明 |
|------|------|
| **使用分区表 B** | 用户程序用 `partitions_user_app.csv`（= 项目根 `partitions_app.csv`），位于 `0x140000`，与 IAP 的分区表 A（`0xB000`）**完全独立** |
| **分区标签** | 用户程序运行在 `user_app`（`ota_0` 子类型）分区 |
| **入口地址** | 用户程序烧录到 `0x150000` |
| **镜像格式** | 必须是合法 ESP 应用镜像（offset 0 = `0xE9`），即 `idf.py build` 产物 |
| **打包 (v6)** | 用 `build_user_app.py` 合并分区表 B，输出从 `0x140000` 开始的单一文件 |
| **不访问配置区** | ⚠️ 用户程序**不得**读写配置区 (`0x8000`)。需要配置请用启动参数 |
| **RTC 配置一致** | `CONFIG_BOOTLOADER_RESERVE_RTC_MEM` / `CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC` 必须与 IAP 相同 |
| **NVS 分区名** | 用 `nvs_flash_init_partition("nvs")`，名字**必须**为 `nvs`（WiFi 驱动要求） |

### v5/v6 架构要点

| 项 | 说明 |
|----|------|
| **配置区访问** | ⚠️ **不开放**。配置区在 IDF flash 写保护区 (`0x0~0xBFFF`) 内，且属 IAP 私有数据 |
| **获取配置** | 从 RTC RAM 启动参数读：`iap_user_boot_param_read()` / `iap_user_get_boot_param()` |
| **修改配置 (v6)** | `iap_user_request_*()` 填 RTC RAM → `iap_user_request_apply()` 重启进 IAP 写入配置区 |
| **烧录方式 (v6)** | 打包为从 `0x140000` 开始的单一文件，IAP 整段写入（与 esptool 语义一致） |
| **救援入口** | 上电时按住 **BOOT 键 (GPIO0)**，或在等待窗口内发 `iap` 字符串 |

---

## 配置区不开放 + 间接修改 (v5/v6 重要变更)

⚠️ **用户程序不允许直接访问配置区 (`0x8000`)**。原因：

1. 配置区位于 IDF 的 flash 写保护区 (`0x0 ~ 0xBFFF`)，用户程序直接写会触发 `abort()`
2. 配置区是 IAP 的私有数据，用户程序读写会破坏双槽冗余的一致性

因此早期版本的 `iap_user_config_read()` / `iap_user_config_write()` /
`iap_user_cfg_addr()` / `iap_user_report_boot_ok()` / `iap_user_report_version()`
**已全部删除**，`iap_user_cfg_t` 结构也已移除。

### 读取配置

| 需求 | 正确做法 |
|------|----------|
| 读取配置 (WiFi/触发源等) | IAP 启动前把需要的字段放入启动参数字符串，用户程序用 `iap_user_get_boot_param()` 读取 |
| 读取自身加载信息 | `iap_user_boot_param_read()` (地址/大小/版本) |

### 修改配置 (v6: 间接持久化)

用户程序**不能直接写配置区**，但可通过 **RTC RAM update 区** 请求 IAP 代为修改：

```
用户程序                           IAP
   |                                |
   | 1. iap_user_request_*()        |
   |    填写 RTC RAM update 区       |
   |                                |
   | 2. iap_user_request_apply()    |
   |    提交 + esp_restart()  -----> bootloader 读 RTC RAM → 启动 IAP
   |                                |
   |                                | 3. 检测 update_flags
   |                                |    写入配置区持久化
   |                                |    清除请求 + esp_restart()
   |                                |
   | <----- bootloader 启动用户程序  |
```

**可修改字段白名单**（IAP 只暴露这些）：

| API | 字段 | 说明 |
|-----|------|------|
| `iap_user_request_boot_param(s, len)` | 启动参数字符串 | `NULL` 表示清空 |
| `iap_user_request_active_slot(slot)` | OTA 槽序号 | 0 = ota_0 |
| `iap_user_request_user_addr(addr)` | 加载地址 | 0 = 默认; 非 0 须 >= 0x140000 |
| `iap_user_request_boot_target(t)` | 启动目标 | 0 = IAP, 1 = APP |

**辅助 API**：

| API | 说明 |
|-----|------|
| `iap_user_request_commit()` | 提交（重算 CRC），不重启 |
| `iap_user_request_apply()` | 提交 + 重启进 IAP（不返回） |
| `iap_user_has_update_request()` | 查询是否有待处理请求 |
| `iap_user_request_clear()` | 放弃请求 |

**用法示例**：

```c
/* 1. 填写要修改的字段 (可只填一部分) */
iap_user_request_boot_param("mode=debug;server=192.168.1.10", 0);
iap_user_request_active_slot(1);

/* 2. 提交 + 重启 (不会返回) */
iap_user_request_apply();
```

> IAP 侧对每个字段做**合法性校验**（槽序号越界、地址落入固定区等），
> 非法值会被忽略并打印警告，不会破坏配置区。

---

## 硬件 CRC 加速

RTC RAM 启动参数的 CRC 校验由 `iap_user_boot_param_valid()` 完成，
内部使用 ROM 硬件 CRC (`esp_rom_crc32_le`)，无需用户程序额外实现。

> ⚠️ v5 起用户程序**不访问配置区**，因此模板中不再有
> `iap_crc32()` / `IAP_USER_CRC_SOFTWARE` 相关代码。

### 镜像完整性

用户程序**无需自行校验镜像**。`user_app` 分区内是纯 ESP 应用镜像，
bootloader 启动时会做完整校验（magic / 段表 / SHA256 / chip_id），
失败则自动回落到 IAP，不会变砖。

---

## 典型交互流程

```
上电
  │
  ├─ bootloader 加载 IAP (factory)
  │
  ├─ IAP 读取 iap_cfg 配置
  │    ├─ 下载位被置位？ → 进入下载模式（等待烧录）
  │    ├─ 等待 N 秒，期间检查触发源（GPIO/I2C/UART/WiFi）
  │    └─ 无触发 → 校验 user_app → 写入启动参数 → 切换启动分区
  │
  └─ bootloader 加载 user_app
       ├─ iap_user_get_boot_param()   读取 IAP 传来的启动参数
       ├─ ... 正常工作 ...
       ├─ 需要升级时 → iap_user_request_download() → 重启回 IAP
       └─ 需要改配置时 → iap_user_request_apply() → 重启进 IAP 写入配置区
```

---

## 启动参数约定

IAP 在启动用户程序前会写入启动参数，格式 `key1=value1;key2=value2`。

IAP 默认写入的键：

| 键 | 含义 | 示例值 |
|----|------|--------|
| `mode` | 运行模式 | `normal` / `debug` / `factory` |
| `reason` | 进入 IAP 的原因 | `wait_timeout` / `user_request` / `trig_gpio` |
| `boot` | 启动计数 | `7` |
| `baud` | 建议通信波特率 | `921600` |

用户程序侧读取：

```c
char param[IAP_USER_BOOT_PARAM_SIZE];
if (iap_user_get_boot_param(param, sizeof(param)) == ESP_OK) {
    char mode[16];
    if (iap_user_boot_param_get_value(param, "mode", mode, sizeof(mode)) == ESP_OK) {
        // 根据 mode 初始化
    }
}
```

---

## 常见问题

**Q: 编译报错找不到 `iap_cfg` 分区？**
A: 确认 `sdkconfig.defaults` 中 `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME`
指向 `partitions_user_app.csv`，且该文件与 IAP 的 `partitions_iap.csv` 内容一致。

**Q: IAP 拒绝加载用户程序？**
A: 确认烧录的是 `idf.py build` 的原始产物（offset 0 为 `0xE9`）。
若用 esptool 手动烧录，地址必须是 `0x150000`。

**Q: 用户程序读不到配置？**
A: 确认分区类型是 `0x40, 0x00`（两个字段），只写 `0x40` 会被当成 data 子类型。

**Q: 如何回到 IAP 下载模式？**
A: 调用 `iap_user_request_download()`，它会置位下载标志并重启。

**Q: 用户程序怎么修改配置？**
A: v6 起用 `iap_user_request_*()` 填写 RTC RAM update 区，再调
`iap_user_request_apply()` 重启进 IAP。IAP 会写入配置区并重启启动用户程序。
注意只能修改白名单字段（启动参数 / OTA 槽 / 加载地址 / 启动目标）。
