# 按键事件

`button.h/.cc` 封装 GPIO、ADC 按键驱动。板级代码负责引脚和硬件配置，并对需要
发布事件的按键调用 `EnableEventPublishing()`。按键对象只识别手势，通过默认 ESP
事件循环发布 `BUTTON_EVENT`，不包含唤醒、AEC、恢复出厂等业务逻辑。

事件 ID 分为 `kShortPress1`、`kShortPress2`、`kShortPress3`、`kShortPress4`、
`kShortPress5To7`、`kShortPress8OrMore` 和 `kLongPress`。事件数据为
`ButtonEvents::Event`，包含按键对象 `source`；短按事件还包含实际连击次数
`click_count`，长按时该值为 0。

目前 BOX3 的主按键启用事件发布，Application 在 `application_button.cc` 中处理
`BUTTON_EVENT`，并转交主任务执行。单击沿用唤醒/配网逻辑，双击和四击预留，
三击切换双工模式；恰好五击清除已保存的 Wi-Fi 凭据并在当前启动周期进入配网，
六、七击和长按无动作。八击及以上播报提示音后复用 `SystemReset` 恢复出厂
（清除 NVS 和 OTA 元数据并复位）。

`boards/common/button.h` 保留为旧板级 include 路径的转发头。

Application 检查当前设备状态并发起唤醒。
旧板型使用的 `BoardEvents::Input::PrimaryClick/TouchDown/TouchUp` 仍走同一唤醒流程。
旧板型的配网、音频测试和 AEC 输入仍保留原有状态限制。

待机时单击的流程：

1. 发布按键事件，检查云端连接。
2. 暂停唤醒检测和语音处理，调用 `player/channels/tone_player.cc`。
3. `TonePlayer` 从 ROMFS 中存在的 `000` 到 `004` 选择应答音，多个候选时排除上一次选择。
4. 播放器完成解码且 PCM 输出队列排空后，完成事件被调度到主任务。
5. 再发送开始聆听请求，启用语音处理和音频上报。

语音唤醒复用步骤 2 到 5。提示音缺失或播放失败时继续聆听；提示音被停止或抢占时取消本次唤醒。
提示音播放期间再次单击，或按住说话的按键松开，会取消等待并返回待机。
断线和离开唤醒状态也会清除等待；旧播放会话的完成事件不会重新开启音频上报。
