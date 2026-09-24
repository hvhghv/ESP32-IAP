#!/usr/bin/env python3
"""
用户程序打包工具 (v6)

把 **分区表 B** 与 **应用镜像** 合并为一个从 0x140000 开始的单一文件，
供 IAP 整段写入可变区（与 esptool 语义一致）。

输出文件布局:
    偏移 0x0000 (0x140000)  分区表 B     (4KB)
    偏移 0x1000 (0x141000)  0xFF 填充    (nvs 位置)
    偏移 0x10000 (0x150000) 应用镜像     (user_app)

用法:
    python merge_user_app.py --app build/user_app_template.bin \\
        --ptable build/partition_table/partition-table.bin \\
        -o build/user_flash.bin

    # 分区表省略时, 用 --flash 自动生成
    python merge_user_app.py --app app.bin --flash 4MB -o user_flash.bin
"""

import argparse
import os
import subprocess
import sys

# --- 可变区布局 (与 main/iap_common.h 一致) ---
VAR_REGION_BASE   = 0x140000      # 分区表 B 地址 / 可变区起点
NVS_APP_ADDR      = 0x141000
USER_APP_ADDR     = 0x150000
ERASE_BYTE        = 0xFF

HERE = os.path.dirname(os.path.abspath(__file__))


def parse_size(s):
    """解析 4MB / 4M / 0x400000 等"""
    s = s.strip().upper()
    mult = 1
    if s.endswith("MB"):
        mult, s = 1024 * 1024, s[:-2]
    elif s.endswith("M"):
        mult, s = 1024 * 1024, s[:-1]
    elif s.endswith("KB"):
        mult, s = 1024, s[:-2]
    elif s.endswith("K"):
        mult, s = 1024, s[:-1]
    return int(s, 0) * mult


def gen_ptable_b(flash_size):
    """调用 gen_partitions.py 生成分区表 B"""
    gen = os.path.join(HERE, "gen_partitions.py")
    if not os.path.isfile(gen):
        print("错误: 找不到 gen_partitions.py", file=sys.stderr)
        return None
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        out = os.path.join(td, "pt_b.bin")
        r = subprocess.run([sys.executable, gen, "--flash", str(flash_size),
                            "--bin", "--out-b", out],
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        if r.returncode != 0 or not os.path.isfile(out):
            print("错误: 生成分区表 B 失败: %s"
                  % r.stderr.decode("utf-8", "replace"), file=sys.stderr)
            return None
        with open(out, "rb") as f:
            return f.read()


def main():
    ap = argparse.ArgumentParser(
        description="合并分区表 B + 应用镜像 -> 从 0x140000 开始的单一文件")
    ap.add_argument("--app", required=True, help="应用镜像 (idf.py build 产物)")
    ap.add_argument("--ptable", help="分区表 B 二进制 (partition-table.bin)")
    ap.add_argument("--flash", help="flash 容量 (无 --ptable 时自动生成分区表 B)")
    ap.add_argument("--slots", type=int, default=1, help="OTA 槽数量 (默认 1)")
    ap.add_argument("-o", "--output", required=True, help="输出文件")
    args = ap.parse_args()

    # --- 读应用镜像 ---
    if not os.path.isfile(args.app):
        print("错误: 找不到应用镜像: %s" % args.app, file=sys.stderr)
        return 1
    with open(args.app, "rb") as f:
        app_data = f.read()

    if app_data[0] != 0xE9:
        print("错误: %s 首字节 0x%02X 不是 ESP 镜像 magic (0xE9)"
              % (os.path.basename(args.app), app_data[0]), file=sys.stderr)
        return 1

    # --- 读/生成分区表 B ---
    if args.ptable:
        if not os.path.isfile(args.ptable):
            print("错误: 找不到分区表: %s" % args.ptable, file=sys.stderr)
            return 1
        with open(args.ptable, "rb") as f:
            pt_data = f.read()
        pt_src = os.path.basename(args.ptable)
    else:
        if not args.flash:
            print("错误: 需指定 --ptable 或 --flash", file=sys.stderr)
            return 1
        pt_data = gen_ptable_b(parse_size(args.flash))
        if pt_data is None:
            return 1
        pt_src = "自动生成 (flash=%s)" % args.flash

    # --- 校验分区表 ---
    if len(pt_data) < 2 or (pt_data[0] | (pt_data[1] << 8)) != 0x50AA:
        print("错误: 分区表 magic 不是 0x50AA", file=sys.stderr)
        return 1
    if len(pt_data) > (USER_APP_ADDR - VAR_REGION_BASE):
        print("错误: 分区表过大 (%d 字节)" % len(pt_data), file=sys.stderr)
        return 1

    # --- 合并 ---
    total = (USER_APP_ADDR - VAR_REGION_BASE) + len(app_data)
    buf = bytearray([ERASE_BYTE] * total)
    buf[0:len(pt_data)] = pt_data
    app_off = USER_APP_ADDR - VAR_REGION_BASE
    buf[app_off:app_off + len(app_data)] = app_data

    with open(args.output, "wb") as f:
        f.write(buf)

    # --- 汇总 ---
    print("已生成用户程序烧录文件 (从 0x%06X 开始):" % VAR_REGION_BASE)
    print("  文件        : %s (%d 字节)" % (args.output, len(buf)))
    print("  0x%06X  分区表 B   (%d 字节, %s)"
          % (VAR_REGION_BASE, len(pt_data), pt_src))
    print("  0x%06X  0xFF 填充  (nvs 位置)"
          % NVS_APP_ADDR)
    print("  0x%06X  应用镜像   (%d 字节)"
          % (USER_APP_ADDR, len(app_data)))
    print()
    print("烧录:")
    print("  esptool : esptool.py write_flash 0x%06X %s" % (VAR_REGION_BASE, args.output))
    print("  IAP     : 发送该文件, IAP 会整段写入 0x%06X" % VAR_REGION_BASE)
    return 0


if __name__ == "__main__":
    sys.exit(main())
