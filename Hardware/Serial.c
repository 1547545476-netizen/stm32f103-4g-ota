#include "stm32f10x.h"                  // Device header
#include <stdio.h>
#include "Serial.h"

uint8_t Serial_RxData;
uint8_t Serial_RxFlag;

UCB_CB U1CB;

uint8_t U1RxBuffer[U0_RX_SIZE];//串口接收缓冲区

/**
  * @brief  只初始化USART1发送功能。
  * @note   正式APP只用USART1打印调试日志，Air780E使用USART2。
  *         不启用USART1接收中断和DMA，因此APP不需要提供USART1_IRQHandler。
  */
void USART1_TxOnlyInit(void)
{
	GPIO_InitTypeDef GPIO_InitStructure;
	USART_InitTypeDef USART_InitStructure;

	RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, ENABLE);
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);

	// PA9=USART1_TX，复用推挽输出；PA10在APP中不使用，保持复位状态即可。
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_9;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(GPIOA, &GPIO_InitStructure);

	USART_InitStructure.USART_BaudRate = 921600;
	USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
	USART_InitStructure.USART_Mode = USART_Mode_Tx;
	USART_InitStructure.USART_Parity = USART_Parity_No;
	USART_InitStructure.USART_StopBits = USART_StopBits_1;
	USART_InitStructure.USART_WordLength = USART_WordLength_8b;
	USART_Init(USART1, &USART_InitStructure);
	USART_Cmd(USART1, ENABLE);
}

/**
  * @brief  USART1 串口初始化
  * @param  无
  * @retval 无
  */

void USART1_Init(void)
{
    // ---------- 1. 使能时钟 ----------
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    
    // ---------- 2. 配置GPIO ----------
    GPIO_InitTypeDef GPIO_InitStructure;
    
    // TX引脚（PA9）- 复用推挽输出
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_9;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &GPIO_InitStructure);
    
    // RX引脚（PA10）- 上拉输入
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_10;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &GPIO_InitStructure);
    
    // ---------- 3. 配置USART参数 ----------
    USART_InitTypeDef USART_InitStructure;
    USART_InitStructure.USART_BaudRate = 921600;
    USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_InitStructure.USART_Mode = USART_Mode_Tx | USART_Mode_Rx;
    USART_InitStructure.USART_Parity = USART_Parity_No;
    USART_InitStructure.USART_StopBits = USART_StopBits_1;
    USART_InitStructure.USART_WordLength = USART_WordLength_8b;
    USART_Init(USART1, &USART_InitStructure);
    
    // ---------- 4. 使能IDLE中断（帧结束检测） ----------
    USART_ITConfig(USART1, USART_IT_IDLE, ENABLE);
    
    // ---------- 5. 配置NVIC（USART中断） ----------
	NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);
    NVIC_InitTypeDef NVIC_InitStructure;
    NVIC_InitStructure.NVIC_IRQChannel = USART1_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 1;
    NVIC_Init(&NVIC_InitStructure);
    
    // ---------- 6. 使能USART1 ----------
	U0Rx_PtrInit();
	DMA1_Init();
    USART_Cmd(USART1, ENABLE);
}

/**
  * @brief  DMA1通道5初始化（用于USART1接收）
  * @param  无
  * @retval 无
  */

void DMA1_Init(void)
{
    // ---------- 1. 使能DMA时钟 ----------
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);
    
    // ---------- 2. 恢复DMA默认配置 ----------
    DMA_DeInit(DMA1_Channel5);
    
    // ---------- 3. 配置DMA参数 ----------
    DMA_InitTypeDef DMA_InitStructure;
    
    // 外设地址：USART1数据寄存器（固定地址）
    DMA_InitStructure.DMA_PeripheralBaseAddr = (uint32_t)(&USART1->DR);
    
    // 内存地址：接收缓冲区（递增）
    DMA_InitStructure.DMA_MemoryBaseAddr = (uint32_t)U1RxBuffer;
    
    // 传输方向：外设 → 内存
    DMA_InitStructure.DMA_DIR = DMA_DIR_PeripheralSRC;
    
    // 传输大小：整个缓冲区
    DMA_InitStructure.DMA_BufferSize = U0_RX_MAX + 1;
    
    // 外设地址不递增
    DMA_InitStructure.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    
    // 内存地址递增
    DMA_InitStructure.DMA_MemoryInc = DMA_MemoryInc_Enable;
    
    // 数据宽度：字节（8位）
    DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    DMA_InitStructure.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
    
    // 模式：不循环
    DMA_InitStructure.DMA_Mode = DMA_Mode_Normal;
    
    // 禁用内存到内存模式
    DMA_InitStructure.DMA_M2M = DMA_M2M_Disable;
    
    // 优先级：高
    DMA_InitStructure.DMA_Priority = DMA_Priority_High;
    
    // 初始化DMA1通道5
    DMA_Init(DMA1_Channel5, &DMA_InitStructure);
    
    // ---------- 5. 使能DMA通道 ----------
    DMA_Cmd(DMA1_Channel5, ENABLE);
    
    // ---------- 6. 使能USART1的DMA接收请求 ----------
    USART_DMACmd(USART1, USART_DMAReq_Rx, ENABLE);
}

void U0Rx_PtrInit(void)
{
	U1CB.URxDataIN = &U1CB.pU1RxData[0];
	U1CB.URxDataOUT = &U1CB.pU1RxData[0];
	U1CB.URxDataEND = &U1CB.pU1RxData[NUM - 1];
	U1CB.URxDataIN->start = &U1RxBuffer[0];
	U1CB.U1RxCounter = 0;
}

void Serial_SendByte(uint8_t Byte)
{
	USART_SendData(USART1, Byte);
	while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET);
}

void Serial_SendArray(uint8_t *Array, uint16_t Length)
{
	uint16_t i;
	for (i = 0; i < Length; i ++)
	{
		Serial_SendByte(Array[i]);
	}
}

void Serial_SendString(char *String)
{
	uint8_t i;
	for (i = 0; String[i] != '\0'; i ++)
	{
		Serial_SendByte(String[i]);
	}
}

uint32_t Serial_Pow(uint32_t X, uint32_t Y)
{
	uint32_t Result = 1;
	while (Y --)
	{
		Result *= X;
	}
	return Result;
}

void Serial_SendNumber(uint32_t Number, uint8_t Length)
{
	uint8_t i;
	for (i = 0; i < Length; i ++)
	{
		Serial_SendByte(Number / Serial_Pow(10, Length - i - 1) % 10 + '0');
	}
}

int fputc(int ch, FILE *f)
{
	Serial_SendByte(ch);
	return ch;
}

void Serial_Printf(const char *format, ...)
{
	// MQTT Topic和部分网络日志可能超过100字节，因此预留256字节。
	// vsnprintf()最多写入sizeof(String)-1个有效字符，并在末尾补'\0'；
	// 即使以后误传了很长的字符串，也只会截断日志，不会覆盖函数栈。
	char String[256];
	va_list arg;
	va_start(arg, format);
	vsnprintf(String, sizeof(String), format, arg);
	va_end(arg);
	Serial_SendString(String);
}

uint8_t Serial_GetRxFlag(void)
{
	if (Serial_RxFlag == 1)
	{
		Serial_RxFlag = 0;
		return 1;
	}
	return 0;
}

uint8_t Serial_GetRxData(void)
{
	return Serial_RxData;
}
