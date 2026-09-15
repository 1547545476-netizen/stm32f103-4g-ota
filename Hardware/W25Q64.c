#include "stm32f10x.h"                  // 设备头文件，包含STM32寄存器定义
#include "MySPI.h"                      // 底层SPI驱动
#include "W25Q64.h"                     // 容量、页/扇区大小以及公开接口
#include "W25Q64_Ins.h"                // W25Q64指令定义（操作码）

/**
  * @brief  W25Q64 外部 Flash 内部布局
  * @note   总容量：64Mbit = 8MByte (0x000000 ~ 0x7FFFFF)
  *         页大小：256 字节
  *         扇区大小：4KB (16 页)
  *         块大小：64KB (16 扇区)
  *         制造商 ID：0xEF (Winbond)
  *         设备 ID：0x4017
  * 
  * 
  * 
  *  W25Q64 存储层次结构 (总容量 8MB):
  * 
  *  ┌──────────────────────────────────────────────────────────────┐
  *  │  块 0 (64KB)   地址: 0x000000 ~ 0x00FFFF                  │
  *  │  ┌───────────────────────────────────────────────────────┐ │
  *  │  │  扇区 0 (4KB)   0x000000 ~ 0x000FFF                 │ │
  *  │  │  ┌─────────────────────────────────────────────────┐ │ │
  *  │  │  │  页 0  0x000000 ~ 0x0000FF                    │ │ │
  *  │  │  ├─────────────────────────────────────────────────┤ │ │
  *  │  │  │  页 1  0x000100 ~ 0x0001FF                    │ │ │
  *  │  │  ├─────────────────────────────────────────────────┤ │ │
  *  │  │  │  页 2  0x000200 ~ 0x0002FF                    │ │ │
  *  │  │  ├─────────────────────────────────────────────────┤ │ │
  *  │  │  │  ...  (共 16 页)                               │ │ │
  *  │  │  ├─────────────────────────────────────────────────┤ │ │
  *  │  │  │  页 15 0x000F00 ~ 0x000FFF                    │ │ │
  *  │  │  └─────────────────────────────────────────────────┘ │ │
  *  │  ├───────────────────────────────────────────────────────┤ │
  *  │  │  扇区 1 (4KB)   0x001000 ~ 0x001FFF                 │ │
  *  │  ├───────────────────────────────────────────────────────┤ │
  *  │  │  ...                                               │ │
  *  │  ├───────────────────────────────────────────────────────┤ │
  *  │  │  扇区 15 (4KB)  0x00F000 ~ 0x00FFFF                 │ │
  *  │  └───────────────────────────────────────────────────────┘ │
  *  ├──────────────────────────────────────────────────────────────┤
  *  │  块 1 (64KB)   地址: 0x010000 ~ 0x01FFFF                  │
  *  │  扇区 16 ~ 31                                             │
  *  ├──────────────────────────────────────────────────────────────┤
  *  │  块 2 (64KB)   地址: 0x020000 ~ 0x02FFFF                  │
  *  ├──────────────────────────────────────────────────────────────┤
  *  │  ...                                                       │
  *  ├──────────────────────────────────────────────────────────────┤
  *  │  块 127 (64KB)  地址: 0x7F0000 ~ 0x7FFFFF                 │
  *  │  扇区 2032 ~ 2047                                          │
  *  └──────────────────────────────────────────────────────────────┘
  * 
  *  W25Q64 操作限制:
  *  ┌──────────────────────────────────────────────────────────────┐
  *  │  操作      │  最小单位  │  限制                           
  *  ├─────────────────────────────────────────────────────────┤
  *  │  读取      │  字节      │  无限制，可连续读              
  *  │  写入      │  页 (256B) │  最多 256 字节/次，不能跨页    
  *  │  擦除      │  扇区(4KB) │  最小擦除单位                   
  *  │  擦除      │  块(64KB)  │  大范围擦除，效率高            
  *  └─────────────────────────────────────────────────────────┘
  */

/**
  * @brief  W25Q64 Flash 初始化
  * @param  无
  * @retval 无
  * @note   实际就是初始化SPI硬件
  */
void W25Q64_Init(void)
{
    MySPI_Init();  // 调用SPI初始化，配置GPIO和SPI参数
}

