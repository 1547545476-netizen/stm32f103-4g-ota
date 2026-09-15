#include "Air780E_Mqtt.h"  // 本文件对外公开的MQTT接口和长度限制。
#include "Air780E.h"       // 通用Air780E命令发送、响应等待函数。
#include "Uart2.h"         // STM32与Air780E之间的USART2收发缓冲区。
#include "Serial.h"        // USART1调试日志，连接电脑查看运行过程。
#include "Delay.h"         // 等待异步响应时使用的毫秒延时。
#include <stdio.h>          // sprintf()：把地址、端口等参数格式化成AT命令。
#include <string.h>         // strlen()/strstr()/strchr()/memcmp()。

/**
  * @brief  判断字符串能否安全放进AT命令的一对双引号中。
  *
  * 例如Topic最终会组成 AT+MSUB="topic",0。如果topic本身含有双引号、回车
  * 或换行，命令就会提前结束或被拆成两条，因此必须在拼接命令前拒绝它。
  * @param  text   待检查的C字符串。
  * @param  maxLen 本工程允许的最大长度。
  * @retval 1=合法，0=空指针、空字符串、过长或含危险字符。
  */
static uint8_t Air780E_MqttIsQuotedArgValid(const char *text, uint16_t maxLen)
{
	uint16_t len;

	// 指针为0表示调用者没有提供字符串，不能对它调用strlen()。
	if (text == 0)
	{
		return 0;
	}
	// strlen()只用于普通文本参数；这些配置字符串不允许包含二进制0字节。
	len = (uint16_t)strlen(text);
	if ((len == 0U) || (len > maxLen) ||
		(strchr(text, '"') != 0) ||
		(strchr(text, '\r') != 0) ||
		(strchr(text, '\n') != 0))
	{
		return 0;
	}
	return 1;
}

/**
  * @brief  等待MQTT相关异步结果，同时识别常见失败URC。
  *
  * AT命令经常分两阶段返回：
  * 1. 先返回OK，只表示模块已经接受命令；
  * 2. 稍后再主动上报CONNECTOK、CONNACKOK、SUBACK等最终结果。
  * 第二阶段这种“不经STM32询问、模块主动发来”的文本称为URC。
  * @param  success   能代表操作成功的关键字。
  * @param  failure   该操作特有的失败关键字；传0表示没有特定关键字。
  * @param  timeoutMs 最长等待时间，循环每1ms检查一次。
  * @retval 1=发现success，0=失败、接收溢出或超时。
  */
static uint8_t Air780E_MqttWaitUrc(const char *success,
	const char *failure,
	uint32_t timeoutMs)
{
	uint32_t elapsed;
	char *rx;

	for (elapsed = 0; elapsed < timeoutMs; elapsed++)
	{
		// USART2中断会在后台不断向缓冲区追加字符，这里只负责反复查看。
		rx = Uart2_GetRxBuffer();
		// 先检查失败关键字，因为"CONNECTFAIL"中也包含"CONNECT"。
		// ERROR是通用失败；failure则是FAIL等当前操作专用失败提示。
		if (((failure != 0) && (strstr(rx, failure) != 0)) ||
			(strstr(rx, "ERROR") != 0) || Uart2_IsRxOverflow())
		{
			return 0;
		}
		if (strstr(rx, success) != 0)
		{
			return 1;
		}
		Delay_ms(1);
	}
	return 0;
}

/**
  * @brief  发送包含阿里云密码的MCONFIG，不把密钥打印到USART1日志。
  *
  * 最终发送格式：
  * AT+MCONFIG="clientId","username","password"\r\n
  * 这里分段发送而不使用sprintf，是为了不再申请一个可能超过700字节的大数组，
  * 同时也避免把完整设备密钥放进USART1调试输出。
  */
