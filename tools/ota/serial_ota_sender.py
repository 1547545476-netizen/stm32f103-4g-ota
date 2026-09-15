#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
STM32 Bootloader 串口 OTA 发送工具。

这个脚本不是把 app.bin 原样扔给单片机。
它会把 app.bin 包装成 Bootloader 能识别的 OTA 协议包：

    START 包：告诉 Bootloader 版本、大小、CRC32、W25Q64地址和HMAC认证标签
    DATA  包：把 app.bin 按 200 字节一包分开发送
    END   包：告诉 Bootloader 固件已经发完，可以做整包校验

典型用法：

    python tools/ota/serial_ota_sender.py --port COM6 --bin app.bin --version 1

安装依赖：

    python -m pip install pyserial
"""

from __future__ import annotations

import argparse
import struct
import sys
import time
import zlib
from pathlib import Path

from firmware_auth import calculate_auth_tag, load_device_key

try:
    import serial
except ImportError:
    print("缺少 pyserial，请先执行：python -m pip install pyserial")
    raise


# ==================== 协议常量 ====================

# 这些命令值必须和 Bootloader 里的 Updete.h 保持一致。
UPDATE_CMD_START = 0x01
UPDATE_CMD_DATA = 0x02
UPDATE_CMD_END = 0x03

# Bootloader 里限制单包 payload 最大 200 字节。
# 协议头尾还要占 13 字节，200 字节可以给串口 DMA 留出余量。
UPDATE_MAX_PAYLOAD = 200

# Bootloader 当前 USART1 的波特率。
DEFAULT_BAUD = 921600

# 固件默认写入 W25Q64 的 0 地址。
DEFAULT_IMAGE_ADDR = 0x00000000
W25Q64_SECTOR_SIZE = 4096
W25Q64_TOTAL_SIZE = 8 * 1024 * 1024
FLASH_APP_SIZE = 44 * 1024


def crc16_modbus(data: bytes) -> int:
    """
    计算一帧协议包的 CRC16。

    Bootloader 里使用的也是这个算法：
        初始值：0xFFFF
        多项式：0xA001

    注意：
        CRC16 的计算范围是 cmd 到 payload 结束。
        不包含帧头 0x55 0xAA，也不包含最后的 crc16 字段本身。
    """
    crc = 0xFFFF

    for value in data:
        crc ^= value
        for _ in range(8):
            if crc & 0x0001:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1

    return crc & 0xFFFF


def make_frame(cmd: int, seq: int, offset: int, payload: bytes) -> bytes:
    """
    生成一帧 Bootloader 能识别的 OTA 协议包。

    帧格式：

        55 AA + cmd + seq + offset + payload_len + payload + crc16

    字段说明：

        55 AA       : 固定帧头，用来让 Bootloader 找到一帧的开始
        cmd         : 命令，START/DATA/END
        seq         : 包序号，上位机和 Bootloader 对 ACK 时使用
        offset      : DATA 包在整个固件里的偏移
        payload_len : payload 长度
        payload     : 实际数据
        crc16       : 单包校验

    struct.pack("<BHIH", ...) 的意思：

        <  : 小端序
        B  : uint8_t  cmd
        H  : uint16_t seq
        I  : uint32_t offset
        H  : uint16_t payload_len
    """
    body = struct.pack(
        "<BHIH",
        cmd,
        seq & 0xFFFF,
        offset & 0xFFFFFFFF,
        len(payload),
    )
    body += payload

    crc = crc16_modbus(body)

    return b"\x55\xAA" + body + struct.pack("<H", crc)


def check_app_vector(image: bytes) -> None:
    """
    检查 app.bin 前两个 word 是否像一个合法的 Cortex-M APP。

    Cortex-M 向量表：

        第 0 个 word：初始 MSP，也就是栈顶地址
        第 1 个 word：Reset_Handler，也就是复位入口函数地址

    对你的工程来说：

        栈顶地址应该在 SRAM 范围：0x20000000 ~ 0x20005000
        Reset_Handler 应该在 APP 区：0x08005000 ~ 0x0800FFFF

    如果这里检查失败，通常说明 APP 工程没有把 IROM1 改成 0x08005000。
    """
    if len(image) < 8:
        raise ValueError("bin 文件太小，不像有效 STM32 APP")

    stack, reset = struct.unpack_from("<II", image, 0)

    stack_ok = 0x20000000 <= stack <= 0x20005000
    reset_thumb_ok = (reset & 1) == 1
    reset_address = reset & ~1
    reset_ok = 0x08005000 <= reset_address < 0x08010000

    print(f"APP 初始栈地址: 0x{stack:08X}")
    print(f"APP Reset_Handler: 0x{reset:08X}")

    if not stack_ok:
        raise ValueError("APP 初始栈地址不在 STM32F103C8 SRAM 范围内")

    if not reset_thumb_ok:
        raise ValueError("APP Reset_Handler 最低位不是 1，不是合法的 Cortex-M Thumb 入口")

    if not reset_ok:
        raise ValueError("APP Reset_Handler 不在 APP 分区，请确认 APP 链接地址是 0x08005000")


def read_until_ack(ser: serial.Serial, cmd: int, seq: int, timeout: float) -> None:
    """
    等待 Bootloader 返回指定包的 ACK。

    Bootloader 成功处理一包后会打印：

        UPD ACK cmd=命令 seq=序号

    如果返回 UPD ERR，说明 Bootloader 拒绝了这一包。
    如果一直没有返回 ACK，说明可能串口线、波特率、复位时机或板子程序有问题。
    """
    deadline = time.time() + timeout
    ack_text = f"UPD ACK cmd={cmd} seq={seq}"

    while time.time() < deadline:
        line = ser.readline()
        if not line:
            continue

        text = line.decode("utf-8", errors="ignore").strip()
        if text:
            print(f"[板子] {text}")

        if ack_text in text:
            return

        if "UPD ERR" in text:
            raise RuntimeError(f"Bootloader 返回错误：{text}")

    raise TimeoutError(f"等待 ACK 超时：cmd={cmd}, seq={seq}")


def wait_user_reset(ser: serial.Serial, wait_boot: float) -> None:
    """
    等待用户手动复位开发板。

    当前 Bootloader 上电后只有 5 秒串口 OTA 窗口。
    所以脚本打开串口后，会提示你：

        先按回车，再立刻按开发板 RESET。

    脚本随后等待 Bootloader 打印窗口提示。
    如果没有等到，也会继续尝试发送 START 包。
    """
    input("请先烧录 Bootloader 并接好串口线。准备好后按回车，然后立刻按一下开发板 RESET...")

    ser.reset_input_buffer()

    deadline = time.time() + wait_boot
    print(f"等待 Bootloader 串口窗口提示，最多 {wait_boot:.1f} 秒...")

    while time.time() < deadline:
        line = ser.readline()
        if not line:
            continue

        text = line.decode("utf-8", errors="ignore").strip()
        if text:
            print(f"[板子] {text}")

        if ("Bootloader serial OTA window" in text) or ("No valid APP" in text):
            return

    print("没有等到窗口提示，继续尝试发送。")


def send_image(args: argparse.Namespace) -> None:
    """
    发送完整 APP bin。

    整体流程：

        1. 读取 app.bin
        2. 计算整包 CRC32
        3. 检查 APP 向量表是否像 0x08005000 的 APP
        4. 打开串口
        5. 等待用户复位板子
        6. 发送 START 包
        7. 循环发送 DATA 包
        8. 发送 END 包
        9. 等待每一包 ACK
    """
    image = Path(args.bin).read_bytes()
    image_crc = zlib.crc32(image) & 0xFFFFFFFF
    auth_tag = calculate_auth_tag(
        image, args.version, load_device_key(args.auth_key_env)
    )

    if not image:
        raise ValueError("APP bin 文件为空")

    if len(image) > FLASH_APP_SIZE:
        raise ValueError(f"APP 固件超过44KB分区：{len(image)} > {FLASH_APP_SIZE} 字节")

    if len(image) > W25Q64_TOTAL_SIZE - args.addr:
        raise ValueError("W25Q64 暂存地址加固件长度超过8MB容量")

    print(f"bin 文件: {args.bin}")
    print(f"固件大小: {len(image)} 字节")
    print(f"固件 CRC32: 0x{image_crc:08X}")
    print(f"固件认证标签: {auth_tag.hex().upper()}")
    print(f"W25Q64 地址: 0x{args.addr:08X}")
    print(f"单包大小: {args.chunk} 字节")

    if not args.skip_vector_check:
        check_app_vector(image)

    with serial.Serial(args.port, args.baud, timeout=0.2, write_timeout=2) as ser:
        print(f"已打开串口 {args.port}, 波特率 {args.baud}")

        if not args.no_reset_prompt:
            wait_user_reset(ser, args.wait_boot)
        else:
            ser.reset_input_buffer()

        seq = 0

        # ==================== 1. 发送 START 包 ====================
        # START payload固定32字节：16字节元数据后追加16字节HMAC认证标签。
        start_payload = struct.pack(
            "<IIII",
            args.version,
            len(image),
            image_crc,
            args.addr,
        ) + auth_tag

        ser.write(make_frame(UPDATE_CMD_START, seq, 0, start_payload))
        read_until_ack(ser, UPDATE_CMD_START, seq, args.ack_timeout)
        seq += 1

        # ==================== 2. 循环发送 DATA 包 ====================
        offset = 0
        total = len(image)

        while offset < total:
            # 从 app.bin 中切出一小块，默认最多 200 字节。
            chunk = image[offset : offset + args.chunk]

            # DATA 包的 offset 表示这一块数据在整个固件里的位置。
            ser.write(make_frame(UPDATE_CMD_DATA, seq, offset, chunk))
            read_until_ack(ser, UPDATE_CMD_DATA, seq, args.ack_timeout)

            offset += len(chunk)
            percent = offset * 100.0 / total
            print(f"发送进度: {offset}/{total} ({percent:.1f}%)")

            seq += 1

        # ==================== 3. 发送 END 包 ====================
        # END 包 payload 为空，表示整个 app.bin 已经发送完成。
        ser.write(make_frame(UPDATE_CMD_END, seq, 0, b""))
        read_until_ack(ser, UPDATE_CMD_END, seq, args.ack_timeout)

        print("升级包发送完成。板子会自动复位，然后 Bootloader 开始搬运 APP。")


def parse_args() -> argparse.Namespace:
    """
    解析命令行参数。

    例如：

        python tools/ota/serial_ota_sender.py --port COM6 --bin app.bin --version 1
    """
    parser = argparse.ArgumentParser(description="STM32 Bootloader 串口 OTA 发送工具")

    parser.add_argument("--port", required=True, help="串口号，例如 COM6")
    parser.add_argument("--bin", required=True, help="APP bin 文件路径")
    parser.add_argument("--version", type=int, default=1, help="APP 版本号，默认 1")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD, help=f"波特率，默认 {DEFAULT_BAUD}")
    parser.add_argument("--addr", type=lambda x: int(x, 0), default=DEFAULT_IMAGE_ADDR, help="W25Q64 暂存地址，默认 0")
    parser.add_argument("--chunk", type=int, default=UPDATE_MAX_PAYLOAD, help=f"单包 payload 大小，默认 {UPDATE_MAX_PAYLOAD}")
    parser.add_argument("--ack-timeout", type=float, default=10.0, help="等待每包 ACK 的超时时间")
    parser.add_argument("--wait-boot", type=float, default=6.0, help="复位后等待 Bootloader 提示的时间")
    parser.add_argument("--no-reset-prompt", action="store_true", help="不提示手动复位，打开串口后立即发送")
    parser.add_argument("--skip-vector-check", action="store_true", help="跳过 APP 向量表检查")
    parser.add_argument(
        "--auth-key-env",
        default="OTA_FIRMWARE_HMAC_KEY",
        help="固件认证密钥环境变量名；未设置时读取User/OtaSecrets.h",
    )

    args = parser.parse_args()

    if args.chunk <= 0 or args.chunk > UPDATE_MAX_PAYLOAD:
        parser.error(f"--chunk 必须在 1~{UPDATE_MAX_PAYLOAD} 之间")

    # Bootloader 按 4KB 扇区擦除，暂存地址必须落在扇区起点。
    if args.addr < 0 or args.addr >= W25Q64_TOTAL_SIZE:
        parser.error("--addr 必须在 W25Q64 的 0x000000~0x7FFFFF 范围内")

    if args.addr % W25Q64_SECTOR_SIZE != 0:
        parser.error("--addr 必须按 4KB 对齐，例如 0x000000、0x001000")

    return args


if __name__ == "__main__":
    try:
        send_image(parse_args())
    except KeyboardInterrupt:
        print("\n用户取消")
        sys.exit(130)
    except Exception as exc:
        print(f"发送失败：{exc}")
        sys.exit(1)
