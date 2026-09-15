#include "OtaService.h"
#include "Air780E.h"
#include "Air780E_Mqtt.h"
#include "AT24C02.h"
#include "OtaManifest.h"
#include "Serial.h"
#include "main.h"
#include <stdio.h>
#include <string.h>

// MQTT命令和解析结果使用静态存储，避免两个大对象占用函数栈。
// STM32F103C8T6只有20KB RAM，因此这里明确限制消息和URL的最大长度。
static char OtaService_MqttMessage[AIR780E_MQTT_MESSAGE_MAX_LEN + 1U];
static OtaManifest_t OtaService_Manifest;

uint8_t OtaService_ConfirmRunningApp(uint32_t runningVersion)
{
	if (OTA_CB_Info.OTA_flag == OTA_STATE_DONE)
	{
		return 1;
	}
	if ((OTA_CB_Info.OTA_flag != OTA_STATE_TRIAL) ||
		(OTA_CB_Info.app_version != runningVersion))
	{
		Serial_Printf("APP confirm rejected: state=%lu stored=%lu running=%lu\r\n",
			(unsigned long)OTA_CB_Info.OTA_flag,
			(unsigned long)OTA_CB_Info.app_version,
			(unsigned long)runningVersion);
		return 0;
	}

	// 只有APP自己运行到健康确认点后才能提交DONE；Bootloader不会替APP做这个决定。
	OTA_CB_Info.OTA_flag = OTA_STATE_DONE;
	OTA_CB_Info.trial_boot_count = 0U;
	OTA_CB_Info.error_code = OTA_ERR_NONE;
	if (!AT24C02_WriteOTA_CB_Info(&OTA_CB_Info))
	{
		Serial_Printf("APP confirmation commit failed\r\n");
		return 0;
	}
	Serial_Printf("APP version %lu confirmed\r\n", (unsigned long)runningVersion);
	return 1;
}

/**
  * @brief  向状态Topic发布当前OTA阶段。
  * @param  state   状态文本，例如online、accepted、ready。
  * @param  version 与状态对应的固件版本号。
  * @note   状态消息使用QoS0。上报失败只会丢失一条日志，不影响本地固件校验结果。
  */
static void OtaService_PublishState(const char *state, uint32_t version)
{
	char report[96];

	// snprintf会限制写入长度，避免状态文本异常时覆盖栈上其他数据。
	snprintf(report,
		sizeof(report),
		"{\"state\":\"%s\",\"version\":%lu}",
		state,
		(unsigned long)version);

	// qos=0表示尽力发送；retain=0表示Broker不长期保存这条状态。
	(void)Air780E_MqttPublish(MQTT_OTA_STATUS_TOPIC, report, 0U, 0U);
}

