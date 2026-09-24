#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ESP IAP 分区表生成工具

根据目标 flash 容量自动生成分区表 CSV / 二进制，并把多出来的空间
全部分配给 `user_app` 分区。

设计目标
--------
IAP 固件固定不变，但模块的 flash 容量可能不同 (4MB / 8MB / 16MB)。
本工具按 flash 容量生成对应的分区表，**只改变 `user_app` 分区大小**，
其余分区的偏移与大小完全固定，从而保证:

  - 一份 IAP 固件代码适配所有模块
  - 用户程序可用空间随 flash 容量自动最大化
  - 分区表可单独烧录 (0x8000)，无需重烧整个固件

允许的修改 (仅此两种)
--------------------
  ✅ 1. 修改 `user_app` 分区的 Size
  ✅ 2. 在 `user_app` **之后**追加新分区

  ✅2 的典型用途: 用户程序需要小型文件系统 (LittleFS / SPIFFS / FAT)
  时，缩小 user_app 腾出空间，在它后面追加一个 data 分区:

      user_app,   app,     ota_0,   0x150000, 0x2A0000,
      storage,    data,    spiffs,  0x3F0000, 0x10000,

禁止的修改
----------
  ❌ 修改 `iap` (表 A) / `user_app` (表 B) 的 Offset
  ❌ 修改它们**之前**任何分区的 名称/类型/子类型/偏移/大小
  ❌ 在它们之前插入新分区
  ❌ 删除它们之前的任何分区

v7 双分区表布局
--------------
  固定区 (0x0 ~ 0x13FFFF, 1280KB) —— 全硬编码
    0x000000  bootloader          (由 IDF 生成，不在分区表内)
    0x008000  iap_cfg             (硬编码，**不在任何分区表**，8KB)
    0x00A000  (保留)              (原 iap_mark 标志区，已取消)
    0x00B000  分区表 A            (IAP 用，0x1000)
    0x00C000  nvs                  0x3000  (12KB，NVS 最小值，名字必须为 "nvs")
    0x00F000  (保留)              0x1000
    0x010000  iap (factory)       0x130000 (1216KB，固定)

  可变区 (0x140000 ~ 末尾) —— 分区表 B
    0x140000  4KB     分区表 B            (分区表必须在所有分区之前)
    0x141000  12KB    nvs                 (NVS 最小 0x3000, 名字必须为 "nvs")
    0x144000  48KB    (保留空隙)          (使 user_app 64KB 对齐)
    0x150000  剩余    user_app            (64KB 对齐)
    (之后)    可选追加用户分区     ← 偏移必须紧接 user_app 末尾

为什么 nvs 是 12KB 而不是 64KB
----------------------------------
  - IDF gen_esp32part.py 要求可读写 NVS >= 0x3000 (12KB)
  - 分区表 B 自身占用 0x140000 一个扇区，且**必须在所有分区之前**
    (gen_esp32part.py 会校验分区不与分区表重叠)
  - user_app 必须按 64KB 对齐 → 落在 0x150000
  - 因此 nvs 从 0x141000 开始，12KB (0x141000~0x143FFF)，
    0x144000~0x14FFFF 为保留空隙

用法
----
    # 生成两个分区表 CSV (表 A + 表 B)
    python gen_partitions.py --flash 4MB

    # 指定输出路径
    python gen_partitions.py --flash 4MB --out-a partitions_iap.csv --out-b partitions_app.csv

    # 直接生成二进制
    python gen_partitions.py --flash 8MB --bin

    # 多 OTA 槽 (分区表 B 生成 ota_0 ~ ota_N)
    python gen_partitions.py --flash 8MB --slots 3 --info

    # 打印布局摘要
    python gen_partitions.py --flash 8MB --info

⚠️ 分区表 B 属**用户程序工程**（`examples/user_app_template/partitions_user_app.csv`），
   HTML 工具不生成也不修改它，只在烧录用户程序时从设备读回。
   本工具的 `--out-b` 用于生成该 CSV 供用户程序工程使用。
