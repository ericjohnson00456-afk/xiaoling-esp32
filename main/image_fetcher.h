#ifndef _IMAGE_FETCHER_H_
#define _IMAGE_FETCHER_H_

#include <string>
#include <memory>

#include "lvgl_image.h"

class Board;
class ImageFetcher {
public:
    ImageFetcher(Board* board) : board_(board) {
    }

    virtual ~ImageFetcher() = default;

    std::unique_ptr<LvglImage> Fetch(const std::string& url, int timeout_ms = 15000);

private:
    Board* board_;
};

#endif // _IMAGE_FETCHER_H_
