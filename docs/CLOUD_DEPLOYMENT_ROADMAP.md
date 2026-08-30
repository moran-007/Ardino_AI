# ESP32-S3 语音设备：音色、公网服务器化、设备密钥与离线唤醒方案

更新时间：2026-08-30

## 0. 结论先行

1. **可以改变音色，但当前机器实际上只有一个可用中文音色。** 现有服务端已经把 `voice` 做成可配置项，并在每次合成时传给 Windows `System.Speech`。本机实测只枚举到 `Microsoft Huihui Desktop`（中文女声）和 `Microsoft Zira Desktop`（英文女声），所以目前不能直接在多个中文音色之间切换。要获得更多中文声音，可安装能被 `System.Speech` 枚举到的 SAPI 声音，或者把服务端 TTS 换成云 TTS / 跨平台本地 TTS。
2. **API Key 放服务器端明显更合适。** 当前局域网实验已经基本采用这一结构：ESP32 只上传 PCM、查询任务状态、下载并播放 PCM；DeepSeek API Key、模型、系统提示词、ASR 和 TTS 都在电脑端。公网版应延续并强化这一边界，ESP32 不应保存 DeepSeek 等上游厂商的 API Key。
3. **服务器为每台 ESP32 分配独立设备密钥完全可行。** 单设备 MVP 难度低到中等；真正做到批量生产、首次配网、轮换、撤销、配额和防复制后，难度为中等。推荐先做“每设备唯一随机 Bearer Token + HTTPS”，后续再升级为 HMAC 请求签名或 mTLS。
4. **离线唤醒应使用 ESP-IDF + ESP-SR 的 AFE/WakeNet。** 唤醒词检测可以完全在 ESP32-S3 上运行；被唤醒后的自由中文问句仍发服务器识别和回答。若要求完全离线自由对话，当前 16 MB Flash / 8 MB PSRAM 的开发板无法承载现有约 350 MB Zipformer、LLM 和高质量 TTS；它适合用 MultiNet 离线执行有限设备命令。
5. **当前代码不能直接暴露到公网。** 至少要补齐 TLS 证书校验、逐设备鉴权、限流、任务归属校验、管理员接口隔离、Linux TTS 替换、持久化存储和密钥管理。

## 1. 现有代码与音色能力

### 1.1 当前语音链路

现有局域网实验的真实链路是：

```text
INMP441
  -> ESP32-S3 录制 16 kHz / 16-bit / mono PCM
  -> HTTP POST /v1/dialogue
  -> 电脑 Zipformer 离线 ASR
  -> 电脑调用 DeepSeek Chat Completions
  -> Windows System.Speech 本地 TTS
  -> ESP32-S3 流式下载 answer.pcm
  -> MAX98357A 播放
```

代码证据：

- 服务端文件开头已经说明上传 PCM、Zipformer ASR、DeepSeek、Windows TTS 和回传 PCM 的完整职责：`pc_server/lan_dialogue_server.py:1-6`。
- 对话任务在服务端依次执行预处理、识别、LLM、TTS 和 WAV 转 PCM：`lan_dialogue_server.py:316-360`。
- ESP32 上传录音：`esp32_lan_device/esp32_lan_device.ino:538-572`。
- ESP32 轮询任务并流式播放服务端 PCM：`esp32_lan_device.ino:575-645`。
- 当前不是语音唤醒，而是 GPIO0/BOOT 按住说话：`esp32_lan_device.ino:647-695, 825-829`。

### 1.2 当前能否换音色

能。当前实现有三处支持：

- 服务端配置中有 `voice`，默认值为 `Microsoft Huihui Desktop`：`lan_dialogue_server.py:56-70`。
- 手机网页可以读取和修改 `voice`：`lan_dialogue_server.py:399-457, 509-515, 558-578`。
- 合成时把 `voice` 传给 `pc_server/windows_speech.ps1`：`pc_server/lan_dialogue_server.py:170-210`；脚本使用 `SpeechSynthesizer.SelectVoice()`：`pc_server/windows_speech.ps1:78-100`。

