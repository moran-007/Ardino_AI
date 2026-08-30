# Ardino_AI · ESP32-S3 局域网独立语音对话

这是项目当前唯一保留的可运行版本。运行时 ESP32-S3 不依赖串口传输：按住 BOOT 录音，松开后通过 Wi-Fi 把 16 kHz PCM 发送给电脑；电脑离线识别、调用 DeepSeek、用 Windows 本地语音合成；ESP32 边下载边通过 MAX98357A 播放。串口只保留调试用途，启动后可以拔掉。

手机连接同一个 Wi-Fi 后，访问 `http://ESP32的IP/` 可查看最近四轮识别文本和 AI 完整回答。ESP32 只提供轻量入口，正文由电脑服务端直接送到手机浏览器，不占用 ESP32 内存；网页每 1.5 秒自动刷新，也可清除当前设备的对话历史。展开“AI 预设与配置”可切换 DeepSeek V4 Flash/Pro、最大输出、思考模式、系统提示词和 Windows TTS 声音，也可替换 API Key；已有 Key 从不回显。API 地址固定为 DeepSeek 官方端点，避免密钥被发送到其他服务器。

## 每次开机只做这两件事

1. 给 ESP32 和功放接通外部电源。ESP32 会自动连接已保存 Wi-Fi，并自动寻找电脑服务；不需要 USB 数据线，也不需要打开串口监视器。
2. 在电脑双击 `pc_server\start_server.cmd`，保持窗口运行。手机与 ESP32 需和电脑位于同一局域网。

电脑服务启动成功会显示 `DeepSeek API: configured`。之后按住 ESP32 的 BOOT 说话，松开即可。API Key、模型和系统提示词只属于电脑；Wi-Fi、电脑服务地址和音量只属于 ESP32。

## 文件位置

- ESP32 Arduino 主程序：`esp32_lan_device\esp32_lan_device.ino`
- ESP32 音频驱动：`esp32_lan_device\voice_input.cpp`、`voice_input.h`
- 电脑服务：`pc_server\lan_dialogue_server.py`
- 电脑首次配置：`pc_server\configure_server.ps1`
- 电脑日常启动：`pc_server\start_server.cmd`
- 电脑本地秘密配置：`pc_server\server_config.local.json`（不要分享）
- ESP32 编译脚本：`build_esp32.ps1`
- 云服务器部署调研：`docs\CLOUD_DEPLOYMENT_ROADMAP.md`（仅路线文档，云服务代码尚未实现）

