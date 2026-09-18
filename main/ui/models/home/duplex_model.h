#pragma once

#include "observable_model.h"
#include "xiaoling/session/session_core.h"

struct DuplexModelState {
    DuplexMode mode = DuplexMode::kHalfDuplex;
};

class DuplexModel final : public ObservableModel<DuplexModelState> {
public:
    static DuplexModel& GetInstance();

    void SetMode(DuplexMode mode);

private:
    DuplexModel() = default;
};