列出当前电脑可用声音：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\pc_server\windows_speech.ps1 -Mode voices
```

2026-08-30 在本机检测到：

| 名称 | 语言 | 性别 | 适合当前中文回答 |
| --- | --- | --- | --- |
| Microsoft Huihui Desktop | zh-CN | Female | 是 |
| Microsoft Zira Desktop | en-US | Female | 否，中文效果不可用或很差 |

修改方式：

1. 手机打开 ESP32 页面，展开“AI 预设与配置”，填写声音的**完整名称**并保存；或
2. 重新运行 `pc_server/configure_server.ps1` 修改 `voice`；或
3. 修改服务端配置，但不要再把 API Key 以明文留在该文件。

注意：如果填写了不存在的声音，`windows_speech.ps1` 会自动退回第一个已启用的 `zh-CN` 声音，因此“配置保存成功但声音没变”通常就是名字不匹配。

### 1.3 想要明显不同的中文音色时怎么选

| 方案 | 优点 | 缺点 | 建议 |
| --- | --- | --- | --- |
| 安装更多 Windows SAPI 声音 | 对当前代码改动最小 | 可用中文声音少；Windows 绑定；未必所有 Windows 新式自然声音都能被 `System.Speech` 枚举 | 仅适合继续在 Windows 单机运行 |
| 云端 TTS | 音色、情感、语速和 SSML 通常最丰富；服务器易扩容 | 增加费用、网络依赖和一个上游密钥 | 公网产品首选，密钥只放服务器 |
| sherpa-onnx 本地 TTS | 跨 Windows/Linux，可离线；项目已有 sherpa-onnx 运行环境 | 需额外下载 TTS 模型、评估声音质量和模型许可证；需要 CPU/GPU 资源 | 希望完全自托管时优先验证 |

sherpa-onnx 官方目前提供中文及中英混合 TTS 模型和 Python/C++ 示例，参见[官方 TTS 模型列表](https://k2-fsa.github.io/sherpa/onnx/tts/all/)和[安装说明](https://k2-fsa.github.io/sherpa/onnx/tts/faq.html)。

当前代码只切换“声音名称”，没有暴露 `rate`、`pitch`、`style`。如需“同一音色更快/更慢、更高/更低、不同情绪”，需要扩展 TTS 适配层；Windows `System.Speech` 至少可再增加 `Rate`，云 TTS 则应使用供应商的 SSML/风格参数。

## 2. 为什么 API 应由服务器统一配置

推荐职责边界：

```text
ESP32-S3
  只保存：Wi-Fi、服务器域名、设备身份凭据、音量、固件版本
  只负责：唤醒/录音、TLS 通信、播放、基础状态灯和 OTA
                  |
                  | HTTPS 443
                  v
公网语音服务
  负责：设备鉴权、限流、ASR、上下文、LLM、TTS、审计、用量与密钥
                  |
                  v
