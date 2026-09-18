#include "png_animation_player.h"

#include <algorithm>
#include <utility>

PngAnimationPlayer::PngAnimationPlayer(lv_obj_t* image) : image_(image) {
    timer_ = lv_timer_create(&PngAnimationPlayer::TimerCallback, 100, this);
    if (timer_ != nullptr) {
        lv_timer_pause(timer_);
    }
}

PngAnimationPlayer::~PngAnimationPlayer() {
    if (timer_ != nullptr) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }
}

void PngAnimationPlayer::Play(const EmojiAnimation* animation) {
    Stop();
    if (animation == nullptr) {
        return;
    }
    animation_ = std::make_unique<EmojiAnimation>(*animation);
    minimum_duration_mode_ = false;

    if (!animation_->intro.empty()) {
        StartPhase(PhaseType::kIntro);
    } else if (!animation_->loop.empty()) {
        StartPhase(PhaseType::kLoop);
    } else if (!animation_->outro.empty()) {
        StartPhase(PhaseType::kOutro);
    }
}

void PngAnimationPlayer::PlayOnce(const EmojiAnimation* animation, uint32_t min_duration_ms,
                                  std::function<void()> on_complete) {
    Stop();
    if (animation == nullptr) {
        if (on_complete) {
            on_complete();
        }
        return;
    }

    animation_ = std::make_unique<EmojiAnimation>(*animation);
    minimum_duration_mode_ = true;
    waiting_for_minimum_ = false;
    min_duration_ms_ = min_duration_ms;
    elapsed_ms_ = 0;
    completion_callback_ = std::move(on_complete);

    if (!animation_->intro.empty()) {
        StartPhase(PhaseType::kIntro);
    } else if (!animation_->loop.empty()) {
        StartPhase(PhaseType::kLoop);
    } else if (!animation_->outro.empty()) {
        StartPhase(PhaseType::kOutro);
    } else {
        CompleteOrHold();
    }
}

void PngAnimationPlayer::Stop() {
    if (timer_ != nullptr) {
        lv_timer_pause(timer_);
    }
    animation_ = nullptr;
    frame_index_ = 0;
    minimum_duration_mode_ = false;
    waiting_for_minimum_ = false;
    min_duration_ms_ = 0;
    elapsed_ms_ = 0;
    completion_callback_ = nullptr;
}

void PngAnimationPlayer::TimerCallback(lv_timer_t* timer) {
    auto* player = static_cast<PngAnimationPlayer*>(lv_timer_get_user_data(timer));
    if (player != nullptr) {
        player->Advance();
    }
}

const EmojiPhase* PngAnimationPlayer::CurrentPhase() const {
    if (animation_ == nullptr) {
        return nullptr;
    }
    switch (phase_) {
        case PhaseType::kIntro:
            return &animation_->intro;
        case PhaseType::kLoop:
            return &animation_->loop;
        case PhaseType::kOutro:
            return &animation_->outro;
    }
    return nullptr;
}

void PngAnimationPlayer::StartPhase(PhaseType phase) {
    phase_ = phase;
    frame_index_ = 0;
    const auto* current = CurrentPhase();
    if (current == nullptr || current->empty()) {
        if (phase == PhaseType::kIntro && animation_ != nullptr && !animation_->loop.empty()) {
            StartPhase(PhaseType::kLoop);
        } else if (phase != PhaseType::kOutro && animation_ != nullptr && !animation_->outro.empty()) {
            StartPhase(PhaseType::kOutro);
        } else {
            CompleteOrHold();
        }
        return;
    }

    ShowCurrentFrame();
    if (timer_ != nullptr) {
        lv_timer_set_period(timer_, current->delays.empty()
                                      ? current->default_interval_ms
                                      : std::max<uint32_t>(1, current->delays[0]));
        lv_timer_resume(timer_);
    }
}

uint32_t PngAnimationPlayer::CurrentFrameDelay() const {
    const auto* current = CurrentPhase();
    if (current == nullptr || current->empty() || frame_index_ >= current->frames.size()) {
        return 0;
    }
    return current->delays.empty()
               ? current->default_interval_ms
               : std::max<uint32_t>(1, current->delays[frame_index_]);
}

void PngAnimationPlayer::ShowCurrentFrame() {
    const auto* current = CurrentPhase();
    if (image_ == nullptr || current == nullptr || frame_index_ >= current->frames.size()) {
        return;
    }
    const auto& frame = current->frames[frame_index_];
    if (frame != nullptr) {
        lv_image_set_src(image_, frame->image_dsc());
    }
}

void PngAnimationPlayer::Advance() {
    if (waiting_for_minimum_) {
        waiting_for_minimum_ = false;
        elapsed_ms_ = min_duration_ms_;
        Finish();
        return;
    }

    const auto* current = CurrentPhase();
    if (animation_ == nullptr || current == nullptr || current->empty()) {
        CompleteOrHold();
        return;
    }

    elapsed_ms_ = std::min<uint32_t>(UINT32_MAX, elapsed_ms_ + CurrentFrameDelay());
    ++frame_index_;
    if (frame_index_ < current->frames.size()) {
        ShowCurrentFrame();
        const uint32_t delay = CurrentFrameDelay();
        if (timer_ != nullptr) {
            lv_timer_set_period(timer_, delay);
        }
        return;
    }

    if (phase_ == PhaseType::kIntro) {
        // Intro is followed by the loop when one exists.  The old logic
        // skipped the loop and went directly to the outro.
        if (!animation_->loop.empty()) {
            StartPhase(PhaseType::kLoop);
        } else if (!animation_->outro.empty()) {
            StartPhase(PhaseType::kOutro);
        } else {
            CompleteOrHold();
        }
        return;
    }

    if (phase_ == PhaseType::kLoop && current->loop == -1) {
        if (minimum_duration_mode_ && elapsed_ms_ >= min_duration_ms_) {
            if (!animation_->outro.empty()) {
                StartPhase(PhaseType::kOutro);
            } else {
                CompleteOrHold();
            }
            return;
        }
        frame_index_ = 0;
        ShowCurrentFrame();
        if (timer_ != nullptr) {
            lv_timer_set_period(timer_, CurrentFrameDelay());
        }
        return;
    }

    if (phase_ != PhaseType::kOutro && !animation_->outro.empty()) {
        StartPhase(PhaseType::kOutro);
    } else {
        CompleteOrHold();
    }
}

void PngAnimationPlayer::CompleteOrHold() {
    if (!minimum_duration_mode_ || elapsed_ms_ >= min_duration_ms_) {
        Finish();
        return;
    }

    waiting_for_minimum_ = true;
    if (timer_ != nullptr) {
        lv_timer_set_period(timer_, std::max<uint32_t>(1, min_duration_ms_ - elapsed_ms_));
        lv_timer_resume(timer_);
    }
}

void PngAnimationPlayer::Finish() {
    if (timer_ != nullptr) {
        lv_timer_pause(timer_);
    }
    animation_ = nullptr;
    frame_index_ = 0;
    minimum_duration_mode_ = false;
    waiting_for_minimum_ = false;
    min_duration_ms_ = 0;
    elapsed_ms_ = 0;

    auto callback = std::move(completion_callback_);
    completion_callback_ = nullptr;
    if (callback) {
        callback();
    }
}
