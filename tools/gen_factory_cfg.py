#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ESP IAP 出厂配置生成工具

生成 iap_cfg 分区的镜像文件，用于出厂预置配置（SSID/密码/IP/I2C 引脚等），
避免设备首次上电后才用终端/HTTP 逐项配置。

生成的镜像可直接烧录:
    esptool.py --chip esp32c6 write_flash 0x10000 iap_cfg.bin

或与其它分区合并为单一镜像（见 merge_bin.py）。

配置区布局 (与 main/iap_common.h 保持一致):
    分区偏移 0x10000, 大小 8192 (2 个 4096 字节槽)
    槽 0: 偏移 0
    槽 1: 偏移 4096

槽结构:
    +----------------------------+ offset 0
    | iap_cfg_header_t (28 字节) |
    +----------------------------+ offset 28
    | iap_cfg_data_t (612 字节)  |
    +----------------------------+ offset 520
    | 0xFF 填充至 4096           |
    +----------------------------+

用法示例:
    # 用默认值生成
    python gen_factory_cfg.py -o iap_cfg.bin

    # 指定 WiFi 与 IP
    python gen_factory_cfg.py -o iap_cfg.bin \
        --ssid MyDevice --password 87654321 --ip 192.168.10.1 --netmask 255.255.255.0

    # 从 JSON 文件读取配置
    python gen_factory_cfg.py -o iap_cfg.bin --json factory.json

    # 导出当前默认配置为 JSON 模板
    python gen_factory_cfg.py --dump-template factory.json
