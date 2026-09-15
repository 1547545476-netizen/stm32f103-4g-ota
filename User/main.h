#ifndef __MAIN_H
#define __MAIN_H

#include <stdint.h>
#include "OtaSecrets.h"  // 本机MQTT配置；该文件被.gitignore排除，模板见OtaSecrets.example.h。

// ==================== Bootloader debug switches ====================
// 0: normal Bootloader flow.
// 1: 4G test mode. The board only tests Air780E network registration and PDP.
#define BOOT_4G_TEST_ENABLE             0U

// 4G测试模式中的HTTP GET子测试。使用时需要同时把BOOT_4G_TEST_ENABLE设为1。
#define BOOT_4G_HTTP_TEST_ENABLE        0U
#define BOOT_4G_HTTP_TEST_URL           "http://airtest.openluat.com"

// OSS固件下载测试。开启前必须填写真实URL、文件大小和CRC32。
// 该开关优先于上面的普通HTTP测试；下载成功后自动复位并进入Bootloader搬运。
#define BOOT_4G_OTA_DOWNLOAD_TEST_ENABLE 0U
#define BOOT_4G_OTA_URL                 "https://example.invalid/firmware/AppTest.bin"
#define BOOT_4G_OTA_VERSION             3UL
#define BOOT_4G_OTA_SIZE                1356UL
#define BOOT_4G_OTA_CRC32               0xE6FAEA30UL
#define BOOT_4G_OTA_AUTH_TAG_HEX         "00000000000000000000000000000000"

// MQTT动态OTA测试。开启后，URL/版本/大小/CRC32不再写死，而由MQTT JSON下发。
// 正式联网任务已经迁移到APP；本开关只保留用于Bootloader阶段的硬件联调。
#define BOOT_4G_MQTT_OTA_TEST_ENABLE    0U

// EMQX CA证书一次性安装模式：通过HTTPS把官方CA下载到Air780E文件系统。
// 第一次烧录时设为1；串口打印"EMQX CA provision PASS"后改回0，再开启MQTT测试。
// CA文件保存在模块内部Flash中，STM32和Air780E重新上电后仍然存在。
#define BOOT_4G_CA_PROVISION_ENABLE     0U
#define BOOT_4G_CA_URL                  "https://assets.emqx.com/data/emqxsl-ca.crt"
#define BOOT_4G_CA_SIZE                 1294UL

// MQTT连接参数和两个Topic位于本机User/OtaSecrets.h，避免密码进入Git历史。
// 命令Topic方向：电脑发布 -> STM32订阅；状态Topic方向：STM32发布 -> 电脑订阅。
// Boot测试模式最多等待300秒；正式APP当前在AppTest/main.c中单独配置为60秒。
#define MQTT_COMMAND_TIMEOUT_MS         300000UL
// 置1时，只接受https://开头的固件URL，拒绝明文HTTP下载地址。
#define MQTT_OTA_REQUIRE_HTTPS          1U

#if BOOT_4G_OTA_DOWNLOAD_TEST_ENABLE && !BOOT_4G_TEST_ENABLE
#error "BOOT_4G_OTA_DOWNLOAD_TEST_ENABLE requires BOOT_4G_TEST_ENABLE=1"
#endif

#if BOOT_4G_OTA_DOWNLOAD_TEST_ENABLE && (BOOT_4G_OTA_SIZE == 0UL)
#error "Set BOOT_4G_OTA_SIZE and BOOT_4G_OTA_CRC32 before enabling OSS download test"
#endif

#if BOOT_4G_MQTT_OTA_TEST_ENABLE && !BOOT_4G_TEST_ENABLE
#error "BOOT_4G_MQTT_OTA_TEST_ENABLE requires BOOT_4G_TEST_ENABLE=1"
#endif

#if BOOT_4G_CA_PROVISION_ENABLE && !BOOT_4G_TEST_ENABLE
#error "BOOT_4G_CA_PROVISION_ENABLE requires BOOT_4G_TEST_ENABLE=1"
#endif

#if BOOT_4G_CA_PROVISION_ENABLE && (BOOT_4G_CA_SIZE == 0UL)
#error "Set BOOT_4G_CA_SIZE before enabling CA provision mode"
#endif

#if (BOOT_4G_HTTP_TEST_ENABLE + BOOT_4G_OTA_DOWNLOAD_TEST_ENABLE + BOOT_4G_MQTT_OTA_TEST_ENABLE + BOOT_4G_CA_PROVISION_ENABLE) > 1
#error "Enable only one 4G sub-test at a time"
#endif

// ==================== Flash 分区布局 ====================
#define FLASH_BASE_ADDR                 0x08000000UL
#define FLASH_SIZE                      0x00010000UL    // 片内 Flash 总容量：64KB
#define FLASH_PAGE_SIZE                 0x00000400UL    // STM32F103C8 每页 1KB
#define FLASH_PAGE_NUM                  64UL

