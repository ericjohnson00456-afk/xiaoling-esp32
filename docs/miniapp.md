# 小聆AI小应用

小聆平台通过 MCP 下载并运行 Lua 小应用，也可通过 USB 串口侧载。Lua VM 和绘图场景保存在 RAM；具备源码分区的固件保留最近一次成功安装的源码，退出或重启后可在桌面四连击功能键重新打开。应用可将少量状态保存到专用 NVS 存档。

## 能力与安装

先调用 `ls.built_in.get_device_capabilities`。返回 `schema_version: 2`、固件版本和实际硬件；固件启用小应用运行时时返回 `runtime.lua_sdk`，其中 `version` 表示固件支持的接口版本。屏幕尺寸以 `hardware.display.width_px/height_px` 为准，`shape` 为 `rectangle` 或 `round`。无屏、无灯或无扬声器时不应生成依赖这些硬件的应用。按键 ID、中文标签和退出长按时长见 `hardware.input.buttons`。

使用 `ls.built_in.miniapp_install`，参数必须包含：

| 参数 | 内容 |
| --- | --- |
| `id`、`version`、`name` | 非空 UTF-8 字符串，各最多 127 字节 |
| `url` | HTTP(S) 源码地址，最多 2047 字节 |
| `size` | 原始源码字节数，以 `source_max_bytes` 为上限，默认 65536 |
| `hash` | 源码 MD5，32 位小写十六进制 |

下载完成后先核对长度和 MD5，再在独立候选 VM 中执行顶层代码、`on_start()` 和首次 `on_tick(20)`。有屏设备还须在启动过程中 `screen.present()` 提交首帧。成功才替换旧实例；下载、解析、堆限制或启动回调失败时保留旧应用、界面及存档。相同 ID、版本、长度和哈希重复安装复用当前实例。其他替换释放旧应用资源，旧异步结果不能进入新实例。

`runtime.lua_sdk.retained_source` 表示是否支持保存最近源码。支持时，候选应用通过启动验证后才写入 Flash；写入失败也保留旧实例。四连击只在桌面生效，已有小应用运行或闹钟响铃时不会重开。重启不会自动运行小应用；恢复出厂设置会清除最近源码和应用存档。

运行期间，全部已注册的 MCP 工具继续上报并可被调用，小应用模式下的操作门禁由云端判断。设备保留各工具的参数校验和执行条件。语音会话携带当前应用身份；退出后恢复常规会话模式。

## USB 侧载与导出

固件需启用小应用与串口 Shell。连接设备并退出占用串口的监视器，安装 Python 依赖后使用仓库脚本：

```sh
python3 -m pip install pyserial
python3 scripts/miniapp.py push app.lua
python3 scripts/miniapp.py pull last.lua
```

仅连接一台可识别的 ESP32 时自动选择串口；多台设备或其他串口桥请使用 `--port`，例如 `python3 scripts/miniapp.py --port /dev/ttyACM0 push app.lua`。

`push` 默认使用文件名（不含扩展名）作为应用 ID，也可指定 `--id counter`。ID 允许英文字母、数字、下划线和连字符；设备中的身份为 `local:<ID>`。每次成功侧载都会重新运行，即使源码相同。传输或启动失败不替换旧应用。

`pull` 导出最近一次成功安装的完整源码，包括云端安装的应用；不要求应用正在运行。导出会校验长度和 MD5，校验成功才替换目标文件。尚未安装过应用、恢复出厂后或固件没有源码分区时，导出会报告无可用源码。

标准 8 MB、16 MB、16 MB P4、32 MB 和 32 MB 语音分区布局包含 `miniapp_src`。首次升级到这些布局时需同时烧录分区表；只有应用 OTA 不会更新分区表。自定义分区若缺少该分区，仍可运行 RAM 中的小应用，但不支持保存、导出或四连击恢复。

## 应用生命周期

```lua
local count = 0
local function draw()
    if screen then
        screen.begin(0x101820)
        screen.text("计数: " .. count, 12, 12, 0xffffff)
        screen.present()
    end
end
function on_start() draw() end
function on_tick(dt_ms) end
function on_button_click(id)
    count = count + 1
    draw()
end
function on_exit() end
function get_state()
    return json.encode({count=count})
end
function get_actions()
    return {
        reset={description="清零", parameters={}}
    }
end
function on_action(name, args)
    if name == "reset" then count = 0; draw() end
    return {ok=true, message="已清零"}
end
```

