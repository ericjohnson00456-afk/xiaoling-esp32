# ESP32-S3 语音链路串口冒烟测试

固件必须与实物板型匹配。`/dev/ttyACM0` 上的机器是 ESP-BOX-3，其他 S3
板型的固件只能在对应硬件上验证麦克风、功放、屏幕和网络接口。

使用 ESP-IDF 5.4.4 打开串口监视器：

```sh
idf.py -p /dev/ttyACM0 monitor
```

如设备尚未联网，在 `xiaoling>` 提示符下输入 `wifi connect <ssid> <password>`，
等待 Wi-Fi 连通、小聆 WebSocket 连通并回到待机。凭据由测试者在串口输入，
不要写入仓库或测试日志。

## Opus 上报与真实对话

```text
xiaoling> test wake xiaoling
```

设备进入聆听后对着麦克风说一句话。串口日志中的“新语音会话确认”以及
`上行链路[WS结束] ... ok=1` 证明 Opus 帧已送达 WebSocket；云端返回非空
IAT、下发 TTS URL 并播放完成，才算同一会话的语义闭环通过。没有人声时只能
确认上行传输，不能把空 IAT 算作完整对话成功。

## 独立验证 TTS URL 拉流

```text
xiaoling> test tts_text 你好，这是小聆语音播放测试。
```

这个命令请求新的云端文本 TTS URL 并立即交给现有播放器。检查日志中是否依次
出现“TTS HTTP 响应就绪: status=200”、“首个 PCM 输出”以及
`ESP_AUD_SIMPLE_PLAYER_FINISHED`，并确认设备从播报回到待机。已有仍有效的
MP3 URL 也可通过 `test tts_url <http_url>` 单独测试；过期 URL 返回 200 但
音频体为空，不代表播放成功。

所有 Flash 容量的 `respak` 都只打包简体中文（`zh-CN`）和英文（`en-US`）。
8MB Flash 的 S3 语音布局使用 `partitions/v2/8m.csv`，只有一个应用槽，
不提供双槽应用 OTA 回滚。
