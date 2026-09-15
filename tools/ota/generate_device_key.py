#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""为当前设备生成新的随机固件认证密钥，并写入被Git忽略的配置头文件。"""

from __future__ import annotations

import argparse
import re
import secrets
import shutil
from pathlib import Path

from firmware_auth import DEFAULT_SECRETS_H


TEMPLATE_H = DEFAULT_SECRETS_H.with_name("OtaSecrets.example.h")


def main() -> None:
    parser = argparse.ArgumentParser(description="生成每设备独立的256位OTA HMAC密钥")
    parser.add_argument("--config-h", default=str(DEFAULT_SECRETS_H))
    parser.add_argument(
        "--force",
        action="store_true",
        help="已有合法密钥时仍轮换；轮换后旧发布端将不能再给该设备升级",
    )
    args = parser.parse_args()
    path = Path(args.config_h).resolve()

    if not path.exists():
        shutil.copyfile(TEMPLATE_H, path)
    text = path.read_text(encoding="utf-8")
    pattern = r'(?m)^(#define\s+OTA_FIRMWARE_HMAC_KEY_HEX\s+)"([^"]*)"\s*$'
    match = re.search(pattern, text)
    if not match:
        raise ValueError(f"{path}中缺少OTA_FIRMWARE_HMAC_KEY_HEX")
    if re.fullmatch(r"[0-9A-Fa-f]{64}", match.group(2)) and not args.force:
        print("配置中已经有合法密钥；需要轮换时显式添加 --force")
        return

    key_hex = secrets.token_hex(32).upper()
    text = re.sub(pattern, rf'\1"{key_hex}"', text, count=1)
    path.write_text(text, encoding="utf-8", newline="")
    print(f"已更新设备密钥：{path}")
    print("真实密钥未回显。请备份OtaSecrets.h，发布工具必须使用同一密钥。")


if __name__ == "__main__":
    main()
