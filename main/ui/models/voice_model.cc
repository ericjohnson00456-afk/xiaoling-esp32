#include "voice_model.h"

#include "xiaoling/xiaoling_events.h"

#include <utility>

VoiceModel& VoiceModel::GetInstance() {
    static VoiceModel instance;
    return instance;
}

VoiceModel::VoiceModel() {
    auto& events = XiaolingEventBus::GetInstance();
    auto subscribe = [this, &events](XiaolingEventType type) {
        event_subscriptions_.push_back(events.Subscribe(type, [this](const XiaolingEvent& event) {
            switch (event.type) {
                case XIAOLING_CONNECTED:
                case XIAOLING_DISCONNECTED:
                case XIAOLING_IAT_START:
                case XIAOLING_IAT_UPDATE:
                case XIAOLING_IAT_END:
                case XIAOLING_TTS_TXT_START:
                case XIAOLING_TTS_TXT_UPDATE:
                case XIAOLING_TTS_TXT_END:
                    break;
                default:
                    return;
            }
            Update([&](VoiceModelState& state) {
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
                        break;
                }
            });
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
    Update([&](VoiceModelState& state) {
        state.emotion = std::move(emotion);
    });
}

void VoiceModel::SetPrompt(std::string text) {
    Update([&](VoiceModelState& state) {
        state.text = std::move(text);
        state.text_source = VoiceTextSource::kNone;
        state.text_active = false;
        ++state.text_revision;
    });
}