static uint8_t Air780E_MqttSetCredentials(const Air780E_MqttConfig_t *config)
{
	// 清掉上一条命令的响应，保证本次查找的OK确实属于MCONFIG。
	Uart2_ClearRxBuffer();
	Serial_Printf("4G-> AT+MCONFIG=<credentials hidden>\r\n");

	// AT+MCONFIG：设置MQTT ClientId、用户名和密码。
	// 成功返回OK；参数格式错误、鉴权参数过长时返回ERROR。
	// 三个参数按字符串格式加双引号，避免纯数字ClientId被定制AT解析器当作整数处理。
	// 调用前已检查内容不含双引号、逗号、回车和换行，不会提前结束AT参数。
	Uart2_SendString("AT+MCONFIG=\"");
	Uart2_SendString(config->clientId);
	Uart2_SendString("\",\"");
	Uart2_SendString(config->username);
	Uart2_SendString("\",\"");
	Uart2_SendString(config->password);
	Uart2_SendString("\"\r\n");

	// 此处只等待命令配置完成；真正的云端认证发生在后面的MCONNECT。
	if (!Air780E_WaitResp("OK", 5000U))
	{
		Serial_Printf("MQTT MCONFIG failed\r\n");
		return 0;
	}
	return 1;
}

/**
  * @brief  连接阿里云MQTT服务器并完成MQTT认证。
  *
  * 完整层次是：4G入网 -> TCP/TLS连接 -> MQTT认证 -> 设置消息接收方式。
  * 本函数只负责后三步，调用前应先用Air780E_NetworkReady()确认4G已入网。
  * @param  config 连接地址以及由阿里云设备三元组生成的鉴权参数。
  * @retval 1=MQTT可正常收发，0=配置或连接失败。
  */
