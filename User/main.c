#include "stm32f10x.h"                  // STM32 标准外设库。
#include "Delay.h"                     // Delay_ms() / Delay_s()。
#include "Serial.h"                    // USART1 调试打印和 DMA 接收。
#include "AT24C02.h"                   // EEPROM 保存 OTA 控制块。
#include "MyI2C.h"                     // 模拟 I2C。
#include "MySPI.h"                     // SPI1。
#include "W25Q64.h"                    // 外部 SPI Flash。
#include "MyFlash.h"                   // 片内 Flash 读写。
#include "main.h"                      // Flash 分区、OTA 控制块、调试开关。
#include "Boot.h"                      // Bootloader 跳转和升级搬运。
#include "Updete.h"                    // 串口 OTA 接收模块。
#include "Air780E.h"                   // Air780E/M100M 4G 模块 AT 测试。
#include "Air780E_Mqtt.h"              // Air780E MQTT连接、订阅、发布和消息接收。
#include "OtaManifest.h"               // MQTT OTA JSON命令解析。
#include "OtaService.h"                // Boot测试模式与正式APP共用的MQTT OTA业务流程。
#include "OtaAuth.h"                   // 解析直接下载测试使用的固件认证标签。
#include <stdio.h>                      // sprintf()生成MQTT状态JSON。
#include <string.h>                     // strncmp()检查HTTPS URL。

// 上电后等待串口升级包的时间。
// 有合法 APP 时：5 秒内没有收到升级包，就自动跳转 APP。
// 没有合法 APP 时：窗口结束后仍然留在 Bootloader，继续等待串口升级。
#define BOOT_SERIAL_WAIT_MS     5000U

OTA_CB_t OTA_CB_Info;                  // 全局 OTA 控制块缓存，AT24C02/Boot/Update 共用。

#if 0
// 旧版Boot内部MQTT OTA实现仅保留作学习对照。
// 当前实际流程已经迁移到Hardware/OtaService.c，避免Boot和APP维护两份不同代码。
// MQTT命令最多700字节，manifest中还包含501字节URL数组。
// 用static把它们放在全局静态RAM区，避免作为局部变量占用本来就很小的函数栈。
// Boot_MqttMessage保存“刚从USART2取出的JSON原文”，Boot_OtaManifest保存“解析后的字段”。
static char Boot_MqttMessage[AIR780E_MQTT_MESSAGE_MAX_LEN + 1U];
static OtaManifest_t Boot_OtaManifest;

/**
  * @brief  通过MQTT状态Topic上报当前OTA阶段。
  * @param  state   状态文字，例如online、accepted、ready或download_failed。
  * @param  version 与本次状态对应的固件版本号。
  * @note   状态消息很短，采用QoS0；即使上报失败，也不改变本地升级校验结果。
  */
static void Boot_MqttPublishState(const char *state, uint32_t version)
{
	// 最终JSON示例：{"state":"ready","version":2}。
	char report[96];

	// sprintf把state字符串填到%s，把uint32_t版本号填到%lu。
	// version先转成unsigned long，是为了匹配%lu并避免编译器类型警告。
	sprintf(report,
		"{\"state\":\"%s\",\"version\":%lu}",
		state,
		(unsigned long)version);
	// QoS=0表示尽力发送；retain=0表示Broker不把它保存为该Topic的保留消息。
	// 这里不检查返回值，因为状态日志失败不能反过来破坏已经下载成功的固件。
	Air780E_MqttPublish(MQTT_OTA_STATUS_TOPIC, report, 0U, 0U);
}

/**
  * @brief  执行一次“连接MQTT -> 等待升级命令 -> OSS下载”的完整远程OTA流程。
  *
  * 注意：这个函数只负责把新app.bin安全地暂存到W25Q64，并把AT24C02状态
  * 写成OTA_STATE_PENDING。它不会在联网过程中直接擦写STM32内部Flash。
  * 返回1后main.c执行系统复位，下一次启动才由BootLoader_Branch()安装固件。
  * @retval 1=固件已验证并写入W25Q64，0=本轮未升级或失败。
  */
