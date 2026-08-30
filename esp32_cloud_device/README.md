# ESP32-S3 公网语音固件

只编译同目录的 `esp32_cloud_device.ino`、`voice_input.cpp` 和 `voice_input.h`。本目录不再保留旧的 `.txt` 固件副本，避免误烧录过期版本。

## 接线

接线与 `esp32_lan_device` 完全一致：ESP32-S3 使用 BCLK=GPIO5、WS=GPIO4、INMP441 SD=GPIO6、MAX98357A DIN=GPIO7、功放 SD=GPIO15、音量电位器=GPIO1、BOOT=GPIO0。云端版音频驱动在局域网版基础上增加了上传前释放 I2S/DMA、播放前恢复硬件的逻辑，两套源码不要交叉复制。

## 首次启动

固件优先读取 `cloud-ai` NVS。若其中没有 Wi-Fi，会从旧局域网固件的 `ai-config` 迁移 Wi-Fi、固定音量和电位器模式，但不会迁移局域网 Token。

Wi-Fi 连上后，串口会打印：

```text
[设备网页] http://ESP32的局域网IP/
```

打开该地址可设置云服务器、设备 ID/Token、中英文音色、语速和音量。保存时必须输入串口显示的 6 位 BLE PIN，Token 不会在网页或状态 JSON 中回显。若没有可迁移的 Wi-Fi，先通过加密 BLE 配置。

首页还包含“实时对话流程”，每秒读取 `/runtime`，显示当前阶段、HTTP 状态码、任务 ID、分段序号、Wi-Fi RSSI、空闲内存、历史最低空闲内存、最大连续内存块、云端连接基址、keep-alive 状态和最近 24 条事件。诊断范围包括联网、NTP/证书、录音、PCM 上传、任务轮询、分段下载及播放；密码和 Token 不写入事件日志。

首页同时包含“对话内容”：固件参考 Windows 联调工具，从 `/voice-api/v1/jobs/{job_id}` 递归读取 `asr_text`、`transcript`、`recognized_text`、`user_text` 等用户识别文字，以及 `answer_text`、`assistant_text`、`llm_text`、`reply` 等 AI 回答；播放期间也会实时拼接 segment 的 `text`。完成的对话以 JSONL 写入 FFat/SPIFFS，约 24 KB 自动轮换，网页每秒刷新。

当前固件采用混合传输：HTTPS 用于服务器可用性探测和设备语音参数同步；对话流的录音上传、任务轮询和分段音频下载默认走 `http://voice.bsnlch.xyz`。这样可以避开 ESP32-S3 在 I2S/DMA、文件系统和 TLS 同时存在时最大连续堆过低导致的 `connection refused` 或 `send payload failed`。录音完成后固件会临时释放 I2S/DMA，上传完成并进入播放时再自动恢复音频硬件。若上传建立请求前失败，固件会重置云端连接后安全重试一次；不会重试可能已经送达服务器的发送或读取失败，以免重复创建任务。

本机 `secrets.h` 可提供首次烧录凭证，该文件已被 `.gitignore` 排除。NVS 中已有 Token 时优先使用 NVS。

## HTTPS

固件先同步时间，再使用 DigiCert Global Root G2 校验 `voice.bsnlch.xyz` 的证书链。根证书有效期至 2038 年；服务器短期站点证书正常续签无需重新烧录。HTTPS 或时间同步不可用时才尝试 HTTP 回退。

正常串口输出包括：

```text
[TLS] 时间同步完成，启用 CA 证书校验。
[NET] HTTPS selected: https://voice.bsnlch.xyz
```

## 板载 RGB 状态

- 紫色：启动
- 蓝色：待机
- 红色：录音
- 黄色：等待服务器/TTS
- 绿色：播放
- 紫红色：错误

这里只修改源码；按项目约定未替用户执行烧录。

## 网页对话显示规则

设备网页将对话分为“当前对话”和“历史记录”两块：

- 当前对话显示本轮用户识别文本，以及 AI 当前正在播放的单个 segment 文本；播放进入下一段时直接替换，不累计整段回答。
- 本轮完成后，将完整的用户文本和 AI 最终回答只写入一次历史记录。用户问题保留到下一轮开始，AI 当前片段在播放完成后清空。
- 历史记录继续保存在设备文件系统的 `/conversation_history.jsonl`，达到约 24 KB 后自动轮换。
- 服务端最终 `/jobs/{job_id}` 文本仅用于校正历史记录，不再覆盖“当前 AI”区域为整段回答。
