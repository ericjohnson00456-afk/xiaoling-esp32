#pragma once

#include <string>

class Display;
enum class DuplexMode;

// Home 页视图：只负责把 Presenter 给出的状态显示到现有显示控件。
class HomeView {
public:
    explicit HomeView(Display* display);

    void Show();
    void Hide();
    void SetEmotion(const std::string& emotion);
    void SetText(const std::string& text);
    void SetWifiIcon(const std::string& icon);
    void SetStatus(const std::string& status);
    void SetTime(const std::string& time);
    void SetDuplexMode(DuplexMode mode);

private:
    Display* display_ = nullptr;
};
