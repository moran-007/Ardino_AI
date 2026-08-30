# ESP32-S3 低成本云端语音服务执行流程

- 更新时间：2026-08-30
- 目标域名：`www.bsnlch.xyz`
- 目标服务器：阿里云杭州 ECS，Ubuntu 22.04，2 vCPU / 2 GiB，40 GB 系统盘

## 1. 文档目标

本执行流程用于把当前局域网语音对话方案逐步迁移到云服务器，并满足以下约束：

- 第一阶段先使用 HTTP 完成封闭联调；
- 优先使用免费、开源或免费额度内的语音识别与语音合成能力；
- ESP32 只负责联网、录音、上传、查询结果和播放；
- DeepSeek、通义千问、智谱 GLM 等大模型供应商统一在服务器端接入和切换；
- 后续可以增加翻译、更多 ASR/TTS、不同音色和其他 OpenAI 兼容供应商；
- 不破坏服务器上已经运行的考试、课程、3D 拼豆等项目；
- 第一阶段验证通过后，使用免费证书升级到 HTTPS。

本文档是执行说明，不代表相关云服务代码已经实现，也不自动修改服务器。

## 2. 当前已确认状态

### 2.1 域名和入口

- `www.bsnlch.xyz` 已解析到当前 ECS 公网 IP。
- `http://www.bsnlch.xyz/` 当前返回 HTTP 200。
- HTTPS 端口在网络层已经放行，但服务器没有 TLS 监听和证书，因此 TLS 握手失败。
- Nginx 当前只监听 80，尚未配置 `www.bsnlch.xyz` 专用 TLS 虚拟主机。

### 2.2 服务器约束

- 实例规格：`ecs.e-c1m1.large`，2 vCPU / 2 GiB。
- 系统实际可见内存约 1.6 GiB，Swap 已使用约 621 MiB。
- 系统盘剩余约 17 GB。
- 已运行 Nginx、MySQL、PostgreSQL、Node/PM2、Flask/Gunicorn 和 3 个 Docker 容器。
- 公网端口 `8765` 已被 `cloisonne-3d-mvp` 占用，不能作为语音服务端口。
- 公网端口 `17860` 也被现有项目直接使用。
- 语音服务应监听新的本机端口 `127.0.0.1:18765`，只通过 Nginx 暴露。

### 2.3 容量结论

当前 2 GiB ECS 可以先运行：

- FastAPI API 网关；
- 设备鉴权和任务管理；
- 多家远程 LLM API 适配；
- 文本翻译；
- 少量音频格式转换；
- 单并发、轻量语音模型的实验。

当前服务器不适合直接长期同时运行：

- 350 MB Zipformer ASR；
- 高质量本地中文 TTS；
- 多个本地翻译模型；
- 多路并发语音推理。

原因不是模型文件大小本身，而是模型运行时、Python/ONNX Runtime、音频缓冲和现有业务共同占用内存。若要长期使用本地免费 ASR + TTS，建议将 ECS 升到 2 vCPU / 4 GiB；如果坚持不升配，应只运行一个轻量语音 Worker、并发固定为 1，并保留远程免费额度作为降级路径。

## 3. 推荐最终架构

```text
ESP32-S3
  │
  │ 第一阶段 HTTP；验收后 HTTPS
  ▼
Nginx :80/:443
  │ /voice-api/
  ▼
cloud_server API 127.0.0.1:18765
  ├── Device Auth：每设备独立 Token
  ├── Job Manager：任务状态、超时、归属、TTL
  ├── ASR Provider
  │     ├── local_sherpa / local_sensevoice
  │     └── aliyun / other（可选降级）
  ├── LLM Provider
  │     ├── deepseek
  │     ├── qwen
  │     ├── glm
  │     └── custom_openai_compatible
  ├── Translation Provider
  │     ├── llm_translation
  │     └── argos_translate（可选本地）
  ├── TTS Provider
  │     ├── local_sherpa_vits
  │     ├── development_edge_tts（仅联调）
  │     └── aliyun_cosyvoice / other（可选降级）
  ├── Audio Adapter：统一输出 16 kHz / s16le / mono PCM
  └── PostgreSQL：设备、任务、配额、供应商配置
```

