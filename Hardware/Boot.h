#ifndef __BOOT_H
#define __BOOT_H

#include <stdint.h>

void BootLoader_Branch(void);
void BootLoader_Brance(void);
uint8_t Boot_IsValidApp(uint32_t appAddress);
void Boot_JumpToApp(uint32_t appAddress);

#endif

