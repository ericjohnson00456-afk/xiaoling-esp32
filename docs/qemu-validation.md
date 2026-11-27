# 全板型 QEMU 激活验证

2026-11-10 最终结果：**109 个板型目录、121 个发布变体全部通过虚拟板级
激活测试及产物审计**，每项均取得真实云端激活码。
原外设驱动镜像达到激活码阶段的项目为 0；其 UART/GDB 记录保留在各自
`original/` 中。该结果不证明原驱动镜像或实板已经联网，也没有验证绑定后的
WebSocket 建连。

逐项结果、镜像及原始证据由测试脚本生成在本地 `tmp/qemu-results/`，
包含 `report.md`、`results.json` 和 `results.csv`，不随源码提交。
客户定制固件及交付记录也保留在本地，避免把应用凭据或单次运行信息带入仓库。

验收条件是固件通过真实网络取得真实云端下发的激活码。设备保持未绑定状态，
不将取得激活码记为 WebSocket 建连成功。覆盖范围由
`scripts/qemu_matrix.py` 扫描 `main/boards/*/config*.json` 生成，包括语言和
同目录下不同 Kconfig 板型选择；目前为 109 个目录、121 个发布变体。

## 测试边界

每个变体先编译正常固件并保存镜像，再运行正常镜像记录 UART/GDB。
QEMU 未实现各板的 LCD、音频芯片、摄像头、Wi-Fi 或蜂窝模块，正常镜像可能
在这些驱动中等待。因此另行编译显式开启 `XIAOLING_QEMU=ON` 的测试镜像：

- 所有原板型源文件仍参与编译、链接，通过链接器包装 `create_board()` 替换板级入口。
- 测试板使用无屏幕、无录音的音频接口和 OpenCores Ethernet；网络通过 lwIP、
  QEMU 用户网络到达真实云端，没有本地激活响应或伪造激活码。
- 应用启动、资源装载、OTA/激活及鉴权保留正常实现。仅在联网成功、取得激活码、
  所需模拟 PSRAM 可用且随后观察两秒未出现 panic/reset 时通过。
- 模拟器关闭 TIMG watchdog，以免宿主机并行编译造成虚拟 CPU 调度延迟。
  固件的 watchdog 配置保留；这不是实时性或 watchdog 验收。
- 使用 `-snapshot`，测试期间的 UUID、NVS 和 OTA 写入不污染保存的初始固件。

**虚拟板级测试通过不等于实板驱动、无线连接、显示激活码、录放音或 WebSocket
已经通过。** 正常镜像和测试镜像分开保存，测试镜像不得作为客户实板固件。

## PSRAM 和模拟器差异

