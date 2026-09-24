#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ESP IAP 烧录镜像打包工具

把 bootloader / 配置区 / 分区表 A / IAP 应用 合并为**单一连续** 1280KB 镜像。

v5 方案：**只产出 1 个文件** `iap_side.bin`，覆盖整个固定区 (0x0 ~ 0x13FFFF)。

镜像布局 (单一连续文件):
    0x000000  bootloader.bin
    0x008000  iap_cfg.bin        <- 出厂配置 (可选，默认自动生成)
    0x00A000  (保留)             <- 0xFF
    0x00B000  partitions_iap.bin <- 分区表 A (自动生成或 --ptable 指定)
    0x00C000  (nvs)              <- 0xFF (首次启动自动初始化)
    0x010000  esp_iap.bin        <- IAP 应用
    0x13FFFF  ---- 固定区末尾 ----

为什么合并分区表 A:
    分区表 A 只描述 IAP 侧布局 (nvs + iap)，与 flash 容量无关，
    因此可以打进固件，无需单独烧录。

    用户程序的分区表 B (0x140000) 属用户程序工程，**不在本工具范围**。

用法:
    # 基本用法: 合并构建目录中的产物 (自动生成默认配置 + 分区表 A)
    python merge_bin.py --build-dir build --default-cfg --flash 4MB \
        -o iap_side.bin

    # 指定出厂配置
    python merge_bin.py --build-dir build --cfg iap_cfg.bin \
        --flash 4MB -o iap_side.bin

    # 指定分区表 A 二进制
    python merge_bin.py --build-dir build --ptable partitions_iap.bin \
        -o iap_side.bin

烧录 (2 个文件，顺序无关):
    esptool.py --chip esp32c6 -p COM18 write_flash \
        0x0      iap_side.bin \
        0x140000 user.bin

    注: user.bin 为**从 0x140000 开始的完整镜像** (含分区表 B + 应用镜像)，
        由 tools/merge_user_app.py 或 examples/user_app_template/build_user_app.py 生成。
