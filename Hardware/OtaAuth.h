#ifndef __OTA_AUTH_H
#define __OTA_AUTH_H

#include "stm32f10x.h"
#include <stdint.h>
#include "main.h"

#define OTA_AUTH_KEY_SIZE       32U

/**
  * @brief  把32个十六进制字符转换成16字节固件认证标签。
  * @note   发布工具发送的是HMAC-SHA256的前16字节，共128位。
  */
uint8_t OtaAuth_ParseTagHex(const char *text, uint8_t tag[OTA_AUTH_TAG_SIZE]);

/** @brief 判断标签是否不是全0；全0代表旧控制块中没有认证信息。 */
uint8_t OtaAuth_IsTagPresent(const uint8_t tag[OTA_AUTH_TAG_SIZE]);

/** @brief 检查OtaSecrets.h中的设备HMAC密钥是否为合法的64位十六进制文本。 */
uint8_t OtaAuth_IsConfigured(void);

/** @brief 计算STM32片内Flash指定区域的固件认证标签。 */
uint8_t OtaAuth_CalculateInternalImage(uint32_t address,
	uint32_t size,
	uint32_t version,
	uint8_t tag[OTA_AUTH_TAG_SIZE]);

/** @brief 验证W25Q64指定区域是否与期望认证标签一致。 */
uint8_t OtaAuth_VerifyExternalImage(uint32_t address,
	uint32_t size,
	uint32_t version,
	const uint8_t expectedTag[OTA_AUTH_TAG_SIZE]);

#endif
