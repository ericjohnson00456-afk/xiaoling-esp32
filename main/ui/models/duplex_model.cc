#include "duplex_model.h"

DuplexModel& DuplexModel::GetInstance() {
    static DuplexModel instance;
    return instance;
}

void DuplexModel::SetMode(DuplexMode mode) {
    if (Snapshot().mode == mode) {
        return;
    }
    Publish(DuplexModelState{.mode = mode});
}
