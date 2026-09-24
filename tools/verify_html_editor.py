#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
交叉验证 HTML 编辑器的编解码逻辑与 Python 工具一致。

做法: 从 esp_iap_tool.html 中提取 JS 常量与算法要点，
      用 Python 重新实现同样的逻辑，然后与 gen_factory_cfg.py 的输出逐字节比对。

用法: python verify_html_editor.py
"""
import binascii
import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# HTML 编辑器位于项目根目录（tools 的上一级）
HTML = os.path.join(os.path.dirname(HERE), "esp_iap_tool.html")
GEN = os.path.join(HERE, "gen_factory_cfg.py")

SLOT_SIZE = 4096
HEADER_SIZE = 28
DATA_SIZE = 612
CFG_MAGIC = 0x49415043
CFG_VERSION = 1
BOOT_PARAM_SIZE = 256
OTA_SLOT_MAX = 8


def crc32(data):
    return binascii.crc32(data) & 0xFFFFFFFF


def ip_to_u32(s):
    a, b, c, d = (int(x) for x in s.split("."))
    return a | (b << 8) | (c << 16) | (d << 24)


def encode_data(cfg):
    """与 HTML 中 encodeData() 相同逻辑。"""
    b = bytearray(DATA_SIZE)
    struct.pack_into("<I", b, 0, cfg["flags"])
    struct.pack_into("<H", b, 4, cfg["wait_seconds"])
    # 8..27 保持 0
    b[28] = cfg["i2c_scl_gpio"] & 0xFF
    b[29] = cfg["i2c_sda_gpio"] & 0xFF
    b[30] = cfg["i2c_addr"] & 0xFF
    # 32..35 占位 (原 i2c_freq_hz，从机模式无效)
    ssid = cfg["wifi_ssid"].encode("utf-8")[:31]
    b[44:44 + len(ssid)] = ssid
    pw = cfg["wifi_password"].encode("utf-8")[:63]
    b[76:76 + len(pw)] = pw
    b[140] = cfg["wifi_channel"] & 0xFF
    struct.pack_into("<I", b, 144, ip_to_u32(cfg["wifi_ip"]))
    struct.pack_into("<I", b, 148, ip_to_u32(cfg["wifi_netmask"]))
    b[160] = cfg["uart_port"] & 0xFF
    b[161] = cfg["uart_tx_gpio"] & 0xFF
    b[162] = cfg["uart_rx_gpio"] & 0xFF
    struct.pack_into("<I", b, 164, cfg["uart_baudrate"])
    b[176] = cfg["trig_gpio"] & 0xFF
    b[177] = cfg["trig_gpio_level"] & 0xFF
    bp = cfg.get("boot_param", "").encode("utf-8")[:BOOT_PARAM_SIZE - 1]
    b[200:200 + len(bp)] = bp
    struct.pack_into("<H", b, 456, len(bp))
    b[458] = 0
    b[459] = 0
    struct.pack_into("<I", b, 460, crc32(b[200:200 + len(bp)]) if bp else 0)

    # 多 OTA 槽
    b[472] = int(cfg.get("active_slot", 0)) & 0xFF
    b[473] = int(cfg.get("ota_slot_count", 1)) & 0xFF
    b[476] = int(cfg.get("ota_gpio", 0xFF)) & 0xFF

    # 槽加载地址/大小 (off 480 / 544, uint64 LE，高 32 位恒 0)
    #
    # 默认值与 gen_factory_cfg.py 保持一致:
    #   槽 0 = 0x150000，大小 = 到 4MB 末尾；其余为 0
    addrs = cfg.get("slot_addr") or [0x150000]
    sizes = cfg.get("slot_size") or [0x400000 - 0x150000]
    for i in range(OTA_SLOT_MAX):
        a = int(addrs[i] if i < len(addrs) else 0) & 0xFFFFFFFF
        s = int(sizes[i] if i < len(sizes) else 0) & 0xFFFFFFFF
        struct.pack_into("<I", b, 480 + i * 8, a)
        struct.pack_into("<I", b, 480 + i * 8 + 4, 0)
        struct.pack_into("<I", b, 544 + i * 8, s)
        struct.pack_into("<I", b, 544 + i * 8 + 4, 0)

    # 配置数据版本
    struct.pack_into("<H", b, 608, int(cfg.get("data_ver", 0x0003)))
    return bytes(b)


def build_slot(data, seq, erase=False):
    if erase:
        return b"\xff" * SLOT_SIZE
    hdr = bytearray(HEADER_SIZE)
    struct.pack_into("<I", hdr, 0, CFG_MAGIC)
    struct.pack_into("<H", hdr, 4, CFG_VERSION)
    struct.pack_into("<H", hdr, 6, HEADER_SIZE)
    struct.pack_into("<I", hdr, 8, seq)
    struct.pack_into("<I", hdr, 12, DATA_SIZE)
    struct.pack_into("<I", hdr, 16, crc32(data))
    struct.pack_into("<I", hdr, 20, crc32(bytes(hdr[:20])))
    slot = bytearray(SLOT_SIZE)
    slot[0:HEADER_SIZE] = hdr
    slot[HEADER_SIZE:HEADER_SIZE + DATA_SIZE] = data
    for i in range(HEADER_SIZE + DATA_SIZE, SLOT_SIZE):
        slot[i] = 0xFF
    return bytes(slot)


def build_partition(cfg):
    return build_slot(encode_data(cfg), 1) + build_slot(b"", 0, erase=True)


def main():
    if not os.path.isfile(HTML):
        print("找不到 %s" % HTML)
        return 1

    html = open(HTML, "r", encoding="utf-8").read()

    print("=== 1. 检查 HTML 中的常量与 C 头文件一致 ===")
    checks = [
        ("CFG_MAGIC", "0x49415043"),
        ("SLOT_SIZE", "4096"),
        ("HEADER_SIZE", "28"),
        ("DATA_SIZE", "612"),
        ("BOOT_PARAM_SIZE", "256"),
        # v5 双分区表布局
        ("CFG_OFFSET_IN_IMAGE", "0x008000"),
        ("IAP_APP_OFFSET", "0x010000"),
        ("PARTITION_TABLE_A_OFFSET", "0x00B000"),
        ("PARTITION_TABLE_B_OFFSET", "0x140000"),
        ("USER_APP_ADDR", "0x150000"),
    ]
    ok = True
    for name, expect in checks:
        m = re.search(r"const\s+%s\s*=\s*([^;]+);" % name, html)
        if not m:
            print("  [FAIL] 未找到 %s" % name)
            ok = False
            continue
        val = m.group(1).strip()
        got = val.replace("_", "")
        # 允许 4096*2 这类表达式
        if name == "CFG_PART_SIZE":
            continue
        match = (expect.lower() in got.lower())
        print("  [%s] %s = %s" % ("PASS" if match else "FAIL", name, val))
        if not match:
            ok = False

    print()
    print("=== 2. 检查关键偏移与 iap_common.h 一致 ===")
    offsets = [
        ("flags", "dv.setUint32(0,", True),
        ("wait_seconds", "dv.setUint16(4,", True),
        ("i2c_scl_gpio", "b[28] =", False),
        ("i2c_sda_gpio", "b[29] =", False),
        ("i2c_addr", "b[30] =", False),
        ("i2c_addr", "b[30] =", True),
        ("wifi_ssid", "writeCStr(b, 44, 32,", False),
        ("wifi_password", "writeCStr(b, 76, 64,", False),
        ("wifi_channel", "b[140] =", False),
        ("wifi_ip", "dv.setUint32(144,", True),
        ("wifi_netmask", "dv.setUint32(148,", True),
        ("uart_port", "b[160] =", False),
        ("uart_baudrate", "dv.setUint32(164,", True),
        ("trig_gpio", "b[176] =", False),
        ("trig_gpio_level", "b[177] =", False),
        ("boot_param", "subarray(0, bpLen), 200)", True),
        ("boot_param_len", "dv.setUint16(456,", True),
        ("boot_param_crc32", "dv.setUint32(460,", True),
        ("active_slot", "b[472] =", False),
        ("ota_slot_count", "b[473] =", False),
        ("ota_gpio", "b[476] =", False),
        ("slot_addr", "dv.setUint32(480 + i * 8,", True),
        ("slot_size", "dv.setUint32(544 + i * 8,", True),
        ("data_ver", "dv.setUint16(608,", True),
    ]
    for name, pat, _ in offsets:
        found = pat in html
        print("  [%s] %s -> %s" % ("PASS" if found else "FAIL", name, pat))
        if not found:
            ok = False

    print()
    print("=== 3. 逐字节比对: HTML 逻辑 vs gen_factory_cfg.py ===")

    test_cases = [
        {
            "name": "默认值",
            "cfg": {
                "flags": 0x79E, "wait_seconds": 3,
                "wifi_ssid": "ESP-IAP", "wifi_password": "12345678",
                "wifi_channel": 1, "wifi_ip": "192.168.4.1",
                "wifi_netmask": "255.255.255.0",
                "i2c_scl_gpio": 7, "i2c_sda_gpio": 6,
                "i2c_addr": 0x42,
                "uart_port": 0, "uart_tx_gpio": 0xFF, "uart_rx_gpio": 0xFF,
                "uart_baudrate": 115200,
                "trig_gpio": 0xFF, "trig_gpio_level": 0,
                "boot_param": "",
            },
            "args": [],
        },
        {
            "name": "自定义",
            "cfg": {
                "flags": 0x1E, "wait_seconds": 5,
                "wifi_ssid": "MyDevice-01", "wifi_password": "MyPass2026",
                "wifi_channel": 6, "wifi_ip": "192.168.10.1",
                "wifi_netmask": "255.255.0.0",
                "i2c_scl_gpio": 5, "i2c_sda_gpio": 4,
                "i2c_addr": 0x55,
                "uart_port": 1, "uart_tx_gpio": 17, "uart_rx_gpio": 16,
                "uart_baudrate": 921600,
                "trig_gpio": 9, "trig_gpio_level": 1,
                "boot_param": "mode=factory;site=A1",
            },
            "args": [
                "--flags", "0x1E",
                "--ssid", "MyDevice-01", "--password", "MyPass2026",
                "--ip", "192.168.10.1", "--netmask", "255.255.0.0",
                "--channel", "6", "--scl", "5", "--sda", "4",
                "--i2c-addr", "0x55", "--uart-port", "1",
                "--uart-tx", "17", "--uart-rx", "16",
                "--baud", "921600", "--wait", "5",
                "--trig-gpio", "9", "--trig-level", "1",
                "--boot-param", "mode=factory;site=A1",
            ],
        },
    ]

    tmp = os.path.join(HERE, "_xcheck_cfg.bin")
    for tc in test_cases:
        # Python 工具输出
        cmd = [sys.executable, GEN, "-o", tmp] + tc["args"]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print("  [FAIL] %s: gen_factory_cfg.py 失败\n%s" % (tc["name"], r.stderr))
            ok = False
            continue
        py_bytes = open(tmp, "rb").read()

        # HTML 逻辑输出
        js_bytes = build_partition(tc["cfg"])

        if py_bytes == js_bytes:
            print("  [PASS] %s: 8192 字节完全一致" % tc["name"])
        else:
            print("  [FAIL] %s: 不一致" % tc["name"])
            ok = False
            for i in range(0, len(py_bytes), 16):
                a = py_bytes[i:i + 16]
                b = js_bytes[i:i + 16]
                if a != b:
                    print("        首个差异 @0x%04X" % i)
                    print("          py: %s" % a.hex(" "))
                    print("          js: %s" % b.hex(" "))
                    break

    if os.path.isfile(tmp):
        os.remove(tmp)

    print()
    if ok:
        print("=== 结果: 全部通过 ===")
        return 0
    print("=== 结果: 存在失败项 ===")
    return 1


if __name__ == "__main__":
    sys.exit(main())