/**
  * @brief  读取 W25Q64 的 JEDEC ID
  * @param  MID: 制造商ID指针（8位），Winbond固定为 0xEF
  * @param  DID: 设备ID指针（16位），W25Q64固定为 0x4017
  * @retval 无
  * @note   JEDEC ID = 0xEF 0x40 0x17
  *         用于验证通信是否正常，以及识别Flash型号
  */
void W25Q64_ReadID(uint8_t *MID, uint16_t *DID)
{
    // 1. 选中从机（SS拉低）
    MySPI_Start();
    
    // 2. 发送读取JEDEC ID指令（0x9F）
    MySPI_SwapByte(W25Q64_JEDEC_ID);
    
    // 3. 接收3个字节的ID数据
    // 发送DUMMY_BYTE（0xFF）是为了产生时钟，让从机输出数据
    *MID = MySPI_SwapByte(W25Q64_DUMMY_BYTE);   // 第1字节：制造商ID（0xEF）
    *DID = MySPI_SwapByte(W25Q64_DUMMY_BYTE);   // 第2字节：设备ID高字节（0x40）
    *DID <<= 8;                                  // 左移8位，让出低8位
    *DID |= MySPI_SwapByte(W25Q64_DUMMY_BYTE);   // 第3字节：设备ID低字节（0x17）
    
    // 4. 释放从机（SS拉高）
    MySPI_Stop();
}

/**
  * @brief  发送写使能指令（WREN）
  * @param  无
  * @retval 无
  * @note   在执行任何写/擦除操作前，必须先发送此指令
  *         操作后 WEL（Write Enable Latch）位会被置1
  *         每次写/擦除操作后，WEL位会自动清零
  */
void W25Q64_WriteEnable(void)
{
    MySPI_Start();                          // 选中从机
    MySPI_SwapByte(W25Q64_WRITE_ENABLE);    // 发送写使能指令（0x06）
    MySPI_Stop();                           // 释放从机
}

/**
  * @brief  等待 W25Q64 内部操作完成
  * @param  无
  * @retval 无
  * @note   通过读取状态寄存器1，检查 BUSY 位（bit 0）
  *         当 BUSY=0 时，表示内部写/擦除操作已完成
  *         写操作约 1~3ms，扇区擦除约 45~300ms
  */
static uint8_t W25Q64_WaitBusy(void)
{
    uint32_t Timeout;
    
    MySPI_Start();                              // 选中从机
    
    // 发送读状态寄存器1指令（0x05）
    MySPI_SwapByte(W25Q64_READ_STATUS_REGISTER_1);
    
    Timeout = 100000;  // 超时计数值，防止程序卡死
    
    // 循环读取状态寄存器，检查 BUSY 位（bit 0）
    // 当 BUSY=1 时，说明内部操作还在进行
    while ((MySPI_SwapByte(W25Q64_DUMMY_BYTE) & 0x01) == 0x01)
    {
        Timeout--;
        if (Timeout == 0)
        {
            MySPI_Stop();
            return 0;  // 超时必须向上传播，不能把未完成的擦写当作成功
        }
    }
    
    MySPI_Stop();  // 释放从机
    return 1;
}

/**
  * @brief  页编程（写入数据到 Flash）
  * @param  Address: 起始地址（24位，范围 0x000000 ~ 0x7FFFFF）
  * @param  DataArray: 要写入的数据缓冲区指针
  * @param  Count: 写入字节数（不能超过一页256字节）
  * @retval 无
  * @note   W25Q64 页大小 = 256 字节
  *         页编程前需要先执行写使能
  *         写入后需要等待 BUSY 位清除
  *         如果写入超过256字节，数据会回卷覆盖本页开头
  */
uint8_t W25Q64_PageProgram(uint32_t Address, uint8_t *DataArray, uint16_t Count)
{
    uint16_t i;

    // 页编程不能越过器件末尾，也不能跨越256字节页边界。
    if ((DataArray == 0) || (Count == 0U) || (Count > W25Q64_PAGE_SIZE) ||
        (Address >= W25Q64_TOTAL_SIZE) ||
        ((uint32_t)Count > (W25Q64_TOTAL_SIZE - Address)) ||
        ((Address % W25Q64_PAGE_SIZE) + Count > W25Q64_PAGE_SIZE))
    {
        return 0;
    }
    
    // 1. 写使能（每次写操作前都要执行）
    W25Q64_WriteEnable();
    
    // 2. 发送页编程指令（0x02）+ 24位地址
    MySPI_Start();
    MySPI_SwapByte(W25Q64_PAGE_PROGRAM);    // 指令：0x02
    MySPI_SwapByte(Address >> 16);          // 地址高8位（A23-A16）
    MySPI_SwapByte(Address >> 8);           // 地址中8位（A15-A8）
    MySPI_SwapByte(Address);                // 地址低8位（A7-A0）
    
    // 3. 连续发送数据（Count 个字节）
    for (i = 0; i < Count; i++)
    {
        MySPI_SwapByte(DataArray[i]);       // 发送数据
    }
    MySPI_Stop();
    
    // 4. 等待内部写入完成
    return W25Q64_WaitBusy();
}

