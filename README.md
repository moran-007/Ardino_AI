# Ardino_AI · ESP32-S3 局域网与公网语音对话

项目保留两套互不覆盖的固件：`esp32_lan_device/` 是已部署的局域网稳定版，配合 Windows `pc_server/`；`esp32_cloud_device/` 是整合后的公网最新版，配合 `cloud_server/`。两套固件接线一致，但网络协议、BLE UUID 和 NVS 配置不同，烧录时必须选对目录。

局域网版运行时 ESP32-S3 不依赖串口传输：按住 BOOT 录音，松开后通过 Wi-Fi 把 16 kHz PCM 发送给电脑；电脑离线识别、调用 DeepSeek、用 Windows 本地语音合成；ESP32 边下载边通过 MAX98357A 播放。公网版把 ASR、LLM 和分段 TTS 交给云端服务，并在设备网页显示实时流程、当前对话和本地历史记录。

手机连接同一个 Wi-Fi 后，访问 `http://ESP32的IP/` 可查看最近四轮识别文本和 AI 完整回答。ESP32 只提供轻量入口，正文由电脑服务端直接送到手机浏览器，不占用 ESP32 内存；网页每 1.5 秒自动刷新，也可清除当前设备的对话历史。展开“AI 预设与配置”可切换 DeepSeek V4 Flash/Pro、最大输出、思考模式、系统提示词和 Windows TTS 声音，也可替换 API Key；已有 Key 从不回显。API 地址固定为 DeepSeek 官方端点，避免密钥被发送到其他服务器。

## 局域网版每次开机只做这两件事

1. 给 ESP32 和功放接通外部电源。ESP32 会自动连接已保存 Wi-Fi，并自动寻找电脑服务；不需要 USB 数据线，也不需要打开串口监视器。
2. 在电脑双击 `pc_server\start_server.cmd`，保持窗口运行。手机与 ESP32 需和电脑位于同一局域网。

电脑服务启动成功会显示 `DeepSeek API: configured`。之后按住 ESP32 的 BOOT 说话，松开即可。API Key、模型和系统提示词只属于电脑；Wi-Fi、电脑服务地址和音量只属于 ESP32。

## 本地服务完整启动与检查

以下命令都从项目根目录执行。第一次使用或更新 Python 依赖后，先运行完整环境检查：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\pc_server\setup_server.ps1
```

该脚本会检查 Python 3.11 虚拟环境、安装或确认依赖、下载或确认 Zipformer 模型，运行 5 项自动测试，并实际加载模型。最终出现下面这行才算准备完成：

```text
LAN server environment check passed.
```

如果还没有本地配置，或需要修改 DeepSeek Key、模型、端口、提示词和音色，再运行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\pc_server\configure_server.ps1
```

日常启动可以双击 `pc_server\start_server.cmd`，也可以在项目根目录执行：

```powershell
.\pc_server\start_server.cmd
```

模型加载需要数秒。服务窗口必须保持运行；看到局域网地址、`UDP discovery port: 8764` 和 `DeepSeek API: configured` 后，可在另一个 PowerShell 窗口检查：

```powershell
Invoke-RestMethod http://127.0.0.1:8765/health
```

正常结果中应包含：

```json
{
  "ok": true,
  "service": "esp32-lan-dialogue",
  "asr": "zipformer-ctc-zh-int8-2025-07-03",
  "api_configured": true
}
```

查看电脑当前实际联网网卡和局域网 IPv4：

```powershell
Get-NetIPConfiguration |
  Where-Object { $_.IPv4DefaultGateway -and $_.NetAdapter.Status -eq "Up" } |
  Select-Object InterfaceAlias, @{Name="IPv4"; Expression={$_.IPv4Address.IPAddress}}
```

忽略虚拟网卡，选择 WLAN 或以太网地址。手机或同一局域网中的其他电脑可访问：

```text
http://电脑的局域网IPv4:8765/health
```