S3 根据生成的 sdkconfig 选择 Quad/Octal 模拟器件，默认测试容量为 8MB；
板内 `qemu.json` 可声明其他容量。Mixgo Nova 按
[厂商规格](https://mixly.cn/fredqian/mixgo_nova) 使用 2MB Quad PSRAM。
Labplus 乐动 V2、掌控 V3 按各自板型说明通过板内 `sdkconfig.defaults`
选择 8MB Quad PSRAM，避免沿用 S3 的 Octal 默认值。
ESP32 使用 4MB 模拟器件。记录启动识别信息和 `app_main` 的 PSRAM heap。
容量是测试输入，不是对真实板上器件的测量。

C5 正常固件保留板型声明的 Quad PSRAM；当前 C5 QEMU 没有 PSRAM 外设，
测试镜像关闭 PSRAM，验证内部 RAM 下能否完成激活。
两块 C5 的板内 `qemu.json` 另行关闭仅供无线接入使用的额外 IRAM 优化和
省电功能，为有线仿真的 TLS 保留内部 DMA heap；生产配置保留这些功能。

P4 QEMU 提供 32MB RAM 窗口，但没有 HEX PSRAM 的器件识别协议。
仅测试镜像包装底层 PSRAM 初始化/容量查询，继续使用 SDK 的映射和 heap，
并执行 64KiB 写入、读取校验。该结果不证明真实 PSRAM 控制器初始化正确。

以 Espressif QEMU `esp_develop_9.2.2_20261109` 为基础，另有两个模拟器补丁：

- `main/boards/common/qemu/qemu-fpu-csr.patch`：P4 浮点 CSR 指令触发异常时
  也设置浮点异常原因，供 SDK 的 lazy FPU handler 启用 FPU。
  保留正常固件的 RVFPLIB；没有替换固件指令或 JSON 解析结果。
- `main/boards/common/qemu/qemu-s3-32mb.patch`：为 S3 machine 增加 32MB
  SPI Flash 型号选择，供 SenseCAP Watcher 的正常 Flash 容量使用。

各次运行记录 QEMU 路径/版本、命令和可用时的二进制 SHA256。
补丁对应的源码基线是
[Espressif QEMU 发布标签](https://github.com/espressif/qemu/releases/tag/esp-develop-9.2.2-20261109)。

准备好官方 QEMU 构建依赖后，可在单独目录构建这两个后端：

```sh
git clone --depth 1 --branch esp-develop-9.2.2-20261109 https://github.com/espressif/qemu.git qemu-esp
cd qemu-esp
git apply /path/to/xiaoling-esp32/main/boards/common/qemu/qemu-fpu-csr.patch
git apply /path/to/xiaoling-esp32/main/boards/common/qemu/qemu-s3-32mb.patch
mkdir build
cd build
../configure --target-list=xtensa-softmmu,riscv32-softmmu --enable-gcrypt --enable-slirp --enable-fdt --disable-docs --disable-sdl --disable-gtk
ninja qemu-system-xtensa qemu-system-riscv32
```

## 复现和证据

当前源码使用 ESP-IDF 6.1.1；历史矩阵结果所用的 SDK 版本见各次验证记录。
本轮 P4 的生成配置为最小硅版本 3.1，模拟 eFuse 也声明 3.1；
矩阵产物不能作为其他硅版本的实板兼容性证明。
不同芯片家族依次构建，以免组件管理器同时修改同一份 `managed_components`。
并行处理不同家族时必须使用各自的源码工作树。

```sh
eim run 'python scripts/qemu_matrix.py --board df-k10 --strip-debug' v6.1.1
eim run 'python scripts/qemu_matrix.py --jobs 2 --resume --strip-debug --qemu-for esp32p4=/path/to/patched/qemu-system-riscv32 --qemu-for esp32s3=/path/to/patched/qemu-system-xtensa' v6.1.1
```

`--strip-debug` 只减小矩阵 ELF 的 DWARF 信息，保留函数符号；普通客户构建
保留原调试信息。该矩阵优化同时限制每个固件默认使用两个编译任务，防止
多个 Ninja 实例各自占满宿主机。指定 `--output` 可以隔离输出。
结果默认位于 `tmp/qemu-matrix/`：

- `inventory.json`、`results.json`、`report.md`：清单和逐变体验收结果。
- `<变体>/sdkconfig.original`、`sdkconfig.fixture`、`fixture-config-overrides.json`：
  正常配置、测试配置和外设差异。
- `<变体>/original/`、`fixture/`：应用、ELF、合并 Flash、bootloader、分区表、
  依赖锁定快照、运行命令、结果及 `evidence.log`。
- 原始 UART 日志以 0600 保存，可能含云端会话信息；对外使用筛选后的证据日志。
- 逐项结果记录源码指纹、SDK、板型选择、镜像大小和 SHA256。

多个工作树的结果通过审计后合并，后列的有效 PASS 优先：

```sh
python scripts/collect_qemu_matrix.py --source tmp/qemu-matrix --source tmp/qemu-matrix-s3 --source tmp/qemu-matrix-p4 --source tmp/qemu-matrix-extra-s3 --source tmp/qemu-matrix-s3b --source tmp/qemu-matrix-s3c
```

汇总在 `tmp/qemu-results/`。收集器核对当前发布预设、生成配置、实际编译板型、
正常/测试镜像的哈希和联网激活证据，拒绝配置不符或被覆盖的旧 PASS。

## 正常固件中需要关注的变化

修复尽量放在板型配置、板型驱动或 `board.cmake` 中。共享修复涉及无 PSRAM
启动时 Opus 编码器的延迟分配、无 LVGL 显示时 UI 命令处理、ESP32 LED fade
兼容、目标相关组件依赖、字体解析/资源打包，以及每个构建目录独立的语言头文件。

编码器额外完成原生逻辑检查：PSRAM 开关两条路径的构造分配时机、首次
320 样本编码/解码、40 字节输出、复用、reset、分块输入及析构均通过。
该检查使用系统 libopus 和 SDK/log 头部桩，记录在
`tmp/qemu-validation/tests/opus/result.json`；不等同于实板录放音验收。

4MB 板型使用独立紧凑分区和功能配置：保留 MP3/Opus/PCM/WAV/OGG 及标准
中文字符集，关闭不使用的音频格式与语音唤醒模型，采用按键唤醒；
只有一个应用槽，不提供双槽应用 OTA。
ESP-HI 使用正常 LCD 激活界面，并降低表情动画帧数以满足容量限制。
这些板型的语音唤醒行为和原配置不同，不能据激活测试宣称所有音频功能已验证。

C3/C6 切换到含四个独立资源分区的布局；P4 使用更大的双 OTA 槽；
SenseCAP Watcher 的 32MB 资源布局保留原工厂/用户 NVS 的地址和容量；
模拟初始镜像不包含实板工厂数据。
**分区变化需要完整镜像迁移，不能仅刷应用 OTA。** 资源 OTA 也必须匹配目标
分区容量，不能给紧凑板型下发超过其分区大小的通用资源包。

DF-K10 单板复现可使用 `--board df-k10`。客户交付记录与固件保留在本地构建目录。
