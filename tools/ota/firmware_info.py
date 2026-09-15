#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""计算4G OTA配置需要的APP大小、CRC32和固件认证标签。"""

from __future__ import annotations

import argparse
import struct
import zlib
from pathlib import Path

from firmware_auth import calculate_auth_tag, load_device_key


FLASH_APP_START = 0x08005000
FLASH_END_ADDR = 0x08010000
SRAM_START = 0x20000000
SRAM_END = 0x20005000


def main() -> None:
    parser = argparse.ArgumentParser(description="输出APP bin的大小、CRC32和向量表信息")
    parser.add_argument("bin", help="APP工程生成的bin文件")
    parser.add_argument("--version", type=int, help="固件版本；填写后同时计算HMAC认证标签")
    args = parser.parse_args()

    path = Path(args.bin)
    image = path.read_bytes()
    if len(image) < 8:
        raise ValueError("bin文件太小，不像有效的Cortex-M APP")

    stack, reset_handler = struct.unpack_from("<II", image, 0)
    reset_address = reset_handler & ~1
    crc32 = zlib.crc32(image) & 0xFFFFFFFF

    stack_ok = SRAM_START <= stack <= SRAM_END
    reset_ok = (reset_handler & 1) == 1 and FLASH_APP_START <= reset_address < FLASH_END_ADDR

    print(f"文件: {path.resolve()}")
    print(f"大小: {len(image)} 字节")
    print(f"CRC32: 0x{crc32:08X}")
    print(f"初始栈顶: 0x{stack:08X} ({'OK' if stack_ok else '错误'})")
    print(f"Reset_Handler: 0x{reset_handler:08X} ({'OK' if reset_ok else '错误'})")
    print()
    print("请填写到 User/main.h：")
    print(f"#define BOOT_4G_OTA_SIZE                {len(image)}UL")
    print(f"#define BOOT_4G_OTA_CRC32               0x{crc32:08X}UL")
    if args.version is not None:
        auth_tag = calculate_auth_tag(image, args.version, load_device_key())
        print(f'#define BOOT_4G_OTA_AUTH_TAG_HEX         "{auth_tag.hex().upper()}"')

    if not stack_ok or not reset_ok:
        raise ValueError("APP向量表不合法，请检查链接地址是否为0x08005000")


if __name__ == "__main__":
    main()
