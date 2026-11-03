# BluFi 蓝牙配网

小聆固件支持通过小聆小程序使用 BluFi 配置 Wi-Fi。Wi-Fi 凭据由 `SsidManager` 保存在设备 NVS 中。

## SDK 配置

在 `idf.py menuconfig` 中进入 `Xiaoling Device → 配网模式`，选择 **BLE配网**（`CONFIG_PROV_MODE_BLE=y`），然后编译并烧录固件。

使用板级默认配置时，在首次配置命令中加入 `-DBOARD_TYPE=<板型>`。发布构建使用对应板型 `config.json` 中的配网设置。已有 `sdkconfig` 的工程可在上述菜单中切换配网方式。

## 进入配网

- 设备已有 Wi-Fi 配置时，上电后连接保存的网络。
- 设备 Wi-Fi 配置为空时，上电后自动进入蓝牙配网。
- 重新配网时，连续按功能键五次，设备清除保存的 Wi-Fi 并重启进入蓝牙配网。

配网界面显示小聆小程序二维码，设备广播名称为 `Xiaoling-XXXX`，后四位来自设备 SoftAP MAC 地址。

## 手机端操作

1. 扫描设备屏幕二维码，或在微信中搜索 **小聆AI** 小程序。
2. 选择 **ESP32 → 蓝牙配网**，授权使用蓝牙并连接对应的 `Xiaoling-XXXX` 设备。
3. 选择 Wi-Fi 并输入密码，等待小程序提示配置完成。
4. 设备在 Wi-Fi 连接成功并收到小程序的完成确认后，保存凭据并自动重启，随后连接小聆平台。

## 客户端协议

自定义客户端使用 [Espressif BluFi 协议](https://docs.espressif.com/projects/esp-idf/zh_CN/stable/esp32/api-guides/ble/blufi.html) 发送 Wi-Fi 凭据。设备联网成功后通过自定义数据上报 `pid` 和 `did`；客户端发送 `done` 完成配网确认，设备回复 `done` 后保存凭据并重启。

热点配网下的 `WiFi Configuration Method → Esp Blufi` 对应另一套通用 BluFi 实现。小聆小程序配网使用 `Xiaoling Device → 配网模式 → BLE配网`。
