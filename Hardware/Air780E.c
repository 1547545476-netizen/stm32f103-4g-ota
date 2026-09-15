#include "Air780E.h"
#include "Uart2.h"
#include "Serial.h"
#include "Delay.h"
#include "W25Q64.h"
#include "AT24C02.h"
#include "main.h"
#include <stdio.h>
#include <string.h>

#define AIR780E_FSREAD_TRAILER       "\r\nOK\r\n"
#define AIR780E_FSREAD_TRAILER_LEN   (sizeof(AIR780E_FSREAD_TRAILER) - 1U)

static uint8_t Air780E_OtaBuffer[AIR780E_OTA_READ_CHUNK];

/**
  * @brief  等待一条包含指定前缀的AT文本行完整接收。
  * @note   只找到前缀还不够，数字可能仍在串口线上；看到行尾\r\n后再解析。
  */
static uint8_t Air780E_WaitLine(const char *prefix, uint32_t timeoutMs)
{
	uint32_t elapsed;
	char *line;

	for (elapsed = 0; elapsed < timeoutMs; elapsed++)
	{
		line = strstr(Uart2_GetRxBuffer(), prefix);
		if ((line != 0) && (strstr(line, "\r\n") != 0))
		{
			return 1;
		}

		if ((strstr(Uart2_GetRxBuffer(), "ERROR") != 0) || Uart2_IsRxOverflow())
		{
			return 0;
		}

		Delay_ms(1);
	}

	return 0;
}

/**
  * @brief  从字符串当前位置读取一个无符号十进制整数。
  * @param  p    : 指向字符串指针的地址，函数会把 *p 移动到数字后面。
  * @param  value: 保存解析出的数值。
  * @retval 1=解析成功，0=当前位置不是数字。
  *
  * 示例：
  * 输入字符串 "+CSQ: 20,0"，当 *p 指向 '2' 时，函数会读出 20。
  */
static uint8_t Air780E_ReadUint(const char **p, uint32_t *value)
{
	uint32_t num = 0;
	uint8_t hasDigit = 0;

	while ((**p == ' ') || (**p == '\t'))
	{
		(*p)++;
	}

	while ((**p >= '0') && (**p <= '9'))
	{
		hasDigit = 1;
		num = num * 10U + (uint32_t)(**p - '0');
		(*p)++;
	}

	if (!hasDigit)
	{
		return 0;
	}

	*value = num;
	return 1;
}

/**
  * @brief  判断网络注册查询响应里是否出现“已注册”状态。
  * @param  resp  : 模块返回的 AT 响应字符串。
  * @param  prefix: 响应前缀，例如 "+CREG:"、"+CGREG:"、"+CEREG:"。
  * @retval 1=已注册，0=未注册。
  *
  * AT 指令含义：
  * AT+CREG?  查询普通网络注册状态。
  * AT+CGREG? 查询分组数据网络注册状态。
  * AT+CEREG? 查询 LTE/EPS 网络注册状态，4G 模块通常重点看它。
  *
  * 典型返回：
  * +CEREG: 0,0  未注册。
  * +CEREG: 0,1  已注册本地网络。
  * +CEREG: 0,2  正在搜索网络。
  * +CEREG: 0,3  注册被拒绝。
  * +CEREG: 0,5  已注册漫游网络。
  *
  * 判断依据：
  * stat 等于 1 或 5，说明模块已经注册到运营商网络。
  */
static uint8_t Air780E_HasRegisteredState(const char *resp, const char *prefix)
{
	const char *p;
	uint32_t firstValue;
	uint32_t state;

	p = strstr(resp, prefix);
	while (p != 0)
	{
		p += strlen(prefix);
		if (!Air780E_ReadUint(&p, &firstValue))
		{
			p = strstr(p, prefix);
			continue;
		}

		// 查询响应可能是 "+CEREG: <stat>"，也可能是
		// "+CEREG: <n>,<stat>"。有逗号时第二个数字才是真正的注册状态。
		while ((*p == ' ') || (*p == '\t'))
		{
			p++;
		}

		state = firstValue;
		if (*p == ',')
		{
			p++;
			if (!Air780E_ReadUint(&p, &state))
			{
				p = strstr(p, prefix);
				continue;
			}
		}

		if ((state == 1U) || (state == 5U))
		{
			return 1;
		}

		p = strstr(p, prefix);
	}

	return 0;
}

/**
  * @brief  配置或查询 APN。
  * @param  apn: APN 字符串；传 0 或空字符串时使用模块自动获取的 APN。
  * @retval 1=成功，0=失败。
  *
  * AT 指令含义：
  * AT+CGDCONT? 查询当前 PDP 上下文，能看到 cid、网络类型、APN、IP 地址。
  * AT+CGDCONT=1,"IP","xxx" 设置 cid=1 的 APN 为 xxx。
  *
  * 典型返回：
  * +CGDCONT: 1,"IP","CMNET","10.xx.xx.xx",...
  * OK
  *
  * 判断依据：
  * 公网卡通常不需要手动设置 APN，所以默认只查询 AT+CGDCONT?。
  * 如果以后使用专网卡，再传入明确 APN，函数才会发送设置命令。
  */
static uint8_t Air780E_SetApn(const char *apn)
{
	if ((apn == 0) || (apn[0] == '\0'))
	{
		Serial_Printf("Use auto APN, query CGDCONT\r\n");
		// AT+CGDCONT?：查询PDP上下文和模块当前使用的APN。
		// 成功返回 +CGDCONT: ... 后跟 OK；失败返回 ERROR 或超时。
		return Air780E_SendCmd("AT+CGDCONT?", "OK", 3000);
	}

	Uart2_ClearRxBuffer();

	// AT+CGDCONT=1,"IP","<apn>"：把cid=1的数据连接配置成IPv4并指定APN。
	// 成功返回 OK；APN错误或模块不接受配置时返回 ERROR。
	Serial_SendString("4G-> AT+CGDCONT=1,\"IP\",\"");
	Serial_SendString((char *)apn);
	Serial_SendString("\"\r\n");

	Uart2_SendString("AT+CGDCONT=1,\"IP\",\"");
	Uart2_SendString(apn);
	Uart2_SendString("\"\r\n");

	if (Air780E_WaitResp("OK", 3000))
	{
		Serial_Printf("4G<- ok\r\n");
		return 1;
	}

	Serial_Printf("Set APN failed\r\n");
	Serial_SendString(Uart2_GetRxBuffer());
	Serial_SendString("\r\n");
	return 0;
}

/**
  * @brief  初始化 Air780E/M100M 通信串口。
  * @param  baud: 4G 模块 AT 串口波特率。
  *
  * 这里初始化的是 USART2：
  * PA2 = USART2_TX，接 4G 模块 RXD。
  * PA3 = USART2_RX，接 4G 模块 TXD。
  */
