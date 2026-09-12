#include "status_model.h"

#include "i18n/lang_config.h"

#include <utility>

StatusModel& StatusModel::GetInstance() {
    static StatusModel instance;
    return instance;
}

StatusModel::StatusModel() {
    Publish(StatusModelState{.text = Lang::Strings::INITIALIZING});
}

void StatusModel::SetText(std::string text) {
    if (Snapshot().text == text) {
        return;
    }
    Publish(StatusModelState{.text = std::move(text)});
}
