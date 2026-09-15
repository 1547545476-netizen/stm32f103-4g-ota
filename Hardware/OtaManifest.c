#include "OtaManifest.h"  // OtaManifest_t结构体和URL长度上限。
#include <string.h>        // strstr()/strlen()/strcmp()/memset()。

/**
  * @brief  找到指定JSON字段冒号后面的“值”的起点。
  *
  * 例如json为 {"version" : 2}，key为 "version"：
  * 1. strstr()先让p指向字段名开头；
  * 2. p跳过字段名和空白；
  * 3. 检查冒号，再跳过冒号后的空白；
  * 4. 最终返回指向字符'2'的指针。
  *
  * @note  这是面向本项目固定OTA格式的小型解析器，不是通用JSON库。
  *        云端命令的字段名必须写成双引号形式，字段顺序可以改变。
  */
static const char *OtaManifest_FindValue(const char *json, const char *key)
{
	const char *p;

	// 在整条JSON文本中寻找字段名，例如寻找"version"。
	p = strstr(json, key);
	if (p == 0)
	{
		return 0;
	}
	// 同一个安全关键字段出现两次时直接拒绝，避免不同解析器采用不同一项。
	if (strstr(p + strlen(key), key) != 0)
	{
		return 0;
	}

	// 跳过字段名。此时p位于字段名后面，通常是空格或冒号。
	p += strlen(key);
	while ((*p == ' ') || (*p == '\t') || (*p == '\r') || (*p == '\n'))
	{
		p++;
	}
	// JSON的“字段名”和“值”之间必须有冒号。
	if (*p != ':')
	{
		return 0;
	}

	// 越过冒号，并允许冒号后存在空格、Tab或换行。
	p++;
	while ((*p == ' ') || (*p == '\t') || (*p == '\r') || (*p == '\n'))
	{
		p++;
	}
	return p;
}

static uint8_t OtaManifest_HasObjectEnvelope(const char *json)
{
	const char *start;
	const char *end;

	start = json;
	while ((*start == ' ') || (*start == '\t') || (*start == '\r') || (*start == '\n'))
	{
		start++;
	}
	if (*start != '{')
	{
		return 0U;
	}
	end = json + strlen(json);
	while ((end > start) && ((end[-1] == ' ') || (end[-1] == '\t') ||
		(end[-1] == '\r') || (end[-1] == '\n')))
	{
		end--;
	}
	return ((end > start) && (end[-1] == '}')) ? 1U : 0U;
}

/**
  * @brief  把十进制文本解析成0~0xFFFFFFFF范围内的uint32_t整数。
  *
  * 例如p指向字符串"12345,"，循环过程为：1 -> 12 -> 123 -> 1234 -> 12345。
  * 每次都执行 num = num * 10 + digit，并在计算前检查是否会溢出。
  */
static uint8_t OtaManifest_ParseUint32(const char *p, uint32_t *value)
{
	uint32_t num = 0;       // 已经解析出来的数值。
	uint32_t digit;         // 当前字符代表的0~9。
	uint8_t hasDigit = 0;   // 防止空字符串被错误地当成数字0。

	if ((p == 0) || (value == 0))
	{
		return 0;
	}

	// 指针p每次向右移动一个字符，直到遇到逗号、右大括号或非法字符。
	while ((*p >= '0') && (*p <= '9'))
	{
		hasDigit = 1;
		// ASCII字符'7'减去字符'0'得到整数7。
		digit = (uint32_t)(*p - '0');
		// 不能先算num*10+digit再判断，因为溢出发生后原值已经丢失。
		// 把不等式 num*10+digit <= 0xFFFFFFFF 变形，就得到下面的判断式。
		if (num > ((0xFFFFFFFFUL - digit) / 10UL))
		{
			return 0;
		}
		num = num * 10UL + digit;
		p++;
	}

	if (!hasDigit)
	{
		return 0;
	}

	// 数字后允许有空白，但再往后只能是下一个字段的逗号或JSON结束的'}'。
	while ((*p == ' ') || (*p == '\t') || (*p == '\r') || (*p == '\n'))
	{
		p++;
	}
	if ((*p != ',') && (*p != '}'))
	{
		return 0;
	}

	// 只有所有检查通过后才写输出参数，失败时调用者原变量不会拿到半成品。
	*value = num;
	return 1;
}

