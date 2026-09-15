#ifndef __AT24C02_H
#define __AT24C02_H

#include "stm32f10x.h"
#include <stdint.h>
#include "main.h"

// AT24C02 设备地址（A0/A1/A2 都接地时）
#define AT24C02_ADDR_WRITE  0xA0  // 写地址（1010 000 0）
#define AT24C02_ADDR_READ   0xA1  // 读地址（1010 000 1）

// 函数声明
void AT24C02_Init(void);
uint8_t AT24C02_WriteByte(uint16_t addr, uint8_t data);
uint8_t AT24C02_ReadByte(uint16_t addr);
void AT24C02_WritePage(uint16_t addr, uint8_t *data, uint16_t len);
uint8_t AT24C02_ReadRandom(uint16_t addr, uint8_t *data, uint16_t len);
void AT24C02_ReadOTA_CB_Info(void);
uint8_t AT24C02_WriteOTA_CB_Info(const OTA_CB_t *info);
uint8_t AT24C02_IsOTA_CB_Valid(const OTA_CB_t *info);

#endif
