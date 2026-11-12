# DF-K10 QEMU 云端验证

客户固件目录为项目根目录的 `build-df-k10`；仿真固件使用
`build-df-k10-qemu`。不要把仿真固件发给客户烧录。

原固件实测在 QEMU 中停在 ILI9341 的 SPI 发送等待，尚未调用
`StartNetwork()`。QEMU 不提供这块板的屏幕、音频、摄像头和 Wi-Fi
设备，因此不能用原镜像直接完成联网验收。

显式传入 `-DXIAOLING_DF_K10_QEMU=ON` 时，仅替换 DF-K10 的 Board
实现：无屏幕、无录音输入、丢弃音频输出，并用 OpenCores Ethernet
提供真实 TCP/IP。Application、OTA、设备激活、鉴权和 WebSocket
仍使用项目原有实现及配置中的真实云端；不模拟云端响应、不跳过绑定。
测试设备采用独立的合成身份，不复用实板身份。

在 ESP-IDF 5.4.4 环境中，从项目根目录执行：

```sh
python main/boards/df-k10/qemu/verify.py
```

脚本会创建独立 sdkconfig、编译仿真固件、合并 Flash，并启动 QEMU。
可用 `--qemu <path>` 指定模拟器，`--skip-build` 复测已编译的镜像，
`--timeout 900` 留出新设备绑定时间。需要调试时用 `--gdb-port <port>`。
`--reuse-flash` 在应用 SHA256 相同的前提下保留模拟 NVS 和设备身份。
本次验证使用官方 `esp_develop_9.2.2_20261109`，此前 SDK 配套的旧版
QEMU 在本机运行 S3 镜像时曾发生宿主进程崩溃。

测试用 eFuse 文件只在 QEMU 中使用。它声明 ADC 校准版本 1，避免
SDK 进入 QEMU 无法完成的 ADC 自校准；不修改真实芯片或固件指令。
QEMU stdin 保持为打开的管道，避免 `/dev/null` 的 EOF 导致串口输出丢失。

结果在 `tmp/qemu-validation/df-k10-qemu/`：

| 文件 | 内容 |
| --- | --- |
| `result.json` | 测试范围、模拟器/固件 SHA256、各阶段结果、失败原因 |
| `evidence.log` | 不含鉴权响应正文的阶段证据 |
| `uart-private.log` | 完整日志，可能含 Token，权限 0600，不应对外分享 |
| `command.json` | 本次 QEMU 启动参数 |

默认 `--criterion cloud` 的 PASS 必须同时满足：获得 IPv4、取得非空 Token、WebSocket 传输连接、
收到真实云端 `connected` 消息产生的 `XiaolingConnected` 事件，并继续
观察 20 秒，期间无断线、panic 或重启。未绑定的新设备会保留失败结果
`cloud_requires_device_binding`，不能据此宣称云端测试通过。

用户本轮接受“能取得绑定码即可”。使用 `--criterion activation` 时，
PASS 仅要求取得 IPv4，并经原有激活接口收到真实绑定码；结果中的
`criterion` 和 `reason` 会明确标注这一标准，不代表 WebSocket 已连接。

即使仿真 PASS，也只覆盖公共应用到云端的链路，不代表客户固件的
真实 Wi-Fi、音频、屏幕、摄像头已经验收。
