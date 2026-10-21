#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace SubtitlePagination {

// 页范围始终指向原文，不删除空白或在 UTF-8 字符中间切分。
struct Page {
    size_t start_byte;
    size_t end_byte;
    size_t start_character;
    size_t end_character;
};

inline bool IsSentenceEnd(std::string_view character) {
    return character == "。" || character == "！" || character == "？" ||
           character == "；" || character == "." || character == "!" ||
           character == "?" || character == ";" || character == "\n";
}

inline bool IsWordBreak(std::string_view character) {
    return character == "，" || character == "、" || character == "：" ||
           character == "," || character == ":" || character == " " ||
           character == "\t";
}

// fits 使用显示层的真实字体和行宽测量。能完整显示的句段不拆分；
// 超长段优先每句一页，单句仍过长时按停顿或可容纳的字符数分页。
template <typename Fits>
std::vector<Page> Split(const std::string& text, Fits fits) {
    std::vector<Page> pages;
    if (text.empty()) return pages;

    std::vector<size_t> offsets{0};
    for (size_t offset = 0; offset < text.size();) {
        ++offset;
        while (offset < text.size() &&
               (static_cast<uint8_t>(text[offset]) & 0xc0) == 0x80) {
            ++offset;
        }
        offsets.push_back(offset);
    }
    const size_t characters = offsets.size() - 1;
    const auto slice = [&](size_t first, size_t last) {
        return std::string_view(text).substr(offsets[first], offsets[last] - offsets[first]);
    };
    if (fits(slice(0, characters))) {
        pages.push_back({0, text.size(), 0, characters});
        return pages;
    }

    for (size_t first = 0; first < characters;) {
        // 单字符都无法容纳说明布局尚未就绪，留待下一次 UI 推进重试。
        if (!fits(slice(first, first + 1))) return {};
        size_t low = first + 1;
        size_t high = characters;
        while (low < high) {
            const size_t mid = low + (high - low + 1) / 2;
            if (fits(slice(first, mid))) low = mid;
            else high = mid - 1;
        }
        size_t end = low;
        size_t word_break = first;
        for (size_t i = first; i < low; ++i) {
            const auto character = slice(i, i + 1);
            if (IsSentenceEnd(character)) {
                end = i + 1;
                break;
            }
            if (IsWordBreak(character)) word_break = i + 1;
            if (i + 1 == low && low < characters && word_break > first) {
                end = word_break;
            }
        }
        pages.push_back({offsets[first], offsets[end], first, end});
        first = end;
    }
    return pages;
}

// 云端没有句内字级时间；按字符占比分配整段时间，不改云端 timeline 游标。
inline size_t PageAt(const std::vector<Page>& pages, uint32_t start_ms,
                     uint32_t end_ms, uint32_t clock_ms) {
    if (pages.empty() || clock_ms <= start_ms || end_ms <= start_ms) return 0;
    const uint64_t duration_ms = static_cast<uint64_t>(end_ms) - start_ms;
    const size_t characters = pages.back().end_character;
    size_t page = 0;
    while (page + 1 < pages.size() &&
           clock_ms >= start_ms + duration_ms * pages[page + 1].start_character / characters) {
        ++page;
    }
    return page;
}

}  // namespace SubtitlePagination