uint8_t Air780E_MqttConnect(const Air780E_MqttConfig_t *config)
{
	// 用来临时生成MIPSTART/SSLMIPSTART命令，命令发完即可重复使用。
	char cmd[192];

	// 在调用strlen()/sprintf()前统一检查指针、长度和AT命令特殊字符。
	if ((config == 0) || (config->port == 0U) ||
		!Air780E_MqttIsQuotedArgValid(config->host, AIR780E_MQTT_HOST_MAX_LEN) ||
		!Air780E_MqttIsQuotedArgValid(config->clientId, 256U) ||
		!Air780E_MqttIsQuotedArgValid(config->username, 256U) ||
		!Air780E_MqttIsQuotedArgValid(config->password, 256U) ||
		(strchr(config->clientId, ',') != 0) ||
		(strchr(config->username, ',') != 0) ||
		(strchr(config->password, ',') != 0))
	{
		Serial_Printf("MQTT config error\r\n");
		return 0;
	}

	// 第1步：清理模块里可能残留的旧MQTT/TCP会话。
	// 例如上一次程序被复位时没有来得及断开，模块中可能仍保存旧连接。
	// 没有旧会话时返回ERROR是正常情况，所以这里故意不判断返回值。
	Air780E_SendCmd("AT+MDISCONNECT", "OK", 3000U);
	Air780E_SendCmd("AT+MIPCLOSE", "OK", 3000U);

	// 第2步：把ClientId、用户名和密码写入模块。
	if (!Air780E_MqttSetCredentials(config))
	{
		return 0;
	}

	// 第3步：建立到MQTT接入域名的底层网络连接。
	if (config->useTls)
	{
		// MQTT使用固定SSL上下文88，后面的SSLMIPSTART会引用同一个编号。
		// AT+SSLCFG="cacert"把一次性安装模式保存的EMQX CA绑定到上下文88。
		// 成功返回OK；文件不存在、路径错误或证书格式不支持时返回ERROR。
		if (!Air780E_SendCmd(
			"AT+SSLCFG=\"cacert\",88,\"/USER/HTTP/emqxsl-ca.crt\"",
			"OK",
			3000U))
		{
			return 0;
		}

		// seclevel=1表示进行单向TLS认证：模块使用上面的CA校验MQTT服务器证书。
		if (!Air780E_SendCmd("AT+SSLCFG=\"seclevel\",88,1", "OK", 3000U))
		{
			return 0;
		}

		// EMQX Serverless是多租户服务，TLS握手必须携带服务器域名（SNI）。
		// AT+SSLCFG="hostname",88,"<host>"把MQTT服务器域名写入SSL上下文88。
		// 成功返回OK；如果没有设置，EMQX会在TLS阶段拒绝连接。
		sprintf(cmd, "AT+SSLCFG=\"hostname\",88,\"%s\"", config->host);
		if (!Air780E_SendCmd(cmd, "OK", 3000U))
		{
			return 0;
		}

		// AT+SSLMIPSTART="域名",端口：建立TLS承载的TCP连接。
		// sprintf把host和port填进%s、%u占位符，结果保存在cmd中。
		// 模块先返回OK，成功后异步上报CONNECTOK，失败上报CONNECTFAIL。
		sprintf(cmd, "AT+SSLMIPSTART=\"%s\",%u",
			config->host,
			(unsigned int)config->port);
	}
	else
	{
		// AT+MIPSTART="域名",端口：建立明文TCP连接。
		// 仅适合临时调试，不建议在明文链路中携带真实设备密钥。
		sprintf(cmd, "AT+MIPSTART=\"%s\",%u",
			config->host,
			(unsigned int)config->port);
	}

	// 必须同时满足“命令被接受”和“网络连接最终成功”两个条件。
	if (!Air780E_SendCmd(cmd, "OK", 5000U) ||
		// Air780EPM V2007上报完整的"CONNECT OK"后，才能发送MCONNECT。
		// 不能只等"CONNECT"前缀，否则可能在URC还没有收完时抢跑。
		!Air780E_MqttWaitUrc("CONNECT OK", "FAIL", 30000U))
	{
		Serial_Printf("MQTT TCP failed\r\n");
		Serial_SendString(Uart2_GetRxBuffer());
		Serial_SendString("\r\n");
		return 0;
	}

	// 第4步：AT+MCONNECT=1,60建立MQTT协议会话。
	// 参数1表示clean session：断开后服务器不保留旧订阅；60是心跳保活秒数。
	// 先返回OK；阿里云认证成功后异步返回CONNACKOK，认证失败返回ERROR。
	if (!Air780E_SendCmd("AT+MCONNECT=1,60", "OK", 5000U) ||
		// 等待完整的"CONNACK OK"，表示MQTT服务器已接受账号和ClientId。
		!Air780E_MqttWaitUrc("CONNACK OK", "ERROR", 15000U))
	{
		Serial_Printf("MQTT auth failed\r\n");
		// 保留MCONNECT后的原始URC，用于区分账号拒绝、ClientId冲突和AT固件格式差异。
		Serial_SendString(Uart2_GetRxBuffer());
		Serial_SendString("\r\n");
		return 0;
	}

	// 第5步：AT+MQTTMODE=0选择ASCII原文发布模式。成功返回OK。
	if (!Air780E_SendCmd("AT+MQTTMODE=0", "OK", 3000U))
	{
		return 0;
	}

	// 第6步：AT+MQTTMSGSET=0让订阅消息直接通过+MSUB URC送到USART2。
	// 本工程把单条OTA命令限制为700字节，因此不使用模块的4槽消息缓存模式。
	if (!Air780E_SendCmd("AT+MQTTMSGSET=0", "OK", 3000U))
	{
		return 0;
	}

	Serial_Printf("MQTT connected\r\n");
	return 1;
}

uint8_t Air780E_MqttSubscribe(const char *topic, uint8_t qos)
{
	// cmd只保存一条订阅命令，大小包含Topic、引号、QoS数字和字符串结束符。
	char cmd[224];

	// MQTT QoS合法范围为0~2；Topic还必须能安全放进AT命令双引号。
	if (!Air780E_MqttIsQuotedArgValid(topic, AIR780E_MQTT_TOPIC_MAX_LEN) || (qos > 2U))
	{
		return 0;
	}

	// AT+MSUB="<topic>",<qos>：订阅云端下发Topic。
	// 例如：AT+MSUB="/pk/device/user/ota/cmd",0。
	// 先返回OK表示命令格式被接受，订阅成功后再异步返回SUBACK。
	sprintf(cmd, "AT+MSUB=\"%s\",%u", topic, (unsigned int)qos);
	if (!Air780E_SendCmd(cmd, "OK", 5000U) ||
		!Air780E_MqttWaitUrc("SUBACK", "ERROR", 10000U))
	{
		return 0;
	}
	return 1;
}

