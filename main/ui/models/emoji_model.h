#pragma once

#include "lvgl_image.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

class Assets;

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

// 表情资源与动画配置模型。
// 资源由 ROMFS 提供，模型只保存 PNG 描述和动画播放参数，不操作 LVGL 控件。
class EmojiModel {
public:
    // 兼容普通单图表情资源；资源所有权由模型接管。
    void AddEmoji(const std::string& name, LvglImage* image);
    const LvglImage* GetEmojiImage(const char* name) const;

    // 在 UI 初始化阶段读取 emoji.json 并建立所有动画帧索引。
    bool LoadFromAssets(Assets& assets, const std::string& config_path = "/emoji/emoji.json");

    // 获取表情动画；alias 会自动解析，找不到时返回 nullptr。
    const EmojiAnimation* GetAnimation(const std::string& name) const;
    const std::string& default_emoji() const { return default_emoji_; }

    void ClearAnimations();

private:
    std::map<std::string, std::unique_ptr<LvglImage>> emojis_;
    std::map<std::string, EmojiAnimation> animations_;
    std::string default_emoji_;
};
