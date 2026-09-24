#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
CRC 算法一致性验证脚本

验证 PC 侧工具使用的 CRC 算法与设备端 iap_crc.c 的实现完全一致。
用于排查"设备校验失败"类问题。

设备端 iap_crc.c 默认走 ROM 硬件 CRC 加速器，本脚本同时模拟：
  1. 软件路径 (4bit 查表法 / 按位实现) —— IAP_CRC32_SOFTWARE=1 时使用
  2. 硬件路径 (esp_rom_crc32_le / esp_rom_crc16_be) —— 默认使用

软件路径的 CRC32 实现 (4bit 查表法):
    uint32_t iap_crc32_update(uint32_t crc, const void *data, size_t len)
    {
        const uint8_t *p = data;
        while (len--) {
            crc ^= *p++;
            crc = (crc >> 4) ^ table[crc & 0x0F];
            crc = (crc >> 4) ^ table[crc & 0x0F];
        }
        return crc;
    }
    uint32_t iap_crc32(const void *data, size_t len)
    {
        return iap_crc32_update(0xFFFFFFFF, data, len) ^ 0xFFFFFFFF;
    }

两条路径必须产生完全相同的结果（本脚本会交叉验证），
且都与 binascii.crc32 / 标准 CRC-16/CCITT-FALSE 一致。

用法:
    python tools/test_crc.py
