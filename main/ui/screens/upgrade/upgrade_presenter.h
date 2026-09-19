#pragma once

#include "ui/nav_stack.h"

#include <cstddef>
#include <memory>
#include <string>

class UpgradeView;

class UpgradePresenter {
public:
    static UpgradePresenter& GetInstance();
    static const UiNavScreen& Screen();

    void SetProgress(std::string target, int progress, size_t speed);

private:
    static int Open(const UiNavScreen*, void** data);
    static int Show(const UiNavScreen*, void* data);
    static int Pause(const UiNavScreen*, void* data);
    static int Resume(const UiNavScreen*, void* data);
    static int Close(const UiNavScreen*, void* data);

    std::unique_ptr<UpgradeView> view_;
    std::string target_;
    int progress_ = 0;
    size_t speed_ = 0;
};