// Bootloader 使用前 20 页：0x08000000 ~ 0x08004FFF。
// APP 使用剩余 44 页：       0x08005000 ~ 0x0800FFFF。
#define FLASH_PAGE_BOOTLOADER_START     0UL
#define FLASH_PAGE_BOOTLOADER_NUM       20UL
#define FLASH_PAGE_APP_START            FLASH_PAGE_BOOTLOADER_NUM
#define FLASH_PAGE_APP_NUM              (FLASH_PAGE_NUM - FLASH_PAGE_BOOTLOADER_NUM)

#define FLASH_BOOTLOADER_START          FLASH_BASE_ADDR
#define FLASH_BOOTLOADER_SIZE           (FLASH_PAGE_SIZE * FLASH_PAGE_BOOTLOADER_NUM)
#define FLASH_APP_START                 (FLASH_BASE_ADDR + FLASH_PAGE_SIZE * FLASH_PAGE_APP_START)
#define FLASH_APP_SIZE                  (FLASH_PAGE_SIZE * FLASH_PAGE_APP_NUM)
#define FLASH_END_ADDR                  (FLASH_BASE_ADDR + FLASH_SIZE)

// W25Q64 中用于暂存下载固件的位置，必须按4KB扇区边界对齐。
#define OTA_IMAGE_STORE_ADDR            0x00000000UL
// 安装新APP前，Bootloader把当前APP备份到独立的64KB区域，供试运行失败时回滚。
#define OTA_BACKUP_STORE_ADDR           0x00010000UL

// ==================== OTA 控制块 ====================
#define OTA_CB_MAGIC                    0x4F544142UL    // "OTAB"
#define OTA_CB_FORMAT_VERSION           3UL
#define OTA_TRIAL_BOOT_LIMIT            3UL
#define OTA_AUTH_TAG_SIZE               16U

// OTA 错误码：升级失败时写入 EEPROM，方便串口打印和后续排查。
#define OTA_ERR_NONE                    0UL
#define OTA_ERR_IMAGE_SIZE              1UL
#define OTA_ERR_EXTERNAL_CRC            2UL
#define OTA_ERR_PROGRAM_APP             3UL
#define OTA_ERR_INTERNAL_CRC            4UL
#define OTA_ERR_APP_VECTOR              5UL
#define OTA_ERR_BACKUP                  6UL
#define OTA_ERR_TRIAL_FAILED            7UL
#define OTA_ERR_ROLLBACK                8UL
#define OTA_ERR_IMAGE_AUTH              9UL

typedef enum {
	OTA_STATE_IDLE = 0,       // 没有升级任务，Bootloader 直接尝试跳 APP。
	OTA_STATE_PENDING = 1,    // 新固件已经放到 W25Q64，等待 Bootloader 搬运。
	OTA_STATE_UPDATING = 2,   // 正在把 W25Q64 的固件写入片内 APP 区。
	OTA_STATE_DONE = 3,       // 上一次升级成功。
	OTA_STATE_ERROR = 4,      // 上一次升级失败，可通过 error_code 判断原因。
	OTA_STATE_TRIAL = 5,      // 新APP已安装但尚未主动确认健康，Boot会限制试运行次数。
	OTA_STATE_ROLLBACK = 6,   // 正在从W25Q64备份区恢复旧APP，掉电后继续恢复。

	// 保留旧名字，避免已有代码立刻大面积改动。
	OTA_RESET_FLAG = OTA_STATE_IDLE,
	OTA_SET_FLAG = OTA_STATE_PENDING,
} OTA_Status_t;

typedef struct {
	uint32_t magic;             // 固定魔数，用来判断 EEPROM 中的数据是不是有效控制块。
	uint32_t format_version;    // 控制块格式版本，用于兼容以后字段扩展。
	uint32_t sequence;          // 每次事务写入递增；双副本中序号较新者生效。
	uint32_t OTA_flag;          // OTA 状态，取值见 OTA_Status_t。
	uint32_t app_version;       // 当前目标APP版本；DONE时表示已经确认的运行版本。
	uint32_t image_size;        // W25Q64 中暂存固件的字节数。
	uint32_t image_crc32;       // 整个固件的 CRC32，用于升级前后完整性校验。
	uint32_t image_addr;        // 固件在 W25Q64 中的起始地址。
	uint32_t backup_version;    // 安装前旧APP的版本号；没有可回滚版本时为0。
	uint32_t backup_size;       // W25Q64备份区中旧APP的有效字节数。
	uint32_t backup_crc32;      // 旧APP备份的CRC32。
	uint32_t backup_addr;       // 旧APP备份在W25Q64中的起始地址。
	uint32_t trial_boot_count;  // 新APP尚未确认期间发生的重新启动次数。
	uint32_t error_code;        // 最近一次升级失败原因。
	uint8_t image_auth_tag[OTA_AUTH_TAG_SIZE];   // 新固件的128位HMAC-SHA256认证标签。
	uint8_t backup_auth_tag[OTA_AUTH_TAG_SIZE];  // 回滚固件的认证标签，防止备份被替换。
	uint32_t header_checksum;   // 控制块自身校验，防止 EEPROM 脏数据误触发升级。
} OTA_CB_t;

#define OTA_CB_T_SIZE                   sizeof(OTA_CB_t)

extern OTA_CB_t OTA_CB_Info;

#endif
