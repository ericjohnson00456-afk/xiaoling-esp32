#include "playback_intent_stack.h"

#include <algorithm>
#include <utility>

namespace {

size_t HandlerIndex(PlaybackIntent intent) {
    return static_cast<size_t>(intent);
}

}  // namespace

void PlaybackIntentStack::SetHandlers(PlaybackIntent intent, Handlers handlers) {
    const size_t index = HandlerIndex(intent);
    if (index < handlers_.size()) {
        handlers_[index] = std::move(handlers);
    }
}

bool PlaybackIntentStack::Push(PlaybackIntent intent) {
    if (intent <= PlaybackIntent::kNone || intent >= PlaybackIntent::kCount) {
        return false;
    }
    if (Contains(intent) || size_ >= intents_.size()) {
        return Contains(intent);
    }

    const PlaybackIntent old_top = Top();
    size_t insert_at = size_;
    while (insert_at > 0 &&
           static_cast<uint8_t>(intents_[insert_at - 1]) >
               static_cast<uint8_t>(intent)) {
        intents_[insert_at] = intents_[insert_at - 1];
        --insert_at;
    }
    intents_[insert_at] = intent;
    ++size_;

    const PlaybackIntent new_top = Top();
    if (new_top != old_top) {
        if (old_top != PlaybackIntent::kNone &&
            handlers_[HandlerIndex(old_top)].on_preempted) {
            handlers_[HandlerIndex(old_top)].on_preempted();
        }
        if (handlers_[HandlerIndex(new_top)].on_enter) {
            handlers_[HandlerIndex(new_top)].on_enter();
        }
        entered_[HandlerIndex(new_top)] = true;
    }
    return true;
}

bool PlaybackIntentStack::Reenter(PlaybackIntent intent) {
    if (intent <= PlaybackIntent::kNone || intent >= PlaybackIntent::kCount) {
        return false;
    }
    if (!Contains(intent)) {
        return Push(intent);
    }

    const size_t index = HandlerIndex(intent);
    entered_[index] = false;
    if (Top() == intent) {
        if (handlers_[index].on_enter) {
            handlers_[index].on_enter();
        }
        entered_[index] = true;
    }
    return true;
}

bool PlaybackIntentStack::Pop(PlaybackIntent intent) {
    if (intent <= PlaybackIntent::kNone || intent >= PlaybackIntent::kCount) {
        return false;
    }
    const auto it = std::find(intents_.begin(), intents_.begin() + size_, intent);
    if (it == intents_.begin() + size_) {
        return false;
    }

    const bool top_changed = (it == intents_.begin() + size_ - 1);
    if (handlers_[HandlerIndex(intent)].on_exit) {
        handlers_[HandlerIndex(intent)].on_exit();
    }
    entered_[HandlerIndex(intent)] = false;
    std::move(it + 1, intents_.begin() + size_, it);
    --size_;

    if (top_changed && size_ > 0) {
        const PlaybackIntent resumed = Top();
        const size_t resumed_index = HandlerIndex(resumed);
        if (!entered_[resumed_index]) {
            if (handlers_[resumed_index].on_enter) {
                handlers_[resumed_index].on_enter();
            }
            entered_[resumed_index] = true;
        } else if (handlers_[resumed_index].on_resumed) {
            handlers_[resumed_index].on_resumed();
        }
    }
    return true;
}

bool PlaybackIntentStack::Contains(PlaybackIntent intent) const {
    if (intent <= PlaybackIntent::kNone || intent >= PlaybackIntent::kCount) {
        return false;
    }
    return std::find(intents_.begin(), intents_.begin() + size_, intent) !=
           intents_.begin() + size_;
}

PlaybackIntent PlaybackIntentStack::Top() const {
    return size_ == 0 ? PlaybackIntent::kNone : intents_[size_ - 1];
}

void PlaybackIntentStack::Clear() {
    size_ = 0;
    entered_.fill(false);
}
