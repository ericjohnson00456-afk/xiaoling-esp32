#include "home_view.h"

#include "display.h"

HomeView::HomeView(Display* display) : display_(display) {
}

void HomeView::Show() {
    if (display_ != nullptr) {
        display_->SetHomeVisible(true);
    }
}

void HomeView::Hide() {
    if (display_ != nullptr) {
        display_->SetHomeVisible(false);
    }
}

void HomeView::SetEmotion(const std::string& emotion) {
    if (display_ != nullptr && !emotion.empty()) {
        display_->SetEmotion(emotion.c_str());
    }
}

void HomeView::SetText(const std::string& text) {
    if (display_ != nullptr) {
        display_->SetChatMessage("system", text.c_str());
    }
}

void HomeView::SetWifiIcon(const std::string& icon) {
    if (display_ != nullptr) {
        display_->SetNetworkIcon(icon.c_str());
    }
}

void HomeView::SetStatus(const std::string& status) {
    if (display_ != nullptr) {
        display_->SetStatus(status.c_str());
    }
}

void HomeView::SetTime(const std::string& time) {
    if (display_ != nullptr && !time.empty()) {
        display_->SetClockText(time.c_str());
    }
}
