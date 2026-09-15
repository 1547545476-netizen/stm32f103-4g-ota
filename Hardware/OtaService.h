#ifndef __OTA_SERVICE_H
#define __OTA_SERVICE_H

#include <stdint.h>

/**
  * @brief  连接MQTT并执行一次远程OTA检查。
  * @param  currentVersion 当前正在运行的APP版本号。
  * @param  commandTimeoutMs 等待MQTT升级命令的最长时间，单位ms。
  * @retval 1=新固件已写入W25Q64且AT24C02已标记PENDING；0=没有升级或执行失败。
  * @note   调用前必须已经完成Air780E初始化、网络注册和PDP激活。
  *         本函数绝不会擦写STM32内部Flash。返回1后应软件复位，由Bootloader安装。
  */
uint8_t OtaService_RunMqttOnce(uint32_t currentVersion, uint32_t commandTimeoutMs);

/**
  * @brief  APP完成自检和基本业务运行后，确认当前试运行版本健康。
  * @retval 1=已经是DONE或TRIAL提交成功，0=版本不匹配、状态错误或EEPROM写失败。
  */
uint8_t OtaService_ConfirmRunningApp(uint32_t runningVersion);

#endif
