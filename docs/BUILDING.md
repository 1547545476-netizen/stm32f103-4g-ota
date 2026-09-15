# 构建与使用

## 1. 工具与配置

本工程使用Keil MDK的ARMCC 5.06，Boot目标为`Target 1`，APP目标为`AppTest`。不附带编译器、器件包或许可证，ARMClang/GCC不能直接替换当前工程工具链。

在PowerShell中，按自己的安装路径设置环境变量：

```powershell
$env:KEIL_ROOT = 'C:\Keil_v5'
python -m pip install -r requirements.txt
python tools/ota/generate_device_key.py
```

若要让VS Code任务继承这个变量，从上述终端启动VS Code，或设置用户环境变量后重启VS Code。IntelliSense配置只辅助编辑，以Keil编译结果为准。

生成的`User/OtaSecrets.h`只保存在本机，Boot和Python发布工具使用相同HMAC密钥。MQTT的ClientId、用户名、密码和Topic需自行填写。可以隐藏输入密码：

```powershell
python tools/ota/emqx_mqtt_config.py --endpoint mqtt.example.invalid --username device-user
```

示例域名不可连接，必须替换成自己的Broker。该工具会输出Topic名称，但不会回显密码或HMAC密钥。生成新配置适用于复现实验；维护原设备时须使用原设备对应的本机配置。

## 2. 编译

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/vscode/keil-build.ps1 -Mode rebuild
powershell -NoProfile -ExecutionPolicy Bypass -File tools/vscode/keil-app-build.ps1 -Mode rebuild
python tools/ci/check_project.py
```

- Boot生成`Objects/Project.axf`，APP生成`AppTest/Objects/AppTest.bin`。
- VS Code菜单“终端 -> 运行任务”提供Boot和APP构建入口。
- `User/main.h`定义分区；APP的IROM Start必须为`0x08005000`，Size为`0xB000`。
- 首次烧Boot使用Keil与自己的ST-Link配置。发布包不含原作者调试器序列号、`.uvoptx`或`.uvguix`。烧录前检查擦除范围，避免误擦已有APP。
- 全部构建产物都留在本机。Boot含设备HMAC密钥；APP也可能含MQTT凭据，均不得直接放到公共仓库。

## 3. 串口升级

USART1连接USB转串口，Air780E独占USART2。实际引脚定义以`Hardware/Serial.c`、`Uart2.c`、`MySPI.c`、`MyI2C.c`为准。先验证供电、电平兼容与共地，不要把5V直接接入仅允许3.3V的接口。

APP编译后，在电脑端运行：

```powershell
python tools/ota/serial_ota_sender.py --port COM3 --bin AppTest/Objects/AppTest.bin --version 6
```

按实际串口号和`AppTest/main.c`中的APP_VERSION修改参数。运行前关闭占用相同COM口的串口助手；启动发送工具后按它的提示复位板子进入Boot接收窗口。`--help`查看等待和超时参数。

协议为`55 AA | cmd | seq | offset | payload_len | payload | crc16`。START元数据包含16字节HMAC标签，DATA正文最大200字节，END执行完整镜像校验。串口脚本处理板子文本ACK，不需要另一串口助手代收ACK。

## 4. MQTT与HTTPS

Air780E AT接口具有型号/固件差异，应核对实际`ATI`版本及厂商手册。代码曾按Air780EPM系列V2007响应联调，不能直接承诺其他型号兼容。

MQTT TLS使用SSL上下文88，CA路径为`/USER/HTTP/emqxsl-ca.crt`。切换Broker时应提供正确CA，必要时调整`Air780E_Mqtt.c`中的CA路径；CA文件名本身不代表它可验证任意服务器。CA安装测试开关位于`User/main.h`，配置URL/大小后启用，安装成功再关闭测试开关。CA公开证书可以共享，私钥不可以。

本地发布工具用`--ca`接收CA文件路径，仓库不预置本机证书文件。上传自己构建的bin到对象存储后：

```powershell
python tools/ota/mqtt_ota_publish.py --bin AppTest/Objects/AppTest.bin --version 6 --url https://example.invalid/firmware/App_v6.bin --host mqtt.example.invalid --port 8883 --username device-user --ca path/to/broker-ca.crt --topic ota/your-device/cmd --status-topic ota/your-device/status
```

上述域名和路径均为占位符。工具隐藏询问密码，读取本机密钥计算auth，检查远端大小/CRC后发布JSON。设备必须使用相同Topic与密钥，并且目标版本高于当前APP版本。

`accepted`只代表接受命令，`ready`代表镜像已暂存；应继续核对Boot安装日志、APP版本及健康确认，不能只凭MQTT发布成功判断OTA成功。设备订阅QoS为0，即便电脑向Broker发布QoS1，转发到该订阅时也不保证QoS1交付。当前不是持久在线服务，错过等待窗口需要再次发布。

## 5. 提交前检查

配置生成后目录中存在本机密钥，直接全目录扫描会按设计报错。日常Git提交检查使用：

```powershell
git add .
python tools/ci/check_public_tree.py --tracked
git diff --cached --stat
```

扫描通过后再提交。不要用`git add -f`添加密钥或构建产物。首次新建仓库请只从这份清理后的目录开始，不要复制开发目录的`.git`。