### 3.1 域名入口选择

推荐新增子域名：

```text
voice.bsnlch.xyz  A  当前 ECS 公网 IP
```

理由：

- 不影响 `www.bsnlch.xyz` 当前已有页面；
- Nginx 可以为语音服务设置独立 `server_name`；
- 以后申请证书、设置限流和迁移服务器更清晰；
- ESP32 只需要保存一个固定服务地址。

建议设备端最终地址：

```text
https://voice.bsnlch.xyz
```

如果暂时不添加子域名，可以用：

```text
http://www.bsnlch.xyz/voice-api
```

但这需要修改当前 `www` 站点的 Nginx 路由，容易与已有项目耦合，因此只作为备选。

## 4. 免费语音和翻译能力选型

“免费”应分成开源本地模型、供应商免费额度、以及没有正式 SLA 的开发接口。三者不能混为一类。

### 4.1 ASR 语音识别

| 方案 | 费用 | 资源 | 适用性 | 第一阶段建议 |
| --- | --- | --- | --- | --- |
| 现有 Zipformer INT8 | 无调用费 | 模型约 350 MB，运行内存更高 | 中文准确率较好 | 2 GiB 服务器只做独立压测，不直接常驻生产 |
| 现有 SenseVoice INT8 | 无调用费 | 模型约 228 MB | 多语言，资源比 Zipformer低 | 免费本地 ASR 首选候选，单并发测试 |
| 阿里云/其他 ASR 免费额度 | 额度内免费 | 服务器资源低 | 接入快、稳定性较好 | 本地模型失败时的降级方案 |

推荐顺序：

1. 先用项目已有 SenseVoice INT8 做服务器单并发压测；
2. 如果常驻后内存、Swap 或现有业务受影响，停止本地 ASR；
3. 联调阶段改用供应商免费额度；
4. 设备数量增加后再比较升配成本和 API 调用成本。

### 4.2 TTS 语音合成

| 方案 | 费用 | 音色 | 风险 | 第一阶段建议 |
| --- | --- | --- | --- | --- |
| sherpa-onnx VITS 中文模型 | 无调用费 | 取决于模型，通常有限 | 需确认每个模型许可证和内存 | 本地免费正式候选 |
| sherpa-onnx Kokoro 中英模型 | 无调用费 | 多音色/中英 | 模型和运行内存较高 | 升到 4 GiB 后评估 |
| Edge TTS 类开发接口 | 通常无需自备 Key | 音色较多 | 非正式生产 API，稳定性和使用条款不可控 | 只用于功能联调，不作为生产依赖 |
| 阿里云 CosyVoice/Qwen-TTS 免费额度 | 额度内免费 | 音色和表现力较丰富 | 超额后收费 | 本地 TTS 不达标时的可插拔方案 |

所有 TTS Provider 最终必须通过统一音频适配层输出：

```text
sample_rate = 16000
sample_format = signed 16-bit little-endian
channels = 1
container = raw PCM
```

### 4.3 翻译能力

低成本优先级：

1. 先让已经接入的 LLM 通过固定系统提示执行翻译，不部署额外翻译模型；
2. 如果要求完全离线、固定语种翻译，再评估 Argos Translate；
3. 不建议当前 2 GiB ECS 一开始公开部署无限制 LibreTranslate 服务，容易被外部滥用并消耗内存；
4. 翻译接口必须与设备鉴权、频率限制和日配额绑定。

## 5. 服务端项目结构

在仓库顶层新建独立目录，不修改现有 `pc_server` 局域网稳定版本：

```text
cloud_server/
  app/
    main.py
    api/
      health.py
      device_dialogue.py
      jobs.py
      admin.py
    core/
      config.py
      security.py
      logging.py
      limits.py
    providers/
      asr/
        base.py
        local_sherpa.py
        local_sensevoice.py
        aliyun.py
      llm/
        base.py
        openai_compatible.py
        deepseek.py
        qwen.py
        glm.py
      tts/
        base.py
        local_sherpa.py
        development_edge.py
        aliyun.py
      translation/
        base.py
        llm_translation.py
        argos.py
    services/
      dialogue_pipeline.py
      audio_converter.py
      job_worker.py
    storage/
      database.py
      models.py
  tests/
  migrations/
  deploy/
    nginx-http.conf
    nginx-https.conf
    voice-api.service
  requirements.txt
  .env.example
  README.md
```