"""

import argparse
import os
import struct
import sys

# ---------------------------------------------------------------------------
# 固定布局常量 (与 partitions_iap.csv / partitions_app.csv 保持一致)
# ---------------------------------------------------------------------------

# 分区表 A (IAP 用)
PARTITION_TABLE_A_OFFSET = 0xB000
# 分区表 B (APP 用) —— 必须在所有 APP 侧分区之前
PARTITION_TABLE_B_OFFSET = 0x140000
PARTITION_TABLE_SIZE     = 0x1000      # 4KB，最多 95 个条目
PARTITION_ENTRY_SIZE     = 32
PARTITION_MAGIC          = 0x50AA

# 分区类型
TYPE_APP  = 0x00
TYPE_DATA = 0x01

# 子类型
SUBTYPE_FACTORY = 0x00
SUBTYPE_OTA_0   = 0x10
SUBTYPE_NVS     = 0x02

# ---- 分区表 A (IAP) 固定分区 ----
# (name, type, subtype, offset, size, flags)
FIXED_PARTITIONS_A = [
    ("nvs", TYPE_DATA, SUBTYPE_NVS,     0x00C000, 0x3000,   0),
    ("iap",     TYPE_APP,  SUBTYPE_FACTORY, 0x010000, 0x130000, 0),
]

# ---- 分区表 B (APP) 固定分区 ----
#
# ⚠️ 分区表 B 自身位于 0x140000 (见 PARTITION_TABLE_B_OFFSET)，
#    因此 nvs 必须从 0x141000 开始 (分区表之后)。
#    user_app 仍需落在 0x150000 (64KB 对齐)。
FIXED_PARTITIONS_B = [
    ("nvs", TYPE_DATA, SUBTYPE_NVS, 0x141000, 0x3000, 0),
]

#  user_app 起始偏移 (固定，不可更改)
USER_APP_OFFSET  = 0x150000
USER_APP_NAME    = "user_app"
USER_APP_SUBTYPE = SUBTYPE_OTA_0

# OTA 槽上限 (与固件 IAP_OTA_SLOT_MAX 一致)
OTA_SLOT_MAX = 16

# 可变区起点 (固定区结束)
FIXED_REGION_END = 0x140000

# flash 最小容量
MIN_FLASH_SIZE = 0x400000   # 4MB

# user_app 最小可用空间
MIN_USER_APP_SIZE = 0x100000   # 1MB

# 对齐
ALIGN_APP  = 0x10000   # app 类型分区: 64KB
ALIGN_DATA = 0x1000    # data 类型分区: 4KB


def parse_size(s: str) -> int:
    """解析大小字符串: 4MB / 8M / 4194304 / 0x400000"""
    s = s.strip().upper()
    mult = 1
    if s.endswith("KB"):
        mult, s = 1024, s[:-2]
    elif s.endswith("MB"):
        mult, s = 1024 * 1024, s[:-2]
    elif s.endswith("GB"):
        mult, s = 1024 * 1024 * 1024, s[:-2]
    elif s.endswith("K"):
        mult, s = 1024, s[:-1]
    elif s.endswith("M"):
        mult, s = 1024 * 1024, s[:-1]
    elif s.endswith("G"):
        mult, s = 1024 * 1024 * 1024, s[:-1]
    return int(s, 0) * mult


def compute_user_app_size(flash_size: int, slot_count: int = 1) -> int:
    """
    计算单个 OTA 槽大小 = (flash 总大小 - user_app 起始偏移) / 槽数

    向下对齐到 64KB (app 类型分区要求)。

    :param flash_size: flash 总容量
    :param slot_count: OTA 槽数量 (1~16)
    """
    if flash_size < MIN_FLASH_SIZE:
        raise ValueError(
            "flash 容量 %s 小于最小值 %s" %
            (fmt_size(flash_size), fmt_size(MIN_FLASH_SIZE)))

    n = max(1, min(slot_count, OTA_SLOT_MAX))
    size = (flash_size - USER_APP_OFFSET) // n
    size = (size // ALIGN_APP) * ALIGN_APP

    if size < MIN_USER_APP_SIZE:
        raise ValueError(
            "%d 个 OTA 槽时每槽仅 %s，小于建议最小值 %s" %
            (n, fmt_size(size), fmt_size(MIN_USER_APP_SIZE)))

    return size


def fmt_size(n: int) -> str:
    """格式化大小显示"""
    if n >= 1024 * 1024:
        mb = n / (1024 * 1024)
        return ("%.0f MB" % mb) if mb == int(mb) else ("%.2f MB" % mb)
    if n >= 1024:
        return "%d KB" % (n // 1024)
    return "%d B" % n


def build_layout_a(flash_size: int):
    """分区表 A (IAP 用) 的完整分区列表"""
    return list(FIXED_PARTITIONS_A)


def build_layout_b(flash_size: int, slot_count: int = 1):
    """
    分区表 B (APP 用) 的完整分区列表 (含多个 OTA 槽)

    槽 0 → user_app   (ota_0)
    槽 N → user_app_N (ota_N)

    :returns: (parts, per_slot_size)
    """
    n = max(1, min(slot_count, OTA_SLOT_MAX))
    per_slot = compute_user_app_size(flash_size, n)
    parts = list(FIXED_PARTITIONS_B)
    for i in range(n):
        parts.append((
            USER_APP_NAME if i == 0 else ("user_app_%d" % i),
            TYPE_APP,
            SUBTYPE_OTA_0 + i,
            USER_APP_OFFSET + i * per_slot,
            per_slot,
            0,
        ))
    return parts, per_slot


# ---------------------------------------------------------------------------
# CSV 输出
# ---------------------------------------------------------------------------

CSV_HEADER_A = """\
# =============================================================================
#  分区表 A —— IAP 程序使用  (位于 flash 0x{BASE:06X}, 4KB)
# =============================================================================
#
# Name,     Type,    SubType, Offset,   Size,     Flags
"""

CSV_HEADER_B = """\
# =============================================================================
#  分区表 B —— 用户程序 (APP) 使用  (位于 flash 0x{BASE:06X}, 4KB)
# =============================================================================
#
# Name,     Type,    SubType, Offset,   Size,     Flags
"""


def _fmt_part_line(name, ptype, subtype, offset, size, flags):
    """格式化一行分区定义 (与手写 CSV 格式一致)"""
    type_name = {TYPE_APP: "app", TYPE_DATA: "data"}
    sub_name = {
        (TYPE_APP, SUBTYPE_FACTORY): "factory",
        (TYPE_APP, SUBTYPE_OTA_0): "ota_0",
        (TYPE_DATA, SUBTYPE_NVS): "nvs",
    }
    t_str = type_name.get(ptype, "0x%02x" % ptype)
    s_str = sub_name.get((ptype, subtype), "0x%02x" % subtype)
    return "%-11s %-7s %-9s 0x%06X, 0x%06X," % (
        name + ",", t_str + ",", s_str + ",", offset, size)


def gen_csv_a(flash_size: int) -> str:
    """生成分区表 A 的 CSV"""
    parts = build_layout_a(flash_size)
    lines = [CSV_HEADER_A.format(BASE=PARTITION_TABLE_A_OFFSET,
                                 flash=fmt_size(flash_size))]
    for p in parts:
        lines.append(_fmt_part_line(*p))
    return "\n".join(lines) + "\n"


def gen_csv_b(flash_size: int, slot_count: int = 1) -> str:
    """生成分区表 B 的 CSV"""
    parts, user_size = build_layout_b(flash_size, slot_count)
    lines = [CSV_HEADER_B.format(BASE=PARTITION_TABLE_B_OFFSET)]
    for p in parts:
        lines.append(_fmt_part_line(*p))
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# 二进制输出
# ---------------------------------------------------------------------------

def _pack_table(parts) -> bytes:
    """
    打包分区表为二进制 (格式与 gen_esp32part.py 一致)

    每个条目 32 字节, 末尾是 MD5 校验块:
      0xEB 0xEB + 14 x 0xFF (16 字节) + MD5(前面所有字节) (16 字节)
    """
    import hashlib

    buf = bytearray()
    for (name, ptype, subtype, offset, size, flags) in parts:
        entry = struct.pack(
            "<HBBII16sI",
            PARTITION_MAGIC,
            ptype,
            subtype,
            offset,
            size,
            name.encode("ascii")[:16].ljust(16, b"\x00"),
            flags,
        )
        assert len(entry) == PARTITION_ENTRY_SIZE, len(entry)
        buf += entry

    md5_begin = b"\xeb\xeb" + b"\xff" * 14
    buf += md5_begin + hashlib.md5(bytes(buf)).digest()

    if len(buf) < PARTITION_TABLE_SIZE:
        buf += b"\xff" * (PARTITION_TABLE_SIZE - len(buf))

    return bytes(buf)


def gen_bin_a(flash_size: int) -> bytes:
    return _pack_table(build_layout_a(flash_size))


def gen_bin_b(flash_size: int, slot_count: int = 1) -> bytes:
    parts, _ = build_layout_b(flash_size, slot_count)
    return _pack_table(parts)


# ---------------------------------------------------------------------------
# 摘要输出
# ---------------------------------------------------------------------------

def print_info(flash_size: int, slot_count: int = 1):
    parts_a = build_layout_a(flash_size)
    parts_b, user_size = build_layout_b(flash_size, slot_count)

    print("目标 flash 容量 : %s (0x%X)" % (fmt_size(flash_size), flash_size))
    print()

    def dump(title, base, parts, mark_name=None):
        print("  %s (0x%X)" % (title, base))
        print("  %-11s %-6s %-9s %-10s %s" %
              ("名称", "类型", "子类型", "偏移", "大小"))
        print("  " + "-" * 58)
        for (name, ptype, subtype, offset, size, flags) in parts:
            t_str = {TYPE_APP: "app", TYPE_DATA: "data"}.get(ptype, "?")
            if ptype == TYPE_APP and subtype >= SUBTYPE_OTA_0:
                s_str = "ota_%d" % (subtype - SUBTYPE_OTA_0)
            else:
                s_str = {
                    (TYPE_APP, SUBTYPE_FACTORY): "factory",
                    (TYPE_DATA, SUBTYPE_NVS): "nvs",
                }.get((ptype, subtype), "?")
            mark = "  <- 可变" if name == mark_name else ""
            print("  %-11s %-6s %-9s 0x%06X   %-10s%s" %
                  (name, t_str, s_str, offset, fmt_size(size), mark))
        print()

    dump("分区表 A (IAP)", PARTITION_TABLE_A_OFFSET, parts_a)
    dump("分区表 B (APP)", PARTITION_TABLE_B_OFFSET, parts_b, USER_APP_NAME)

    n = max(1, min(slot_count, OTA_SLOT_MAX))
    print("  OTA 槽数量      : %d" % n)
    print("  每槽大小        : %s (0x%X)" % (fmt_size(user_size), user_size))
    print("  剩余未使用      : %s" %
          fmt_size(flash_size - (USER_APP_OFFSET + user_size * n)))
    print()
    print("  硬编码区 (不在任何分区表):")
    print("    0x008000   8KB   iap_cfg")
    print("    0x00A000   4KB   (保留, 原 iap_mark)")


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(
        description="按 flash 容量生成 ESP IAP 双分区表 (v5)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument("--flash", "-f", required=True,
                    help="目标 flash 容量 (如 4MB / 8MB / 16MB / 0x800000)")
    ap.add_argument("--slots", type=int, default=1,
                    help="OTA 槽数量 1~%d (默认 1)" % OTA_SLOT_MAX)
    ap.add_argument("--out-a", default="partitions_iap.csv",
                    help="分区表 A 输出路径 (默认 partitions_iap.csv)")
    ap.add_argument("--out-b", default="partitions_app.csv",
                    help="分区表 B 输出路径 (默认 partitions_app.csv)")
    ap.add_argument("--bin", action="store_true",
                    help="输出二进制分区表 (而非 CSV)")
    ap.add_argument("--info", action="store_true",
                    help="只打印布局摘要，不写文件")
    ap.add_argument("--print-user-size", action="store_true",
                    help="只输出单个 OTA 槽大小 (十进制)，供脚本调用")

    args = ap.parse_args()
    slots = max(1, min(args.slots, OTA_SLOT_MAX))

    try:
        flash_size = parse_size(args.flash)
        user_size = compute_user_app_size(flash_size, slots)
    except ValueError as e:
        print("错误: %s" % e, file=sys.stderr)
        return 1

    if args.print_user_size:
        print(user_size)
        return 0

    if args.info:
        print_info(flash_size, slots)
        return 0

    if args.bin:
        data_a = gen_bin_a(flash_size)
        data_b = gen_bin_b(flash_size, slots)
        with open(args.out_a, "wb") as f:
            f.write(data_a)
        with open(args.out_b, "wb") as f:
            f.write(data_b)
        print("已生成分区表 A 二进制: %s (%d 字节)" % (args.out_a, len(data_a)))
        print("已生成分区表 B 二进制: %s (%d 字节, %d 个 OTA 槽)" %
              (args.out_b, len(data_b), slots))
        print()
        print("烧录:")
        print("  esptool.py --chip <chip> write_flash \\")
        print("      0x%X %s \\" % (PARTITION_TABLE_A_OFFSET, args.out_a))
        print("      0x%X %s" % (PARTITION_TABLE_B_OFFSET, args.out_b))
    else:
        with open(args.out_a, "w", encoding="utf-8", newline="\n") as f:
            f.write(gen_csv_a(flash_size))
        with open(args.out_b, "w", encoding="utf-8", newline="\n") as f:
            f.write(gen_csv_b(flash_size, slots))
        print("已生成分区表 A CSV: %s" % args.out_a)
        print("已生成分区表 B CSV: %s (%d 个 OTA 槽)" % (args.out_b, slots))

    print()
    print_info(flash_size, slots)
    return 0


if __name__ == "__main__":
    sys.exit(main())
