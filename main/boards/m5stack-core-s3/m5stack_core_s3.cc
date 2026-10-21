#include "wifi_board.h"
#include "cores3_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "config.h"
#include "power_save_timer.h"
#include "i2c_device.h"
#include "axp2101.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_ili9341.h>
#include <esp_timer.h>
#include "esp32_camera.h"
#include "system/spi_bus_arbiter.h"

#define TAG "M5StackCoreS3Board"

namespace {

// AXP2101 的电源键中断使能和状态寄存器。状态位写 1 清除。
constexpr uint8_t kAxp2101IrqEnable1Reg = 0x41;
constexpr uint8_t kAxp2101IrqStatus1Reg = 0x49;
constexpr uint8_t kPowerKeyLongPressMask = 1 << 2;
constexpr uint8_t kPowerKeyShortPressMask = 1 << 3;
constexpr uint8_t kPowerKeyEventMask = kPowerKeyLongPressMask | kPowerKeyShortPressMask;

// 等待后续短按以识别双击、多击；Application 会根据最终击次执行业务。
constexpr int64_t kPowerKeyMultiClickTimeoutUs = 400 * 1000;
constexpr uint64_t kPowerKeyPollPeriodUs = 50 * 1000;

struct Gc0308Register {
    uint8_t address;
    uint8_t value;
};

// GC0308 QQVGA 保留完整的 640x480 视场，并由传感器从每个 4x4 像素块
// 输出一个像素。基础 QVGA 配置负责时序、AE/AWB 和 YUYV，此处只调整
// 输入窗口与传感器抽样阶段。
constexpr Gc0308Register kGc0308QqvgaRegisters[] = {
    {0xfe, 0x00},  // 切换到第 0 页寄存器
    {0x05, 0x00},  // 输入窗口行起点高字节
    {0x06, 0x00},  // 输入窗口行起点低字节
    {0x07, 0x00},  // 输入窗口列起点高字节
    {0x08, 0x00},  // 输入窗口列起点低字节
    {0x09, 0x01},  // 输入窗口高度为 480 + 8
    {0x0a, 0xe8},
    {0x0b, 0x02},  // 输入窗口宽度为 640 + 8
    {0x0c, 0x88},
    {0xfe, 0x01},  // 切换到第 1 页寄存器
    {0x53, 0x83},  // 启用主抽样路径
    {0x54, 0x44},  // 水平和垂直方向均按 1/4 抽样
    {0x55, 0x03},  // 启用辅助抽样路径
    {0x56, 0x00},
    {0x57, 0x00},
    {0x58, 0x00},
    {0x59, 0x00},
    {0xfe, 0x00},  // 返回第 0 页寄存器
};

const esp_cam_sensor_format_t kGc0308QqvgaFormat = {
    .name = "GC0308 YUYV 160x120 sensor subsample",
    .format = ESP_CAM_SENSOR_PIXFORMAT_YUV422,
    .port = ESP_CAM_SENSOR_DVP,
    .xclk = XCLK_FREQ_HZ,
    .width = 160,
    .height = 120,
    .regs = kGc0308QqvgaRegisters,
    .regs_size = static_cast<int>(std::size(kGc0308QqvgaRegisters)),
    .fps = 20,
    .isp_info = nullptr,
    .mipi_info = {},
    .reserved = nullptr,
};

}  // namespace

