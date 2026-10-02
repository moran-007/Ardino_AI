# 自定义裁剪检查

`photo_crop_fixture.py` 从生产 `robot_photo_ui.h` 提取页面，并使用 `web_admin_ui.h` 的实际样式；服务仅监听 `127.0.0.1:8791`。上传记录保存在夹具内存，测试不会上传或删除设备照片。

```powershell
python .\esp32_cloud_ota_device\tests\photo_crop_fixture.py
npx --package @playwright/cli playwright-cli -s=photo-crop open http://127.0.0.1:8791
npx --package @playwright/cli playwright-cli -s=photo-crop snapshot
npx --package @playwright/cli playwright-cli -s=photo-crop run-code --filename .\esp32_cloud_ota_device\tests\photo_crop_load.js
npx --package @playwright/cli playwright-cli -s=photo-crop snapshot
npx --package @playwright/cli playwright-cli -s=photo-crop run-code --filename .\esp32_cloud_ota_device\tests\photo_crop_check.js
```

每次完整检查从新的夹具进程开始，确保上传列表为空。电脑上如无法启动默认 Chrome，可通过 Playwright CLI 的 `--config` 选择已安装的 Chromium。

覆盖真实指针拖动、滑块缩放、键盘微调、两张图片各自的裁剪恢复、390px 手机布局、四种目标屏幕尺寸的裁剪边界，以及上传的 RGB565 SHA-256 与各自预览精确一致。320×240 图片仍为 153,608 字节。截图保存在 `output/playwright/photo-crop-mobile.png`。
