#include "sd_music_scanner.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

#include <esp_log.h>
#include <ff.h>

namespace {

constexpr char kTag[] = "SdMusicScanner";
constexpr char kDefaultMusicDirectory[] = "/sdcard/audio";
constexpr int kMaxDirectoryDepth = 3;
constexpr size_t kMaxPathBytes = 512;
constexpr size_t kMaxDirectoryNameBytes = 32 * 3;
constexpr size_t kMaxFileNameBytes = 64 * 3 + 8;
constexpr size_t kMaxTextFrameBytes = 4096;
constexpr size_t kMaxTitleBytes = 63;
constexpr size_t kMaxArtistBytes = 31;
constexpr size_t kMaxAlbumBytes = 31;

bool EqualsAsciiIgnoreCase(char left, char right) {
    if (left >= 'A' && left <= 'Z') left += 'a' - 'A';
    if (right >= 'A' && right <= 'Z') right += 'a' - 'A';
    return left == right;
}

bool EndsWithIgnoreCase(const std::string& value, const char* suffix) {
    const size_t suffix_length = std::strlen(suffix);
    if (value.size() < suffix_length) return false;
    const size_t offset = value.size() - suffix_length;
    for (size_t i = 0; i < suffix_length; ++i) {
        if (!EqualsAsciiIgnoreCase(value[offset + i], suffix[i])) return false;
    }
    return true;
}

bool IsMp3(const std::string& name) {
    return EndsWithIgnoreCase(name, ".mp3");
}

bool IsFatRecoveryDirectory(const std::string& name) {
    if (name.size() != 9 || !EqualsAsciiIgnoreCase(name[0], 'F') ||
        !EqualsAsciiIgnoreCase(name[1], 'O') ||
        !EqualsAsciiIgnoreCase(name[2], 'U') ||
        !EqualsAsciiIgnoreCase(name[3], 'N') ||
        !EqualsAsciiIgnoreCase(name[4], 'D') || name[5] != '.') {
        return false;
    }
    return name[6] >= '0' && name[6] <= '9' &&
           name[7] >= '0' && name[7] <= '9' &&
           name[8] >= '0' && name[8] <= '9';
}

bool ShouldIgnoreDirectory(const std::string& name) {
    return name == ".Spotlight-V100" || name == ".Trashes" ||
           name == ".fseventsd" || name == "System Volume Information" ||
           IsFatRecoveryDirectory(name);
}

bool ShouldIgnoreFile(const std::string& name) {
    if (name == ".DS_Store" || name.rfind("._", 0) == 0 ||
        EndsWithIgnoreCase(name, ".CHK")) {
        return true;
    }
    // macOS 在部分 FAT 卡上会留下 _xxx.mp3 形式的 AppleDouble 伴生文件。
    return !name.empty() && name.front() == '_' && IsMp3(name);
}

bool IsValidUtf8(const uint8_t* data, size_t length) {
    size_t i = 0;
    while (i < length) {
        const uint8_t value = data[i];
        if (value <= 0x7f) {
            ++i;
            continue;
        }
        size_t count = 0;
        uint32_t codepoint = 0;
        if (value >= 0xc2 && value <= 0xdf) {
            count = 2;
            codepoint = value & 0x1f;
        } else if (value >= 0xe0 && value <= 0xef) {
            count = 3;
            codepoint = value & 0x0f;
        } else if (value >= 0xf0 && value <= 0xf4) {
            count = 4;
            codepoint = value & 0x07;
        } else {
            return false;
        }
        if (i + count > length) return false;
        for (size_t j = 1; j < count; ++j) {
            if ((data[i + j] & 0xc0) != 0x80) return false;
            codepoint = (codepoint << 6) | (data[i + j] & 0x3f);
        }
        if ((count == 3 && codepoint < 0x800) ||
            (count == 4 && codepoint < 0x10000) ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
            codepoint > 0x10ffff) {
            return false;
        }
        i += count;
    }
    return true;
}

void AppendUtf8(std::string& output, uint32_t codepoint) {
    if (codepoint <= 0x7f) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ff) {
        output.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0xffff) {
        output.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0x10ffff) {
        output.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
}

std::string TrimText(std::string value) {
    while (!value.empty() &&
           (value.back() == '\0' || value.back() == ' ' || value.back() == '\t' ||
            value.back() == '\r' || value.back() == '\n')) {
        value.pop_back();
    }
    size_t begin = 0;
    while (begin < value.size() &&
           (value[begin] == '\0' || value[begin] == ' ' || value[begin] == '\t' ||
            value[begin] == '\r' || value[begin] == '\n')) {
        ++begin;
    }
    if (begin != 0) value.erase(0, begin);
    return value;
}

bool DecodeGbk(const uint8_t* data, size_t length, std::string& output) {
    output.clear();
    output.reserve(length);
    bool converted_non_ascii = false;
    for (size_t i = 0; i < length && data[i] != 0;) {
        const uint8_t value = data[i];
        if (value < 0x80) {
            output.push_back(static_cast<char>(value));
            ++i;
            continue;
        }
        // GBK 是双字节编码；交给 FatFs 的 CP936 表校验并转成 Unicode。
        if (i + 1 >= length || data[i + 1] == 0) return false;
        const WORD gbk = static_cast<WORD>((static_cast<WORD>(value) << 8) |
                                           data[i + 1]);
        const WCHAR codepoint = ff_oem2uni(gbk, 936);
        if (codepoint == 0) return false;
        AppendUtf8(output, codepoint);
        converted_non_ascii = true;
        i += 2;
    }
    return converted_non_ascii;
}

std::string DecodeLegacyText(const uint8_t* data, size_t length) {
    if (IsValidUtf8(data, length)) {
        return TrimText(std::string(reinterpret_cast<const char*>(data), length));
    }

    std::string gbk;
    if (DecodeGbk(data, length, gbk)) return TrimText(std::move(gbk));

    // 少数文件使用 ISO-8859-1；保留原有回退，避免丢失西欧字符。
    std::string output;
    output.reserve(length * 2);
    for (size_t i = 0; i < length && data[i] != 0; ++i) {
        AppendUtf8(output, data[i]);
    }
    return TrimText(std::move(output));
}

std::string DecodeUtf16(const uint8_t* data, size_t length, bool big_endian) {
    std::string output;
    size_t offset = 0;
    if (length >= 2) {
        if (data[0] == 0xff && data[1] == 0xfe) {
            big_endian = false;
            offset = 2;
        } else if (data[0] == 0xfe && data[1] == 0xff) {
            big_endian = true;
            offset = 2;
        }
    }
    auto read_word = [data, big_endian](size_t pos) -> uint16_t {
        return big_endian
                   ? static_cast<uint16_t>((data[pos] << 8) | data[pos + 1])
                   : static_cast<uint16_t>(data[pos] | (data[pos + 1] << 8));
    };
    while (offset + 1 < length) {
        uint32_t codepoint = read_word(offset);
        offset += 2;
        if (codepoint == 0) break;
        if (codepoint >= 0xd800 && codepoint <= 0xdbff && offset + 1 < length) {
            const uint16_t low = read_word(offset);
            if (low >= 0xdc00 && low <= 0xdfff) {
                offset += 2;
                codepoint = 0x10000 + ((codepoint - 0xd800) << 10) +
                            (low - 0xdc00);
            }
        }
        AppendUtf8(output, codepoint);
    }
    return TrimText(std::move(output));
}

std::string DecodeId3Text(const uint8_t* data, size_t length) {
    if (data == nullptr || length <= 1) return {};
    const uint8_t encoding = data[0];
    ++data;
    --length;
    if (encoding == 1) return DecodeUtf16(data, length, false);
    if (encoding == 2) return DecodeUtf16(data, length, true);
    if (encoding == 3) {
        if (IsValidUtf8(data, length)) {
            return TrimText(std::string(reinterpret_cast<const char*>(data), length));
        }
        // 一些播放器错误地把 GBK 内容标记为 UTF-8，仍尝试按旧编码恢复。
        return DecodeLegacyText(data, length);
    }
    return DecodeLegacyText(data, length);
}

std::string TruncateUtf8(std::string value, size_t max_bytes) {
    if (value.size() <= max_bytes) return value;
    size_t offset = 0;
    size_t valid_end = 0;
    while (offset < value.size() && offset < max_bytes) {
        const uint8_t first = static_cast<uint8_t>(value[offset]);
        size_t length = 1;
        if (first >= 0xc2 && first <= 0xdf) length = 2;
        else if (first >= 0xe0 && first <= 0xef) length = 3;
        else if (first >= 0xf0 && first <= 0xf4) length = 4;
        if (offset + length > max_bytes || offset + length > value.size()) break;
        offset += length;
        valid_end = offset;
    }
    value.resize(valid_end);
    return value;
}

uint32_t ReadBigEndian32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) | data[3];
}

