#pragma once
#include "esp_err.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { BSP_TOUCH_FRONT_PAD, BSP_TOUCH_BACK_PAD } bsp_touch_pad_t;
typedef void (*bsp_touch_callback_t)(bsp_touch_pad_t pad, bool pressed, void *context);
// Digital touch-controller outputs, not ESP32 capacitive touch-pad channels.
esp_err_t bsp_touch_start(bsp_touch_callback_t callback, void *context);
esp_err_t bsp_touch_stop(void);
#ifdef __cplusplus
}
#endif
