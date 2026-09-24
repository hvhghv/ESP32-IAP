#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ESP IAP - XMODEM 发送工具

通过串口以 XMODEM-1K + CRC 协议向 IAP 终端发送固件文件。
对应 IAP 终端的 `xmodem recv` 命令。

用法:
    python iap_xmodem_send.py --port COM3 --file user_app.bin
    python iap_xmodem_send.py --port /dev/ttyUSB0 --file user_app.bin --baud 115200

依赖:
    pip install pyserial
"""

import argparse
import os
import sys
import time

try:
    import serial
except ImportError:
    print("错误: 需要 pyserial，请执行 pip install pyserial", file=sys.stderr)
    sys.exit(1)

# 协议控制字符
SOH = 0x01      # 128 字节包起始
STX = 0x02      # 1024 字节包起始
EOT = 0x04      # 传输结束
ACK = 0x06      # 确认
NAK = 0x15      # 否认
CAN = 0x18      # 取消
CRC_CHAR = ord('C')
SUB = 0x1A      # 填充字节

MAX_RETRY = 10
PKT_TIMEOUT = 3.0
HANDSHAKE_TIMEOUT = 60.0


def crc16(data: bytes) -> int:
    """CRC16-CCITT，多项式 0x1021，初值 0xFFFF，与设备端 iap_crc16() 一致。"""
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def wait_byte(ser: serial.Serial, timeout: float):
    """等待一个字节，超时返回 None。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        b = ser.read(1)
        if b:
            return b[0]
    return None


def send_packet(ser: serial.Serial, seq: int, data: bytes, pkt_len: int) -> bool:
    """发送一个数据包，等待 ACK。"""
    start = STX if pkt_len == 1024 else SOH

    payload = data + bytes([SUB]) * (pkt_len - len(data))
    body = payload[:pkt_len]
    crc = crc16(body)

    frame = bytes([start, seq & 0xFF, (0xFF - seq) & 0xFF]) + body + \
            bytes([(crc >> 8) & 0xFF, crc & 0xFF])

    for attempt in range(MAX_RETRY):
        ser.reset_input_buffer()
        ser.write(frame)
        ser.flush()

        resp = wait_byte(ser, PKT_TIMEOUT)
        if resp == ACK:
            return True
        if resp == CAN:
            print("\n接收方取消传输", file=sys.stderr)
            return False
        # NAK 或超时: 重传
        if attempt < MAX_RETRY - 1:
            print(f"  包 {seq} 重传 ({attempt + 1}/{MAX_RETRY})")

    print(f"\n包 {seq} 发送失败", file=sys.stderr)
    return False


def send_file(ser: serial.Serial, path: str) -> bool:
    """以 XMODEM-1K 发送文件。"""
    size = os.path.getsize(path)
    name = os.path.basename(path)

    print(f"文件: {path} ({size} 字节)")
    print("等待接收方握手...")

    # 握手: 等待 'C'
    deadline = time.time() + HANDSHAKE_TIMEOUT
    while time.time() < deadline:
        b = wait_byte(ser, 1.0)
        if b == CRC_CHAR:
            print("收到 'C'，使用 CRC 模式")
            break
        if b == NAK:
            print("收到 NAK，使用校验和模式 (不推荐)")
            return False
    else:
        print("握手超时", file=sys.stderr)
        return False

    # 第 0 包: 文件名 + 长度 (YMODEM 风格)
    info = name.encode() + b"\x00" + str(size).encode() + b"\x00"
    if not send_packet(ser, 0, info, 128):
        return False

    # 等待接收方再次发送 'C'
    if wait_byte(ser, PKT_TIMEOUT) != CRC_CHAR:
        print("警告: 未收到第 0 包的第二次握手", file=sys.stderr)

    # 数据包
    seq = 1
    sent = 0
    with open(path, "rb") as f:
        while sent < size:
            chunk = f.read(1024)
            if not chunk:
                break

            pkt_len = 1024 if len(chunk) > 128 else 128
            if not send_packet(ser, seq, chunk, pkt_len):
                return False

            sent += len(chunk)
            seq = (seq + 1) & 0xFF
            pct = sent * 100 // size if size else 100
            print(f"\r进度: {sent}/{size} 字节 ({pct}%)", end="", flush=True)

    print()

    # EOT
    for attempt in range(MAX_RETRY):
        ser.write(bytes([EOT]))
        ser.flush()
        if wait_byte(ser, PKT_TIMEOUT) == ACK:
            print("传输完成")
            return True
        print(f"  EOT 重传 ({attempt + 1}/{MAX_RETRY})")

    print("EOT 未确认", file=sys.stderr)
    return False


def main() -> int:
    parser = argparse.ArgumentParser(
        description="通过串口以 XMODEM-1K 向 ESP IAP 发送固件",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("--port", required=True, help="串口设备，如 COM3 或 /dev/ttyUSB0")
    parser.add_argument("--file", required=True, help="待发送文件")
    parser.add_argument("--baud", type=int, default=115200, help="波特率 (默认 115200)")
    parser.add_argument("--timeout", type=float, default=1.0, help="串口读超时秒数")

    args = parser.parse_args()

    if not os.path.isfile(args.file):
        print(f"错误: 找不到文件 {args.file}", file=sys.stderr)
        return 1

    try:
        ser = serial.Serial(args.port, args.baud, timeout=args.timeout)
    except serial.SerialException as e:
        print(f"错误: 无法打开串口 {args.port}: {e}", file=sys.stderr)
        return 1

    try:
        # 清空缓冲
        ser.reset_input_buffer()
        ser.reset_output_buffer()

        ok = send_file(ser, args.file)
        return 0 if ok else 1
    finally:
        ser.close()


if __name__ == "__main__":
    sys.exit(main())
