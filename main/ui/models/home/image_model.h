#pragma once

#include "observable_model.h"

#include <cstdint>
#include <memory>

class LvglImage;

struct ImageModelState {
    std::shared_ptr<LvglImage> image;
    uint32_t revision = 0;
};

// Home 页当前网络图片。模型持有图片生命周期，不操作显示控件。
class ImageModel final : public ObservableModel<ImageModelState> {
public:
    static ImageModel& GetInstance();

    void SetImage(std::unique_ptr<LvglImage> image);
    void Clear();

private:
    ImageModel() = default;
};
