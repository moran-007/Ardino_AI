# 离线中文唤醒名字

## 本固件已内置，可在网页实时切换

| 唤醒名字 | WakeNet 模型 ID |
|---|---|
| 你好小智（默认） | `wn9_nihaoxiaozhi_tts` |
| 小爱同学 | `wn9_xiaoaitongxue` |
| 你好小鑫 | `wn9_nihaoxiaoxin_tts` |
| 小美同学 | `wn9_xiaomeitongxue_tts` |
| 小龙小龙 | `wn9_xiaolongxiaolong_tts` |
| 喵喵同学 | `wn9_miaomiaotongxue_tts` |
| Hi 喵喵 | `wn9_himiaomiao_tts` |
| Hi 莉莉 | `wn9_hilili_tts` |
| 小宇同学 | `wn9_xiaoyutongxue_tts2` |
| 小明同学 | `wn9_xiaomingtongxue_tts2` |
| 小鹿小鹿 | `wn9_xiaoluxiaolu_tts2` |
| 你好小安 | `wn9_nihaoxiaoan_tts2` |
| 嗨小欧 | `wn9_hai1xiao3ou1_tts3` |

13 个模型共 3,784,096 字节，已接近当前 3.9 MB 模型分区上限，因此不能再同时内置更多完整 WakeNet9 模型。

## 乐鑫 ESP-SR 2.4.6 中其余中文或中英混合备选

下列名称可作为后续“替换模型包”的备选，但当前 1.4.1 未同时烧入：

- Hi 乐鑫、Hi ESP、你好喵伴、Hi 瓦力、Hi 小星
- Hi 泰力、小滨小滨 / 小冰小冰、Hi 小巫、小鸭小鸭、璃奈板、小酥肉
- 小康同学、小箭小箭、小特小特、你好小益、你好百应、你好东东
- 你好小脉、你好小瑞、小珈小珈、小峰小峰、嗨小象、你好星宝

## 关于“自定义名字”

网页可以在已经烧入的 13 个模型之间切换，但不能把任意文字直接变成唤醒词。离线 WakeNet 识别依赖训练后的模型文件；完全自定义名字需要按乐鑫 WakeNet 定制流程生成模型，再重新打包并 USB 烧录 `model` 分区。仅更改网页文字或 NVS 配置不会让模型学会新名字。

部分备选名称可能涉及第三方商标，商用前应自行确认使用权。