本项目在 2026-08-30 完整实测了以下启动条件：5 项自动测试通过、真实 Zipformer 模型加载成功、TCP 8765 与 UDP 8764 正常监听、局域网健康接口返回 200、手机页面返回 200、UDP 自动发现返回 `ESP32_AI_SERVER_V1 8765`。检查过程未回显 API Key。

停止服务时，在 `start_server.cmd` 窗口按 `Ctrl+C`，确认后关闭窗口。不要重复启动多个实例；若提示端口已占用，可查找当前监听进程：

```powershell
Get-NetTCPConnection -State Listen -LocalPort 8765 |
  Select-Object LocalAddress, LocalPort, OwningProcess
```

若本机健康检查成功、但手机或 ESP32 无法访问，请确认三台设备在同一局域网，并允许 Windows 防火墙在“专用网络”上接收 TCP 8765 和 UDP 8764。不要把这两个端口直接映射到公网。

## 本地服务命令速查

以下命令都在项目根目录的 PowerShell 中执行。

### 首次准备或依赖更新

```powershell
# 创建/更新虚拟环境、安装依赖、确认模型、运行测试并加载模型
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\pc_server\setup_server.ps1

# 首次写入配置，或修改 Key、模型、端口、提示词与音色
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\pc_server\configure_server.ps1
```

### 启动服务

```powershell
# 推荐：前台启动，窗口中可以直接查看日志，按 Ctrl+C 停止
.\pc_server\start_server.cmd
```

不要在服务已经监听 8765 时重复启动。

### 查看健康状态与脱敏配置

```powershell
# 本机健康检查
Invoke-RestMethod http://127.0.0.1:8765/health

# 只返回脱敏后的公开配置；API Key 只显示 configured 或 missing
Invoke-RestMethod http://127.0.0.1:8765/config/public
```

### 查看局域网 IP

```powershell
Get-NetIPConfiguration |
  Where-Object { $_.IPv4DefaultGateway -and $_.NetAdapter.Status -eq "Up" } |
  Select-Object InterfaceAlias, @{Name="IPv4"; Expression={$_.IPv4Address.IPAddress}}
```

选择实际联网的 WLAN 或以太网地址，然后从同一局域网的手机或电脑检查：

```powershell
Invoke-RestMethod http://电脑的局域网IPv4:8765/health
```

### 查看端口和服务进程

```powershell
# TCP 服务端口
Get-NetTCPConnection -State Listen -LocalPort 8765 |
  Select-Object LocalAddress, LocalPort, OwningProcess

# UDP 自动发现端口
Get-NetUDPEndpoint -LocalPort 8764 |
  Select-Object LocalAddress, LocalPort, OwningProcess

# 查看占用 TCP 8765 的进程
$serverProcessId = (Get-NetTCPConnection -State Listen -LocalPort 8765).OwningProcess
Get-Process -Id $serverProcessId
```

### 手动验证 UDP 自动发现

```powershell
$discoveryClient = [Net.Sockets.UdpClient]::new()
try {
  $discoveryClient.Client.ReceiveTimeout = 3000
  $discoveryClient.Connect("127.0.0.1", 8764)
  $request = [Text.Encoding]::ASCII.GetBytes("ESP32_AI_DISCOVER_V1")
  [void]$discoveryClient.Send($request, $request.Length)
  $remote = [Net.IPEndPoint]::new([Net.IPAddress]::Any, 0)
  $reply = $discoveryClient.Receive([ref]$remote)
  [Text.Encoding]::ASCII.GetString($reply)
}
finally {
  $discoveryClient.Dispose()
}
```

正常回复：

```text
ESP32_AI_SERVER_V1 8765
```

### 运行测试和只检查环境

```powershell
# 运行全部 5 项自动测试；不会调用真实 DeepSeek，也不会播放声音
.\pc_server\.venv\Scripts\python.exe -B -m unittest discover -s .\pc_server -p "test_*.py" -v

# 检查 Python 依赖是否冲突
.\pc_server\.venv\Scripts\python.exe -m pip check

# 加载本地配置和真实 Zipformer 模型后退出，不启动端口
.\pc_server\.venv\Scripts\python.exe -B .\pc_server\lan_dialogue_server.py --check
```

