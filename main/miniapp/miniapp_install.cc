#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>
#include <esp_heap_caps.h>
#include "miniapp_install.h"
#include "miniapp_platform.h"
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include "system/md5.h"
#include <cJSON.h>
#include <cstring>
#include <memory>

namespace Miniapp {
std::string JsonText(cJSON *value) {
    if (!value)
        return {};
    char *raw = cJSON_PrintUnformatted(value);
    std::string result = raw ? raw : "";
    cJSON_free(raw);
    cJSON_Delete(value);
    return result;
}
bool VerifySource(const miniapp_package_t &package, const char *source, std::string &error) {
    unsigned char digest[16];
    Md5::Digest(source, package.size, digest);
    char hash[33];
    for (int i = 0; i < 16; ++i)
        snprintf(hash + i * 2, 3, "%02x", digest[i]);
    if (strcmp(hash, package.hash)) {
        error = "source MD5 mismatch";
        return false;
    }
    return true;
}
namespace {
struct Install {
    miniapp_package_t package;
    std::string url;
    Completion completion;
};
void Worker(void *value) {
    std::unique_ptr<Install> job(static_cast<Install *>(value));
    bool ok = false;
    std::string error;
    char *source = static_cast<char *>(miniapp_alloc(job->package.size + 1));
    esp_http_client_config_t config{};
    config.url = job->url.c_str();
    config.timeout_ms = 15000;
    config.disable_auto_redirect = true;
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    config.crt_bundle_attach = esp_crt_bundle_attach;
#endif
    auto client = esp_http_client_init(&config);
    if (!source || !client)
        error = "not enough memory for source download";
    else if (esp_http_client_open(client, 0) != ESP_OK)
        error = "source download failed";
    else {
        const auto length = esp_http_client_fetch_headers(client);
        if (esp_http_client_get_status_code(client) != 200 ||
            (length > 0 && length != job->package.size))
            error = "source length or HTTP status mismatch";
        else {
            size_t read = 0;
            const auto start = xTaskGetTickCount();
            while (read <= job->package.size) {
                if (xTaskGetTickCount() - start > pdMS_TO_TICKS(30000)) {
                    error = "source download timed out";
                    break;
                }
                const int n =
                    esp_http_client_read(client, source + read, job->package.size + 1 - read);
                if (n <= 0) {
                    if (n < 0 || !esp_http_client_is_complete_data_received(client))
                        error = "source download interrupted";
                    break;
                }
                read += n;
                if (read > job->package.size)
                    break;
            }
            if (error.empty() && read != job->package.size)
                error = "source length mismatch";
            if (error.empty() && VerifySource(job->package, source, error)) {
                char message[128]{};
                ok = miniapp_install(&job->package, source, message, sizeof(message)) == 0;
                error = message;
            }
        }
    }
    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    free(source);
    miniapp_install_end();
    job->completion(ok, ok ? "miniapp installed" : error);
    job.reset();
    vTaskDeleteWithCaps(nullptr);
}
} // namespace
void InstallUrl(miniapp_package_t package, std::string url, Completion completion) {
    if (miniapp_init() != 0) {
        completion(false, "miniapp runtime unavailable");
        return;
    }
    if (!miniapp_install_begin()) {
        completion(false, "miniapp unavailable or install in progress");
        return;
    }
    if (miniapp_matches(&package)) {
        miniapp_install_end();
        completion(true, "miniapp already running");
        return;
    }
    auto job = std::make_unique<Install>(Install{package, std::move(url), std::move(completion)});
    if (xTaskCreateWithCaps(Worker, "miniapp_install", 8192, job.get(), 4, nullptr,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS)
        job.release();
    else {
        miniapp_install_end();
        job->completion(false, "not enough memory for install worker");
    }
}
} // namespace Miniapp