DeepSeek / 其他 LLM、TTS 供应商
```

这样做的收益：

- 上游 API Key 不会随固件/NVS 被提取，也不需要为换 Key 重新刷机。
- 可在服务器统一轮换密钥、切换模型/供应商、限制每台设备的费用和并发。
- 可隐藏系统提示词、业务逻辑和供应商错误细节。
- 可做多设备隔离、封禁、审计、欠费停用和灰度升级。
- ESP32 固件更稳定，服务器功能可以快速迭代。

当前局域网实验已经把 DeepSeek Key 放在 PC 服务端，并且环境变量 `DEEPSEEK_API_KEY` 优先于本地 JSON：`lan_dialogue_server.py:43-70, 104-113`。方向是正确的，但公网化前要移除 JSON 中的明文 Key。

### 2.1 必须立即处理的密钥问题

本次检查发现 `pc_server/server_config.local.json` 中存在明文 DeepSeek API Key，并且检查输出曾显示该行。该 Key 必须视为已泄漏：

1. 立即在 DeepSeek 控制台撤销旧 Key 并创建新 Key。
2. 新 Key 只注入服务器进程环境变量或云密钥管理服务，不写源码、JSON、日志、镜像或固件。
3. 当前子目录 `.gitignore` 已忽略 `pc_server/server_config.local.json`，但这只能防误提交，不能加密磁盘上的文件。
4. 禁止通过设备网页更新上游 API Key；这一能力应迁移到独立管理员后台。

DeepSeek 官方 API 使用服务器端 Bearer API Key，当前请求格式与官方 Chat Completions 示例一致，参见[DeepSeek API 入门](https://api-docs.deepseek.com/zh-cn/)和[Chat Completions API](https://api-docs.deepseek.com/zh-cn/api/create-chat-completion/)。

## 3. 从局域网迁移到公网服务器要做什么

### 3.1 推荐的公网部署形态

推荐 Linux 云服务器或容器平台，而不是把家庭 Windows 电脑的 8765 端口直接映射到公网：

```text
Internet
  -> DNS: voice.example.com
  -> 反向代理/负载均衡：TLS 终止、限流、最大请求体、访问日志
  -> FastAPI：仅监听 127.0.0.1 或容器内网
  -> ASR/TTS Worker：限制并发，不在请求线程无限创建任务
  -> 数据库：devices、jobs、conversation、quota
  -> 对象存储/受保护文件存储：临时录音和回答音频
  -> 密钥管理：DeepSeek/TTS Key、服务器 pepper、签名密钥
```

可选路径：

- **最少改动验证版：Windows 云主机。** 可继续使用现有 `windows_speech.ps1`，但成本、授权、运维和横向扩容较差。
- **推荐生产版：Linux。** Zipformer/sherpa-onnx 可保留；必须把 Windows TTS 换成云 TTS 或 sherpa-onnx 本地 TTS。
- **VPN/专网版。** 如果设备固定在几个场所，可让路由器通过 WireGuard/Tailscale 等接入服务器，减少公网 API 暴露；但 ESP32 本身直接运行这类完整客户端会增加固件复杂度，移动设备场景仍推荐 HTTPS 公网服务。

### 3.2 云端基础设施操作清单

1. 购买云主机或容器资源；先按 1 个 ASR Worker 做容量测试。Zipformer 模型约 350 MB，启动内存和并发内存必须实测。
2. 准备域名，例如 `voice.example.com`，DNS 指向公网入口。
3. 只开放公网 TCP 443；SSH 使用密钥和来源限制。不要开放 UDP 8764，也不要直接开放 Uvicorn 8765。
4. 在 Nginx/Caddy/云负载均衡上申请并自动续期可信 TLS 证书。
5. FastAPI 只监听回环或容器内网，由反向代理转发。
6. 设置最大请求体。当前 20 秒、16 kHz、16-bit、单声道原始 PCM 最大约 `20 * 16000 * 2 = 640000` 字节；可先限制为 1 MiB。
7. 设置连接超时、任务超时、并发数、设备级和 IP 级限流。
8. 用服务管理器或容器编排运行 API 和 Worker；进程崩溃应自动恢复。
9. 将设备、任务、配额写入数据库；不要继续只用内存 `histories` 和本地 `jobs/` 目录作为生产状态。
10. 对录音、识别文字、回答文字和音频制定保留时间；默认最小化保留，并提供清除机制。
11. 加入监控：请求量、401/429/5xx、ASR/LLM/TTS 延迟、上游费用、Worker 队列、磁盘和内存。
12. 上线前做 Key 轮换、设备撤销、重放、超大请求、并发和断网恢复测试。

### 3.3 ESP32 固件必须修改的地方

| 现状 | 公网问题 | 必须修改 |
| --- | --- | --- |
| UDP 广播自动发现服务器：`esp32_lan_device.ino:191-227` | 广播不会跨互联网路由 | 公网模式禁用发现，BLE/出厂配置固定 `https://voice.example.com` |
| BLE 配置只接受 `http://`：`esp32_lan_device.ino:481-485` | 无法保存 HTTPS 地址 | 只接受 `https://`；开发模式才允许局域网 HTTP |
| Arduino Core 3.3.11 的 `HTTPClient.begin(url)` 在没有 CA 时会对 HTTPS 调用 `setInsecure()` | 加密但不验证服务器身份，仍可被中间人攻击 | 使用 `NetworkClientSecure` + 根 CA/证书包，或 `http.begin(url, root_ca)`；同步时间并验证证书 |
| 当前只保存一个 `lan_token`，且 BLE 配置没有写入它 | 无法可靠逐设备发放凭据 | 增加一次性配网/注册流程，保存唯一设备凭据 |
| `deviceId()` 只取 eFuse MAC 的低 32 位：`esp32_lan_device.ino:70-72` | 批量设备碰撞空间和可伪造性不理想 | 使用完整芯片标识或注册生成的 UUID；身份最终由密钥证明而不是 ID 字符串 |
| `/info` 无鉴权返回 BLE PIN：`esp32_lan_device.ino:260-272` | 同网段用户可获取配网 PIN | 删除 PIN 回传；仅串口、包装二维码或按键物理在场时显示 |
| 录音和回答使用原始 PCM | WAN 流量较高 | MVP 可保留；规模化后评估 Opus/FLAC，或服务端分段流式 PCM |