void Air780E_Init(uint32_t baud)
{
	Uart2_Init(baud);
	Serial_Printf("Air780E USART2 init ok\r\n");
}

/**
  * @brief  等待 4G 模块返回指定字符串。
  * @param  expect   : 期望字符串，例如 "OK"、"+CPIN:READY"。
  * @param  timeoutMs: 超时时间，单位 ms。
  * @retval 1=等到期望字符串，0=超时或收到 ERROR。
  *
  * 原理：
  * USART2_IRQHandler() 会把模块返回的字符放入 Uart2_RxBuffer。
  * 本函数循环查看 Uart2_RxBuffer 里有没有 expect。
  */
uint8_t Air780E_WaitResp(const char *expect, uint32_t timeoutMs)
{
	uint32_t elapsed;
	char *rx;

	for (elapsed = 0; elapsed < timeoutMs; elapsed++)
	{
		rx = Uart2_GetRxBuffer();

		if ((expect != 0) && (strstr(rx, expect) != 0))
		{
			return 1;
		}

		if (strstr(rx, "ERROR") != 0)
		{
			return 0;
		}

		Delay_ms(1);
	}

	return 0;
}

/**
  * @brief  发送一条 AT 指令并等待期望响应。
  * @param  cmd      : AT 指令，不需要带 "\r\n"，例如 "AT"。
  * @param  expect   : 期望响应，例如 "OK"。
  * @param  timeoutMs: 超时时间，单位 ms。
  * @retval 1=成功，0=失败。
  *
  * AT 指令格式：
  * STM32 实际发送的是 cmd + "\r\n"。
  * 例如传入 "AT"，真正发给模块的是 "AT\r\n"。
  *
  * 返回值判断：
  * 收到 expect，例如 "OK"，认为成功。
  * 收到 "ERROR" 或等待超时，认为失败。
  */
uint8_t Air780E_SendCmd(const char *cmd, const char *expect, uint32_t timeoutMs)
{
	if (cmd == 0)
	{
		return 0;
	}

	Uart2_ClearRxBuffer();

	Serial_SendString("4G-> ");
	Serial_SendString((char *)cmd);
	Serial_SendString("\r\n");

	Uart2_SendString(cmd);
	Uart2_SendString("\r\n");

	if (Air780E_WaitResp(expect, timeoutMs))
	{
		Serial_Printf("4G<- ok\r\n");
		return 1;
	}

	Serial_Printf("4G<- timeout/error\r\n");
	Serial_SendString(Uart2_GetRxBuffer());
	Serial_SendString("\r\n");
	return 0;
}

/**
  * @brief  第一阶段 AT 通信测试。
  * @retval 1=基础通信成功，0=失败。
  *
  * 用到的 AT 指令：
  * AT        测试模块是否有响应。正常返回 OK。
  * ATE0      关闭回显。正常返回 OK。关闭后返回数据更干净。
  * ATI       查询模块信息。正常返回模块型号/版本信息和 OK。
  * AT+CPIN?  查询 SIM 卡状态。READY 表示 SIM 卡可用。
  * AT+CSQ    查询信号质量。rssi=99 表示未知或无信号。
  *
  * 注意：
  * 这个函数只能说明串口、SIM、信号基本正常，不代表已经能访问公网。
  */
uint8_t Air780E_AT_Test(void)
{
	Serial_Printf("Air780E AT test start\r\n");

	// AT：测试STM32与4G模块的串口通信。成功返回 OK，失败时无响应或返回 ERROR。
	if (!Air780E_SendCmd("AT", "OK", 1000))
	{
		Serial_Printf("Air780E no AT response\r\n");
		return 0;
	}

	// ATE0：关闭AT命令回显。成功返回 OK，之后响应中不再重复显示发送的命令。
	Air780E_SendCmd("ATE0", "OK", 1000);
	// ATI：查询模块型号和固件版本。成功返回模块信息，最后返回 OK。
	Air780E_SendCmd("ATI", "OK", 1000);

	if (!Air780E_CheckSimReady())
	{
		return 0;
	}

	if (!Air780E_GetSignalQuality(0, 0))
	{
		return 0;
	}

	Serial_Printf("Air780E AT test pass\r\n");
	return 1;
}

/**
  * @brief  检查 SIM 卡是否就绪。
  * @retval 1=SIM 卡就绪，0=未就绪或查询失败。
  *
  * AT 指令：
  * AT+CPIN?
  *
  * 典型返回：
  * +CPIN:READY        SIM 卡已经就绪。
  * +CPIN:SIM PIN      需要输入 PIN 码。
  * +CPIN:SIM PUK      需要输入 PUK 码。
  * +CPIN:SIM REMOVED  没检测到 SIM 卡。
  *
  * 判断依据：
  * 响应中出现 +CPIN:READY 或 +CPIN: READY，就认为 SIM 卡可用。
  */
uint8_t Air780E_CheckSimReady(void)
{
	// AT+CPIN?：查询SIM卡状态。可用时返回 +CPIN: READY，最后返回 OK。
	// 未插卡、需要PIN/PUK或SIM异常时，会返回对应状态或 ERROR。
	if (!Air780E_SendCmd("AT+CPIN?", "OK", 3000))
	{
		Serial_Printf("SIM query failed\r\n");
		return 0;
	}

	if ((strstr(Uart2_GetRxBuffer(), "+CPIN:READY") != 0) ||
		(strstr(Uart2_GetRxBuffer(), "+CPIN: READY") != 0))
	{
		Serial_Printf("SIM ready\r\n");
		return 1;
	}

	Serial_Printf("SIM not ready\r\n");
	Serial_SendString(Uart2_GetRxBuffer());
	Serial_SendString("\r\n");
	return 0;
}

/**
  * @brief  查询信号质量。
  * @param  rssi: 保存信号强度，0~31 有效，99 表示未知；可传 0。
  * @param  ber : 保存误码率，99 表示未知；可传 0。
  * @retval 1=查询并解析成功，0=失败。
  *
  * AT 指令：
  * AT+CSQ
  *
  * 典型返回：
  * +CSQ: 20,0
  * OK
  *
  * 返回值含义：
  * rssi = 0~31，信号强度，数值越大通常信号越好。
  * rssi = 99，未知或无信号。
  * ber  = 误码率，普通联网测试阶段可以先不重点关注。
  */
