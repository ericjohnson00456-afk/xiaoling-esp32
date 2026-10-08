# M5Stack CoreS3 SE

板型标识：`m5stack-core-s3-se`；menuconfig 选项：`M5Stack CoreS3 SE`。

项目默认板型仍为 `esp-box-3`（ESP-BOX-3），全局 SDK 默认配置保持原样。
本目录的 `sdkconfig.defaults` 仅在显式指定 SE 板型时加载；以下编译命令
将 SE 的配置写入独立构建目录。

## 官方资料与硬件差异

核对日期：2026-10-08，原理图版本：v1.0。

- [CoreS3 SE 官方规格与 PinMap](https://docs.m5stack.com/en/core/CoreS3-SE)
- [CoreS3 SE 官方原理图](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/498/Sch_M5_CoreS3_SE_v1.0.pdf)
- [CoreS3 官方规格](https://docs.m5stack.com/en/core/CoreS3)
- [M5Unified 官方板型与音频初始化](https://github.com/m5stack/M5Unified/blob/master/src/M5Unified.inl)
- [M5Unified AXP2101 电池检测](https://github.com/m5stack/M5Unified/blob/master/src/utility/power/AXP2101_Class.inl)

SE 使用 ESP32-S3、16 MB Flash、8 MB **Quad PSRAM**，保留 ILI9342C
320×240 屏幕、FT6336U 触摸、ES7210 双麦克风、AW88298 扬声器、AXP2101、
AW9523B、BM8563 RTC 和 microSD 卡槽。SE 没有 GC0308 摄像头、LTR-553
接近传感器、BMI270 IMU、BMM150 磁力计，出厂也不带电池。

本项目使用到的 LCD、触摸、音频及 SD 引脚与 CoreS3 相同，但两个板型独立维护。
SE 的板级实现、引脚配置和音频驱动全部位于本目录，不包含或编译
`m5stack-core-s3` 目录中的文件；仅使用项目公共基础设施和通用外设驱动。
SE 没有板级摄像头初始化代码，`GetCamera()` 返回 `nullptr`。
无电池时不报告电量，也不启用电池休眠/自动关机计时；接入兼容电池底座后，
通过 AXP2101 的电池存在状态恢复电量检测和放电省电逻辑。

| 外设信号（以 ESP32 为方向） | GPIO / 控制器 |
| --- | --- |
| 内部 I2C SDA / SCL | 12 / 11 |
| I2S MCLK / BCLK / WS | 0 / 34 / 33 |
| 麦克风数据输入 DIN | 14（ES7210 SDOUT1） |
| 扬声器数据输出 DOUT | 13（AW88298 DATAI） |
| LCD MOSI / SCLK / CS / DC | 37 / 36 / 3 / 35 |
| LCD RESET | AW9523B P1_1 |
| 触摸 RESET / INT | AW9523B P0_0 / P1_2 |
| SD MOSI / MISO / SCLK / CS | 37 / 35 / 36 / 4 |
| 电源按键 | AXP2101 PWRON，非 ESP32 GPIO |

**官方网页勘误依据：** 检索时 SE 网页的音频 PinMap 将 G13/G14 的音频方向
写反。原理图第 3、4 页明确显示 GPIO13 → I2S_DATO → AW88298 DATAI，
ES7210 SDOUT1 → I2S_DATI → GPIO14；M5Unified 的 CoreS3/SE 共用分支也使用
`pin_data_in = GPIO_NUM_14` 和 `pin_data_out = GPIO_NUM_13`。本板型按原理图
及官方驱动配置，没有交换这两个引脚。

LCD 与 SD 共用 SPI3，GPIO35 在 LCD 事务中为 DC、在 SD 事务中为 MISO，
继续使用项目已有的 SPI 总线仲裁。

## 编译

在已加载 ESP-IDF 5.4.4 环境的终端中，为 SE 使用独立构建目录和 sdkconfig，
避免沿用 CoreS3 的摄像头配置或其他 S3 板型的 Octal PSRAM 配置：

```bash
idf.py -B build-core-s3-se -DSDKCONFIG=build-core-s3-se/sdkconfig -DIDF_TARGET=esp32s3 -DBOARD_TYPE=m5stack-core-s3-se -DBOARD_NAME=m5stack-core-s3-se build
```

该命令自动加载本目录的 `sdkconfig.defaults`。若使用 menuconfig，选中
`M5Stack CoreS3 SE` 后也需确认 PSRAM 为 Quad、Flash 为 16 MB；仅切换菜单
板型不会重新应用板级 defaults。

发布脚本也可识别此板型（使用项目默认构建目录及 sdkconfig）：

```bash
python scripts/release.py m5stack-core-s3-se
```

## 实机验证

2026-10-08 独立实现已通过 ESP-IDF 5.4.4 完整构建和固件打包。编译源码清单、
编译器头文件依赖及库符号检查确认不依赖 `m5stack-core-s3` 目录。另用全局
默认配置生成临时配置，确认默认仍为 ESP-BOX-3 / Octal PSRAM。
未烧录，未完成实机验证。

编译通过不代表已经完成实机验证。上板后需检查启动日志识别 8 MB PSRAM、
屏幕显示/背光/触摸、录音及扬声器播放、电源键、SD 卡与屏幕并发访问；确认
未初始化 GC0308，未接电池时板级接口不报告电量且不会因电池省电计时自动关机。
