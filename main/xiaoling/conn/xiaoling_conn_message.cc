#include "xiaoling_conn.h"

#include "board.h"
#include "system/system_info.h"
#include "xiaoling/xiaoling_events.h"
#include "xiaoling/session/session_voice.h"
#include "xiaoling_config.h"

#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_log.h>
#include "base64_codec.h"

#include <algorithm>
#include <cstring>

#define TAG "XiaolingConn"

namespace {

std::string JsonToString(cJSON* root) {
    char* text = cJSON_PrintUnformatted(root);
    if (text == nullptr) {
        return {};
    }
    std::string result(text);
    cJSON_free(text);
    return result;
}

std::string ReadRequestId(const cJSON* root) {
    const auto* rid = cJSON_GetObjectItemCaseSensitive(root, "rid");
    if (cJSON_IsString(rid) && rid->valuestring != nullptr) {
        return rid->valuestring;
    }
    if (cJSON_IsNumber(rid)) {
        return std::to_string(static_cast<unsigned long>(rid->valuedouble));
    }
    return {};
}

std::string ReadScalar(const cJSON* object, const char* name) {
    if (!cJSON_IsObject(object)) {
        return {};
    }
    const auto* item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (cJSON_IsString(item) && item->valuestring != nullptr) {
        return item->valuestring;
    }
    if (cJSON_IsNumber(item)) {
        return std::to_string(item->valueint);
    }
    return {};
}

std::string ReadErrorDetail(const cJSON* root) {
    static constexpr const char* kDetailNames[] = {"message", "msg", "desc"};
    const auto* data = cJSON_IsObject(root)
                           ? cJSON_GetObjectItemCaseSensitive(root, "data")
                           : nullptr;
    for (const auto* owner : {root, data}) {
        for (const auto* name : kDetailNames) {
            auto detail = ReadScalar(owner, name);
            if (!detail.empty()) {
                constexpr size_t kMaxDetailBytes = 256;
                if (detail.size() > kMaxDetailBytes) {
                    detail.resize(kMaxDetailBytes);
                    detail += "...";
                }
                return detail;
            }
        }
    }
    return {};
}

void LogTextFrame(const char* direction, const std::string& text) {
    constexpr size_t kLogChunkBytes = 512;
    for (size_t offset = 0, chunk = 0; offset < text.size();
         offset += kLogChunkBytes, ++chunk) {
        const size_t length = std::min(kLogChunkBytes, text.size() - offset);
        ESP_LOGI(TAG, "WS %s text[%u]: %.*s", direction,
                 static_cast<unsigned>(chunk), static_cast<int>(length),
                 text.data() + offset);
    }
}

}  // 匿名命名空间

std::string XiaolingConn::BuildWebSocketUrl() const {
    cJSON* params = cJSON_CreateObject();
    if (params == nullptr) {
        return {};
    }
    cJSON_AddStringToObject(params, "scene", "main");
    cJSON_AddBoolToObject(params, "mcp", true);
    cJSON_AddStringToObject(params, "tool_protocol_version", "v2");
    cJSON_AddStringToObject(params, "type", "fullduplex");

    cJSON* firmware_info = cJSON_CreateObject();
    if (firmware_info != nullptr) {
        cJSON_AddStringToObject(firmware_info, "type", BOARD_TYPE);
        cJSON_AddStringToObject(firmware_info, "version", esp_app_get_description()->version);
        cJSON_AddItemToObject(params, "firmware_info", firmware_info);
    }

    const auto params_json = JsonToString(params);
    cJSON_Delete(params);
    const auto encoded_params = Base64Codec::Encode(params_json);
    if (encoded_params.empty()) {
        return {};
    }

    std::string base_url = XiaolingConfig::GetWebSocketUrl();
    while (!base_url.empty() && base_url.back() == '/') {
        base_url.pop_back();
    }
    return base_url + "/v1/interaction?param=" + encoded_params;
}

bool XiaolingConn::SendText(const std::string& text) {
    if (text.empty() || !IsConnected()) {
        ESP_LOGW(TAG, "WebSocket 未连接或发送文本为空");
        return false;
    }
    LogTextFrame("TX", text);

    bool sent = false;
    {
        std::lock_guard<std::mutex> lock(websocket_mutex_);
        sent = websocket_ != nullptr && websocket_->SendText(text);
    }
    if (!sent) {
        HandleTransportDisconnected(generation_.load());
        SetError(XiaolingError::ServerError);
    }
    return sent;
}

void XiaolingConn::SendMcpMessage(const std::string& message) {
    cJSON* root = cJSON_ParseWithLength(message.data(), message.size());
    if (!cJSON_IsObject(root)) {
        ESP_LOGE(TAG, "MCP 响应不是有效 JSON");
        cJSON_Delete(root);
        return;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(root, "action");
    cJSON_AddStringToObject(root, "action", "mcp");
    const auto text = JsonToString(root);
    cJSON_Delete(root);
    SendText(text);
}

bool XiaolingConn::IsCurrentSessionMessage(const cJSON* root) const {
    const auto rid = ReadRequestId(root);
    const auto& active_session_id = session_id();
    return rid.empty() || active_session_id.empty() || rid == active_session_id;
}

void XiaolingConn::HandleIncomingText(const std::string& text) {
    cJSON* root = cJSON_ParseWithLength(text.data(), text.size());
    if (root == nullptr) {
        LogTextFrame("RX", text);
        ESP_LOGE(TAG, "收到的小聆消息不是有效 JSON");
        return;
    }
    LogTextFrame("RX", text);

    const auto* action = cJSON_GetObjectItemCaseSensitive(root, "action");
    const bool current_session = IsCurrentSessionMessage(root);
    if (cJSON_IsString(action) && action->valuestring != nullptr && current_session) {
        if (voice_ != nullptr) {
            voice_->HandleMessage(root);
        }

        if (std::strcmp(action->valuestring, "connected") == 0) {
            error_occurred_ = false;
            protocol_connected_ = true;
            if (event_group_ != nullptr) {
                xEventGroupSetBits(event_group_, kProtocolConnectedBit);
            }
            XiaolingEventBus::GetInstance().Publish(XIAOLING_CONNECTED);
            if (on_connected_) {
                on_connected_();
            }
        } else if (std::strcmp(action->valuestring, "started") == 0) {
            // started 证明链路可用，清掉历史会话遗留的错误标志，避免闩死。
            if (error_occurred_) {
                ESP_LOGI(TAG, "收到 started，清除遗留错误标志");
                error_occurred_ = false;
            }
        } else if (std::strcmp(action->valuestring, "error") == 0) {
            const auto code = ReadScalar(root, "code");
            const auto detail = ReadErrorDetail(root);
            ESP_LOGE(TAG, "小聆云端错误: code=%s, detail=%s",
                     code.empty() ? "unknown" : code.c_str(),
                     detail.empty() ? "-" : detail.c_str());
            if (code == "401") {
                ClearAuthentication();
            }
            SetError(XiaolingError::ServerError);
        }
    }

    if (on_incoming_json_ && current_session) {
        on_incoming_json_(root);
    }
    cJSON_Delete(root);
}