uint8_t Air780E_GetSignalQuality(uint8_t *rssi, uint8_t *ber)
{
	const char *p;
	uint32_t rssiValue;
	uint32_t berValue;
	char *rx;

	// AT+CSQ：查询无线信号质量。成功返回 +CSQ:<rssi>,<ber>，最后返回 OK。
	// rssi为0~31时数值越大通常越好，99表示未知或当前没有可用信号。
	if (!Air780E_SendCmd("AT+CSQ", "OK", 2000))
	{
		Serial_Printf("CSQ check failed\r\n");
		return 0;
	}

	rx = Uart2_GetRxBuffer();
	p = strstr(rx, "+CSQ:");
	if (p == 0)
	{
		Serial_Printf("CSQ parse failed\r\n");
		return 0;
	}

	p += strlen("+CSQ:");
	if (!Air780E_ReadUint(&p, &rssiValue))
	{
		Serial_Printf("CSQ rssi parse failed\r\n");
		return 0;
	}

	if (*p != ',')
	{
		Serial_Printf("CSQ comma parse failed\r\n");
		return 0;
	}
	p++;

	if (!Air780E_ReadUint(&p, &berValue))
	{
		Serial_Printf("CSQ ber parse failed\r\n");
		return 0;
	}

	if (rssi != 0)
	{
		*rssi = (uint8_t)rssiValue;
	}

	if (ber != 0)
	{
		*ber = (uint8_t)berValue;
	}

	Serial_Printf("CSQ rssi=%d ber=%d\r\n", rssiValue, berValue);

	if (rssiValue == 99)
	{
		Serial_Printf("Signal unknown\r\n");
		return 0;
	}

	return 1;
}

/**
  * @brief  等待模块注册到运营商网络。
  * @param  timeoutMs: 最大等待时间，单位 ms。
  * @retval 1=已入网，0=超时未入网。
  *
  * 用到的 AT 指令：
  * AT+CREG?   查询普通网络注册状态。
  * AT+CGREG?  查询分组数据网络注册状态。
  * AT+CEREG?  查询 LTE/EPS 网络注册状态。
  *
  * 典型返回：
  * +CREG: 0,1
  * +CGREG: 0,1
  * +CEREG: 0,1
  *
  * stat 含义：
  * 0 未注册。
  * 1 已注册本地网络。
  * 2 正在搜索网络。
  * 3 注册被拒绝。
  * 5 已注册漫游网络。
  */
uint8_t Air780E_WaitNetworkRegistered(uint32_t timeoutMs)
{
	uint32_t elapsed = 0;
	char *rx;

	Serial_Printf("Wait network register...\r\n");

	while (elapsed < timeoutMs)
	{
		// AT+CREG?：查询电路域网络注册状态。返回 +CREG:<n>,<stat> 和 OK。
		// stat=1表示本地注册成功，stat=5表示漫游注册成功。
		if (Air780E_SendCmd("AT+CREG?", "OK", 1000))
		{
			rx = Uart2_GetRxBuffer();
			if (Air780E_HasRegisteredState(rx, "+CREG:"))
			{
				Serial_Printf("Network registered by CREG\r\n");
				return 1;
			}
		}

		// AT+CGREG?：查询分组数据网络注册状态。返回 +CGREG:<n>,<stat> 和 OK。
		if (Air780E_SendCmd("AT+CGREG?", "OK", 1000))
		{
			rx = Uart2_GetRxBuffer();
			if (Air780E_HasRegisteredState(rx, "+CGREG:"))
			{
				Serial_Printf("Network registered by CGREG\r\n");
				return 1;
			}
		}

		// AT+CEREG?：查询LTE/EPS注册状态。返回 +CEREG:<n>,<stat> 和 OK。
		// Air780E是4G模块，因此这项最能直接反映LTE网络是否注册成功。
		if (Air780E_SendCmd("AT+CEREG?", "OK", 1000))
		{
			rx = Uart2_GetRxBuffer();
			if (Air780E_HasRegisteredState(rx, "+CEREG:"))
			{
				Serial_Printf("Network registered by CEREG\r\n");
				return 1;
			}
		}

		Delay_ms(1000);
		elapsed += 4000;
	}

	Serial_Printf("Network register timeout\r\n");
	return 0;
}

/**
  * @brief  激活 PDP 数据网络。
  * @param  apn: APN 字符串；传 0 或空字符串时使用自动 APN。
  * @retval 1=PDP 激活成功，0=失败。
  *
  * PDP 可以先理解成“4G 模块真正打开了移动数据通道”。
  * 没有 PDP，后面的 MQTT/HTTP 指令就无法访问公网。
  *
  * 用到的 AT 指令：
  * AT+CGATT?       查询是否附着到分组数据网络。
  * AT+CGATT=1      如果未附着，则主动附着。
  * AT+CGDCONT?     自动 APN 模式下查询当前 PDP 上下文。
  * AT+CGDCONT=...  专用 APN 模式下设置 APN。
  * AT+CGACT=1,1    激活 cid=1 的 PDP 上下文。
  * AT+CGPADDR=1    查询 cid=1 分配到的 IP 地址。
  *
  * 典型返回：
  * +CGATT:1
  * +CGACT: 1,1
  * +CGPADDR:1,"10.xx.xx.xx"
  */
uint8_t Air780E_ActivatePdp(const char *apn)
{
	// AT+CGATT?：查询是否已经附着分组数据网络。
	// 返回 +CGATT:1 表示已附着，+CGATT:0表示未附着，最后返回 OK。
	if (!Air780E_SendCmd("AT+CGATT?", "OK", 3000))
	{
		Serial_Printf("CGATT query failed\r\n");
		return 0;
	}

	if ((strstr(Uart2_GetRxBuffer(), "+CGATT:1") == 0) &&
		(strstr(Uart2_GetRxBuffer(), "+CGATT: 1") == 0))
	{
		// AT+CGATT=1：主动附着分组数据网络。成功返回 OK，失败返回 ERROR或超时。
		if (!Air780E_SendCmd("AT+CGATT=1", "OK", 10000))
		{
			Serial_Printf("CGATT attach failed\r\n");
			return 0;
		}
	}

	if (!Air780E_SetApn(apn))
	{
		return 0;
	}

	// AT+CGACT=1,1：激活cid=1的PDP上下文，让模块获得可访问公网的数据通道。
	// 成功返回 OK；网络、SIM或APN不正确时可能返回 ERROR。
	if (!Air780E_SendCmd("AT+CGACT=1,1", "OK", 15000))
	{
		Serial_Printf("PDP activate failed\r\n");
		return 0;
	}

	// AT+CGPADDR=1：查询cid=1获得的IP地址。
	// 成功返回 +CGPADDR:1,"<IP地址>"，最后返回 OK。
	Air780E_SendCmd("AT+CGPADDR=1", "OK", 3000);
	Serial_Printf("PDP active\r\n");
	return 1;
}

