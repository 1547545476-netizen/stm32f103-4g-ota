#include "stm32f10x.h"                  // Device header
#include "Delay.h"
#include "Serial.h"
#include "AT24C02.h"
#include "MyI2C.h"
#include "MySPI.h"
#include "W25Q64.h"
#include "MyFlash.h"
#include "main.h"
#include "Boot.h"
#include "OtaAuth.h"
#include <string.h>

#define OTA_COPY_BUF_SIZE    256U

typedef void (*BootJumpFunc_t)(void);

// OTA 搬运缓冲区。
// W25Q64 一页是 256 字节，用 256 字节缓冲区刚好方便按页读取、按半字写入片内 Flash。
static uint8_t OTA_CopyBuffer[OTA_COPY_BUF_SIZE];

static uint8_t Boot_PerformRollback(void);

static uint8_t Boot_SaveControlBlock(void)
{
	if (!AT24C02_WriteOTA_CB_Info(&OTA_CB_Info))
	{
		Serial_Printf("OTA control block commit failed\r\n");
		return 0;
	}
	return 1;
}

/**
  * @brief  增量计算 CRC32。
  * @note   初始值传 0xFFFFFFFF，全部数据算完后再异或 0xFFFFFFFF。
  */
static uint32_t Boot_CRC32Update(uint32_t crc, const uint8_t *data, uint32_t len)
{
	uint32_t i;
	uint8_t bit;

	for (i = 0; i < len; i++)
	{
		crc ^= data[i];
		for (bit = 0; bit < 8; bit++)
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
  * @brief  计算 W25Q64 中暂存固件的 CRC32。
  * @param  imageAddr: 固件在 W25Q64 中的起始地址。
  * @param  imageSize: 固件大小，单位字节。
  * @retval 固件 CRC32。
  */
static uint32_t Boot_CalcExternalImageCRC(uint32_t imageAddr, uint32_t imageSize)
{
	uint32_t offset = 0;
	uint32_t readSize;
	uint32_t crc = 0xFFFFFFFFUL;

	while (offset < imageSize)
	{
		readSize = imageSize - offset;
		if (readSize > OTA_COPY_BUF_SIZE)
		{
			readSize = OTA_COPY_BUF_SIZE;
		}

		W25Q64_ReadData(imageAddr + offset, OTA_CopyBuffer, readSize);
		crc = Boot_CRC32Update(crc, OTA_CopyBuffer, readSize);
		offset += readSize;
	}

	return crc ^ 0xFFFFFFFFUL;
}

/**
  * @brief  计算片内 APP 区的 CRC32。
  * @note   写入 APP 后再算一次，确认内部 Flash 中的数据和外部固件一致。
  */
static uint32_t Boot_CalcInternalAppCRC(uint32_t appAddr, uint32_t appSize)
{
	uint32_t offset = 0;
	uint32_t readSize;
	uint32_t crc = 0xFFFFFFFFUL;

	while (offset < appSize)
	{
		readSize = appSize - offset;
		if (readSize > OTA_COPY_BUF_SIZE)
		{
			readSize = OTA_COPY_BUF_SIZE;
		}

		memcpy(OTA_CopyBuffer, (const void *)(appAddr + offset), readSize);
		crc = Boot_CRC32Update(crc, OTA_CopyBuffer, readSize);
		offset += readSize;
	}

	return crc ^ 0xFFFFFFFFUL;
}

/**
  * @brief  安装新固件前，把当前片内APP复制到W25Q64独立备份槽。
  * @note   控制块保持PENDING直到备份写完并回读CRC通过；中途断电会重新备份。
  */
static uint8_t Boot_BackupCurrentApp(void)
{
	uint32_t addr;
	uint32_t endAddr;
	uint32_t offset = 0U;
	uint32_t copySize;
	uint32_t crc;

	if (OTA_CB_Info.backup_size == 0U)
	{
		// 从旧版32字节控制块迁移时没有旧APP大小，退化为备份完整44KB APP分区。
		// 多复制的0xFF不会影响恢复，但保证第一次迁移也具备二进制回滚能力。
		if (!Boot_IsValidApp(FLASH_APP_START))
		{
			return 1;
		}
		OTA_CB_Info.backup_version = 0U;
		OTA_CB_Info.backup_size = FLASH_APP_SIZE;
		OTA_CB_Info.backup_crc32 = Boot_CalcInternalAppCRC(FLASH_APP_START, FLASH_APP_SIZE);
		OTA_CB_Info.backup_addr = OTA_BACKUP_STORE_ADDR;
	}
	if ((OTA_CB_Info.backup_size > FLASH_APP_SIZE) ||
		(OTA_CB_Info.backup_addr != OTA_BACKUP_STORE_ADDR) ||
		!Boot_IsValidApp(FLASH_APP_START))
	{
		return 0;
	}

	// 先确认片内当前APP仍与记录的CRC一致，避免把已经损坏的程序当作回滚镜像。
	crc = Boot_CalcInternalAppCRC(FLASH_APP_START, OTA_CB_Info.backup_size);
	if (crc != OTA_CB_Info.backup_crc32)
	{
		Serial_Printf("Current APP CRC mismatch, backup refused\r\n");
		return 0;
	}
	if (!OtaAuth_CalculateInternalImage(FLASH_APP_START,
		OTA_CB_Info.backup_size,
		OTA_CB_Info.backup_version,
		OTA_CB_Info.backup_auth_tag))
	{
		Serial_Printf("Current APP authentication calculation failed\r\n");
		return 0;
	}

	endAddr = OTA_CB_Info.backup_addr + OTA_CB_Info.backup_size;
	for (addr = OTA_CB_Info.backup_addr; addr < endAddr; addr += W25Q64_SECTOR_SIZE)
	{
		if (!W25Q64_SectorErase(addr))
		{
			return 0;
		}
	}

	while (offset < OTA_CB_Info.backup_size)
	{
		copySize = OTA_CB_Info.backup_size - offset;
		if (copySize > OTA_COPY_BUF_SIZE)
		{
			copySize = OTA_COPY_BUF_SIZE;
		}
		memcpy(OTA_CopyBuffer, (const void *)(FLASH_APP_START + offset), copySize);
		if (!W25Q64_PageProgram(
			OTA_CB_Info.backup_addr + offset,
			OTA_CopyBuffer,
			(uint16_t)copySize))
		{
			return 0;
		}
		offset += copySize;
	}

	crc = Boot_CalcExternalImageCRC(OTA_CB_Info.backup_addr, OTA_CB_Info.backup_size);
	if (crc != OTA_CB_Info.backup_crc32)
	{
		Serial_Printf("APP backup CRC mismatch\r\n");
		return 0;
	}
	if (!OtaAuth_VerifyExternalImage(OTA_CB_Info.backup_addr,
		OTA_CB_Info.backup_size,
		OTA_CB_Info.backup_version,
		OTA_CB_Info.backup_auth_tag))
	{
		Serial_Printf("APP backup authentication mismatch\r\n");
		return 0;
	}
	Serial_Printf("Current APP backup ready: version=%d size=%d\r\n",
		OTA_CB_Info.backup_version,
		OTA_CB_Info.backup_size);
	return 1;
}

/**
  * @brief  在片内APP尚未擦除时放弃坏升级，恢复旧APP元数据并继续运行旧版本。
  */
static uint8_t Boot_AbortBeforeAppErase(uint32_t errorCode)
{
	uint32_t crc;

	if ((OTA_CB_Info.backup_size > 0U) &&
		(OTA_CB_Info.backup_size <= FLASH_APP_SIZE) &&
		Boot_IsValidApp(FLASH_APP_START))
	{
		crc = Boot_CalcInternalAppCRC(FLASH_APP_START, OTA_CB_Info.backup_size);
		if (crc == OTA_CB_Info.backup_crc32)
		{
			OTA_CB_Info.app_version = OTA_CB_Info.backup_version;
			OTA_CB_Info.image_size = OTA_CB_Info.backup_size;
			OTA_CB_Info.image_crc32 = OTA_CB_Info.backup_crc32;
			OTA_CB_Info.image_addr = OTA_CB_Info.backup_addr;
			memcpy(OTA_CB_Info.image_auth_tag,
				OTA_CB_Info.backup_auth_tag,
				OTA_AUTH_TAG_SIZE);
			OTA_CB_Info.backup_version = 0U;
			OTA_CB_Info.backup_size = 0U;
			OTA_CB_Info.backup_crc32 = 0U;
			OTA_CB_Info.backup_addr = OTA_BACKUP_STORE_ADDR;
			memset(OTA_CB_Info.backup_auth_tag, 0, OTA_AUTH_TAG_SIZE);
			OTA_CB_Info.OTA_flag = OTA_STATE_DONE;
			OTA_CB_Info.error_code = errorCode;
			Serial_Printf("OTA aborted before APP erase, keep old version=%d\r\n",
				OTA_CB_Info.app_version);
			return Boot_SaveControlBlock();
		}
	}

	// 旧格式且新镜像在安装前就损坏：片内旧APP尚未动过，允许回到IDLE继续运行。
	if (Boot_IsValidApp(FLASH_APP_START))
	{
		memset(&OTA_CB_Info, 0, OTA_CB_T_SIZE);
		OTA_CB_Info.magic = OTA_CB_MAGIC;
		OTA_CB_Info.format_version = OTA_CB_FORMAT_VERSION;
		OTA_CB_Info.OTA_flag = OTA_STATE_IDLE;
		OTA_CB_Info.image_addr = OTA_IMAGE_STORE_ADDR;
		OTA_CB_Info.backup_addr = OTA_BACKUP_STORE_ADDR;
		OTA_CB_Info.error_code = errorCode;
		Serial_Printf("OTA aborted, keep existing APP without legacy metadata\r\n");
		return Boot_SaveControlBlock();
	}

	OTA_CB_Info.OTA_flag = OTA_STATE_ERROR;
	OTA_CB_Info.error_code = errorCode;
	Boot_SaveControlBlock();
	return 0;
}

/**
  * @brief  从 W25Q64 搬运固件到 STM32 片内 APP 分区。
  * @note   STM32F1 片内 Flash 最小写入单位是半字，也就是 2 字节。
  */
static uint8_t Boot_ProgramAppFromExternalFlash(uint32_t imageAddr, uint32_t imageSize)
{
	uint32_t offset = 0;
	uint32_t readSize;
	uint32_t i;
	uint32_t writeAddr;
	uint16_t halfWord;

	// 写 APP 前必须先擦除整个 APP 分区。
	MyFLASH_ErasePages(FLASH_APP_START, FLASH_PAGE_APP_NUM);

	while (offset < imageSize)
	{
		// 每次最多从 W25Q64 读 256 字节到 RAM 缓冲区。
		readSize = imageSize - offset;
		if (readSize > OTA_COPY_BUF_SIZE)
		{
			readSize = OTA_COPY_BUF_SIZE;
		}

		W25Q64_ReadData(imageAddr + offset, OTA_CopyBuffer, readSize);

		// 片内 Flash 按 2 字节写入，写完立刻读回校验。
		for (i = 0; i < readSize; i += 2)
		{
			if ((offset + i) >= imageSize)
			{
				break;
			}

			halfWord = OTA_CopyBuffer[i];
			if ((i + 1) < readSize)
			{
				halfWord |= ((uint16_t)OTA_CopyBuffer[i + 1] << 8);
			}
			else
			{
				// 固件长度是奇数时，最后 1 字节补 0xFF。
				halfWord |= 0xFF00U;
			}

			writeAddr = FLASH_APP_START + offset + i;
			MyFLASH_ProgramHalfWord(writeAddr, halfWord);

			if (MyFLASH_ReadHalfWord(writeAddr) != halfWord)
			{
				return 0;
			}
		}

		offset += readSize;
	}

	return 1;
}

/**
  * @brief  执行一次完整 OTA 升级。
  * @retval 1=升级成功，0=升级失败。
  *
  * 完整流程：
  * 1. 检查 EEPROM 控制块。
  * 2. 校验 W25Q64 中固件 CRC。
  * 3. 把当前APP备份到W25Q64独立槽，再标记OTA_STATE_UPDATING。
  * 4. 擦除片内APP区，并把新镜像写入APP区。
  * 5. 再校验片内APP CRC和中断向量表。
  * 6. 安装成功写TRIAL等待APP确认；破坏APP后的失败会自动恢复备份。
  */
static uint8_t Boot_PerformOTAUpdate(void)
{
	uint32_t crc;

	if (OTA_CB_Info.magic != OTA_CB_MAGIC)
	{
		Serial_Printf("OTA CB magic error\r\n");
		return 0;
	}

	if ((OTA_CB_Info.image_size == 0) || (OTA_CB_Info.image_size > FLASH_APP_SIZE))
	{
		Serial_Printf("OTA image size error: %d\r\n", OTA_CB_Info.image_size);
		if (OTA_CB_Info.OTA_flag == OTA_STATE_UPDATING)
		{
			return Boot_PerformRollback();
		}
		return Boot_AbortBeforeAppErase(OTA_ERR_IMAGE_SIZE);
	}

	Serial_Printf("OTA image size: %d\r\n", OTA_CB_Info.image_size);

	crc = Boot_CalcExternalImageCRC(OTA_CB_Info.image_addr, OTA_CB_Info.image_size);
	if (crc != OTA_CB_Info.image_crc32)
	{
		Serial_Printf("OTA external CRC error: 0x%08X\r\n", crc);
		if (OTA_CB_Info.OTA_flag == OTA_STATE_UPDATING)
		{
			return Boot_PerformRollback();
		}
		return Boot_AbortBeforeAppErase(OTA_ERR_EXTERNAL_CRC);
	}

	// CRC通过后再验证HMAC。该检查发生在APP擦除前，是Bootloader的最终信任边界。
	if (!OtaAuth_VerifyExternalImage(OTA_CB_Info.image_addr,
		OTA_CB_Info.image_size,
		OTA_CB_Info.app_version,
		OTA_CB_Info.image_auth_tag))
	{
		Serial_Printf("OTA firmware authentication failed\r\n");
		if (OTA_CB_Info.OTA_flag == OTA_STATE_UPDATING)
		{
			return Boot_PerformRollback();
		}
		return Boot_AbortBeforeAppErase(OTA_ERR_IMAGE_AUTH);
	}

	// 只有PENDING阶段制作备份。若备份时掉电，状态仍是PENDING，下次会从头重做。
	if ((OTA_CB_Info.OTA_flag == OTA_STATE_PENDING) && !Boot_BackupCurrentApp())
	{
		Serial_Printf("Current APP backup failed\r\n");
		return Boot_AbortBeforeAppErase(OTA_ERR_BACKUP);
	}

	OTA_CB_Info.OTA_flag = OTA_STATE_UPDATING;
	OTA_CB_Info.error_code = 0;
	if (!Boot_SaveControlBlock())
	{
		return 0;
	}

	if (!Boot_ProgramAppFromExternalFlash(OTA_CB_Info.image_addr, OTA_CB_Info.image_size))
	{
		Serial_Printf("OTA program APP error\r\n");
		OTA_CB_Info.error_code = OTA_ERR_PROGRAM_APP;
		return Boot_PerformRollback();
	}

	crc = Boot_CalcInternalAppCRC(FLASH_APP_START, OTA_CB_Info.image_size);
	if (crc != OTA_CB_Info.image_crc32)
	{
		Serial_Printf("OTA internal CRC error: 0x%08X\r\n", crc);
		OTA_CB_Info.error_code = OTA_ERR_INTERNAL_CRC;
		return Boot_PerformRollback();
	}

	if (!Boot_IsValidApp(FLASH_APP_START))
	{
		Serial_Printf("OTA APP vector error\r\n");
		OTA_CB_Info.error_code = OTA_ERR_APP_VECTOR;
		return Boot_PerformRollback();
	}

	// 安装成功不等于业务健康。先进入TRIAL，APP运行到确认点后才会改成DONE。
	OTA_CB_Info.OTA_flag = OTA_STATE_TRIAL;
	OTA_CB_Info.trial_boot_count = 0U;
	OTA_CB_Info.error_code = 0;
	if (!Boot_SaveControlBlock())
	{
		return 0;
	}

	Serial_Printf("OTA installed, waiting for APP confirmation\r\n");
	return 1;
}

/**
  * @brief  用W25Q64备份槽中的旧镜像恢复片内APP。
  * @note   擦除片内Flash前先提交ROLLBACK；恢复中掉电后，下次上电会再次执行。
  */
static uint8_t Boot_PerformRollback(void)
{
	uint32_t crc;
	uint32_t rollbackReason = OTA_CB_Info.error_code;

	if (rollbackReason == OTA_ERR_NONE)
	{
		rollbackReason = OTA_ERR_TRIAL_FAILED;
	}

	if ((OTA_CB_Info.backup_size == 0U) ||
		(OTA_CB_Info.backup_size > FLASH_APP_SIZE) ||
		(OTA_CB_Info.backup_addr != OTA_BACKUP_STORE_ADDR))
	{
		Serial_Printf("No valid rollback metadata\r\n");
		OTA_CB_Info.OTA_flag = OTA_STATE_ERROR;
		OTA_CB_Info.error_code = OTA_ERR_ROLLBACK;
		Boot_SaveControlBlock();
		return 0;
	}

	crc = Boot_CalcExternalImageCRC(OTA_CB_Info.backup_addr, OTA_CB_Info.backup_size);
	if (crc != OTA_CB_Info.backup_crc32)
	{
		Serial_Printf("Rollback image CRC error\r\n");
		OTA_CB_Info.OTA_flag = OTA_STATE_ERROR;
		OTA_CB_Info.error_code = OTA_ERR_ROLLBACK;
		Boot_SaveControlBlock();
		return 0;
	}
	if (!OtaAuth_VerifyExternalImage(OTA_CB_Info.backup_addr,
		OTA_CB_Info.backup_size,
		OTA_CB_Info.backup_version,
		OTA_CB_Info.backup_auth_tag))
	{
		Serial_Printf("Rollback image authentication error\r\n");
		OTA_CB_Info.OTA_flag = OTA_STATE_ERROR;
		OTA_CB_Info.error_code = OTA_ERR_ROLLBACK;
		Boot_SaveControlBlock();
		return 0;
	}

	if (OTA_CB_Info.OTA_flag != OTA_STATE_ROLLBACK)
	{
		OTA_CB_Info.OTA_flag = OTA_STATE_ROLLBACK;
		OTA_CB_Info.error_code = rollbackReason;
		if (!Boot_SaveControlBlock())
		{
			return 0;
		}
	}

	if (!Boot_ProgramAppFromExternalFlash(
		OTA_CB_Info.backup_addr,
		OTA_CB_Info.backup_size))
	{
		OTA_CB_Info.OTA_flag = OTA_STATE_ERROR;
		OTA_CB_Info.error_code = OTA_ERR_ROLLBACK;
		Boot_SaveControlBlock();
		return 0;
	}

	crc = Boot_CalcInternalAppCRC(FLASH_APP_START, OTA_CB_Info.backup_size);
	if ((crc != OTA_CB_Info.backup_crc32) || !Boot_IsValidApp(FLASH_APP_START))
	{
		OTA_CB_Info.OTA_flag = OTA_STATE_ERROR;
		OTA_CB_Info.error_code = OTA_ERR_ROLLBACK;
		Boot_SaveControlBlock();
		return 0;
	}

	OTA_CB_Info.app_version = OTA_CB_Info.backup_version;
	OTA_CB_Info.image_size = OTA_CB_Info.backup_size;
	OTA_CB_Info.image_crc32 = OTA_CB_Info.backup_crc32;
	OTA_CB_Info.image_addr = OTA_CB_Info.backup_addr;
	memcpy(OTA_CB_Info.image_auth_tag,
		OTA_CB_Info.backup_auth_tag,
		OTA_AUTH_TAG_SIZE);
	OTA_CB_Info.backup_version = 0U;
	OTA_CB_Info.backup_size = 0U;
	OTA_CB_Info.backup_crc32 = 0U;
	OTA_CB_Info.backup_addr = OTA_BACKUP_STORE_ADDR;
	memset(OTA_CB_Info.backup_auth_tag, 0, OTA_AUTH_TAG_SIZE);
	OTA_CB_Info.trial_boot_count = 0U;
	OTA_CB_Info.OTA_flag = OTA_STATE_DONE;
	OTA_CB_Info.error_code = rollbackReason;
	if (!Boot_SaveControlBlock())
	{
		return 0;
	}

	Serial_Printf("Rollback success, restored version=%d\r\n", OTA_CB_Info.app_version);
	return 1;
}

/**
  * @brief  处理未确认APP的重复启动计数，达到阈值时执行回滚。
  * @retval 1=仍允许本次试运行或回滚成功，0=无法继续安全启动。
  */
static uint8_t Boot_HandleTrialState(void)
{
	OTA_CB_Info.trial_boot_count++;
	Serial_Printf("APP trial boot %d/%d\r\n",
		OTA_CB_Info.trial_boot_count,
		OTA_TRIAL_BOOT_LIMIT);

	if (OTA_CB_Info.trial_boot_count >= OTA_TRIAL_BOOT_LIMIT)
	{
		Serial_Printf("APP was not confirmed, start rollback\r\n");
		return Boot_PerformRollback();
	}

	if (!Boot_SaveControlBlock())
	{
		return 0;
	}
	return 1;
}

/**
  * @brief  判断 APP 分区里是否有一个看起来合法的程序。
  * @note   Cortex-M 向量表第 1 个 word 是栈顶，第 2 个 word 是 Reset_Handler。
  */
uint8_t Boot_IsValidApp(uint32_t appAddress)
{
	uint32_t appStack;
	uint32_t appResetHandler;
	uint32_t appResetAddress;

	appStack = *(__IO uint32_t *)appAddress;
	appResetHandler = *(__IO uint32_t *)(appAddress + 4);

	if ((appStack < 0x20000000UL) || (appStack > 0x20005000UL))
	{
		return 0;
	}

	// Cortex-M 函数入口地址最低位必须为 1，表示使用 Thumb 指令集。
	if ((appResetHandler & 1U) == 0U)
	{
		return 0;
	}

	appResetAddress = appResetHandler & ~1UL;
	if ((appResetAddress < FLASH_APP_START) || (appResetAddress >= FLASH_END_ADDR))
	{
		return 0;
	}

	return 1;
}

/**
  * @brief  跳 APP 前关闭 Bootloader 用到的外设和中断。
  * @note   如果不关 DMA/USART/SysTick，APP 启动后可能被 Bootloader 的中断打断。
  */
static void Boot_DeInitBeforeJump(void)
{
	uint8_t index;

	USART_ITConfig(USART1, USART_IT_IDLE, DISABLE);
	USART_DMACmd(USART1, USART_DMAReq_Rx, DISABLE);
	DMA_Cmd(DMA1_Channel5, DISABLE);
	USART_Cmd(USART1, DISABLE);

	SysTick->CTRL = 0;
	SysTick->LOAD = 0;
	SysTick->VAL = 0;

	// 先全局关中断，再清理Bootloader在NVIC中留下的使能位和挂起位。
	// Cortex-M3最多提供8组32位NVIC寄存器；未实现的位写1不会产生副作用。
	// 这样APP接管后不会突然进入Bootloader曾经启用过的USART/DMA中断。
	__disable_irq();
	for (index = 0; index < 8U; index++)
	{
		NVIC->ICER[index] = 0xFFFFFFFFUL;
		NVIC->ICPR[index] = 0xFFFFFFFFUL;
	}

	// 清除Cortex-M内核可能挂起的SysTick和PendSV异常。
	SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;
}

/**
  * @brief  跳转到 APP。
  * @param  appAddress: APP 向量表起始地址，本工程为 0x08005000。
  */
void Boot_JumpToApp(uint32_t appAddress)
{
	uint32_t jumpAddress;
	BootJumpFunc_t jumpToApp;

	if (!Boot_IsValidApp(appAddress))
	{
		Serial_Printf("No valid APP at 0x%08X\r\n", appAddress);
		return;
	}

	Serial_Printf("Jump to APP: 0x%08X\r\n", appAddress);

	jumpAddress = *(__IO uint32_t *)(appAddress + 4);
	jumpToApp = (BootJumpFunc_t)jumpAddress;

	Boot_DeInitBeforeJump();
	SCB->VTOR = appAddress;
	__set_MSP(*(__IO uint32_t *)appAddress);

	// 确保新的向量表和主栈已经生效，再恢复全局中断。
	// NVIC使能位已经清零，因此此时不会在APP初始化完成前误入外设中断；
	// APP随后可以正常使用SysTick、串口、定时器等中断功能。
	__DSB();
	__ISB();
	__enable_irq();
	jumpToApp();
}

/**
  * @brief  Bootloader 主分支。
  *
  * 上电后的决策：
  * - 如果 EEPROM 标记为 OTA_STATE_PENDING，说明 W25Q64 中有新固件，先执行升级。
  * - 升级成功后跳 APP。
  * - 没有升级任务时，如果 APP 合法就直接跳 APP。
  * - 如果没有合法 APP，就留在 Bootloader，等待后续串口升级功能。
  */
void BootLoader_Branch(void)
{
	uint8_t installedNow = 0U;

	if ((OTA_CB_Info.OTA_flag == OTA_STATE_PENDING) ||
		(OTA_CB_Info.OTA_flag == OTA_STATE_UPDATING))
	{
		Serial_Printf("OTA update pending\r\n");
		if (!Boot_PerformOTAUpdate())
		{
			// APP 区可能已经被擦除或只写入了一部分，不能只看向量表就继续跳转。
			Serial_Printf("OTA update failed, stay in Bootloader\r\n");
			return;
		}
		installedNow = 1U;
	}

	if (OTA_CB_Info.OTA_flag == OTA_STATE_ROLLBACK)
	{
		Serial_Printf("Resume interrupted rollback\r\n");
		if (!Boot_PerformRollback())
		{
			return;
		}
	}

	// 刚安装完成时直接给新APP第一次试运行机会，不在同一次启动里增加失败计数。
	if ((OTA_CB_Info.OTA_flag == OTA_STATE_TRIAL) && !installedNow)
	{
		if (!Boot_HandleTrialState())
		{
			return;
		}
	}

	if (Boot_IsValidApp(FLASH_APP_START))
	{
		Boot_JumpToApp(FLASH_APP_START);
	}
	else
	{
		Serial_Printf("Stay in Bootloader\r\n");
	}
}

void BootLoader_Brance(void)
{
	BootLoader_Branch();
}
