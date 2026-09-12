#include "png_animation_player.h"

#include <algorithm>

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
    animation_ = animation;
    if (animation_ == nullptr) {
        return;
    }

    if (!animation_->intro.empty()) {
        StartPhase(PhaseType::kIntro);
    } else if (!animation_->loop.empty()) {
        StartPhase(PhaseType::kLoop);
    } else if (!animation_->outro.empty()) {
        StartPhase(PhaseType::kOutro);
    }
}

void PngAnimationPlayer::Stop() {
    if (timer_ != nullptr) {
        lv_timer_pause(timer_);
    }
    animation_ = nullptr;
    frame_index_ = 0;
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
            Stop();
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
    const auto* current = CurrentPhase();
    if (animation_ == nullptr || current == nullptr || current->empty()) {
        Stop();
        return;
    }

    ++frame_index_;
    if (frame_index_ < current->frames.size()) {
        ShowCurrentFrame();
        const uint32_t delay = current->delays.empty()
                                   ? current->default_interval_ms
                                   : std::max<uint32_t>(1, current->delays[frame_index_]);
        if (timer_ != nullptr) {
            lv_timer_set_period(timer_, delay);
        }
        return;
    }

    if (phase_ == PhaseType::kLoop && current->loop == -1) {
        frame_index_ = 0;
        ShowCurrentFrame();
        if (timer_ != nullptr) {
            lv_timer_set_period(timer_, current->delays.empty()
                                          ? current->default_interval_ms
                                          : std::max<uint32_t>(1, current->delays[0]));
        }
        return;
    }

    if (phase_ == PhaseType::kIntro && animation_->loop.empty()) {
        if (!animation_->outro.empty()) {
            StartPhase(PhaseType::kOutro);
        } else {
            Stop();
        }
    } else if (phase_ != PhaseType::kOutro && !animation_->outro.empty()) {
        StartPhase(PhaseType::kOutro);
    } else {
        Stop();
    }
}
