#ifndef __W25Q64_H
#define __W25Q64_H

#include <stdint.h>

// W25Q64 容量和擦写单位，供底层驱动与 OTA 模块共同使用。
#define W25Q64_TOTAL_SIZE       0x00800000UL  // 总容量：8MB。
#define W25Q64_SECTOR_SIZE      0x00001000UL  // 最小擦除单位：4KB。
#define W25Q64_PAGE_SIZE        0x00000100UL  // 单次页编程不能跨越256字节边界。

void W25Q64_Init(void);
void W25Q64_ReadID(uint8_t *MID, uint16_t *DID);
uint8_t W25Q64_PageProgram(uint32_t Address, uint8_t *DataArray, uint16_t Count);
uint8_t W25Q64_SectorErase(uint32_t Address);
void W25Q64_ReadData(uint32_t Address, uint8_t *DataArray, uint32_t Count);
uint8_t W25Q64_BlockErase64(uint32_t Address);

#endif
