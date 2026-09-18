#include "home_view.h"

#include "display.h"
#include "ui/tools/image/lvgl_image.h"
#include "xiaoling/session/session_core.h"

#include <utility>

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
#ifdef CONFIG_LSPLATFORM
        display_->DismissImage();
#endif
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

#ifdef CONFIG_LSPLATFORM
void HomeView::ShowImage(std::unique_ptr<LvglImage> image) {
    if (display_ != nullptr && image != nullptr) {
        display_->ShowImage(std::move(image));
    }
}

void HomeView::HideImage() {
    if (display_ != nullptr) {
        display_->DismissImage();
    }
}
#endif

void HomeView::SetDuplexMode(DuplexMode mode) {
    if (display_ != nullptr) {
        display_->SetDuplexMode(mode);
    }
}