static uint8_t Boot_RunMqttOtaOnce(void)
{
	// 这个结构体只保存指针，不会复制几百字节的配置字符串。
	Air780E_MqttConfig_t mqttConfig;

	// 第1步：把main.h中的宏整理成MQTT驱动需要的配置结构体。
	// host/port决定连接哪个Broker；后三项是该Broker要求的客户端身份参数。
	mqttConfig.host = MQTT_BROKER_HOST;
	mqttConfig.port = MQTT_BROKER_PORT;
	mqttConfig.useTls = MQTT_USE_TLS;
	mqttConfig.clientId = MQTT_CLIENT_ID;
	mqttConfig.username = MQTT_USERNAME;
	mqttConfig.password = MQTT_PASSWORD;

	// 默认宏中带有replace-占位文字。没填真实参数时立即退出，避免发送无意义AT命令。
	if ((strstr(MQTT_BROKER_HOST, "replace-") != 0) ||
		(strstr(MQTT_USERNAME, "replace-") != 0))
	{
		Serial_Printf("Fill MQTT broker config in User/main.h first\r\n");
		return 0;
	}

	// 第2步：建立TCP/TLS连接并使用ClientId、username、password完成MQTT认证。
	if (!Air780E_MqttConnect(&mqttConfig))
	{
		return 0;
	}
	// 第3步：订阅升级命令Topic。QoS0足够用于当前联调，云端可以在需要时重发命令。
	if (!Air780E_MqttSubscribe(MQTT_OTA_CMD_TOPIC, 0U))
	{
		Air780E_MqttDisconnect();
		return 0;
	}

	// 第4步：告诉云端设备已在线，并等待一条完整的+MSUB升级消息。
	// OTA_CB_Info.app_version来自AT24C02，表示设备当前记录的固件版本。
	Boot_MqttPublishState("online", OTA_CB_Info.app_version);
	Serial_Printf("Wait OTA command topic: %s\r\n", MQTT_OTA_CMD_TOPIC);
	if (!Air780E_MqttWaitMessage(
		MQTT_OTA_CMD_TOPIC,
		Boot_MqttMessage,
		sizeof(Boot_MqttMessage),
		MQTT_COMMAND_TIMEOUT_MS))
	{
		Air780E_MqttDisconnect();
		return 0;
	}

	// 第5步：把JSON原文转换成version、size、crc32、auth、url结构化数据。
	// 这里失败说明消息缺字段或格式不正确，绝不能带着不可信参数下载和擦写。
	if (!OtaManifest_Parse(Boot_MqttMessage, &Boot_OtaManifest))
	{
		Serial_Printf("OTA command JSON invalid\r\n");
		Boot_MqttPublishState("invalid_manifest", OTA_CB_Info.app_version);
		Air780E_MqttDisconnect();
		return 0;
	}

	// 只打印元数据，不打印可能很长且带签名参数的OSS URL。
	Serial_Printf("OTA command: version=%d size=%d crc=0x%08X\r\n",
		Boot_OtaManifest.version,
		Boot_OtaManifest.size,
		Boot_OtaManifest.crc32);

	// 第6步：版本策略检查。
	// 正常状态下只允许新版本号大于当前版本，防止云端误发旧固件造成降级。
	// ERROR表示上一次安装失败，此时允许同一版本重新下载用于恢复。
	if ((Boot_OtaManifest.version <= OTA_CB_Info.app_version) &&
		(OTA_CB_Info.OTA_flag != OTA_STATE_ERROR))
	{
		Serial_Printf("Ignore old OTA version\r\n");
		Boot_MqttPublishState("ignored_old_version", Boot_OtaManifest.version);
		Air780E_MqttDisconnect();
		return 0;
	}

	// 第7步：资源和下载地址检查。
	// app.bin不能超过44KB APP分区；启用HTTPS要求时，URL必须以https://开头。
	// strncmp(..., 8)只比较前8个字符，不会要求整条长URL完全相同。
	if ((Boot_OtaManifest.size > FLASH_APP_SIZE) ||
		(MQTT_OTA_REQUIRE_HTTPS &&
		 (strncmp(Boot_OtaManifest.url, "https://", 8U) != 0)))
	{
		Serial_Printf("OTA size or URL rejected\r\n");
		Boot_MqttPublishState("rejected", Boot_OtaManifest.version);
		Air780E_MqttDisconnect();
		return 0;
	}

	// 参数全部通过，云端看到accepted就知道设备准备开始下载。
	Boot_MqttPublishState("accepted", Boot_OtaManifest.version);

	// 第8步：取消命令订阅后再下载。
	// MQTT和HTTP共用USART2，若下载期间又收到+MSUB，可能混入HTTP/FSREAD响应。
	if (!Air780E_MqttUnsubscribe(MQTT_OTA_CMD_TOPIC))
	{
		Air780E_MqttDisconnect();
		return 0;
	}

	// 第9步：复用第四步已经实现的OSS下载函数。该函数内部完整流程是：
	// HTTPGETTOFS把bin下载到Air780E文件系统 -> FSREAD每次最多读200字节
	// -> 分块写W25Q64 -> 计算下载流CRC32 -> 回读W25Q64再算CRC32
	// -> 全部正确后把版本、大小、CRC、地址和PENDING状态写入AT24C02。
	if (!Air780E_DownloadFirmwareToW25Q64(
		Boot_OtaManifest.url,
		Boot_OtaManifest.version,
		Boot_OtaManifest.size,
		Boot_OtaManifest.crc32,
		Boot_OtaManifest.authTag,
		OTA_IMAGE_STORE_ADDR))
	{
		Boot_MqttPublishState("download_failed", Boot_OtaManifest.version);
		Air780E_MqttDisconnect();
		return 0;
	}

	// 第10步：ready表示W25Q64和AT24C02已准备完成。
	// 断开网络后返回1，外层代码将复位MCU，并由Bootloader安装新APP。
	Boot_MqttPublishState("ready", Boot_OtaManifest.version);
	Air780E_MqttDisconnect();
	return 1;
}
#endif

