# M5Stack StickS3

StickS3 使用 ESP32-S3-PICO-1-N8R8，配有 8MB Flash、8MB Octal PSRAM、
135×240 ST7789 屏幕、ES8311 单麦音频、AW8737 功放和 M5PM1 电源管理。

板级代码来自上游 [StickS3 支持提交 b392c630](https://github.com/78/xiaozhi-esp32/commit/b392c630aa74bc9bb0ff5791bb89b1d8c96b3181)。
引脚和电源控制参考 [M5Stack 官方资料](https://docs.m5stack.com/en/core/StickS3)。

## Hardware

* SYS_I2C/I2C0
    * SCL -- G48
    * SDA -- G47
* PMIC: M5PM1
    * Interface: I2C0@0x6E
* LCD
    * Power: M5PM1_G2 高电平使能
    * Resolution: 135x240
    * Driver: ST7789
    * Interface: SPI
        * MOSI -- G39
        * SCLK -- G40
        * CS   -- G41
        * RS   -- G45
    * BL -- G38
* Audio
    * Power: M5PM1_G2 高电平使能
    * ES8311@0x18
        * Control Interface: SYS_I2C
        * Data Interface: I2S0
            * MCLK   -- G18
            * BCLK   -- G17
            * ASDOUT -- G16
            * LRCK   -- G15
            * DSDIN  -- G14
    * PA -- PM1_G3
* Key
    * KEY1 -- G11
* IMU
    * BMI270@0x68
        * Interface: SYS_I2C
* IR
    * TX -- G46
    * RX -- G42

## 编译与烧录

使用 ESP-IDF 5.4.4，在工程根目录运行：

```sh
python scripts/release.py m5stack-stick-s3
idf.py -p <串口> flash
```

发布脚本会重建 `build/` 并更新 `sdkconfig`；先保留需要继续使用的构建产物。
验证新改动前应移走已有的同名发布 ZIP，避免脚本跳过构建。
若设备没有自动进入下载模式，连接 USB 后长按侧面 RESET，直到绿色指示灯闪烁。
烧录后若绿灯仍持续闪烁，短按一次 RESET 启动应用。

## 功能与操作

- 正面 A 键使用应用统一按键逻辑：单击返回主页或开始/结束对话，三击显示
  配置二维码，四击无操作，五击清除 Wi-Fi 并重新配网，八击及以上恢复
  出厂设置。双击为公共拍照入口，本板无摄像头，会提示未检测到摄像头。
- A 键长按用于停止闹钟或跳过待确认的固件升级；运行小应用时，A/B 键短按
  分别作为 `function` / `user` 事件，持续按住 A 键 3 秒退出小应用。
- 侧面 B 键单击将逻辑音量设为 60%。
- 音频输入和输出均为 16kHz。功放通过 M5PM1 G3 控制，实际输出音量限制为
  逻辑音量的 60%，减少电池供电时的大音量压降。
- 小屏使用 16px 中文字体；表情按屏幕可用区域缩放，状态栏保留网络和电池信息。
- 原生 USB Serial/JTAG 提供启动日志和串口命令。
- 默认使用 BluFi 蓝牙配网；具备 PSRAM 时默认启用 Lua 小应用，画布使用实际
  135×240 屏幕尺寸。

8MB 配置使用 `partitions/v2/8m.csv`。基础资源只打包选中的 16px 字体。
该布局只有一个应用槽，支持独立资源更新，
不支持双槽应用 OTA 或回滚。

默认关闭设备端 AEC。
红外、IMU 和 RTC 不属于本板型的语音助手驱动范围。