uint8_t Air780E_MqttUnsubscribe(const char *topic)
{
	char cmd[224];

	if (!Air780E_MqttIsQuotedArgValid(topic, AIR780E_MQTT_TOPIC_MAX_LEN))
	{
		return 0;
	}

	// AT+MUNSUB="<topic>"：取消订阅。
	// 本项目收到一条合法OTA命令后就取消订阅，避免下载过程中又收到+MSUB，
	// 否则MQTT的异步文本可能和HTTP/FSREAD响应同时进入同一个USART2缓冲区。
	// 先返回OK表示命令被接受，成功后异步返回UNSUBACK。
	sprintf(cmd, "AT+MUNSUB=\"%s\"", topic);
	if (!Air780E_SendCmd(cmd, "OK", 5000U) ||
		!Air780E_MqttWaitUrc("UNSUBACK", "ERROR", 10000U))
	{
		return 0;
	}
	return 1;
}

uint8_t Air780E_MqttPublish(const char *topic,
	const char *message,
	uint8_t qos,
	uint8_t retain)
{
	char cmd[224];             // 保存不包含消息正文的MPUBEX命令头。
	uint16_t messageLen;       // 将要发送的正文准确字节数。

	if (!Air780E_MqttIsQuotedArgValid(topic, AIR780E_MQTT_TOPIC_MAX_LEN) ||
		(message == 0) || (qos > 2U) || (retain > 1U))
	{
		return 0;
	}
	// 状态上报是普通JSON文本，所以可以用strlen()取得长度。
	// 固件二进制不能这样做，因为固件中可能包含0x00。
	messageLen = (uint16_t)strlen(message);
	if ((messageLen == 0U) || (messageLen > AIR780E_MQTT_MESSAGE_MAX_LEN))
	{
		return 0;
	}

	// AT+MPUBEX使用定长数据模式发布消息：命令中先声明正文长度，随后再发正文。
	// 因此JSON里的双引号不属于AT命令参数，不需要逐个转换成\22。
	// 参数依次是Topic、QoS、retain和正文长度。retain=0表示服务器不保留该消息。
	sprintf(cmd, "AT+MPUBEX=\"%s\",%u,%u,%u",
		topic,
		(unsigned int)qos,
		(unsigned int)retain,
		(unsigned int)messageLen);

	// 第1步：清接收区、发送命令头，等待模块返回字符'>'。
	// '>'类似“我准备好了，请发送messageLen个正文数据”。
	Uart2_ClearRxBuffer();
	Serial_Printf("4G-> MQTT publish topic=%s len=%d\r\n", topic, messageLen);
	Uart2_SendString(cmd);
	Uart2_SendString("\r\n");
	if (!Air780E_WaitResp(">", 5000U))
	{
		Serial_Printf("MQTT publish prompt timeout\r\n");
		return 0;
	}

	// 第2步：清掉'>'，发送准确的messageLen字节正文，不额外发送\r\n。
	// 模块按长度收满后才会把消息发布到MQTT服务器。
	Uart2_ClearRxBuffer();
	Uart2_SendData((const uint8_t *)message, messageLen);
	if (!Air780E_WaitResp("OK", 10000U))
	{
		Serial_Printf("MQTT publish failed\r\n");
		return 0;
	}
	// QoS0只需模块返回OK；QoS1还要等服务器PUBACK；QoS2要等PUBCOMP。
	if ((qos == 1U) && !Air780E_MqttWaitUrc("PUBACK", "ERROR", 10000U))
	{
		return 0;
	}
	if ((qos == 2U) && !Air780E_MqttWaitUrc("PUBCOMP", "ERROR", 10000U))
	{
		return 0;
	}
	return 1;
}