uint8_t OtaService_RunMqttOnce(uint32_t currentVersion, uint32_t commandTimeoutMs)
{
	Air780E_MqttConfig_t mqttConfig;

	// 把main.h里的宏整理成MQTT驱动需要的结构体。
	mqttConfig.host = MQTT_BROKER_HOST;
	mqttConfig.port = MQTT_BROKER_PORT;
	mqttConfig.useTls = MQTT_USE_TLS;
	mqttConfig.clientId = MQTT_CLIENT_ID;
	mqttConfig.username = MQTT_USERNAME;
	mqttConfig.password = MQTT_PASSWORD;

	// 防止忘记填写配置时仍向模块发送带占位符的AT命令。
	if ((strstr(MQTT_BROKER_HOST, "replace-") != 0) ||
		(strstr(MQTT_CLIENT_ID, "replace-") != 0) ||
		(strstr(MQTT_USERNAME, "replace-") != 0))
	{
		Serial_Printf("Fill MQTT broker config in User/main.h first\r\n");
		return 0;
	}

	// 先建立TCP/TLS连接，再由Air780E的MQTT协议栈完成CONNECT认证。
	if (!Air780E_MqttConnect(&mqttConfig))
	{
		return 0;
	}

	// 订阅命令Topic后，云端发来的JSON才会以+MSUB URC送到USART2。
	if (!Air780E_MqttSubscribe(MQTT_OTA_CMD_TOPIC, 0U))
	{
		Air780E_MqttDisconnect();
		return 0;
	}

	OtaService_PublishState("online", currentVersion);
	Serial_Printf("Wait OTA command topic: %s\r\n", MQTT_OTA_CMD_TOPIC);

	// commandTimeoutMs到期只表示这一轮没有命令，不会修改W25Q64或AT24C02。
	if (!Air780E_MqttWaitMessage(
		MQTT_OTA_CMD_TOPIC,
		OtaService_MqttMessage,
		sizeof(OtaService_MqttMessage),
		commandTimeoutMs))
	{
		Air780E_MqttDisconnect();
		return 0;
	}

	// 把JSON中的version、size、crc32、auth、url转换为有类型的字段并检查格式。
	if (!OtaManifest_Parse(OtaService_MqttMessage, &OtaService_Manifest))
	{
		Serial_Printf("OTA command JSON invalid\r\n");
		OtaService_PublishState("invalid_manifest", currentVersion);
		Air780E_MqttDisconnect();
		return 0;
	}

	Serial_Printf("OTA command: version=%lu size=%lu crc=0x%08lX\r\n",
		(unsigned long)OtaService_Manifest.version,
		(unsigned long)OtaService_Manifest.size,
		(unsigned long)OtaService_Manifest.crc32);

	// 只接受比当前APP更新的版本，防止旧命令或保留消息导致意外降级。
	if (OtaService_Manifest.version <= currentVersion)
	{
		Serial_Printf("Ignore OTA version %lu, current version is %lu\r\n",
			(unsigned long)OtaService_Manifest.version,
			(unsigned long)currentVersion);
		OtaService_PublishState("ignored_old_version", OtaService_Manifest.version);
		Air780E_MqttDisconnect();
		return 0;
	}

	// APP分区只有44KB；启用安全策略时还必须拒绝明文HTTP固件地址。
	if ((OtaService_Manifest.size == 0U) ||
		(OtaService_Manifest.size > FLASH_APP_SIZE) ||
		(MQTT_OTA_REQUIRE_HTTPS &&
		 (strncmp(OtaService_Manifest.url, "https://", 8U) != 0)))
	{
		Serial_Printf("OTA size or URL rejected\r\n");
		OtaService_PublishState("rejected", OtaService_Manifest.version);
		Air780E_MqttDisconnect();
		return 0;
	}

	OtaService_PublishState("accepted", OtaService_Manifest.version);

	// MQTT和HTTP共用USART2。下载前取消订阅，避免新的+MSUB消息混入HTTP响应。
	if (!Air780E_MqttUnsubscribe(MQTT_OTA_CMD_TOPIC))
	{
		Air780E_MqttDisconnect();
		return 0;
	}

	// 下载函数完成以下工作：
	// 1. HTTPGETTOFS把bin保存到Air780E内部文件系统；
	// 2. FSREAD每次读取最多200字节并写入W25Q64；
	// 3. 校验下载流CRC32，再回读W25Q64校验一次；
	// 4. 保存认证标签并把AT24C02 v3控制块写成OTA_STATE_PENDING。
	if (!Air780E_DownloadFirmwareToW25Q64(
		OtaService_Manifest.url,
		OtaService_Manifest.version,
		OtaService_Manifest.size,
		OtaService_Manifest.crc32,
		OtaService_Manifest.authTag,
		OTA_IMAGE_STORE_ADDR))
	{
		OtaService_PublishState("download_failed", OtaService_Manifest.version);
		Air780E_MqttDisconnect();
		return 0;
	}

	// 此时内部Flash还没有变化。软件复位后由Bootloader读取PENDING并安装。
	OtaService_PublishState("ready", OtaService_Manifest.version);
	Air780E_MqttDisconnect();
	return 1;
}
