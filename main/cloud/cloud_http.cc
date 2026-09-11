#include "cloud_http.h"

#include "board.h"

#include <esp_log.h>

#define TAG "CloudHttp"

bool CloudHttp::Get(const std::string& url, CloudHttpResponse& response,
                    const CloudHttpHeaders& headers, int timeout_ms) const {
    return Request("GET", url, "", response, headers, timeout_ms);
}

bool CloudHttp::PostJson(const std::string& url, const std::string& body,
                         CloudHttpResponse& response, const CloudHttpHeaders& headers,
                         int timeout_ms) const {
    auto request_headers = headers;
    request_headers.emplace_back("Content-Type", "application/json");
    return Request("POST", url, body, response, request_headers, timeout_ms);
}

bool CloudHttp::Request(const std::string& method, const std::string& url,
                        const std::string& body, CloudHttpResponse& response,
                        const CloudHttpHeaders& headers, int timeout_ms) const {
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        ESP_LOGE(TAG, "Network interface is unavailable");
        return false;
    }

    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        return false;
    }

    if (timeout_ms > 0) {
        http->SetTimeout(timeout_ms);
    }
    for (const auto& [name, value] : headers) {
        http->SetHeader(name.c_str(), value.c_str());
    }
    if (!body.empty()) {
        http->SetContent(std::string(body));
    }

    if (!http->Open(method, url)) {
        ESP_LOGE(TAG, "Failed to open %s request: %s", method.c_str(), url.c_str());
        return false;
    }

    response.status_code = http->GetStatusCode();
    response.body = http->ReadAll();
    http->Close();
    return true;
}
