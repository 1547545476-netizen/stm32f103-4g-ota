#ifndef __OTA_MANIFEST_H
#define __OTA_MANIFEST_H

#include "stm32f10x.h"
#include <stdint.h>
#include "main.h"

// OSS签名URL可能带有较长的查询参数，因此给URL预留500个可见字符。
// 数组还要额外加1字节保存C字符串结束符'\0'。
#define OTA_MANIFEST_URL_MAX_LEN    500U

/**
  * @brief  MQTT升级命令解析后的结构化信息。
  *
  * 云端发来的是一整段JSON文本，不方便后续代码直接使用。解析器会把JSON中的
  * version、size、crc32、auth和url分别放进结构体，下载函数再使用这些已检查的值。
  */
typedef struct {
	uint32_t version;                         // 云端固件版本号。
	uint32_t size;                            // app.bin准确字节数。
	uint32_t crc32;                           // app.bin整体CRC32。
	uint8_t authTag[OTA_AUTH_TAG_SIZE];       // HMAC-SHA256前16字节，证明固件来自可信发布端。
	char url[OTA_MANIFEST_URL_MAX_LEN + 1U];  // OSS固件下载URL。
} OtaManifest_t;

/**
  * @brief  解析并检查一条固定格式的OTA JSON命令。
  * @param  json     以'\0'结尾的MQTT消息正文。
  * @param  manifest 解析成功后保存版本、大小、CRC32和URL。
  * @retval 1=全部字段合法，0=缺字段、格式错误或数值越界。
  */
uint8_t OtaManifest_Parse(const char *json, OtaManifest_t *manifest);

#endif
