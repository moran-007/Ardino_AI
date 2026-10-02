# 显示主机检查

便签页另覆盖两种面板、四个方向、待机/底部语音条模式的 64 种组合：两类都有时左右圆角窗口，只有便签或待办时单窗口占满宽度，两类都空时不画空窗口。检查窗口分隔、圆角边框、底部条边界、窄列长中文分页及句尾标点不单独换行。生成 `*-memos-both.png`、`*-memos-notes.png`、`*-memos-todos.png` 和 `*-memos-empty.png` 原生像素预览。

`test_display.cpp` 直接包含生产 `robot_display.cpp`，链接生产 `robot_avatar.cpp` 和本机安装的 Adafruit GFX / U8g2 实现；使用实际中文字库。此目录只替换 Arduino GPIO、SPI、NVS、FreeRTOS 和二维码编码入口，不访问串口或设备，不会被 Arduino 作为固件源码编译。

覆盖亮度范围、ADC 平滑及端点、PWM 失败回退、NVS 失败不更新当前值、冷启动恢复、两个触发源的四种显示组合；在两种屏幕四个旋转方向上渲染 192 个场景，检查照片缓存、底部条清除后的恢复及文字翻页。另检查 48 位表情选择的严格解析、非法与重复 ID、单组固定/多组不连续重复、保存失败、8 秒/手动预览恢复和忙时拒绝；逐帧检查 48 组表情动作不超出区域、不同时间的身体动作确实变化、二维码缓存失败重试。96 张 BMP 缩略图使用生产 HTTP 输出和绘制代码，校验尺寸并检查横屏/竖屏各有 48 个不同画面。

二维码编码替身使用 Python `qrcode` 生成固定测试数据的模块；生产绘制函数负责白边、缩放和排版，`zxing-cpp` 独立解码 16 张原生分辨率截图。它证明二维码绘制和内容布局，不是对 ESP32 随附二维码编码器的运行验证；后者在固件编译链接中检查。

Windows 需要 gcc/g++、Python（Pillow、qrcode、zxing-cpp），以及已安装的 Arduino 绘制库。依赖可装到输出目录的 `pythonlibs`，无需修改项目或系统 Python。

```powershell
.\esp32_cloud_ota_device\tests\display_host\run.ps1 `
  -ArduinoLibraries 'E:\Users\moran\Documents\Arduino\libraries' `
  -Python 'C:\Users\moran\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe'
```

结果在根目录被 Git 忽略的 `build-display-controls/`：原生像素截图、`display-preview.png`、`display-review.png`、`expression-catalog.png`（48 组总览）、实际 C++ 输出的网页表单与分类数据。用以下本地夹具检查表单交互：

```powershell
python .\esp32_cloud_ota_device\tests\display_host\serve_fixture.py .\build-display-controls
```

仅监听 `127.0.0.1:8789`；`/controls#display` 是显示设置，`/controls#expressions` 是表情库，`/setup` 是初始化登录页。分类和缩略图来自生产 C++ 的生成文件；提交只回显字段，**不模拟或验证固件会话、路由、NVS、Wi-Fi**。固件编译仍使用 README 中的 ESP SR 16M / OPI PSRAM 配置；屏幕观感、相机扫码、音频并行显示与实体 PWM/ADC 需实机验收。
