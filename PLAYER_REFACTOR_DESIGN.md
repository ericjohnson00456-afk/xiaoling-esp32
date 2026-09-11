# 播放器重构设计

## 1. 目标

项目进行整体重构后，播放器需要统一支持：

- ROMFS/assets 中的内置 MP3；
- HTTP/HTTPS URL 的 MP3 流式播放；
- TTS、音乐、提示音、闹钟等逻辑播放通道；
- 通道优先级、排队、抢占、取消、暂停和恢复；
- 播放完成、抢占、取消、失败、缓冲不足等事件回调；
- 多步骤业务编排，例如“tone -> TTS -> 循环 tone”以及“拍照 -> 预览 -> tone -> TTS”。

上行语音和下行播放分离：

```text
上行：麦克风 -> AEC/VAD -> Opus 编码 -> 上传
下行：URL/ROMFS -> 流读取 -> 解码 -> PCM -> ES8311/I2S/PA
```

## 2. 底层方案

首版使用 Espressif 官方 `esp_audio_simple_player 1.0.0~2`，底层为 ESP-GMF：

```text
esp_audio_simple_player -> gmf_audio + gmf_io + gmf_core
```

它已经提供当前需要的 HTTP/HTTPS 流读取、MP3 解码、RingBuffer 和音频格式转换，
且不要求把整个 ADF 板级音频框架接入项目。ESP-ADF 仍可作为未来复杂管线的备选，
但首版不再直接依赖 ADF。

ESP-GMF/简单播放器负责：

- HTTP/HTTPS stream；
- Flash/file stream；
- MP3 等格式解码；
- RingBuffer；
- pipeline 的启动、停止、暂停和恢复；
- pipeline 内部事件。

项目播放器层仍负责：

- 播放请求和逻辑通道；
- 优先级和抢占策略；
- 播放会话生命周期；
- 工作流和 MCP 编排。

## 3. 目录结构

新的播放器代码统一放在 `main/audio/player/`：

```text
main/audio/
├── audio_codec.*              # 音频硬件抽象
├── audio_service.*            # 现有采集、AEC/VAD、唤醒词、Opus 上行
├── processors/                # 音频前端处理
├── wake_words/                # 唤醒词
└── player/                    # 新的下行播放系统
    ├── audio_player.*
    ├── core/
    │   ├── playback_manager.*
    │   ├── playback_session.*
    │   ├── playback_request.h
    │   ├── playback_event.h
    │   └── playback_policy.h
    ├── engine/
    │   ├── player_engine.h
    │   ├── adf_player_engine.*
    │   └── adf_pipeline_factory.*
    ├── sources/
    │   ├── audio_source.h
    │   ├── url_source.*
    │   ├── romfs_source.*
    │   └── file_source.*
    ├── output/
    │   ├── audio_output.h
    │   ├── codec_output.*
    │   └── pa_controller.*
    └── playlist/
        ├── playlist.*
        └── playlist_item.h

main/workflow/
├── workflow_engine.*
├── startup_workflow.*
├── alarm_workflow.*
└── photo_workflow.*
```

业务工作流不应该直接调用 GMF API。业务只调用 `AudioPlayer`/`PlaybackManager`。

## 4. 播放管线

统一管线：

```text
UrlSource / RomfsSource / FileSource
                ↓
            GMF IO
                ↓
           MP3 decoder
                ↓
          PCM RingBuffer
                ↓
        AudioOutputController
                ↓
           ES8311 / I2S / PA
```

不能让 GMF 和现有 `AudioService` 各自独占 ES8311/I2S。物理音频输出必须只有一个所有者。

首版由 GMF 输出回调把标准 PCM 写入 `AudioService` 播放队列，仍由现有
`AudioOutputTask` 唯一调用 Codec/I2S/PA。后续再把它迁移为独立的
`AudioOutputController`。

## 5. 音频源

### URL 音频

使用 GMF HTTP IO 实现边下载边解码，不等待整个 MP3 下载完成。

URL 音频需要关注：

- HTTPS 和鉴权 header；
- HTTP chunked/Content-Length；
- 网络缓冲不足和重连；
- URL 过期；
- HTTP Range 是否支持。

### ROMFS/assets 音频

优先使用项目已有的 `espressif/esp_mmap_assets`，该依赖已存在于 `main/idf_component.yml`。

它可以获取资源的地址和 size：

```text
mmap_assets_get_mem()
mmap_assets_get_size()
```

播放器后续通过 GMF `embed` IO 或自定义 Source 的 read callback 读取 MP3 数据。

如果使用 memory mapping 模式，可以直接读取映射地址；否则使用 `mmap_assets_copy_mem()` 或分区读取接口。资源句柄和映射必须保持有效，直到播放结束。

当前项目的 `Assets::GetAssetData()` 也能返回资源指针和 size。播放器应通过 `RomfsSource` 抽象访问，不直接依赖具体打包格式。

固定内置音效适合 assets/mmap；动态下载和管理的音乐适合 FATFS/LittleFS。

## 6. 播放请求和策略

```cpp
struct PlaybackRequest {
    PlaybackSource source;       // URL / ROMFS / file
    std::string channel;         // music / tts / alarm / prompt
    int priority;
    PlaybackPolicy policy;
    bool resumable;
    bool loop;
    PlaybackCallbacks callbacks;
};
```

