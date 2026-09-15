#include "MyFLASH.h"
#include "stm32f10x.h"
#include "Serial.h"

/**
  * @brief  STM32F103C8T6 内部 Flash 布局
  * @note   总容量：64KB (0x08000000 ~ 0x0800FFFF)
  *         页大小：1KB (1024 字节)
  *         页数量：64 页 (Page 0 ~ Page 63)
  * 
  * 
  * 
  *  地址范围                    页号        用途
  *  ┌──────────────────────────────────────────────────────────────┐
  *  │  0x08000000                                               │
  *  │  ┌───────────────────────────────────────────────────────┐ │
  *  │  │  0x08000000 ~ 0x080003FF   Page 0    (1KB)          │ │
  *  │  ├───────────────────────────────────────────────────────┤ │
  *  │  │  0x08000400 ~ 0x080007FF   Page 1    (1KB)          │ │
  *  │  ├───────────────────────────────────────────────────────┤ │
  *  │  │  0x08000800 ~ 0x08000BFF   Page 2    (1KB)          │ │
  *  │  ├───────────────────────────────────────────────────────┤ │
  *  │  │                     ...                             │ │
  *  │  ├───────────────────────────────────────────────────────┤ │
  *  │  │  0x0800F000 ~ 0x0800F3FF   Page 60   (1KB)          │ │
  *  │  ├───────────────────────────────────────────────────────┤ │
  *  │  │  0x0800F400 ~ 0x0800F7FF   Page 61   (1KB)          │ │
  *  │  ├───────────────────────────────────────────────────────┤ │
  *  │  │  0x0800F800 ~ 0x0800FBFF   Page 62   (1KB)          │ │
  *  │  ├───────────────────────────────────────────────────────┤ │
  *  │  │  0x0800FC00 ~ 0x0800FFFF   Page 63   (1KB)          │ │
  *  │  └───────────────────────────────────────────────────────┘ │
  *  └──────────────────────────────────────────────────────────────┘
  * 
  * 
  */

// ==================== 内部等待函数 ====================

/**
  * @brief  等待 Flash 操作完成
  * @param  无
  * @retval FLASH_Status: 
  *           - FLASH_COMPLETE:   操作成功完成
  *           - FLASH_TIMEOUT:    等待超时（BSY 位一直为 1）
  *           - FLASH_ERROR_PG:   编程错误（目标地址未被擦除）
  *           - FLASH_ERROR_WRP:  写保护错误（目标地址受保护）
  * @note   此函数会循环检查 BSY 位，直到操作完成或超时
  *         同时会检查并清除 PGERR 和 WRPRTERR 错误标志
  */
static FLASH_Status MyFLASH_WaitBusy(void)
{
    uint32_t timeout = FLASH_WAIT_TIMEOUT;  // 超时计数值
    uint32_t sr;                            // 用于保存状态寄存器值
    
    // -------- 1. 等待 BSY 位清除 --------
    // BSY (Bit 0) = 1 表示 Flash 正在执行擦除或写入操作
    while ((FLASH->SR & FLASH_FLAG_BSY) == FLASH_FLAG_BSY)
    {
        timeout--;                          // 计数值减1
        if (timeout == 0)                   // 超时
        {
            return FLASH_TIMEOUT;           // 返回超时错误
        }
    }
    
    // -------- 2. 检查错误标志 --------
    sr = FLASH->SR;                         // 读取状态寄存器
    
    // 2.1 检查编程错误 (PGERR, Bit 2)
    // PGERR = 1 表示尝试写入未被擦除的地址
    if (sr & FLASH_FLAG_PGERR)
    {
        FLASH->SR |= FLASH_FLAG_PGERR;      // 写1清除 PGERR 标志
        return FLASH_ERROR_PG;              // 返回编程错误
    }
    
    // 2.2 检查写保护错误 (WRPRTERR, Bit 4)
    // WRPRTERR = 1 表示尝试写入受保护的 Flash 区域
    if (sr & FLASH_FLAG_WRPRTERR)
    {
        FLASH->SR |= FLASH_FLAG_WRPRTERR;   // 写1清除 WRPRTERR 标志
        return FLASH_ERROR_WRP;             // 返回写保护错误
    }
    
    // -------- 3. 操作成功 --------
    return FLASH_COMPLETE;                  // 返回完成状态
}


// ==================== 读取函数 ====================
// 读取 Flash 不需要解锁，可直接像访问内存一样读取

/**
  * @brief  从指定地址读取一个 32 位字（4 字节）
  * @param  Address: 要读取的地址（必须 4 字节对齐）
  * @retval 读取到的 32 位数据
  */
