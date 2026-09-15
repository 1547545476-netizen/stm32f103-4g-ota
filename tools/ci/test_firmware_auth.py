#!/usr/bin/env python3
"""固定测试向量，防止发布工具的认证格式与STM32实现悄悄漂移。"""

from __future__ import annotations

import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "ota"))

from firmware_auth import calculate_auth_tag  # noqa: E402


def main() -> None:
    key = bytes(range(32))
    image = bytes(range(256))
    expected = bytes.fromhex("4D49F3FC5D4BCB6817B314728BCFF6FC")
    actual = calculate_auth_tag(image, 7, key)
    if actual != expected:
        raise RuntimeError(f"firmware auth vector mismatch: {actual.hex().upper()}")
    if calculate_auth_tag(image, 8, key) == expected:
        raise RuntimeError("version is not covered by firmware authentication")
    if calculate_auth_tag(image[:-1] + b"\x00", 7, key) == expected:
        raise RuntimeError("firmware bytes are not covered by authentication")
    print("firmware authentication vectors PASS")


if __name__ == "__main__":
    main()
