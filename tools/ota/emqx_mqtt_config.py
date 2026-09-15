#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把EMQX连接参数写入不会提交到Git的User/OtaSecrets.h。"""

from __future__ import annotations

import argparse
import getpass
from pathlib import Path
import re
import secrets
import shutil


USER_DIR = Path(__file__).resolve().parents[2] / "User"
DEFAULT_CONFIG_H = USER_DIR / "OtaSecrets.h"
DEFAULT_TEMPLATE_H = USER_DIR / "OtaSecrets.example.h"


def validate_c_string(label: str, value: str) -> str:
    """拒绝会破坏C字符串或AT命令双引号的字符。"""
    value = value.strip()
    if not value:
        raise ValueError(f"{label}不能为空")
    if any(ch in value for ch in ('"', "\\", "\r", "\n")):
        raise ValueError(f"{label}不能包含双引号、反斜杠或换行")
    return value


def update_config_h(path: Path, defines: list[tuple[str, str]]) -> None:
    """只替换本机配置头文件中已有的MQTT宏。"""
    if not path.exists():
        if not DEFAULT_TEMPLATE_H.exists():
            raise ValueError(f"缺少配置模板：{DEFAULT_TEMPLATE_H}")
        path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(DEFAULT_TEMPLATE_H, path)

    text = path.read_text(encoding="utf-8")

    for name, value in defines:
        pattern = rf"(?m)^#define[ \t]+{re.escape(name)}[ \t]+.*$"
        replacement = f"#define {name:<36} {value}"
        text, count = re.subn(pattern, lambda _: replacement, text, count=1)
        if count != 1:
            raise ValueError(f"在{path}中没有唯一找到宏{name}")

    path.write_text(text, encoding="utf-8", newline="")


def ensure_firmware_auth_key(path: Path) -> bool:
    """新设备没有合法密钥时自动生成；已有密钥保持不变，避免意外失去升级能力。"""
    text = path.read_text(encoding="utf-8")
    pattern = r'(?m)^(#define\s+OTA_FIRMWARE_HMAC_KEY_HEX\s+)"([^"]*)"\s*$'
    match = re.search(pattern, text)
    if not match:
        raise ValueError(f"在{path}中没有找到OTA_FIRMWARE_HMAC_KEY_HEX")
    if re.fullmatch(r"[0-9A-Fa-f]{64}", match.group(2)):
        return False
    key_hex = secrets.token_hex(32).upper()
    text = re.sub(pattern, rf'\1"{key_hex}"', text, count=1)
    path.write_text(text, encoding="utf-8", newline="")
    return True


def main() -> None:
    parser = argparse.ArgumentParser(
        description="配置EMQX MQTT连接参数并更新本机User/OtaSecrets.h"
    )
    parser.add_argument(
        "--endpoint",
        required=True,
        help="EMQX部署概览中的TLS Endpoint域名，不带协议和端口",
    )
    parser.add_argument(
        "--username",
        required=True,
        help="EMQX访问控制中创建的用户名",
    )
    parser.add_argument(
        "--password",
        help="EMQX密码；省略时隐藏输入，避免留在命令历史中",
    )
    parser.add_argument("--port", type=int, default=8883, help="MQTT TLS端口")
    parser.add_argument(
        "--client-id",
        default="example-mqtt-client",
        help="MQTT ClientId，同一Broker内必须唯一",
    )
    parser.add_argument(
        "--device-id",
        default="example-device",
        help="组成OTA Topic的设备标识",
    )
    parser.add_argument(
        "--config-h",
        "--main-h",
        dest="config_h",
        default=str(DEFAULT_CONFIG_H),
        help="本机配置头文件路径；--main-h作为旧命令兼容别名保留",
    )
    args = parser.parse_args()

    endpoint = validate_c_string("Endpoint", args.endpoint)
    username = validate_c_string("Username", args.username)
    client_id = validate_c_string("ClientId", args.client_id)
    device_id = validate_c_string("DeviceId", args.device_id)
    password = args.password or getpass.getpass("EMQX password: ")
    password = validate_c_string("Password", password)

    if any(ch in endpoint for ch in ("/", ":")):
        raise ValueError("Endpoint只填写域名，不要包含ssl://、端口或斜杠")
    if not (1 <= args.port <= 65535):
        raise ValueError("端口必须在1~65535范围内")
    if len(client_id) > 64:
        raise ValueError("ClientId不能超过64字符")
    if any(ch not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-" for ch in device_id):
        raise ValueError("DeviceId只能包含字母、数字、下划线和短横线")

    cmd_topic = f"ota/{device_id}/cmd"
    status_topic = f"ota/{device_id}/status"
    defines = [
        ("MQTT_BROKER_HOST", f'"{endpoint}"'),
        ("MQTT_BROKER_PORT", f"{args.port}U"),
        ("MQTT_USE_TLS", "1U"),
        ("MQTT_CLIENT_ID", f'"{client_id}"'),
        ("MQTT_USERNAME", f'"{username}"'),
        ("MQTT_PASSWORD", f'"{password}"'),
        ("MQTT_OTA_CMD_TOPIC", f'"{cmd_topic}"'),
        ("MQTT_OTA_STATUS_TOPIC", f'"{status_topic}"'),
    ]

    config_h_path = Path(args.config_h).resolve()
    update_config_h(config_h_path, defines)
    generated_key = ensure_firmware_auth_key(config_h_path)
    print(f"\n已更新：{config_h_path}")
    print(f"命令Topic：{cmd_topic}")
    print(f"状态Topic：{status_topic}")
    print("密码没有回显；OtaSecrets.h已被.gitignore排除，请继续避免手工强制提交。")
    if generated_key:
        print("已同时生成每设备唯一的OTA认证密钥，真实值未回显。")


if __name__ == "__main__":
    main()