/**
  * @brief  完整执行“模块可联网”检查。
  * @param  apn      : APN 字符串；传 0 或空字符串时使用自动 APN。
  * @param  timeoutMs: 等待入网的最大时间，建议 60000 ms。
  * @retval 1=模块已经具备 MQTT/HTTP 前置网络条件，0=失败。
  *
  * 总流程：
  * 1. AT             确认 STM32 和模块串口通信正常。
  * 2. ATE0           关闭回显，避免响应里重复出现命令本身。
  * 3. AT+CPIN?       检查 SIM 卡。
  * 4. AT+CSQ         检查信号。
  * 5. CREG/CGREG/CEREG 等待注册运营商网络。
  * 6. CGATT/CGACT    附着并激活 PDP。
  * 7. CGPADDR        查询 IP 地址。
  *
  * 成功后只能说明 4G 模块已经具备联网条件。
  * 下一步还要连接MQTT Broker接收命令，并通过HTTP GET从OSS下载固件。
  */
uint8_t Air780E_NetworkReady(const char *apn, uint32_t timeoutMs)
{
	// AT：联网流程开始前再次确认串口通信正常。成功返回 OK。
	if (!Air780E_SendCmd("AT", "OK", 1000))
	{
		Serial_Printf("Air780E no AT response\r\n");
		return 0;
	}

	// ATE0：关闭命令回显，让后续接收缓存只保留模块响应。成功返回 OK。
	Air780E_SendCmd("ATE0", "OK", 1000);

	if (!Air780E_CheckSimReady())
	{
		return 0;
	}

	if (!Air780E_GetSignalQuality(0, 0))
	{
		return 0;
	}

	if (!Air780E_WaitNetworkRegistered(timeoutMs))
	{
		return 0;
	}

	if (!Air780E_ActivatePdp(apn))
	{
		return 0;
	}

	Serial_Printf("Air780E network ready\r\n");
	return 1;
}

/**
  * @brief  解析 AT+HTTPACTION 的异步结果。
  * @param  resp  : USART2 接收到的完整响应字符串。
  * @param  result: 保存 HTTP 状态码和响应正文长度。
  * @retval 1=找到了并成功解析 +HTTPACTION，0=格式不正确。
  *
  * 典型返回：
  * +HTTPACTION:0,200,285
  *
  * 三个数字依次表示：
  * 0   = GET 方法；
  * 200 = HTTP 请求成功；
  * 285 = 服务器响应正文有285字节。
  */
static uint8_t Air780E_ParseHttpAction(const char *resp, Air780E_HttpResult_t *result)
{
	const char *p;
	uint32_t method;
	uint32_t statusCode;
	uint32_t dataLength;

	p = strstr(resp, "+HTTPACTION:");
	if (p == 0)
	{
		return 0;
	}

	p += strlen("+HTTPACTION:");
	if (!Air780E_ReadUint(&p, &method) || (*p != ','))
	{
		return 0;
	}
	p++;

	if (!Air780E_ReadUint(&p, &statusCode) || (*p != ','))
	{
		return 0;
	}
	p++;

	if (!Air780E_ReadUint(&p, &dataLength))
	{
		return 0;
	}

	if ((method != 0U) || (statusCode < 100U) || (statusCode > 599U))
	{
		return 0;
	}

	result->statusCode = (uint16_t)statusCode;
	result->dataLength = dataLength;
	return 1;
}

/**
  * @brief  为 HTTP 功能准备 SAPBR 承载。
  * @retval 1=承载已经激活，0=失败。
  *
  * AT 指令含义：
  * AT+SAPBR=2,1                  查询 cid=1 的承载状态。
  * AT+SAPBR=3,1,"CONTYPE","GPRS" 设置承载类型为移动数据网络。
  * AT+SAPBR=3,1,"APN",""         使用模块自动获得的公网卡 APN。
  * AT+SAPBR=1,1                  激活 cid=1 的承载。
  *
  * 正常查询返回：+SAPBR:1,1,<IP>，第二个1表示承载已经激活。
  */
static uint8_t Air780E_HttpPrepareBearer(void)
{
	// AT+SAPBR=2,1：查询承载1的状态。
	// +SAPBR:1,1,"<IP>"表示已激活；+SAPBR:1,0表示未激活；最后返回 OK。
	if (Air780E_SendCmd("AT+SAPBR=2,1", "OK", 3000))
	{
		if ((strstr(Uart2_GetRxBuffer(), "+SAPBR:1,1") != 0) ||
			(strstr(Uart2_GetRxBuffer(), "+SAPBR: 1,1") != 0))
		{
			Serial_Printf("HTTP bearer already active\r\n");
			return 1;
		}
	}

	// AT+SAPBR=3,1,"CONTYPE","GPRS"：指定承载1使用移动分组数据网络。
	// 配置成功返回 OK，参数不支持或状态不允许时返回 ERROR。
	if (!Air780E_SendCmd("AT+SAPBR=3,1,\"CONTYPE\",\"GPRS\"", "OK", 3000))
	{
		return 0;
	}

	// AT+SAPBR=3,1,"APN",""：承载1不强制写死APN，使用公网卡自动获得的APN。
	// 配置成功返回 OK；专网卡以后应把空字符串替换为运营商提供的APN。
	if (!Air780E_SendCmd("AT+SAPBR=3,1,\"APN\",\"\"", "OK", 3000))
	{
		return 0;
	}

	// AT+SAPBR=1,1：打开承载1。成功返回 OK，联网失败时返回 ERROR或超时。
	if (!Air780E_SendCmd("AT+SAPBR=1,1", "OK", 30000))
	{
		return 0;
	}

	// AT+SAPBR=2,1：打开后再查询一次，必须看到 +SAPBR:1,1,"<IP>" 和 OK。
	if (!Air780E_SendCmd("AT+SAPBR=2,1", "OK", 3000))
	{
		return 0;
	}

	if ((strstr(Uart2_GetRxBuffer(), "+SAPBR:1,1") == 0) &&
		(strstr(Uart2_GetRxBuffer(), "+SAPBR: 1,1") == 0))
	{
		Serial_Printf("HTTP bearer has no active IP\r\n");
		return 0;
	}

	return 1;
}

/**
  * @brief  把 HTTP/HTTPS URL 设置给模块。
  * @note   URL 可能接近500字节，所以直接分段发送，不在 STM32 RAM 中拼接大字符串。
  */
static uint8_t Air780E_HttpSetUrl(const char *url)
{
	uint32_t urlLength;

	if (url == 0)
	{
		return 0;
	}

	urlLength = strlen(url);
	if ((urlLength == 0U) || (urlLength > AIR780E_HTTP_URL_MAX_LEN))
	{
		Serial_Printf("HTTP URL length error: %d\r\n", urlLength);
		return 0;
	}

	Uart2_ClearRxBuffer();
	Serial_Printf("4G-> set HTTP URL, length=%d\r\n", urlLength);

	// AT+HTTPPARA="URL","<url>"：把随后要访问的HTTP/HTTPS地址交给模块。
	// URL配置成功返回 OK；URL过长、格式错误或HTTP未初始化时可能返回 ERROR。
	Uart2_SendString("AT+HTTPPARA=\"URL\",\"");
	Uart2_SendString(url);
	Uart2_SendString("\"\r\n");

	if (!Air780E_WaitResp("OK", 5000))
	{
		Serial_Printf("Set HTTP URL failed\r\n");
		Serial_SendString(Uart2_GetRxBuffer());
		Serial_SendString("\r\n");
		return 0;
	}

	return 1;
}

