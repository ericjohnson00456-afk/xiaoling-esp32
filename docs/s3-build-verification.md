# ESP32-S3 构建与语音链路验证记录

验证日期：2026-10-23。环境：EIM / ESP-IDF 5.4.4，目标 `esp32s3`。

## 覆盖范围

80 个板型目录、86 个构建变体均完成编译、链接、镜像生成和应用分区大小检查。
原有 81 个发布变体，以及原先没有 `config.json` 的 5 个板型的默认配置均已构建；
后者已补上发布配置：`atk-dnesp32s3m-4g`、`atk-dnesp32s3m-wifi`、
`bread-compact-wifi-lcd`、`bread-compact-wifi-s3cam`、`kevin-sp-v3-dev`。
这些默认配置的 Flash/PSRAM 容量仍需在对应实物上核实。

构建使用独立的 `SDKCONFIG` 和构建目录，保留项目当前 BOX3 配置。例如：

```sh
eim run 'idf.py --ccache -B /tmp/xiaoling-s3-verify/<name>/build -DSDKCONFIG=/tmp/xiaoling-s3-verify/<name>/sdkconfig -DIDF_TARGET=esp32s3 -DBOARD_TYPE=<board> -DBOARD_NAME=<name> build' v5.4.4
```

每个独立 sdkconfig 由相应 `CONFIG_BOARD_TYPE_*=y` 和 `config.json` 的
`sdkconfig_append` 初始化，再由 IDF 加载项目默认值。CameraPlus v1.2 使用其独立板型枚举。
逐项最终日志和镜像路径保存在 `/tmp/xiaoling-s3-verify/final-results.json`。
下表中的日志路径相对于 `/tmp/xiaoling-s3-verify/`；`/tmp` 为本机临时产物，可能被清理。

## 兼容性调整与限制

- 修复旧显示接口、触摸屏配置初始化顺序、GPIO 枚举、缺失标准库声明及 LVGL 标志类型错误。
- Electron Bot 和 Otto 统一使用通用 LCD 的 PNG 表情逻辑，移除旧 GIF 显示类及构建排除判断。
  SenseCAP Watcher 改用通用 LCD 显示，原专用圆屏布局未验证。
  SenseCAP 的旧工厂串口 REPL 移除，使用应用统一串口命令。
- 8MB S3 采用 `8m.csv`：单应用槽和四个 ROMFS 分区。
  支持资源更新，不支持双槽应用 OTA 及回滚。
- 所有 Flash 容量的 `respak` 统一只打包简体中文（`zh-CN`）和英文（`en-US`），
  其他语言源文件保留。中英资源包实测 1,487,872 字节，可放入 8MB 布局的 1.5MiB 分区。
- 无唤醒模型的配置生成有效空 `wake_word.bin`，避免打包/烧录时缺少文件。
- 修正误标为 S3 的 Waveshare C6 AMOLED 2.06 的目标为 C6；C6、其他 C 系列及 P4 不计入本次验证。

## BOX3 实物验证

唯一实物为 `/dev/ttyACM0` 的 ESP-BOX-3。其他版型仅完成构建检查，
其引脚、麦克风、功放、屏幕、网络和性能须在对应硬件上另行验证。

- 正确 BOX3 固件启动，资源挂载、MicroWakeWord/VAD 初始化、Wi-Fi 和云端连接正常。
- 串口 `test wake xiaoling` 建立会话，Opus 约 50 帧/秒上报，测试段 dropped=0，
  日志出现 `上行链路[WS结束] ... ok=1`。
- `test tts_text` 请求新云端 TTS URL，经 HTTP 200、MP3 解码、首个 PCM 输出、
  `ESP_AUD_SIMPLE_PLAYER_FINISHED` 并回到待机；最终固件复测首个 PCM 在拉流后
  340ms 输出，累计输出 83 个 PCM 块。
- BOX3 的临时 8MB 布局固件也通过启动、资源更新与 Opus 上报；测试后恢复 16MB BOX3 配置。
- 上报和 TTS 使用独立命令触发。测试时云端 IAT 为空，尚未验证真实人声在同一会话中的
  ASR→回复→TTS 闭环，也未据此判定语音打断效果或卡顿改善。
- 最终固件曾遇到激活 API 连接超时；重启后恢复连接和待机，并重新完成 TTS 与 Opus
  测试。筛选后的运行证据在本机 `/tmp/xiaoling-s3-verify/box3-smoke-evidence.log`，
  最终烧录记录在 `/tmp/xiaoling-box3-final-flash.log`。

复现串口步骤见 [语音冒烟测试](s3-voice-smoke-test.md)。

## 逐项构建结果

应用镜像大小和应用槽大小均以字节为单位。所有条目均通过。

