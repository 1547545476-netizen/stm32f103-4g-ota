#ifndef __W25Q64_INS_H
#define __W25Q64_INS_H

// ==================== 指令定义 ====================
// 所有指令码来自 W25Q64 数据手册，每个指令码都是固定值

// -------- 写使能/禁用（必须放在写/擦除操作之前）--------
#define W25Q64_WRITE_ENABLE                    0x06    // 写使能（设置 WEL=1，允许写/擦除）
#define W25Q64_WRITE_DISABLE                   0x04    // 写禁用（清除 WEL=0，禁止写/擦除）

// -------- 状态寄存器操作 --------
#define W25Q64_READ_STATUS_REGISTER_1          0x05    // 读状态寄存器1（包含 BUSY 位和 WEL 位）
#define W25Q64_READ_STATUS_REGISTER_2          0x35    // 读状态寄存器2（包含 Quad 使能等）
#define W25Q64_WRITE_STATUS_REGISTER           0x01    // 写状态寄存器（配置保护区域等）

// -------- 写入操作 --------
#define W25Q64_PAGE_PROGRAM                    0x02    // 页编程（标准模式，一次最多写 256 字节）
#define W25Q64_QUAD_PAGE_PROGRAM               0x32    // 四线页编程（使用 4 根数据线，速度更快）

// -------- 擦除操作 --------
#define W25Q64_BLOCK_ERASE_64KB                0xD8    // 64KB 块擦除（大范围擦除，效率最高）
#define W25Q64_BLOCK_ERASE_32KB                0x52    // 32KB 块擦除（中等范围擦除）
#define W25Q64_SECTOR_ERASE_4KB                0x20    // 4KB 扇区擦除（最小擦除单位）
#define W25Q64_CHIP_ERASE                      0xC7    // 全片擦除（擦除整颗芯片，或使用 0x60）
#define W25Q64_ERASE_SUSPEND                   0x75    // 擦除暂停（用于中断正在进行的擦除操作）
#define W25Q64_ERASE_RESUME                    0x7A    // 擦除恢复（继续被暂停的擦除操作）

// -------- 电源管理 --------
#define W25Q64_POWER_DOWN                      0xB9    // 进入掉电模式（降低功耗）
#define W25Q64_HIGH_PERFORMANCE_MODE           0xA3    // 进入高性能模式（提高读取速度）
#define W25Q64_CONTINUOUS_READ_MODE_RESET      0xFF    // 连续读模式复位（退出连续读模式）
#define W25Q64_RELEASE_POWER_DOWN_HPM_DEVICE_ID 0xAB   // 退出掉电/高性能模式，并读取设备ID

// -------- ID 读取 --------
#define W25Q64_MANUFACTURER_DEVICE_ID          0x90    // 读取厂商和设备ID（兼容旧设备）
#define W25Q64_READ_UNIQUE_ID                  0x4B    // 读取唯一ID（每颗芯片独有的 64 位 ID）
#define W25Q64_JEDEC_ID                        0x9F    // 读取 JEDEC ID（标准 3 字节 ID：厂商+设备）

// -------- 读取操作 --------
#define W25Q64_READ_DATA                       0x03    // 标准读数据（最高 50MHz）
#define W25Q64_FAST_READ                       0x0B    // 快速读取（最高 104MHz，带空指令周期）
#define W25Q64_FAST_READ_DUAL_OUTPUT           0x3B    // 双线输出快速读取（2 根数据线输出）
#define W25Q64_FAST_READ_DUAL_IO               0xBB    // 双线 I/O 快速读取（2 根数据线双向）
#define W25Q64_FAST_READ_QUAD_OUTPUT           0x6B    // 四线输出快速读取（4 根数据线输出）
#define W25Q64_FAST_READ_QUAD_IO               0xEB    // 四线 I/O 快速读取（4 根数据线双向）
#define W25Q64_OCTAL_WORD_READ_QUAD_IO         0xE3    // 八进制字读取四线 I/O（更高速读取）

// -------- 工具宏 --------
#define W25Q64_DUMMY_BYTE                      0xFF    // 空字节（用于产生时钟，从机在此过程中输出数据）

#endif