/**
  * @brief  准备HTTP承载、初始化HTTP会话并设置URL。
  */
static uint8_t Air780E_HttpOpen(const char *url)
{
	if (url == 0)
	{
		return 0;
	}

	if (!Air780E_HttpPrepareBearer())
	{
		Serial_Printf("HTTP bearer prepare failed\r\n");
		return 0;
	}

	// AT+HTTPINIT：创建模块内部HTTP会话并分配资源。成功返回 OK。
	// 若上次异常复位留下旧会话，可能返回 ERROR，所以失败后先HTTPTERM再重试。
	if (!Air780E_SendCmd("AT+HTTPINIT", "OK", 3000))
	{
		// AT+HTTPTERM：结束旧HTTP会话并释放资源。正常返回 OK；没有会话时可能返回 ERROR。
		Air780E_SendCmd("AT+HTTPTERM", "OK", 3000);
		// 再次执行AT+HTTPINIT，成功返回 OK；再次失败则本次HTTP流程终止。
		if (!Air780E_SendCmd("AT+HTTPINIT", "OK", 3000))
		{
			return 0;
		}
	}

	if (strncmp(url, "https://", 8) == 0)
	{
		// AT+HTTPSSL=1：为当前HTTP会话启用TLS，访问https://地址。成功返回 OK。
		if (!Air780E_SendCmd("AT+HTTPSSL=1", "OK", 3000))
		{
			return 0;
		}
	}
	else
	{
		// AT+HTTPSSL=0：关闭TLS，访问普通http://地址。成功返回 OK。
		if (!Air780E_SendCmd("AT+HTTPSSL=0", "OK", 3000))
		{
			return 0;
		}
	}

	// AT+HTTPPARA="CID",1：指定HTTP会话使用前面准备好的“承载1”访问网络。
	// 它相当于把HTTP功能和SAPBR的1号联网通道绑定起来；配置成功返回 OK。
	if (!Air780E_SendCmd("AT+HTTPPARA=\"CID\",1", "OK", 3000))
	{
		return 0;
	}

	return Air780E_HttpSetUrl(url);
}

/**
  * @brief  启动一次 HTTP GET，并取得状态码和正文长度。
  * @param  url      : 完整 HTTP/HTTPS URL，最长500字节。
  * @param  result   : 保存状态码和正文长度，不能传空指针。
  * @param  timeoutMs: 等待 +HTTPACTION 异步结果的时间，建议120000ms。
  * @retval 1=HTTP状态为200或206，0=AT失败、解析失败或服务器返回其他状态。
  *
  * 本函数暂时不执行 AT+HTTPREAD，因此不会把固件正文读进512字节AT缓存。
  * 下一阶段将根据 dataLength 分块执行 HTTPREAD，并把正文写入 W25Q64。
  */
uint8_t Air780E_HttpGetStart(const char *url,
	Air780E_HttpResult_t *result,
	uint32_t timeoutMs)
{
	if ((url == 0) || (result == 0))
	{
		return 0;
	}

	result->statusCode = 0;
	result->dataLength = 0;

	if (!Air780E_HttpOpen(url))
	{
		return 0;
	}

	// AT+HTTPACTION=0：启动HTTP GET请求，0代表GET方法。
	// 先立即返回 OK；请求完成后再异步上报 +HTTPACTION:0,<状态码>,<正文长度>。
	if (!Air780E_SendCmd("AT+HTTPACTION=0", "OK", 5000))
	{
		return 0;
	}

	// HTTPACTION 先返回 OK，真正的请求结果稍后通过 +HTTPACTION URC 上报。
	if (!Air780E_WaitLine("+HTTPACTION:", timeoutMs))
	{
		Serial_Printf("HTTPACTION result timeout\r\n");
		return 0;
	}

	if (!Air780E_ParseHttpAction(Uart2_GetRxBuffer(), result))
	{
		Serial_Printf("HTTPACTION parse failed\r\n");
		return 0;
	}

	Serial_Printf("HTTP status=%d length=%d\r\n",
		result->statusCode,
		result->dataLength);

	return ((result->statusCode == 200U) || (result->statusCode == 206U));
}

/**
  * @brief  结束HTTP会话并释放模块内部HTTP资源。
  * @retval 1=模块返回OK，0=失败。
  */
uint8_t Air780E_HttpTerminate(void)
{
	// AT+HTTPTERM：结束当前HTTP会话并释放模块资源。成功返回 OK。
	return Air780E_SendCmd("AT+HTTPTERM", "OK", 3000);
}

/**
  * @brief  解析指定AT响应前缀后面的一个整数。
  */
static uint8_t Air780E_ParseValueAfterPrefix(const char *resp,
	const char *prefix,
	uint32_t *value)
{
	const char *p;

	p = strstr(resp, prefix);
	if (p == 0)
	{
		return 0;
	}

	p += strlen(prefix);
	return Air780E_ReadUint(&p, value);
}

/**
  * @brief  增量计算与串口OTA一致的CRC32。
  */
static uint32_t Air780E_Crc32Update(uint32_t crc, const uint8_t *data, uint16_t len)
{
	uint16_t i;
	uint8_t bit;

	for (i = 0; i < len; i++)
	{
		crc ^= data[i];
		for (bit = 0; bit < 8U; bit++)
		{
			if (crc & 1U)
			{
				crc = (crc >> 1) ^ 0xEDB88320UL;
			}
			else
			{
				crc >>= 1;
			}
		}
	}

	return crc;
}

/**
  * @brief  擦除W25Q64中的固件暂存区域。
  */
static uint8_t Air780E_EraseImageArea(uint32_t imageAddr, uint32_t imageSize)
{
	uint32_t addr;
	uint32_t endAddr = imageAddr + imageSize;

	for (addr = imageAddr; addr < endAddr; addr += W25Q64_SECTOR_SIZE)
	{
		if (!W25Q64_SectorErase(addr))
		{
			return 0;
		}
	}
	return 1;
}

/**
  * @brief  把一个下载块按W25Q64页边界拆开写入。
  */