"""

import argparse
import binascii
import json
import struct
import sys

# --- 与 main/iap_common.h 保持一致 ---

CFG_PART_SIZE = 8192        # iap_cfg 分区大小
SLOT_SIZE = 4096            # 单个槽大小
SLOT_COUNT = 2              # 槽数量

MAGIC = 0x49415043          # "IAPC"
VERSION = 0x0001            # 槽头部格式版本
CFG_DATA_VER = 0x0003       # 配置数据版本 (用于迁移)
HEADER_SIZE = 28            # sizeof(iap_cfg_header_t)
DATA_SIZE = 612             # sizeof(iap_cfg_data_t)

BOOT_PARAM_SIZE = 256

# 标志位
FLAG_DOWNLOAD_MODE = 1 << 0
FLAG_WIFI_ENABLE = 1 << 1
FLAG_I2C_ENABLE = 1 << 2
FLAG_UART_ENABLE = 1 << 3
FLAG_VERIFY_USER_APP = 1 << 4
FLAG_WAIT_GPIO_TRIG = 1 << 5
FLAG_WAIT_I2C_TRIG = 1 << 6
FLAG_WAIT_UART_TRIG = 1 << 7
FLAG_WAIT_WIFI_TRIG = 1 << 8
FLAG_USB_ENABLE = 1 << 9
FLAG_WAIT_USB_TRIG = 1 << 10

# 触发电平
TRIG_ACTIVE_LOW = 0
TRIG_ACTIVE_HIGH = 1

TRIG_GPIO_NONE = 0xFF

# 默认值（与 iap_common.h 的 IAP_CFG_DEFAULT_* 一致）
DEFAULTS = {
    "flags": (FLAG_WIFI_ENABLE | FLAG_I2C_ENABLE | FLAG_UART_ENABLE |
              FLAG_VERIFY_USER_APP | FLAG_WAIT_UART_TRIG | FLAG_WAIT_WIFI_TRIG |
              FLAG_USB_ENABLE | FLAG_WAIT_USB_TRIG),
    "wait_seconds": 3,
    "i2c_scl_gpio": 7,
    "i2c_sda_gpio": 6,
    "i2c_addr": 0x42,
    "i2c_reserved_freq": 0,   # 占位 (原 i2c_freq_hz，从机模式无效)
    "wifi_ssid": "ESP-IAP",
    "wifi_password": "12345678",
    "wifi_channel": 1,
    "wifi_ip": "192.168.4.1",
    "wifi_netmask": "255.255.255.0",
    "uart_port": 0,
    "uart_tx_gpio": 0xFF,   # 0xFF = 用该端口芯片默认引脚
    "uart_rx_gpio": 0xFF,
    "uart_baudrate": 115200,
    "trig_gpio": TRIG_GPIO_NONE,
    "trig_gpio_level": TRIG_ACTIVE_LOW,
    "boot_param": "",
}


def crc32(data: bytes) -> int:
    """CRC32 (IEEE 802.3)，与设备端 iap_crc32() 一致。"""
    return binascii.crc32(data) & 0xFFFFFFFF


def ip_to_u32(ip: str) -> int:
    """点分十进制 -> 小端 u32 (a | b<<8 | c<<16 | d<<24)。"""
    parts = ip.strip().split(".")
    if len(parts) != 4:
        raise ValueError("IP 格式错误: %s" % ip)
    a, b, c, d = (int(x) for x in parts)
    for v in (a, b, c, d):
        if not 0 <= v <= 255:
            raise ValueError("IP 数值越界: %s" % ip)
    return a | (b << 8) | (c << 16) | (d << 24)


def u32_to_ip(v: int) -> str:
    return "%d.%d.%d.%d" % (v & 0xFF, (v >> 8) & 0xFF,
                            (v >> 16) & 0xFF, (v >> 24) & 0xFF)


def build_data(cfg: dict) -> bytes:
    """构造 iap_cfg_data_t (612 字节)。

    偏移表 (见 iap_common.h 注释):
        flags(4)@0, wait_seconds(2)@4, boot_fail_reserved(2)@6,
        user_app_size(4)@8, user_app_crc32(4)@12, user_app_version(4)@16,
        boot_count(4)@20, last_boot_reason(4)@24,
        i2c_scl_gpio(1)@28, i2c_sda_gpio(1)@29, i2c_addr(1)@30,
        i2c_reserved(1)@31, i2c_reserved_freq(4)@32, i2c_reserved1(8)@36,
        wifi_ssid(32)@44, wifi_password(64)@76, wifi_channel(1)@140,
        wifi_reserved(3)@141, wifi_ip(4)@144, wifi_netmask(4)@148,
        wifi_reserved1(8)@152,
        uart_port(1)@160, uart_tx_gpio(1)@161, uart_rx_gpio(1)@162,
        uart_reserved(1)@163, uart_baudrate(4)@164,
        uart_reserved1(8)@168,
        trig_gpio(1)@176, trig_gpio_level(1)@177, trig_gpio_reserved(2)@178,
        trig_gpio_reserved1(8)@180,
        usb_reserved0(4)@188, usb_reserved1(8)@192,
        boot_param(256)@200, boot_param_len(2)@456, boot_param_flags(1)@458,
        boot_param_reserved(1)@459, boot_param_crc32(4)@460,
        boot_param_reserved1(8)@464,
        active_slot(1)@472, ota_slot_count(1)@473, ota_reserved0(2)@474,
        ota_gpio(1)@476, ota_gpio_reserved(3)@477,
        slot_addr(64)@480, slot_size(64)@544,
        data_ver(2)@608, data_ver_reserved(2)@610
    """
    b = bytearray(DATA_SIZE)

    # --- 标志与等待 ---
    struct.pack_into("<I", b, 0, int(cfg["flags"]))
    struct.pack_into("<H", b, 4, int(cfg["wait_seconds"]))

    # --- 用户程序信息 (出厂时为空) ---
    struct.pack_into("<I", b, 8, 0)     # user_app_size
    struct.pack_into("<I", b, 12, 0)    # user_app_crc32
    struct.pack_into("<I", b, 16, 0)    # user_app_version
    struct.pack_into("<I", b, 20, 0)    # boot_count
    struct.pack_into("<I", b, 24, 0)    # last_boot_reason

    # --- I2C ---
    b[28] = int(cfg["i2c_scl_gpio"]) & 0xFF
    b[29] = int(cfg["i2c_sda_gpio"]) & 0xFF
    b[30] = int(cfg["i2c_addr"]) & 0xFF
    struct.pack_into("<I", b, 32, int(cfg["i2c_reserved_freq"]))

    # --- WiFi ---
    ssid = str(cfg["wifi_ssid"]).encode("utf-8")[:31]
    b[44:44 + len(ssid)] = ssid
    pw = str(cfg["wifi_password"]).encode("utf-8")[:63]
    b[76:76 + len(pw)] = pw
    b[140] = int(cfg["wifi_channel"]) & 0xFF
    struct.pack_into("<I", b, 144, ip_to_u32(cfg["wifi_ip"]))
    struct.pack_into("<I", b, 148, ip_to_u32(cfg["wifi_netmask"]))

    # --- UART ---
    b[160] = int(cfg["uart_port"]) & 0xFF
    b[161] = int(cfg["uart_tx_gpio"]) & 0xFF
    b[162] = int(cfg["uart_rx_gpio"]) & 0xFF
    struct.pack_into("<I", b, 164, int(cfg["uart_baudrate"]))

    # --- 触发引脚 ---
    b[176] = int(cfg["trig_gpio"]) & 0xFF
    b[177] = int(cfg["trig_gpio_level"]) & 0xFF

    # --- 启动参数字符串 ---
    bp = str(cfg.get("boot_param", "")).encode("utf-8")
    if len(bp) > BOOT_PARAM_SIZE - 1:
        bp = bp[:BOOT_PARAM_SIZE - 1]
    b[200:200 + len(bp)] = bp
    struct.pack_into("<H", b, 456, len(bp))     # boot_param_len
    b[458] = 0                                   # boot_param_flags
    if bp:
        struct.pack_into("<I", b, 460, crc32(bp))  # boot_param_crc32

    # --- 多 OTA 槽 ---
    b[472] = int(cfg.get("active_slot", 0)) & 0xFF
    b[473] = int(cfg.get("ota_slot_count", 1)) & 0xFF
    b[476] = int(cfg.get("ota_gpio", 0xFF)) & 0xFF

    # --- 槽加载地址/大小 (v7, uint64 LE) ---
    # 默认: 槽 0 = 0x150000, 大小 = 到 4MB 末尾; 其余为 0
    slot_addr = cfg.get("slot_addr") or [0x150000]
    slot_size = cfg.get("slot_size") or [0x400000 - 0x150000]
    for i in range(8):
        a = int(slot_addr[i]) if i < len(slot_addr) else 0
        s = int(slot_size[i]) if i < len(slot_size) else 0
        struct.pack_into("<Q", b, 480 + i * 8, a)
        struct.pack_into("<Q", b, 544 + i * 8, s)

    # --- 配置数据版本 ---
    struct.pack_into("<H", b, 608, int(cfg.get("data_ver", CFG_DATA_VER)))

    return bytes(b)


def build_slot(data: bytes, seq: int, erase: bool = False) -> bytes:
    """构造完整配置槽 (4096 字节)。

    erase=True 时返回全 0xFF（表示空槽）。
    """
    if erase:
        return b"\xff" * SLOT_SIZE

    hdr = bytearray(HEADER_SIZE)
    struct.pack_into("<I", hdr, 0, MAGIC)
    struct.pack_into("<H", hdr, 4, VERSION)
    struct.pack_into("<H", hdr, 6, HEADER_SIZE)
    struct.pack_into("<I", hdr, 8, seq)
    struct.pack_into("<I", hdr, 12, DATA_SIZE)
    struct.pack_into("<I", hdr, 16, crc32(data))
    # header_crc32 覆盖 [0..20)，即 magic/version/header_size/seq/data_size/data_crc32
    struct.pack_into("<I", hdr, 20, crc32(bytes(hdr[:20])))
    # reserved @24 = 0

    slot = bytearray(SLOT_SIZE)
    slot[0:HEADER_SIZE] = hdr
    slot[HEADER_SIZE:HEADER_SIZE + DATA_SIZE] = data
    for i in range(HEADER_SIZE + DATA_SIZE, SLOT_SIZE):
        slot[i] = 0xFF
    return bytes(slot)


def build_partition(cfg: dict) -> bytes:
    """构造完整 iap_cfg 分区镜像 (8192 字节)。

    槽 0 写入配置 (seq=1)，槽 1 保持擦除态。
    设备读取时会选择 CRC 正确且 seq 最大的槽，因此槽 0 生效。
    """
    img = bytearray()
    img += build_slot(build_data(cfg), seq=1)
    img += build_slot(b"", seq=0, erase=True)
    assert len(img) == CFG_PART_SIZE, \
        "分区镜像大小错误: %d != %d" % (len(img), CFG_PART_SIZE)
    return bytes(img)


def load_json(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as f:
        user = json.load(f)
    cfg = dict(DEFAULTS)
    cfg.update(user)
    return cfg


def dump_template(path: str) -> None:
    tpl = dict(DEFAULTS)
    tpl["_comment"] = (
        "flags 位: bit0=强制下载模式 bit1=WiFi bit2=I2C bit3=UART "
        "bit4=启动校验 bit5=GPIO触发 bit6=I2C触发 bit7=UART触发 bit8=WiFi触发"
    )
    tpl["_trig_gpio"] = "0xFF 表示未配置；否则填引脚号"
    tpl["_trig_gpio_level"] = "0=低电平有效(内部上拉), 1=高电平有效(内部下拉)"
    with open(path, "w", encoding="utf-8") as f:
        json.dump(tpl, f, indent=2, ensure_ascii=False)
    print("已导出配置模板: %s" % path)


def main() -> int:
    ap = argparse.ArgumentParser(
        description="生成 ESP IAP 出厂配置镜像 (iap_cfg 分区)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument("-o", "--output", help="输出镜像文件 (8192 字节)")
    ap.add_argument("--json", help="从 JSON 文件读取配置")
    ap.add_argument("--dump-template", metavar="FILE",
                    help="导出默认配置为 JSON 模板后退出")

    ap.add_argument("--flags", type=lambda s: int(s, 0),
                    help="配置标志位 (十六进制或十进制)")
    ap.add_argument("--wait", type=int, dest="wait_seconds",
                    help="启动等待秒数")
    ap.add_argument("--ssid", dest="wifi_ssid", help="WiFi AP SSID")
    ap.add_argument("--password", dest="wifi_password", help="WiFi AP 密码")
    ap.add_argument("--channel", type=int, dest="wifi_channel", help="WiFi 信道")
    ap.add_argument("--ip", dest="wifi_ip", help="AP IP 地址")
    ap.add_argument("--netmask", dest="wifi_netmask", help="AP 子网掩码")
    ap.add_argument("--scl", type=int, dest="i2c_scl_gpio", help="I2C SCL 引脚")
    ap.add_argument("--sda", type=int, dest="i2c_sda_gpio", help="I2C SDA 引脚")
    ap.add_argument("--i2c-addr", type=lambda s: int(s, 0), dest="i2c_addr",
                    help="I2C 从机地址")
    ap.add_argument("--uart-port", type=int, dest="uart_port", help="UART 端口 (0/1/2)")
    ap.add_argument("--uart-tx", type=lambda s: int(s, 0), dest="uart_tx_gpio",
                    help="UART TX 引脚 (0xFF=默认)")
    ap.add_argument("--uart-rx", type=lambda s: int(s, 0), dest="uart_rx_gpio",
                    help="UART RX 引脚 (0xFF=默认)")
    ap.add_argument("--baud", type=int, dest="uart_baudrate", help="UART 波特率")
    ap.add_argument("--trig-gpio", type=lambda s: int(s, 0), dest="trig_gpio",
                    help="触发引脚 (0xFF=未配置)")
    ap.add_argument("--trig-level", type=int, dest="trig_gpio_level",
                    help="触发有效电平 (0=低, 1=高)")
    ap.add_argument("--boot-param", dest="boot_param",
                    help="启动参数字符串 (IAP->用户程序)")

    args = ap.parse_args()

    if args.dump_template:
        dump_template(args.dump_template)
        return 0

    if not args.output:
        ap.error("需要 -o/--output 指定输出文件 (或使用 --dump-template)")

    # 起始配置: 默认值 -> JSON -> 命令行覆盖
    cfg = dict(DEFAULTS)
    if args.json:
        cfg = load_json(args.json)

    for key in ("flags", "wait_seconds", "wifi_ssid", "wifi_password",
                "wifi_channel", "wifi_ip", "wifi_netmask",
                "i2c_scl_gpio", "i2c_sda_gpio", "i2c_addr",
                "uart_port", "uart_tx_gpio", "uart_rx_gpio", "uart_baudrate",
                "trig_gpio", "trig_gpio_level",
                "boot_param"):
        v = getattr(args, key, None)
        if v is not None:
            cfg[key] = v

    # 校验
    try:
        ip_to_u32(cfg["wifi_ip"])
        ip_to_u32(cfg["wifi_netmask"])
    except ValueError as e:
        print("错误: %s" % e, file=sys.stderr)
        return 1

    if not 1 <= int(cfg["wifi_channel"]) <= 14:
        print("错误: WiFi 信道应在 1..14", file=sys.stderr)
        return 1

    if len(str(cfg["wifi_password"])) > 63:
        print("错误: WiFi 密码最长 63 字节", file=sys.stderr)
        return 1
    if str(cfg["wifi_password"]) and len(str(cfg["wifi_password"])) < 8:
        print("警告: WiFi 密码少于 8 字符，部分客户端可能拒绝连接",
              file=sys.stderr)

    if len(str(cfg["wifi_ssid"])) > 31:
        print("错误: WiFi SSID 最长 31 字节", file=sys.stderr)
        return 1

    data = build_data(cfg)
    img = build_partition(cfg)

    with open(args.output, "wb") as f:
        f.write(img)

    # --- 输出摘要 ---
    print("已生成出厂配置镜像: %s (%d 字节)" % (args.output, len(img)))
    print()
    print("  配置槽 0: seq=1 (生效)")
    print("  配置槽 1: 擦除态")
    print()
    print("  flags           = 0x%08X" % cfg["flags"])
    fl = []
    if cfg["flags"] & FLAG_DOWNLOAD_MODE:   fl.append("强制下载模式")
    if cfg["flags"] & FLAG_WIFI_ENABLE:     fl.append("WiFi")
    if cfg["flags"] & FLAG_I2C_ENABLE:      fl.append("I2C")
    if cfg["flags"] & FLAG_UART_ENABLE:     fl.append("UART")
    if cfg["flags"] & FLAG_VERIFY_USER_APP: fl.append("启动校验")
    if cfg["flags"] & FLAG_WAIT_GPIO_TRIG:  fl.append("GPIO触发")
    if cfg["flags"] & FLAG_WAIT_I2C_TRIG:   fl.append("I2C触发")
    if cfg["flags"] & FLAG_WAIT_UART_TRIG:  fl.append("UART触发")
    if cfg["flags"] & FLAG_WAIT_WIFI_TRIG:  fl.append("WiFi触发")
    print("    -> %s" % (", ".join(fl) if fl else "(无)"))
    print("  wait_seconds    = %d" % cfg["wait_seconds"])
    print("  WiFi SSID       = %r" % cfg["wifi_ssid"])
    print("  WiFi 密码       = %r" % cfg["wifi_password"])
    print("  WiFi 信道       = %d" % cfg["wifi_channel"])
    print("  AP IP           = %s" % cfg["wifi_ip"])
    print("  AP 掩码         = %s" % cfg["wifi_netmask"])
    print("  I2C SCL/SDA     = GPIO%d / GPIO%d" % (cfg["i2c_scl_gpio"], cfg["i2c_sda_gpio"]))
    print("  I2C 地址       = 0x%02X" % cfg["i2c_addr"])
    def pin_str(v):
        return "默认" if v == 0xFF else "GPIO%d" % v
    print("  UART 端口/波特率= %d / %d" % (cfg["uart_port"], cfg["uart_baudrate"]))
    print("  UART TX/RX      = %s / %s" % (pin_str(cfg["uart_tx_gpio"]),
                                            pin_str(cfg["uart_rx_gpio"])))
    if cfg["trig_gpio"] == TRIG_GPIO_NONE:
        print("  触发引脚        = 未配置")
    else:
        print("  触发引脚        = GPIO%d (%s有效)" %
              (cfg["trig_gpio"], "低电平" if cfg["trig_gpio_level"] == 0 else "高电平"))
    if cfg["boot_param"]:
        print("  启动参数        = %r" % cfg["boot_param"])
    print()
    print("  data_crc32      = 0x%08X" % crc32(data))
    print()
    print("烧录命令:")
    print("  esptool.py --chip <chip> -p <port> write_flash 0x10000 %s" % args.output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
