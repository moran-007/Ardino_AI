# 陪伴功能验证记录（2026-10-02）

授权范围：用户确认实现单麦克风功能，个人特征当前保存在设备并云端备份，后续可改 SD；自定义唤醒允许实验准确率；新增番茄钟专注模块。范围包含前面讨论的每日简报和互动。现有未提交修改保留，未同步多麦克风工程。

## 可复现检查

从仓库根目录 PowerShell 运行：

```powershell
& esp32_cloud_ota_device/tests/profile_host/run.ps1
& esp32_cloud_ota_device/tests/focus_host/run.ps1
& esp32_cloud_ota_device/tests/custom_wake_host/run.ps1
& esp32_cloud_ota_device/tests/interaction_host/run.ps1
& .venv/Scripts/python.exe -m unittest discover -s cloud_server/tests -q
```

主机 C++ 检查使用本机 MSYS2 GCC 与已安装 ArduinoJson，驱动/文件系统/NVS 采用可注入失败的替身。唤醒执行真实 FFT、分段、DTW；监听集成运行真实 `offline_wake.cpp`，替换硬件和官方模型接口。动作检查运行真实舵机 PWM 命令代码。

| 检查 | 本次结果 |
| --- | --- |
| 个人记忆 | 保存、重启、观察/主动确认、删除语法、版本、关闭开关、写失败、损坏恢复、旧副本清理失败与同值重试通过 |
| 番茄钟 | 234 项状态机检查，25 项实际 UI JavaScript DOM 检查通过 |
| 实验唤醒 | 1591 项声音/事务/录入检查、UI 流程、17 项无官方模型集成、22 项有模型集成通过 |
| 官方唤醒回归 | SDK `1` 触发，`-1/0` 不触发通过 |
| 回应动作 | 704 项断言及实际 UI JavaScript 流程通过 |
| 云端全套 | 运行 148 项：147 通过，1 项 POSIX 文件权限检查在 Windows 跳过；新增边界后再运行 15 项个人记忆检查通过。供应商为测试替身，无付费调用 |
| 屏幕 | 192 个原有场景、各方向专注页和不同说话表情检查通过；16 张二维码独立解码、48 组双造型缩略图通过 |
| 浏览器 | 实际网页模板 + 本地模拟 REST：添加记忆、开始/暂停专注、保存唤醒名称、模拟录入与启用通过 |
| ESP32 目标构建 | 最终源码编译通过，Arduino-ESP32 3.3.12；应用统计 2,351,558 字节，生成 BIN 2,351,712 字节，占 3 MiB 槽约 74%；全局 RAM 80,420 字节 |

目标构建使用现有 16 MB Flash、OPI PSRAM、ESP SR 16M 配置：

```powershell
& 'D:/Arduino CLI/arduino-cli.exe' compile --fqbn 'esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=default,FlashSize=16M,PartitionScheme=esp_sr_16,PSRAM=opi' --libraries 'E:/Users/moran/Documents/Arduino/libraries' --build-path 'E:/moran_project/arduino/build-personal-features' 'E:/moran_project/arduino/esp32_cloud_ota_device'
& esp32_cloud_ota_device/verify_offline_wake_build.ps1 -BuildDirectory build-personal-features
```

离线唤醒构建检查通过：引导镜像声明 16 MB，官方 13 模型包 SHA-256 与项目一致，实际上传配方包含 `0xC10000 srmodels.bin`。`flash_args` / `.merged.bin` 仍不能代替完整首次上传流程。22 个最终源文件与 Arduino 构建输入比对通过（剔除工具生成的行标记/前置声明），源摘要保存于构建目录 `companion-source-hashes.json`。

失败证明：实现前对应状态机、声学、存储和互动基线失败；新增个人记忆回归曾在不合法编号删除、观察提升、损坏恢复、说话表情、旧副本删除和同值保存清理处失败，修复后通过。真实 ESP32 编译发现并修复 Arduino `PI` / `constrain` 宏兼容错误。

独立只读审查分工：云端切片作者审查设备个人记忆/界面/表情；番茄钟切片作者审查唤醒及动作集成；父任务检查所有接口、运行 focused checks 和整机编译。审查发现的旧模板残留/恢复和失效唤醒回归脚本已修复。

阶段结论：Implementation → 独立 Change Review → 本机验证通过；无剩余 P1/P2 审查问题。本次交付为可审阅的源码、使用说明与本机构建，未进入发布执行。

## 实机与部署边界

未刷写、未部署云端、未发布 OTA。网页测试使用模拟状态，不证明设备 HTTP 或麦克风采集。未测真实自定义唤醒准确率、CPU/RTOS 时序、各距离噪声表现、音频时的 NVS 写入延迟、屏幕流畅度、舵机机械行程和电源负载。上线前应按这些项目在目标硬件确认；合成声学结果没有准确率承诺。

存储通过文件替换避免常见写入失败覆盖，不承诺物理断电/文件系统损坏绝不丢失或介质安全擦除。单设备单使用者、现有云端对话/诊断数据留存策略继续适用。备份关闭与上下文发送的区别已显示在产品界面和功能文档。
