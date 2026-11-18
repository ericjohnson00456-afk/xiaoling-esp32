# 小聆AI小应用

小聆平台通过 MCP 下载并运行临时 Lua 小应用。应用源码、Lua VM 和绘图场景保存在 RAM，重启后不恢复；应用可将少量状态保存到专用 NVS 存档。

## 能力与安装

先调用 `ls.built_in.get_device_capabilities`。返回 `schema_version: 2`、固件版本和实际硬件；仅当运行时就绪时返回 `runtime.lua_sdk`，其中 `version` 表示固件支持的接口版本。屏幕尺寸以 `hardware.display.width_px/height_px` 为准，`shape` 为 `rectangle` 或 `round`。无屏、无灯或无扬声器时不应生成依赖这些硬件的应用。按键 ID、中文标签和退出长按时长见 `hardware.input.buttons`。

使用 `ls.built_in.miniapp_install`，参数必须包含：

| 参数 | 内容 |
| --- | --- |
| `id`、`version`、`name` | 非空 UTF-8 字符串，各最多 127 字节 |
| `url` | HTTP(S) 源码地址，最多 2047 字节 |
| `size` | 原始源码字节数，1～65536 |
| `hash` | 源码 MD5，32 位小写十六进制 |

下载完成后先核对长度和 MD5，再在独立候选 VM 中执行顶层代码、`on_start()` 和首次 `on_tick(20)`。有屏设备还须在启动过程中 `screen.present()` 提交首帧。成功才替换旧实例；下载、解析、堆限制或启动回调失败时保留旧应用、界面及存档。相同 ID、版本、长度和哈希重复安装复用当前实例。其他替换释放旧应用资源，旧异步结果不能进入新实例。

运行期间，云端只开放能力查询及四个小应用工具，避免其他设备工具改变应用界面。语音会话携带当前应用身份；退出后恢复常规工具和会话模式。

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

`on_tick(dt_ms)` 必须定义，其他回调可省略；通常每 20 ms 调用。回调串行执行，不应阻塞或长循环。顶层、启动、首次 tick 和后续回调均受执行时间、指令及 256 KB Lua 堆限制。未捕获的运行时错误终止应用并回到系统界面。安全标准库包括 base、table、string、math、utf8；不开放文件、OS、动态模块、debug、协程及捕获预算异常的接口。

BOOT 通常为 `function`，没有 BOOT 的设备使用已明确适配的主要按键。点击在释放时投递，持续按住功能键 3 秒退出。应用期间不触发系统双击、三击快捷操作。闹钟优先覆盖界面并冻结回调，响铃期间拒绝操作，结束后继续；不会补投冻结期间的点击。

`ls.built_in.miniapp_describe` 返回身份、`running`、状态字符串和操作声明。`miniapp_action` 接收 `action` 与 `arguments` 对象，先验证声明、类型、范围、枚举及总长度，再调用 `on_action`；不进行字符串与数字等类型转换。`miniapp_exit` 退出并释放应用资源。

## 可用接口

| 模块 | 主要接口及限制 |
| --- | --- |
| `app` | `api_version`、`width`、`height`；按实际画布布局，圆屏需留出边缘安全区 |
| `screen` | `begin`、`rect`、`circle`、`line`、`arc`、`text`、`present`；最多 256 图形、16 文本，每段文本最多 63 字节；可设置顺序、透明度和线宽 |
| `led` | `on`、`off`、`blink(on_ms, off_ms)`；仅在灯驱动支持时存在 |
| `buzzer` | `play(hz, ms)`，通过现有扬声器输出 |
| `synth` | `play(notes, options)`、`pause`、`resume`；16 kHz，最多 4 声部、256 音符、60 秒，square/triangle/noise；支持循环 |
| `audio` | `play(url)`、`pause`、`resume`、`stop`、`get_state`；独立单播放器，MP3 和 MPEG-TS AAC HLS；支持直播与自动让位系统语音 |
| `tts` | `speak(text)`、`cancel(id)`，最多 512 字节、1 个待处理请求，通过 `on_tts_result(id,result)` 回报 completed/failed/interrupted |
| `http` | `get`、`request`、`cancel`；GET/POST，最多 4 请求、511 字节 URL、16 个请求头、1 KB 请求头总量、8 KB 请求体、32 KB 响应；通过 `on_http_response(id,response)` 回调 |
| `json` | `encode`、`decode`、`null`；保留 JSON 数组及空值，不支持循环引用 |
| `clock` | `now()`、`localtime()`；未校时时返回不可用，按系统时区换算 |
| `storage` | `load`、`save`、`clear`；按应用 ID 隔离的扁平 JSON，每应用 1 KB、共 4 槽，默认有效期 7 天、最长 30 天，写入间隔 10 秒 |

详细参数预算以能力查询返回值为准。`tts.speak` 必须在应用已提交运行后调用。HLS 不支持加密、字节范围分段或 fMP4；播放错误可从状态查询获取。在线音频用户手动暂停后不会因系统语音结束而自行恢复。

## 板型与验证

使用现有 Display 获取尺寸，公共 Button 从板型已有 `config.h` 的 GPIO 定义注册真实按键。特殊 PMIC/ADC 输入或 LED 驱动没有适配时保持不支持，不影响板型原有功能。非矩形显示只需在板型 `config.json` 加 `"display_shape": "round"`。

运行时默认在启用小聆平台、PSRAM 且使用完整 printf 的配置上启用；`CONFIG_NEWLIB_NANO_FORMAT` 不适用于 Lua 的 64 位整数和浮点格式化。未启用运行时的固件仍提供硬件能力查询。

按板型使用 `python scripts/release.py <板型>` 构建。主机回归见 `tests/miniapp/README.md`；`smoke.lua` 用于 320×240 CoreS3SE 实机验证，其他尺寸需要调整布局。通过云端正常安装路径传入源码，检查状态、操作、HTTP、TTS、音效、在线音频、存档及退出；同时核对串口日志、实际屏幕和实体按键。开发时使用的临时串口上传、按键注入和截图入口不能保留在交付固件中。