### 停止服务

正常停止：回到 `start_server.cmd` 窗口按 `Ctrl+C`，输入 `Y` 确认后关闭窗口。

只有服务在后台运行、窗口丢失或无法响应时，才根据监听端口停止进程：

```powershell
$listener = Get-NetTCPConnection -State Listen -LocalPort 8765 -ErrorAction SilentlyContinue
if ($listener) {
  Stop-Process -Id $listener.OwningProcess
}
```

停止后确认 TCP 和 UDP 端口均已释放：

```powershell
Get-NetTCPConnection -State Listen -LocalPort 8765 -ErrorAction SilentlyContinue
Get-NetUDPEndpoint -LocalPort 8764 -ErrorAction SilentlyContinue
```

### Windows 防火墙检查与专用网络放行

先检查现有规则，不要重复创建：

```powershell
Get-NetFirewallRule -DisplayName "Ardino_AI LAN*" -ErrorAction SilentlyContinue |
  Select-Object DisplayName, Enabled, Profile, Direction, Action
```

只有局域网设备无法访问、并确认是防火墙拦截时，才在“以管理员身份运行”的 PowerShell 中添加仅限专用网络的规则：

```powershell
New-NetFirewallRule -DisplayName "Ardino_AI LAN TCP 8765" `
  -Direction Inbound -Action Allow -Profile Private -Protocol TCP -LocalPort 8765

New-NetFirewallRule -DisplayName "Ardino_AI LAN UDP 8764" `
  -Direction Inbound -Action Allow -Profile Private -Protocol UDP -LocalPort 8764
```

需要撤销这些规则时：

```powershell
Remove-NetFirewallRule -DisplayName "Ardino_AI LAN TCP 8765"
Remove-NetFirewallRule -DisplayName "Ardino_AI LAN UDP 8764"
```

## 文件位置

- 局域网 ESP32 主程序：`esp32_lan_device\esp32_lan_device.ino`
- 公网 ESP32 主程序：`esp32_cloud_device\esp32_cloud_device.ino`
- 两套音频驱动：各固件目录内的 `voice_input.cpp`、`voice_input.h`（云端版增加 I2S/DMA 释放与恢复，不要混用）
- 电脑服务：`pc_server\lan_dialogue_server.py`
- 电脑首次配置：`pc_server\configure_server.ps1`
- 电脑日常启动：`pc_server\start_server.cmd`
- 电脑本地秘密配置：`pc_server\server_config.local.json`（不要分享）
- ESP32 编译脚本：`build_esp32.ps1`
- 云服务器部署调研：`docs\CLOUD_DEPLOYMENT_ROADMAP.md`（早期路线与服务器现状记录）
- 低成本 HTTP 云端执行流程：`docs\LOW_COST_HTTP_CLOUD_EXECUTION_FLOW.md`（按阶段实施、验收和回滚）
- 云端服务与电脑模拟：`cloud_server\README.md`（已实现，默认 mock 测试不产生 API 费用）
- ESP32 公网固件说明：`esp32_cloud_device\README.md`（候选 1/2/3 已整合为此唯一目录）

云端依赖和 Windows 局域网服务保持独立运行环境；本地密钥、模型、数据库和生成音频由 `.gitignore` 排除，不上传 GitHub。

## 架构与边界

```text
INMP441 -> ESP32-S3 --局域网--> Windows：Zipformer ASR -> DeepSeek -> Windows TTS
MAX98357A <- ESP32-S3 <--流式 PCM-- Windows

INMP441 -> ESP32-S3 --HTTP/HTTPS--> cloud_server：ASR -> LLM 主备 -> 分段 TTS
MAX98357A <- ESP32-S3 <--按句 PCM 分段-- cloud_server
```

