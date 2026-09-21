#ifndef XIAOLING_EVENTS_H
#define XIAOLING_EVENTS_H

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

// 小聆业务事件。IAT/TTS_TXT 的 text 是展示文本，TTS_URL 的 text 是解码后的 URL。
// 订阅者可以安全地跨任务复制事件数据。
enum XiaolingEventType {
    XIAOLING_CONNECTED,
    XIAOLING_DISCONNECTED,

    XIAOLING_IAT_START,
    XIAOLING_IAT_UPDATE,
    XIAOLING_IAT_END,

    XIAOLING_TTS_URL,
    XIAOLING_TTS_TXT_START,
    XIAOLING_TTS_TXT_UPDATE,
    XIAOLING_TTS_TXT_END,
    XIAOLING_TTS_TXT_INTERRUPT,
    XIAOLING_TTS_STREAM_READY,
    XIAOLING_TTS_PLAYING,
    XIAOLING_TTS_OUTPUT_STARTED,
    XIAOLING_TTS_OUTPUT_PROGRESS,
    XIAOLING_TTS_STOPPED,
};

struct XiaolingEvent {
    XiaolingEventType type;
    std::string text;
};

class XiaolingEventBus final {
public:
    using Callback = std::function<void(const XiaolingEvent&)>;
    using SubscriptionId = uint32_t;

    static XiaolingEventBus& GetInstance();

    SubscriptionId Subscribe(XiaolingEventType type, Callback callback);
    void Unsubscribe(SubscriptionId id);
    void Publish(XiaolingEventType type, std::string text = {});

private:
    struct Subscription {
        SubscriptionId id;
        XiaolingEventType type;
        Callback callback;
    };

    std::mutex mutex_;
    std::vector<Subscription> subscriptions_;
    SubscriptionId next_id_ = 1;
};

#endif  // XIAOLING_EVENTS_H
