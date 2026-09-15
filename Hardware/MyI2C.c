#include "stm32f10x.h"                  // STM32 标准外设库头文件。
#include "Delay.h"                     // 提供 Delay_us()，用于模拟 I2C 时序延时。
#include "Serial.h"                    // 预留调试打印使用。

/**
  * @brief  设置 SCL 时钟线电平。
  * @param  BitValue: 0=拉低 SCL，1=释放/拉高 SCL。
  * @note   本工程使用 PB10 作为模拟 I2C 的 SCL。
  */
void MyI2C_W_SCL(uint8_t BitValue)
{
	GPIO_WriteBit(GPIOB, GPIO_Pin_10, (BitAction)BitValue);  // 把 PB10 输出为指定电平。
	Delay_us(10);                                            // 留出电平稳定时间。
}

/**
  * @brief  设置 SDA 数据线电平。
  * @param  BitValue: 0=拉低 SDA，1=释放/拉高 SDA。
  * @note   本工程使用 PB11 作为模拟 I2C 的 SDA。
  */
void MyI2C_W_SDA(uint8_t BitValue)
{
	GPIO_WriteBit(GPIOB, GPIO_Pin_11, (BitAction)BitValue);  // 把 PB11 输出为指定电平。
	Delay_us(10);                                            // 留出电平稳定时间。
}

/**
  * @brief  读取 SDA 数据线当前电平。
  * @retval 0=低电平，1=高电平。
  * @note   开漏输出模式下，释放 SDA 后可直接读取 IDR 判断从机应答或数据位。
  */
uint8_t MyI2C_R_SDA(void)
{
	return (GPIOB->IDR & GPIO_Pin_11) ? 1 : 0;               // 直接读输入数据寄存器。
}

/**
  * @brief  初始化模拟 I2C GPIO。
  * @note   PB10=SCL，PB11=SDA，配置为开漏输出，符合 I2C 总线要求。
  */
void MyI2C_Init(void)
{
	GPIO_InitTypeDef GPIO_InitStructure;                     // GPIO 初始化结构体。

	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);    // 使能 GPIOB 时钟。

	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_OD;         // 开漏输出：器件只能主动拉低，总线靠上拉为高。
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_10 | GPIO_Pin_11; // 同时配置 PB10 和 PB11。
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;        // GPIO 翻转速度配置为 50MHz。
	GPIO_Init(GPIOB, &GPIO_InitStructure);                   // 应用 GPIO 配置。

	GPIO_SetBits(GPIOB, GPIO_Pin_10 | GPIO_Pin_11);          // I2C 空闲状态：SCL 和 SDA 都为高。
}

/**
  * @brief  发送 I2C 起始信号。
  * @note   起始条件：SCL 为高电平时，SDA 从高电平跳变到低电平。
  */
void MyI2C_Start(void)
{
	MyI2C_W_SDA(1);                                          // 先释放 SDA，确保总线处于空闲高电平。
	MyI2C_W_SCL(1);                                          // 再释放 SCL，形成空闲状态。
	MyI2C_W_SDA(0);                                          // SCL 高电平期间拉低 SDA，产生 START。
	MyI2C_W_SCL(0);                                          // 拉低 SCL，准备传输数据位。
}

/**
  * @brief  发送 I2C 停止信号。
  * @note   停止条件：SCL 为高电平时，SDA 从低电平跳变到高电平。
  */
void MyI2C_Stop(void)
{
	MyI2C_W_SDA(0);                                          // 先确保 SDA 为低。
	MyI2C_W_SCL(1);                                          // 释放 SCL 到高电平。
	MyI2C_W_SDA(1);                                          // SCL 高电平期间释放 SDA，产生 STOP。
}

/**
  * @brief  通过模拟 I2C 发送 1 字节。
  * @param  Byte: 要发送的 8 位数据，高位先发。
  */
void MyI2C_SendByte(uint8_t Byte)
{
	uint8_t i;                                               // 位计数变量。

	for (i = 0; i < 8; i++)
	{
		MyI2C_W_SDA(Byte & (0x80 >> i));                     // 按从 bit7 到 bit0 的顺序放到 SDA。
		MyI2C_W_SCL(1);                                      // 拉高 SCL，从机在高电平期间采样 SDA。
		MyI2C_W_SCL(0);                                      // 拉低 SCL，准备发送下一位。
	}
}

/**
  * @brief  通过模拟 I2C 接收 1 字节。
  * @retval 接收到的 8 位数据，高位先收。
  */
uint8_t MyI2C_ReceiveByte(void)
{
	uint8_t i;                                               // 位计数变量。
	uint8_t Byte = 0x00;                                     // 接收结果缓存。

	MyI2C_W_SDA(1);                                          // 释放 SDA，让从机控制数据线。
	for (i = 0; i < 8; i++)
	{
		MyI2C_W_SCL(1);                                      // 拉高 SCL，主机在高电平期间读取 SDA。
		if (MyI2C_R_SDA() == 1)
		{
			Byte |= (0x80 >> i);                             // 如果 SDA 为高，就把对应 bit 置 1。
		}
		MyI2C_W_SCL(0);                                      // 拉低 SCL，进入下一位。
	}

	return Byte;                                            // 返回组合好的 1 字节数据。
}

/**
  * @brief  主机发送 ACK/NACK。
  * @param  AckBit: 0=ACK，表示继续接收；1=NACK，表示本次读取结束。
  */
void MyI2C_SendAck(uint8_t AckBit)
{
	MyI2C_W_SDA(AckBit);                                     // 把 ACK 位放到 SDA。
	MyI2C_W_SCL(1);                                          // 拉高 SCL，让从机采样 ACK 位。
	MyI2C_W_SCL(0);                                          // 拉低 SCL，结束 ACK 位。
}

/**
  * @brief  主机读取从机 ACK/NACK。
  * @retval 0=收到 ACK，1=收到 NACK。
  */
uint8_t MyI2C_ReceiveAck(void)
{
	uint8_t AckBit;                                          // 保存从机返回的 ACK 位。

	MyI2C_W_SDA(1);                                          // 释放 SDA，让从机拉低或保持高。
	MyI2C_W_SCL(1);                                          // 拉高 SCL，读取从机 ACK。
	AckBit = MyI2C_R_SDA();                                  // 读取 SDA：0 表示从机应答。
	MyI2C_W_SCL(0);                                          // 拉低 SCL，结束 ACK 时钟。

	return AckBit;                                           // 返回 ACK/NACK。
}