"""

import binascii
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from iap_xmodem_send import crc16

# 与 iap_crc.c 中的 s_crc32_nibble_table 完全一致
CRC32_NIBBLE_TABLE = [
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC,
    0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
    0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
]


def dev_crc32_update(crc: int, data: bytes) -> int:
    """复现设备端 iap_crc32_update()。"""
    for b in data:
        crc ^= b
        crc = (crc >> 4) ^ CRC32_NIBBLE_TABLE[crc & 0x0F]
        crc = (crc >> 4) ^ CRC32_NIBBLE_TABLE[crc & 0x0F]
    return crc & 0xFFFFFFFF


def dev_crc32(data: bytes) -> int:
    """复现设备端 iap_crc32()。"""
    return dev_crc32_update(0xFFFFFFFF, data) ^ 0xFFFFFFFF


# ============================================================================
# ROM 硬件 CRC 模拟
#
# 设备端默认走 ROM 硬件 CRC。ROM 源码 (components/esp_rom/patches/
# esp_rom_crc.c) 的语义是「进入时取反、返回时再取反」:
#
#   uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *buf, uint32_t len) {
#       crc = ~crc;
#       for (i...) crc = TBL[(crc ^ buf[i]) & 0xff] ^ (crc >> 8);
#       return ~crc;
#   }
#   uint16_t esp_rom_crc16_be(uint16_t crc, const uint8_t *buf, uint32_t len) {
#       crc = ~crc;
#       for (i...) crc = TBL[(crc >> 8) ^ buf[i]] ^ (crc << 8);
#       return ~crc;
#   }
#
# 由此可推导出设备端 iap_crc.c 使用的等价调用:
#   CRC32: iap_crc32(d)          == esp_rom_crc32_le(0, d, len)
#          iap_crc32_update(c,d) == esp_rom_crc32_le(c, d, len)
#   CRC16: iap_crc16(d)          == ~esp_rom_crc16_be(0, d, len)
#          iap_crc16_update(c,d) == ~esp_rom_crc16_be(~c, d, len)   <- 双重取反
# ============================================================================

def _crc32_le_table() -> list:
    """标准反射 CRC32 表 (poly 0xEDB88320)。"""
    tbl = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ 0xEDB88320 if c & 1 else c >> 1
        tbl.append(c)
    return tbl


def _crc16_be_table() -> list:
    """非反射 CRC16 表 (poly 0x1021)。"""
    tbl = []
    for i in range(256):
        c = i << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
        tbl.append(c)
    return tbl


_CRC32_TBL = _crc32_le_table()
_CRC16_TBL = _crc16_be_table()


def rom_crc32_le(crc: int, data: bytes) -> int:
    """模拟 esp_rom_crc32_le()。"""
    crc = (~crc) & 0xFFFFFFFF
    for b in data:
        crc = _CRC32_TBL[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return (~crc) & 0xFFFFFFFF


def rom_crc16_be(crc: int, data: bytes) -> int:
    """模拟 esp_rom_crc16_be()。"""
    crc = (~crc) & 0xFFFF
    for b in data:
        crc = _CRC16_TBL[(crc >> 8) ^ b] ^ ((crc << 8) & 0xFFFF)
    return (~crc) & 0xFFFF


def hw_crc32(data: bytes) -> int:
    """设备端硬件路径 iap_crc32()。"""
    return rom_crc32_le(0, data)


def hw_crc32_update(crc: int, data: bytes) -> int:
    """设备端硬件路径 iap_crc32_update()。"""
    return rom_crc32_le(crc, data)


def hw_crc16(data: bytes) -> int:
    """设备端硬件路径 iap_crc16()。"""
    return (~rom_crc16_be(0, data)) & 0xFFFF


def hw_crc16_update(crc: int, data: bytes) -> int:
    """设备端硬件路径 iap_crc16_update()（注意双重取反）。"""
    return (~rom_crc16_be((~crc) & 0xFFFF, data)) & 0xFFFF


def check(name: str, actual: int, expected: int) -> bool:
    ok = actual == expected
    mark = "PASS" if ok else "FAIL"
    print(f"[{mark}] {name}: 0x{actual:08X} (期望 0x{expected:08X})")
    return ok


def main() -> int:
    print("=== CRC 算法一致性验证 ===\n")

    all_ok = True

    # --- CRC32 标准测试向量 ---
    print("CRC32 (IEEE 802.3, 反射多项式 0xEDB88320):")
    all_ok &= check('设备实现 CRC32("123456789")', dev_crc32(b"123456789"), 0xCBF43926)
    all_ok &= check("设备实现 CRC32(空)", dev_crc32(b""), 0x00000000)
    all_ok &= check('设备实现 CRC32("a")', dev_crc32(b"a"), 0xE8B7BE43)
    all_ok &= check('设备实现 CRC32("abc")', dev_crc32(b"abc"), 0x352441C2)

    # --- 设备实现 vs Python binascii 交叉验证 ---
    print("\n设备实现 vs binascii.crc32 交叉验证:")
    samples = [
        b"", b"a", b"abc", b"123456789",
        bytes(range(256)),
        os.urandom(1024),
        os.urandom(65536),
    ]
    for i, s in enumerate(samples):
        d = dev_crc32(s)
        p = binascii.crc32(s) & 0xFFFFFFFF
        all_ok &= check(f"样本 {i} ({len(s)} 字节)", d, p)

    # --- CRC16-CCITT 标准测试向量 ---
    print("\nCRC16-CCITT (多项式 0x1021, 初值 0xFFFF, 不反射):")
    all_ok &= check('CRC16("123456789")', crc16(b"123456789"), 0x29B1)
    all_ok &= check("CRC16(空)", crc16(b""), 0xFFFF)
    all_ok &= check('CRC16("A")', crc16(b"A"), 0xB915)
    all_ok &= check('CRC16("abc")', crc16(b"abc"), 0x514A)

    # --- 增量计算一致性 ---
    print("\n增量计算一致性 (分片 vs 一次性):")
    data = bytes(range(256)) * 8

    # CRC32 增量 (使用设备算法)
    crc = 0xFFFFFFFF
    for i in range(0, len(data), 100):
        crc = dev_crc32_update(crc, data[i:i + 100])
    inc_crc32 = crc ^ 0xFFFFFFFF
    all_ok &= check("CRC32 增量", inc_crc32, dev_crc32(data))

    # CRC16 增量
    crc = 0xFFFF
    for i in range(0, len(data), 100):
        for b in data[i:i + 100]:
            crc ^= b << 8
            for _ in range(8):
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 \
                      else (crc << 1) & 0xFFFF
    all_ok &= check("CRC16 增量", crc, crc16(data))

    # --- 硬件路径 vs 软件路径等价性 ---
    print("\n硬件路径 (ROM) vs 软件路径 等价性:")
    samples2 = [
        b"", b"a", b"123456789",
        bytes(range(256)),
        os.urandom(1000),
        os.urandom(65536),
    ]
    for i, s in enumerate(samples2):
        all_ok &= check(f"CRC32 硬件 vs 软件 样本{i} ({len(s)}B)",
                        hw_crc32(s), dev_crc32(s))
    for i, s in enumerate(samples2):
        all_ok &= check(f"CRC16 硬件 vs 软件 样本{i} ({len(s)}B)",
                        hw_crc16(s), crc16(s))

    # --- 硬件路径标准测试向量 ---
    print("\n硬件路径标准测试向量:")
    all_ok &= check('硬件 CRC32("123456789")', hw_crc32(b"123456789"), 0xCBF43926)
    all_ok &= check('硬件 CRC16("123456789")', hw_crc16(b"123456789"), 0x29B1)

    # --- 硬件路径增量 (链式) 一致性 ---
    print("\n硬件路径增量 (多分片大小) 一致性:")
    big = os.urandom(10000)
    for chunk in (1, 7, 128, 4096):
        c32 = 0
        c16 = 0xFFFF
        for i in range(0, len(big), chunk):
            c32 = hw_crc32_update(c32, big[i:i + chunk])
            c16 = hw_crc16_update(c16, big[i:i + chunk])
        all_ok &= check(f"CRC32 链式 chunk={chunk}", c32, hw_crc32(big))
        all_ok &= check(f"CRC16 链式 chunk={chunk}", c16, hw_crc16(big))

    print("\n" + ("=== 全部通过 ===" if all_ok else "=== 存在失败项 ==="))
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