static uint8_t Air780E_WriteImageChunk(uint32_t addr, uint8_t *data, uint16_t len)
{
	uint16_t writeLen;
	uint16_t pageRemain;

	while (len > 0U)
	{
		pageRemain = (uint16_t)(W25Q64_PAGE_SIZE - (addr % W25Q64_PAGE_SIZE));
		writeLen = len;
		if (writeLen > pageRemain)
		{
			writeLen = pageRemain;
		}

		if (!W25Q64_PageProgram(addr, data, writeLen))
		{
			return 0;
		}
		addr += writeLen;
		data += writeLen;
		len -= writeLen;
	}
	return 1;
}

/**
  * @brief  让Air780E把当前HTTP URL完整保存到模块文件系统。
  * @note   文件固定保存为/USER/HTTP/ota.bin，同名文件会被覆盖。
  */
static uint8_t Air780E_HttpDownloadToModuleFs(uint32_t expectedSize)
{
	const char *p;
	uint32_t statusCode;
	uint32_t writtenSize;

	// AT+FSDEL="/USER/HTTP/ota.bin"：删除模块文件系统中的旧固件临时文件。
	// 文件存在并删除成功时返回 OK；文件不存在时可能返回 ERROR，这里允许忽略。
	Air780E_SendCmd("AT+FSDEL=\"/USER/HTTP/ota.bin\"", "OK", 3000);

	Uart2_ClearRxBuffer();
	// AT+HTTPGETTOFS=0,"ota.bin"：对已经设置的URL执行GET，并把完整正文保存到
	// 模块文件系统/USER/HTTP/ota.bin。0表示GET方法，不是文件编号。
	// 指令先返回 OK；下载结束后异步上报 +HTTPGETTOFS:<HTTP状态码>,<写入字节数>。
	Serial_Printf("4G-> AT+HTTPGETTOFS=0,\"ota.bin\"\r\n");
	Uart2_SendString("AT+HTTPGETTOFS=0,\"ota.bin\"\r\n");

	// 下载完成后模块主动上报：+HTTPGETTOFS:<HTTP状态码>,<已写字节数>。
	// 不只等立即返回的OK，因为实际下载时间可能超过普通AT命令超时。
	if (!Air780E_WaitLine("+HTTPGETTOFS:", 120000U))
	{
		Serial_Printf("HTTPGETTOFS timeout\r\n");
		return 0;
	}

	p = strstr(Uart2_GetRxBuffer(), "+HTTPGETTOFS:");
	if (p == 0)
	{
		return 0;
	}
	p += strlen("+HTTPGETTOFS:");

	if (!Air780E_ReadUint(&p, &statusCode) || (*p != ','))
	{
		Serial_Printf("HTTPGETTOFS status parse failed\r\n");
		return 0;
	}
	p++;

	if (!Air780E_ReadUint(&p, &writtenSize))
	{
		Serial_Printf("HTTPGETTOFS size parse failed\r\n");
		return 0;
	}

	Serial_Printf("HTTPGETTOFS status=%d written=%d\r\n", statusCode, writtenSize);
	if (((statusCode != 200U) && (statusCode != 206U)) ||
		(writtenSize != expectedSize))
	{
		return 0;
	}

	return 1;
}

/**
  * @brief  查询模块文件系统中的固件大小。
  */
static uint8_t Air780E_CheckModuleFileSize(uint32_t expectedSize)
{
	uint32_t fileSize;

	// AT+FSFLSIZE="/USER/HTTP/ota.bin"：查询模块中临时固件文件的实际大小。
	// 成功返回 +FSFLSIZE:<字节数>，最后返回 OK；文件不存在时返回 ERROR。
	if (!Air780E_SendCmd("AT+FSFLSIZE=\"/USER/HTTP/ota.bin\"", "OK", 3000))
	{
		return 0;
	}

	if (!Air780E_ParseValueAfterPrefix(Uart2_GetRxBuffer(), "+FSFLSIZE:", &fileSize))
	{
		Serial_Printf("FSFLSIZE parse failed\r\n");
		return 0;
	}

	Serial_Printf("Module file size=%d\r\n", fileSize);
	return fileSize == expectedSize;
}

#if BOOT_4G_CA_PROVISION_ENABLE
/**
  * @brief  通过HTTPS下载并安装EMQX Serverless的服务器CA证书。
  * @param  url          : EMQX官方CA下载地址。
  * @param  expectedSize : 已知的证书文件大小，用于发现下载不完整或网页错误。
  * @retval 1=下载、大小检查及SSL上下文绑定全部成功，0=任一步失败。
  *
  * 本函数是一次性上板准备工具，不参与日常OTA：
  * 1. HTTPGETTOFS把证书保存到Air780E内部Flash；
  * 2. FSFLSIZE再次确认保存后的字节数；
  * 3. SSLCFG cacert让SSL上下文88使用这个CA验证EMQX服务器证书。
  */
uint8_t Air780E_ProvisionCaCertificate(const char *url, uint32_t expectedSize)
{
	const char *p;
	uint32_t statusCode;
	uint32_t writtenSize;
	uint32_t fileSize;

	if ((url == 0) || (expectedSize == 0U))
	{
		return 0;
	}

	// 复用已经通过OSS固件下载验证的HTTP初始化、HTTPS和URL设置流程。
	if (!Air780E_HttpOpen(url))
	{
		return 0;
	}

	// AT+FSDEL：删除旧证书。文件不存在时可能返回ERROR，不影响后续重新下载。
	Air780E_SendCmd("AT+FSDEL=\"/USER/HTTP/emqxsl-ca.crt\"", "OK", 3000U);
	Uart2_ClearRxBuffer();

	// AT+HTTPGETTOFS=0,"emqxsl-ca.crt"：GET当前URL，并把正文保存到
	// 模块内部文件系统/USER/HTTP/emqxsl-ca.crt。完成后异步上报
	// +HTTPGETTOFS:<HTTP状态码>,<实际写入字节数>。
	Serial_Printf("4G-> AT+HTTPGETTOFS=0,\"emqxsl-ca.crt\"\r\n");
	Uart2_SendString("AT+HTTPGETTOFS=0,\"emqxsl-ca.crt\"\r\n");
	if (!Air780E_WaitLine("+HTTPGETTOFS:", 120000U))
	{
		Serial_Printf("CA download timeout\r\n");
		Air780E_HttpTerminate();
		return 0;
	}

	p = strstr(Uart2_GetRxBuffer(), "+HTTPGETTOFS:");
	if (p == 0)
	{
		Air780E_HttpTerminate();
		return 0;
	}
	p += strlen("+HTTPGETTOFS:");
	if (!Air780E_ReadUint(&p, &statusCode) || (*p != ','))
	{
		Air780E_HttpTerminate();
		return 0;
	}
	p++;
	if (!Air780E_ReadUint(&p, &writtenSize))
	{
		Air780E_HttpTerminate();
		return 0;
	}

	Serial_Printf("CA HTTP status=%d written=%d\r\n", statusCode, writtenSize);
	if (((statusCode != 200U) && (statusCode != 206U)) ||
		(writtenSize != expectedSize))
	{
		Air780E_HttpTerminate();
		return 0;
	}

	// AT+FSFLSIZE：查询模块Flash中证书文件的实际大小；成功返回+FSFLSIZE:<字节数>和OK。
	if (!Air780E_SendCmd("AT+FSFLSIZE=\"/USER/HTTP/emqxsl-ca.crt\"", "OK", 3000U) ||
		!Air780E_ParseValueAfterPrefix(Uart2_GetRxBuffer(), "+FSFLSIZE:", &fileSize) ||
		(fileSize != expectedSize))
	{
		Serial_Printf("CA file size check failed\r\n");
		Air780E_HttpTerminate();
		return 0;
	}
	Serial_Printf("CA module file size=%d\r\n", fileSize);

	Air780E_HttpTerminate();

	// AT+SSLCFG="cacert",88,"文件路径"：让SSL上下文88信任该CA。
	// 返回OK说明当前定制AT固件接受这个路径；之后仍需实际TLS握手验证证书链。
	if (!Air780E_SendCmd(
		"AT+SSLCFG=\"cacert\",88,\"/USER/HTTP/emqxsl-ca.crt\"",
		"OK",
		3000U))
	{
		Serial_Printf("CA path rejected by SSL config\r\n");
		return 0;
	}

	return 1;
}
#endif