乐鑫官方安全指南明确建议验证服务器 X.509 证书、为生产设备开启 Secure Boot、Flash Encryption，并对包含 Wi-Fi/设备秘密的 NVS 使用 NVS Encryption。参考：[ESP32-S3 Security Overview](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/security/security.html)、[Security Features Enablement Workflows](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/security/security-features-enablement-workflows.html)、[NVS 安全说明](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/storage/nvs_flash.html)。这些 eFuse 操作可能不可逆，必须先在测试板完整验证 OTA、恢复和量产烧录流程。

### 3.4 当前服务端不能直接公网化的具体原因

1. `device_token` 当前是**全局共享单令牌**，默认还是空值：`lan_dialogue_server.py:70, 303-308`；本机配置检查结果为 disabled。
2. `configure_server.ps1` 每次都会把 `device_token` 写为空字符串：`configure_server.ps1:52`。
3. 任意持有共享令牌的设备都能访问全局 `/v1/settings`，甚至替换上游 API Key、系统提示词和 TTS 声音：`lan_dialogue_server.py:399-457`。设备接口和管理员接口必须拆开。
4. `/v1/job/{job_id}` 和 `/v1/audio/{job_id}` 只检查“令牌是否有效”，没有验证任务属于当前 `device_id`：`lan_dialogue_server.py:649-680`。
5. 每个请求直接创建一个 Python Thread：`lan_dialogue_server.py:632-641`，无全局并发限制，公网下容易被耗尽 CPU/内存。
6. 历史记录只在进程内存，重启丢失；任务状态和录音写单机磁盘，不适合多实例。
7. 设备令牌通过 ESP32 页面 iframe 的 URL fragment 交给浏览器：`esp32_lan_device.ino:248-250`。生产版手机管理界面应使用独立用户登录，不应复用设备长期凭据。
8. 健康检查可以公开，但不能回显敏感配置；详细运维状态应只在内网或管理员认证后提供。

## 4. 服务器独立分配设备密钥

### 4.1 先区分两种“密钥”

- **上游 API Key：** DeepSeek/TTS 服务商发给你的服务器。只保存在服务器，通常一个账户或一个环境使用一个/一组 Key。
- **设备密钥：** 你的服务器发给每台 ESP32，用来证明“这是第几台合法设备”。每台必须不同，可单独禁用、轮换和限额。

