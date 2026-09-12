#pragma once

#include <string>

// UI 主题的基础接口。具体颜色、字体和图片资源由 UI 层的实现提供。
class Theme {
public:
    explicit Theme(const std::string& name) : name_(name) {}
    virtual ~Theme() = default;

    const std::string& name() const { return name_; }

private:
    std::string name_;
};