/**
  * @brief  扇区擦除（4KB）
  * @param  Address: 要擦除的扇区起始地址（24位）
  * @retval 无
  * @note   擦除后扇区内所有数据变为 0xFF
  *         擦除前需要先执行写使能
  *         擦除时间约 45ms ~ 300ms
  *         4KB扇区擦除是最小的擦除单位
  */
uint8_t W25Q64_SectorErase(uint32_t Address)
{
    if ((Address >= W25Q64_TOTAL_SIZE) || ((Address % W25Q64_SECTOR_SIZE) != 0U))
    {
        return 0;
    }
    // 1. 写使能
    W25Q64_WriteEnable();
    
    // 2. 发送扇区擦除指令（0x20）+ 24位地址
    MySPI_Start();
    MySPI_SwapByte(W25Q64_SECTOR_ERASE_4KB);    // 指令：0x20
    MySPI_SwapByte(Address >> 16);              // 地址高8位
    MySPI_SwapByte(Address >> 8);               // 地址中8位
    MySPI_SwapByte(Address);                    // 地址低8位
    MySPI_Stop();
    
    // 3. 等待内部擦除完成
    return W25Q64_WaitBusy();
}

/**
  * @brief  连续读取数据
  * @param  Address: 起始地址（24位）
  * @param  DataArray: 数据缓冲区指针（用于存放读取的数据）
  * @param  Count: 要读取的字节数
  * @retval 无
  * @note   读取没有页限制，可以连续读取任意长度
  *         读取不会改变 Flash 内容
  *         读取速度远快于写入/擦除
  */
void W25Q64_ReadData(uint32_t Address, uint8_t *DataArray, uint32_t Count)
{
    uint32_t i;
    
    // 1. 发送读数据指令（0x03）+ 24位地址
    MySPI_Start();
    MySPI_SwapByte(W25Q64_READ_DATA);       // 指令：0x03
    MySPI_SwapByte(Address >> 16);          // 地址高8位
    MySPI_SwapByte(Address >> 8);           // 地址中8位
    MySPI_SwapByte(Address);                // 地址低8位
    
    // 2. 连续读取 Count 个字节
    for (i = 0; i < Count; i++)
    {
        // 发送 DUMMY_BYTE（0xFF）产生时钟，同时接收从机数据
        DataArray[i] = MySPI_SwapByte(W25Q64_DUMMY_BYTE);
    }
    MySPI_Stop();
}

/**
  * @brief  64KB 块擦除
  * @param  Address: 要擦除的块内任意地址（24位）
  * @retval 无
  * @note   擦除后该 64KB 块内所有数据变为 0xFF
  *         擦除前需要先执行写使能
  *         擦除时间约 150ms ~ 400ms
  *         64KB 块擦除比 4KB 扇区擦除更快（擦除整块）
  */
uint8_t W25Q64_BlockErase64(uint32_t Address)
{
    if ((Address >= W25Q64_TOTAL_SIZE) || ((Address % 0x00010000UL) != 0U))
    {
        return 0;
    }
    // 1. 写使能（每次擦除前必须）
    W25Q64_WriteEnable();
    
    // 2. 发送 64KB 块擦除指令（0xD8）+ 24位地址
    MySPI_Start();
    MySPI_SwapByte(W25Q64_BLOCK_ERASE_64KB);    // 指令：0xD8
    MySPI_SwapByte(Address >> 16);              // 地址高8位（A23-A16）
    MySPI_SwapByte(Address >> 8);               // 地址中8位（A15-A8）
    MySPI_SwapByte(Address);                    // 地址低8位（A7-A0）
    MySPI_Stop();
    
    // 3. 等待内部擦除完成
    return W25Q64_WaitBusy();
}
