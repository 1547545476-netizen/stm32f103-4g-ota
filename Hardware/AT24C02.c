// ==================== AT24C02.c ====================
#include "AT24C02.h"
#include "MyI2C.h"  
#include "Delay.h"
#include "W25Q64.h"
#include <string.h>
#include "main.h"

static uint32_t OTA_CB_CalcChecksum(const OTA_CB_t *info);

// v3控制块每份92字节，两个事务槽各预留128字节，正好覆盖AT24C02的256字节。
#define OTA_CB_SLOT_A_ADDR       0x00U
#define OTA_CB_SLOT_B_ADDR       0x80U

// v2曾使用两个64字节槽。迁移时先读取0x00/0x40，再把v3写到新的0x80槽。
#define OTA_CB_V2_SLOT_A_ADDR    0x00U
#define OTA_CB_V2_SLOT_B_ADDR    0x40U

typedef struct {
	uint32_t magic;
	uint32_t format_version;
	uint32_t sequence;
	uint32_t OTA_flag;
	uint32_t app_version;
	uint32_t image_size;
	uint32_t image_crc32;
	uint32_t image_addr;
	uint32_t backup_version;
	uint32_t backup_size;
	uint32_t backup_crc32;
	uint32_t backup_addr;
	uint32_t trial_boot_count;
	uint32_t error_code;
	uint32_t header_checksum;
} OTA_CB_V2_t;

// 旧版控制块没有格式版本、序号和回滚字段，只用于首次升级后的自动迁移。
typedef struct {
	uint32_t magic;
	uint32_t OTA_flag;
	uint32_t app_version;
	uint32_t image_size;
	uint32_t image_crc32;
	uint32_t image_addr;
	uint32_t error_code;
	uint32_t header_checksum;
} OTA_CB_Legacy_t;

