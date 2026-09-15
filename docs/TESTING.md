# 验证记录与待验证范围

## 历史上板记录

开发过程已记录串口OTA、4G入网、HTTPS读取、MQTT订阅/发布，以及2026-08-18 APP v5到v6的远程升级闭环。该次固件大小18,280字节，CRC32为`0x839EF916`，APP链接地址`0x08005000`，Reset_Handler值`0x08005105`。

发布包排除了历史bin、设备账号、真实对象URL和原始本机日志。上述是历史记录摘要，不是本源码全部新增机制已经上板通过的声明。HMAC版本不能直接套用旧版不含auth的发布清单。

## 无硬件检查

GitHub Actions运行Python语法检查、源码分区检查、发布侧认证固定向量检查，以及Git暂存内容的敏感文件扫描。Actions不包含Keil许可证，不能代替ARMCC固件构建，也不模拟真实Flash掉电行为。

```sh
python -m compileall -q tools
python tools/ci/check_project.py --source-only
python tools/ci/test_firmware_auth.py
python tools/ci/check_public_tree.py --tracked
```

认证向量检查当前运行的是Python发布侧逻辑；不能声称已经对所有C加密路径做了主机单元测试。

## 当前版本应补齐的实物测试

| 场景 | 应记录的结果 |
| --- | --- |
| 下载、备份、安装和回滚各阶段断电 | 上电状态、恢复路径、最终运行版本 |
| EEPROM A/B写入途中断电 | 哪份记录有效，sequence是否正确选择 |
| 新APP确认前复位、无Delay死循环 | IWDG复位、计数与旧版本恢复 |
| bin/版本/auth被修改 | 擦除APP前拒绝，旧APP是否仍可运行 |
| 超大镜像、坏向量、错误地址 | 边界拒绝，不覆盖Boot和其他槽位 |
| 弱网、404、CA错误、模块文件系统故障 | 有限超时、错误报告和下次重试 |
| 持续循环升级 | 寿命、栈空间、重连与业务阻塞影响 |

测试报告应绑定Git提交、Boot和APP版本、模块ATI、硬件接线、输入、预期、实际日志与结论。不要将“编译0错误”替换为“硬件验证通过”。

## 源码走读发现的待修问题

本发布快照保留当前实现，以下问题尚未修复，不应直接用于生产设备。

- `Hardware/OtaService.c`的`OtaService_ConfirmRunningApp()`先修改RAM中的DONE状态，再提交EEPROM。提交失败后，下次调用可能误认为已经确认。需要使用候选控制块，持久化成功后再更新运行状态，并注入EEPROM写失败验证。
- `Hardware/Boot.c`的`BootLoader_Branch()`在部分控制块提交失败时返回，外层`User/main.c`仍可能根据非ERROR状态和基础向量检查跳APP。需要统一启动许可结果，避免部分安装失败路径进入不完整镜像。
- 当前健康确认只检查约5轮示例业务循环；`Delay_ms()`中无条件喂IWDG可能掩盖仍执行延时的业务死循环。看门狗由APP开启，开启前的启动故障尚未覆盖。应建立关键任务进展检查及Boot到APP的看门狗生命周期设计。
