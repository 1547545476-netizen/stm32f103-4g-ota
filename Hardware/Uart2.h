#ifndef __UART2_H
#define __UART2_H

#include "stm32f10x.h"
#include <stdint.h>

// USART2 接 Air780E/M100M 的主串口。
// STM32F103C8T6 默认引脚：
// PA2 = USART2_TX，接 4G 模块 RXD
// PA3 = USART2_RX，接 4G 模块 TXD

// MQTT下发的OTA JSON中包含最长500字节的OSS URL，还要叠加Topic和AT响应头。
// 1024字节可以容纳一条受限的OTA命令；固件正文仍按200字节分块，不会一次占满RAM。
#define UART2_RX_BUFFER_SIZE    1024U

void Uart2_Init(uint32_t baud);
void Uart2_SendByte(uint8_t data);
void Uart2_SendData(const uint8_t *data, uint16_t len);
void Uart2_SendString(const char *str);
void Uart2_ClearRxBuffer(void);
char *Uart2_GetRxBuffer(void);
uint16_t Uart2_GetRxLength(void);
uint8_t Uart2_IsRxOverflow(void);
uint8_t Uart2_WaitRxLength(uint16_t length, uint32_t timeoutMs);
uint8_t Uart2_CopyRxData(uint16_t offset, uint8_t *data, uint16_t len);

#endif
