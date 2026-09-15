#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""校验OSS固件并通过MQTT发布一条OTA升级命令。"""

from __future__ import annotations

import argparse
import getpass
import json
import os
import ssl
import struct
import threading
import time
import urllib.error
import urllib.request
import zlib
from pathlib import Path

from firmware_auth import calculate_auth_tag, load_device_key

import paho.mqtt.client as mqtt


FLASH_APP_START = 0x08005000
FLASH_END = 0x08010000
SRAM_START = 0x20000000
SRAM_END = 0x20005000


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="核对本地/OSS固件后，通过MQTT给STM32发布OTA命令"
    )
    parser.add_argument("--bin", required=True, help="本地APP bin文件")
    parser.add_argument("--version", required=True, type=int, help="新固件版本号")
    parser.add_argument("--url", required=True, help="OSS公开HTTPS下载地址")
    parser.add_argument("--host", default="broker.emqx.io", help="MQTT Broker域名")
    parser.add_argument("--port", default=8883, type=int, help="MQTT TLS端口")
    parser.add_argument(
        "--topic", default="ota/example-device/cmd", help="OTA命令Topic"
    )
    parser.add_argument(
        "--status-topic",
        default="ota/example-device/status",
        help="设备状态Topic",
    )
    parser.add_argument(
        "--ca",
        default="tmp/emqx/emqxsl-ca.crt",
        help="验证Broker服务器证书所用的CA文件",
    )
    parser.add_argument("--username", help="需要认证时填写MQTT用户名")
    parser.add_argument("--password", help="需要认证时填写MQTT密码")
    parser.add_argument(
        "--password-env",
        default="MQTT_PASSWORD",
        help="从环境变量读取密码；默认MQTT_PASSWORD，避免密码出现在命令历史",
    )
    parser.add_argument(
        "--wait", default=20, type=int, help="发布后等待设备状态的秒数"
    )
    parser.add_argument(
        "--auth-key-env",
        default="OTA_FIRMWARE_HMAC_KEY",
        help="固件认证密钥环境变量名；未设置时读取User/OtaSecrets.h",
    )
    return parser.parse_args()


def inspect_local_image(path: Path) -> tuple[bytes, int]:
    """读取APP并检查向量表，返回固件正文及其CRC32。"""
    image = path.read_bytes()
    if len(image) < 8:
        raise ValueError("APP bin太小，缺少栈顶指针和Reset_Handler")

    stack, reset_handler = struct.unpack_from("<II", image, 0)
    reset_address = reset_handler & ~1
    if not (SRAM_START <= stack <= SRAM_END):
        raise ValueError(f"APP初始栈顶不合法: 0x{stack:08X}")
    if (reset_handler & 1) == 0:
        raise ValueError(f"Reset_Handler不是Thumb地址: 0x{reset_handler:08X}")
    if not (FLASH_APP_START <= reset_address < FLASH_END):
        raise ValueError(f"Reset_Handler不在APP分区: 0x{reset_handler:08X}")

    return image, zlib.crc32(image) & 0xFFFFFFFF


def verify_remote_image(url: str, local_image: bytes, local_crc32: int) -> None:
    """实际下载OSS对象，确认URL指向的内容与本地APP完全一致。"""
    if not url.startswith("https://"):
        raise ValueError("OTA URL必须使用https://")

    request = urllib.request.Request(url, headers={"User-Agent": "stm32-ota-check/1.0"})
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            remote_image = response.read()
    except urllib.error.HTTPError as exc:
        raise RuntimeError(
            f"OSS URL访问失败，HTTP状态码={exc.code}；请检查文件名和公共读权限"
        ) from None
    except urllib.error.URLError as exc:
        raise RuntimeError(f"OSS URL访问失败: {exc.reason}") from None

    remote_crc32 = zlib.crc32(remote_image) & 0xFFFFFFFF
    print(f"OSS文件大小: {len(remote_image)} 字节")
    print(f"OSS文件CRC32: 0x{remote_crc32:08X}")
    if len(remote_image) != len(local_image) or remote_crc32 != local_crc32:
        raise ValueError("OSS文件与本地bin不一致，取消发布OTA命令")