核心原则：

- API 层不能直接写供应商判断；
- 每个供应商实现统一接口；
- 配置只决定当前启用和降级顺序；
- 上游错误转换成统一内部错误码；
- ESP32 不知道实际使用哪一家供应商。

## 6. 配置模型

示例 `.env` 字段只放字段名，不提交真实值：

```dotenv
APP_ENV=development
BIND_HOST=127.0.0.1
PORT=18765
PUBLIC_BASE_URL=http://voice.bsnlch.xyz

DATABASE_URL=postgresql://voice_app:REDACTED@127.0.0.1:5432/voice_service
SERVER_PEPPER=REDACTED

ASR_PROVIDER=local_sensevoice
ASR_FALLBACK_PROVIDER=disabled
ASR_MAX_CONCURRENCY=1

LLM_PROVIDER=deepseek
LLM_FALLBACK_PROVIDER=qwen
DEEPSEEK_API_KEY=REDACTED
QWEN_API_KEY=REDACTED
GLM_API_KEY=REDACTED

TTS_PROVIDER=local_sherpa
TTS_FALLBACK_PROVIDER=disabled
TTS_VOICE=default

TRANSLATION_PROVIDER=llm_translation

MAX_RECORD_SECONDS=20
MAX_UPLOAD_BYTES=1048576
JOB_TTL_SECONDS=3600
DEVICE_REQUESTS_PER_MINUTE=6
```

约束：

- `.env` 文件权限设为 `600`；
- `.env` 不加入 Git；
- API Key 不进入 ESP32、不进入网页、不进入日志；
- 后期使用阿里云 KMS/RAM Role 替代长期 AK；
- 供应商配置管理仅允许管理员访问，设备接口无权修改。

## 7. API 协议第一版

### 7.1 健康检查

```http
GET /voice-api/v1/health
```

只返回服务状态和版本，不返回 API Key、模型路径或系统内部信息。

### 7.2 上传语音

```http
POST /voice-api/v1/dialogue
Authorization: Bearer <device_token>
X-Device-ID: <device_id>
X-Sample-Rate: 16000
Content-Type: application/octet-stream
```

Body 为 16 kHz、16-bit、单声道 PCM，最大 1 MiB。

响应：

```json
{
  "ok": true,
  "job_id": "32位随机任务ID",
  "status_path": "/voice-api/v1/jobs/任务ID"
}
```

### 7.3 查询任务

```http
GET /voice-api/v1/jobs/{job_id}
Authorization: Bearer <device_token>
X-Device-ID: <device_id>
```

服务器必须验证任务属于当前设备。

### 7.4 下载音频

```http
GET /voice-api/v1/jobs/{job_id}/audio
Authorization: Bearer <device_token>
X-Device-ID: <device_id>
```

返回原始 PCM，并提供：

```http
Content-Type: application/octet-stream
X-Audio-Sample-Rate: 16000
X-Audio-Format: s16le-mono
Cache-Control: no-store
```

### 7.5 翻译接口

```http
POST /voice-api/v1/translate
Authorization: Bearer <device_token>
Content-Type: application/json

{
  "text": "待翻译文本",
  "source_language": "auto",
  "target_language": "zh-CN"
}
```

设备 Token 不能访问 `/admin`、修改供应商或提交 API Key。

## 8. 每设备独立密钥流程

第一版使用每设备唯一 Bearer Token：

1. 管理员在服务器执行注册命令；
2. 服务器生成 UUID `device_id` 和 32 字节随机 `device_token`；
3. 服务器只保存 `HMAC-SHA256(server_pepper, device_token)`；
4. 明文 Token 只显示一次；
5. 通过加密 BLE 配置或串口写入 ESP32 NVS；
6. 每次请求检查设备状态、Token、额度和任务归属；
7. 后台可以单独禁用和轮换一台设备。