/**
  * @brief  读取一对双引号之间的JSON字符串，并补上C字符串结束符。
  *
  * 例如p指向 "https://a.com/app.bin"，函数只把双引号内部的URL复制到out。
  * @note  本项目的cmd和OSS URL不需要JSON转义。主动拒绝反斜杠，可以避免
  *        把\n、\uXXXX或被截断的复杂转义误当成合法URL。
  */
static uint8_t OtaManifest_ParseString(const char *p,
	char *out,
	uint16_t outSize)
{
	uint16_t len = 0;  // 已经复制到out中的字符数，同时也是下一个写入下标。

	if ((p == 0) || (out == 0) || (outSize == 0U) || (*p != '"'))
	{
		return 0;
	}
	// 跳过开头的双引号，让p指向字符串第一个实际字符。
	p++;

	// 一直复制到字符串结尾双引号。若先遇到'\0'，说明JSON被截断。
	while ((*p != '\0') && (*p != '"'))
	{
		// 0x20以下是控制字符；反斜杠代表转义；out最后1字节必须留给'\0'。
		if (((uint8_t)*p < 0x20U) || (*p == '\\') || (len >= (outSize - 1U)))
		{
			return 0;
		}
		// 复制当前字符，然后输入指针p和输出下标len同时加1。
		out[len++] = *p++;
	}

	if (*p != '"')
	{
		return 0;
	}
	// JSON正文中没有C字符串结束符，因此由解析器在有效字符后补一个。
	out[len] = '\0';
	return 1;
}

/**
  * @brief  把一个十六进制字符转换成0~15。
  *
  * 同时接受数字、小写a~f和大写A~F。例如字符'B'转换为数值11。
  */
static uint8_t OtaManifest_HexDigit(char ch, uint8_t *value)
{
	if ((ch >= '0') && (ch <= '9'))
	{
		*value = (uint8_t)(ch - '0');
		return 1;
	}
	if ((ch >= 'a') && (ch <= 'f'))
	{
		*value = (uint8_t)(ch - 'a' + 10);
		return 1;
	}
	if ((ch >= 'A') && (ch <= 'F'))
	{
		*value = (uint8_t)(ch - 'A' + 10);
		return 1;
	}
	return 0;
}

/**
  * @brief  解析固定格式"0x12345678"的CRC32字符串。
  *
  * 每个十六进制字符代表4个二进制位。crc左移4位腾出低四位，再用按位或
  * 放入新数字。连续处理8次后，正好得到一个32位CRC值。
  */
static uint8_t OtaManifest_ParseCrc32(const char *p, uint32_t *value)
{
	uint8_t digit;      // 当前十六进制字符转换出的0~15。
	uint8_t i;          // 处理8个十六进制字符的循环下标。
	uint32_t crc = 0;   // 逐步拼出的32位数值。

	// 前三个字符必须是双引号、0、x，例如"0x89ABCDEF"。
	if ((p == 0) || (value == 0) || (p[0] != '"') ||
		(p[1] != '0') || ((p[2] != 'x') && (p[2] != 'X')))
	{
		return 0;
	}

	for (i = 0; i < 8U; i++)
	{
		if (!OtaManifest_HexDigit(p[3U + i], &digit))
		{
			return 0;
		}
		// 例：已有0x12，读到字符'3'后变为(0x12<<4)|3 = 0x123。
		crc = (crc << 4) | digit;
	}

	// 下标3~10是8个CRC字符，因此下标11必须是结束双引号。
	if (p[11] != '"')
	{
		return 0;
	}

	*value = crc;
	return 1;
}

/**
  * @brief  解析固定长度的固件认证标签。
  * @note   JSON里是32个可打印十六进制字符，设备内保存为16个原始字节。
  */