static uint8_t OTA_CB_ReadSlot(uint16_t addr, OTA_CB_t *info);
static uint8_t OTA_CB_ReadV2(OTA_CB_t *info);
static uint8_t OTA_CB_ReadLegacy(OTA_CB_t *info);
static uint8_t OTA_CB_IsSequenceNewer(uint32_t left, uint32_t right);
/**
  * @brief  AT24C02 EEPROM 内部布局
  * @note   总容量：256 字节 (0x00 ~ 0xFF)
  *         页大小：16 字节
  *         页数量：16 页 (Page 0 ~ Page 15)
  *         设备地址：0xA0 (写) / 0xA1 (读)  (A0/A1/A2 全接地时)
  * 
  * 
  * 
  *  AT24C02 内部地址映射 (256 字节):
  * 
  *  地址范围                    页号        用途
  *  ┌──────────────────────────────────────────────────────────────┐
  *  │  0x00                                                     │
  *  │  ┌───────────────────────────────────────────────────────┐ │
  *  │  │  0x00 ~ 0x0F         页 0     (16 字节)             │ │
  *  │  ├───────────────────────────────────────────────────────┤ │
  *  │  │  0x10 ~ 0x1F         页 1     (16 字节)             │ │
  *  │  ├───────────────────────────────────────────────────────┤ │
  *  │  │  0x20 ~ 0x2F         页 2     (16 字节)             │ │
  *  │  ├───────────────────────────────────────────────────────┤ │
  *  │  │  0x30 ~ 0x3F         页 3     (16 字节)             │ │
  *  │  ├───────────────────────────────────────────────────────┤ │
  *  │  │  0x40 ~ 0x4F         页 4     (16 字节)             │ │
  *  │  ├───────────────────────────────────────────────────────┤ │
  *  │  │  0x50 ~ 0x5F         页 5     (16 字节)             │ │
  *  │  ├───────────────────────────────────────────────────────┤ │
  *  │  │  0x60 ~ 0x6F         页 6     (16 字节)             │ │
  *  │  ├───────────────────────────────────────────────────────┤ │
  *  │  │  0x70 ~ 0x7F         页 7     (16 字节)             │ │
  *  │  ├───────────────────────────────────────────────────────┤ │
  *  │  │  0x80 ~ 0x8F         页 8     (16 字节)             │ │
  *  │  ├───────────────────────────────────────────────────────┤ │
  *  │  │  0x90 ~ 0x9F         页 9     (16 字节)             │ │
  *  │  ├───────────────────────────────────────────────────────┤ │
  *  │  │  0xA0 ~ 0xAF         页 10    (16 字节)             │ │
  *  │  ├───────────────────────────────────────────────────────┤ │
  *  │  │  0xB0 ~ 0xBF         页 11    (16 字节)             │ │
  *  │  ├───────────────────────────────────────────────────────┤ │
  *  │  │  0xC0 ~ 0xCF         页 12    (16 字节)             │ │
  *  │  ├───────────────────────────────────────────────────────┤ │
  *  │  │  0xD0 ~ 0xDF         页 13    (16 字节)             │ │
  *  │  ├───────────────────────────────────────────────────────┤ │
  *  │  │  0xE0 ~ 0xEF         页 14    (16 字节)             │ │
  *  │  ├───────────────────────────────────────────────────────┤ │
  *  │  │  0xF0 ~ 0xFF         页 15    (16 字节)             │ │
  *  │  └───────────────────────────────────────────────────────┘ │
  *  └──────────────────────────────────────────────────────────────┘
  * 
  *  AT24C02 设备地址配置:
  *  ┌──────────────────────────────────────────────────────────────┐
  *  │  A2  │  A1  │  A0  │  写地址  │  读地址  │  说明         │
  *  ├──────┼──────┼──────┼──────────┼──────────┼───────────────┤
  *  │  0   │  0   │  0   │  0xA0    │  0xA1    │  默认 (接地)  │
  *  │  0   │  0   │  1   │  0xA2    │  0xA3    │               │
  *  │  0   │  1   │  0   │  0xA4    │  0xA5    │               │
  *  │  0   │  1   │  1   │  0xA6    │  0xA7    │               │
  *  │  1   │  0   │  0   │  0xA8    │  0xA9    │               │
  *  │  1   │  0   │  1   │  0xAA    │  0xAB    │               │
  *  │  1   │  1   │  0   │  0xAC    │  0xAD    │               │
  *  │  1   │  1   │  1   │  0xAE    │  0xAF    │               │
  *  └──────┴──────┴──────┴──────────┴──────────┴───────────────┘
  * 
  */
/**
  * @brief  AT24C02 初始化（调用IIC初始化即可）
  */
void AT24C02_Init(void)
{
    MyI2C_Init();  // GPIO初始化，开漏输出，总线空闲
}

/**
  * @brief  向AT24C02写入一个字节
  * @param  addr: 存储地址（0~255，AT24C02共256字节）
  * @param  data: 要写入的数据
  * @retval 1=成功，0=失败（从机无应答）
  */
uint8_t AT24C02_WriteByte(uint16_t addr, uint8_t data)
{
    uint8_t ack;
    
    // 1. 起始信号
    MyI2C_Start();
    
    // 2. 发送设备写地址（0xA0）
    MyI2C_SendByte(AT24C02_ADDR_WRITE);
    ack = MyI2C_ReceiveAck();  // 等待从机应答
    if (ack == 1)
    {
        // 从机无应答，停止并返回失败
        MyI2C_Stop();
        return 0;
    }
    
    // 3. 发送存储地址（AT24C02地址范围0~255）
    MyI2C_SendByte((uint8_t)addr);
    ack = MyI2C_ReceiveAck();
    if (ack == 1)
    {
        MyI2C_Stop();
        return 0;
    }
    
    // 4. 发送要写入的数据
    MyI2C_SendByte(data);
    ack = MyI2C_ReceiveAck();
    if (ack == 1)
    {
        MyI2C_Stop();
        return 0;
    }
    
    // 5. 停止信号
    MyI2C_Stop();
    
    // 6. 等待写入完成（AT24C02需要最多5ms写入时间）
    Delay_ms(5);  // 需要实现毫秒级延时函数
    
    return 1;
}

/**
  * @brief  从AT24C02读取一个字节（随机读）
  * @param  addr: 存储地址（0~255）
  * @retval 读取到的数据
  */
