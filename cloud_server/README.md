# ESP32 云端语音服务

这是与 `pc_server` 完全隔离的公网版本。当前第一版链路为：

```text
ESP32/电脑模拟器 -> 设备鉴权 -> ASR -> DeepSeek -> Qwen -> GLM
                                      -> 中英双 VITS 路由 -> 16 kHz PCM 分段
ESP32/电脑模拟器 <- 每生成一段就轮询、下载并立即播放
```

服务只在本机/服务器保存大模型厂商 Key。ESP32 只保存 Wi-Fi、`device_id` 和该设备自己的 Token。数据库只保存 Token 的 HMAC-SHA256 摘要；注册时的明文 Token 只显示一次。

## 已实现

- `POST /voice-api/v1/dialogue`：上传 16 kHz、16-bit、单声道裸 PCM。
- `GET /voice-api/v1/jobs/{id}/segments?after=N`：按序获得已就绪的句级音频段。
- `GET /voice-api/v1/jobs/{id}/segments/{index}/audio`：下载长度确定的 16 kHz PCM，适配 ESP32 当前播放方式。
- `GET /voice-api/v1/jobs/{id}/audio`：任务完成后的合并音频兼容接口。
- 任务详情同时返回数据库字段 `question`/`answer` 和 ESP32 兼容字段 `recognized_text`/`answer_text`。
- `POST /voice-api/v1/translate`：使用相同 LLM 主备链路完成文字翻译。
- `GET/PUT /voice-api/v1/device-settings`：设备用自己的 Token 读取/修改中文音色、英文音色和语速。
- 每设备独立 Bearer Token、资源归属校验、每分钟限流、上传大小限制。
- DeepSeek 主用、通义千问备用、GLM 第三备用；均走 OpenAI 兼容接口。
- SenseVoice、本地中文 VITS 与 Piper 英文 VITS；混合文本按语种路由后统一为 ESP32 所需的 16 kHz PCM。
- 中文音色（0～4）、英文音色（0～903）和语速（0.5～2.0）按设备 ID 隔离，不修改服务器全局值。
- 无费用 mock Provider、电脑 ESP32 协议模拟器、自动测试。
- 单密码管理员页面：无用户名，支持接口开关、管理员密码修改、三家 LLM API、ASR/TTS Provider、本地模型路径、VITS 音色与语速；Key 加密保存且不回显。

## 管理员配置页

部署后的入口为：

```text
http://voice.bsnlch.xyz/admin/
```

输入唯一管理员密码即可解锁，不需要用户名。会话有效 8 小时，也可主动“锁定页面”。设备表可代为调整每台设备的角色、记忆、中文/英文音色和语速；ESP32 也能只修改自己的语音设置。页面保存后会自动重载 Provider；API Key 输入框留空表示保持原值，勾选“清除”才会删除。切换大型本地模型时若仍有任务运行，服务会拒绝切换，避免中途释放模型。

配置文件保存在 `VOICE_DATA_DIR/admin_config.enc`，由 `VOICE_CONFIG_KEY` 使用 Fernet 加密；管理员密码使用 PBKDF2-SHA256 摘要。初始密码可在页面中修改。当前 HTTP 只适合短期联调，因为密码和 Key 的传输没有 TLS 保护；申请证书后应将 `VOICE_ADMIN_COOKIE_SECURE=true` 并切换 HTTPS。

## 电脑端无费用测试

从项目根目录运行自动测试：

```powershell
.\pc_server\.venv\Scripts\python.exe -B -m unittest discover -s .\cloud_server\tests -p "test_*.py" -v
```

启动 mock 服务：

```powershell
.\cloud_server\start_mock_server.ps1
```

第一次先在另一个 PowerShell 注册模拟设备。下面的 Pepper 必须与 `start_mock_server.ps1` 一致：

```powershell
$env:VOICE_AUTH_PEPPER = "local-simulator-only-pepper"
$env:VOICE_DATA_DIR = (Resolve-Path .\cloud_server).Path + "\data"
.\pc_server\.venv\Scripts\python.exe -m cloud_server.cli register-device pc-simulator-01 --name "PC simulator"
```

保存输出的 Token，并运行模拟器：

```powershell
.\pc_server\.venv\Scripts\python.exe -m cloud_server.simulator.esp32_simulator `
  --url http://127.0.0.1:18765 `
  --device-id pc-simulator-01 `
  --token "刚才生成的Token" `
  --question "请用两句话介绍你自己" `
  --output .\cloud_server\output\simulator-answer.wav
```

mock 模式不会调用真实 ASR、TTS 或厂商 API，也不会产生 API 费用。它会完整验证 HTTP、鉴权、任务状态、句级分段、分段长度和 WAV 拼接。

### 本地 VITS 真实压测

