#!/usr/bin/env python3
"""Check OTA partition invariants and optional local APP build artifacts."""

from __future__ import annotations

import argparse
import re
import struct
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MAIN_H = ROOT / "User" / "main.h"
APP_BIN = ROOT / "AppTest" / "Objects" / "AppTest.bin"


def macro(name: str) -> int:
    text = MAIN_H.read_text(encoding="utf-8")
    match = re.search(rf"(?m)^#define\s+{re.escape(name)}\s+(0x[0-9A-Fa-f]+|\d+)(?:U|UL)?", text)
    if not match:
        raise RuntimeError(f"cannot find numeric macro {name}")
    return int(match.group(1), 0)


def check_source_layout() -> None:
    flash_base = macro("FLASH_BASE_ADDR")
    flash_size = macro("FLASH_SIZE")
    page_size = macro("FLASH_PAGE_SIZE")
    boot_pages = macro("FLASH_PAGE_BOOTLOADER_NUM")
    image_addr = macro("OTA_IMAGE_STORE_ADDR")
    backup_addr = macro("OTA_BACKUP_STORE_ADDR")
    cb_version = macro("OTA_CB_FORMAT_VERSION")
    auth_tag_size = macro("OTA_AUTH_TAG_SIZE")

    app_start = flash_base + page_size * boot_pages
    app_size = flash_size - page_size * boot_pages
    if app_start != 0x08005000 or app_size != 0xB000:
        raise RuntimeError(f"unexpected APP partition: start=0x{app_start:08X} size=0x{app_size:X}")
    if image_addr % 0x1000 or backup_addr % 0x1000:
        raise RuntimeError("W25Q64 image slots must be 4KB aligned")
    if not (image_addr + app_size <= backup_addr or backup_addr + app_size <= image_addr):
        raise RuntimeError("W25Q64 download and rollback slots overlap")
    if not (ROOT / "User" / "OtaSecrets.example.h").is_file():
        raise RuntimeError("missing OtaSecrets.example.h")
    if cb_version != 3 or auth_tag_size != 16:
        raise RuntimeError("unexpected authenticated OTA control-block format")

    template = (ROOT / "User" / "OtaSecrets.example.h").read_text(encoding="utf-8")
    if "OTA_FIRMWARE_HMAC_KEY_HEX" not in template:
        raise RuntimeError("missing firmware authentication key template")
    boot_project = (ROOT / "Project.uvprojx").read_text(encoding="utf-8")
    app_project = (ROOT / "AppTest" / "AppTest.uvprojx").read_text(encoding="utf-8")
    if "OtaAuth.c" not in boot_project:
        raise RuntimeError("OtaAuth.c missing from Bootloader project")
    if "OtaAuth.c" in app_project:
        raise RuntimeError("firmware authentication key must not be linked into public APP bin")

    result = subprocess.run(
        ["git", "ls-files", "--error-unmatch", "User/OtaSecrets.h"],
        cwd=ROOT,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    if result.returncode == 0:
        raise RuntimeError("User/OtaSecrets.h must not be tracked by Git")

    print(f"source layout PASS: APP=0x{app_start:08X}+0x{app_size:X}")
    print(f"W25Q64 slots PASS: download=0x{image_addr:06X}, backup=0x{backup_addr:06X}")
    print("credential tracking PASS")
    print("authenticated OTA layout PASS")


def check_app_bin() -> None:
    image = APP_BIN.read_bytes()
    app_size = macro("FLASH_SIZE") - macro("FLASH_PAGE_SIZE") * macro("FLASH_PAGE_BOOTLOADER_NUM")
    if len(image) < 8 or len(image) > app_size:
        raise RuntimeError(f"invalid APP bin size: {len(image)}")
    stack, reset_handler = struct.unpack_from("<II", image)
    reset_address = reset_handler & ~1
    if not (0x20000000 <= stack <= 0x20005000):
        raise RuntimeError(f"invalid initial MSP: 0x{stack:08X}")
    if not ((reset_handler & 1) and 0x08005000 <= reset_address < 0x08010000):
        raise RuntimeError(f"invalid Reset_Handler: 0x{reset_handler:08X}")

    secrets_h = ROOT / "User" / "OtaSecrets.h"
    if secrets_h.is_file():
        config = secrets_h.read_text(encoding="utf-8")
        match = re.search(r'OTA_FIRMWARE_HMAC_KEY_HEX\s+"([0-9A-Fa-f]{64})"', config)
        if match and match.group(1).encode("ascii") in image:
            raise RuntimeError("firmware HMAC key leaked into public APP artifact")
    print(f"APP artifact PASS: {len(image)} bytes, MSP=0x{stack:08X}, reset=0x{reset_handler:08X}")
    print("APP firmware-key isolation PASS")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-only", action="store_true", help="do not require a local Keil APP build")
    args = parser.parse_args()
    check_source_layout()
    if not args.source_only:
        if not APP_BIN.is_file():
            raise RuntimeError(f"missing local APP build: {APP_BIN}")
        check_app_bin()


if __name__ == "__main__":
    main()