uint8_t AT24C02_ReadByte(uint16_t addr)
{
    uint8_t data;
    uint8_t ack;
    
    // ===== 第一步：伪写操作，设置地址指针 =====
    MyI2C_Start();
    
    // 发送设备写地址（0xA0）
    MyI2C_SendByte(AT24C02_ADDR_WRITE);
    ack = MyI2C_ReceiveAck();
    if (ack == 1)
    {
        MyI2C_Stop();
        return 0;
    }
    
    // 发送存储地址
    MyI2C_SendByte((uint8_t)addr);
    ack = MyI2C_ReceiveAck();
    if (ack == 1)
    {
        MyI2C_Stop();
        return 0;
    }
    
    // ===== 第二步：重新开始，转为读操作 =====
    MyI2C_Start();
    
    // 发送设备读地址（0xA1）
    MyI2C_SendByte(AT24C02_ADDR_READ);
    ack = MyI2C_ReceiveAck();
    if (ack == 1)
    {
        MyI2C_Stop();
        return 0;
    }
    
    // ===== 第三步：读取数据 =====
    data = MyI2C_ReceiveByte();
    
    // 发送非应答（NACK），表示读完了
    MyI2C_SendAck(1);
    
    // 停止信号
    MyI2C_Stop();
    
    return data;
}

/**
  * @brief  页写入（最多16字节）
  * @param  addr: 起始地址（0~255，会自动页内回卷）
  * @param  data: 数据缓冲区指针
  * @param  len:  要写入的数据长度（最多16字节）
  * @retval 无
  */
void AT24C02_WritePage(uint16_t addr, uint8_t *data, uint16_t len)
{
    uint8_t ack;
    uint16_t i;
    
    // 限制最大长度16字节（AT24C02页大小）
    if (len > 16)
    {
        len = 16;
    }
    
    // 1. 起始信号
    MyI2C_Start();
    
    // 2. 发送设备写地址
    MyI2C_SendByte(AT24C02_ADDR_WRITE);
    ack = MyI2C_ReceiveAck();
    if (ack == 1)
    {
        MyI2C_Stop();
        return;
    }
    
    // 3. 发送起始地址
    MyI2C_SendByte((uint8_t)addr);
    ack = MyI2C_ReceiveAck();
    if (ack == 1)
    {
        MyI2C_Stop();
        return;
    }
    
    // 4. 连续发送数据
    for (i = 0; i < len; i++)
    {
        MyI2C_SendByte(data[i]);
        ack = MyI2C_ReceiveAck();
        if (ack == 1)
        {
            // 从机无应答，停止
            MyI2C_Stop();
            return;
        }
    }
    
    // 5. 停止信号
    MyI2C_Stop();
    
    // 6. 等待写入完成
    Delay_ms(5);
}

/**
  * @brief  连续读取数据（顺序读）
  * @param  addr: 起始地址
  * @param  data: 数据缓冲区指针
  * @param  len:  要读取的数据长度
  * @retval 无
  */
uint8_t AT24C02_ReadRandom(uint16_t addr, uint8_t *data, uint16_t len)
{
    uint8_t ack;
    uint16_t i;
    
    if ((data == 0) || (len == 0U) || ((uint32_t)addr + len > 256U))
    {
        return 0;
    }

    // ===== 第一步：伪写操作，设置地址指针 =====
    MyI2C_Start();
    MyI2C_SendByte(AT24C02_ADDR_WRITE);
    ack = MyI2C_ReceiveAck();
    if (ack == 1)
    {
        MyI2C_Stop();
        return 0;
    }
    
    MyI2C_SendByte((uint8_t)addr);
    ack = MyI2C_ReceiveAck();
    if (ack == 1)
    {
        MyI2C_Stop();
        return 0;
    }
    
    // ===== 第二步：重新开始，转为读操作 =====
    MyI2C_Start();
    MyI2C_SendByte(AT24C02_ADDR_READ);
    ack = MyI2C_ReceiveAck();
    if (ack == 1)
    {
        MyI2C_Stop();
        return 0;
    }
    
    // ===== 第三步：连续读取数据 =====
    for (i = 0; i < len; i++)
    {
        data[i] = MyI2C_ReceiveByte();
        
        // 如果不是最后一个字节，发送应答（ACK）
        if (i < len - 1)
        {
            MyI2C_SendAck(0);  // 0 = ACK，继续读
        }
    }
    
    // 最后一个字节发送非应答（NACK）
    MyI2C_SendAck(1);  // 1 = NACK，结束读
    
    // 停止信号
    MyI2C_Stop();

    return 1;
}

