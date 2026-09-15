#ifndef __UPDETE_H
#define __UPDETE_H

#include "stm32f10x.h"
#include <stdint.h>

// 文件名保留你最开始创建时的 Updete，函数名统一使用 Update_，对外讲项目时更自然。

// ==================== 升级数据来源 ====================
typedef enum {
	UPDATE_SOURCE_SERIAL = 0,  // 当前已实现：USART1 + DMA + IDLE 接收。
	UPDATE_SOURCE_4G = 1,      // 预留：以后 4G 模块收到数据后调用 Update_InputData()。
} UpdateSource_t;

// ==================== 升级协议命令 ====================
#define UPDATE_CMD_START        0x01U   // 开始升级，携带版本、大小、CRC、地址和HMAC认证标签。
#define UPDATE_CMD_DATA         0x02U   // 固件数据包，携带 offset 和 payload。
#define UPDATE_CMD_END          0x03U   // 结束传输，校验 W25Q64 固件并写 OTA 控制块。
#define UPDATE_CMD_ABORT        0x04U   // 取消本次升级。

// 单包 payload 最大值。
// USART1 DMA 单次接收最大约 257 字节，协议头尾占 13 字节，所以这里留余量。
#define UPDATE_MAX_PAYLOAD      200U

// END 后自动复位进入 Bootloader 搬运流程。
// 上位机发完 END 并收到 ACK 后，板子会复位，然后把 W25Q64 固件搬到 APP 区。
#define UPDATE_AUTO_RESET_AFTER_END  1U

void Update_Init(UpdateSource_t source);
void Update_Task(void);
void Update_InputData(uint8_t *data, uint16_t len);
uint8_t Update_IsBusy(void);

#endif
