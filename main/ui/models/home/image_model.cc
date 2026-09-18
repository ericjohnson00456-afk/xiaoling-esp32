#include "image_model.h"

#include "ui/tools/image/lvgl_image.h"

#include <utility>

ImageModel& ImageModel::GetInstance() {
    static ImageModel instance;
    return instance;
}

void ImageModel::SetImage(std::unique_ptr<LvglImage> image) {
    if (image == nullptr) {
        return;
    }

    auto shared_image = std::shared_ptr<LvglImage>(std::move(image));
    Update([image = std::move(shared_image)](ImageModelState& state) mutable {
        state.image = std::move(image);
        ++state.revision;
    });
}

void ImageModel::Clear() {
    if (Snapshot().image == nullptr) {
        return;
    }

    Update([](ImageModelState& state) {
        state.image.reset();
        ++state.revision;
    });
}