HTTP 第一阶段的特殊限制：

- HTTP 无法保护 Token、录音和回答不被链路窃听；
- 只能使用可随时撤销的临时测试 Token；
- 不得使用真实敏感对话；
- 不得把管理员接口开放到 HTTP 公网；
- 联调周期建议不超过 72 小时；
- 功能闭环通过后立即升级 HTTPS 并轮换全部测试 Token。

HMAC 请求签名可以防部分篡改和重放，但不能加密音频，因此不能代替 HTTPS。

## 9. 分阶段执行流程

## 阶段 0：备份和安全基线

### 操作

1. 在阿里云控制台为系统盘创建快照。
2. 记录当前安全组、Nginx、Docker 和 systemd 配置。
3. 不删除或覆盖当前 8765/17860 服务。
4. 创建语音服务专用 Linux 用户，例如 `voiceapp`，禁止交互登录。
5. 创建目录：

```text
/opt/voice-service/app
/opt/voice-service/data/jobs
/opt/voice-service/models
/opt/voice-service/logs
```

6. 数据目录和秘密配置只允许 `voiceapp` 读取。

### 验收

- 快照创建成功；
- 现有网站和容器仍正常；
- 新目录不与现有项目重名；
- 回滚点明确。

## 阶段 1：DNS 与 HTTP 独立入口

### 操作

1. 在 DNS 增加：

```text
voice.bsnlch.xyz -> 当前 ECS 公网 IP
```

2. 语音 API 只监听：

```text
127.0.0.1:18765
```

3. 新建独立 Nginx 配置：

```nginx
server {
    listen 80;
    server_name voice.bsnlch.xyz;

    client_max_body_size 1m;

    location /voice-api/ {
        proxy_pass http://127.0.0.1:18765/voice-api/;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_connect_timeout 10s;
        proxy_read_timeout 900s;
        proxy_send_timeout 60s;
    }
}
```

4. 执行 `nginx -t` 通过后才能 reload。
5. 安全组不开放 18765；只保留 Nginx 80，未来增加 443。

### 验收

```text
GET http://voice.bsnlch.xyz/voice-api/v1/health -> 200
公网访问 IP:18765 -> 失败
现有 www 页面和已有项目 -> 正常
```

## 阶段 2：云服务骨架和数据库

### 操作

1. 创建 `cloud_server` 项目结构。
2. 新建 Python 虚拟环境或专用 Docker 镜像。
3. 在现有 PostgreSQL 中创建独立数据库、用户和最小权限：

```text
database: voice_service
user: voice_app
```

4. 创建 `devices`、`dialogue_jobs`、`usage_daily` 表。
5. 实现健康检查、统一错误格式、结构化日志和配置加载。
6. 使用 systemd 或 Docker `unless-stopped` 管理进程。
7. 为进程设置内存和 CPU 限制，避免影响现有项目。

### 验收

- 重启进程后数据库状态仍存在；
- API Key 和 Token 不出现在日志；
- 服务崩溃能自动恢复；
- 一个设备不能读取另一个设备的 job。

## 阶段 3：多家 LLM Provider

### 操作

1. 先实现统一 `LLMProvider` 接口。
2. 第一个接入 DeepSeek。
3. 第二个接入通义千问或其他 OpenAI 兼容接口。
4. 再接入 GLM 和自定义 OpenAI-compatible 地址。
5. 供应商选择只存服务端配置。
6. 设置超时、重试、最大 tokens、费用/次数统计和熔断。
7. 供应商错误不得把原始响应中的敏感信息返回 ESP32。

### 验收

- 修改服务端配置即可切换供应商，ESP32 无需升级；
- 主供应商失败时按配置切换备用供应商；
- 不允许设备自己提交任意 `api_url`，避免密钥被发送到攻击者域名。

## 阶段 4：免费 ASR 压测

### 操作

1. 优先测试已有 SenseVoice INT8。
2. 单独启动一个 ASR Worker，并发固定为 1。
3. 连续处理 20 条真实中文录音。
4. 记录：

