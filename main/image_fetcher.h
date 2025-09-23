#ifndef _IMAGE_FETCHER_H_
#define _IMAGE_FETCHER_H_

#include <string>
#include <lvgl.h>

class Board;
class ImageFetcher {
public:
    ImageFetcher(Board* board) : board_(board) {
    }

    virtual ~ImageFetcher();

    bool Fetch(const std::string& url, lv_img_dsc_t* into, int timeout_ms = 15000);

private:
    Board* board_;
    uint8_t* rgb_buffer_ = nullptr;
};

#endif // _IMAGE_FETCHER_H_