/**
  * @brief  等待直接上报格式+MSUB:<topic>,<len>,<message>并提取正文。
  *
  * 假设模块收到消息后，USART2缓冲区内容如下：
  * +MSUB:/pk/dev/user/ota/cmd,86,{"cmd":"ota",...}\r\n
  *                           ^  ^
  *                           |  payloadStart，JSON正文从这里开始
  *                           lengthEnd，长度字段后的逗号
  *
  * 解析时不能对正文使用strlen()，因为解析开始时串口可能只收到半条消息。
  * 本函数先读出模块声明的payloadLen，再等待接收总长度达到要求，最后按长度复制。
  * @param  expectedTopic 只接受这个Topic，避免把其他业务消息当成OTA命令。
  * @param  message       调用者提供的正文输出缓冲区。
  * @param  messageSize   输出缓冲区总容量，包含最后补上的'\0'。
  * @param  timeoutMs     最长等待时间。
  * @retval 1=完整收到并复制一条消息，0=格式错误、溢出或超时。
  */
uint8_t Air780E_MqttWaitMessage(const char *expectedTopic,
	char *message,
	uint16_t messageSize,
	uint32_t timeoutMs)
{
	uint32_t elapsed;          // 已等待的毫秒数。
	uint32_t payloadLen;       // +MSUB中声明的消息正文长度。
	uint32_t totalLen;         // 从缓冲区开头到消息末尾至少应收到的总字节数。
	uint16_t topicLen;         // 实际收到的Topic长度。
	uint16_t payloadOffset;    // 正文首字节相对Uart2_RxBuffer[0]的偏移。
	char *rx;                  // USART2接收缓冲区首地址。
	char *p;                   // 通用扫描指针，解析过程中从左向右移动。
	char *topicStart;          // Topic第一个有效字符。
	char *topicEnd;            // Topic结束位置，指向双引号或逗号。
	char *lengthEnd;           // 长度字段结束位置，指向长度后的逗号。
	char *payloadStart;        // 消息正文第一个字节。

	if (!Air780E_MqttIsQuotedArgValid(expectedTopic, AIR780E_MQTT_TOPIC_MAX_LEN) ||
		(message == 0) || (messageSize < 2U))
	{
		return 0;
	}

	// 这里故意不调用Uart2_ClearRxBuffer()：云端可能在订阅成功或online上报后
	// 立刻下发命令。进入本函数前+MSUB可能已经到达，清空会把真实命令丢掉。
	for (elapsed = 0; elapsed < timeoutMs; elapsed++)
	{
		if (Uart2_IsRxOverflow())
		{
			Serial_Printf("MQTT message overflow\r\n");
			return 0;
		}

		// GetRxBuffer()只返回缓冲区地址，不清空数据，也不会改变RxLength。
		rx = Uart2_GetRxBuffer();
		// 先寻找一条订阅消息的固定开头。找不到通常表示消息还没到齐。
		p = strstr(rx, "+MSUB:");
		if (p == 0)
		{
			Delay_ms(1);
			continue;
		}
		// 跳过固定前缀，此后p应指向Topic开头或Topic前的空格。
		p += strlen("+MSUB:");
		while (*p == ' ')
		{
			p++;
		}

		// 不同AT固件可能把Topic上报为"topic"或topic，两种格式都兼容。
		if (*p == '"')
		{
			topicStart = p + 1;
			topicEnd = strchr(topicStart, '"');
			if (topicEnd == 0)
			{
				Delay_ms(1);
				continue;
			}
			p = topicEnd + 1;
		}
		else
		{
			topicStart = p;
			topicEnd = strchr(topicStart, ',');
			if (topicEnd == 0)
			{
				Delay_ms(1);
				continue;
			}
			p = topicEnd;
		}

		// Topic后必须紧跟逗号，否则这条+MSUB格式不符合手册定义。
		if (*p != ',')
		{
			return 0;
		}
		p++;
		// p现在指向长度第一个字符；下一个逗号就是长度字段终点。
		lengthEnd = strchr(p, ',');
		if (lengthEnd == 0)
		{
			Delay_ms(1);
			continue;
		}

		// 手工把十进制文本（例如"86"）转换为整数86。
		payloadLen = 0;
		if ((*p < '0') || (*p > '9'))
		{
			return 0;
		}
		while ((p < lengthEnd) && (*p >= '0') && (*p <= '9'))
		{
			// 原数乘10再加当前个位，例如8 -> 8*10+6 -> 86。
			payloadLen = payloadLen * 10UL + (uint32_t)(*p - '0');
			p++;
		}
		// 不同AT固件可能返回“123”、“123byte”或“123 byte”。
		// 先跳过数字后的空格，再识别可选的byte，最后还允许byte后有空格。
		while ((p < lengthEnd) && ((*p == ' ') || (*p == '\t')))
		{
			p++;
		}
		if (((lengthEnd - p) >= 4) && (strncmp(p, "byte", 4) == 0))
		{
			p += 4;
		}
		while ((p < lengthEnd) && ((*p == ' ') || (*p == '\t')))
		{
			p++;
		}
		// p必须刚好走到逗号；还要给message末尾的'\0'保留1字节。
		if ((p != lengthEnd) || (payloadLen == 0U) ||
			(payloadLen > AIR780E_MQTT_MESSAGE_MAX_LEN) ||
			(payloadLen >= messageSize))
		{
			return 0;
		}

		// 两个指针相减得到Topic字符数，再比较长度和每一个字符。
		topicLen = (uint16_t)(topicEnd - topicStart);
		if ((strlen(expectedTopic) != topicLen) ||
			(memcmp(topicStart, expectedTopic, topicLen) != 0))
		{
			Serial_Printf("MQTT unexpected topic\r\n");
			return 0;
		}

		// lengthEnd指向正文前的逗号，+1就是正文首字节。
		payloadStart = lengthEnd + 1;
		// 指针相减得到正文首字节在整个USART2缓冲区中的数组下标。
		payloadOffset = (uint16_t)(payloadStart - rx);
		// 完整消息至少包含：前缀和字段 + payloadLen字节正文 + 末尾\r\n两个字节。
		totalLen = (uint32_t)payloadOffset + payloadLen + 2U;
		if (totalLen > (UART2_RX_BUFFER_SIZE - 1U))
		{
			return 0;
		}
		// 串口中断还在接收时先等待，不能把半条JSON交给上层解析。
		if (Uart2_GetRxLength() < totalLen)
		{
			Delay_ms(1);
			continue;
		}

		// 正文后必须是模块URC的\r\n，借此检查声明长度和真实数据是否一致。
		if ((payloadStart[payloadLen] != '\r') ||
			(payloadStart[payloadLen + 1U] != '\n'))
		{
			return 0;
		}
		// 二进制安全地按长度复制。当前正文是JSON，但仍不依赖字符串结束符。
		if (!Uart2_CopyRxData(payloadOffset, (uint8_t *)message, (uint16_t)payloadLen))
		{
			return 0;
		}
		// 复制完成后由STM32补上C字符串结束符，OtaManifest_Parse才能使用strstr()。
		message[payloadLen] = '\0';
		Serial_Printf("MQTT OTA command received, len=%d\r\n", payloadLen);
		return 1;
	}

	Serial_Printf("MQTT command wait timeout\r\n");
	return 0;
}

uint8_t Air780E_MqttIsConnected(void)
{
	// AT+MQTTSTATU：查询连接状态。命令成功先返回OK，响应中的
	// +MQTTSTATU:1或+MQTTSTATU: 1表示已完成MQTT认证并可发布数据。
	if (!Air780E_SendCmd("AT+MQTTSTATU", "OK", 3000U))
	{
		return 0;
	}
	return ((strstr(Uart2_GetRxBuffer(), "+MQTTSTATU:1") != 0) ||
		(strstr(Uart2_GetRxBuffer(), "+MQTTSTATU: 1") != 0));
}

void Air780E_MqttDisconnect(void)
{
	// 先用AT+MDISCONNECT关闭MQTT协议会话，再用AT+MIPCLOSE关闭底层TCP/TLS。
	// 即使第一条因为连接已断而失败，仍继续执行第二条，尽量清理模块状态。
	Air780E_SendCmd("AT+MDISCONNECT", "OK", 3000U);
	Air780E_SendCmd("AT+MIPCLOSE", "OK", 3000U);
}