def publish_command(args: argparse.Namespace, payload: str) -> None:
    """连接Broker、监听设备状态并发布OTA JSON。"""
    connected = threading.Event()
    command_result = threading.Event()
    connect_result = {"code": "timeout"}

    def on_connect(client, userdata, flags, reason_code, properties=None):
        connect_result["code"] = str(reason_code)
        if reason_code == 0:
            client.subscribe(args.status_topic, qos=0)
        connected.set()

    def on_message(client, userdata, message):
        text = message.payload.decode("utf-8", errors="replace")
        print(f"设备状态 [{message.topic}]: {text}")
        try:
            state = json.loads(text).get("state")
        except (json.JSONDecodeError, AttributeError):
            state = None
        # online只说明设备刚连上Broker；收到它以后仍需继续等待OTA命令处理结果。
        if state != "online":
            command_result.set()

    client_id = f"ota-publisher-{int(time.time())}"
    client = mqtt.Client(
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
        client_id=client_id,
        protocol=mqtt.MQTTv311,
    )
    if args.username is not None:
        password = args.password or os.environ.get(args.password_env)
        if password is None:
            password = getpass.getpass("MQTT password: ")
        client.username_pw_set(args.username, password)
    client.tls_set(ca_certs=str(Path(args.ca).resolve()), cert_reqs=ssl.CERT_REQUIRED)
    client.on_connect = on_connect
    client.on_message = on_message

    client.connect(args.host, args.port, keepalive=60)
    client.loop_start()
    try:
        if not connected.wait(15) or connect_result["code"] != "Success":
            raise RuntimeError(f"MQTT连接失败: {connect_result['code']}")

        # 留一点时间让状态Topic订阅在Broker端生效，然后再发布命令。
        time.sleep(0.5)
        print("等待设备返回accepted状态，同时观察板子USART1下载日志...")
        # 公共Broker不保存本条命令。设备若恰好正在重连，第一次发布可能早于订阅，
        # 因此最多重发6次、覆盖约30秒重连窗口；设备接受首条后会立即取消订阅，
        # 后续消息不会再次进入OTA解析，不会重复启动下载。
        for attempt in range(1, 7):
            info = client.publish(args.topic, payload, qos=1, retain=False)
            info.wait_for_publish(timeout=10)
            if info.rc != mqtt.MQTT_ERR_SUCCESS:
                raise RuntimeError(f"MQTT发布失败，错误码: {info.rc}")
            print(f"OTA命令已发布到: {args.topic}（第{attempt}次）")
            if command_result.wait(min(5, args.wait)):
                break
        if not command_result.is_set():
            print("未收到accepted/拒绝状态，请查看板子USART1日志确认是否收到命令")
    finally:
        client.disconnect()
        client.loop_stop()


def main() -> None:
    args = parse_args()
    bin_path = Path(args.bin).resolve()
    image, crc32 = inspect_local_image(bin_path)
    auth_tag = calculate_auth_tag(
        image, args.version, load_device_key(args.auth_key_env)
    )

    print(f"本地文件: {bin_path}")
    print(f"固件版本: {args.version}")
    print(f"本地大小: {len(image)} 字节")
    print(f"本地CRC32: 0x{crc32:08X}")
    print(f"固件认证标签: {auth_tag.hex().upper()}")

    verify_remote_image(args.url, image, crc32)

    manifest = {
        "cmd": "ota",
        "version": args.version,
        "size": len(image),
        "crc32": f"0x{crc32:08X}",
        "auth": auth_tag.hex().upper(),
        "url": args.url,
    }
    payload = json.dumps(manifest, separators=(",", ":"), ensure_ascii=True)
    print(f"OTA命令: {payload}")
    publish_command(args, payload)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError) as exc:
        # 输出到stdout，避免Windows PowerShell按旧代码页解码stderr而显示乱码。
        print(f"错误: {exc}")
        raise SystemExit(1) from None