- 语音识别和语音合成都在电脑离线执行；只有文字问题会发给 DeepSeek。
- 回答 PCM 不整段放入 ESP32 内存或闪存，而是直接流式播放，因此长度主要受 API 上限、电脑磁盘和 TTS 时间限制。
- `max_tokens` 可配置为 128..384000，默认 4096。设置越大，费用、等待时间和生成失败风险越高。
- 局域网版面向可信家庭网络；公网版已实现设备 Token、资源隔离、限流和 HTTPS 探测，但长期使用前仍应确认 Nginx 全链路 HTTPS，不要长期依赖 HTTP 回退。
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

## 局域网 ESP32 配置命令速查

### 打开串口控制台

先查看开发板端口：

```powershell
arduino-cli board list
```

将示例中的 `COM4` 换成实际端口，以 460800 波特率打开控制台：

```powershell
arduino-cli monitor -p COM4 --config baudrate=460800
```

输入命令后发送换行；按 `Ctrl+C` 退出串口监视器。串口仅用于调试，局域网运行时可以拔掉 USB 数据线。

### ESP32 串口命令

| 命令 | 作用 | 是否保存到 NVS |
| --- | --- | --- |
| `/status` | 显示 Wi-Fi、SSID、ESP32 IP、手机页面、电脑服务地址、发现端口、音量、I2S 引脚、BLE 名称和 PIN | 否 |
| `/discover` | 立即通过 UDP 8764 自动发现电脑服务，并保存发现到的地址 | 是 |
| `/server` | 显示当前电脑服务地址；空值表示自动发现 | 否 |
| `/server http://192.168.3.9:8765` | 手动设置并保存电脑服务地址 | 是 |
| `/volume 0..100` | 设置并保存固定音量百分比，同时关闭电位器模式 | 是 |
| `/volume pot` | 启用并保存 GPIO1 电位器音量模式 | 是 |
| `/volume fixed` | 关闭电位器模式，恢复已保存的固定音量 | 是 |
| `/voice` | 从控制台触发一轮对话；按提示立即按住 BOOT 说话，松开结束 | 否 |
| `/connect` | 使用已保存的 SSID 和密码重新连接 Wi-Fi | 否 |
| `/reboot` | 立即重启 ESP32 | 否 |

当前 LAN 固件没有串口 Wi-Fi 设置命令。更换 Wi-Fi 名称或密码时，必须使用下面的 BLE 配置特征。需要把固定电脑地址恢复为自动发现时，也应通过 BLE 写入空的 `server_url`。

### ESP32 常驻 BLE 配置

ESP32 启动后 BLE 一直保持广播，与 Wi-Fi 同时工作。设备名形如 `ESP32-LAN-AI-xxxx`。配对 PIN 会打印在首次烧录后的串口状态，也可在联网时访问 `http://ESP32的IP/info` 查看并记下。

可使用 Android/iOS 的 nRF Connect、LightBlue 等通用 BLE 工具：

1. 搜索并连接 `ESP32-LAN-AI-xxxx`，按提示输入 6 位 PIN。
2. 找到服务 `6f7a0001-7f9e-4a0b-9a8b-51f60f6d1000`。
3. 读取状态特征 `6f7a0004-7f9e-4a0b-9a8b-51f60f6d1000`，可看到 Wi-Fi 状态、SSID、ESP32 IP、手机网页地址、电脑服务地址和 RSSI；也可启用 Notify。
4. 向配置特征 `6f7a0002-7f9e-4a0b-9a8b-51f60f6d1000` 写入 UTF-8 JSON。密码不会被读回。
5. 向命令特征 `6f7a0003-7f9e-4a0b-9a8b-51f60f6d1000` 写入 UTF-8 文本 `status`、`connect`、`discover` 或 `reboot`。

BLE 服务和特征速查：