class Pmic : public Axp2101 {
public:
    // Power Init
    Pmic(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : Axp2101(i2c_bus, addr) {
        uint8_t data = ReadReg(0x90);
        data |= 0b10110100;
        WriteReg(0x90, data);
        WriteReg(0x99, (0b11110 - 5));
        WriteReg(0x97, (0b11110 - 2));
        WriteReg(0x69, 0b00110101);
        WriteReg(0x30, 0b111111);
        WriteReg(0x90, 0xBF);
        WriteReg(0x94, 33 - 5);
        WriteReg(0x95, 33 - 5);
    }

    void EnablePowerKeyEvents() {
        // 先清除上电过程中残留的按键状态，避免启动完成后误触发一次短按。
        WriteReg(kAxp2101IrqStatus1Reg, kPowerKeyEventMask);
        const uint8_t irq_enable = ReadReg(kAxp2101IrqEnable1Reg);
        WriteReg(kAxp2101IrqEnable1Reg, irq_enable | kPowerKeyEventMask);
    }

    uint8_t ReadAndClearPowerKeyEvents() {
        const uint8_t events = ReadReg(kAxp2101IrqStatus1Reg) & kPowerKeyEventMask;
        if (events != 0) {
            WriteReg(kAxp2101IrqStatus1Reg, events);
        }
        return events;
    }

    void SetBrightness(uint8_t brightness) {
        brightness = ((brightness + 641) >> 5);
        WriteReg(0x99, brightness);
    }
};

class CustomBacklight : public Backlight {
public:
    CustomBacklight(Pmic *pmic) : pmic_(pmic) {}

    void SetBrightnessImpl(uint8_t brightness) override {
        pmic_->SetBrightness(target_brightness_);
        brightness_ = target_brightness_;
    }

private:
    Pmic *pmic_;
};

class Aw9523 : public I2cDevice {
public:
    // Exanpd IO Init
    Aw9523(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
        WriteReg(0x02, 0b00000111);  // P0
        WriteReg(0x03, 0b10001111);  // P1
        WriteReg(0x04, 0b00011000);  // CONFIG_P0
        WriteReg(0x05, 0b00001100);  // CONFIG_P1
        WriteReg(0x11, 0b00010000);  // GCR P0 port is Push-Pull mode.
        WriteReg(0x12, 0b11111111);  // LEDMODE_P0
        WriteReg(0x13, 0b11111111);  // LEDMODE_P1
    }

