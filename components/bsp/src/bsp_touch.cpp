#include "bsp_touch.h"
#include "bsp_pins.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <atomic>
static std::atomic<bool> running{false};
static SemaphoreHandle_t stopped;
static std::atomic<bool> active{false};
static bsp_touch_callback_t callback;
static void *context;
static void worker(void *) {
    const gpio_num_t pins[] = {BSP_TOUCH_FRONT, BSP_TOUCH_BACK};
    bool stable[2] = {}, candidate[2] = {};
    unsigned count[2] = {};
    while (running) {
        for (int i = 0; i < 2; i++) {
            bool pressed = gpio_get_level(pins[i]) == 0;
            if (pressed != candidate[i]) {
                candidate[i] = pressed;
                count[i] = 1;
            } else if (count[i] < 3)
                count[i]++;
            if (count[i] >= 3 && stable[i] != pressed) {
                stable[i] = pressed;
                if (callback)
                    callback(static_cast<bsp_touch_pad_t>(i), pressed, context);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    active = false;
    xSemaphoreGive(stopped);
    vTaskDelete(nullptr);
}
esp_err_t bsp_touch_start(bsp_touch_callback_t cb, void *user) {
    if (!cb)
        return ESP_ERR_INVALID_ARG;
    if (active.exchange(true))
        return ESP_ERR_INVALID_STATE;
    running = true;
    if (stopped)
        xSemaphoreTake(stopped, 0);
    gpio_config_t config = {};
    config.pin_bit_mask = (1ULL << BSP_TOUCH_FRONT) | (1ULL << BSP_TOUCH_BACK);
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    auto e = gpio_config(&config);
    if (e != ESP_OK) {
        running = false;
        active = false;
        return e;
    }
    if (!stopped)
        stopped = xSemaphoreCreateBinary();
    if (!stopped) {
        running = false;
        active = false;
        return ESP_ERR_NO_MEM;
    }
    callback = cb;
    context = user;
    if (xTaskCreate(worker, "bsp_touch", 3072, nullptr, 3, nullptr) != pdPASS) {
        running = false;
        active = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
esp_err_t bsp_touch_stop() {
    if (!active)
        return ESP_OK;
    running = false;
    return xSemaphoreTake(stopped, pdMS_TO_TICKS(1000)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}