"""

import argparse
import os
import subprocess
import sys

# 各分区在 flash 中的地址（与 partitions_iap.csv / partitions_app.csv 一致）
#
# v6 双分区表布局:
#   0x000000  32KB    bootloader
#   0x008000   8KB    iap_cfg        (硬编码, 不在分区表)
#   0x00A000   4KB    (保留, 原 iap_mark)
#   0x00B000   4KB    分区表 A       <- iap_side.bin 内含
#   0x00C000  12KB    nvs
#   0x00F000   4KB    (保留)
#   0x010000  1216KB  iap
#   ---- 可变区 (由 user.bin 单独烧录, 从 0x140000 开始) ----
#   0x140000   4KB    分区表 B
#   0x141000  12KB    nvs
#   0x150000  剩余    user_app
ADDR_BOOTLOADER      = 0x000000
ADDR_IAP_CFG         = 0x008000
ADDR_PARTITION_TABLE_A = 0x00B000
ADDR_IAP_APP         = 0x010000
ADDR_NVS_APP         = 0x141000
ADDR_PARTITION_TABLE_B = 0x140000
ADDR_USER_APP        = 0x150000

PARTITION_TABLE_SIZE = 0x1000     # 4KB
IAP_CFG_SIZE         = 0x2000     # 8KB
NVS_APP_SIZE         = 0x10000    # 64KB

# 固定区结束 (可变区起点)
FIXED_REGION_END     = 0x140000

# 默认镜像大小 (覆盖到 user_app 分区末尾)
DEFAULT_SIZE = 4 * 1024 * 1024

ERASE_BYTE = 0xFF


def parse_size(s: str) -> int:
    """解析大小字符串: 4096 / 4K / 4MB / 0x100000"""
    s = s.strip().upper()
    mult = 1
    if s.endswith("KB"):
        mult, s = 1024, s[:-2]
    elif s.endswith("MB"):
        mult, s = 1024 * 1024, s[:-2]
    elif s.endswith("K"):
        mult, s = 1024, s[:-1]
    elif s.endswith("M"):
        mult, s = 1024 * 1024, s[:-1]
    return int(s, 0) * mult


def read_file(path: str) -> bytes:
    with open(path, "rb") as f:
        return f.read()


def find_in_build(build_dir: str, *candidates: str) -> str:
    """在构建目录中查找第一个存在的文件。"""
    for c in candidates:
        p = os.path.join(build_dir, c)
        if os.path.isfile(p):
            return p
    return ""


def main() -> int:
    ap = argparse.ArgumentParser(
        description="合并 ESP IAP 各分区为单一烧录镜像",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument("--build-dir", default="build",
                    help="ESP-IDF 构建目录 (默认 build)")
    ap.add_argument("-o", "--output", required=True,
                    help="输出镜像文件 (iap_side.bin)")
    ap.add_argument("--bootloader", help="bootloader.bin 路径 (默认自动查找)")
    ap.add_argument("--app", help="IAP 应用 bin 路径 (默认自动查找)")
    ap.add_argument("--cfg", help="出厂配置镜像 (由 gen_factory_cfg.py 生成)")
    ap.add_argument("--default-cfg", action="store_true",
                    help="自动生成默认出厂配置并合并")
    ap.add_argument("--cfg-json", help="传给 gen_factory_cfg.py 的 JSON 配置")
    ap.add_argument("--ptable", default=None,
                    help="分区表 A 二进制 (partitions_iap_*.bin)。"
                         "未指定且给了 --flash 时自动生成")
    ap.add_argument("--size", default="4MB",
                    help="目标 flash 容量 (决定镜像填充范围，默认 4MB)")
    ap.add_argument("--pad-to-end", action="store_true",
                    help="填充到固定区末尾 (默认已填充，此项保留兼容)")
    ap.add_argument("--flash", "-f", default=None,
                    help="自动生成分区表 A: 指定目标 flash 容量 (如 4MB)")
    ap.add_argument("--ptable-output", default=None,
                    help="(已废弃，保留兼容) 分区表输出文件名")

    args = ap.parse_args()

    total_size = parse_size(args.size)

    # --- 定位各分区文件 ---
    boot = args.bootloader or find_in_build(
        args.build_dir, "bootloader/bootloader.bin")

    app = args.app
    if not app:
        # 应用名可能是 esp_iap.bin 或任意工程名
        app = find_in_build(args.build_dir, "esp_iap.bin")
        if not app:
            # 扫描构建目录根下的 .bin
            for name in sorted(os.listdir(args.build_dir)):
                if name.endswith(".bin") and name != "bootloader.bin":
                    cand = os.path.join(args.build_dir, name)
                    if os.path.isfile(cand) and "partition" not in name:
                        app = cand
                        break

    missing = []
    if not boot or not os.path.isfile(boot):
        missing.append("bootloader.bin")
    if not app or not os.path.isfile(app):
        missing.append("IAP 应用 bin")

    if missing:
        print("错误: 找不到以下文件: %s" % ", ".join(missing), file=sys.stderr)
        print("请检查 --build-dir 是否正确，或显式指定路径。", file=sys.stderr)
        return 1

    # --- 出厂配置 ---
    cfg_path = args.cfg
    tmp_cfg = None
    if args.default_cfg and not cfg_path:
        tmp_cfg = os.path.join(os.path.dirname(os.path.abspath(args.output)),
                               "_auto_iap_cfg.bin")
        gen = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "gen_factory_cfg.py")
        if not os.path.isfile(gen):
            print("错误: 找不到 gen_factory_cfg.py", file=sys.stderr)
            return 1
        cmd = [sys.executable, gen, "-o", tmp_cfg]
        if args.cfg_json:
            cmd += ["--json", args.cfg_json]
        print("生成默认出厂配置 ...")
        r = subprocess.run(cmd)
        if r.returncode != 0:
            print("错误: 生成出厂配置失败", file=sys.stderr)
            return 1
        cfg_path = tmp_cfg

    # --- 组装镜像 ---
    #
    # v5 简化方案: **单一连续文件** (0x000000 ~ 0x13FFFF)
    #
    #   0x000000  32KB    bootloader
    #   0x008000   8KB    iap_cfg
    #   0x00A000   4KB    (保留)
    #   0x00B000   4KB    分区表 A        <- 包含在内
    #   0x00C000  12KB    nvs
    #   0x00F000   4KB    (保留)
    #   0x010000  ~      iap
    #
    # 因此烧录只需 **2 个文件**:
    #   1. iap_side.bin  -> 0x000000  (本文件，含分区表 A)
    #   2. user.bin      -> 0x140000  (用户程序完整镜像，含分区表 B)
    #
    # 优点: 分区表 A 与 IAP 程序配套，一起烧录不会出现版本不匹配。
    # 代价: 烧 iap_side.bin 会覆盖设备上的分区表 A (这正是期望行为)。
    regions = [
        (ADDR_BOOTLOADER, boot, "bootloader"),
        (ADDR_IAP_APP, app, "IAP 应用"),
    ]
    if cfg_path:
        if not os.path.isfile(cfg_path):
            print("错误: 配置镜像不存在: %s" % cfg_path, file=sys.stderr)
            return 1
        regions.append((ADDR_IAP_CFG, cfg_path, "出厂配置"))

    # 分区表 A: 由 --ptable 指定，或由 --flash 自动生成
    pt_a_path = args.ptable
    if pt_a_path:
        if not os.path.isfile(pt_a_path):
            print("错误: 分区表文件不存在: %s" % pt_a_path, file=sys.stderr)
            return 1
        regions.append((ADDR_PARTITION_TABLE_A, pt_a_path, "分区表 A"))
    elif args.flash:
        # 自动生成到临时文件
        tmp_pt = os.path.join(os.path.dirname(os.path.abspath(args.output)),
                              "_auto_ptable_a.bin")
        tmp_pt_b = os.path.join(os.path.dirname(os.path.abspath(args.output)),
                                "_auto_ptable_b.bin")
        gen_pt = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                              "gen_partitions.py")
        if not os.path.isfile(gen_pt):
            print("错误: 找不到 gen_partitions.py", file=sys.stderr)
            return 1
        print("生成分区表 A (flash=%s) ..." % args.flash)
        r = subprocess.run([sys.executable, gen_pt, "--flash", args.flash,
                            "--bin", "--out-a", tmp_pt,
                            "--out-b", tmp_pt_b])
        if r.returncode != 0:
            print("错误: 生成分区表失败", file=sys.stderr)
            return 1
        pt_a_path = tmp_pt
        regions.append((ADDR_PARTITION_TABLE_A, pt_a_path, "分区表 A"))

    # 按地址排序，便于检查重叠
    regions.sort(key=lambda x: x[0])

    # 检查地址重叠
    for i in range(len(regions) - 1):
        a_addr, a_path, a_name = regions[i]
        b_addr, _, b_name = regions[i + 1]
        a_end = a_addr + os.path.getsize(a_path)
        if a_end > b_addr:
            print("错误: %s (0x%X~0x%X) 与 %s (0x%X) 重叠" %
                  (a_name, a_addr, a_end, b_name, b_addr), file=sys.stderr)
            return 1

    # 检查是否越过固定区末尾
    for addr, path, name in regions:
        if addr + os.path.getsize(path) > FIXED_REGION_END:
            print("错误: %s (0x%X) 超出固定区末尾 (0x%X)" %
                  (name, addr, FIXED_REGION_END), file=sys.stderr)
            return 1

    # ------------------------------------------------------------------
    # 构造**单一连续镜像** (0x000000 ~ 固定区末尾)
    #
    # v5 简化: 不再分段跳过分区表 A —— 分区表 A 与 IAP 程序配套，
    #          一起烧录可避免版本不匹配。
    #
    # 镜像内的空隙 (0xA000~0xAFFF、0xF000~0xFFFF 等) 填充 0xFF (擦除态)。
    # ------------------------------------------------------------------

    def build_image(regions, base_addr, end_limit, pad_full=False):
        """
        构造连续镜像，返回 (bytes, 实际结束地址)

        pad_full=True 时填满到 end_limit (用于确保镜像覆盖整个固定区，
        避免 bootloader 之后的空隙残留旧数据)。
        """
        if not regions:
            return b"", base_addr
        last_addr, last_path, _ = regions[-1]
        last_end = last_addr + os.path.getsize(last_path)

        if pad_full or args.pad_to_end:
            seg_len = end_limit - base_addr
        else:
            # 只填到内容末尾，再对齐到 4KB
            seg_len = ((last_end - base_addr) + 0xFFF) & ~0xFFF

        buf = bytearray([ERASE_BYTE]) * seg_len
        for addr, path, name in regions:
            data = read_file(path)
            off = addr - base_addr
            if off + len(data) > seg_len:
                raise ValueError("%s 超出镜像范围" % name)
            buf[off:off + len(data)] = data
        return bytes(buf), base_addr + seg_len

    try:
        # 填满到固定区末尾 (0x140000)，确保镜像覆盖整个固定区
        img, img_end = build_image(regions, ADDR_BOOTLOADER,
                                   FIXED_REGION_END, pad_full=True)
    except ValueError as e:
        print("错误: %s" % e, file=sys.stderr)
        return 1

    if img_end > FIXED_REGION_END:
        print("错误: 内容 (0x%X) 超出固定区末尾 (0x%X)" %
              (img_end, FIXED_REGION_END), file=sys.stderr)
        return 1

    out1 = args.output
    with open(out1, "wb") as f:
        f.write(img)

    # --- 输出摘要 ---
    print()
    print("已生成 IAP 烧录文件 (单一连续文件, 0x%06X ~ 0x%06X):" %
          (ADDR_BOOTLOADER, img_end - 1))
    print()
    print("  %-22s %-10s %-10s %s" % ("文件", "地址", "大小", "内容"))
    print("  " + "-" * 76)
    print("  %-22s 0x%06X   0x%06X   %s" %
          (os.path.basename(out1), ADDR_BOOTLOADER, len(img),
           " + ".join(os.path.basename(p) for _, p, _ in regions)))
    print()
    print("  %-22s %-10s %-10s %s" % ("(可变区)", "0x140000", "—",
                                       "由 user.bin 单独烧录"))

    # --- 可选: 生成配套分区表 ---
    ptable_path = args.ptable_output
    if args.flash:
        gen_pt = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                              "gen_partitions.py")
        if not os.path.isfile(gen_pt):
            print("\n错误: 找不到 gen_partitions.py", file=sys.stderr)
            return 1
        if not ptable_path:
            tag = args.flash.strip().upper().replace(" ", "").lower()
            ptable_path = "partitions_%s.bin" % tag
        print()
        print("生成配套分区表 A (flash=%s) ..." % args.flash)
        r = subprocess.run([sys.executable, gen_pt, "--flash", args.flash,
                            "--bin", "--out-a", ptable_path,
                            "--out-b", "partitions_app.bin"])
        if r.returncode != 0:
            print("错误: 生成分区表失败", file=sys.stderr)
            return 1

    # --- 烧录指引 (v6: 只需 2 个文件) ---
    print()
    print("烧录命令 (2 个文件，顺序无关):")
    cmd = "  esptool.py --chip <chip> -p <port> write_flash"
    cmd += " 0x%X %s" % (ADDR_BOOTLOADER, out1)
    cmd += " 0x%X %s" % (ADDR_PARTITION_TABLE_B, "user.bin")
    print(cmd)
    print()
    print("说明:")
    print("  - iap_side.bin 已包含 bootloader + iap_cfg + 分区表 A + nvs + IAP")
    print("  - user.bin 为**从 0x%X 开始的完整镜像** (分区表 B + 应用镜像)" %
          ADDR_PARTITION_TABLE_B)
    print("    由 tools/merge_user_app.py 或 build_user_app.py 生成")
    print("  - 若只需更新 IAP 程序，可用 HTML 工具「用户程序」槽位单独烧 0x%X 起的应用" %
          ADDR_IAP_APP)

    # 清理临时文件
    if tmp_cfg and os.path.isfile(tmp_cfg):
        os.remove(tmp_cfg)
    if pt_a_path and pt_a_path.endswith("_auto_ptable_a.bin") and os.path.isfile(pt_a_path):
        os.remove(pt_a_path)
        pt_b = pt_a_path[:-6] + "_b.bin"          # _a.bin -> _b.bin
        if os.path.isfile(pt_b):
            os.remove(pt_b)

    return 0


if __name__ == "__main__":
    sys.exit(main())
