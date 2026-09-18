#include "qrcode_view.h"

#include "display.h"
#include "ui/tools/image/lvgl_image.h"

QrCodeView::QrCodeView(Display* display) : display_(display) {
}

void QrCodeView::Show() {
}

void QrCodeView::Hide() {
#ifdef CONFIG_LSPLATFORM
    if (display_ != nullptr) {
        display_->DismissActivation();
    }
#endif
}

void QrCodeView::ShowQrCode(std::unique_ptr<LvglImage> image, const std::string& message) {
#ifdef CONFIG_LSPLATFORM
    if (display_ != nullptr && image != nullptr) {
        display_->ShowActivation(std::move(image), message);
    }
#else
    (void)image;
    (void)message;
#endif
}
