#ifndef __AIR780E_H
#define __AIR780E_H

#include "stm32f10x.h"
#include <stdint.h>
#include "main.h"

// Air780E/M100M AT firmware common UART baud rate.
// If there is no response, try 9600, 115200, or ask the seller for the default baud rate.
#define AIR780E_DEFAULT_BAUD        115200UL

// Public SIM cards usually support automatic APN acquisition.
// Empty string means: do not force a fixed APN such as "CMNET"; query the current PDP context instead.
// If a private network SIM card provides APN/user/password later, replace this with the real APN.
#define AIR780E_DEFAULT_APN         ""
#define AIR780E_HTTP_URL_MAX_LEN    500U
#define AIR780E_OTA_READ_CHUNK      200U
#define AIR780E_OTA_FILE_PATH       "/USER/HTTP/ota.bin"

// AT+HTTPACTION 的结果。statusCode 是 HTTP 状态码，dataLength 是响应正文长度。
typedef struct {
	uint16_t statusCode;
	uint32_t dataLength;
} Air780E_HttpResult_t;

void Air780E_Init(uint32_t baud);

uint8_t Air780E_SendCmd(const char *cmd, const char *expect, uint32_t timeoutMs);
uint8_t Air780E_WaitResp(const char *expect, uint32_t timeoutMs);

uint8_t Air780E_AT_Test(void);
uint8_t Air780E_CheckSimReady(void);
uint8_t Air780E_GetSignalQuality(uint8_t *rssi, uint8_t *ber);
uint8_t Air780E_WaitNetworkRegistered(uint32_t timeoutMs);
uint8_t Air780E_ActivatePdp(const char *apn);
uint8_t Air780E_NetworkReady(const char *apn, uint32_t timeoutMs);

uint8_t Air780E_HttpGetStart(const char *url,
	Air780E_HttpResult_t *result,
	uint32_t timeoutMs);
uint8_t Air780E_HttpTerminate(void);
uint8_t Air780E_ProvisionCaCertificate(const char *url, uint32_t expectedSize);
uint8_t Air780E_DownloadFirmwareToW25Q64(const char *url,
	uint32_t version,
	uint32_t expectedSize,
	uint32_t expectedCrc32,
	const uint8_t expectedAuthTag[OTA_AUTH_TAG_SIZE],
	uint32_t imageAddr);

#endif
