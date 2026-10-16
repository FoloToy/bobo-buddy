#pragma once
#include "esp_err.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t bsp_haptic_init(void);
// Worker context only; library operation may block for the requested duration.
esp_err_t bsp_haptic_play(uint8_t waveform, uint8_t gain, uint32_t duration_ms);
#ifdef __cplusplus
}
#endif
