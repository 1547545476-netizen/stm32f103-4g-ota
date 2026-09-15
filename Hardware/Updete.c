#include "Updete.h"                    // 升级模块对外接口和协议命令定义。
#include "Serial.h"                    // 串口打印、USART1 DMA 接收队列。
#include "W25Q64.h"                    // 外部 SPI Flash 读、写、擦除接口。
#include "AT24C02.h"                   // EEPROM 读写接口，用来保存 OTA 控制块。
#include "main.h"                      // Flash 分区、OTA 控制块结构体和全局变量。
#include "Delay.h"                     // 自动复位前的短延时。
#include "OtaAuth.h"                   // HMAC-SHA256固件真实性校验。
#include <string.h>                    // memset()。

// ==================== 升级协议格式 ====================
// 一帧升级包格式：
// byte0      : 0x55，帧头第 1 字节。
// byte1      : 0xAA，帧头第 2 字节。
// byte2      : cmd，命令类型，见 Updete.h 中的 UPDATE_CMD_xxx。
// byte3~4    : seq，包序号，小端序，主要用于 ACK/ERR 打印和上位机调试。
// byte5~8    : offset，小端序；DATA 命令中表示当前 payload 应写入固件的偏移地址。
// byte9~10   : payload_len，小端序；表示 payload 字节数。
// byte11...  : payload，命令携带的数据。
// last 2byte : crc16，小端序；计算范围为 cmd 到 payload 结束，不包含 0x55 0xAA。
//
// START payload 固定 32 字节：
// version(4) + image_size(4) + image_crc32(4) + image_addr(4) + auth_tag(16)
//
// DATA payload 是固件数据：
// 当前版本要求 offset 必须等于已经接收的字节数，也就是必须顺序发送。
//
// END payload 长度必须为 0：
// 收到 END 后，Bootloader 会从 W25Q64 读回完整固件计算 CRC32，
// 校验通过后把 OTA 控制块写入 AT24C02，等待下次复位执行搬运。

#define UPDATE_HEAD_1           0x55U   // 协议帧头第 1 字节。
#define UPDATE_HEAD_2           0xAAU   // 协议帧头第 2 字节。
#define UPDATE_START_PAYLOAD    32U     // START元数据16字节 + HMAC认证标签16字节。
#define UPDATE_FRAME_BASE_LEN   13U     // 帧头 2 + 固定字段 9 + CRC16 2 = 13 字节。
#define UPDATE_FRAME_MAX_LEN    (UPDATE_FRAME_BASE_LEN + UPDATE_MAX_PAYLOAD)
// ==================== 模块内部状态 ====================
typedef enum {
	UPDATE_STATE_IDLE = 0,       // 空闲状态：还没有收到 START 包。
	UPDATE_STATE_RECEIVING,      // 接收状态：已收到 START，正在接收 DATA 包。
} UpdateState_t;

typedef struct {
	UpdateSource_t source;        // 当前升级数据来源：串口或预留 4G。
	UpdateState_t state;          // 当前接收状态。
	uint32_t version;             // START 包中的固件版本号。
	uint32_t imageSize;           // START 包中的固件总大小，单位字节。
	uint32_t imageCrc32;          // START 包中的完整固件 CRC32。
	uint32_t imageAddr;           // 固件暂存在 W25Q64 中的起始地址。
	uint8_t imageAuthTag[OTA_AUTH_TAG_SIZE]; // 发布工具计算的128位HMAC认证标签。
	uint32_t receivedSize;        // 当前已经收到并写入 W25Q64 的固件字节数。
	uint16_t lastSeq;             // 最近处理过的包序号，方便调试观察。
} UpdateContext_t;

static UpdateContext_t UpdateCtx;                         // 升级接收上下文。
static uint8_t UpdateFrameBuf[UPDATE_FRAME_MAX_LEN];      // 协议帧解析缓存。
static uint16_t UpdateFrameLen;                           // 当前已经缓存的帧长度。
static uint16_t UpdateExpectedLen;                        // 根据 payload_len 计算出的完整帧长度。