| 类型 | UUID | 操作 | 内容 |
| --- | --- | --- | --- |
| 服务 | `6f7a0001-7f9e-4a0b-9a8b-51f60f6d1000` | — | Ardino_AI 配置服务 |
| 配置 | `6f7a0002-7f9e-4a0b-9a8b-51f60f6d1000` | 加密读/写 | 写入配置 JSON；读取时密码只返回是否已配置 |
| 命令 | `6f7a0003-7f9e-4a0b-9a8b-51f60f6d1000` | 加密写 | UTF-8 文本命令 `status`、`connect`、`discover`、`reboot` |
| 状态 | `6f7a0004-7f9e-4a0b-9a8b-51f60f6d1000` | 加密读/Notify | Wi-Fi、SSID、IP、RSSI、手机页面、服务地址、发现端口和状态消息 |

配置 JSON 支持的字段：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `ssid` | 字符串 | Wi-Fi 名称，最长 32 bytes；UTF-8 中文通常每字 3 bytes |
| `wifi_password` | 字符串 | Wi-Fi 密码，最长 63 bytes |
| `server_url` | 字符串 | 电脑服务地址；留空启用 UDP 自动发现，手动地址必须以 `http://` 开头 |
| `lan_server` | 字符串 | `server_url` 的兼容别名，新配置优先使用 `server_url` |
| `discovery_port` | 整数 | UDP 自动发现端口，范围 1..65535，默认 8764 |
| `volume_percent` | 整数 | 固定音量，超出范围时自动限制为 0..100，并切换到固定音量模式 |
| `volume_mode` | 字符串 | `fixed` 或 `pot`；`pot` 使用 GPIO1 电位器 |
| `connect` | 布尔值 | `true` 保存后立即连接；`false` 只保存；省略时默认为 `true` |

配置 JSON 总长度不得超过 768 bytes。使用 BLE App 时优先选择 Long Write/长写入。

一次配置 Wi-Fi、自动发现和固定音量：

```json
{"ssid":"新的中文Wi-Fi","wifi_password":"新密码","server_url":"","discovery_port":8764,"volume_percent":50,"volume_mode":"fixed","connect":true}
```

恢复电脑服务自动发现，不修改其他字段：

```json
{"server_url":"","discovery_port":8764,"connect":true}
```

只有 UDP 广播被路由器隔离时才手动固定电脑地址：

```json
{"server_url":"http://192.168.3.9:8765","connect":true}
```

修改固定音量但暂不重连 Wi-Fi：

```json
{"volume_percent":35,"volume_mode":"fixed","connect":false}
```

启用 GPIO1 电位器音量：

```json
{"volume_mode":"pot","connect":false}
```

BLE 命令特征只接受纯文本，不是 JSON：

```text
status
connect
discover
reboot
```

常见状态消息：`config_saved`、`connected`、`wifi_failed`、`server_discovered`、`server_not_found`、`invalid_json`、`invalid_config`、`invalid_discovery_port`、`invalid_volume_mode`、`save_failed`、`unknown_command`。

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

## 云端 ESP32 配置命令速查

云端固件的设备名形如 `ESP32-CLOUD-xxxx`，6 位配对 PIN 只在串口输出。若设备 NVS 中已有局域网版 Wi-Fi，首次启动会迁移 Wi-Fi 和音量；否则使用 BLE 完成首次配置。联网后也可打开串口打印的 `http://ESP32的局域网IP/`，填写同一个 PIN 后保存服务器、设备凭证、音色、语速和音量。

### 云端 BLE UUID

| 类型 | UUID | 操作 |
| --- | --- | --- |
| 服务 | `7b210001-4184-4ea4-a359-856aee830000` | 云端配置服务 |
| 配置 | `7b210002-4184-4ea4-a359-856aee830000` | 加密读/写 UTF-8 JSON |
| 命令 | `7b210003-4184-4ea4-a359-856aee830000` | 加密写 UTF-8 文本命令 |
| 状态 | `7b210004-4184-4ea4-a359-856aee830000` | 加密读/Notify；Token 不回显 |

