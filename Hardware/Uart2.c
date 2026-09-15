#include "Uart2.h"
#include "Delay.h"
#include <string.h>

// USART2 接收缓存。
// 第一阶段只是验证 AT/OK，所以先用简单线性缓存，不做复杂环形队列。
static char Uart2_RxBuffer[UART2_RX_BUFFER_SIZE];
static volatile uint16_t Uart2_RxLength;
static volatile uint8_t Uart2_RxOverflow;

/**
  * @brief  初始化 USART2。
  * @param  baud: 串口波特率。Air780E/M100M AT 固件常见默认值是 115200。
  * @note   PA2=TX，PA3=RX，用来连接 4G 模块主串口。
  */
void Uart2_Init(uint32_t baud)
{
	GPIO_InitTypeDef GPIO_InitStructure;
	USART_InitTypeDef USART_InitStructure;
	NVIC_InitTypeDef NVIC_InitStructure;

	// 1. 使能 GPIOA 和 USART2 时钟。
	// USART2 挂在 APB1 总线上，和 USART1 的 APB2 不同。
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
	RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE);

	// 2. PA2 配置为复用推挽输出，作为 USART2_TX。
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_2;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(GPIOA, &GPIO_InitStructure);

	// 3. PA3 配置为上拉输入，作为 USART2_RX。
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_3;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(GPIOA, &GPIO_InitStructure);

	// 4. 配置 USART2 参数：8 数据位、1 停止位、无校验、无流控。
	USART_InitStructure.USART_BaudRate = baud;
	USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
	USART_InitStructure.USART_Mode = USART_Mode_Tx | USART_Mode_Rx;
	USART_InitStructure.USART_Parity = USART_Parity_No;
	USART_InitStructure.USART_StopBits = USART_StopBits_1;
	USART_InitStructure.USART_WordLength = USART_WordLength_8b;
	USART_Init(USART2, &USART_InitStructure);

	// 5. 开启 RXNE 中断。模块返回的 AT 响应会逐字节进入 Uart2_RxBuffer。
	USART_ITConfig(USART2, USART_IT_RXNE, ENABLE);

	// 6. 配置 USART2 中断优先级。
	NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);
	NVIC_InitStructure.NVIC_IRQChannel = USART2_IRQn;
	NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
	NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 2;
	NVIC_InitStructure.NVIC_IRQChannelSubPriority = 1;
	NVIC_Init(&NVIC_InitStructure);

	Uart2_ClearRxBuffer();
	USART_Cmd(USART2, ENABLE);
}

/**
  * @brief  USART2 发送 1 字节。
  */
void Uart2_SendByte(uint8_t data)
{
	USART_SendData(USART2, data);
	while (USART_GetFlagStatus(USART2, USART_FLAG_TXE) == RESET);
}

/**
  * @brief  USART2 发送一段二进制数据。
  */
void Uart2_SendData(const uint8_t *data, uint16_t len)
{
	uint16_t i;

	if (data == 0)
	{
		return;
	}

	for (i = 0; i < len; i++)
	{
		Uart2_SendByte(data[i]);
	}
}

/**
  * @brief  USART2 发送字符串。
  * @note   AT 指令一般以 "\r\n" 结尾。
  */
void Uart2_SendString(const char *str)
{
	if (str == 0)
	{
		return;
	}

	while (*str != '\0')
	{
		Uart2_SendByte((uint8_t)*str);
		str++;
	}
}

/**
  * @brief  清空 USART2 接收缓存。
  * @note   发送一条新的 AT 指令前调用，避免旧响应影响新判断。
  */
void Uart2_ClearRxBuffer(void)
{
	USART_ITConfig(USART2, USART_IT_RXNE, DISABLE);
	memset(Uart2_RxBuffer, 0, sizeof(Uart2_RxBuffer));
	Uart2_RxLength = 0;
	Uart2_RxOverflow = 0;
	USART_ITConfig(USART2, USART_IT_RXNE, ENABLE);
}

/**
  * @brief  获取接收缓存字符串。
  */
char *Uart2_GetRxBuffer(void)
{
	// USART2_IRQHandler() 每收到一个字节，已经在“有效数据的下一个位置”写入了 '\0'。
	// 这里再固定保护数组最后一格，防止异常情况下字符串函数越过缓存边界。
	Uart2_RxBuffer[UART2_RX_BUFFER_SIZE - 1] = '\0';
	return Uart2_RxBuffer;
}

/**
  * @brief  获取当前已接收长度。
  */
uint16_t Uart2_GetRxLength(void)
{
	return Uart2_RxLength;
}

/**
  * @brief  判断接收缓存是否溢出。
  */
uint8_t Uart2_IsRxOverflow(void)
{
	return Uart2_RxOverflow;
}

/**
  * @brief  等待接收缓存达到指定字节数。
  * @note   HTTP固件正文是二进制，不能用strstr()或字符串结束符判断收完，必须按长度计数。
  */
uint8_t Uart2_WaitRxLength(uint16_t length, uint32_t timeoutMs)
{
	uint32_t elapsed;

	if (length > (UART2_RX_BUFFER_SIZE - 1U))
	{
		return 0;
	}

	for (elapsed = 0; elapsed < timeoutMs; elapsed++)
	{
		if (Uart2_RxLength >= length)
		{
			return 1;
		}

		if (Uart2_RxOverflow)
		{
			return 0;
		}

		Delay_ms(1);
	}

	return 0;
}

/**
  * @brief  从USART2缓存复制一段原始二进制数据。
  * @retval 1=复制成功，0=范围无效。
  * @note   使用长度复制，所以数据中出现0x00、回车、换行或"OK"都不会提前停止。
  */
uint8_t Uart2_CopyRxData(uint16_t offset, uint8_t *data, uint16_t len)
{
	uint16_t rxLength;

	if (data == 0)
	{
		return 0;
	}

	rxLength = Uart2_RxLength;
	if ((offset > rxLength) || (len > (uint16_t)(rxLength - offset)))
	{
		return 0;
	}

	USART_ITConfig(USART2, USART_IT_RXNE, DISABLE);
	memcpy(data, &Uart2_RxBuffer[offset], len);
	USART_ITConfig(USART2, USART_IT_RXNE, ENABLE);
	return 1;
}

/**
  * @brief  USART2 中断服务函数。
  * @note   每收到 1 字节就放入缓存，供 Air780E 驱动查找 OK/ERROR 等响应。
  */
void USART2_IRQHandler(void)
{
	uint8_t data;

	if (USART_GetITStatus(USART2, USART_IT_RXNE) != RESET)
	{
		data = (uint8_t)USART_ReceiveData(USART2);

		if (Uart2_RxLength < (UART2_RX_BUFFER_SIZE - 1))
		{
			Uart2_RxBuffer[Uart2_RxLength] = (char)data;
			Uart2_RxLength++;
			Uart2_RxBuffer[Uart2_RxLength] = '\0';
		}
		else
		{
			Uart2_RxOverflow = 1;
		}
	}

	// 如果出现溢出错误，读 DR 可以释放 USART 状态，避免后续接收异常。
	if (USART_GetFlagStatus(USART2, USART_FLAG_ORE) != RESET)
	{
		USART_ReceiveData(USART2);
	}
}
