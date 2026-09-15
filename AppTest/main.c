#include "stm32f10x.h"
#include "main.h"
#include "Delay.h"
#include "Serial.h"
#include "AT24C02.h"
#include "W25Q64.h"
#include "Air780E.h"
#include "OtaService.h"
#include "stm32f10x_iwdg.h"

// ==================== APP configuration ====================
// 每次发布新固件都要递增版本号。MQTT OTA只接受大于当前值的版本。
#define APP_VERSION                    6UL

// APP正常运行多少秒后检查一次云端。检查期间会看到详细的4G/MQTT串口日志。
#define APP_OTA_CHECK_INTERVAL_S       15U

// 每次连接MQTT后等待升级命令的时间。超时后断开连接并返回正常业务循环。
#define APP_OTA_COMMAND_TIMEOUT_MS     60000UL

// SIM注册和PDP激活的最长等待时间。
#define APP_NETWORK_TIMEOUT_MS         60000UL

// 新APP至少完成5次一秒业务循环后才确认健康；在此之前复位会被Boot计为试运行失败。
#define APP_CONFIRM_UPTIME_S            5UL

// 设为0可临时关闭APP联网OTA，但不影响Bootloader和APP本身运行。
#define APP_OTA_ENABLE                 1U

#define APP_LED_PIN                    GPIO_Pin_13

// AT24C02和Air780E下载函数通过main.h中的extern声明访问这一份控制块。
// APP与Bootloader不会同时运行，因此它们各自在自己的RAM中定义同名变量即可。
OTA_CB_t OTA_CB_Info;

static uint32_t App_UptimeSeconds;
static uint8_t App_StorageReady;
static uint8_t App_TrialConfirmed;

/**
  * @brief  启动约26秒独立看门狗，捕获新APP死循环或长期卡死。
  * @note   Delay_ms()会在正常AT等待期间喂狗；不再执行正常延时路径的死循环会触发复位。
  */
static void App_WatchdogInit(void)
{
	IWDG_WriteAccessCmd(IWDG_WriteAccess_Enable);
	IWDG_SetPrescaler(IWDG_Prescaler_256);
	IWDG_SetReload(0x0FFFU);
	IWDG_ReloadCounter();
	IWDG_Enable();
}

/**
  * @brief  初始化最小系统板上的PC13 LED。
  * @note   常见Blue-Pill板载LED为低电平点亮。
  */
static void App_LedInit(void)
{
	GPIO_InitTypeDef gpio;

	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC, ENABLE);
	gpio.GPIO_Pin = APP_LED_PIN;
	gpio.GPIO_Mode = GPIO_Mode_Out_PP;
	gpio.GPIO_Speed = GPIO_Speed_2MHz;
	GPIO_Init(GPIOC, &gpio);
	GPIO_SetBits(GPIOC, APP_LED_PIN);
}

/**
  * @brief  执行一秒钟的示例业务任务。
  * @note   这里用闪灯和运行时间代表真实产品中的采集、控制或显示任务。
  */
static void App_RunBusinessOneSecond(void)
{
	Delay_s(1);
	GPIO_WriteBit(GPIOC,
		APP_LED_PIN,
		(BitAction)(1U - GPIO_ReadOutputDataBit(GPIOC, APP_LED_PIN)));

	App_UptimeSeconds++;
	Serial_Printf("APP v%lu running, uptime=%lu s\r\n",
		(unsigned long)APP_VERSION,
		(unsigned long)App_UptimeSeconds);

	if (!App_TrialConfirmed && (App_UptimeSeconds >= APP_CONFIRM_UPTIME_S))
	{
		App_TrialConfirmed = OtaService_ConfirmRunningApp(APP_VERSION);
	}
}

/**
  * @brief  执行一次正式的4G远程OTA检查。
  * @retval 1=已收到并保存新固件，即将复位；0=本轮没有完成升级。
  *
  * 数据路径：
  * MQTT下发version/size/crc32/auth/url元数据；真正的bin由HTTP从OSS下载。
  * 下载完成后只写W25Q64和AT24C02，不在APP运行期间擦写APP自己的内部Flash。
  */
