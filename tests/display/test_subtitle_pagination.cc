#include "ui/screens/home/subtitle_pagination.h"

#include <cassert>
#include <climits>
#include <cstdio>
#include <string>
#include <string_view>

namespace {

// 独立的可变字宽布局条件，用于检查分页器遵守显示层测量结果。
// 实机由 LVGL 按实际字体、行距和内容宽度测量。
struct Layout {
    size_t columns = 28;
    size_t lines = 3;

    bool operator()(std::string_view text) const {
        size_t row = 1, column = 0;
        for (size_t i = 0; i < text.size();) {
            const auto character = static_cast<unsigned char>(text[i++]);
            if (character == '\r') continue;
            if (character == '\n') {
                ++row;
                column = 0;
            } else {
                const size_t width = character < 0x80 ? 1 : 2;
                while (i < text.size() &&
                       (static_cast<unsigned char>(text[i]) & 0xc0) == 0x80) ++i;
                if (column + width > columns) {
                    ++row;
                    column = 0;
                }
                column += width;
            }
            if (row > lines) return false;
        }
        return true;
    }
};

std::vector<SubtitlePagination::Page> Check(const std::string& text, Layout layout = {}) {
    const auto pages = SubtitlePagination::Split(text, layout);
    std::string restored;
    size_t characters = 0;
    for (const auto& page : pages) {
        assert(page.start_byte == restored.size());
        assert(page.start_character == characters);
        assert(page.end_byte > page.start_byte);
        assert(page.end_character > page.start_character);
        assert((static_cast<unsigned char>(text[page.start_byte]) & 0xc0) != 0x80);
        const auto value = text.substr(page.start_byte, page.end_byte - page.start_byte);
        assert(layout(value));
        restored += value;
        characters = page.end_character;
    }
    assert(restored == text);
    return pages;
}

}  // namespace

int main() {
    const std::string sentence = "浔阳江头夜送客，枫叶荻花秋瑟瑟。";
    assert(Check(sentence).size() == 1);  // 正常日志的 16 字符句段。
    assert(Check(sentence + sentence).size() == 1);  // 完整放得下，不额外切句。

    const std::string long_segment = sentence + sentence + sentence + sentence;
    const auto pages = Check(long_segment);
    assert(long_segment.size() == 192);  // 异常日志的 64 字符/192 字节句段。
    assert(pages.size() == 4);
    for (const auto& page : pages) {
        assert(long_segment.substr(page.start_byte, page.end_byte - page.start_byte) == sentence);
    }

    // 使用异常日志第 2 段的真实起止时间回放所有页边界。
    assert(SubtitlePagination::PageAt(pages, 4067, 22177, 4068) == 0);
    assert(SubtitlePagination::PageAt(pages, 4067, 22177, 8593) == 0);
    assert(SubtitlePagination::PageAt(pages, 4067, 22177, 8594) == 1);
    assert(SubtitlePagination::PageAt(pages, 4067, 22177, 13122) == 2);
    assert(SubtitlePagination::PageAt(pages, 4067, 22177, 17649) == 3);
    assert(SubtitlePagination::PageAt(pages, 4067, 22177, 22176) == 3);
    // 字幕晚到直接跟随当前播放位置，不从第一页重新计时。
    assert(SubtitlePagination::PageAt(pages, 4067, 22177, 18000) == 3);

    assert(Check("").empty());
    Check(std::string(180, 'A'));  // 无标点长句。
    Check("第一行\n第二行\n第三行\n第四行\n第五行\n");
    Check("中文与 English、数字123、表情🙂混排，保证 UTF-8 不被截断。" + long_segment);
    Check(long_segment, {18, 2});  // 更窄的显示区域。
    Check(long_segment, {40, 3});  // 更宽的显示区域。
    const auto proportional = Check("短句。" + long_segment);
    const uint32_t duration = 10000;
    const uint32_t second_page_at = duration * proportional[1].start_character /
                                    proportional.back().end_character;
    assert(SubtitlePagination::PageAt(proportional, 0, duration, second_page_at) == 1);
    assert(SubtitlePagination::PageAt(pages, UINT32_MAX - 20000, UINT32_MAX,
                                     UINT32_MAX - 1) == 3);
    assert(SubtitlePagination::PageAt({}, 0, 1000, 100) == 0);
    const auto not_ready = SubtitlePagination::Split(sentence,
        [](std::string_view) { return false; });
    assert(not_ready.empty());
    std::puts("subtitle pagination: lossless UTF-8, layout limits, short segments and timeline replay passed");
}
