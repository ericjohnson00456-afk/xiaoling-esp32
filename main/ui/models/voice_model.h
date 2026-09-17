#pragma once

#include "observable_model.h"

#include <cstdint>
#include <string>
#include <vector>

enum class VoiceTextSource {
    kNone,
    kIat,
    kTts,
};

struct VoiceModelState {
    std::string emotion = "neutral";
    std::string text;
    VoiceTextSource text_source = VoiceTextSource::kNone;
    bool text_active = false;
    bool connected = false;
    uint32_t text_revision = 0;
};

class VoiceModel final : public ObservableModel<VoiceModelState> {
public:
    static VoiceModel& GetInstance();

    void SetEmotion(std::string emotion);
    void SetPrompt(std::string text);

private:
    VoiceModel();
    std::vector<uint32_t> event_subscriptions_;
};