uint32_t ReadSyncSafe32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0] & 0x7f) << 21) |
           (static_cast<uint32_t>(data[1] & 0x7f) << 14) |
           (static_cast<uint32_t>(data[2] & 0x7f) << 7) | (data[3] & 0x7f);
}

void AssignTag(const char* id, std::string value, SdMusicTrack& track) {
    if (value.empty()) return;
    if (std::strcmp(id, "TIT2") == 0 || std::strcmp(id, "TT2") == 0) {
        track.title = TruncateUtf8(std::move(value), kMaxTitleBytes);
    } else if (std::strcmp(id, "TPE1") == 0 || std::strcmp(id, "TP1") == 0) {
        track.artist = TruncateUtf8(std::move(value), kMaxArtistBytes);
    } else if (std::strcmp(id, "TALB") == 0 || std::strcmp(id, "TAL") == 0) {
        track.album = TruncateUtf8(std::move(value), kMaxAlbumBytes);
    }
}

void ReadId3v2(FILE* file, SdMusicTrack& track) {
    std::array<uint8_t, 10> header{};
    if (std::fseek(file, 0, SEEK_SET) != 0 ||
        std::fread(header.data(), 1, header.size(), file) != header.size() ||
        std::memcmp(header.data(), "ID3", 3) != 0) {
        return;
    }
    const uint8_t version = header[3];
    if (version < 2 || version > 4) return;
    const uint32_t tag_size = ReadSyncSafe32(header.data() + 6);
    uint32_t consumed = 0;
    while (consumed < tag_size) {
        std::array<uint8_t, 10> frame_header{};
        const size_t header_size = version == 2 ? 6 : 10;
        if (tag_size - consumed < header_size ||
            std::fread(frame_header.data(), 1, header_size, file) != header_size) {
            break;
        }
        consumed += header_size;
        const size_t id_size = version == 2 ? 3 : 4;
        if (frame_header[0] == 0) break;
        char id[5] = {};
        std::memcpy(id, frame_header.data(), id_size);
        const uint32_t frame_size = version == 2
                                        ? (static_cast<uint32_t>(frame_header[3]) << 16) |
                                              (static_cast<uint32_t>(frame_header[4]) << 8) |
                                              frame_header[5]
                                        : (version == 4
                                               ? ReadSyncSafe32(frame_header.data() + 4)
                                               : ReadBigEndian32(frame_header.data() + 4));
        if (frame_size == 0 || frame_size > tag_size - consumed) break;
        const bool wanted = std::strcmp(id, "TIT2") == 0 ||
                            std::strcmp(id, "TPE1") == 0 ||
                            std::strcmp(id, "TALB") == 0 ||
                            std::strcmp(id, "TT2") == 0 ||
                            std::strcmp(id, "TP1") == 0 ||
                            std::strcmp(id, "TAL") == 0;
        if (wanted && frame_size <= kMaxTextFrameBytes) {
            std::vector<uint8_t> payload(frame_size);
            if (std::fread(payload.data(), 1, frame_size, file) != frame_size) break;
            AssignTag(id, DecodeId3Text(payload.data(), payload.size()), track);
        } else if (std::fseek(file, static_cast<long>(frame_size), SEEK_CUR) != 0) {
            break;
        }
        consumed += frame_size;
        if (!track.title.empty() && !track.artist.empty() && !track.album.empty()) break;
    }
}