/**
  * @brief  从模块文件系统按二进制长度读取一个固件块。
  * @param  position : 文件偏移。
  * @param  requestLen: 本次期望读取长度，最大200字节。
  * @param  actualLen: 返回模块实际给出的正文长度。
  *
  * 当前M100M AT固件返回格式：+FSREAD: + 二进制正文 + \r\nOK\r\n。
  * FSREADHEAD=1只增加固定的"+FSREAD:"前缀，不会再输出十进制长度和换行。
  * 正文可能含0x00、0xFF、换行和字符"OK"，所以找到前缀后必须依据requestLen收取。
  */
static uint8_t Air780E_ReadModuleFileChunk(uint32_t position,
	uint16_t requestLen,
	uint16_t *actualLen)
{
	char cmd[96];
	char *rx;
	const char *p;
	const char *dataStart;
	uint32_t totalLen;
	uint16_t headerLen;

	if ((actualLen == 0) || (requestLen == 0U) ||
		(requestLen > AIR780E_OTA_READ_CHUNK))
	{
		return 0;
	}

	// sprintf()按格式把数字填入字符串，生成例如：
	// AT+FSREAD="/USER/HTTP/ota.bin",1,200,400
	// 含义：以普通读取模式1，从文件偏移400处读取200字节。
	sprintf(cmd,
		"AT+FSREAD=\"/USER/HTTP/ota.bin\",1,%u,%lu",
		(unsigned int)requestLen,
		(unsigned long)position);

	Uart2_ClearRxBuffer();
	Serial_Printf("4G-> FSREAD offset=%d len=%d\r\n", position, requestLen);
	// 当前模块的AT+FSREAD成功时返回固定前缀+FSREAD:，随后立即输出二进制正文，
	// 正文之后才是\r\nOK\r\n。前缀后没有十进制长度字段，也没有单独的\r\n。
	// 读取失败、偏移越界或文件不存在时返回 ERROR。
	Uart2_SendString(cmd);
	Uart2_SendString("\r\n");

	// 这里只等待固定前缀出现，不能调用WaitLine：前缀后面紧接二进制，不存在文本行尾。
	if (!Air780E_WaitResp("+FSREAD:", 5000U))
	{
		Serial_Printf("FSREAD prefix/error, raw response:\r\n");
		Serial_SendString(Uart2_GetRxBuffer());
		Serial_SendString("\r\n");
		return 0;
	}

	rx = Uart2_GetRxBuffer();
	p = strstr(rx, "+FSREAD:");
	if (p == 0)
	{
		return 0;
	}
	dataStart = p + strlen("+FSREAD:");
	headerLen = (uint16_t)(dataStart - rx);

	// 这块M100M的实物响应在固定前缀后还带一个ASCII空格：
	// +FSREAD:<space><binary data>\r\nOK\r\n
	// WaitResp可能刚看到前缀就返回，因此先确保前缀后的第1字节已经到达，再判断空格。
	if (!Uart2_WaitRxLength((uint16_t)(headerLen + 1U), 1000U))
	{
		Serial_Printf("FSREAD first data byte timeout\r\n");
		return 0;
	}
	if (*dataStart == ' ')
	{
		dataStart++;
		headerLen++;
	}
	// 完整响应由“前缀 + requestLen字节正文 + \r\nOK\r\n”组成。
	// headerLen还包含模块可能在+FSREAD:前输出的起始回车换行。
	totalLen = (uint32_t)headerLen + requestLen + AIR780E_FSREAD_TRAILER_LEN;
	if (totalLen > (UART2_RX_BUFFER_SIZE - 1U))
	{
		return 0;
	}

	// 按总字节数等待，不使用strlen/strstr判断二进制正文是否接收完成。
	if (!Uart2_WaitRxLength((uint16_t)totalLen, 5000U))
	{
		Serial_Printf("FSREAD data timeout\r\n");
		return 0;
	}

	// 正文可以包含任意字节，所以只在“正文结束后的固定位置”检查AT成功尾部。
	if (memcmp(dataStart + requestLen,
		AIR780E_FSREAD_TRAILER,
		AIR780E_FSREAD_TRAILER_LEN) != 0)
	{
		Serial_Printf("FSREAD trailer error\r\n");
		return 0;
	}

	// 二进制正文从USART2缓存的headerLen位置开始，复制到Air780E_OtaBuffer。
	// 函数返回后，本次读出的固件块就在Air780E_OtaBuffer中，等待写入W25Q64。
	if (!Uart2_CopyRxData(headerLen, Air780E_OtaBuffer, requestLen))
	{
		return 0;
	}

	*actualLen = requestLen;
	return 1;
}

/**
  * @brief  从OSS URL下载固件，经Air780E文件系统中转后写入W25Q64。
  * @retval 1=下载、长度、CRC32和控制块写入全部成功；0=任一步失败。
  */