向配置特征写入的完整示例：

```json
{"ssid":"家庭Wi-Fi","wifi_password":"无线密码","server_url":"https://voice.bsnlch.xyz","device_id":"esp32-01","device_token":"注册设备时仅显示一次的Token","chinese_speaker_id":2,"english_speaker_id":128,"speech_speed":0.9,"volume_percent":50,"volume_mode":"fixed","connect":true}
```

支持字段：`ssid`、`wifi_password`、`server_url`、`device_id`、`device_token`、`chinese_speaker_id`（0..4）、`english_speaker_id`（0..903）、`speech_speed`（0.5..2.0）、`volume_percent`（0..100）、`volume_mode`（`fixed`/`pot`）和 `connect`。总 JSON 不得超过 768 bytes，建议使用 BLE Long Write。

云端 BLE 命令特征接受：

```text
status
connect
sync_voice
reboot
```

云端串口命令接受：

```text
/status
/voice
/connect
/sync_voice
/reboot
```

`sync_voice` 会使用设备 Token 把中英文音色与语速同步到服务器。需要预置首次烧录凭证时，可在本机创建 `esp32_cloud_device/secrets.h`，定义 `CLOUD_PROVISIONED_DEVICE_ID` 和 `CLOUD_PROVISIONED_DEVICE_TOKEN`；该文件已被 Git 忽略，不能上传或分享。

## 局域网版脱离 USB 数据线供电

固件不依赖 COM4 或串口数据。拔掉电脑 USB 数据线前，必须给 ESP32 提供独立电源：最稳妥是使用 5V、建议 2A 以上的 USB 充电器/充电宝连接开发板供电口；功放使用稳定 5V，并与 ESP32、麦克风共地。不要把 5V 接入 3V3 引脚，也不要在不确定开发板电源路径时同时从两个 5V 源反向供电。

拔掉数据线后，电脑仍通过 Wi-Fi 提供 Zipformer、DeepSeek 调用和 TTS。若电脑关机或 `start_server.cmd` 未运行，ESP32 的 BLE、Wi-Fi 和网页入口仍可用，但 AI 对话不会完成。

## ESP32 编译与运行

构建脚本默认编译局域网版；用 `-Target cloud` 选择公网版。两条命令都只编译、不烧录：

```powershell
.\build_esp32.ps1
.\build_esp32.ps1 -Target cloud
```

确认 COM4 没有被 Arduino 串口监视器占用后，按需要手动上传其中一套固件：

```powershell
# 局域网稳定版
arduino-cli upload --fqbn "esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=huge_app,PSRAM=opi" -p COM4 .\esp32_lan_device

# 公网最新版
arduino-cli upload --fqbn "esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=huge_app,PSRAM=opi" -p COM4 .\esp32_cloud_device
```

烧录后按复位键。两套固件均使用蓝灯待机、红灯录音、黄灯处理、绿灯播放；公网版错误状态为紫红色。播放或等待期间按 BOOT 可中止本轮设备端等待/播放。串口会打印设备网页地址，例如 `http://192.168.x.x/`。

局域网版和公网版的 BLE UUID、JSON 字段与串口命令不同，分别使用上面的两节速查表。

## 验证

电脑端自动测试：

```powershell
# 局域网 Windows 服务
.\pc_server\.venv\Scripts\python.exe -B -m unittest discover -s .\pc_server -p "test_*.py" -v

# 公网服务、设备鉴权和分段协议（mock，不调用付费 API）
.\pc_server\.venv\Scripts\python.exe -B -m unittest discover -s .\cloud_server\tests -p "test_*.py" -v
```

这些测试不会调用真实 DeepSeek，也不会播放声音；它们验证音频前处理、配置边界、PCM/WAV 转换、设备令牌、资源隔离、异步任务状态、句级分段和回答音频下载。最终硬件验收仍需接回功放并在外部稳定电源下分别实测需要使用的固件。