void ReadId3v1(FILE* file, SdMusicTrack& track) {
    std::array<uint8_t, 128> tag{};
    if (std::fseek(file, -static_cast<long>(tag.size()), SEEK_END) != 0 ||
        std::fread(tag.data(), 1, tag.size(), file) != tag.size() ||
        std::memcmp(tag.data(), "TAG", 3) != 0) {
        return;
    }
    if (track.title.empty()) {
        track.title = TruncateUtf8(DecodeLegacyText(tag.data() + 3, 30),
                                   kMaxTitleBytes);
    }
    if (track.artist.empty()) {
        track.artist = TruncateUtf8(DecodeLegacyText(tag.data() + 33, 30),
                                    kMaxArtistBytes);
    }
    if (track.album.empty()) {
        track.album = TruncateUtf8(DecodeLegacyText(tag.data() + 63, 30),
                                   kMaxAlbumBytes);
    }
}

void ReadMetadata(SdMusicTrack& track) {
    FILE* file = std::fopen(track.path.c_str(), "rb");
    if (file == nullptr) {
        ESP_LOGW(kTag, "无法读取 MP3 元数据: %s, errno=%d",
                 track.path.c_str(), errno);
        return;
    }
    ReadId3v2(file, track);
    ReadId3v1(file, track);
    std::fclose(file);
}

