#pragma once

#include "emoji_model.h"

#include <lvgl.h>

// 基于 LVGL 定时器播放 EmojiModel 提供的 PNG 帧序列。
class PngAnimationPlayer final {
public:
    explicit PngAnimationPlayer(lv_obj_t* image);
    ~PngAnimationPlayer();

    PngAnimationPlayer(const PngAnimationPlayer&) = delete;
    PngAnimationPlayer& operator=(const PngAnimationPlayer&) = delete;

    void Play(const EmojiAnimation* animation);
    void Stop();

private:
    enum class PhaseType {
        kIntro,
        kLoop,
        kOutro,
    };

    static void TimerCallback(lv_timer_t* timer);
    void Advance();
    void StartPhase(PhaseType phase);
    const EmojiPhase* CurrentPhase() const;
    void ShowCurrentFrame();

    lv_obj_t* image_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    const EmojiAnimation* animation_ = nullptr;
    PhaseType phase_ = PhaseType::kIntro;
    size_t frame_index_ = 0;
};