不建议给每台 ESP32 烧录一个 DeepSeek API Key。这样会把计费凭据交给物理可接触的终端，难以阻止提取、转卖和滥用。应由服务器使用上游 Key，再在数据库中记录和限制每台设备的用量。

### 4.2 推荐的第一版：每设备随机 Bearer Token

难度：**低到中等**，适合先上线。

注册/生产流程：

1. 服务器生成 `device_id` 和 32 字节密码学随机 Token。
2. 服务器数据库保存 `device_id`、Token 的服务器侧摘要、状态、额度、固件版本；不保存可直接使用的明文 Token。
3. Token 只在注册时显示一次，通过工装、加密 BLE + 一次性配对码或包装二维码安全写入 ESP32。
4. ESP32 把 Token 存入加密 NVS，并通过 HTTPS 发送 `Authorization: Bearer <token>` 和 `X-Device-ID`。
5. 服务器按 `device_id` 查找记录，用常量时间比较摘要，检查 enabled/quota/rate-limit，然后把认证后的 `device_id` 写入请求上下文。
6. 所有 job、audio、history 查询都用数据库中的 owner 做归属检查，不信任客户端自己声明的 ID。
7. 后台可单独撤销/轮换某台设备，不影响其他设备。

建议表：

```text
devices(
  id, credential_digest, status, quota_daily,
  firmware_version, created_at, rotated_at, last_seen_at
)

jobs(
  id, owner_device_id, status, input_object, output_object,
  created_at, expires_at, error_code
)
```

高熵 Token 可以用 `HMAC-SHA256(server_pepper, token)` 生成服务器侧摘要；`server_pepper` 放 KMS/环境密钥，不放数据库。传输安全仍依赖正确验证的 TLS。

### 4.3 防重放增强：HMAC 请求签名

难度：**中等**。如果希望即便某些代理日志泄漏请求头，也不直接泄漏可重复使用的 Bearer Token，可让设备对每次请求签名：

```text
canonical = METHOD + "\n" + PATH + "\n" + TIMESTAMP + "\n" + NONCE + "\n" + SHA256(BODY)
signature = HMAC-SHA256(device_secret, canonical)
```

请求携带 `X-Device-ID`、`X-Timestamp`、`X-Nonce`、`X-Signature`。服务器验证：

- 时间偏差在允许窗口内；
- nonce 没用过；
- body 摘要与签名相符；
- 设备启用且未超额度。

这能防请求篡改和重放，但需要 SNTP 时间、nonce 存储、规范化规则和 Secret 轮换。它**不能代替 HTTPS**，因为音频和回答仍需要保密。

### 4.4 更强方案：每设备 mTLS 证书

难度：**高**。每台设备持有独立私钥和客户端证书，TLS 网关验证客户端证书。优点是标准化、网关层即可拒绝非法设备；难点是证书签发、到期、吊销、工厂注入和私钥保护。ESP32-S3 还可评估 RSA Digital Signature 外设，使私钥不直接暴露给软件；乐鑫安全概览将其作为安全设备身份的方案之一。

### 4.5 推荐落地顺序

1. 第一阶段：唯一 Bearer Token + HTTPS + 数据库 + 撤销/限流。
2. 第二阶段：Token 自动轮换、一次性注册、加密 NVS、Secure Boot/Flash Encryption。
3. 第三阶段：有较高复制攻击风险时升级 HMAC 或 mTLS/硬件密钥。

## 5. ESP32-S3 离线唤醒

### 5.1 “离线唤醒”与“完全离线对话”不是一回事

- **离线唤醒：可行。** ESP32 一直分析麦克风的小帧音频，检测到“Hi 乐鑫”等唤醒词后才连接/发送服务器。
- **离线固定命令：可行。** 用 MultiNet 处理“停止播放、音量大一点、重新联网”等有限命令。
- **完全离线自由问答：当前硬件不合适。** 项目现有 Zipformer INT8 约 350 MB、SenseVoice 约 228 MB，而开发板为 16 MB Flash / 8 MB PSRAM；更不用说本地大语言模型。

