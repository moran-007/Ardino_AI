# 中文离线唤醒模型包

`srmodels.bin` 由乐鑫官方 `esp-sr` 2.4.6（提交 `cfdbb52172d4f1b3e5f7469d6c3c6e5dd0738143`）的 WakeNet9 模型使用官方 `model/pack_model.py` 打包。

- 大小：3,784,096 字节
- SHA-256：`b55e4f1c3ee4267762577b19485a79b1a1ee84da166e1f0626596b1a2c035843`
- 分区：`model`，偏移 `0xC10000`，最大 `0x3E0000`
- 默认唤醒词：`你好小智`
- 运行时可选模型：见 [`../WAKE_WORD_MODELS.md`](../WAKE_WORD_MODELS.md)

Arduino IDE 的 ESP-SR 构建钩子默认复制开发板包自带的英文 `Hi ESP` 模型。编译前运行：

```powershell
.\install_offline_wake_models.ps1
```

脚本会先备份原模型，再安装本目录的中文模型包。需要恢复开发板包默认模型时运行：

```powershell
.\install_offline_wake_models.ps1 -Restore
```
