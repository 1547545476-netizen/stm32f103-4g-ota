#include "stm32f10x.h"                  // Device header

/**
  * @brief  写SS（从机选择）引脚电平
  * @param  BitValue: 0 或 1
  * @retval 无
  * @note   SS引脚为PA4，低电平有效（拉低表示选中从机）
  */
void MySPI_W_SS(uint8_t BitValue)
{
    GPIO_WriteBit(GPIOA, GPIO_Pin_4, (BitAction)BitValue);
}

/**
  * @brief  SPI1 硬件初始化
  * @param  无
  * @retval 无
  * @note   配置为模式0（CPOL=0, CPHA=0），主模式，软件NSS
  */
void MySPI_Init(void)
{
    // ==================== 1. 使能时钟 ====================
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);   // GPIOA时钟
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_SPI1, ENABLE);    // SPI1时钟
    
    // ==================== 2. 配置GPIO ====================
    GPIO_InitTypeDef GPIO_InitStructure;
    
    // 2.1 SS（从机选择）引脚 PA4 → 推挽输出
    // 虽然是软件控制，但需要输出高低电平来选中/释放从机
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_4;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &GPIO_InitStructure);
    
    // 2.2 SCK（时钟） PA5 和 MOSI（主出从入） PA7 → 复用推挽输出
    // SPI硬件控制这两个引脚，必须配置为复用功能
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_5 | GPIO_Pin_7;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &GPIO_InitStructure);
    
    // 2.3 MISO（主入从出） PA6 → 上拉输入
    // 从机通过此引脚发送数据给主机，主机读取
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_6;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &GPIO_InitStructure);
    
    // ==================== 3. 配置SPI参数 ====================
    SPI_InitTypeDef SPI_InitStructure;
    SPI_InitStructure.SPI_Mode = SPI_Mode_Master;           // 主机模式
    SPI_InitStructure.SPI_Direction = SPI_Direction_2Lines_FullDuplex; // 双线全双工
    SPI_InitStructure.SPI_DataSize = SPI_DataSize_8b;       // 8位数据帧
    SPI_InitStructure.SPI_FirstBit = SPI_FirstBit_MSB;      // 高位先发（MSB first）
    SPI_InitStructure.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_8;  // 9 MHz
    SPI_InitStructure.SPI_CPOL = SPI_CPOL_Low;              // 时钟极性：空闲时低电平
    SPI_InitStructure.SPI_CPHA = SPI_CPHA_1Edge;            // 时钟相位：第1个边沿采样
    SPI_InitStructure.SPI_NSS = SPI_NSS_Soft;               // 软件控制NSS
    SPI_InitStructure.SPI_CRCPolynomial = 7;                // CRC多项式（不用CRC，随便写）
    SPI_Init(SPI1, &SPI_InitStructure);
    
    // ==================== 4. 使能SPI ====================
    SPI_Cmd(SPI1, ENABLE);
    
    // ==================== 5. 释放SS（不选中任何从机） ====================
    MySPI_W_SS(1);  // SS高电平 → 不选中从机
}

/**
  * @brief  SPI 开始传输（选中从机）
  * @param  无
  * @retval 无
  * @note   SS拉低 → 选中从机，开始通信
  */
void MySPI_Start(void)
{
    MySPI_W_SS(0);  // SS低电平 → 选中从机
}

/**
  * @brief  SPI 结束传输（释放从机）
  * @param  无
  * @retval 无
  * @note   SS拉高 → 释放从机，结束通信
  */
void MySPI_Stop(void)
{
    MySPI_W_SS(1);  // SS高电平 → 释放从机
}

/**
  * @brief  SPI 交换一个字节（发送并接收）
  * @param  ByteSend: 要发送的字节
  * @retval 接收到的字节
  * @note   SPI全双工，发送一个字节的同时会收到一个字节
  */
uint8_t MySPI_SwapByte(uint8_t ByteSend)
{
    // 1. 等待发送缓冲区为空（TXE=1）
    // 硬件SPI有2个缓冲区：发送缓冲区和接收缓冲区
    // TXE=1表示发送缓冲区空了，可以写入新数据++
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_TXE) != SET);
    
    // 2. 将要发送的字节写入数据寄存器（DR）
    // 写入DR会自动清除TXE标志（因为缓冲区被填满）
    SPI_I2S_SendData(SPI1, ByteSend);
    
    // 3. 等待接收缓冲区非空（RXNE=1）
    // 在发送数据的同时，从机也在发送数据回来
    // 当收到一个字节时，RXNE标志置1
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_RXNE) != SET);
    
    // 4. 从数据寄存器（DR）读取接收到的字节
    // 读取DR会自动清除RXNE标志（因为缓冲区被读空）
    return SPI_I2S_ReceiveData(SPI1);
}
