#pragma once

#include "emoji_model.h"

#include <functional>

#include <lvgl.h>

// 基于 LVGL 定时器播放 EmojiModel 提供的 PNG 帧序列。
class PngAnimationPlayer final {
public:
    explicit PngAnimationPlayer(lv_obj_t* image);
    ~PngAnimationPlayer();

    PngAnimationPlayer(const PngAnimationPlayer&) = delete;
    PngAnimationPlayer& operator=(const PngAnimationPlayer&) = delete;

    void Play(const EmojiAnimation* animation);
    void PlayOnce(const EmojiAnimation* animation, uint32_t min_duration_ms,
                 std::function<void()> on_complete = {});
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
    void CompleteOrHold();
    void Finish();
    uint32_t CurrentFrameDelay() const;

    lv_obj_t* image_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    // Own a snapshot of the animation definition.  EmojiModel can be
    // replaced when cloud assets are refreshed; keeping a raw pointer here
    // would leave the LVGL timer reading freed model memory.
    std::unique_ptr<EmojiAnimation> animation_ = nullptr;
    PhaseType phase_ = PhaseType::kIntro;
    size_t frame_index_ = 0;
    bool minimum_duration_mode_ = false;
    bool waiting_for_minimum_ = false;
    uint32_t min_duration_ms_ = 0;
    uint32_t elapsed_ms_ = 0;
    std::function<void()> completion_callback_;
};