uint32_t MyFLASH_ReadWord(uint32_t Address)
{
    // 将地址转换为指向 uint32_t 的指针，然后解引用
    return *((__IO uint32_t *)(Address));
}

/**
  * @brief  从指定地址读取一个 16 位半字（2 字节）
  * @param  Address: 要读取的地址（必须 2 字节对齐）
  * @retval 读取到的 16 位数据
  */
uint16_t MyFLASH_ReadHalfWord(uint32_t Address)
{
    // 将地址转换为指向 uint16_t 的指针，然后解引用
    return *((__IO uint16_t *)(Address));
}

/**
  * @brief  从指定地址读取一个 8 位字节（1 字节）
  * @param  Address: 要读取的地址
  * @retval 读取到的 8 位数据
  */
uint8_t MyFLASH_ReadByte(uint32_t Address)
{
    // 将地址转换为指向 uint8_t 的指针，然后解引用
    return *((__IO uint8_t *)(Address));
}


// ==================== 擦除函数 ====================
// 擦除 Flash 必须先解锁，擦除完成后需要等待，最后锁定
// 擦除后数据变为 0xFFFFFFFF（所有位变为 1）

/**
  * @brief  擦除整片 Flash（所有 64 页）
  * @param  无
  * @retval 无
  * @warning 会擦除包括程序代码在内的所有内容！
  * @note    执行后芯片变为空片，需要重新烧录程序
  *          擦除时间约 20~40ms
  */
void MyFLASH_EraseAllPages(void)
{
    FLASH_Unlock();             // 解锁 Flash（必须）
    FLASH_EraseAllPages();      // 整片擦除
    MyFLASH_WaitBusy();         // 等待擦除完成
    FLASH_Lock();               // 锁定 Flash（保护）
}

/**
  * @brief  擦除指定地址所在的页
  * @param  PageAddress: 页内的任意地址（会自动对齐到页起始地址）
  * @retval 无
  * @note   STM32F103C8T6 每页大小为 1KB
  *         擦除后该页所有数据变为 0xFFFFFFFF
  *         页擦除是 Flash 的最小擦除单位
  *         擦除时间约 20~40ms
  *         请确保不要擦除程序代码所在的页！
  */
void MyFLASH_ErasePage(uint32_t PageAddress)
{
    FLASH_Unlock();             // 解锁 Flash
    FLASH_ErasePage(PageAddress); // 擦除指定页
    MyFLASH_WaitBusy();         // 等待擦除完成
    FLASH_Lock();               // 锁定 Flash
}

/**
  * @brief  擦除从指定地址开始的连续多页
  * @param  StartAddress: 起始地址（会自动对齐到页起始地址）
  * @param  PageCount: 要擦除的页数
  * @retval 无
  * @note   每页 1KB，总擦除大小 = PageCount × 1KB
  *         擦除时间约 20~40ms/页
  *         例如：擦除 16 页 (16KB) 约需 320~640ms
  */
void MyFLASH_ErasePages(uint32_t StartAddress, uint32_t PageCount)
{
    uint32_t i;
    uint32_t currentPage;
    uint32_t pageStartAddress;
    FLASH_Status status;
    
    // 计算起始页号（自动对齐到页边界）
    currentPage = (StartAddress - FLASH_BASE_ADDR) / FLASH_PAGE_SIZE;
    
    FLASH_Unlock();             // 解锁 Flash
    
    // 逐页擦除
    for (i = 0; i < PageCount; i++)
    {
        // 计算当前页的起始地址
        pageStartAddress = FLASH_BASE_ADDR + (currentPage + i) * FLASH_PAGE_SIZE;
        
        FLASH_ErasePage(pageStartAddress);  // 擦除当前页
        status = MyFLASH_WaitBusy();        // 等待擦除完成
        
        if (status != FLASH_COMPLETE)       // 如果出错，提前退出
        {
            break;
        }
    }
    
    FLASH_Lock();               // 锁定 Flash
}


// ==================== 写入函数 ====================
// 写入 Flash 必须先解锁，写入完成后需要等待，最后锁定
// 写入前目标地址必须是 0xFFFFFFFF（已擦除状态）
// Flash 只能把 1 变成 0，不能把 0 变成 1

/**
  * @brief  在指定地址写入一个 32 位字（4 字节）
  * @param  Address: 要写入的地址（必须 4 字节对齐）
  * @param  Data: 要写入的 32 位数据
  * @retval 无
  * @note   写入前目标地址必须已擦除（数据为 0xFFFFFFFF）
  *         写入时间约 40~70μs
  */
