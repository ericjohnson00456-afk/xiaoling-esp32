#include "xiaoling_conn.h"

#include "board.h"
#include "system/system_info.h"
#include "event/app_event.h"
#include "xiaoling/session/session_voice.h"
#include "xiaoling_config.h"

#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_random.h>
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
        cJSON_AddStringToObject(firmware_info, "type", "arcs-mini");
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
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        if (control_queue_.size() >= kMaxControlMessages) {
            ESP_LOGE(TAG, "WebSocket 控制队列已满: pending=%u",
                     static_cast<unsigned>(control_queue_.size()));
            return false;
        }
        control_queue_.push_back(ControlFrame{false, text});
    }
    if (task_handle_ != nullptr) {
        xTaskNotifyGive(task_handle_);
    }
    return true;
}

bool XiaolingConn::RequestAlarmTtsUrl(const std::string& text,
                                      AlarmTtsCallback callback) {
    if (text.empty() || !callback || !IsConnected()) {
        return false;
    }

    uint32_t request_id = esp_random();
    if (request_id == 0) request_id = 1;
    const std::string rid = std::to_string(static_cast<unsigned long>(request_id));

    cJSON* root = cJSON_CreateObject();
    cJSON* params = root != nullptr ? cJSON_AddObjectToObject(root, "params") : nullptr;
    cJSON* features = params != nullptr ? cJSON_AddArrayToObject(params, "features") : nullptr;
    cJSON* nlu = params != nullptr ? cJSON_AddObjectToObject(params, "nlu_properties") : nullptr;
    cJSON* custom = nlu != nullptr ? cJSON_AddObjectToObject(nlu, "custom") : nullptr;
    if (root == nullptr || params == nullptr || features == nullptr || nlu == nullptr ||
        custom == nullptr) {
        cJSON_Delete(root);
        return false;
    }
    cJSON_AddStringToObject(root, "action", "start");
    cJSON_AddStringToObject(params, "data_type", "text");
    cJSON_AddItemToArray(features, cJSON_CreateString("tts"));
    cJSON_AddStringToObject(custom, "rid", rid.c_str());
    const std::string start_message = JsonToString(root);
    cJSON_Delete(root);
    if (start_message.empty()) {
        return false;
    }

    std::string text_frame = text;
    text_frame.push_back('\0');
    {
        std::lock_guard<std::mutex> lock(alarm_tts_mutex_);
        if (!pending_alarm_tts_.rid.empty()) {
            ESP_LOGW(TAG, "已有闹钟 TTS 请求正在等待: rid=%s",
                     pending_alarm_tts_.rid.c_str());
            return false;
        }
        pending_alarm_tts_ = PendingAlarmTts{rid, std::move(callback), false};
    }
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        if (control_queue_.size() + 2 > kMaxControlMessages) {
            std::lock_guard<std::mutex> tts_lock(alarm_tts_mutex_);
            pending_alarm_tts_ = {};
            ESP_LOGE(TAG, "WebSocket 控制队列空间不足，无法请求闹钟 TTS");
            return false;
        }
        // 两帧在同一次加锁内入队，确保文本二进制帧不会越过 start。
        control_queue_.push_back(ControlFrame{false, start_message});
        control_queue_.push_back(ControlFrame{true, std::move(text_frame)});
    }
    LogTextFrame("TX", start_message);
    ESP_LOGI(TAG, "已提交闹钟 TTS 文本会话: rid=%s, text_bytes=%u", rid.c_str(),
             static_cast<unsigned>(text.size()));
    if (task_handle_ != nullptr) {
        xTaskNotifyGive(task_handle_);
    }
    return true;
}

void XiaolingConn::CancelAlarmTtsRequest() {
    std::lock_guard<std::mutex> lock(alarm_tts_mutex_);
    if (pending_alarm_tts_.rid.empty()) {
        return;
    }
    ignored_alarm_tts_rids_.push_back(pending_alarm_tts_.rid);
    while (ignored_alarm_tts_rids_.size() > 4) {
        ignored_alarm_tts_rids_.pop_front();
    }
    pending_alarm_tts_ = {};
}