```text
启动后 RSS
单次峰值 RSS
平均识别时间
P95 识别时间
CPU 使用率
Swap in/out
现有网站响应时间
```

5. 如果出现持续 Swap、OOM、现有项目明显变慢，则停止本地模型，不进入常驻部署。
6. 本地模型不通过时，启用免费额度 ASR Provider 继续完成业务闭环。

### 通过门槛

- 无 OOM；
- 压测期间没有持续 Swap in/out；
- 单次识别延迟满足实际交互要求；
- 现有网站没有明显超时；
- 模型许可证允许当前使用方式。

## 阶段 5：免费 TTS 压测

### 操作

1. 选择一个许可证明确的中文 sherpa-onnx VITS 模型。
2. 并发固定为 1。
3. 合成短句、中句、长句各 10 条。
4. 所有输出统一转成 16 kHz/s16le/mono PCM。
5. 测试听感、首包延迟、总耗时、峰值内存和中文数字读法。
6. 本地模型不通过时，联调阶段使用开发 TTS 或供应商免费额度。

### 通过门槛

- ESP32 能完整流式播放；
- 无削波、杂音和采样率错误；
- 中文可懂度合格；
- 服务器无持续内存压力；
- 音色模型许可证可接受。

## 阶段 6：对话流水线

流水线固定为：

```text
设备鉴权
 -> 校验 PCM 和大小
 -> 创建归属当前设备的 job
 -> ASR
 -> 可选翻译
 -> LLM
 -> 清洗不适合朗读的 Markdown
 -> TTS
 -> 转换 PCM
 -> 写入完成状态
 -> ESP32 下载播放
 -> TTL 清理
```

限制：

- 总并发先设为 1；
- 任务最长 10 分钟；
- 单设备每分钟最多 6 次；
- 单次录音最长 20 秒；
- job/audio 只允许 owner 下载；
- 音频和任务默认 1 小时清理；
- 服务端更新配置不能影响正在执行的任务。

## 阶段 7：ESP32 公网 HTTP 联调

### 固件修改

1. 禁用公网模式的 UDP 自动发现。
2. 服务器地址配置为：

```text
http://voice.bsnlch.xyz
```

3. 增加 `device_id` 和临时 `device_token`。
4. 继续使用现有 POST、轮询和 PCM 流式播放逻辑。
5. 连接失败时指数退避，不能高频重试。
6. 不在串口完整打印 Token。

### 验收场景

- ESP32 与服务器不在同一局域网；
- 家庭 Wi-Fi、手机热点分别完成对话；
- 错误 Token 返回 401；
- 超大录音返回 413；
- 超频返回 429；
- 下载其他设备任务返回 403/404；
- 服务重启后设备能恢复；
- 供应商失败有明确但不敏感的提示。

## 阶段 8：免费 HTTPS 上线门槛

HTTP 完成封闭联调后，使用 Let’s Encrypt 免费证书，不产生证书购买费用。

### 操作

1. 确认 `voice.bsnlch.xyz` DNS 已稳定解析。
2. 安装 Certbot 的 Nginx 集成或使用阿里云证书服务免费证书。
3. 为 `voice.bsnlch.xyz` 申请证书。
4. Nginx 监听 443，并将 80 重定向到 HTTPS。
5. ESP32 使用 `NetworkClientSecure` 和可信根 CA 验证服务器身份。
6. 轮换 HTTP 联调期间使用过的全部设备 Token。
7. 上线后不允许退回 insecure TLS 或明文 HTTP。

### 验收

```text
https://voice.bsnlch.xyz/voice-api/v1/health -> 200
http://voice.bsnlch.xyz/... -> 301/308 到 HTTPS
证书域名、有效期和链完整
ESP32 在错误证书下拒绝连接
```

## 10. 服务器安全整改顺序

这些工作与语音开发并行，但必须在正式公网使用前完成：

1. 快照并验证回滚。
2. 将 8765、17860 从安全组公网入口移除；先确认 Nginx 反代已有项目正常。
3. Docker 端口改为绑定 `127.0.0.1`，避免绕过 Nginx。
4. SSH 22 只允许可信源 IP。
5. 创建普通 sudo 用户并确认密钥登录。
6. 确认新用户可登录后关闭 SSH 密码登录和 root 直接登录。
7. 安装安全更新并安排维护重启。
8. 为每个容器设置资源上限。
9. 配置日志轮转、磁盘告警和任务目录清理。
10. 正式 API 只开放 443；80 只做跳转和证书续期。

