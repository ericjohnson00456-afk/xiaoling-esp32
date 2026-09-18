#pragma once

#include "observable_model.h"

#include <string>

struct StatusModelState {
    std::string text;
};

// 设备状态文字 Model，可同时供 Home 和后续其他 Presenter 使用。
class StatusModel final : public ObservableModel<StatusModelState> {
public:
    static StatusModel& GetInstance();
    void SetText(std::string text);

private:
    StatusModel();
};