    void ResetAw88298() {
        ESP_LOGI(TAG, "Reset AW88298");
        WriteReg(0x02, 0b00000011);
        vTaskDelay(pdMS_TO_TICKS(10));
        WriteReg(0x02, 0b00000111);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    void ResetIli9342() {
        ESP_LOGI(TAG, "Reset IlI9342");
        WriteReg(0x03, 0b10000001);
        vTaskDelay(pdMS_TO_TICKS(20));
        WriteReg(0x03, 0b10000011);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
};

class Ft6336 : public I2cDevice {
public:
    struct TouchPoint_t {
        int num = 0;
        int x = -1;
        int y = -1;
    };
    
    Ft6336(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
        uint8_t chip_id = ReadReg(0xA3);
        ESP_LOGI(TAG, "Get chip ID: 0x%02X", chip_id);
        read_buffer_ = new uint8_t[6];
    }

    ~Ft6336() {
        delete[] read_buffer_;
    }

    void UpdateTouchPoint() {
        ReadRegs(0x02, read_buffer_, 6);
        tp_.num = read_buffer_[0] & 0x0F;
        tp_.x = ((read_buffer_[1] & 0x0F) << 8) | read_buffer_[2];
        tp_.y = ((read_buffer_[3] & 0x0F) << 8) | read_buffer_[4];
    }

    inline const TouchPoint_t& GetTouchPoint() {
        return tp_;
    }

private:
    uint8_t* read_buffer_ = nullptr;
    TouchPoint_t tp_;
};

class M5StackCoreS3Board : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Pmic* pmic_;
    Aw9523* aw9523_;
    Ft6336* ft6336_;
    LcdDisplay* display_;
    Esp32Camera* camera_;
    esp_timer_handle_t touchpad_timer_;
    esp_timer_handle_t power_key_timer_;
    PowerSaveTimer* power_save_timer_;
    uint8_t pending_power_key_clicks_ = 0;
    int64_t last_power_key_click_time_us_ = 0;
    SpiBusArbiter spi_bus_arbiter_;

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(-1, 60, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(10);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->OnShutdownRequest([this]() {
            pmic_->PowerOff();
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void I2cDetect() {
        uint8_t address;
        printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\r\n");
        for (int i = 0; i < 128; i += 16) {
            printf("%02x: ", i);
            for (int j = 0; j < 16; j++) {
                fflush(stdout);
                address = i + j;
                esp_err_t ret = i2c_master_probe(i2c_bus_, address, pdMS_TO_TICKS(200));
                if (ret == ESP_OK) {
                    printf("%02x ", address);
                } else if (ret == ESP_ERR_TIMEOUT) {
                    printf("UU ");
                } else {
                    printf("-- ");
                }
            }
            printf("\r\n");
        }
    }

    void InitializeAxp2101() {
        ESP_LOGI(TAG, "Init AXP2101");
        pmic_ = new Pmic(i2c_bus_, 0x34);
    }

    void PollPowerKey() {
        const uint8_t events = pmic_->ReadAndClearPowerKeyEvents();

        // 某些按压过程可能同时留下多个状态位，长按优先，避免再触发短按业务。
        if ((events & kPowerKeyLongPressMask) != 0) {
            pending_power_key_clicks_ = 0;
            ESP_LOGI(TAG, "Power key long press");
            Application::GetInstance().HandleButtonLongPress();
            return;
        }

        if ((events & kPowerKeyShortPressMask) != 0) {
            // Application 当前只区分 1~5 击和 8 击以上，饱和到 8 可避免溢出。
            if (pending_power_key_clicks_ < 8) {
                ++pending_power_key_clicks_;
            }
            last_power_key_click_time_us_ = esp_timer_get_time();
        }

        if (pending_power_key_clicks_ == 0 ||
            esp_timer_get_time() - last_power_key_click_time_us_ < kPowerKeyMultiClickTimeoutUs) {
            return;
        }

        const uint8_t click_count = pending_power_key_clicks_;
        pending_power_key_clicks_ = 0;
        ESP_LOGI(TAG, "Power key click count: %u", static_cast<unsigned>(click_count));
        Application::GetInstance().HandleButtonClick(click_count);
    }

    void InitializePowerKey() {
        pmic_->EnablePowerKeyEvents();
        const esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                static_cast<M5StackCoreS3Board*>(arg)->PollPowerKey();
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "power_key_timer",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &power_key_timer_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(power_key_timer_, kPowerKeyPollPeriodUs));
        ESP_LOGI(TAG, "AXP2101 power key enabled");
    }

    void InitializeAw9523() {
        ESP_LOGI(TAG, "Init AW9523");
        aw9523_ = new Aw9523(i2c_bus_, 0x58);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    void PollTouchpad() {
        static bool was_touched = false;
        static int64_t touch_start_time = 0;
        const int64_t TOUCH_THRESHOLD_MS = 500;  // 触摸时长阈值，超过500ms视为长按
        
        ft6336_->UpdateTouchPoint();
        auto& touch_point = ft6336_->GetTouchPoint();
        
        // 检测触摸开始
        if (touch_point.num > 0 && !was_touched) {
            was_touched = true;
            touch_start_time = esp_timer_get_time() / 1000; // 转换为毫秒
        } 
        // 检测触摸释放
        else if (touch_point.num == 0 && was_touched) {
            was_touched = false;
            int64_t touch_duration = (esp_timer_get_time() / 1000) - touch_start_time;
            
            // 只有短触才触发
            if (touch_duration < TOUCH_THRESHOLD_MS) {
                // auto& app = Application::GetInstance();
                // if (app.GetDeviceState() == kDeviceStateStarting) {
                //     EnterWifiConfigMode();
                //     return;
                // }
                // app.ToggleChatState();
            }
        }
    }

    void InitializeFt6336TouchPad() {
        ESP_LOGI(TAG, "Init FT6336");
        ft6336_ = new Ft6336(i2c_bus_, 0x38);
        
        // 创建定时器，20ms 间隔
        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                M5StackCoreS3Board* board = (M5StackCoreS3Board*)arg;
                board->PollTouchpad();
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "touchpad_timer",
            .skip_unhandled_events = true,
        };
        
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &touchpad_timer_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(touchpad_timer_, 20 * 1000));
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = GPIO_NUM_37;
        buscfg.miso_io_num = GPIO_NUM_35;
        buscfg.sclk_io_num = GPIO_NUM_36;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeIli9342Display() {
        ESP_LOGI(TAG, "Init IlI9342");

        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = GPIO_NUM_3;
        io_config.dc_gpio_num = GPIO_NUM_35;
        io_config.spi_mode = 2;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));
        
        esp_lcd_panel_reset(panel);
        aw9523_->ResetIli9342();

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, true);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

        display_ = new SpiLcdDisplay(panel_io, panel,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY,
                                    &spi_bus_arbiter_);
    }