static uint8_t OtaManifest_ParseAuthTag(const char *p,
	uint8_t tag[OTA_AUTH_TAG_SIZE])
{
	char text[OTA_AUTH_TAG_SIZE * 2U + 1U];
	uint32_t i;
	uint8_t digit;
	uint8_t high;
	uint8_t low;
	uint8_t anyValue = 0U;

	if ((p == 0) || (tag == 0) || (*p != '"'))
	{
		return 0U;
	}
	p++;
	for (i = 0U; i < OTA_AUTH_TAG_SIZE * 2U; i++)
	{
		if (!OtaManifest_HexDigit(p[i], &digit))
		{
			return 0U;
		}
		text[i] = p[i];
	}
	if (p[OTA_AUTH_TAG_SIZE * 2U] != '"')
	{
		return 0U;
	}
	text[OTA_AUTH_TAG_SIZE * 2U] = '\0';
	for (i = 0U; i < OTA_AUTH_TAG_SIZE; i++)
	{
		if (!OtaManifest_HexDigit(text[i * 2U], &high) ||
			!OtaManifest_HexDigit(text[i * 2U + 1U], &low))
		{
			return 0U;
		}
		tag[i] = (uint8_t)((high << 4U) | low);
		anyValue |= tag[i];
	}
	return (anyValue != 0U) ? 1U : 0U;
}

/**
  * @brief  解析MQTT命令Topic收到的固定OTA JSON命令。
  *
  * 要求格式包含：
  * {"cmd":"ota","version":2,"size":12345,
  *  "crc32":"0x12345678","auth":"001122...EEFF","url":"https://.../app.bin"}
  */
uint8_t OtaManifest_Parse(const char *json, OtaManifest_t *manifest)
{
	const char *p;  // 每次指向当前字段值的第一个字符。
	char cmd[8];    // 临时保存"ota"，留有足够空间和末尾'\0'。

	if ((json == 0) || (manifest == 0))
	{
		return 0;
	}
	if (!OtaManifest_HasObjectEnvelope(json))
	{
		return 0;
	}
	// 先清零输出结构体。后面任何一步失败，调用者也不会看到上一次解析的旧值。
	memset(manifest, 0, sizeof(OtaManifest_t));

	// 第1项cmd必须严格等于"ota"，防止把其他MQTT业务命令误当成升级。
	p = OtaManifest_FindValue(json, "\"cmd\"");
	if (!OtaManifest_ParseString(p, cmd, sizeof(cmd)) || (strcmp(cmd, "ota") != 0))
	{
		return 0;
	}

	// 第2项version是大于0的十进制版本号，后续用于拒绝旧版本降级。
	p = OtaManifest_FindValue(json, "\"version\"");
	if (!OtaManifest_ParseUint32(p, &manifest->version) || (manifest->version == 0U))
	{
		return 0;
	}

	// 第3项size必须等于app.bin的准确字节数，不能写成Flash分区大小。
	p = OtaManifest_FindValue(json, "\"size\"");
	if (!OtaManifest_ParseUint32(p, &manifest->size) || (manifest->size == 0U))
	{
		return 0;
	}

	// 第4项crc32用于下载完成后校验整个app.bin，格式固定为"0x"加8位十六进制。
	p = OtaManifest_FindValue(json, "\"crc32\"");
	if (!OtaManifest_ParseCrc32(p, &manifest->crc32))
	{
		return 0;
	}

	// 第5项auth认证版本号、大小和bin正文；没有正确设备密钥就无法伪造。
	p = OtaManifest_FindValue(json, "\"auth\"");
	if (!OtaManifest_ParseAuthTag(p, manifest->authTag))
	{
		return 0;
	}

	// 第6项url是OSS文件的完整HTTP/HTTPS下载地址，可以包含签名查询参数。
	p = OtaManifest_FindValue(json, "\"url\"");
	if (!OtaManifest_ParseString(p, manifest->url, sizeof(manifest->url)))
	{
		return 0;
	}

	// 六个字段全部存在且格式合法，才把整条命令判定为有效。
	return 1;
}