void AT24C02_ReadOTA_CB_Info(void)
{
	OTA_CB_t slotA;
	OTA_CB_t slotB;
	uint8_t validA;
	uint8_t validB;

	validA = OTA_CB_ReadSlot(OTA_CB_SLOT_A_ADDR, &slotA);
	validB = OTA_CB_ReadSlot(OTA_CB_SLOT_B_ADDR, &slotB);

	// 两份都有效时选择sequence更新的一份；正在写另一份时断电不会影响旧副本。
	if (validA && validB)
	{
		OTA_CB_Info = OTA_CB_IsSequenceNewer(slotB.sequence, slotA.sequence) ? slotB : slotA;
		return;
	}
	if (validA)
	{
		OTA_CB_Info = slotA;
		return;
	}
	if (validB)
	{
		OTA_CB_Info = slotB;
		return;
	}

	// 兼容v2双槽；认证标签先清0，下一次可信升级会写入新的HMAC标签。
	if (OTA_CB_ReadV2(&OTA_CB_Info))
	{
		return;
	}

	// 兼容最早保存在0地址的32字节控制块；下一次写入会迁移到v3的B槽。
	if (OTA_CB_ReadLegacy(&OTA_CB_Info))
	{
		return;
	}

	// 空片、两份都损坏或I2C读取失败时，只在RAM中建立安全的IDLE默认值。
	memset(&OTA_CB_Info, 0, OTA_CB_T_SIZE);
	OTA_CB_Info.magic = OTA_CB_MAGIC;
	OTA_CB_Info.format_version = OTA_CB_FORMAT_VERSION;
	OTA_CB_Info.OTA_flag = OTA_STATE_IDLE;
	OTA_CB_Info.image_addr = OTA_IMAGE_STORE_ADDR;
	OTA_CB_Info.backup_addr = OTA_BACKUP_STORE_ADDR;
	OTA_CB_Info.header_checksum = OTA_CB_CalcChecksum(&OTA_CB_Info);
}

static uint32_t OTA_CB_CalcChecksum(const OTA_CB_t *info)
{
	uint32_t i;
	uint32_t checksum = 5381;
	const uint8_t *data = (const uint8_t *)info;

	// header_checksum 字段本身不参与计算，否则每次写入都会改变结果。
	for (i = 0; i < OTA_CB_T_SIZE - sizeof(info->header_checksum); i++)
	{
		checksum = ((checksum << 5) + checksum) + data[i];
	}

	return checksum;
}

uint8_t AT24C02_IsOTA_CB_Valid(const OTA_CB_t *info)
{
	if (info == 0)
	{
		return 0;
	}

	// magic 不对，说明 EEPROM 中不是我们定义的 OTA 控制块。
	if (info->magic != OTA_CB_MAGIC)
	{
		return 0;
	}

	if (info->format_version != OTA_CB_FORMAT_VERSION)
	{
		return 0;
	}

	// OTA 状态值必须在当前格式定义的枚举范围内。
	if (info->OTA_flag > OTA_STATE_ROLLBACK)
	{
		return 0;
	}

	// 固件不能比 APP 分区还大，否则一定写不下。
	if (info->image_size > FLASH_APP_SIZE)
	{
		return 0;
	}
	if (info->backup_size > FLASH_APP_SIZE)
	{
		return 0;
	}
	if ((info->image_addr % W25Q64_SECTOR_SIZE) != 0U ||
		(info->backup_addr % W25Q64_SECTOR_SIZE) != 0U)
	{
		return 0;
	}

	// 控制块自身校验失败，说明 EEPROM 数据可能损坏。
	if (info->header_checksum != OTA_CB_CalcChecksum(info))
	{
		return 0;
	}

	return 1;
}

