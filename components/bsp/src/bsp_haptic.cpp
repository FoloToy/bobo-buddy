#include "bsp_haptic.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "port_aw862xx.hpp"
#include <mutex>
static AW862XX driver(BSP_HAPTIC_RESET);
static bool ready;
static std::mutex guard;
esp_err_t bsp_haptic_init() {
    std::lock_guard<std::mutex> lock(guard);
    if (ready)
        return ESP_OK;
    auto e = bsp_i2c_init();
    if (e != ESP_OK)
        return e;
    e = driver.init(BSP_HAPTIC_ADDR, 0, BSP_I2C_PORT);
    ready = e == ESP_OK;
    return e;
}
esp_err_t bsp_haptic_play(uint8_t waveform, uint8_t gain, uint32_t duration) {
    std::lock_guard<std::mutex> lock(guard);
    if (!ready)
        return ESP_ERR_INVALID_STATE;
    if (!waveform || !duration || duration > 1000)
        return ESP_ERR_INVALID_ARG;
    return driver.longVibWork(waveform, gain, duration) == 0 ? ESP_OK : ESP_FAIL;
}