可以使用 [sherpa-onnx 官方 VITS 模型页](https://k2-fsa.github.io/sherpa/onnx/tts/pretrained_models/vits.html)列出的 `sherpa-onnx-vits-zh-ll`（中文、5 个 speaker、16 kHz）验证真实合成。模型放到 `cloud_server/models/sherpa-onnx-vits-zh-ll/` 后执行：

```powershell
.\pc_server\.venv\Scripts\python.exe -m cloud_server.benchmark_tts `
  --model-dir .\cloud_server\models\sherpa-onnx-vits-zh-ll `
  --speaker-ids 0,1,2,3,4 `
  --rounds 3
```

输出包含加载耗时、工作集内存、每轮音频时长和 RTF，并为 5 个音色分别生成 WAV。RTF 小于 1 表示合成速度快于音频播放速度。该社区模型压缩包没有附带明确的商业授权文件，所以只能用于技术压测；产品上线前必须换成许可证清晰且允许目标用途的模型。

2026-08-30 实测结果：本机 5 个中文 speaker 各运行 3 轮，平均 RTF 为 0.354～0.402；双模型混合句生成 4.70 秒音频耗时约 0.76 秒。ECS 同时加载 SenseVoice、中文 VITS 和 904-speaker Piper 英文模型后进程 RSS 约 577 MB，systemd 统计约 733 MB，适合当前单 worker、并发 1 的 2 vCPU/2 GB 配置。

## 生产环境关键配置

复制 `.env.example` 到服务器 `/etc/esp32-voice/voice.env`，至少修改：

```dotenv
VOICE_ALLOW_SIMULATED_INPUT=false
VOICE_AUTH_PEPPER=使用 openssl rand -hex 32 生成
VOICE_DATA_DIR=/var/lib/esp32-voice
VOICE_DATABASE_PATH=/var/lib/esp32-voice/voice.db
VOICE_ASR_PROVIDER=sensevoice
VOICE_TTS_PROVIDER=sherpa_vits
VOICE_LLM_ORDER=deepseek,qwen,glm
DEEPSEEK_API_KEY=...
QWEN_API_KEY=...
GLM_API_KEY=...
SHERPA_ENGLISH_TTS_MODEL=/opt/voice/models/vits-piper-en_US-libritts_r-medium/en_US-libritts_r-medium.onnx
SHERPA_ENGLISH_TTS_TOKENS=/opt/voice/models/vits-piper-en_US-libritts_r-medium/tokens.txt
SHERPA_ENGLISH_TTS_DATA_DIR=/opt/voice/models/vits-piper-en_US-libritts_r-medium/espeak-ng-data
```

模型路径见 `.env.example`。第一版使用单 Uvicorn worker 和 SQLite，是针对当前 2 vCPU/2 GB ECS 的低成本配置；音频合成并发默认为 1，避免 VITS 抢满内存。扩容到多实例时再把 Store 替换为 PostgreSQL/Redis，当前 API 和 ESP32 协议无需改变。

## 阿里云 HTTP 部署顺序

1. 将整个仓库上传/拉取到 `/opt/esp32-voice`，创建 Python 虚拟环境并安装 `cloud_server/requirements.txt`。
2. 创建无登录用户 `voice-gateway`，创建并授权 `/var/lib/esp32-voice` 与 `/etc/esp32-voice/voice.env`。
3. 下载 SenseVoice 与选定的轻量 VITS 模型到 `/opt/voice/models`，在环境文件填写绝对路径。
4. 复制 `deploy/voice-gateway.service` 到 `/etc/systemd/system/`，执行 daemon-reload、enable、start。
5. 先从服务器本机检查 `curl http://127.0.0.1:18765/voice-api/v1/health`。
6. 复制 `deploy/nginx-voice-http.conf` 到 Nginx 站点目录，链接启用后运行 `nginx -t`，成功才 reload。
7. 从公网检查 `http://voice.bsnlch.xyz/voice-api/v1/health`。安全组只开放 80；不要开放 18765。
8. 用 CLI 为每块 ESP32 分别注册设备，把各自 Token 通过加密 BLE 写入 NVS。
9. HTTP 只用于首轮短期联调。Token 会在链路上以明文可见，正式长期使用前必须申请证书并切换 HTTPS；切换时 API 路径不变，只修改 Nginx 与设备 `server_url`。

模型和代码应先下载到本地电脑再通过 SCP 上传 ECS；服务器上的 Python 包安装优先使用阿里云等国内 PyPI 镜像，不让生产服务器直接从 GitHub 下载大模型。

部署与回滚的更详细检查项见 [`../docs/LOW_COST_HTTP_CLOUD_EXECUTION_FLOW.md`](../docs/LOW_COST_HTTP_CLOUD_EXECUTION_FLOW.md)。

## ESP32 代码边界

公网固件在 `esp32_cloud_device/`，不会修改稳定的 `esp32_lan_device/`。它已实现：

- 固定公网域名，不再做 UDP 局域网发现；
- 加密 BLE 配置 Wi-Fi、设备 ID、设备 Token、服务器地址、音量，以及设备独立的中英文音色和语速；
- 首次运行自动迁移局域网固件 `ai-config` NVS 中的 Wi-Fi/音量，并在局域网提供 `http://ESP32-IP/` 配置页；保存时需要串口显示的 6 位设备 PIN，Token 不回显；
- HTTPS 使用 DigiCert Global Root G2 做服务器身份校验，短期站点证书续签无需重刷固件；NTP/HTTPS 不可用时才回退 HTTP；
- 按住 BOOT 录音，提交任务，按序轮询并立即播放每个 PCM 段；
- 网络重试退避、设备归属鉴权、播放时按键中止；
- 串口不打印 Token。

本阶段按要求没有编译、烧录或硬件测试。离线唤醒词需要 ESP-SR/WakeNet 模型和真实板卡内存/误唤醒测试，因此没有伪装成“已验证”功能；当前入口仍是 BOOT 按住说话。

BLE 配置 JSON 可增加以下字段；写入后 ESP32 会用自己的设备凭证同步服务器，失败时可向命令特征写入 `sync_voice` 重试：

```json
{"chinese_speaker_id":2,"english_speaker_id":128,"speech_speed":0.9}
```
