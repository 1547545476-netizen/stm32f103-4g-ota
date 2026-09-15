#ifndef __SERIAL_H
#define __SERIAL_H

#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>

#define U0_RX_SIZE 2048
#define U0_RX_MAX 256 //单次接收最大量
#define NUM 10 

/*记录起始和结束的结构体*/
typedef struct {
	uint8_t *start;
	uint8_t *end;
}UCB_U1RxBuffer; 

typedef struct{
	uint16_t U1RxCounter;//已存放数据的累加值
	UCB_U1RxBuffer pU1RxData[NUM];
	UCB_U1RxBuffer *URxDataIN;
	UCB_U1RxBuffer *URxDataOUT;
	UCB_U1RxBuffer *URxDataEND;
}UCB_CB;

void USART1_Init(void);
// 只初始化USART1发送功能，供正式APP打印日志使用，不开启DMA和接收中断。
void USART1_TxOnlyInit(void);
void DMA1_Init(void);
void U0Rx_PtrInit(void);
void Serial_SendByte(uint8_t Byte);
void Serial_SendArray(uint8_t *Array, uint16_t Length);
void Serial_SendString(char *String);
void Serial_SendNumber(uint32_t Number, uint8_t Length);
// 格式化并通过USART1输出调试日志。内部会限制最大长度，防止长字符串破坏栈。
void Serial_Printf(const char *format, ...);

uint8_t Serial_GetRxFlag(void);
uint8_t Serial_GetRxData(void);

extern uint8_t U1RxBuffer[U0_RX_SIZE];
extern UCB_CB U1CB;
#endif
