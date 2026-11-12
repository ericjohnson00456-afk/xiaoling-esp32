# 板型与资源构建边界

版型目录只描述真实的硬件差异：驱动、组件依赖、源文件和确实只属于某个硬件的资源预处理。资源包的容量策略由公共构建流程读取最终的 `sdkconfig` 后统一决定，因此新增版型不需要复制一份“紧凑资源”脚本。

| 配置入口 | 用途 |
| --- | --- |
| `common/sdkconfig.defaults` 及目标芯片后缀文件 | 同系列硬件共享的分区和基础资源布局 |
| `<board>/sdkconfig.defaults` | 硬件默认值，例如 Flash、PSRAM 模式和外设开关 |
| `<board>/config*.json` | 发布变体设置，通过发布脚本写入最终构建配置 |
| `<board>/board.cmake` | 版型组件依赖、源文件设置，以及确实特殊的资源输入 |
| `common/resource_options.cmake` | 根据最终 Flash/PSRAM 配置选择资源包，并接收少量版型特殊输入 |
| `common/qemu/matrix.cmake` | 仅矩阵测试启用的调试信息与构建并发设置 |

公共资源策略目前按有效硬件预算选择 split respak 字体：没有 PSRAM 时使用
`font_puhui_14_1`，有 PSRAM 且 Flash 为 4MB 或 8MB 时使用
`font_puhui_20_4`，其他配置保留完整资源。这个判断发生在 `main/CMakeLists.txt`
调用 `build_split_resources()` 时，与具体版型名称无关。PSRAM 的 Quad/Octal 模式仍由最终
`sdkconfig` 负责硬件初始化；ESP-IDF 的通用配置没有可靠的实装容量字段，当前资源档位
只把 PSRAM 是否可用和 Flash 容量作为打包依据。

ESP-HI 的 4MB 表情缩帧属于内容预处理，不是通用容量规则，所以仍由
`main/boards/esp-hi/prepare_emoji.py` 生成专用资源目录，再交给公共打包器。
公共脚本未指定专用目录时会打包原始表情资源。

分区 CSV 保持在项目既有的 `partitions/v2` 目录；布局由芯片系列默认值和最终
`sdkconfig` 共同决定。版型文件不应再通过 include 选择字体或资源容量档位。

## 公共修复单独维护

以下问题属于公共模块本身，修复保留在对应模块，建议与版型配置分开提交：

- `components/image_codec`：依赖展开阶段正确识别目标芯片及声明 JPEG 驱动依赖。
- `main/audio/opus_encoder`：无 PSRAM 时推迟工作区分配，保留启动和 TLS 所需内存。
- `main/led`：兼容缺少 fade-stop API 的 ESP32。
- `main/ui`：无 LVGL 显示时仍启动 UI 命令处理。
- 语言与字体脚本：隔离构建生成物，修正语言回退和字体注释解析。

`scripts/qemu_matrix.py`、`scripts/collect_qemu_matrix.py` 是项目级构建及审计工具，
不参与正常固件运行；模拟器适配实现位于本目录的 `qemu/` 中。
