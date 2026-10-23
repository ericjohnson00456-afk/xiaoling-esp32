#pragma once

#include "ui/utils/image/lvgl_image.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

// 表情动画的一个阶段，例如 intro、loop 或 outro。
struct EmojiPhase {
    std::vector<std::shared_ptr<LvglImage>> frames;
    std::vector<uint32_t> delays;
    uint32_t default_interval_ms = 100;
    int loop = 0;  // 0 表示播放一次，-1 表示无限循环。

    bool empty() const { return frames.empty(); }
};

// emoji.json 中一个表情的完整动画定义。
struct EmojiAnimation {
    std::string name;
    std::string alias;
    EmojiPhase intro;
    EmojiPhase loop;
    EmojiPhase outro;
};

// 表情资源仓库。资源由 ROMFS 提供；小聆黑底样式在加载时把帧预解码为
// PSRAM 中的不透明 RGB565，并由共享指针维持播放快照的像素生命周期。
// 不保存页面状态，也不操作 LVGL 控件。
class EmojiRepository {
public:
    // 兼容普通单图表情资源；资源所有权由仓库接管。
    void AddEmoji(const std::string& name, LvglImage* image);
    const LvglImage* GetEmojiImage(const char* name) const;

    // LVGL 初始化后读取 emoji.json 并准备动画帧；运行时重载须持有显示锁。
    bool LoadFromResources(const std::string& config_path = "/emoji/emoji.json");

    // 热重载安装新仓库、释放旧仓库后，利用归还的 PSRAM 补齐回退帧。
    // 调用方须持有显示锁，且尚未从新仓库创建播放快照。
    void PredecodeRemainingFrames();

    // 获取表情动画；alias 会自动解析，找不到时返回 nullptr。
    const EmojiAnimation* GetAnimation(const std::string& name) const;
    std::vector<std::string> GetNames() const;
    const std::string& default_emoji() const { return default_emoji_; }

    void ClearAnimations();

private:
    std::map<std::string, std::unique_ptr<LvglImage>> emojis_;
    std::map<std::string, EmojiAnimation> animations_;
    std::string default_emoji_;
};
