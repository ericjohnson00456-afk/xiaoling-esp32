#include "voice_model.h"

#include "xiaoling_events.h"

#include <utility>

VoiceModel& VoiceModel::GetInstance() {
    static VoiceModel instance;
    return instance;
}

VoiceModel::VoiceModel() {
    auto& events = XiaolingEventBus::GetInstance();
    auto subscribe = [this, &events](XiaolingEventType type) {
        event_subscriptions_.push_back(events.Subscribe(type, [this](const XiaolingEvent& event) {
            auto state = Snapshot();
            switch (event.type) {
                case XIAOLING_CONNECTED:
                    state.connected = true;
                    break;
                case XIAOLING_DISCONNECTED:
                    state.connected = false;
                    state.text_active = false;
                    break;
                case XIAOLING_IAT_START:
                case XIAOLING_IAT_UPDATE:
                    state.text = event.text;
                    state.text_source = VoiceTextSource::kIat;
                    state.text_active = true;
                    ++state.text_revision;
                    break;
                case XIAOLING_IAT_END:
                    state.text = event.text;
                    state.text_source = VoiceTextSource::kIat;
                    state.text_active = false;
                    ++state.text_revision;
                    break;
                case XIAOLING_TTS_TXT_START:
                case XIAOLING_TTS_TXT_UPDATE:
                    state.text = event.text;
                    state.text_source = VoiceTextSource::kTts;
                    state.text_active = true;
                    ++state.text_revision;
                    break;
                case XIAOLING_TTS_TXT_END:
                    state.text = event.text;
                    state.text_source = VoiceTextSource::kTts;
                    state.text_active = false;
                    ++state.text_revision;
                    break;
                default:
                    return;
            }
            Publish(std::move(state));
        }));
    };

    subscribe(XIAOLING_CONNECTED);
    subscribe(XIAOLING_DISCONNECTED);
    subscribe(XIAOLING_IAT_START);
    subscribe(XIAOLING_IAT_UPDATE);
    subscribe(XIAOLING_IAT_END);
    subscribe(XIAOLING_TTS_TXT_START);
    subscribe(XIAOLING_TTS_TXT_UPDATE);
    subscribe(XIAOLING_TTS_TXT_END);
}

void VoiceModel::SetEmotion(std::string emotion) {
    auto state = Snapshot();
    state.emotion = std::move(emotion);
    Publish(std::move(state));
}