uint8_t AT24C02_WriteOTA_CB_Info(const OTA_CB_t *info)
{
	uint16_t i;
	uint16_t targetAddr;
	OTA_CB_t temp;
	OTA_CB_t verify;
	OTA_CB_t slotA;
	OTA_CB_t slotB;
	OTA_CB_t legacy;
	uint8_t validA;
	uint8_t validB;
	uint32_t latestSequence = 0U;

	if (info == 0)
	{
		return 0;
	}

	validA = OTA_CB_ReadSlot(OTA_CB_SLOT_A_ADDR, &slotA);
	validB = OTA_CB_ReadSlot(OTA_CB_SLOT_B_ADDR, &slotB);

	if (validA && validB)
	{
		if (OTA_CB_IsSequenceNewer(slotB.sequence, slotA.sequence))
		{
			latestSequence = slotB.sequence;
			targetAddr = OTA_CB_SLOT_A_ADDR;
		}
		else
		{
			latestSequence = slotA.sequence;
			targetAddr = OTA_CB_SLOT_B_ADDR;
		}
	}
	else if (validA)
	{
		latestSequence = slotA.sequence;
		targetAddr = OTA_CB_SLOT_B_ADDR;
	}
	else if (validB)
	{
		latestSequence = slotB.sequence;
		targetAddr = OTA_CB_SLOT_A_ADDR;
	}
	else if (OTA_CB_ReadV2(&legacy) || OTA_CB_ReadLegacy(&legacy))
	{
		// 首次迁移先写B槽，写到一半断电时0地址的旧控制块仍然存在。
		targetAddr = OTA_CB_SLOT_B_ADDR;
	}
	else
	{
		targetAddr = OTA_CB_SLOT_A_ADDR;
	}

	temp = *info;
	temp.magic = OTA_CB_MAGIC;
	temp.format_version = OTA_CB_FORMAT_VERSION;
	temp.sequence = latestSequence + 1U;
	temp.header_checksum = OTA_CB_CalcChecksum(&temp);

	// 只更新非活动槽。每个字节写完都有EEPROM内部提交，最后再整块读回校验。
	for (i = 0; i < OTA_CB_T_SIZE; i++)
	{
		if (!AT24C02_WriteByte(targetAddr + i, ((uint8_t *)&temp)[i]))
		{
			return 0;
		}
	}

	memset(&verify, 0, OTA_CB_T_SIZE);
	if (!AT24C02_ReadRandom(targetAddr, (uint8_t *)&verify, OTA_CB_T_SIZE))
	{
		return 0;
	}

	if ((memcmp(&temp, &verify, OTA_CB_T_SIZE) != 0) ||
		!AT24C02_IsOTA_CB_Valid(&verify))
	{
		return 0;
	}

	// 让调用者后续打印和再次写入时看到本次真正提交的sequence和checksum。
	OTA_CB_Info = temp;
	return 1;
}

static uint8_t OTA_CB_ReadSlot(uint16_t addr, OTA_CB_t *info)
{
	memset(info, 0, OTA_CB_T_SIZE);
	if (!AT24C02_ReadRandom(addr, (uint8_t *)info, OTA_CB_T_SIZE))
	{
		return 0;
	}
	return AT24C02_IsOTA_CB_Valid(info);
}

static uint8_t OTA_CB_IsSequenceNewer(uint32_t left, uint32_t right)
{
	// 无符号序号回卷后仍可比较；两次有效写入的距离不可能超过2^31。
	return ((int32_t)(left - right) > 0) ? 1U : 0U;
}

static uint32_t OTA_CB_CalcV2Checksum(const OTA_CB_V2_t *info)
{
	uint32_t i;
	uint32_t checksum = 5381U;
	const uint8_t *data = (const uint8_t *)info;

	for (i = 0U; i < sizeof(OTA_CB_V2_t) - sizeof(info->header_checksum); i++)
	{
		checksum = ((checksum << 5) + checksum) + data[i];
	}
	return checksum;
}

