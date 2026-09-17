#ifndef _SYSTEM_RESET_H
#define _SYSTEM_RESET_H

#include <driver/gpio.h>

class SystemReset {
public:
    SystemReset(gpio_num_t reset_nvs_pin, gpio_num_t reset_factory_pin); // 构造函数私有化
    void CheckButtons();
    static void FactoryReset();

private:
    gpio_num_t reset_nvs_pin_;
    gpio_num_t reset_factory_pin_;

    static void ResetNvsFlash();
    static void ResetToFactory();
    static void RestartInSeconds(int seconds);
};


#endif
