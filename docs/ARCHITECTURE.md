# 架构与状态机

## 模块边界

| 文件 | 职责 |
| --- | --- |
| `User/main.c` | Boot初始化、控制块读取和启动分支、串口接收窗口 |
| `Hardware/Boot.c` | 新镜像验证、旧APP备份、片内安装、TRIAL计数、回滚及跳转 |
| `AppTest/main.c` | 示例业务、IWDG、5秒确认点、周期OTA检查 |
| `Hardware/OtaService.c` | MQTT连接/清单校验/HTTP下载编排、APP确认 |
| `Hardware/Air780E.c` | AT、入网、PDP、HTTP、文件系统和分块传输 |
| `Hardware/Air780E_Mqtt.c` | MQTT相关AT、异步结果和消息接收 |
| `Hardware/OtaManifest.c` | 有界OTA JSON解析，非完整通用JSON实现 |
| `Hardware/Updete.c` | USART1 START/DATA/END串口OTA |
| `Hardware/AT24C02.c` | v3双槽控制块、递增序号、校验、兼容迁移 |
| `Hardware/OtaAuth.c` | SHA-256/HMAC，Boot擦除APP前验证 |

`UpdateCtx.source`只选择串口帧解析器的取数入口，当前MQTT/HTTP路径不通过该解析器。两种接收通道最终提交相同格式的控制块。

## 安装阶段

```text
PENDING -> 验证新镜像CRC/HMAC -> 备份片内旧APP并验证
        -> 提交UPDATING -> 擦写APP -> CRC/向量校验
        -> 提交TRIAL -> 跳APP -> 健康确认 -> DONE
```

W25Q64接收块最多200字节，但页编程不得跨256字节边界，写函数按边界拆分；Boot搬运缓冲为256字节。二进制正文必须按显式长度处理，不能用strlen寻找文件结束。

## 恢复策略与边界

- 下载失败且尚未提交PENDING：片内旧APP保持原样，之后重新下载，不支持持久化断点续传。
- 备份中断：仍为PENDING，下次重做备份，尚未擦APP。
- 安装中断：UPDATING触发重新安装，新镜像失效时尝试回滚。
- 回滚中断：ROLLBACK触发从备份槽重新恢复。
- 没有合法备份：无法回滚时留Boot恢复模式，不能保证任何硬件损坏都可自愈。

安装当次写TRIAL并将计数置0，不在同一启动中加计数；后续未确认重启逐次加一，达到3触发回滚。掉电/手动复位也可能计入，不代表已经区分所有故障复位原因。DONE是已确认状态；确认后的业务崩溃目前不再自动触发TRIAL回滚。

IWDG由APP启用，当前Delay_ms等待路径喂狗，属于示例健康机制，不能捕获持续执行Delay的逻辑死循环。产品化需把确认条件与业务自检结合，并明确跨复位IWDG持续工作的处理。

AT24C02 A/B槽各128字节，v3结构体92字节；更新非活动槽，sequence加一并计算checksum，写完回读验证。checksum是非密码学结构校验，不等同于HMAC，也不是芯片级原子写入承诺。

## 地址与认证

APP初始MSP存于`0x08005000`，Reset_Handler值存于`0x08005004`；后者为向量项地址，不是复位函数本身。Boot检查SRAM范围、Thumb位和APP地址范围，清理外设中断后设置VTOR/MSP并跳转。

认证格式：`HMAC-SHA256(key, "STM32OTA1" || LE32(version) || LE32(size) || bin)[:16]`。32字节密钥只链接进Boot，APP不链接`OtaAuth.c`，但这不构成硬件密钥隔离。CRC负责检测损坏；版本判断并不等同于硬件单调计数防回放。