## 11. 监控与成本控制

最少记录以下指标：

```text
请求数、成功率、401、413、429、5xx
ASR/LLM/TTS 各阶段耗时
各供应商调用次数和 tokens/字符数
每设备日使用量
队列长度和并发数
进程 RSS、服务器可用内存、Swap in/out
磁盘剩余空间和临时音频目录大小
```

低成本策略：

1. 默认并发 1；
2. 本地免费模型优先，但必须通过资源门槛；
3. 免费额度只作为可配置 Provider，不在代码里写死；
4. 超过日配额停止服务，而不是无限产生费用；
5. 对话历史只保留必要轮数；
6. 长回答在 TTS 前截断或摘要；
7. 录音和音频按小时清理；
8. 每月比较“ECS 升配费用”和“ASR/TTS API 费用”再决定迁移方向。

## 12. 测试清单

### 自动测试

- 配置校验和敏感字段脱敏；
- 每设备 Token 验证；
- job owner 隔离；
- PCM/WAV 转换；
- ASR、LLM、TTS Provider mock；
- 主/备用供应商切换；
- 超时、重试和熔断；
- 请求体、录音时长和频率限制；
- TTL 清理；
- 服务重启后的任务恢复。

### 硬件验收

- 不同网络完整对话 100 次；
- 短句、长句、数字、英文混合；
- Wi-Fi 断开和恢复；
- 服务重启；
- 上游供应商超时；
- 播放中用户取消；
- 最大录音；
- 连续 20 轮内存和磁盘无增长泄漏。

## 13. 回滚方案

每阶段必须可独立回滚：

- DNS：删除 `voice` A 记录，不影响 `www`；
- Nginx：移除独立 `voice` 配置并 reload；
- API：停止 `voice-api` systemd/Docker 服务；
- 数据库：语音服务使用独立 database/user，不影响现有库；
- 模型：停止 Worker 并删除独立模型目录；
- ESP32：保留当前局域网稳定固件和构建产物；
- ECS：严重故障时使用阶段 0 快照恢复。

不得为了部署语音服务删除现有 Docker 容器、覆盖现有 Nginx 文件或占用已有端口。

## 14. 实际推荐路线

结合当前服务器资源和节约成本目标，推荐按以下顺序执行：

1. 新增 `voice.bsnlch.xyz`，不要占用 `www` 现有入口；
2. 在当前 ECS 部署轻量 `cloud_server` API 和多供应商 Provider 框架；
3. 第一阶段仅用 HTTP + 临时 Token，完成跨网络 API 联调；
4. 先压测 SenseVoice INT8，本地 ASR 通过才常驻；
5. TTS 先测试一个许可证明确的轻量 VITS；不通过时使用免费额度 Provider；
6. 翻译先通过 LLM Provider 实现，不额外部署翻译模型；
7. 总并发保持 1，避免影响现有项目；
8. HTTP 闭环通过后立即申请免费 HTTPS 证书并轮换 Token；
9. 如果出现持续 Swap 或现有业务延迟，升级到 2 vCPU / 4 GiB，而不是继续堆模型；
10. 设备数量增长后再决定云 ASR/TTS 与独立推理 ECS 的成本分界。

## 15. 开始实施前需要确定的值

执行代码实现前，只需确定以下项目：

```text
是否新增 voice.bsnlch.xyz：建议是
第一家 LLM：建议 DeepSeek
备用 LLM：建议通义千问或 GLM
第一版 ASR：建议 SenseVoice INT8 压测
第一版 TTS：建议轻量 sherpa-onnx VITS 压测
翻译：建议先使用 LLM 翻译
允许并发：1
录音最长：20 秒
任务保留：1 小时
HTTP 联调期限：不超过 72 小时
```

这些值确定后，下一阶段才进入 `cloud_server` 代码实现和服务器部署。