#if BOOT_4G_TEST_ENABLE
/**
  * @brief  Bootloader 4G 专用测试模式。
  * @note   这个模式只用于上板调试 Air780E，不执行串口 OTA 和 APP 跳转。
  */
static void Boot_Run4GTestMode(void)
{
#if BOOT_4G_OTA_DOWNLOAD_TEST_ENABLE
	uint8_t otaAuthTag[OTA_AUTH_TAG_SIZE];
#endif
#if BOOT_4G_HTTP_TEST_ENABLE && !BOOT_4G_OTA_DOWNLOAD_TEST_ENABLE && !BOOT_4G_MQTT_OTA_TEST_ENABLE && !BOOT_4G_CA_PROVISION_ENABLE
	Air780E_HttpResult_t httpResult;
#endif

	Serial_Printf("\r\n===== Boot 4G test mode =====\r\n");
	Serial_Printf("USART1: debug log, USART2: Air780E AT port\r\n");

	// USART2初始化为模块AT口，波特率必须与Air780E AT固件当前设置一致。
	Air780E_Init(AIR780E_DEFAULT_BAUD);

	while (1)
	{
		// 先检查AT通信、SIM卡、注册状态和数据网络。公网卡无专用APN时宏可填空字符串。
		if (Air780E_NetworkReady(AIR780E_DEFAULT_APN, 60000U))
		{
			Serial_Printf("4G test PASS\r\n");

			#if BOOT_4G_CA_PROVISION_ENABLE
			// 这是只需执行一次的准备步骤：把EMQX官方CA证书保存到Air780E内部Flash。
			// 成功后停在这里，防止10秒后再次下载并反复擦写模块文件系统。
			if (Air780E_ProvisionCaCertificate(BOOT_4G_CA_URL, BOOT_4G_CA_SIZE))
			{
				Serial_Printf("EMQX CA provision PASS\r\n");
				Serial_Printf("Set CA provision=0 and MQTT test=1, then rebuild\r\n");
				while (1)
				{
					Delay_s(1);
				}
			}
			else
			{
				Serial_Printf("EMQX CA provision FAIL\r\n");
			}
#elif BOOT_4G_MQTT_OTA_TEST_ENABLE
			// MQTT模式成功返回时，固件已经在W25Q64中且控制块为PENDING。
			if (OtaService_RunMqttOnce(
				OTA_CB_Info.app_version,
				MQTT_COMMAND_TIMEOUT_MS))
			{
				Serial_Printf("MQTT OTA image ready, reset to install\r\n");
				Delay_ms(100);
				// 软件复位后从Bootloader重新运行，BootLoader_Branch()负责安装APP。
				NVIC_SystemReset();
			}
#elif BOOT_4G_OTA_DOWNLOAD_TEST_ENABLE
			if (OtaAuth_ParseTagHex(BOOT_4G_OTA_AUTH_TAG_HEX, otaAuthTag) &&
				OtaAuth_IsTagPresent(otaAuthTag) &&
				Air780E_DownloadFirmwareToW25Q64(
				BOOT_4G_OTA_URL,
				BOOT_4G_OTA_VERSION,
				BOOT_4G_OTA_SIZE,
				BOOT_4G_OTA_CRC32,
				otaAuthTag,
				OTA_IMAGE_STORE_ADDR))
			{
				Serial_Printf("OSS firmware download PASS, reset to install\r\n");
				Delay_ms(100);
				NVIC_SystemReset();
			}
			else
			{
				Serial_Printf("OSS firmware download FAIL\r\n");
			}
#elif BOOT_4G_HTTP_TEST_ENABLE
			if (Air780E_HttpGetStart(BOOT_4G_HTTP_TEST_URL, &httpResult, 120000U))
			{
				Serial_Printf("HTTP GET test PASS, data length=%d\r\n", httpResult.dataLength);
			}
			else
			{
				Serial_Printf("HTTP GET test FAIL, status=%d\r\n", httpResult.statusCode);
			}
			Air780E_HttpTerminate();
#endif
		}
		else
		{
			Serial_Printf("4G test FAIL\r\n");
		}

		Serial_Printf("Retry after 10 seconds\r\n");
		Delay_s(10);
	}
}
#endif

