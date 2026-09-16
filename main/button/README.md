# 按键与唤醒

`button.h/.cc` 封装 GPIO、ADC 按键驱动与手势回调。板级代码负责引脚和硬件配置；
`button_events.h/.cc` 绑定唤醒按键，并通过默认 ESP 事件循环发布 `BUTTON_EVENT`。
BOX3 单击发布 `kClick`；启用设备端 AEC 时，双击发布 `kToggleAec`。
`boards/common/button.h` 保留为旧板级 include 路径的转发头。

Application 将按键事件转交主任务，检查当前设备状态并发起唤醒。
旧板型使用的 `BoardEvents::Input::PrimaryClick/TouchDown/TouchUp` 仍走同一唤醒流程。
配网、音频测试和 AEC 切换保留原有状态限制。

待机时单击的流程：

1. 发布按键事件，检查云端连接。
2. 暂停唤醒检测和语音处理，调用 `player/channels/tone_player.cc`。
3. `TonePlayer` 从 ROMFS 中存在的 `000` 到 `004` 选择应答音，多个候选时排除上一次选择。
4. 播放器完成解码且 PCM 输出队列排空后，完成事件被调度到主任务。
5. 再发送开始聆听请求，启用语音处理和音频上报。

语音唤醒复用步骤 2 到 5。提示音缺失或播放失败时继续聆听；提示音被停止或抢占时取消本次唤醒。
提示音播放期间再次单击，或按住说话的按键松开，会取消等待并返回待机。
断线和离开唤醒状态也会清除等待；旧播放会话的完成事件不会重新开启音频上报。