void MyFLASH_ProgramWord(uint32_t Address, uint32_t Data)
{
    FLASH_Unlock();             // 解锁 Flash
    FLASH_ProgramWord(Address, Data); // 写入 4 字节
    MyFLASH_WaitBusy();         // 等待写入完成
    FLASH_Lock();               // 锁定 Flash
}

/**
  * @brief  在指定地址写入一个 16 位半字（2 字节）
  * @param  Address: 要写入的地址（必须 2 字节对齐）
  * @param  Data: 要写入的 16 位数据
  * @retval 无
  * @note   写入前目标地址必须已擦除（数据为 0xFFFF）
  *         写入时间约 40~70μs
  *         STM32 Flash 最小编程单位为 16 位（半字）
  */
void MyFLASH_ProgramHalfWord(uint32_t Address, uint16_t Data)
{
    FLASH_Unlock();             // 解锁 Flash
    FLASH_ProgramHalfWord(Address, Data); // 写入 2 字节
    MyFLASH_WaitBusy();         // 等待写入完成
    FLASH_Lock();               // 锁定 Flash
}

/**
  * @brief  连续写入 32 位数据到指定起始地址
  * @param  StartAddress: 起始地址（必须 4 字节对齐）
  * @param  DataArray: 要写入的 32 位数据数组指针
  * @param  Count: 要写入的字数（每个字 4 字节）
  * @retval 无
  * @note   总写入字节数 = Count × 4
  *         地址会自动递增，每次增加 4 字节
  *         每写入一个字节后等待完成
  */
void MyFLASH_ProgramWords(uint32_t StartAddress, uint32_t *DataArray, uint32_t Count)
{
    uint32_t i;
    uint32_t currentAddress = StartAddress;
    FLASH_Status status;
    
    // 检查地址是否 4 字节对齐
    if (StartAddress & 0x03) return;
    
    FLASH_Unlock();             // 解锁 Flash
    
    for (i = 0; i < Count; i++)
    {
        FLASH_ProgramWord(currentAddress, DataArray[i]); // 写入一个字
        status = MyFLASH_WaitBusy();                    // 等待写入完成
        
        if (status != FLASH_COMPLETE)                   // 如果出错，提前退出
        {
			Serial_Printf("写入失败！地址: 0x%08X, 索引: %d, 状态: %d\r\n",
                          currentAddress, i, status);
            break;
        }
        
        currentAddress += 4;    // 地址递增 4 字节
    }
    
    FLASH_Lock();               // 锁定 Flash
}

/**
  * @brief  连续写入 16 位数据到指定起始地址
  * @param  StartAddress: 起始地址（必须 2 字节对齐）
  * @param  DataArray: 要写入的 16 位数据数组指针
  * @param  Count: 要写入的半字数（每个半字 2 字节）
  * @retval 无
  * @note   总写入字节数 = Count × 2
  *         地址会自动递增，每次增加 2 字节
  *         每写入一个字节后等待完成
  */
void MyFLASH_ProgramHalfWords(uint32_t StartAddress, uint16_t *DataArray, uint32_t Count)
{
    uint32_t i;
    uint32_t currentAddress = StartAddress;
    FLASH_Status status;
    
    // 检查地址是否 2 字节对齐
    if (StartAddress & 0x01) return;
    
    FLASH_Unlock();             // 解锁 Flash
    
    for (i = 0; i < Count; i++)
    {
        FLASH_ProgramHalfWord(currentAddress, DataArray[i]); // 写入一个半字
        status = MyFLASH_WaitBusy();                        // 等待写入完成
        
        if (status != FLASH_COMPLETE)                       // 如果出错，提前退出
        {
            break;
        }
        
        currentAddress += 2;    // 地址递增 2 字节
    }
    
    FLASH_Lock();               // 锁定 Flash
}

/**
  * @brief  获取指定页的起始地址
  * @param  PageNum: 页号（0 ~ 63）
  * @retval 该页的起始地址，如果页号超出范围返回 0
  */
uint32_t MyFLASH_GetPageAddr(uint32_t PageNum)
{
    // 检查页号是否合法
    if (PageNum >= FLASH_PAGE_NUM)
    {
        return 0;  // 页号超出范围，返回 0 表示无效
    }
    
    // 计算页起始地址：基地址 + 页号 × 页大小
    return FLASH_BASE_ADDR + PageNum * FLASH_PAGE_SIZE;
}

