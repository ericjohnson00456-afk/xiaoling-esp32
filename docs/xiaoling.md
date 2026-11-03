# ESP32-S3 小聆 AI 2.0.0 SDK 配置说明

本文说明小聆 AI 固件中与 SDK 配置相关的项目。配置和编译环境使用 **ESP-IDF v5.4.4**。

## 打开 SDK 配置

在项目根目录执行：

```bash
idf.py set-target esp32s3
idf.py menuconfig
```

在配置菜单中进入 **Xiaoling Device**，完成以下设置。保存配置后重新编译并烧录固件。

`set-target` 会重新生成 `sdkconfig`，适用于首次配置或切换芯片。已有配置只需调整功能时，直接运行 `idf.py menuconfig`。

## 1. 选择开发板

进入 `Xiaoling Device → Board Type`，选择实际使用的开发板，例如：

- `Espressif ESP-BOX-3`
- `M5Stack CoreS3`

如果选择其他板型，请根据实际硬件配置确认 Flash、PSRAM、显示屏、音频编解码器和摄像头参数，并使用对应的芯片编译目标。

ESP32-S3 的通用默认值使用 Octal PSRAM；CoreS3 使用 Quad PSRAM，首次配置时应同时加载板级默认值：

```bash
idf.py -DBOARD_TYPE=m5stack-core-s3 set-target esp32s3
idf.py -DBOARD_TYPE=m5stack-core-s3 menuconfig
```

这两条命令会加载 CoreS3 的板型和 Quad PSRAM 默认配置。已有 `sdkconfig` 的选项优先；切换板型时，在 `menuconfig` 中同步调整板型及 PSRAM 模式。

![开发板配置](images/xiaoling/image.7.png)

## 2. 配置小聆应用

在 `Xiaoling Device → LSPlatform 选项 → Product ID` 中选择应用类型：

- **内置**：使用项目默认的小聆应用；
- **自建应用**：填写自建应用的 Product ID 和 Product Secret。

自建应用需要先在小聆平台创建，并获取 Product ID 和 Secret ID。配置完成后保存，再执行编译和烧录。

设备运行时优先读取 NVS 中保存的 `user/pid` 和 `user/sid`；各项为空时使用对应的编译默认值。普通 `idf.py flash` 保留 NVS 中的应用身份。

可通过 USB 串口 shell 查询当前生效的 Product ID 和 NVS 中保存的 Product ID：

```text
device get_pid
kv get string user pid
```

切换应用时，同时更新对应的 Product ID 和 Product Secret，然后重启设备。

## 3. 配置唤醒词

默认唤醒词为“小聆小聆”。如需修改：

1. 进入 `Xiaoling Device → Wake Word Implementation Type`。
2. 选择 `MicroWakeWord Detection`。
3. 进入 `MicroWakeWord 选项 → 模型`，选择需要的唤醒词模型。
4. 保存配置，重新编译并通过 USB 完整烧录固件及资源分区。

![唤醒词配置](images/xiaoling/image.6.png)

## 4. 配网模式

在 `Xiaoling Device → 配网模式` 中选择：

- **WIFI热点配网**：设备启动热点，通过小聆配网页面配置 Wi-Fi；
- **BLE配网**：使用 BLE BluFi 配置 Wi-Fi，需确保芯片和板型支持蓝牙。

## 5. 资源与语言

根据设备需求配置以下项目：

- `Flash Assets`：根据资源分区布局选择烧录方式，详见下表；
- `Default Language`：选择默认界面语言；当前拆分资源包仅包含中文和英文资源，选择其他语言需要另行补齐资源并验证；
- 唤醒词、表情、摄像头等板级功能选项。

| 分区布局与选择 | 资源来源及烧录行为 |
| --- | --- |
| 拆分资源布局，选择默认资源 | 从 `main/resource/assets/` 和所选唤醒词模型生成 `respak`、`tone`、`emoji`、`wake_word` 镜像，随 `idf.py flash` 烧录 |
| 含 `assets` 分区的布局，选择默认资源 | 按 SDK 配置生成单个资源镜像并烧录到 `assets` 分区 |
| 含 `assets` 分区的布局，选择自定义资源 | 从 `Custom Assets File` 指定的本地路径或 URL 获取资源镜像并烧录 |
| 选择保留现有资源 | 继续使用设备资源分区中的内容 |

项目默认的 `partitions/v2/16m.csv` 使用拆分资源布局。

修改 SDK 配置后，建议执行完整编译，并重新烧录应用、分区表和资源分区。

## 6. 编译与烧录

完成上述配置并保存后执行：

```bash
idf.py build
idf.py -p <串口号> flash monitor
```

`flash` 会按构建目录的 `flash_args` 烧录应用、引导程序、分区表和启用的资源镜像，并保留 NVS 中的 Wi-Fi、应用身份及用户配置。合并固件用于从 `0x0` 开始的整包烧录，写入范围包含分区之间的填充区域。升级已有设备并保留配置时，使用 `idf.py flash`。

启动日志中的 `app_init: App version` 和 `Compile time` 可用于核对烧录后的版本与编译时间。退出 `monitor` 使用 `Ctrl+]`。
