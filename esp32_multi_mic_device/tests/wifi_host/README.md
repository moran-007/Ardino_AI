# Wi-Fi 扫描回归检查

在仓库根目录运行：

```powershell
.\esp32_cloud_ota_device\tests\wifi_host\run.ps1 -Gxx D:\msys64\ucrt64\bin\g++.exe
```

直接编译生产 `wifi_scan.cpp`，用替身提供 Wi-Fi 扫描结果和已保存网络，复用显示主机检查的 Arduino 基础类型。覆盖：仅选择可见的已保存网络、RSSI 排序、同名 AP 去重、手动优先项不可见时跳过、结果容量限制、每次重连强制刷新、扫描中/取消/失败/空结果不使用旧候选、网页前 20 条之外的弱信号已保存网络，以及已忘记网络不再进入候选。

该检查不模拟无线驱动、路由器认证或网页服务器；这些路径仍须通过串口和设备网页验证。输出保存在仓库的 `build-size-fix/wifi-host`。