`on_tick(dt_ms)` 必须定义，其他回调可省略；通常每 20 ms 调用。回调串行执行，不应阻塞或长循环。顶层、启动、首次 tick 和后续回调均受执行时间、指令及 256 KB Lua 堆限制。未捕获的运行时错误终止应用并回到系统界面；若该实例读取过存档，则清除该应用存档，避免再次加载故障状态。候选应用启动失败不会清除旧存档。安全标准库包括 base、table、string、math、utf8；不开放文件、OS、动态模块、debug、协程及捕获预算异常的接口。

BOOT 通常为 `function`，没有 BOOT 的设备使用已明确适配的主要按键。点击在释放时投递，持续按住功能键 3 秒退出。应用期间不触发系统双击、三击快捷操作。闹钟优先覆盖界面并冻结回调，响铃期间拒绝操作，结束后继续；不会补投冻结期间的点击。

`ls.built_in.miniapp_describe` 返回身份、`running`、状态字符串和操作声明。`miniapp_action` 接收 `action` 与 `arguments` 对象，先验证声明、类型、范围、枚举及总长度，再调用 `on_action`；不进行字符串与数字等类型转换。`miniapp_exit` 退出并释放应用资源。

## 可用接口

| 模块 | 主要接口及限制 |
| --- | --- |
| `app` | `api_version`、`width`、`height`；按实际画布布局，圆屏需留出边缘安全区 |
| `screen` | `begin`、`rect`、`circle`、`line`、`arc`、`text`、`present`；最多 256 图形、16 文本，每段文本最多 63 字节；可设置顺序、透明度和线宽 |
| `led` | `on("status")`、`off("status")`、`blink("status", on_ms, off_ms)`；仅在灯驱动支持时存在 |
| `buzzer` | `play(hz, ms)`，通过现有扬声器输出 |
| `synth` | `play(notes, options)`、`pause`、`resume`；16 kHz，最多 4 声部、256 音符、60 秒，square/triangle/noise；支持循环 |
| `audio` | `play(url)`、`pause`、`resume`、`stop`、`get_state`；独立单播放器，MP3 和 MPEG-TS AAC HLS；支持直播与自动让位系统语音 |
| `tts` | `speak(text)`、`cancel(id)`，最多 512 字节、1 个待处理请求，通过 `on_tts_result(id,result)` 回报 completed/failed/interrupted |
| `http` | `get`、`request`、`cancel`；GET/POST，最多 4 请求、511 字节 URL、16 个请求头、1 KB 请求头总量、8 KB 请求体、32 KB 响应；通过 `on_http_response(id,response)` 回调 |
| `json` | `encode`、`decode`、`null`；保留 JSON 数组及空值，不支持循环引用 |
| `clock` | `now()` 返回 Unix 时间戳；`localtime()` 按系统默认北京时间（UTC+8）换算，`utc_offset` 单位为秒；未校时时返回不可用 |
| `storage` | `load`、`save`、`clear`；按应用 ID 隔离的扁平 JSON，每应用 1 KB、共 4 槽，默认有效期 7 天、最长 30 天，写入间隔 10 秒 |

详细参数预算以能力查询返回值为准。启动期间的 `tts.speak` 在候选应用成功提交后才发起播报；参数无效或设备忙时通过结果回调报告失败。HLS 不支持加密、字节范围分段或 fMP4；播放错误可从状态查询获取。在线音频用户手动暂停后不会因系统语音结束而自行恢复。

## 板型与验证

使用现有 Display 获取尺寸，公共 Button 从板型已有 `config.h` 的 GPIO 定义注册真实按键。特殊 PMIC/ADC 输入或 LED 驱动没有适配时保持不支持，不影响板型原有功能。非矩形显示只需在板型 `config.json` 加 `"display_shape": "round"`。

运行时默认在启用小聆平台、PSRAM 且使用完整 printf 的配置上启用；`CONFIG_NEWLIB_NANO_FORMAT` 不适用于 Lua 的 64 位整数和浮点格式化。未启用运行时的固件仍提供硬件能力查询。

按板型使用 `python scripts/build.py <板型>` 构建。主机回归见 `tests/miniapp/README.md`；`smoke.lua` 用于 320×240 CoreS3SE 实机验证，其他尺寸需要调整布局。通过云端安装或 USB 侧载传入源码，检查状态、操作、HTTP、TTS、音效、在线音频、存档及退出；同时核对串口日志、实际屏幕和实体按键。开发时使用的临时按键注入和截图入口不能保留在交付固件中。
