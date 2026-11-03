# 小聆 AI ESP32 固件

小聆 AI 是面向桌面机器人、陪伴设备和语音终端的智能语音交互方案。本仓库提供运行在 ESP32 系列芯片上的设备端固件：设备完成配网和激活后，通过小聆平台建立长连接，支持语音唤醒、录音上传、流式语音播放和多种设备能力扩展。

项目当前版本为 `2.0.0`，基于 [ESP-IDF](https://github.com/espressif/esp-idf) 构建，依赖在 `main/idf_component.yml` 中声明，由 Component Manager 安装并生成本地 `dependencies.lock`。

## 了解小聆

- [小聆 AI 演示视频（B 站）](https://www.bilibili.com/video/BV18QtJz4ET5/)
- [如何接入小聆 AI](https://docs2.listenai.com/x/S_TEd8h7C)
- [ESP32 接入小聆 AI 教程](https://zhuanlan.zhihu.com/p/1936115549143823284)

## 能力概览

- **语音交互**：本地唤醒词、VAD/AEC、语音会话、打断和连续对话。
- **小聆平台连接**：设备激活、鉴权、WebSocket 实时音频链路，以及网络诊断。
- **配网**：BLE BluFi 蓝牙配网和小聆热点配网，由板级配置选择默认方式。
- **多媒体与交互**：TTS、提示音、音乐播放、闹钟、LVGL 图形界面、字幕、表情和状态灯。
- **设备能力**：摄像头拍照/识图、TF 卡音乐同步与播放、MCP 工具、设备设置和系统信息。
- **在线维护**：固件 OTA、唤醒词/表情等资源 OTA，以及量产烧录包和 OTA 元信息生成。
- **硬件适配**：通过 `main/boards/` 支持大量 ESP32、ESP32-C3/C5/C6、ESP32-S3 和 ESP32-P4 开发板，并允许按文档增加自定义板型。

## 快速开始

### 环境要求

- Windows、Linux 或 macOS
- 推荐 ESP-IDF `v5.4.4`；组件清单要求 `>= 5.4.0`，其他版本需要单独验证。使用前执行 `export.bat`（Windows）或 `source export.sh`（Linux/macOS）
- Python、CMake、Ninja 等 ESP-IDF 工具链
- 一块已在 `main/boards/` 中适配的开发板

### 获取代码并安装依赖

```bash
git clone <仓库地址> xiaoling-esp32
cd xiaoling-esp32
```

首次配置时，ESP-IDF Component Manager 会根据 `main/idf_component.yml` 安装组件。工程自动加载 `sdkconfig.defaults`、对应芯片的默认配置和 `sdkconfig.lsplatform`；已有 `sdkconfig` 的设置优先。

### 选择芯片、板型和功能

```bash
idf.py set-target esp32s3
idf.py menuconfig
```

在 `Xiaoling Device` 菜单中配置：

- `Board Type`：选择实际开发板；
- `LSPlatform 选项 → Product ID`：使用内置小聆应用，或填写自建应用的 Product ID/Secret；
- `配网模式`：选择 BLE 配网或热点配网；
- `Flash Assets`、默认语言、唤醒词、摄像头等功能。

板型由 `menuconfig` 中的 `Board Type` 选择，或由下文的发布脚本写入配置。部分板型还需要匹配的板级默认值，例如 CoreS3 的 Quad PSRAM 配置；具体操作见 [SDK 配置说明](docs/xiaoling.md)。

设备 NVS 中保存的 `user/pid` 和 `user/sid` 分别优先于固件内置的 Product ID 和 Product Secret。普通 `idf.py flash` 保留这些配置；切换应用时同时更新设备保存的应用身份。

### 编译、烧录和串口监视

```bash
idf.py build
idf.py -p <串口号> flash monitor
```

退出串口监视使用 `Ctrl+]`。如果切换了芯片或板型，建议先执行：

```bash
idf.py fullclean
idf.py set-target <esp32|esp32c3|esp32c5|esp32c6|esp32s3|esp32p4>
```

## 发布构建

项目提供 `scripts/release.py`，用于按板型读取 `config.json`、追加板级 `sdkconfig`、编译、合并镜像并生成 `releases/v<版本>_<板型>.zip`。

```bash
# 查看所有可发布的板型和变体
python scripts/release.py --list-boards

# 编译指定板型的全部变体
python scripts/release.py <板型>

# 只编译指定变体
python scripts/release.py <板型> --name <变体名称>

# 编译所有已定义发布板型
python scripts/release.py all
```

普通 `idf.py build` 也会在构建目录生成 `merged-binary.bin`、带板型前缀的应用 `.bin`、`.elf` 和 OTA 版本/MD5 信息，并打包以下文件：

- `<板型>-xiaoling_v<版本>.zip`：应用固件、ELF 调试文件、合并固件和 OTA 信息。
- `<板型>-xiaoling_v<版本>_test.zip`：应用固件、合并固件和 OTA 信息。

发布脚本额外生成的 `releases/v<版本>_<板型>.zip` 仅包含 `merged-binary.bin`；同名文件已存在时会跳过编译，需要重新构建时先移走旧包。脚本会重建默认 `build/` 目录并修改根目录的 `sdkconfig`，建议在独立工作区运行或提前备份配置。

## 首次使用设备

1. 烧录对应板型的固件和资源。
2. 上电后按设备提示完成热点或 BLE 配网。
3. 完成小聆平台激活，等待 WebSocket 连接建立。
4. 使用唤醒词开始对话；具体唤醒词、配网提示和设备按键行为由板型配置决定。
5. 如需使用长期记忆和声纹识别，可在微信中搜索 **小聆助手**，进入“添加设备”绑定设备。

## 目录结构

| 路径 | 说明 |
| --- | --- |
| `main/` | 应用、音频、显示、连接、OTA、MCP 和板级实现 |
| `main/boards/` | 各开发板驱动、引脚配置、资源和发布配置 |
| `components/` | 本地组件，如 ROMFS、图片编解码和 Base64 |
| `assets/` | 默认图片等工程资源 |
| `partitions/` | 不同 Flash 容量和资源布局的分区表 |
| `scripts/` | 发布、资源打包、音频/图片转换和调试工具 |
| `docs/` | 协议、架构、唤醒、配网、MCP 和自定义板型文档 |
| `tests/` | 音频、显示、事件和会话相关测试 |

## 开发文档

- [系统架构](docs/architecture.md)
- [SDK 配置说明](docs/xiaoling.md)
- [WebSocket 协议](docs/websocket.md)
- [唤醒与 VAD](docs/wake-vad.md)
- [唤醒录音](docs/wake-recording.md)
- [MCP 使用说明](docs/mcp-usage.md) / [MCP 协议](docs/mcp-protocol.md)
- [MQTT/UDP 调试](docs/mqtt-udp.md)
- [BLE BluFi 配网](docs/blufi.md)
- [自定义开发板](docs/custom-board.md)
- [S3 语音冒烟测试](docs/s3-voice-smoke-test.md)

## 获取帮助

使用前请先确认芯片型号、板型、Flash/PSRAM 配置和分区表匹配。遇到构建或运行问题时，建议附上板型、ESP-IDF 版本、完整编译日志和串口日志，在项目 Issue 或社区中反馈。

## 致谢与许可

本项目 Fork 自 [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)，并在其基础上适配和扩展小聆平台能力。项目遵循仓库中的 [LICENSE](LICENSE) 发布。