static uint8_t OTA_CB_IsV2Valid(const OTA_CB_V2_t *info)
{
	if ((info->magic != OTA_CB_MAGIC) ||
		(info->format_version != 2UL) ||
		(info->OTA_flag > OTA_STATE_ROLLBACK) ||
		(info->image_size > FLASH_APP_SIZE) ||
		(info->backup_size > FLASH_APP_SIZE) ||
		(info->header_checksum != OTA_CB_CalcV2Checksum(info)))
	{
		return 0U;
	}
	return 1U;
}

static uint8_t OTA_CB_ReadV2Slot(uint16_t address, OTA_CB_V2_t *info)
{
	memset(info, 0, sizeof(*info));
	if (!AT24C02_ReadRandom(address, (uint8_t *)info, sizeof(*info)))
	{
		return 0U;
	}
	return OTA_CB_IsV2Valid(info);
}

static uint8_t OTA_CB_ReadV2(OTA_CB_t *info)
{
	OTA_CB_V2_t slotA;
	OTA_CB_V2_t slotB;
	OTA_CB_V2_t *latest;
	uint8_t validA;
	uint8_t validB;

	validA = OTA_CB_ReadV2Slot(OTA_CB_V2_SLOT_A_ADDR, &slotA);
	validB = OTA_CB_ReadV2Slot(OTA_CB_V2_SLOT_B_ADDR, &slotB);
	if (!validA && !validB)
	{
		return 0U;
	}
	if (validA && validB)
	{
		latest = OTA_CB_IsSequenceNewer(slotB.sequence, slotA.sequence) ? &slotB : &slotA;
	}
	else
	{
		latest = validA ? &slotA : &slotB;
	}

	memset(info, 0, OTA_CB_T_SIZE);
	info->magic = OTA_CB_MAGIC;
	info->format_version = OTA_CB_FORMAT_VERSION;
	info->sequence = latest->sequence;
	info->OTA_flag = latest->OTA_flag;
	info->app_version = latest->app_version;
	info->image_size = latest->image_size;
	info->image_crc32 = latest->image_crc32;
	info->image_addr = latest->image_addr;
	info->backup_version = latest->backup_version;
	info->backup_size = latest->backup_size;
	info->backup_crc32 = latest->backup_crc32;
	info->backup_addr = latest->backup_addr;
	info->trial_boot_count = latest->trial_boot_count;
	info->error_code = latest->error_code;
	info->header_checksum = OTA_CB_CalcChecksum(info);
	return 1U;
}

static uint8_t OTA_CB_ReadLegacy(OTA_CB_t *info)
{
	OTA_CB_Legacy_t legacy;
	uint32_t i;
	uint32_t checksum = 5381U;
	const uint8_t *data = (const uint8_t *)&legacy;

	memset(&legacy, 0, sizeof(legacy));
	if (!AT24C02_ReadRandom(OTA_CB_SLOT_A_ADDR, (uint8_t *)&legacy, sizeof(legacy)))
	{
		return 0;
	}
	for (i = 0; i < sizeof(legacy) - sizeof(legacy.header_checksum); i++)
	{
		checksum = ((checksum << 5) + checksum) + data[i];
	}
	if ((legacy.magic != OTA_CB_MAGIC) ||
		(legacy.OTA_flag > OTA_STATE_ERROR) ||
		(legacy.image_size > FLASH_APP_SIZE) ||
		(legacy.header_checksum != checksum))
	{
		return 0;
	}

	memset(info, 0, OTA_CB_T_SIZE);
	info->magic = OTA_CB_MAGIC;
	info->format_version = OTA_CB_FORMAT_VERSION;
	info->OTA_flag = legacy.OTA_flag;
	info->app_version = legacy.app_version;
	info->image_size = legacy.image_size;
	info->image_crc32 = legacy.image_crc32;
	info->image_addr = legacy.image_addr;
	info->backup_addr = OTA_BACKUP_STORE_ADDR;
	info->error_code = legacy.error_code;
	info->header_checksum = OTA_CB_CalcChecksum(info);
	return 1;
}
