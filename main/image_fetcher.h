#ifndef _IMAGE_FETCHER_H_
#define _IMAGE_FETCHER_H_

#include <string>
#include <network_interface.h>
#include <lvgl.h>

class ImageFetcher {
protected:
    ImageFetcher(NetworkInterface* network) : network_(network) {
    }

public:
    static ImageFetcher From(NetworkInterface* network) {
        return ImageFetcher(network);
    }

    virtual ~ImageFetcher() = default;

    bool Fetch(const std::string& url, lv_img_dsc_t* into, int timeout_ms = 15000);

private:
    NetworkInterface* network_;
};

#endif // _IMAGE_FETCHER_H_