| 板型目录 | 构建名称 | Flash 配置 | 应用 / 应用槽（字节） | 最终日志 |
| --- | --- | --- | --- | --- |
| aipi-lite | aipi-lite | 16MB | 4034976 / 4653056 | `aipi-lite/build.log` |
| atk-dnesp32s3 | atk-dnesp32s3 | 16MB | 4069232 / 4653056 | `atk-dnesp32s3/build.log` |
| atk-dnesp32s3-box | atk-dnesp32s3-box | 16MB | 4149008 / 4653056 | `atk-dnesp32s3-box/build.log` |
| atk-dnesp32s3-box0 | atk-dnesp32s3-box0 | 16MB | 4011072 / 4653056 | `atk-dnesp32s3-box0/build.log` |
| atk-dnesp32s3-box2-4g | atk-dnesp32s3-box2-4g | 16MB | 4099248 / 4653056 | `atk-dnesp32s3-box2-4g/build.log` |
| atk-dnesp32s3-box2-wifi | atk-dnesp32s3-box2-wifi | 16MB | 4022832 / 4653056 | `atk-dnesp32s3-box2-wifi/build.log` |
| atk-dnesp32s3m-4g | atk-dnesp32s3m-4g | 16MB | 3954320 / 4653056 | `atk-dnesp32s3m-4g/build.log` |
| atk-dnesp32s3m-wifi | atk-dnesp32s3m-wifi | 16MB | 4019968 / 4653056 | `atk-dnesp32s3m-wifi/build.log` |
| atom-echos3r | atom-echos3r | 8MB | 3725488 / 4980736 | `atom-echos3r/build.log` |
| atoms3-echo-base | atoms3-echo-base | 8MB | 3845712 / 4980736 | `atoms3-echo-base/build3.log` |
| atoms3r-cam-m12-echo-base | atoms3r-cam-m12-echo-base | 8MB | 3858048 / 4980736 | `atoms3r-cam-m12-echo-base/build.log` |
| atoms3r-echo-base | atoms3r-echo-base | 8MB | 3996432 / 4980736 | `atoms3r-echo-base/build.log` |
| bread-compact-ml307 | bread-compact-ml307 | 16MB | 4058224 / 4653056 | `bread-compact-ml307/build.log` |
| bread-compact-ml307 | bread-compact-ml307-128x64 | 16MB | 4058272 / 4653056 | `bread-compact-ml307-128x64/build.log` |
| bread-compact-wifi | bread-compact-wifi | 16MB | 3981392 / 4653056 | `bread-compact-wifi/build.log` |
| bread-compact-wifi | bread-compact-wifi-128x64 | 16MB | 3981456 / 4653056 | `bread-compact-wifi-128x64/build.log` |
| bread-compact-wifi-lcd | bread-compact-wifi-lcd | 16MB | 3982704 / 4653056 | `bread-compact-wifi-lcd/build.log` |
| bread-compact-wifi-s3cam | bread-compact-wifi-s3cam | 16MB | 4048352 / 4653056 | `bread-compact-wifi-s3cam/build.log` |
| df-k10 | df-k10 | 16MB | 4072784 / 4653056 | `df-k10/build.log` |
| df-s3-ai-cam | df-s3-ai-cam | 16MB | 3850720 / 4653056 | `df-s3-ai-cam/build.log` |
| doit-s3-aibox | doit-s3-aibox | 16MB | 3706144 / 4653056 | `doit-s3-aibox/build.log` |
| du-chatx | du-chatx | 16MB | 3997264 / 4653056 | `du-chatx/build.log` |
| echoear | echoear | 16MB | 4018352 / 4653056 | `echoear/build.log` |
| electron-bot | electron-bot | 16MB | 3985936 / 4653056 | `electron-bot/build2.log` |
| esp-box | esp-box | 16MB | 4008048 / 4653056 | `esp-box/build.log` |
| esp-box-3 | esp-box-3 | 16MB | 4026624 / 4653056 | `esp-box-3/build.log` |
| esp-box-lite | esp-box-lite | 16MB | 4015408 / 4653056 | `esp-box-lite/build.log` |
| esp-s3-lcd-ev-board | esp-s3-lcd-ev-board-1p4 | 16MB | 4033680 / 4653056 | `esp-s3-lcd-ev-board-1p4/build.log` |
| esp-s3-lcd-ev-board | esp-s3-lcd-ev-board-1p5 | 16MB | 4033680 / 4653056 | `esp-s3-lcd-ev-board-1p5/build.log` |
| esp-s3-lcd-ev-board-2 | esp-s3-lcd-ev-board-2 | 16MB | 4041632 / 4653056 | `esp-s3-lcd-ev-board-2/build2.log` |
| esp-sparkbot | esp-sparkbot | 16MB | 4080960 / 4653056 | `esp-sparkbot/build.log` |
| esp32-s3-touch-amoled-1.8 | esp32-s3-touch-amoled-1.8 | 16MB | 4009552 / 4653056 | `esp32-s3-touch-amoled-1.8/build.log` |
| esp32-s3-touch-lcd-1.46 | esp32-s3-touch-lcd-1.46 | 16MB | 3984480 / 4653056 | `esp32-s3-touch-lcd-1.46/build.log` |
| esp32-s3-touch-lcd-1.85 | esp32-s3-touch-lcd-1.85 | 16MB | 3984608 / 4653056 | `esp32-s3-touch-lcd-1.85/build.log` |
| esp32-s3-touch-lcd-1.85c | esp32-s3-touch-lcd-1.85c | 16MB | 3989008 / 4653056 | `esp32-s3-touch-lcd-1.85c/build.log` |
| esp32-s3-touch-lcd-3.5 | esp32-s3-touch-lcd-3.5 | 16MB | 4076352 / 4653056 | `esp32-s3-touch-lcd-3.5/build2.log` |
| esp32s3-korvo2-v3 | esp32s3-korvo2-v3 | 16MB | 4089296 / 4653056 | `esp32s3-korvo2-v3/build.log` |
| genjutech-s3-1.54tft | genjutech-s3-1.54tft | 16MB | 4029872 / 4653056 | `genjutech-s3-1.54tft/build.log` |
| hu-087 | hu-087 | 16MB | 3960832 / 4653056 | `hu-087/build.log` |
| jiuchuan-s3 | jiuchuan-s3 | 16MB | 4048048 / 4653056 | `jiuchuan-s3/build.log` |
| kevin-box-2 | kevin-box-2 | 16MB | 4090224 / 4653056 | `kevin-box-2/build.log` |
| kevin-sp-v3-dev | kevin-sp-v3-dev | 16MB | 4049120 / 4653056 | `kevin-sp-v3-dev/build2.log` |
| kevin-sp-v4-dev | kevin-sp-v4-dev | 16MB | 4085968 / 4653056 | `kevin-sp-v4-dev/build.log` |
| kevin-yuying-313lcd | kevin-yuying-313lcd | 16MB | 4014656 / 4653056 | `kevin-yuying-313lcd/build.log` |
| labplus-ledong-v2 | labplus-ledong-v2 | 16MB | 4017584 / 4653056 | `labplus-ledong-v2/build.log` |
| labplus-mpython-v3 | labplus-mpython-v3 | 16MB | 4017376 / 4653056 | `labplus-mpython-v3/build.log` |
| lichuang-dev | lichuang-dev | 16MB | 4073696 / 4653056 | `lichuang-dev/build.log` |
| lilygo-t-cameraplus-s3 | lilygo-t-cameraplus-s3 | 16MB | 4041216 / 4653056 | `lilygo-t-cameraplus-s3/build.log` |
| lilygo-t-cameraplus-s3 | lilygo-t-cameraplus-s3_v1_2 | 16MB | 4043904 / 4653056 | `lilygo-t-cameraplus-s3_v1_2/build.log` |
| lilygo-t-circle-s3 | lilygo-t-circle-s3 | 16MB | 3981968 / 4653056 | `lilygo-t-circle-s3/build.log` |
| lilygo-t-display-s3-pro-mvsrlora | lilygo-t-display-s3-pro-mvsrlora | 16MB | 3986112 / 4653056 | `lilygo-t-display-s3-pro-mvsrlora/build.log` |
| m5stack-core-s3 | m5stack-core-s3 | 16MB | 4055200 / 4653056 | `m5stack-core-s3/build.log` |
| m5stack-core-s3-se | m5stack-core-s3-se | 16MB | 3996576 / 4653056 | `m5stack-core-s3-se/build.log` |
| magiclick-2p4 | magiclick-2p4 | 16MB | 4032752 / 4653056 | `magiclick-2p4/build.log` |
| magiclick-2p5 | magiclick-2p5 | 16MB | 4125312 / 4653056 | `magiclick-2p5/build.log` |
| minsi-k08-dual | minsi-k08-dual | 16MB | 4069872 / 4653056 | `minsi-k08-dual/build.log` |
| mixgo-nova | mixgo-nova | 8MB | 4016992 / 4980736 | `mixgo-nova/build.log` |
| movecall-cuican-esp32s3 | movecall-cuican-esp32s3 | 8MB | 4020768 / 4980736 | `movecall-cuican-esp32s3/build.log` |
| movecall-moji-esp32s3 | movecall-moji-esp32s3 | 16MB | 4020752 / 4653056 | `movecall-moji-esp32s3/build.log` |
| otto-robot | otto-robot | 16MB | 4085792 / 4653056 | `otto-robot/build.log` |
| sensecap-watcher | sensecap-watcher | 32MB | 4138720 / 4194304 | `sensecap-watcher/build2.log` |
| sp-esp32-s3-1.28-box | sp-esp32-s3-1.28-box | 16MB | 4040384 / 4653056 | `sp-esp32-s3-1.28-box/build.log` |
| sp-esp32-s3-1.54-muma | sp-esp32-s3-1.54-muma | 16MB | 4038944 / 4653056 | `sp-esp32-s3-1.54-muma/build.log` |
| taiji-pi-s3 | taiji-pi-s3 | 16MB | 3977888 / 4653056 | `taiji-pi-s3/build.log` |
| taiji-pi-s3 | taiji-pi-s3-pdm | 16MB | 3980032 / 4653056 | `taiji-pi-s3-pdm/build.log` |
| tudouzi | tudouzi | 16MB | 3964384 / 4653056 | `tudouzi/build.log` |
| waveshare-s3-audio-board | waveshare-s3-audio-board | 16MB | 4092704 / 4653056 | `waveshare-s3-audio-board/build.log` |
| waveshare-s3-epaper-1.54 | waveshare-s3-epaper-1.54 | 8MB | 3988080 / 4980736 | `waveshare-s3-epaper-1.54/build2.log` |
| waveshare-s3-touch-amoled-1.32 | waveshare-s3-touch-amoled-1.32 | 8MB | 3996208 / 4980736 | `waveshare-s3-touch-amoled-1.32/build.log` |
| waveshare-s3-touch-amoled-1.75 | waveshare-s3-touch-amoled-1.75 | 16MB | 4016816 / 4653056 | `waveshare-s3-touch-amoled-1.75/build.log` |
| waveshare-s3-touch-amoled-2.06 | waveshare-s3-touch-amoled-2.06 | 16MB | 4013280 / 4653056 | `waveshare-s3-touch-amoled-2.06/build.log` |
| waveshare-s3-touch-lcd-1.83 | waveshare-s3-touch-lcd-1.83 | 16MB | 4018720 / 4653056 | `waveshare-s3-touch-lcd-1.83/build2.log` |
| waveshare-s3-touch-lcd-3.49 | waveshare-s3-touch-lcd-3.49 | 16MB | 4004112 / 4653056 | `waveshare-s3-touch-lcd-3.49/build.log` |
| waveshare-s3-touch-lcd-3.5b | waveshare-s3-touch-lcd-3.5b | 16MB | 4067008 / 4653056 | `waveshare-s3-touch-lcd-3.5b/build.log` |
| waveshare-s3-touch-lcd-4b | waveshare-s3-touch-lcd-4b | 16MB | 4038464 / 4653056 | `waveshare-s3-touch-lcd-4b/build2.log` |
| xingzhi-cube-0.85tft-ml307 | xingzhi-cube-0.85tft-ml307 | 16MB | 3915824 / 4653056 | `xingzhi-cube-0.85tft-ml307/build.log` |
| xingzhi-cube-0.85tft-wifi | xingzhi-cube-0.85tft-wifi | 16MB | 3980384 / 4653056 | `xingzhi-cube-0.85tft-wifi/build.log` |
| xingzhi-cube-0.96oled-ml307 | xingzhi-cube-0.96oled-ml307 | 16MB | 4073584 / 4653056 | `xingzhi-cube-0.96oled-ml307/build.log` |
| xingzhi-cube-0.96oled-wifi | xingzhi-cube-0.96oled-wifi | 16MB | 3996768 / 4653056 | `xingzhi-cube-0.96oled-wifi/build.log` |
| xingzhi-cube-1.54tft-ml307 | xingzhi-cube-1.54tft-ml307 | 16MB | 4057136 / 4653056 | `xingzhi-cube-1.54tft-ml307/build.log` |
| xingzhi-cube-1.54tft-ml307 | xingzhi-cube-1.54tft-ml307-wechatui | 16MB | 4057200 / 4653056 | `xingzhi-cube-1.54tft-ml307-wechatui/build.log` |
| xingzhi-cube-1.54tft-wifi | xingzhi-cube-1.54tft-wifi | 16MB | 3980064 / 4653056 | `xingzhi-cube-1.54tft-wifi/build.log` |
| xingzhi-metal-1.54-wifi | xingzhi-metal-1.54-wifi | 16MB | 4020768 / 4653056 | `xingzhi-metal-1.54-wifi/build.log` |
| yunliao-s3 | yunliao-s3 | 16MB | 4093088 / 4653056 | `yunliao-s3/build.log` |
| zhengchen-1.54tft-ml307 | zhengchen-1.54tft-ml307 | 16MB | 4053488 / 4653056 | `zhengchen-1.54tft-ml307/build.log` |
| zhengchen-1.54tft-wifi | zhengchen-1.54tft-wifi | 16MB | 3977008 / 4653056 | `zhengchen-1.54tft-wifi/build.log` |
