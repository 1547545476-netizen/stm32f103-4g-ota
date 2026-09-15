#ifndef __OTA_SECRETS_H
#define __OTA_SECRETS_H

/*
 * Copy this file to User/OtaSecrets.h, then fill in the device-specific values.
 * OtaSecrets.h is intentionally ignored by Git and must never be committed.
 */
#define MQTT_BROKER_HOST              "replace-with-private-broker-host"
#define MQTT_BROKER_PORT              8883U
#define MQTT_USE_TLS                  1U
#define MQTT_CLIENT_ID                "replace-with-unique-client-id"
#define MQTT_USERNAME                 "replace-with-device-username"
#define MQTT_PASSWORD                 "replace-with-device-password"
#define MQTT_OTA_CMD_TOPIC            "ota/replace-device-id/cmd"
#define MQTT_OTA_STATUS_TOPIC         "ota/replace-device-id/status"

/*
 * 每台设备独立的32字节固件认证密钥，共64个十六进制字符。
 * 发布工具和设备必须使用同一密钥；不要把真实值提交到Git。
 */
#define OTA_FIRMWARE_HMAC_KEY_HEX     "replace-with-64-hex-character-hmac-key"

#endif