uint8_t Air780E_DownloadFirmwareToW25Q64(const char *url,
	uint32_t version,
	uint32_t expectedSize,
	uint32_t expectedCrc32,
	const uint8_t expectedAuthTag[OTA_AUTH_TAG_SIZE],
	uint32_t imageAddr)
{
	uint32_t offset = 0;
	uint32_t crc = 0xFFFFFFFFUL;
	uint32_t storedCrc = 0xFFFFFFFFUL;
	uint32_t verifyOffset;
	uint16_t requestLen;
	uint16_t actualLen;
	uint16_t verifyLen;
	uint32_t previousVersion = 0U;
	uint32_t previousSize = 0U;
	uint32_t previousCrc32 = 0U;
	uint8_t previousAuthTag[OTA_AUTH_TAG_SIZE];

	memset(previousAuthTag, 0, sizeof(previousAuthTag));

	if ((url == 0) || (expectedAuthTag == 0) || (expectedSize == 0U) ||
		(expectedSize > FLASH_APP_SIZE) ||
		(imageAddr >= W25Q64_TOTAL_SIZE) ||
		(expectedSize > (W25Q64_TOTAL_SIZE - imageAddr)) ||
		((imageAddr % W25Q64_SECTOR_SIZE) != 0U))
	{
		Serial_Printf("4G OTA parameter error\r\n");
		return 0;
	}

	if (!Air780E_HttpOpen(url))
	{
		return 0;
	}

	if (!Air780E_HttpDownloadToModuleFs(expectedSize) ||
		!Air780E_CheckModuleFileSize(expectedSize))
	{
		Air780E_HttpTerminate();
		return 0;
	}

	// AT+FSREADHEAD=1：要求FSREAD在二进制正文前输出 +FSREAD:<长度> 头部。
	// 成功返回 OK；有了长度头，STM32才能安全区分AT文本头和任意二进制正文。
	if (!Air780E_SendCmd("AT+FSREADHEAD=1", "OK", 3000))
	{
		Air780E_HttpTerminate();
		return 0;
	}

	if (!Air780E_EraseImageArea(imageAddr, expectedSize))
	{
		Serial_Printf("W25Q64 erase timeout/error\r\n");
		Air780E_HttpTerminate();
		return 0;
	}
	while (offset < expectedSize)
	{
		requestLen = (uint16_t)(expectedSize - offset);
		if (requestLen > AIR780E_OTA_READ_CHUNK)
		{
			requestLen = AIR780E_OTA_READ_CHUNK;
		}

		if (!Air780E_ReadModuleFileChunk(offset, requestLen, &actualLen) ||
			(actualLen != requestLen))
		{
			Serial_Printf("4G OTA read failed at offset=%d\r\n", offset);
			Air780E_HttpTerminate();
			return 0;
		}

		if (!Air780E_WriteImageChunk(imageAddr + offset, Air780E_OtaBuffer, actualLen))
		{
			Serial_Printf("W25Q64 program timeout/error at offset=%d\r\n", offset);
			Air780E_HttpTerminate();
			return 0;
		}
		crc = Air780E_Crc32Update(crc, Air780E_OtaBuffer, actualLen);
		offset += actualLen;
		Serial_Printf("4G OTA progress=%d/%d\r\n", offset, expectedSize);
	}

	crc ^= 0xFFFFFFFFUL;
	if (crc != expectedCrc32)
	{
		Serial_Printf("4G OTA CRC error: calc=0x%08X expected=0x%08X\r\n",
			crc,
			expectedCrc32);
		Air780E_HttpTerminate();
		return 0;
	}

	// 再从W25Q64读回计算一次CRC，确认外部Flash实际保存的数据也正确。
	for (verifyOffset = 0; verifyOffset < expectedSize; verifyOffset += verifyLen)
	{
		verifyLen = (uint16_t)(expectedSize - verifyOffset);
		if (verifyLen > AIR780E_OTA_READ_CHUNK)
		{
			verifyLen = AIR780E_OTA_READ_CHUNK;
		}

		W25Q64_ReadData(imageAddr + verifyOffset, Air780E_OtaBuffer, verifyLen);
		storedCrc = Air780E_Crc32Update(storedCrc, Air780E_OtaBuffer, verifyLen);
	}
	storedCrc ^= 0xFFFFFFFFUL;
	if (storedCrc != expectedCrc32)
	{
		Serial_Printf("W25Q64 CRC error: calc=0x%08X expected=0x%08X\r\n",
			storedCrc,
			expectedCrc32);
		Air780E_HttpTerminate();
		return 0;
	}

	/*
	 * APP不能持有固件认证密钥，否则公开app.bin会泄露密钥。这里仅保存云端给出的
	 * 标签；复位后由只存在于Bootloader的OtaAuth再次读取W25Q64并做最终HMAC验证。
	 */

	// 当前控制块仍描述正在运行且已经确认的旧APP，先保存它的元数据供Boot备份和回滚。
	if ((OTA_CB_Info.OTA_flag == OTA_STATE_DONE) &&
		(OTA_CB_Info.app_version > 0U) &&
		(OTA_CB_Info.image_size > 0U) &&
		(OTA_CB_Info.image_size <= FLASH_APP_SIZE))
	{
		previousVersion = OTA_CB_Info.app_version;
		previousSize = OTA_CB_Info.image_size;
		previousCrc32 = OTA_CB_Info.image_crc32;
		memcpy(previousAuthTag, OTA_CB_Info.image_auth_tag, OTA_AUTH_TAG_SIZE);
	}

	memset(&OTA_CB_Info, 0, OTA_CB_T_SIZE);
	OTA_CB_Info.magic = OTA_CB_MAGIC;
	OTA_CB_Info.format_version = OTA_CB_FORMAT_VERSION;
	OTA_CB_Info.OTA_flag = OTA_STATE_PENDING;
	OTA_CB_Info.app_version = version;
	OTA_CB_Info.image_size = expectedSize;
	OTA_CB_Info.image_crc32 = expectedCrc32;
	OTA_CB_Info.image_addr = imageAddr;
	memcpy(OTA_CB_Info.image_auth_tag, expectedAuthTag, OTA_AUTH_TAG_SIZE);
	OTA_CB_Info.backup_version = previousVersion;
	OTA_CB_Info.backup_size = previousSize;
	OTA_CB_Info.backup_crc32 = previousCrc32;
	OTA_CB_Info.backup_addr = OTA_BACKUP_STORE_ADDR;
	memcpy(OTA_CB_Info.backup_auth_tag, previousAuthTag, OTA_AUTH_TAG_SIZE);
	OTA_CB_Info.error_code = OTA_ERR_NONE;

	if (!AT24C02_WriteOTA_CB_Info(&OTA_CB_Info))
	{
		Serial_Printf("4G OTA control block write failed\r\n");
		Air780E_HttpTerminate();
		return 0;
	}

	// AT+FSDEL：W25Q64写入及两次CRC校验成功后，删除模块中的临时ota.bin。
	// 删除成功返回 OK；即使删除失败也不影响W25Q64里已经验证通过的固件。
	Air780E_SendCmd("AT+FSDEL=\"/USER/HTTP/ota.bin\"", "OK", 3000);
	Air780E_HttpTerminate();
	Serial_Printf("4G OTA image ready in W25Q64\r\n");
	return 1;
}