std::string TitleFromFileName(const std::string& file_name) {
    return TruncateUtf8(IsMp3(file_name)
                            ? file_name.substr(0, file_name.size() - 4)
                            : file_name,
                        kMaxTitleBytes);
}

struct ScanContext {
    size_t max_total = 0;
    bool read_metadata = false;
    SdMusicScanner::BatchCallback callback;
    const std::atomic_bool* cancelled = nullptr;
    SdMusicScanStats stats;
    std::vector<SdMusicTrack> batch;
    esp_err_t result = ESP_OK;
    bool stopped = false;
};

bool IsCancelled(const ScanContext& context) {
    return context.cancelled != nullptr && context.cancelled->load();
}

esp_err_t Flush(ScanContext& context, bool is_last) {
    if (context.batch.empty() && !is_last) return ESP_OK;
    const esp_err_t result = context.callback(context.batch, is_last);
    context.batch.clear();
    if (result != ESP_OK) {
        context.result = result;
        context.stopped = true;
    }
    return result;
}

uint32_t CountMp3Under(const std::string& path, const std::atomic_bool* cancelled) {
    if (cancelled != nullptr && cancelled->load()) return 0;
    DIR* directory = opendir(path.c_str());
    if (directory == nullptr) return 0;
    uint32_t count = 0;
    while (dirent* entry = readdir(directory)) {
        const std::string name(entry->d_name);
        if (name == "." || name == "..") continue;
        const std::string child = path + "/" + name;
        struct stat info = {};
        if (stat(child.c_str(), &info) != 0) continue;
        if (S_ISDIR(info.st_mode) && !ShouldIgnoreDirectory(name)) {
            count += CountMp3Under(child, cancelled);
        } else if (S_ISREG(info.st_mode) && !ShouldIgnoreFile(name) && IsMp3(name)) {
            ++count;
        }
    }
    closedir(directory);
    return count;
}

// 默认扫描范围是 /sdcard/audio。为了让完成页能说明为什么没有上报，
// 额外统计 audio 目录之外的 MP3，但这些文件仍然不会加入上报批次。
uint32_t CountMp3OutsideAudio(const std::string& path,
                              const std::atomic_bool* cancelled) {
    if (cancelled != nullptr && cancelled->load()) return 0;
    DIR* directory = opendir(path.c_str());
    if (directory == nullptr) return 0;
    uint32_t count = 0;
    while (dirent* entry = readdir(directory)) {
        const std::string name(entry->d_name);
        if (name == "." || name == "..") continue;
        const std::string child = path + "/" + name;
        struct stat info = {};
        if (stat(child.c_str(), &info) != 0) continue;
        if (S_ISDIR(info.st_mode)) {
            if (ShouldIgnoreDirectory(name) ||
                (path == "/sdcard" && name == "audio")) {
                continue;
            }
            count += CountMp3OutsideAudio(child, cancelled);
        } else if (S_ISREG(info.st_mode) && !ShouldIgnoreFile(name) &&
                   IsMp3(name)) {
            ++count;
        }
    }
    closedir(directory);
    return count;
}

