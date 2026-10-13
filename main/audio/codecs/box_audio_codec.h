#ifndef _BOX_AUDIO_CODEC_H
#define _BOX_AUDIO_CODEC_H

#include "audio_codec.h"

#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>
#include <mutex>
#include <atomic>


class BoxAudioCodec : public AudioCodec {
private:
    const audio_codec_data_if_t* data_if_ = nullptr;
    const audio_codec_ctrl_if_t* out_ctrl_if_ = nullptr;
    const audio_codec_if_t* out_codec_if_ = nullptr;
    const audio_codec_ctrl_if_t* in_ctrl_if_ = nullptr;
    const audio_codec_if_t* in_codec_if_ = nullptr;
    const audio_codec_gpio_if_t* gpio_if_ = nullptr;

    esp_codec_dev_handle_t output_dev_ = nullptr;
    esp_codec_dev_handle_t input_dev_ = nullptr;
    std::mutex data_if_mutex_;
    // I2S0 只有一组 BOX 双工通道；ISR 统计放内部 RAM，不访问 PSRAM 对象。
    static std::atomic<uint32_t> input_overflows_;
    static bool IRAM_ATTR OnInputOverflow(i2s_chan_handle_t handle,
                                         i2s_event_data_t* event, void* user_data);

    void CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din);

    virtual int Read(int16_t* dest, int samples) override;
    virtual int Write(const int16_t* data, int samples) override;

public:
    BoxAudioCodec(void* i2c_master_handle, int input_sample_rate, int output_sample_rate,
        gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
        gpio_num_t pa_pin, uint8_t es8311_addr, uint8_t es7210_addr, bool input_reference,
        float input_gain = 30.0f);
    virtual ~BoxAudioCodec();

    virtual void SetOutputVolume(int volume) override;
    virtual void EnableInput(bool enable) override;
    virtual void EnableOutput(bool enable) override;
    virtual int64_t input_overflow_count() const override {
        return input_overflows_.load(std::memory_order_relaxed);
    }
    // ESP-BOX-3 gates the microphone in hardware. Re-open the ES7210 after the
    // mute latch is released so recording resumes without a board reset.
    bool ReinitializeInput();
    virtual size_t OutputDmaBufferBytes() const override {
        return AUDIO_CODEC_DMA_FRAME_NUM * 2 * sizeof(int32_t);
    }
};

#endif // _BOX_AUDIO_CODEC_H