乐鑫官方 WakeNet 面向低功耗 MCU，ESP32-S3 的 WakeNet9 支持最多 5 个唤醒词，输入为 16 kHz、单声道、signed 16-bit；参见[WakeNet 官方文档](https://docs.espressif.com/projects/esp-sr/zh_CN/latest/esp32s3/wake_word_engine/README.html)。MultiNet 面向离线命令词，官方文档说明最多支持 200 个自定义命令词；参见[MultiNet 命令词文档](https://docs.espressif.com/projects/esp-sr/zh_CN/latest/esp32s3/speech_command_recognition/README.html)。

### 5.2 推荐技术路线

不要直接在当前 Arduino 固件里硬塞模型。项目已有 `docs/ESP32_EDGE_ROADMAP.md`，其建议是正确的：新建独立 ESP-IDF 工程，使用 ESP-SR。

运行状态机建议：

```text
BOOT
 -> 初始化 I2S + ESP-SR 模型分区 + AFE
 -> LISTENING：AFE 持续 NS/AGC/VAD/WakeNet
 -> WAKE_DETECTED：亮灯/提示音，保留 300~500 ms pre-roll
 -> RECORDING：持续收音，VAD 判断说话结束，最长 20 秒
 -> UPLOADING：HTTPS + 设备鉴权上传
 -> WAITING：轮询或保持流式连接
 -> PLAYING：播放回答；禁用 WakeNet 或向 AEC 提供播放参考
 -> LISTENING
```

实施步骤：

1. 新建 `experiments/esp32_wakenet_device/` ESP-IDF 工程，保留当前可工作的 Arduino 固件和二进制。
2. 先只做 `INMP441 -> I2S -> AFE feed/fetch`，验证 16 kHz/s16/mono、RMS、丢帧和 PSRAM。
3. 通过 `idf.py menuconfig -> ESP Speech Recognition` 选择 AFE、WakeNet 模型，并添加 `model` 分区。官方模型加载说明给出的分区示例为 6000K，但应按实际选中模型生成结果调整：[模型选择与加载](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/flash_model/README.html)。
4. 先使用官方开放唤醒词完成闭环，不要一开始定制唤醒词。
5. 唤醒后复用现有录音/网络/播放逻辑，但改为 HTTPS、CA 校验和逐设备凭据。
6. 加入 VAD 自动结束、pre-roll、防抖和最大录音时间。
7. 播放回答时先简单禁用 WakeNet，避免设备被自己的喇叭唤醒；需要“播放中可打断”时，再把播放 PCM 作为 AEC reference 送入 AFE。
8. 用真实外壳、1 m/3 m、安静/电视声/音乐/喇叭回声等场景测误唤醒和漏唤醒，不能直接套用官方 Korvo 板基准。

AFE 官方包含 AEC、NS、VAD、AGC 和 WakeNet，参见[ESP-SR AFE 文档](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/audio_front_end/README.html)。

### 5.3 自定义唤醒词的现实成本

建议原型阶段先用官方开放词。如果必须使用品牌词，乐鑫官方定制流程说明：

- 单模型最多 5 个唤醒词；
- 唤醒词通常 3～6 个音节；
- 自备语料时需要大于 2 万条合格录音；
- 训练和调优通常需要 2～3 周，并可能收费。

参见[乐鑫唤醒词定制流程](https://docs.espressif.com/projects/esp-sr/zh_CN/latest/esp32s3/wake_word_engine/ESP_Wake_Words_Customization.html)。

### 5.4 功耗与硬件注意事项

- WakeNet 是本地持续推理，不等于 ESP32 可以进入 deep sleep；I2S 和处理核心需要持续运行。市电设备问题不大，电池产品需要单独做功耗预算，必要时使用专用超低功耗语音唤醒芯片。
- 如果唤醒后才打开 Wi-Fi，会省电但增加联网延迟；常供电产品可保持 Wi-Fi 连接。
- 当前 INMP441 单麦能做基础唤醒；远场、强噪声和播放中打断的性能会受麦克风位置、外壳、地线和 AEC reference 质量影响。
- MAX98357A 播放与麦克风共享时钟并不等于自动具备 AEC；固件必须把实际播放参考数据送入 AFE。

## 6. 推荐的分阶段交付计划

### 阶段 A：1 台设备公网 MVP

- 撤销并轮换已暴露的 DeepSeek Key。
- 服务端 Key 改为环境变量/KMS。
- 准备域名、TLS 入口，只开放 443。
- Linux 版替换 Windows TTS；若暂时不替换，则先用 Windows 云主机验证。
- ESP32 固定 HTTPS 域名并正确校验证书。
- 加入一台设备一个 Bearer Token；删除设备访问 `/v1/settings` 的权限。
- 加入并发限制、1 MiB 请求上限、基础限流和日志脱敏。

验收：不同网络下可完成 100 次对话；错误 Key/错误证书/超限请求被拒绝；断网可恢复；日志中无 Token/API Key/完整音频正文。

### 阶段 B：多设备与可运营

- 数据库保存 devices/jobs/quota；job/audio 强制归属检查。
- Worker 队列替代无限创建线程。
- 管理员后台与设备 API 完全分离。
- Token 注册、轮换、撤销和设备封禁。
- 音频保留策略、用户隐私提示、监控告警、成本统计。
- OTA 签名、回滚以及设备版本上报。

### 阶段 C：离线唤醒与量产安全

- ESP-IDF + ESP-SR AFE/WakeNet 独立实验通过。
- 唤醒、VAD、pre-roll、播放防自唤醒和打断策略通过场景测试。
- NVS Encryption、Flash Encryption、Secure Boot 在测试板验证后进入量产烧录。
- 每设备工厂凭据、一次性注册、包装/售后换绑流程。
- 需要品牌词时再启动定制模型和语料流程。

## 7. 当前验证结果、假设与未知项

### 已验证事实

- 现有服务端组件测试 3/3 通过：配置边界、异步任务/音频下载、PCM/WAV 往返均正常。
- 这台电脑只有一个中文 `System.Speech` 声音。
- 本地 DeepSeek Key 已配置但以明文存储；`device_token` 未启用。
- 当前 Arduino ESP32 Core 是 3.3.11；该版本无 CA 参数的 HTTPS `HTTPClient.begin(url)` 会进入 insecure 模式。
- 当前局域网固件没有 WakeNet，只有 BOOT push-to-talk。

### 假设

- 公网目标是“设备可在任意普通 Wi-Fi 下访问你的服务器”，而不是仅连接几个固定私网。
- 第一阶段设备量不大，允许先使用非流式、整段 PCM 上传和任务轮询。
- 自由问答仍依赖服务器；离线只要求唤醒和少量本地控制命令。

### 仍需在实施前确认

- 服务器是 Windows 还是 Linux、预计设备数/并发数和月度成本上限。
- 期望使用云 TTS 还是本地 TTS，以及音色授权是否允许商业使用。
- 是否保存对话历史、保存多久、是否涉及儿童/家庭场景的录音隐私要求。
- 设备是市电还是电池供电；是否要求播放过程中可语音打断。
- 是否需要自定义品牌唤醒词，还是官方开放词可接受。

## 8. 推荐下一步

先实施“阶段 A 公网 MVP”，但不要同时开始 WakeNet 迁移。公网 TLS/鉴权和 ESP-IDF 音频迁移都涉及底层改动，分开验证更容易定位问题。建议顺序为：

1. 轮换泄漏 Key；
2. 确定 Linux 本地 TTS 或云 TTS；
3. 完成公网服务端和每设备 Token；
4. 修改 ESP32 HTTPS/CA/注册流程并做跨网络验收；
5. 再在独立 ESP-IDF 工程加入 WakeNet，最后接回已稳定的公网 API。