static uint8_t App_CheckOtaOnce(void)
{
	Serial_Printf("\r\n===== APP OTA check =====\r\n");

	if (!App_StorageReady)
	{
		Serial_Printf("Skip OTA: W25Q64 JEDEC ID is invalid\r\n");
		return 0;
	}

	// 完成AT测试、SIM检查、网络注册、附着和PDP激活。
	if (!Air780E_NetworkReady(AIR780E_DEFAULT_APN, APP_NETWORK_TIMEOUT_MS))
	{
		Serial_Printf("APP OTA network unavailable\r\n");
		return 0;
	}

	// MQTT命令合法且固件下载、两次CRC32校验、EEPROM写入全部成功时返回1。
	if (!OtaService_RunMqttOnce(APP_VERSION, APP_OTA_COMMAND_TIMEOUT_MS))
	{
		Serial_Printf("APP OTA check finished without new image\r\n");
		return 0;
	}

	Serial_Printf("APP OTA image ready, reset for Bootloader install\r\n");
	Delay_ms(200);
	return 1;
}

int main(void)
{
	uint8_t manufacturerId;
	uint16_t deviceId;
	uint16_t intervalSeconds;

	// Bootloader跳转APP后，所有中断都必须从APP的向量表0x08005000查找。
	// 这一步必须放在初始化USART2等中断外设之前。
	SCB->VTOR = FLASH_APP_START;
	__DSB();
	__ISB();

	// USART1只负责921600波特率调试输出；Air780E仍使用USART2的PA2/PA3。
	USART1_TxOnlyInit();
	App_LedInit();
	App_WatchdogInit();

	// 初始化两种外部存储器，并读取上一次升级留下的控制块。
	AT24C02_Init();
	W25Q64_Init();
	AT24C02_ReadOTA_CB_Info();

	// JEDEC ID应为EF 4017。ID不匹配时继续运行业务，但禁止下载固件。
	W25Q64_ReadID(&manufacturerId, &deviceId);
	App_StorageReady = ((manufacturerId == 0xEFU) && (deviceId == 0x4017U));

	__enable_irq();
	Serial_Printf("\r\nAPP v%lu started at 0x%08lX\r\n",
		(unsigned long)APP_VERSION,
		(unsigned long)FLASH_APP_START);
	Serial_Printf("W25Q64 JEDEC ID: %02X %04X (%s)\r\n",
		manufacturerId,
		deviceId,
		App_StorageReady ? "PASS" : "FAIL");
	Serial_Printf("OTA CB: state=%lu version=%lu size=%lu crc=0x%08lX error=%lu\r\n",
		(unsigned long)OTA_CB_Info.OTA_flag,
		(unsigned long)OTA_CB_Info.app_version,
		(unsigned long)OTA_CB_Info.image_size,
		(unsigned long)OTA_CB_Info.image_crc32,
		(unsigned long)OTA_CB_Info.error_code);

#if APP_OTA_ENABLE
	// 只初始化USART2和接收中断。真正联网放到周期任务中执行。
	Air780E_Init(AIR780E_DEFAULT_BAUD);
	Serial_Printf("APP remote OTA enabled, check interval=%u s\r\n",
		APP_OTA_CHECK_INTERVAL_S);
#else
	Serial_Printf("APP remote OTA disabled\r\n");
#endif

	while (1)
	{
		// 两次联网检查之间，APP继续执行自己的业务。
		for (intervalSeconds = 0;
			 intervalSeconds < APP_OTA_CHECK_INTERVAL_S;
			 intervalSeconds++)
		{
			App_RunBusinessOneSecond();
		}

#if APP_OTA_ENABLE
		if (App_CheckOtaOnce())
		{
			// 复位后CPU从0x08000000重新启动，Bootloader看到PENDING后安装新APP。
			NVIC_SystemReset();
		}
#endif
	}
}