int main(void)
{
	uint32_t waitMs;

	// 1. 初始化 USART1。USART1 既打印调试日志，也接收 PC 发来的串口升级包。
	USART1_Init();

	// 2. 初始化模拟 I2C。AT24C02 通过 PB10/PB11 保存 OTA 控制块。
	MyI2C_Init();

	// 3. 初始化 W25Q64。升级包先写入外部 Flash，校验通过后再搬运到片内 APP 区。
	W25Q64_Init();

	// 4. 从 AT24C02 读取 OTA 控制块，判断上一次是否已经收到完整新固件。
	AT24C02_ReadOTA_CB_Info();
	// 上电打印EEPROM中的升级记录，方便实物测试时确认AT24C02确实读写成功。
	// state：0=IDLE、1=PENDING、2=UPDATING、3=DONE、4=ERROR、5=TRIAL、6=ROLLBACK。
	Serial_Printf("OTA CB: state=%d seq=%d version=%d size=%d crc=0x%08X addr=0x%08X error=%d\r\n",
		OTA_CB_Info.OTA_flag,
		OTA_CB_Info.sequence,
		OTA_CB_Info.app_version,
		OTA_CB_Info.image_size,
		OTA_CB_Info.image_crc32,
		OTA_CB_Info.image_addr,
		OTA_CB_Info.error_code);

	// 5. 如果已经有待升级任务，先执行搬运。
	// 搬运成功后 BootLoader_Branch() 会尝试跳 APP；失败则留在 Bootloader。
	if ((OTA_CB_Info.OTA_flag == OTA_STATE_PENDING) ||
		(OTA_CB_Info.OTA_flag == OTA_STATE_UPDATING) ||
		(OTA_CB_Info.OTA_flag == OTA_STATE_TRIAL) ||
		(OTA_CB_Info.OTA_flag == OTA_STATE_ROLLBACK))
	{
		BootLoader_Branch();
	}

#if BOOT_4G_TEST_ENABLE
	// 先处理已有PENDING升级，再进入4G测试；这样OSS下载成功复位后可以正常安装APP。
	Boot_Run4GTestMode();
#endif

	if (OTA_CB_Info.OTA_flag == OTA_STATE_ERROR)
	{
		// 升级失败时 APP 区可能只写了一部分，必须等待重新升级，不能冒险跳转。
		Serial_Printf("OTA error=%d, recovery mode\r\n", OTA_CB_Info.error_code);
	}

	// 6. 没有待搬运任务时，进入串口升级等待窗口。
	Update_Init(UPDATE_SOURCE_SERIAL);
	Serial_Printf("Bootloader serial OTA window: %d ms\r\n", BOOT_SERIAL_WAIT_MS);

	for (waitMs = 0; waitMs < BOOT_SERIAL_WAIT_MS; waitMs++)
	{
		Update_Task();

		// 只要收到 START 或半包数据，就说明上位机正在升级，继续留在 Bootloader。
		if (Update_IsBusy())
		{
			Serial_Printf("OTA receiving, stay in Bootloader\r\n");
			while (1)
			{
				Update_Task();
			}
		}

		Delay_ms(1);
	}

	// 7. 等待窗口结束后，如果 APP 合法，就跳转 APP。
	if ((OTA_CB_Info.OTA_flag != OTA_STATE_ERROR) && Boot_IsValidApp(FLASH_APP_START))
	{
		Boot_JumpToApp(FLASH_APP_START);
	}

	// 8. 没有合法 APP 时，留在 Bootloader，持续等待串口升级。
	Serial_Printf("No valid APP, stay in Bootloader\r\n");
	while (1)
	{
		Update_Task();
	}
}

