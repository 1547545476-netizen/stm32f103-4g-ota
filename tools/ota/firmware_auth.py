#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""发布端固件认证公共函数，必须与Hardware/OtaAuth.c保持完全一致。"""

from __future__ import annotations

import hashlib
import hmac
import os
import re
import struct
from pathlib import Path


AUTH_DOMAIN = b"STM32OTA1"
AUTH_KEY_SIZE = 32
AUTH_TAG_SIZE = 16
DEFAULT_SECRETS_H = Path(__file__).resolve().parents[2] / "User" / "OtaSecrets.h"


def parse_key_hex(text: str) -> bytes:
    """把64位十六进制设备密钥转换为32个原始字节。"""
    value = text.strip()
    if not re.fullmatch(r"[0-9A-Fa-f]{64}", value):
        raise ValueError("固件HMAC密钥必须正好是64个十六进制字符")
    return bytes.fromhex(value)


def load_device_key(
    env_name: str = "OTA_FIRMWARE_HMAC_KEY",
    secrets_h: Path = DEFAULT_SECRETS_H,
) -> bytes:
    """优先读环境变量，否则从本机且被Git忽略的OtaSecrets.h读取。"""
    env_value = os.environ.get(env_name)
    if env_value:
        return parse_key_hex(env_value)

    try:
        text = secrets_h.read_text(encoding="utf-8")
    except OSError as exc:
        raise ValueError(
            f"没有环境变量{env_name}，也无法读取设备配置：{secrets_h}"
        ) from exc

    match = re.search(
        r'^\s*#define\s+OTA_FIRMWARE_HMAC_KEY_HEX\s+"([^"]+)"',
        text,
        flags=re.MULTILINE,
    )
    if not match:
        raise ValueError(f"{secrets_h}中缺少OTA_FIRMWARE_HMAC_KEY_HEX")
    return parse_key_hex(match.group(1))


def calculate_auth_tag(image: bytes, version: int, key: bytes) -> bytes:
    """认证域标识、版本、小端长度和完整bin，返回HMAC-SHA256前16字节。"""
    if not 1 <= version <= 0xFFFFFFFF:
        raise ValueError("固件版本必须在1~0xFFFFFFFF之间")
    if len(key) != AUTH_KEY_SIZE:
        raise ValueError("固件HMAC密钥长度不是32字节")
    metadata = AUTH_DOMAIN + struct.pack("<II", version, len(image))
    return hmac.new(key, metadata + image, hashlib.sha256).digest()[:AUTH_TAG_SIZE]
