#ifndef __AIR780E_MQTT_H
#define __AIR780E_MQTT_H

#include "stm32f10x.h"
#include <stdint.h>

// 下面三个长度是STM32侧主动设置的资源上限，不是Air780E硬件极限。
// STM32F103C8T6只有20KB RAM，不能毫无节制地接收模块支持的超大MQTT消息。
// OTA命令只包含版本、大小、CRC32和OSS URL，700字节已经足够。
#define AIR780E_MQTT_HOST_MAX_LEN       128U
#define AIR780E_MQTT_TOPIC_MAX_LEN      160U
#define AIR780E_MQTT_MESSAGE_MAX_LEN    700U

/**
  * @brief  Air780E连接MQTT服务器需要的参数。
  * @note   认证参数由所选MQTT服务商提供；公共测试Broker可能不会真正校验账号。
  */
typedef struct {
	const char *host;       // MQTT Broker域名，不包含mqtt://、协议名或端口。
	uint16_t port;          // MQTT服务端口，常见明文端口1883、TLS端口8883。
	uint8_t useTls;         // 1=使用AT+SSLMIPSTART，0=使用明文AT+MIPSTART。
	const char *clientId;   // MQTT客户端标识；同一Broker上的在线客户端应保持唯一。
	const char *username;   // Broker账号；具体格式由服务商决定。
	const char *password;   // Broker密码或签名；具体格式由服务商决定。
} Air780E_MqttConfig_t;

/**
  * @brief  配置Air780E并连接MQTT服务器。
  * @param  config Broker地址、端口、TLS开关及MQTT鉴权参数。
  * @retval 1=TCP/TLS和MQTT认证都成功，0=任一步骤失败。
  */
uint8_t Air780E_MqttConnect(const Air780E_MqttConfig_t *config);

/**
  * @brief  订阅一个Topic，建立“Broker向设备下发消息”的通道。
  * @param  topic 要监听的消息主题，例如ota/example-device/cmd。
  * @param  qos   接收服务等级，合法值0、1、2；本工程OTA命令使用0。
  * @retval 1=命令收到OK且随后收到SUBACK，0=参数、响应或超时错误。
  * @note   订阅本身不读取消息；真正的消息随后以+MSUB URC到达USART2。
  */
uint8_t Air780E_MqttSubscribe(const char *topic, uint8_t qos);

/**
  * @brief  取消订阅，不再接收指定Topic的后续消息。
  * @note   本工程下载前取消订阅，避免+MSUB和HTTP/FSREAD共用USART2时互相混入。
  */
uint8_t Air780E_MqttUnsubscribe(const char *topic);

/**
  * @brief  向指定Topic发布文本，建立“设备向Broker上报消息”的通道。
  * @param  topic   消息主题，例如ota/example-device/status。
  * @param  message 消息正文，本工程传入状态JSON。
  * @param  qos     发送服务等级0、1、2。
  * @param  retain  1=Broker保存最后一条消息，0=不保存；本工程使用0。
  * @retval 1=发送完成，0=参数、提示符、响应或确认超时。
  */
uint8_t Air780E_MqttPublish(const char *topic,
	const char *message,
	uint8_t qos,
	uint8_t retain);

/**
  * @brief  等待订阅消息，校验Topic和长度，再把正文复制到message。
  * @note   此函数处理的是Air780E通过USART2上报的+MSUB，不直接访问互联网。
  */
uint8_t Air780E_MqttWaitMessage(const char *expectedTopic,
	char *message,
	uint16_t messageSize,
	uint32_t timeoutMs);

// 发送AT+MQTTSTATU主动查询模块当前是否仍保持MQTT会话。
uint8_t Air780E_MqttIsConnected(void);

// 依次关闭上层MQTT会话和下层TCP/TLS连接；不关闭4G PDP网络。
void Air780E_MqttDisconnect(void);

#endif