以后添加云服务器实现时，建议新建顶层 `cloud_server\`，不要把云端依赖和 Windows 局域网服务混入同一个运行环境。

## 架构与边界

```text
INMP441 -> ESP32-S3 --局域网--> Windows：Zipformer ASR -> DeepSeek -> Windows TTS
MAX98357A <- ESP32-S3 <--流式 PCM-- Windows
```

- 语音识别和语音合成都在电脑离线执行；只有文字问题会发给 DeepSeek。
- 回答 PCM 不整段放入 ESP32 内存或闪存，而是直接流式播放，因此长度主要受 API 上限、电脑磁盘和 TTS 时间限制。
- `max_tokens` 可配置为 128..384000，默认 4096。设置越大，费用、等待时间和生成失败风险越高。
- 当前实现面向可信家庭局域网。若要部署到公网服务器，必须再加入 HTTPS、鉴权、限流和安全的密钥管理。
- ESP32-S3 上的 ESP-SR 适合唤醒词和有限命令，不适合代替本方案的任意中文连续听写。

## 接线

所有模块和 ESP32 必须共地。

| 模块引脚 | ESP32-S3 / 电源 | 说明 |
| --- | --- | --- |
| INMP441 VDD | 3V3 | 不接 5V |
| INMP441 GND | GND | 共地 |
| INMP441 SCK/BCLK | GPIO5 | 与功放共享时钟 |
| INMP441 WS/LRCL | GPIO4 | 与功放共享帧时钟 |
| INMP441 SD | GPIO6 | 麦克风数据输入 |
| INMP441 L/R | GND | 选择左声道 |
| MAX98357A VIN | 外部稳定 5V | 建议至少 1A，按喇叭功率留余量 |
| MAX98357A GND | GND | 与外部电源、ESP32 共地 |
| MAX98357A BCLK | GPIO5 | 与麦克风共享 |
| MAX98357A LRC | GPIO4 | 与麦克风共享 |
| MAX98357A DIN | GPIO7 | 播放数据输出 |
| MAX98357A SD/EN | GPIO15 | 静音与使能 |
| MAX98357A SPK+ / SPK- | 喇叭两端 | 喇叭任一端都不要接 GND |

BOOT 是 GPIO0，作为按住说话按钮。外部 5V 不要接 ESP32 的 3V3 引脚；若同时用 USB 给 ESP32 供电，只需让两个电源共地。上电、断电或改线前先移除功放电源。

以后加入 10k 电位器时：一端接 3V3，一端接 GND，中间滑臂接 GPIO1。接好后在调试串口执行 `/volume pot`；恢复控制台固定音量用 `/volume fixed`，直接设置固定音量用 `/volume 0..100`。未接电位器时不要启用电位器模式。

## 电脑端一次性准备

在 PowerShell 中进入 `pc_server`，依次运行：

```powershell
.\setup_server.ps1
.\configure_server.ps1
```

`setup_server.ps1` 会创建本地 Python 3.11 环境、安装依赖、下载约 350 MB 的中文 Zipformer 模型，并运行自动测试。虚拟环境和模型都保留在本机，不会提交到 GitHub。配置脚本会隐藏输入 DeepSeek API Key，并把本机配置写入不应分享的 `server_config.local.json`。Wi-Fi 名称和密码保存在 ESP32 现有 NVS 中，中文 SSID 不需要写入源码。

网页保存 AI 配置时同样只写电脑上的 `server_config.local.json`，API Key 不会写入 ESP32。若启用 `device_token`，手机页面和 ESP32 必须使用相同令牌；当前家庭局域网的默认配置未启用令牌，因此不要把 TCP 8765 暴露到公网。

Windows 防火墙需允许专用网络上的 TCP 8765 与 UDP 8764。随后双击 `start_server.cmd`，保持窗口运行。服务启动时会加载现有的高准确度 Zipformer 中文模型。

电脑端配置内容如下：

| 配置 | 保存位置 | 修改方法 |
| --- | --- | --- |
| DeepSeek API Key | 电脑 `server_config.local.json` | 首次运行 `configure_server.ps1`，以后也可在手机网页替换 |
| DeepSeek 模型/最大输出/思考模式 | 电脑 | 手机网页“AI 预设与配置” |
| 系统提示词/Windows TTS 声音 | 电脑 | 手机网页“AI 预设与配置” |
| HTTP 端口 | 电脑，默认 8765 | `configure_server.ps1` |
| UDP 自动发现端口 | 电脑，默认 8764 | `configure_server.ps1`；须与 ESP32 一致 |

## ESP32 常驻 BLE 配置与断联恢复

ESP32 启动后 BLE 一直保持广播，与 Wi-Fi 同时工作。设备名形如 `ESP32-LAN-AI-xxxx`。配对 PIN 会打印在首次烧录后的串口状态，也可在联网时访问 `http://ESP32的IP/info` 查看并记下。

可使用 Android/iOS 的 nRF Connect、LightBlue 等通用 BLE 工具：

1. 搜索并连接 `ESP32-LAN-AI-xxxx`，按提示输入 6 位 PIN。
2. 找到服务 `6f7a0001-7f9e-4a0b-9a8b-51f60f6d1000`。
3. 读取状态特征 `6f7a0004-7f9e-4a0b-9a8b-51f60f6d1000`，可看到 Wi-Fi 状态、SSID、ESP32 IP、手机网页地址、电脑服务地址和 RSSI；也可启用 Notify。
4. 向配置特征 `6f7a0002-7f9e-4a0b-9a8b-51f60f6d1000` 写入 UTF-8 JSON。密码不会被读回。
5. 向命令特征 `6f7a0003-7f9e-4a0b-9a8b-51f60f6d1000` 写入 UTF-8 文本 `status`、`connect`、`discover` 或 `reboot`。

