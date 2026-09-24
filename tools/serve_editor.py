#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
本地预览 / 校验浏览器配置编辑器

用法:
    python tools/serve_editor.py            # 启动本地服务并打开浏览器
    python tools/serve_editor.py --check    # 仅做校验，不启动服务
    python tools/serve_editor.py --port 9000

为什么需要本地服务:
    Web Serial API 要求安全上下文 (Secure Context)。
    file:// 不是安全上下文，浏览器会拒绝串口访问。
    而 http://localhost 被视为安全，因此必须用本地 HTTP 服务打开。

    (若只用配置编辑功能、不用烧录，可直接双击 HTML 文件)
"""

import argparse
import http.server
import os
import re
import socket
import socketserver
import subprocess
import sys
import threading
import webbrowser

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
EDITOR = "esp_iap_tool.html"

# 关键元素（与 CI 检查保持一致）
REQUIRED = [
    'id="fileInput"', 'id="btnConnect"', 'id="btnFlash"',
    'id="btnDump"', 'id="btnDumpEdit"', 'id="burnTargetBar"',
    'id="appMethod"', 'id="dumpScope"',
    'CFG_MAGIC', 'USER_APP_ADDR', 'xmodemSend',
]


def check() -> int:
    """校验 HTML 完整性与 JS 语法。返回 0 表示通过。"""
    path = os.path.join(ROOT, EDITOR)
    print("=== 校验 %s ===" % EDITOR)

    if not os.path.isfile(path):
        print("  [FAIL] 文件不存在: %s" % path)
        return 1
    size = os.path.getsize(path)
    print("  [PASS] 文件存在 (%d 字节)" % size)
    if size < 10000:
        print("  [FAIL] 文件过小，可能损坏")
        return 1

    html = open(path, "r", encoding="utf-8").read()

    # 1. 提取 module script
    m = re.search(r'<script type="module">(.*?)</script>', html, re.S)
    if not m:
        print('  [FAIL] 未找到 <script type="module"> 块')
        return 1
    js = m.group(1)
    print("  [PASS] 提取 JS (%d 行)" % js.count("\n"))

    # 2. 关键元素
    missing = [r for r in REQUIRED if r not in html]
    if missing:
        print("  [FAIL] 缺少关键元素: %s" % ", ".join(missing))
        return 1
    print("  [PASS] 关键元素齐全 (%d 项)" % len(REQUIRED))

    # 3. JS 语法（若本机有 node）
    tmp = os.path.join(ROOT, "_check.mjs")
    try:
        with open(tmp, "w", encoding="utf-8") as f:
            f.write(js)
        r = subprocess.run(["node", "--check", tmp],
                           capture_output=True, text=True)
        if r.returncode == 0:
            print("  [PASS] JS 语法检查通过")
        else:
            print("  [FAIL] JS 语法错误:")
            print(r.stderr)
            return 1
    except FileNotFoundError:
        print("  [SKIP] 未找到 node，跳过 JS 语法检查")
    finally:
        if os.path.isfile(tmp):
            os.remove(tmp)

    print()
    print("校验通过")
    return 0


def find_free_port(start: int) -> int:
    """从 start 开始找一个可用端口。"""
    for port in range(start, start + 100):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            if s.connect_ex(("127.0.0.1", port)) != 0:
                return port
    raise RuntimeError("找不到可用端口")


def serve(port: int, open_browser: bool) -> int:
    """在项目根目录启动 HTTP 服务。"""
    port = find_free_port(port)

    class Handler(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *a, **kw):
            super().__init__(*a, directory=ROOT, **kw)

        def log_message(self, fmt, *args):
            # 精简日志
            print("  %s" % (fmt % args))

    url = "http://localhost:%d/%s" % (port, EDITOR)

    class Server(socketserver.ThreadingTCPServer):
        allow_reuse_address = True

    with Server(("127.0.0.1", port), Handler) as httpd:
        print("=" * 62)
        print("  ESP IAP 配置编辑器 — 本地服务")
        print("=" * 62)
        print()
        print("  地址: %s" % url)
        print("  目录: %s" % ROOT)
        print()
        print("  提示:")
        print("    - 用 Chrome / Edge 打开（Firefox / Safari 不支持 Web Serial）")
        print("    - 必须是 localhost 或 https，file:// 无法使用烧录功能")
        print("    - 按 Ctrl+C 停止服务")
        print()
        print("=" * 62)

        if open_browser:
            threading.Timer(0.8, lambda: webbrowser.open(url)).start()

        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\n已停止")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(
        description="本地预览 / 校验浏览器配置编辑器",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument("--check", action="store_true",
                    help="仅做校验，不启动服务")
    ap.add_argument("--port", type=int, default=8899,
                    help="服务端口 (默认 8899，被占用时自动递增)")
    ap.add_argument("--no-open", action="store_true",
                    help="不自动打开浏览器")
    args = ap.parse_args()

    rc = check()
    if rc != 0 or args.check:
        return rc

    print()
    return serve(args.port, not args.no_open)


if __name__ == "__main__":
    sys.exit(main())