/**
  * @brief  从小端字节数组读取 uint16_t。
  * @param  data: 指向 2 字节小端数据。
  * @retval 解析出的 16 位无符号整数。
  */
static uint16_t Update_ReadLE16(const uint8_t *data)
{
	return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

/**
  * @brief  从小端字节数组读取 uint32_t。
  * @param  data: 指向 4 字节小端数据。
  * @retval 解析出的 32 位无符号整数。
  */
static uint32_t Update_ReadLE32(const uint8_t *data)
{
	return (uint32_t)data[0] |
		   ((uint32_t)data[1] << 8) |
		   ((uint32_t)data[2] << 16) |
		   ((uint32_t)data[3] << 24);
}

/**
  * @brief  计算协议帧 CRC16。
  * @param  data: 待校验数据起始地址。
  * @param  len : 待校验数据长度。
  * @retval CRC16 结果。
  * @note   使用 Modbus 常见多项式 0xA001，初始值 0xFFFF。
  */
static uint16_t Update_CRC16(const uint8_t *data, uint16_t len)
{
	uint16_t crc = 0xFFFF;
	uint16_t i;
	uint8_t bit;

	for (i = 0; i < len; i++)
	{
		crc ^= data[i];                        // 当前字节先异或进 CRC 低 8 位。
		for (bit = 0; bit < 8; bit++)
		{
			if (crc & 0x0001U)                 // 最低位为 1 时右移后异或多项式。
			{
				crc = (crc >> 1) ^ 0xA001U;
			}
			else                               // 最低位为 0 时只右移。
			{
				crc >>= 1;
			}
		}
	}

	return crc;
}

/**
  * @brief  增量计算 CRC32。
  * @param  crc : 上一次 CRC 中间值。
  * @param  data: 新增数据起始地址。
  * @param  len : 新增数据长度。
  * @retval 更新后的 CRC 中间值。
  * @note   外部调用时一般初始值传 0xFFFFFFFF，全部数据算完后再异或 0xFFFFFFFF。
  */
static uint32_t Update_CRC32Update(uint32_t crc, const uint8_t *data, uint32_t len)
{
	uint32_t i;
	uint8_t bit;

	for (i = 0; i < len; i++)
	{
		crc ^= data[i];                        // 把当前字节并入 CRC。
		for (bit = 0; bit < 8; bit++)
		{
			if (crc & 1U)                      // 低位为 1 时异或 CRC32 多项式。
			{
				crc = (crc >> 1) ^ 0xEDB88320UL;
			}
			else                               // 低位为 0 时只右移。
			{
				crc >>= 1;
			}
		}
	}

	return crc;
}

/**
  * @brief  发送升级 ACK 文本。
  * @param  cmd: 已成功处理的命令。
  * @param  seq: 已成功处理的包序号。
  * @note   现在先用串口文本应答，后续上位机可以根据这行文本判断是否继续发下一包。
  */
static void Update_SendAck(uint8_t cmd, uint16_t seq)
{
	Serial_Printf("UPD ACK cmd=%d seq=%d\r\n", cmd, seq);
}

/**
  * @brief  发送升级错误文本。
  * @param  cmd  : 出错命令。
  * @param  seq  : 出错包序号。
  * @param  error: 模块内部错误码，便于串口调试。
  */
static void Update_SendError(uint8_t cmd, uint16_t seq, uint8_t error)
{
	Serial_Printf("UPD ERR cmd=%d seq=%d err=%d\r\n", cmd, seq, error);
}

/**
  * @brief  清空协议解析器状态。
  * @note   遇到错误帧、超长帧、处理完完整帧后都会调用。
  */
static void Update_ResetParser(void)
{
	UpdateFrameLen = 0;
	UpdateExpectedLen = 0;
}

/**
  * @brief  擦除 W25Q64 中用于暂存固件的区域。
  * @param  imageAddr: 固件暂存起始地址。
  * @param  imageSize: 固件总大小。
  * @note   W25Q64 必须先擦后写，这里按 4KB 扇区擦除覆盖整个固件区域。
  */
static uint8_t Update_EraseImageArea(uint32_t imageAddr, uint32_t imageSize)
{
	uint32_t addr;
	uint32_t endAddr;

	endAddr = imageAddr + imageSize;                         // 计算擦除结束地址，前面 START 已做越界检查。
	for (addr = imageAddr; addr < endAddr; addr += W25Q64_SECTOR_SIZE)
	{
		if (!W25Q64_SectorErase(addr))                       // 每次擦除一个 4KB 扇区。
		{
			return 0;
		}
	}
	return 1;
}

/**
  * @brief  把一段数据写入 W25Q64。
  * @param  addr: W25Q64 写入起始地址。
  * @param  data: 待写入数据。
  * @param  len : 待写入长度。
  * @retval 1=写入流程完成。
  * @note   W25Q64 页编程不能跨 256 字节页，本函数会自动按页边界拆开写。
  */
static uint8_t Update_WriteW25Q64(uint32_t addr, uint8_t *data, uint16_t len)
{
	uint16_t writeLen;
	uint16_t pageRemain;

	while (len > 0)
	{
		pageRemain = (uint16_t)(W25Q64_PAGE_SIZE - (addr % W25Q64_PAGE_SIZE)); // 当前页剩余可写空间。
		writeLen = len;                                                       // 默认本次写完剩余全部数据。
		if (writeLen > pageRemain)
		{
			writeLen = pageRemain;                                             // 如果会跨页，就只写到本页末尾。
		}

		if (!W25Q64_PageProgram(addr, data, writeLen))                         // 写入当前页内的一段数据。
		{
			return 0;
		}

		addr += writeLen;                                                      // 移动 W25Q64 地址。
		data += writeLen;                                                      // 移动 RAM 数据指针。
		len -= writeLen;                                                       // 扣除已经写入的字节数。
	}

	return 1;
}

/**
  * @brief  从 W25Q64 读回暂存固件并计算 CRC32。
  * @retval W25Q64 中完整固件的 CRC32。
  * @note   END 阶段使用它确认外部 Flash 里保存的固件是完整且正确的。
  */
static uint32_t Update_CalcStoredImageCRC(void)
{
	uint32_t offset = 0;
	uint32_t readLen;
	uint32_t crc = 0xFFFFFFFFUL;
	uint8_t buf[UPDATE_MAX_PAYLOAD];

	while (offset < UpdateCtx.imageSize)
	{
		readLen = UpdateCtx.imageSize - offset;                // 剩余未校验长度。
		if (readLen > UPDATE_MAX_PAYLOAD)
		{
			readLen = UPDATE_MAX_PAYLOAD;                      // 分块读取，避免 RAM 占用过大。
		}

		W25Q64_ReadData(UpdateCtx.imageAddr + offset, buf, readLen); // 从 W25Q64 读一块数据。
		crc = Update_CRC32Update(crc, buf, readLen);                 // 把这一块加入 CRC32。
		offset += readLen;                                           // 继续校验下一块。
	}

	return crc ^ 0xFFFFFFFFUL;                                // CRC32 最终异或输出。
}

/**
  * @brief  处理 START 命令。
  * @param  seq    : START 包序号。
  * @param  payload: START 包 payload。
  * @param  len    : START 包 payload 长度。
  * @retval 1=处理成功，0=处理失败。
  */
static uint8_t Update_HandleStart(uint16_t seq, uint8_t *payload, uint16_t len)
{
	// START包携带4个uint32_t元数据和16字节HMAC认证标签。
	if (len != UPDATE_START_PAYLOAD)
	{
		Update_SendError(UPDATE_CMD_START, seq, 1);
		return 0;
	}

	// 协议按小端序传输。虽然 STM32F103 也是小端，显式解析能避免移植时出错。
	UpdateCtx.version = Update_ReadLE32(&payload[0]);
	UpdateCtx.imageSize = Update_ReadLE32(&payload[4]);
	UpdateCtx.imageCrc32 = Update_ReadLE32(&payload[8]);
	UpdateCtx.imageAddr = Update_ReadLE32(&payload[12]);
	memcpy(UpdateCtx.imageAuthTag, &payload[16], OTA_AUTH_TAG_SIZE);
	if (!OtaAuth_IsTagPresent(UpdateCtx.imageAuthTag))
	{
		Update_SendError(UPDATE_CMD_START, seq, 6);
		return 0;
	}

	// 固件大小不能为 0，也不能超过片内 APP 分区，否则后续搬运会越界。
	if ((UpdateCtx.imageSize == 0) || (UpdateCtx.imageSize > FLASH_APP_SIZE))
	{
		Update_SendError(UPDATE_CMD_START, seq, 2);
		return 0;
	}

	// 固件暂存在 W25Q64，所以还要检查外部 Flash 地址范围。
	// 写成减法形式是为了避免 imageAddr + imageSize 发生 32 位整数溢出。
	if ((UpdateCtx.imageAddr >= W25Q64_TOTAL_SIZE) ||
		(UpdateCtx.imageSize > (W25Q64_TOTAL_SIZE - UpdateCtx.imageAddr)))
	{
		Update_SendError(UPDATE_CMD_START, seq, 3);
		return 0;
	}

	// 固件槽按 4KB 扇区对齐，避免擦除首尾扇区时误伤同扇区里的其他数据。
	if ((UpdateCtx.imageAddr % W25Q64_SECTOR_SIZE) != 0U)
	{
		Update_SendError(UPDATE_CMD_START, seq, 4);
		return 0;
	}

	Serial_Printf("UPD START size=%d crc=0x%08X\r\n", UpdateCtx.imageSize, UpdateCtx.imageCrc32);

	// 写 W25Q64 前必须先擦除。这里按 4KB 扇区擦除覆盖整个固件暂存区域。
	if (!Update_EraseImageArea(UpdateCtx.imageAddr, UpdateCtx.imageSize))
	{
		Update_SendError(UPDATE_CMD_START, seq, 5);
		return 0;
	}

	// 清零接收进度，并进入接收状态；后续 DATA 包必须从 offset=0 开始顺序写入。
	UpdateCtx.receivedSize = 0;
	UpdateCtx.lastSeq = seq;
	UpdateCtx.state = UPDATE_STATE_RECEIVING;
	Update_SendAck(UPDATE_CMD_START, seq);

	return 1;
}

/**
  * @brief  处理 DATA 命令。
  * @param  seq    : DATA 包序号。
  * @param  offset : 当前 payload 对应的固件偏移。
  * @param  payload: 固件数据。
  * @param  len    : 固件数据长度。
  * @retval 1=处理成功，0=处理失败。
  */
static uint8_t Update_HandleData(uint16_t seq, uint32_t offset, uint8_t *payload, uint16_t len)
{
	// 没收到 START 前，不允许直接写 DATA，避免误把普通串口数据写入 W25Q64。
	if (UpdateCtx.state != UPDATE_STATE_RECEIVING)
	{
		Update_SendError(UPDATE_CMD_DATA, seq, 1);
		return 0;
	}

	// DATA 包 payload 不能为空，否则没有实际固件数据。
	if (len == 0)
	{
		Update_SendError(UPDATE_CMD_DATA, seq, 2);
		return 0;
	}

	// 第一版先要求顺序传输，逻辑最简单，也方便你调试。
	// 后续 4G 如果有乱序重传需求，可以扩展成按 offset 随机写。
	if (offset != UpdateCtx.receivedSize)
	{
		Update_SendError(UPDATE_CMD_DATA, seq, 3);
		return 0;
	}

	// 当前包不能写出 START 声明的固件大小范围。
	if ((offset + len) > UpdateCtx.imageSize)
	{
		Update_SendError(UPDATE_CMD_DATA, seq, 4);
		return 0;
	}

	// W25Q64 页编程不能跨 256 字节页，Update_WriteW25Q64() 内部会自动拆页。
	if (!Update_WriteW25Q64(UpdateCtx.imageAddr + offset, payload, len))
	{
		Update_SendError(UPDATE_CMD_DATA, seq, 5);
		return 0;
	}

	// 记录已接收大小，用于下一包 offset 校验和 END 完整性判断。
	UpdateCtx.receivedSize += len;
	UpdateCtx.lastSeq = seq;
	Update_SendAck(UPDATE_CMD_DATA, seq);

	return 1;
}

/**
  * @brief  处理 END 命令。
  * @param  seq: END 包序号。
  * @retval 1=处理成功，0=处理失败。
  */
static uint8_t Update_HandleEnd(uint16_t seq)
{
	uint32_t crc;
	uint32_t previousVersion = 0U;
	uint32_t previousSize = 0U;
	uint32_t previousCrc32 = 0U;
	uint8_t previousAuthTag[OTA_AUTH_TAG_SIZE];

	memset(previousAuthTag, 0, sizeof(previousAuthTag));

	// END 只能出现在 START 和若干 DATA 之后。
	if (UpdateCtx.state != UPDATE_STATE_RECEIVING)
	{
		Update_SendError(UPDATE_CMD_END, seq, 1);
		return 0;
	}

	// 收到的总字节数必须和 START 里声明的固件大小一致。
	if (UpdateCtx.receivedSize != UpdateCtx.imageSize)
	{
		Update_SendError(UPDATE_CMD_END, seq, 2);
		return 0;
	}

	// 再从 W25Q64 读回整包固件计算 CRC，确认外部 Flash 里的数据没有错。
	crc = Update_CalcStoredImageCRC();
	if (crc != UpdateCtx.imageCrc32)
	{
		Serial_Printf("UPD CRC ERR calc=0x%08X target=0x%08X\r\n", crc, UpdateCtx.imageCrc32);
		Update_SendError(UPDATE_CMD_END, seq, 3);
		return 0;
	}

	// CRC正确只说明数据没有随机损坏；HMAC还要确认固件确实由可信发布工具生成。
	if (!OtaAuth_VerifyExternalImage(UpdateCtx.imageAddr,
		UpdateCtx.imageSize,
		UpdateCtx.version,
		UpdateCtx.imageAuthTag))
	{
		Serial_Printf("UPD AUTH ERR\r\n");
		Update_SendError(UPDATE_CMD_END, seq, 4);
		return 0;
	}

	// 校验通过后，写 OTA 控制块。
	// 下次复位进入 Bootloader 时，Bootloader 会根据 PENDING 状态执行搬运。
	// 串口升级同样保留当前已确认APP的元数据，Boot安装前才能制作回滚备份。
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
	OTA_CB_Info.app_version = UpdateCtx.version;
	OTA_CB_Info.image_size = UpdateCtx.imageSize;
	OTA_CB_Info.image_crc32 = UpdateCtx.imageCrc32;
	OTA_CB_Info.image_addr = UpdateCtx.imageAddr;
	memcpy(OTA_CB_Info.image_auth_tag, UpdateCtx.imageAuthTag, OTA_AUTH_TAG_SIZE);
	OTA_CB_Info.backup_version = previousVersion;
	OTA_CB_Info.backup_size = previousSize;
	OTA_CB_Info.backup_crc32 = previousCrc32;
	OTA_CB_Info.backup_addr = OTA_BACKUP_STORE_ADDR;
	memcpy(OTA_CB_Info.backup_auth_tag, previousAuthTag, OTA_AUTH_TAG_SIZE);
	OTA_CB_Info.error_code = OTA_ERR_NONE;

	if (!AT24C02_WriteOTA_CB_Info(&OTA_CB_Info))
	{
		Update_SendError(UPDATE_CMD_END, seq, 5);
		return 0;
	}

	UpdateCtx.state = UPDATE_STATE_IDLE;
	Update_SendAck(UPDATE_CMD_END, seq);
	Serial_Printf("UPD READY, reset MCU to update APP\r\n");

#if UPDATE_AUTO_RESET_AFTER_END
	Delay_ms(100);                                           // 给串口 ACK 留一点发送时间。
	NVIC_SystemReset();                                      // 自动复位后进入 Bootloader 搬运 APP。
#endif

	return 1;
}

/**
  * @brief  处理一帧已经收完整的升级协议帧。
  * @param  frame   : 完整协议帧缓存。
  * @param  frameLen: 完整协议帧长度。
  */
static void Update_HandleFrame(uint8_t *frame, uint16_t frameLen)
{
	uint8_t cmd;
	uint16_t seq;
	uint32_t offset;
	uint16_t payloadLen;
	uint16_t recvCrc;
	uint16_t calcCrc;
	uint8_t *payload;

	cmd = frame[2];                                           // 取命令字。
	seq = Update_ReadLE16(&frame[3]);                         // 取包序号。
	offset = Update_ReadLE32(&frame[5]);                       // 取固件偏移。
	payloadLen = Update_ReadLE16(&frame[9]);                   // 取 payload 长度。
	payload = &frame[11];                                      // payload 从第 11 字节开始。

	// 二次确认帧长度，防止后续有人直接调用 Update_HandleFrame() 时绕过解析器检查。
	if (frameLen != (uint16_t)(UPDATE_FRAME_BASE_LEN + payloadLen))
	{
		Update_SendError(cmd, seq, 0xED);
		return;
	}

	// 取出上位机发送的 CRC16，并重新计算本地 CRC16。
	recvCrc = Update_ReadLE16(&frame[11 + payloadLen]);
	calcCrc = Update_CRC16(&frame[2], (uint16_t)(9 + payloadLen));

	if (recvCrc != calcCrc)
	{
		Update_SendError(cmd, seq, 0xEE);                      // CRC 错误，直接丢弃这一帧。
		return;
	}

	switch (cmd)
	{
		case UPDATE_CMD_START:
			Update_HandleStart(seq, payload, payloadLen);       // 开始升级。
			break;

		case UPDATE_CMD_DATA:
			Update_HandleData(seq, offset, payload, payloadLen); // 写入一段固件数据。
			break;

		case UPDATE_CMD_END:
			if (payloadLen != 0)                                // END 包不应携带 payload。
			{
				Update_SendError(cmd, seq, 0xEC);
				break;
			}
			Update_HandleEnd(seq);                              // 结束接收并写 OTA 控制块。
			break;

		case UPDATE_CMD_ABORT:
			UpdateCtx.state = UPDATE_STATE_IDLE;                // 放弃当前升级，回到空闲状态。
			Update_SendAck(UPDATE_CMD_ABORT, seq);
			break;

		default:
			Update_SendError(cmd, seq, 0xEF);                    // 未知命令。
			break;
	}
}

/**
  * @brief  协议解析器：每次喂入 1 字节，自动拼出完整帧。
  * @param  byte: 串口或 4G 收到的一个原始字节。
  * @note   这种逐字节解析方式适合串口、4G、TCP 等流式数据。
  */
static void Update_ParseByte(uint8_t byte)
{
	uint16_t payloadLen;

	if (UpdateFrameLen == 0)
	{
		if (byte == UPDATE_HEAD_1)                              // 等待帧头 0x55。
		{
			UpdateFrameBuf[UpdateFrameLen++] = byte;
		}
		return;
	}

	if (UpdateFrameLen == 1)
	{
		if (byte == UPDATE_HEAD_2)                              // 等待帧头 0xAA。
		{
			UpdateFrameBuf[UpdateFrameLen++] = byte;
		}
		else
		{
			// 如果当前字节仍是 0x55，就把它保留为下一帧的第一个帧头。
			UpdateFrameBuf[0] = byte;
			UpdateFrameLen = (byte == UPDATE_HEAD_1) ? 1U : 0U;
		}
		return;
	}

	if (UpdateFrameLen >= UPDATE_FRAME_MAX_LEN)
	{
		Update_ResetParser();                                    // 帧太长，说明数据异常，直接复位解析器。
		return;
	}

	UpdateFrameBuf[UpdateFrameLen++] = byte;                     // 保存当前字节。

	if (UpdateFrameLen == 11)
	{
		payloadLen = Update_ReadLE16(&UpdateFrameBuf[9]);         // 收到 payload_len 字段后就能算完整帧长。
		if (payloadLen > UPDATE_MAX_PAYLOAD)
		{
			Update_ResetParser();                                // payload 超过单包最大值，丢弃。
			return;
		}
		UpdateExpectedLen = (uint16_t)(UPDATE_FRAME_BASE_LEN + payloadLen);
	}

	if ((UpdateExpectedLen != 0) && (UpdateFrameLen >= UpdateExpectedLen))
	{
		Update_HandleFrame(UpdateFrameBuf, UpdateFrameLen);       // 收齐完整帧，进入命令处理。
		Update_ResetParser();                                    // 无论成功失败，都准备接收下一帧。
	}
}

/**
  * @brief  从 USART1 DMA 接收队列取数据并交给协议解析器。
  * @note   中断里只登记数据范围，真正解析、擦写 Flash 都放在主循环里做。
  */
static void Update_PollSerial(void)
{
	uint8_t *start;
	uint8_t *end;
	uint16_t len;

	// USART1_IRQHandler() 只负责把 DMA 收到的一帧登记到队列。
	// 主循环在这里消费队列，避免在中断里做 CRC、擦 Flash 这种耗时操作。
	while (U1CB.URxDataOUT != U1CB.URxDataIN)
	{
		start = U1CB.URxDataOUT->start;                         // 当前待处理数据起始地址。
		end = U1CB.URxDataOUT->end;                             // 当前待处理数据结束地址。

		// 当前 DMA 描述符有效时，把这一段原始串口数据送进协议解析器。
		if ((start != 0) && (end != 0) && (end >= start))
		{
			len = (uint16_t)(end - start + 1);                   // 计算本段数据长度。
			Update_InputData(start, len);                        // 逐字节解析协议帧。
		}

		// 消费完一个描述符后，移动读指针；走到队列末尾就回卷到数组头部。
		U1CB.URxDataOUT++;
		if (U1CB.URxDataOUT > U1CB.URxDataEND)
		{
			U1CB.URxDataOUT = &U1CB.pU1RxData[0];
		}
	}
}

/**
  * @brief  初始化升级接收模块。
  * @param  source: 升级数据来源，当前已实现 UPDATE_SOURCE_SERIAL。
  */
void Update_Init(UpdateSource_t source)
{
	memset(&UpdateCtx, 0, sizeof(UpdateCtx));                 // 清空升级上下文。
	UpdateCtx.source = source;                                // 记录当前数据来源。
	UpdateCtx.state = UPDATE_STATE_IDLE;                      // 初始为空闲状态。
	UpdateCtx.imageAddr = OTA_IMAGE_STORE_ADDR;               // 默认外部 Flash 暂存地址。
	Update_ResetParser();                                     // 清空协议解析器。

	Serial_Printf("Update mode: %s\r\n", source == UPDATE_SOURCE_SERIAL ? "serial" : "4g");
}

/**
  * @brief  向升级模块输入一段原始数据。
  * @param  data: 原始数据起始地址。
  * @param  len : 原始数据长度。
  * @note   串口和后续 4G 都可以复用这个入口。
  */
void Update_InputData(uint8_t *data, uint16_t len)
{
	uint16_t i;

	if (data == 0)
	{
		return;                                                // 空指针保护。
	}

	for (i = 0; i < len; i++)
	{
		Update_ParseByte(data[i]);                             // 逐字节喂给状态机。
	}
}

/**
  * @brief  判断升级模块当前是否正在处理升级数据。
  * @retval 1=正在接收升级包或解析半包；0=空闲。
  * @note   main() 的启动等待窗口会用它判断是否继续留在 Bootloader。
  */
uint8_t Update_IsBusy(void)
{
	if ((UpdateCtx.state != UPDATE_STATE_IDLE) || (UpdateFrameLen != 0))
	{
		return 1;
	}

	return 0;
}

/**
  * @brief  升级模块周期任务。
  * @note   在 main() 的 while(1) 中持续调用。
  */
void Update_Task(void)
{
	if (UpdateCtx.source == UPDATE_SOURCE_SERIAL)
	{
		Update_PollSerial();                                   // 当前实现：从 USART1 DMA 队列取数据。
	}
	else
	{
		// 4G 模式预留：后续 4G 驱动收到数据后直接调用 Update_InputData()。
	}
}
