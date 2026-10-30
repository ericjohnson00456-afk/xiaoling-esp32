# ESP32-S3 小聆 AI 2.0.0 SDK 配置说明

本文以 ESP32-S3-BOX-3 为例，说明小聆 AI 固件中与 SDK 配置相关的项目。配置和编译环境使用 **ESP-IDF v5.4.4**。

## 打开 SDK 配置

在项目根目录执行：

```bash
idf.py set-target esp32s3
idf.py menuconfig
```

在配置菜单中进入 **Xiaoling Device**，完成以下设置。保存配置后重新编译并烧录固件。

## 1. 选择开发板

进入 `Xiaoling Device → Board Type`，选择实际使用的开发板，例如：

- `Espressif ESP-BOX-3`
- `M5Stack CoreS3`

如果选择其他板型，请根据实际硬件配置确认 Flash、PSRAM、显示屏、音频编解码器和摄像头参数，并使用对应的芯片编译目标。

> 目前已验证测试 `ESP32-S3-BOX-3` 与 `M5Stack CoreS3`。其他板型需要根据硬件进行适配。

## 2. 配置小聆应用

在 `Xiaoling Device → LSPlatform 选项 → Product ID` 中选择应用类型：

- **内置**：使用项目默认的小聆应用；
- **自建应用**：填写自建应用的 Product ID 和 Product Secret。

自建应用需要先在小聆平台创建，并获取 Product ID 和 Secret ID。配置完成后保存，再执行编译和烧录。

![Product ID 配置](images/xiaoling/image.6.png)

## 3. 配置唤醒词

默认唤醒词为“小聆小聆”。如需修改：

1. 进入 `Xiaoling Device → Wake Word Implementation Type`。
2. 选择 `MicroWakeWord Detection`。
3. 进入 `MicroWakeWord 选项 → 模型`，选择需要的唤醒词模型。
4. 保存配置，重新编译并通过 USB 完整烧录固件及资源分区。

![唤醒词配置](images/xiaoling/image.7.png)

## 4. 配网模式

在 `Xiaoling Device → LSPlatform 选项 → 配网模式` 中选择：

- **WIFI热点配网**：设备启动热点，通过小聆配网页面配置 Wi-Fi；
- **BLE配网**：使用 BLE BluFi 配置 Wi-Fi，需确保芯片和板型支持蓝牙。

## 5. 资源与语言

根据设备需求配置以下项目：

- `Flash Assets`：选择默认资源、自定义资源或不烧录资源；
- `Default Language`：选择设备界面语言；
- 唤醒词、表情、摄像头等板级功能选项。

修改 SDK 配置后，建议执行完整编译，并重新烧录应用、分区表和资源分区。
