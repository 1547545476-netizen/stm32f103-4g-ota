# STM32F103 4G OTA Bootloader

基于STM32F103C8T6与Air780E AT固件的远程升级实验工程。APP通过MQTT接收升级清单，通过HTTPS下载固件到W25Q64；复位后由Bootloader认证、备份、安装，并管理新APP试运行和失败回滚。保留USART1串口升级通道。

本仓库提供源码、Keil工程、配置模板和发布工具；使用者需要配置自己的MQTT账号、固件地址与HMAC密钥。没有预置可用账号、云存储资源或预编译固件，也不需要GPT/Codex账号才能编译运行。

## 硬件与技术

- STM32F103C8T6：64KB Flash、20KB SRAM；STM32标准外设库。
- Air780E系列AT固件模块：USART2，4G入网、MQTT/TLS、HTTP/HTTPS和文件系统。
- W25Q64：SPI，8MB，保存下载镜像与回滚镜像。
- AT24C02：I2C，256字节，保存双副本OTA控制块。
- USART1：串口升级与调试日志，DMA + IDLE接收。
- 工具：Keil MDK / ARMCC 5、Python、Git、VS Code任务。

## 数据流

```text
发布端: APP bin -> 大小/CRC32/HMAC -> 上传对象存储 -> MQTT升级清单
设备APP: MQTT -> JSON解析 -> HTTPS -> 模块文件 -> 200B分块 -> W25Q64
        -> 下载流CRC32 + 存储回读CRC32 -> 控制块PENDING -> 复位
Boot: 外部CRC32/HMAC -> 备份旧APP -> UPDATING -> 写片内Flash
      -> 片内CRC32/向量表 -> TRIAL -> APP确认DONE，或启动失败后回滚
```

MQTT Broker与对象存储分工独立。历史联调使用EMQX和OSS，代码不要求购买数据库服务。

## 分区

| 存储 | 起始地址 | 容量/用途 |
| --- | --- | --- |
| STM32 Boot | `0x08000000` | 20KB |
| STM32 APP | `0x08005000` | 44KB |
| W25Q64下载槽 | `0x00000000` | 预留64KB，镜像最多44KB |
| W25Q64备份槽 | `0x00010000` | 预留64KB，镜像最多44KB |
| EEPROM A/B槽 | `0x00` / `0x80` | 各128字节，v3记录92字节 |

## 快速开始

1. 安装Keil MDK、ARMCC 5.06与STM32F1xx器件包，以及Python 3.10以上版本和Git。
2. `python -m pip install -r requirements.txt`
3. `python tools/ota/generate_device_key.py`：创建本机配置和随机设备密钥，不回显密钥。
4. 编辑生成的`User/OtaSecrets.h`，设置自己的Broker和Topic；该文件禁止提交。
5. 按[构建与使用](docs/BUILDING.md)设置`KEIL_ROOT`并构建Boot和APP。

源码检查无需真实凭据和硬件：

```sh
python tools/ci/check_project.py --source-only
python tools/ci/test_firmware_auth.py
python tools/ci/check_public_tree.py
```

第一次使用密钥必须与烧入Boot的密钥一致；更换Boot密钥会影响现有设备的升级能力。不要为正在运行的设备随意执行`--force`轮换。

## 验证范围

历史开发记录包含串口升级、4G入网、HTTPS下载以及APP v5到v6远程升级上板测试（18,280字节）。后续双副本控制块、HMAC、TRIAL/IWDG与回滚已进入源码，仍需针对当前发布版本执行断电和故障注入矩阵。不能用旧版成功日志代替新增功能验证。

APP当前采用周期性、阻塞式OTA检查，联网等待期间业务循环会暂停；不宣称实时并行调度。当前未实现HTTP Range断点续传、片内双APP执行区、非对称签名或设备批量发布平台。

## 文件索引

- [架构与状态机](docs/ARCHITECTURE.md)
- [构建、串口与MQTT发布](docs/BUILDING.md)
- [测试证据与待验证清单](docs/TESTING.md)
- [安全与凭据边界](SECURITY.md)
- [第三方声明](THIRD_PARTY_NOTICES.md)

本次源码整理没有为项目原创部分新增开源许可证。第三方文件保留原有声明，使用范围应分别依据对应条款；不要将STM32标准库与ARM CMSIS描述为项目原创代码。