配置新 Wi-Fi（支持中文 SSID）：

```json
{"ssid":"新的中文Wi-Fi","wifi_password":"新密码","connect":true}
```

电脑地址通常留空让 ESP32 自动发现：

```json
{"server_url":"","discovery_port":8764,"connect":true}
```

只有 UDP 广播被路由器隔离时才手动固定电脑地址：

```json
{"server_url":"http://192.168.3.9:8765","connect":true}
```

ESP32 端配置内容如下：

| 配置 | 保存位置 | 推荐设置 |
| --- | --- | --- |
| Wi-Fi SSID/密码 | ESP32 NVS | 通过 BLE 写入；支持中文 |
| 电脑服务地址 | ESP32 NVS | 默认留空/自动发现；无需填写网站域名 |
| UDP 发现端口 | ESP32 NVS | 8764 |
| 固定音量/电位器模式 | ESP32 NVS | BLE、串口调试命令均可修改 |
| API Key/模型/提示词 | 不保存在 ESP32 | 始终在电脑网页配置 |

断联处理顺序：

- Wi-Fi 短暂掉线：ESP32 自动重连；下一次按 BOOT 时也会再次连接。
- 更换路由器或密码：用 BLE 写入新 `ssid`、`wifi_password`，无需重新烧录。
- 电脑 IP 改变：ESP32 请求失败时会自动 UDP 发现新地址；也可用 BLE 命令 `discover`。
- 不知道 ESP32 新 IP：读取 BLE 状态特征中的 `ip` 或 `phone_url`。
- 网页能开但 AI 不回答：确认电脑 `start_server.cmd` 仍在运行且防火墙允许专用网络。
- BLE 断开：ESP32 会立即重新广播，可再次连接；BLE 不会因完成一次配置而关闭。

## 脱离 USB 数据线供电

固件不依赖 COM4 或串口数据。拔掉电脑 USB 数据线前，必须给 ESP32 提供独立电源：最稳妥是使用 5V、建议 2A 以上的 USB 充电器/充电宝连接开发板供电口；功放使用稳定 5V，并与 ESP32、麦克风共地。不要把 5V 接入 3V3 引脚，也不要在不确定开发板电源路径时同时从两个 5V 源反向供电。

拔掉数据线后，电脑仍通过 Wi-Fi 提供 Zipformer、DeepSeek 调用和 TTS。若电脑关机或 `start_server.cmd` 未运行，ESP32 的 BLE、Wi-Fi 和网页入口仍可用，但 AI 对话不会完成。

## ESP32 编译与运行

只编译、不烧录：

```powershell
.\build_esp32.ps1
```

确认 COM4 没有被 Arduino 串口监视器或旧语音桥占用后，才可手动上传：

```powershell
arduino-cli upload --fqbn "esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=huge_app,PSRAM=opi" -p COM4 .\esp32_lan_device
```

烧录后按复位键。蓝灯表示待机；按住 BOOT 说话时为红灯，松开后变为黄灯处理，绿色表示播放。播放或等待期间按 BOOT 可中止本轮设备端等待/播放。串口状态会打印手机网页地址，例如 `http://192.168.x.x/`。

可选调试命令：`/status`、`/discover`、`/server [URL]`、`/volume 0..100|pot|fixed`、`/voice`、`/connect`、`/reboot`。

## 验证

电脑端自动测试：

```powershell
.\pc_server\.venv\Scripts\python.exe -B -m unittest discover -s .\pc_server -p "test_*.py" -v
```

这些测试不会调用真实 DeepSeek，也不会播放声音；它们验证音频前处理、配置边界、PCM/WAV 转换、设备令牌、异步任务状态和回答音频下载。最终硬件验收仍需接回功放并在外部稳定电源下实测一次完整按键对话。
