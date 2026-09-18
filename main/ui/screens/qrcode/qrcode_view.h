#pragma once

#include <memory>
#include <string>

class Display;
class LvglImage;

class QrCodeView {
public:
    explicit QrCodeView(Display* display);

    void Show();
    void Hide();
    void ShowQrCode(std::unique_ptr<LvglImage> image, const std::string& message);

private:
    Display* display_ = nullptr;
};
