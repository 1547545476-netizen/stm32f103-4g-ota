#ifndef __MYFLASH_H
#define __MYFLASH_H

#include "stm32f10x.h"
#include "main.h"

#define FLASH_WAIT_TIMEOUT          100000UL

uint32_t MyFLASH_ReadWord(uint32_t Address);
uint16_t MyFLASH_ReadHalfWord(uint32_t Address);
uint8_t  MyFLASH_ReadByte(uint32_t Address);

void MyFLASH_EraseAllPages(void);
void MyFLASH_ErasePage(uint32_t PageAddress);
void MyFLASH_ErasePages(uint32_t StartAddress, uint32_t PageCount);

void MyFLASH_ProgramWord(uint32_t Address, uint32_t Data);
void MyFLASH_ProgramHalfWord(uint32_t Address, uint16_t Data);
void MyFLASH_ProgramWords(uint32_t StartAddress, uint32_t *DataArray, uint32_t Count);
void MyFLASH_ProgramHalfWords(uint32_t StartAddress, uint16_t *DataArray, uint32_t Count);

uint32_t MyFLASH_GetPageAddr(uint32_t PageNum);

#endif
