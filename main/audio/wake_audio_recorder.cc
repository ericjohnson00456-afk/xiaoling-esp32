#include "wake_audio_recorder.h"

#include "wake_recording_wav.h"
#include "sd_card/sd_card_manager.h"

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {
constexpr char kTag[] = "WakeRecorder";
// Engineering buffer budget: two seconds of model PCM, not a guaranteed SD latency.
constexpr size_t kBufferSamples = wake_recording::kSampleRate * 2;
constexpr size_t kWriteSamples = 1024;  // Batch four 512-byte SD sectors per write.
}

WakeAudioRecorder& WakeAudioRecorder::GetInstance() {
    static WakeAudioRecorder instance;
    return instance;
}

bool WakeAudioRecorder::Start() {
    std::lock_guard<std::mutex> control(control_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (writer_running_) return false;
        error_ = nullptr;
        path_.clear();
        accepted_bytes_ = written_bytes_ = 0;
    }
    auto fail = [this](const char* error) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = error;
        return false;
    };
    auto& card = SdCardManager::GetInstance();
    if (!card_available_ || !card.IsConfigured() ||
        (!card.IsMounted() && card.Initialize() != ESP_OK)) {
        return fail("TF card is unavailable");
    }

    const std::string directory = std::string(card.MountPoint()) + "/recordings";
    if (mkdir(directory.c_str(), 0775) != 0 && errno != EEXIST) {
        return fail("Cannot create recordings directory");
    }
    auto* storage = static_cast<int16_t*>(heap_caps_malloc(
        kBufferSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (storage == nullptr) return fail("Cannot allocate recording buffer");

    std::string path;
    int fd = -1;
    // O_EXCL preserves earlier recordings, including after a reboot.
    while (next_file_number_ <= 99999) {
        char name[32];
        std::snprintf(name, sizeof(name), "/wake-%05lu.wav",
                      static_cast<unsigned long>(next_file_number_++));
        path = directory + name;
        fd = open(path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0666);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) {
        heap_caps_free(storage);
        return fail("Cannot create a new WAV file (card full or filename limit)");
    }
    FILE* file = fdopen(fd, "wb+");
    if (file == nullptr) {
        close(fd);
        unlink(path.c_str());
        heap_caps_free(storage);
        return fail("Cannot open WAV stream");
    }
    const auto header = wake_recording::WavHeader(0);
    if (std::fwrite(header.data(), 1, header.size(), file) != header.size() ||
        std::fflush(file) != 0) {
        std::fclose(file);
        unlink(path.c_str());
        heap_caps_free(storage);
        return fail("Cannot write WAV header");
    }

    std::unique_lock<std::mutex> lock(mutex_);
    path_ = path;
    file_ = file;
    storage_ = storage;
    buffer_.Reset(storage_, kBufferSamples);
    writer_running_ = true;
    recording_.store(true);
    if (xTaskCreate([](void* arg) {
            static_cast<WakeAudioRecorder*>(arg)->WriterLoop();
            vTaskDelete(nullptr);
        }, "wake_wav", 6144, this, tskIDLE_PRIORITY + 1, nullptr) != pdPASS) {
        recording_.store(false);
        writer_running_ = false;
        file_ = nullptr;
        storage_ = nullptr;
        buffer_.Reset(nullptr, 0);
        error_ = "Cannot start WAV writer task";
        lock.unlock();
        std::fclose(file);
        unlink(path.c_str());
        heap_caps_free(storage);
        return false;
    }
    lock.unlock();
    ESP_LOGI(kTag, "Recording model PCM: %s (16000 Hz, mono, s16)", path.c_str());
    return true;
}

void WakeAudioRecorder::Feed(const int16_t* pcm, size_t samples) {
    if (!recording_.load() || samples == 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!recording_.load()) return;
    if (samples > (wake_recording::kMaxDataBytes - accepted_bytes_) / sizeof(int16_t)) {
        error_ = "WAV/FAT32 size limit reached; recording stopped";
        recording_.store(false);
    } else if (!buffer_.Push(pcm, samples)) {
        // Never block model inference on a slow card or silently skip audio.
        error_ = "SD writer buffer full; recording stopped before losing PCM";
        recording_.store(false);
    } else {
        accepted_bytes_ += samples * sizeof(int16_t);
    }
    cv_.notify_all();
}

bool WakeAudioRecorder::StopLocked() {
    std::unique_lock<std::mutex> lock(mutex_);
    recording_.store(false);
    cv_.notify_all();
    cv_.wait(lock, [this]() { return !writer_running_; });
    return error_ == nullptr;
}

bool WakeAudioRecorder::Stop() {
    std::lock_guard<std::mutex> control(control_mutex_);
    return StopLocked();
}

void WakeAudioRecorder::SetCardAvailable(bool available) {
    std::lock_guard<std::mutex> control(control_mutex_);
    card_available_ = available;
    if (!available) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (writer_running_) error_ = "TF card removed; WAV may be incomplete";
        }
        StopLocked();  // Close the recording before SdCardManager unmounts FATFS.
    }
}

WakeAudioRecordingStatus WakeAudioRecorder::GetStatus() {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool recording = recording_.load();
    return {recording, writer_running_ && !recording, written_bytes_,
            buffer_.Size(), path_, error_ == nullptr ? "" : error_};
}

void WakeAudioRecorder::WriterLoop() {
    int16_t pcm[kWriteSamples];
    uint32_t data_bytes = 0;
    while (true) {
        size_t samples;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() {
                return !recording_.load() || buffer_.Size() >= kWriteSamples;
            });
            samples = buffer_.Pop(pcm, sizeof(pcm) / sizeof(pcm[0]));
            if (samples == 0 && !recording_.load()) break;
        }
        const size_t written = std::fwrite(pcm, sizeof(int16_t), samples, file_);
        data_bytes += written * sizeof(int16_t);
        std::lock_guard<std::mutex> lock(mutex_);
        written_bytes_ = data_bytes;
        if (written != samples) {
            recording_.store(false);
            error_ = "TF card PCM write failed (card full or removed)";
            break;
        }
    }

    // Try to salvage the contiguous prefix even when a previous write failed.
    std::clearerr(file_);
    const auto header = wake_recording::WavHeader(data_bytes);
    bool finalized = std::fflush(file_) == 0;
    if (std::fseek(file_, 0, SEEK_SET) != 0 ||
        std::fwrite(header.data(), 1, header.size(), file_) != header.size()) {
        finalized = false;
    }
    if (std::fclose(file_) != 0) finalized = false;

    std::lock_guard<std::mutex> lock(mutex_);
    file_ = nullptr;
    heap_caps_free(storage_);
    storage_ = nullptr;
    buffer_.Reset(nullptr, 0);
    if (!finalized) error_ = "WAV finalization failed; file may be incomplete";
    ESP_LOGI(kTag, "Recording finished: %s, bytes=%lu, result=%s", path_.c_str(),
             static_cast<unsigned long>(data_bytes), error_ == nullptr ? "ok" : error_);
    writer_running_ = false;
    cv_.notify_all();
}
