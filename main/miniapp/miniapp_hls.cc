#include "miniapp_hls.h"
#include "miniapp_hls_playlist.h"
#include "player/backend/esp_simple_player_backend.h"
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_gmf_io.h>
#include <esp_gmf_oal_mem.h>
#include <esp_gmf_payload.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <memory>
#include <cstring>

namespace {
constexpr const char *kPrefix = "miniapp-hls://";
constexpr const char *kSuffix = "#__miniapp.ts";
class Reader {
  public:
    explicit Reader(esp_gmf_io_t *io) : io_(io) {}
    ~Reader() { Close(); }
    bool Open(const std::string &url) {
        playlist_url_ = url;
        for (unsigned depth = 0; depth < 4; ++depth) {
            if (!Refresh())
                return false;
            if (playlist_.variant.empty()) {
                next_ = playlist_.sequence;
                if (!playlist_.end && playlist_.segments.size() > 3)
                    next_ += playlist_.segments.size() - 3;
                return true;
            }
            playlist_url_ = MiniappHls::Resolve(playlist_url_, playlist_.variant);
        }
        return false;
    }
    int Read(uint8_t *bytes, size_t capacity) {
        if (io_->_is_abort)
            return -1;
        for (;;) {
            if (client_) {
                const int size =
                    esp_http_client_read(client_, reinterpret_cast<char *>(bytes), capacity);
                if (size > 0)
                    return size;
                if (size < 0 || !esp_http_client_is_complete_data_received(client_))
                    return -1;
                Close();
            }
            if (next_ < playlist_.sequence)
                next_ = playlist_.sequence;
            if (next_ - playlist_.sequence < playlist_.segments.size()) {
                auto url = MiniappHls::Resolve(playlist_url_,
                                               playlist_.segments[next_ - playlist_.sequence]);
                ++next_;
                if (!Connect(url))
                    return -1;
                continue;
            }
            if (playlist_.end)
                return 0;
            const auto start = xTaskGetTickCount();
            while (!io_->_is_abort && xTaskGetTickCount() - start < pdMS_TO_TICKS(500))
                vTaskDelay(pdMS_TO_TICKS(20));
            if (io_->_is_abort || !Refresh())
                return -1;
            if (next_ < playlist_.sequence + playlist_.segments.size())
                stalled_ = 0;
            else if (++stalled_ > 60)
                return -1;
        }
    }