策略：

```text
音乐：低优先级，可暂停恢复
TTS：中优先级，被抢占后通常丢弃
提示音：短音效，结束后恢复原播放
闹钟：高优先级，可抢占其他播放
```

典型结束原因：

```text
natural_end
preempted
cancelled
failed
underrun
timeout
```

音乐续播需要保存媒体时间和源偏移。ROMFS 文件可直接 seek；HTTP URL 只有在服务端支持 Range 时才能可靠地硬恢复。不支持 Range 的网络音频只保证短暂软暂停，TTS 通常不续播。

## 7. 播放会话状态

```text
created
  -> queued
  -> preparing
  -> playing
  -> paused
  -> resuming
  -> completed / preempted / cancelled / failed
```

每个会话必须有唯一 `session_id`。所有播放事件都携带 `session_id`，避免多个通道的完成事件互相误触发。

## 8. 模块通信

项目已经使用 ESP-IDF `esp_event` 处理 WiFi/IP 事件，但当前还没有统一的业务事件总线。

不增加第三方 EBus，使用：

```text
ESP-IDF esp_event
        ↓
DomainEventBus 封装
        ↓
PlaybackManager / WorkflowEngine / Network / Upload
```

建议定义项目事件：

```text
APP_EVENT_NETWORK_CONNECTED
APP_EVENT_NETWORK_DISCONNECTED
APP_EVENT_PLAYBACK_STARTED
APP_EVENT_PLAYBACK_COMPLETED
APP_EVENT_PLAYBACK_FAILED
APP_EVENT_UPLOAD_STARTED
APP_EVENT_UPLOAD_STOPPED
```

事件只传状态和少量元数据，不传 PCM、MP3 或 Opus 数据。大数据仍使用 GMF RingBuffer 或专用队列。

GMF 状态回调只在引擎适配层内部使用，由 `GmfPlayerEngine` 转换成项目自己的 `PlaybackEvent`。

## 9. 工作流示例

### WiFi 成功后播放 tone，完成后开始上传

```text
NetworkConnected
    ↓
StartupWorkflow 提交 tone
    ↓
PlaybackCompleted(session_id = tone)
    ↓
VoiceSessionController.Start()
    ↓
Opus 编码并上传
```

播放器只报告播放完成，不直接启动上传。下一步由工作流决定。

### 闹钟

```text
Play(tone)
    ↓ completed
Play(tts)
    ↓ completed
Loop(tone)
    ↓ user confirm
Stop()
```

### 拍照播报

```text
CapturePhoto()
    ↓
Preview(3s)
    ↓
Play(tone)
    ↓
Play(tts)
```

## 10. 现有 `main/audio` 的迁移边界

保留：

```text
AudioCodec 和各类 Codec 驱动
麦克风采集
AEC/VAD/降噪
唤醒词检测
Opus 上行编码
音频硬件和 PA 控制
```

逐步迁移到 `main/audio/player`：

```text
Opus 下行解码
audio_decode_queue_
audio_playback_queue_
AudioOutputTask
PushPacketToDecodeQueue()
PlaySound()
ResetDecoder()
SetDecodeSampleRate()
```

迁移初期可以保留旧下行链路作为兼容通道，等 GMF 播放管线验证完成后删除。

## 11. 实施顺序

1. 建立 `main/audio/player` 目录和公共接口。
2. 接入 `esp_audio_simple_player`/ESP-GMF，先验证单个 HTTP MP3。
3. 增加 ROMFS/assets MP3 Source。
4. 接入统一 ES8311/I2S 输出。
5. 增加播放事件和 `session_id`。
6. 增加优先级、抢占、取消和恢复。
7. 增加播放列表和自动切歌。
8. 增加 `DomainEventBus` 和工作流。
9. 迁移现有提示音和下行 Opus 播放调用。
10. 删除旧的下行 Opus 播放队列和重复 Codec 输出逻辑。

## 12. 当前默认决策

```text
下行引擎：esp_audio_simple_player 1.0.0~2 / ESP-GMF
下行首版格式：MP3
上行格式：Opus
内置音效：esp_mmap_assets/现有 Assets 适配
URL 播放：HTTP/HTTPS 流式播放
物理输出：单 Codec、单 I2S、单 PA 所有者
事件总线：ESP-IDF esp_event + 项目 DomainEventBus
工作流：独立于播放器核心
```

## 13. 首版落地状态

当前已经完成：

```text
小聆 WebSocket 下发 data.sub=tts
        -> Base64 解码 data.content 得到 TTS URL
        -> AudioPlayer::PlayTtsUrl()
        -> GmfPlayerEngine HTTP/HTTPS 流读取和 MP3 解码
        -> 转换为 16 kHz / 单声道 / 16-bit PCM
        -> AudioService 播放队列
        -> AudioOutputTask
        -> ES8311 / I2S / PA
```

首版已包含 TTS session ID、播放完成/停止/失败事件，以及唤醒时丢弃当前 TTS。
音乐恢复、通用优先级仲裁、ROMFS MP3、播放列表和工作流编排按前述步骤继续实现。