     void InitializeCamera() {
        static esp_cam_ctlr_dvp_pin_config_t dvp_pin_config = {
            .data_width = CAM_CTLR_DATA_WIDTH_8,
            .data_io = {
                [0] = CAMERA_PIN_D0,
                [1] = CAMERA_PIN_D1,
                [2] = CAMERA_PIN_D2,
                [3] = CAMERA_PIN_D3,
                [4] = CAMERA_PIN_D4,
                [5] = CAMERA_PIN_D5,
                [6] = CAMERA_PIN_D6,
                [7] = CAMERA_PIN_D7,
            },
            .vsync_io = CAMERA_PIN_VSYNC,
            .de_io = CAMERA_PIN_HREF,
            .pclk_io = CAMERA_PIN_PCLK,
            .xclk_io = CAMERA_PIN_XCLK,
        };

        esp_video_init_sccb_config_t sccb_config = {
            .init_sccb = false,
            .i2c_handle = i2c_bus_,
            .freq = 100000,
        };

        esp_video_init_dvp_config_t dvp_config = {
            .sccb_config = sccb_config,
            .reset_pin = CAMERA_PIN_RESET,
            .pwdn_pin = CAMERA_PIN_PWDN,
            .dvp_pin = dvp_pin_config,
            .xclk_freq = XCLK_FREQ_HZ,
        };

        esp_video_init_config_t video_config = {
            .dvp = &dvp_config,
        };

        camera_ = new Esp32Camera(video_config, &kGc0308QqvgaFormat);
        const bool mirror_set = camera_->SetHMirror(CAMERA_H_MIRROR);
        const bool flip_set = camera_->SetVFlip(CAMERA_V_FLIP);
        if (!mirror_set || !flip_set) {
            ESP_LOGW(TAG, "Failed to configure camera mirror/flip registers");
        }
    }

public:
    M5StackCoreS3Board() {
        InitializePowerSaveTimer();
        InitializeI2c();
        InitializeAxp2101();
        InitializePowerKey();
        InitializeAw9523();
        I2cDetect();
        InitializeSpi();
        InitializeIli9342Display();
        InitializeCamera();
        InitializeFt6336TouchPad();
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static CoreS3AudioCodec audio_codec(i2c_bus_,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_AW88298_ADDR,
            AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual const SdSpiCardConfig* GetSdCardConfig() override {
        static const SdSpiCardConfig config = {
            .host = SD_CARD_SPI_HOST,
            .mosi = SD_CARD_SPI_MOSI,
            .miso = SD_CARD_SPI_MISO,
            .sclk = SD_CARD_SPI_SCLK,
            .cs = SD_CARD_SPI_CS,
            .power = SD_CARD_POWER,
            .power_on_level = 0,
            .bus_ownership = SdSpiBusOwnership::kExternallyOwned,
            .spi_bus_arbiter = &spi_bus_arbiter_,
        };
        return &config;
    }

    bool IsSdCardSpiSharedWithDisplay() const override { return true; }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }

    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging) override {
        static bool last_discharging = false;
        // AXP2101 会将充电完成单独报告，不再保持正在充电状态。
        // 电池充满但仍连接 USB 电源时，继续显示充电闪电图标。
        charging = pmic_->IsCharging() || pmic_->IsChargingDone();
        discharging = pmic_->IsDischarging();
        if (discharging != last_discharging) {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }

        level = pmic_->GetBatteryLevel();
        return true;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }

    virtual Backlight *GetBacklight() override {
        static CustomBacklight backlight(pmic_);
        return &backlight;
    }
};

DECLARE_BOARD(M5StackCoreS3Board);