  private:
    void Close() {
        if (client_) {
            esp_http_client_close(client_);
            esp_http_client_cleanup(client_);
            client_ = nullptr;
        }
    }
    bool Connect(std::string url) {
        Close();
        for (unsigned redirect = 0; redirect < 4; ++redirect) {
            redirect_location_.clear();
            esp_http_client_config_t config{};
            config.user_data = &redirect_location_;
            config.event_handler = [](esp_http_client_event_t *event) -> esp_err_t {
                if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key &&
                    event->header_value && !strcasecmp(event->header_key, "Location"))
                    *static_cast<std::string *>(event->user_data) = event->header_value;
                return ESP_OK;
            };
            config.url = url.c_str();
            config.timeout_ms = 5000;
            config.disable_auto_redirect = true;
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
            config.crt_bundle_attach = esp_crt_bundle_attach;
#endif
            client_ = esp_http_client_init(&config);
            if (!client_ || io_->_is_abort || esp_http_client_open(client_, 0) != ESP_OK ||
                esp_http_client_fetch_headers(client_) < 0)
                return false;
            const int status = esp_http_client_get_status_code(client_);
            if (status == 200) {
                connected_url_ = url;
                return true;
            }
            if (status < 300 || status >= 400 || redirect_location_.empty())
                return false;
            url = MiniappHls::Resolve(url, redirect_location_);
            Close();
        }
        return false;
    }
    bool Refresh() {
        if (!Connect(playlist_url_))
            return false;
        playlist_url_ = connected_url_;
        std::string text;
        char buffer[1024];
        for (;;) {
            if (io_->_is_abort)
                return false;
            int size = esp_http_client_read(client_, buffer, sizeof(buffer));
            if (size < 0 || text.size() + size > 32768)
                return false;
            if (!size)
                break;
            text.append(buffer, size);
        }
        if (!esp_http_client_is_complete_data_received(client_))
            return false;
        Close();
        return MiniappHls::Parse(text, playlist_);
    }
    esp_gmf_io_t *io_;
    esp_http_client_handle_t client_ = nullptr;
    std::string playlist_url_, connected_url_, redirect_location_;
    MiniappHls::Playlist playlist_;
    uint64_t next_ = 0;
    unsigned stalled_ = 0;
};
struct HlsIo {
    esp_gmf_io_t base;
    Reader *reader;
};
esp_gmf_err_t Init(void *cfg, esp_gmf_obj_handle_t *out);
esp_gmf_err_t Close(esp_gmf_io_handle_t io) {
    auto *value = static_cast<HlsIo *>(io);
    delete value->reader;
    value->reader = nullptr;
    return ESP_GMF_ERR_OK;
}
esp_gmf_err_t Delete(esp_gmf_io_handle_t io) {
    Close(io);
    free(OBJ_GET_CFG(io));
    esp_gmf_io_deinit(io);
    free(io);
    return ESP_GMF_ERR_OK;
}
esp_gmf_err_t Open(esp_gmf_io_handle_t io) {
    auto *value = static_cast<HlsIo *>(io);
    if (value->reader)
        return ESP_GMF_ERR_OK;
    char *uri = nullptr;
    esp_gmf_io_get_uri(io, &uri);
    if (!uri || strlen(uri) <= strlen(kPrefix) + strlen(kSuffix))
        return ESP_GMF_ERR_INVALID_ARG;
    std::string url(uri + strlen(kPrefix));
    url.resize(url.size() - strlen(kSuffix));
    auto reader = std::make_unique<Reader>(&value->base);
    if (!reader->Open(url))
        return ESP_GMF_ERR_FAIL;
    value->reader = reader.release();
    return ESP_GMF_ERR_OK;
}
esp_gmf_err_io_t Read(esp_gmf_io_handle_t io, void *payload, uint32_t wanted, int) {
    auto *value = static_cast<HlsIo *>(io);
    auto *data = static_cast<esp_gmf_payload_t *>(payload);
    if (!value->reader)
        return ESP_GMF_IO_FAIL;
    const int size = value->reader->Read(data->buf, wanted);
    if (size < 0)
        return ESP_GMF_IO_FAIL;
    data->valid_size = size;
    data->is_done = !size;
    return ESP_GMF_IO_OK;
}
esp_gmf_err_t Init(void *input, esp_gmf_obj_handle_t *out) {
    auto *io = static_cast<HlsIo *>(esp_gmf_oal_calloc(1, sizeof(HlsIo)));
    auto *cfg = static_cast<esp_gmf_io_cfg_t *>(esp_gmf_oal_calloc(1, sizeof(esp_gmf_io_cfg_t)));
    if (!io || !cfg) {
        free(io);
        free(cfg);
        return ESP_GMF_ERR_MEMORY_LACK;
    }
    *cfg = *static_cast<esp_gmf_io_cfg_t *>(input);
    auto *obj = &io->base.parent;
    obj->new_obj = Init;
    obj->del_obj = Delete;
    esp_gmf_obj_set_config(obj, cfg, sizeof(*cfg));
    esp_gmf_obj_set_tag(obj, "io_miniapp_hls");
    io->base.dir = ESP_GMF_IO_DIR_READER;
    io->base.type = ESP_GMF_IO_TYPE_BYTE;
    io->base.get_score = [](esp_gmf_io_handle_t, const char *url, int *score) -> esp_gmf_err_t {
        *score = strncmp(url, kPrefix, strlen(kPrefix)) ? ESP_GMF_IO_SCORE_NONE
                                                        : ESP_GMF_IO_SCORE_PERFECT;
        return ESP_GMF_ERR_OK;
    };
    io->base.open = Open;
    io->base.close = Close;
    io->base.acquire_read = Read;
    io->base.release_read = [](esp_gmf_io_handle_t, void *, int) -> esp_gmf_err_io_t {
        return ESP_GMF_IO_OK;
    };
    auto result = esp_gmf_io_init(obj, cfg);
    if (result != ESP_GMF_ERR_OK) {
        esp_gmf_obj_delete(obj);
        return result;
    }
    *out = obj;
    return ESP_GMF_ERR_OK;
}
} // namespace
bool MiniappRegisterHls(EspSimplePlayerBackend &backend) {
    esp_gmf_io_cfg_t config{};
    config.thread.stack = 6144;
    config.thread.prio = 4;
    config.thread.core = 0;
    config.thread.stack_in_ext = true;
    config.buffer_cfg.io_size = 2048;
    config.buffer_cfg.buffer_size = 32768;
    esp_gmf_obj_handle_t io = nullptr;
    if (Init(&config, &io) != ESP_GMF_ERR_OK)
        return false;
    if (backend.RegisterInputIo(io))
        return true;
    esp_gmf_obj_delete(io);
    return false;
}
std::string MiniappAudioUri(const std::string &url) {
    const auto path = url.substr(0, url.find_first_of("?#"));
    if (path.size() >= 5 && !strcasecmp(path.c_str() + path.size() - 5, ".m3u8"))
        return kPrefix + url + kSuffix;
    return url;
}