bool XiaolingConn::HandleAlarmTtsMessage(const cJSON* root) {
    const std::string rid = ReadRequestId(root);
    if (rid.empty()) {
        return false;
    }
    const auto* action_item = cJSON_GetObjectItemCaseSensitive(root, "action");
    const char* action = cJSON_IsString(action_item) ? action_item->valuestring : nullptr;
    if (action == nullptr) {
        return false;
    }

    AlarmTtsCallback callback;
    bool callback_success = false;
    std::string url;
    bool matched = false;
    const bool terminal = std::strcmp(action, "finish") == 0 ||
                          std::strcmp(action, "closed") == 0 ||
                          std::strcmp(action, "error") == 0;
    {
        std::lock_guard<std::mutex> lock(alarm_tts_mutex_);
        if (pending_alarm_tts_.rid == rid) {
            matched = true;
            if (std::strcmp(action, "result") == 0 && !pending_alarm_tts_.url_delivered) {
                const auto* data = cJSON_GetObjectItemCaseSensitive(root, "data");
                const auto* sub = cJSON_IsObject(data)
                                      ? cJSON_GetObjectItemCaseSensitive(data, "sub")
                                      : nullptr;
                const auto* content = cJSON_IsObject(data)
                                          ? cJSON_GetObjectItemCaseSensitive(data, "content")
                                          : nullptr;
                if (cJSON_IsString(sub) && sub->valuestring != nullptr &&
                    std::strcmp(sub->valuestring, "tts") == 0 &&
                    cJSON_IsString(content) && content->valuestring != nullptr &&
                    Base64Codec::Decode(content->valuestring, url) && !url.empty() &&
                    std::memchr(url.data(), '\0', url.size()) == nullptr) {
                    pending_alarm_tts_.url_delivered = true;
                    callback_success = true;
                    callback = std::move(pending_alarm_tts_.callback);
                }
            }
            if (terminal) {
                if (!pending_alarm_tts_.url_delivered) {
                    callback = std::move(pending_alarm_tts_.callback);
                }
                pending_alarm_tts_ = {};
            }
        } else {
            const auto ignored = std::find(ignored_alarm_tts_rids_.begin(),
                                           ignored_alarm_tts_rids_.end(), rid);
            if (ignored != ignored_alarm_tts_rids_.end()) {
                matched = true;
                if (terminal) {
                    ignored_alarm_tts_rids_.erase(ignored);
                }
            }
        }
    }
    if (callback) {
        callback(callback_success, std::move(url));
    }
    return matched;
}

void XiaolingConn::FailPendingAlarmTtsRequest() {
    AlarmTtsCallback callback;
    {
        std::lock_guard<std::mutex> lock(alarm_tts_mutex_);
        callback = std::move(pending_alarm_tts_.callback);
        pending_alarm_tts_ = {};
        ignored_alarm_tts_rids_.clear();
    }
    if (callback) {
        callback(false, {});
    }
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
    const bool alarm_tts_message = HandleAlarmTtsMessage(root);
    const bool current_session = IsCurrentSessionMessage(root);
    if (!alarm_tts_message && cJSON_IsString(action) && action->valuestring != nullptr &&
        current_session) {
        if (voice_ != nullptr) {
            voice_->HandleMessage(root);
        }

        if (std::strcmp(action->valuestring, "connected") == 0) {
            error_occurred_ = false;
            protocol_connected_ = true;
            if (event_group_ != nullptr) {
                xEventGroupSetBits(event_group_, kProtocolConnectedBit);
            }
            AppEvent::Publish(AppEvent::Id::XiaolingConnected);
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

    if (!alarm_tts_message && on_incoming_json_ && current_session) {
        on_incoming_json_(root);
    }
    cJSON_Delete(root);
}
