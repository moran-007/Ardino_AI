# ESP32-S3 端离线识别后续路线

当前稳定 Arduino 程序不修改。ESP32 端实验应新建独立 ESP-IDF 工程，采用乐鑫官方 ESP-SR，而不是把电脑端 ONNX 模型强行移植到开发板。

## 能放入 ESP32-S3 的功能

1. ESP-SR AFE：单麦克风降噪、VAD、AGC；恢复扬声器后再加入 AEC 播放参考通道。
2. WakeNet：本地唤醒词。
3. MultiNet：本地中文固定命令，最多约 200 条，适合“开始对话、停止播放、调高音量、重新联网”等设备命令。
4. VAD 截取的人声 PCM 继续发送给电脑 Zipformer，完成自由中文问句识别。

官方资料：

- ESP-SR 入门：https://docs.espressif.com/projects/esp-sr/zh_CN/latest/esp32s3/getting_started/readme.html
- AFE：https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/audio_front_end/index.html
- MultiNet：https://docs.espressif.com/projects/esp-sr/zh_CN/latest/esp32s3/speech_command_recognition/README.html
- 模型分区：https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/flash_model/README.html

## 不能合理放入当前开发板的功能

- Zipformer INT8 约 350 MB。
- SenseVoice INT8 约 228 MB。
- 当前 ESP32-S3 只有 16 MB Flash 和 8 MB PSRAM，无法容纳这两种自由文本识别模型及其运行内存。
- ESP-SR MultiNet 是命令词识别，不是任意中文听写，不能替代电脑端 ASR。

## 推荐迁移顺序

1. 保持当前稳定固件，先在电脑端完成 Zipformer + 自动增益 + VAD 的实测。
2. 新建 ESP-IDF 实验工程，只验证 INMP441 -> AFE AGC/NS/VAD -> USB PCM，不接 AI API、不覆盖当前固件。
3. 加入 WakeNet 和少量 MultiNet 中文命令，验证误唤醒率和命令准确率。
4. 最后将 VAD 后的语音片段接回电脑 ASR/DeepSeek；只有固定控制命令完全在 ESP32 本地执行。