/**
  * @brief  USART1 中断服务函数。
  * @note   利用 IDLE 空闲中断判断一段串口数据已经接收完成。
  */
void USART1_IRQHandler(void)
{
	uint16_t rxLength;

	// 检测 IDLE 中断。IDLE 表示串口线路空闲，通常可认为“一段数据结束”。
	if (USART_GetITStatus(USART1, USART_IT_IDLE) != RESET)
	{
		// 1. 清除 IDLE 标志：必须先读 SR，再读 DR。
		USART_GetFlagStatus(USART1, USART_FLAG_IDLE);
		USART_ReceiveData(USART1);

		// 2. 停止 DMA，防止计算长度时 DMA 继续写入。
		DMA_Cmd(DMA1_Channel5, DISABLE);

		// 3. 计算本次实际收到的字节数。
		rxLength = (U0_RX_MAX + 1) - DMA_GetCurrDataCounter(DMA1_Channel5);
		if (rxLength == 0)
		{
			DMA_SetCurrDataCounter(DMA1_Channel5, U0_RX_MAX + 1);
			DMA1_Channel5->CMAR = (uint32_t)U1CB.URxDataIN->start;
			DMA_Cmd(DMA1_Channel5, ENABLE);
			return;
		}

		// 4. 把本段数据登记到接收队列，由主循环里的 Update_Task() 解析。
		U1CB.U1RxCounter += rxLength;
		U1CB.URxDataIN->end = &U1RxBuffer[U1CB.U1RxCounter - 1];
		U1CB.URxDataIN++;
		if (U1CB.URxDataIN > U1CB.URxDataEND)
		{
			U1CB.URxDataIN = &U1CB.pU1RxData[0];
		}

		// 5. 为下一次 DMA 接收准备新的缓冲区起始地址。
		if ((U0_RX_SIZE - U1CB.U1RxCounter) >= (U0_RX_MAX + 1))
		{
			U1CB.URxDataIN->start = &U1RxBuffer[U1CB.U1RxCounter];
		}
		else
		{
			U1CB.URxDataIN->start = &U1RxBuffer[0];
			U1CB.U1RxCounter = 0;
		}

		// 6. 重新启动 DMA 接收。
		DMA_SetCurrDataCounter(DMA1_Channel5, U0_RX_MAX + 1);
		DMA1_Channel5->CMAR = (uint32_t)U1CB.URxDataIN->start;
		DMA_Cmd(DMA1_Channel5, ENABLE);
	}
}