bool ScanDirectory(ScanContext& context, const std::string& path, int depth) {
    if (context.stopped || IsCancelled(context)) return false;
    DIR* directory = opendir(path.c_str());
    if (directory == nullptr) {
        return false;
    }
    int read_error = 0;
    while (!context.stopped && !IsCancelled(context)) {
        // 目录项读取前清除 errno。扫描目录时会嵌套执行 stat/fopen 等操作，
        // 它们可能留下与 readdir 无关的 errno，不能据此判定目录读取失败。
        errno = 0;
        dirent* entry = readdir(directory);
        if (entry == nullptr) {
            read_error = errno;
            break;
        }
        const std::string name(entry->d_name);
        if (name == "." || name == "..") continue;
        if (ShouldIgnoreDirectory(name) || ShouldIgnoreFile(name)) continue;
        if (path.size() + 1 + name.size() >= kMaxPathBytes) {
            if (IsMp3(name)) ++context.stats.skipped_count;
            else ++context.stats.unknown_skip_count;
            continue;
        }
        const std::string child = path + "/" + name;
        struct stat info = {};
        if (stat(child.c_str(), &info) != 0) {
            ESP_LOGW(kTag, "无法读取文件信息: %s, errno=%d", child.c_str(), errno);
            context.result = ESP_FAIL;
            context.stopped = true;
            break;
        }
        if (S_ISDIR(info.st_mode)) {
            if (depth >= kMaxDirectoryDepth || name.size() > kMaxDirectoryNameBytes) {
                const uint32_t skipped = CountMp3Under(child, context.cancelled);
                context.stats.skipped_count += skipped;
                if (skipped == 0) ++context.stats.unknown_skip_count;
                ESP_LOGW(kTag, "跳过目录: depth=%d, name=%s, mp3=%lu",
                         depth + 1, child.c_str(),
                         static_cast<unsigned long>(skipped));
                continue;
            }
            if (!ScanDirectory(context, child, depth + 1)) {
                ESP_LOGW(kTag, "无法扫描目录: %s, errno=%d", child.c_str(), errno);
                context.result = ESP_FAIL;
                context.stopped = true;
            }
            continue;
        }
        if (!S_ISREG(info.st_mode) || !IsMp3(name)) continue;
        if (name.size() > kMaxFileNameBytes) {
            ++context.stats.skipped_count;
            ESP_LOGW(kTag, "跳过文件名过长的 MP3: %s", child.c_str());
            continue;
        }
        SdMusicTrack track{
            .path = child,
            .file_name = name,
        };
        if (context.read_metadata) ReadMetadata(track);
        if (track.title.empty()) track.title = TitleFromFileName(name);
        context.batch.push_back(std::move(track));
        ++context.stats.reported_count;
        const bool at_limit = context.max_total != 0 &&
                              context.stats.reported_count >= context.max_total;
        if (context.batch.size() >= SdMusicScanner::kBatchSize || at_limit) {
            Flush(context, at_limit);
        }
        if (at_limit) context.stopped = true;
    }
    closedir(directory);
    if (!context.stopped && !IsCancelled(context) && read_error != 0) {
        ESP_LOGW(kTag, "读取目录失败: %s, errno=%d", path.c_str(), read_error);
        context.result = ESP_FAIL;
        context.stopped = true;
    }
    errno = read_error;
    return true;
}

}  // namespace

esp_err_t SdMusicScanner::Scan(const std::string& path, size_t max_total,
                               bool read_metadata, BatchCallback callback,
                               SdMusicScanStats* stats,
                               const std::atomic_bool* cancelled) {
    if (!callback) return ESP_ERR_INVALID_ARG;
    ScanContext context{
        .max_total = max_total,
        .read_metadata = read_metadata,
        .callback = std::move(callback),
        .cancelled = cancelled,
    };
    context.batch.reserve(kBatchSize);
    const bool use_default = path.empty();
    const std::string first_path = use_default ? kDefaultMusicDirectory : path;
    const bool first_opened = ScanDirectory(context, first_path, 0);
    // 服务会先做一遍不读取元数据的计数扫描。只在这一遍统计 audio
    // 目录之外的文件，避免本地预览和实际上传扫描重复遍历整张卡。
    if (use_default && !read_metadata && !IsCancelled(context)) {
        const uint32_t outside_audio_count =
            CountMp3OutsideAudio("/sdcard", cancelled);
        context.stats.skipped_count += outside_audio_count;
        if (outside_audio_count != 0) {
            ESP_LOGI(kTag, "跳过 audio 目录之外的 MP3 文件: %lu",
                     static_cast<unsigned long>(outside_audio_count));
        }
    }
    if (!first_opened) {
        if (use_default) {
            // audio 目录不是必选项。目录不存在时按空列表处理，卡上其他目录
            // 的 MP3 已计入 skipped_count，但不会带入音乐库。
            ESP_LOGI(kTag, "未找到默认音乐目录，跳过目录外文件，仅扫描 %s",
                     first_path.c_str());
        } else {
            ESP_LOGW(kTag, "无法打开扫描目录: %s, errno=%d", first_path.c_str(), errno);
            context.result = ESP_FAIL;
            context.stopped = true;
        }
    }
    if (IsCancelled(context)) {
        context.result = ESP_ERR_INVALID_STATE;
    } else if (!context.stopped) {
        Flush(context, true);
    }
    if (stats != nullptr) *stats = context.stats;
    ESP_LOGI(kTag, "MP3 扫描结束: result=%s, files=%lu, skipped=%lu, unknown=%lu",
             esp_err_to_name(context.result),
             static_cast<unsigned long>(context.stats.reported_count),
             static_cast<unsigned long>(context.stats.skipped_count),
             static_cast<unsigned long>(context.stats.unknown_skip_count));
    return context.result;
}
