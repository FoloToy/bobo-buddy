#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
// APN is deployment configuration, not a board constant. SIM selector 0 or 1.
esp_err_t bsp_4g_start(const char *apn, int sim_slot);
esp_err_t bsp_4g_wait_connected(uint32_t timeout_ms);
esp_err_t bsp_4g_stop(void);
bool bsp_4g_is_started(void);
bool bsp_4g_is_connected(void);
// Call from a worker; modem AT query may briefly enter command mode.
int bsp_4g_signal_quality(void);
#ifdef __cplusplus
}
#endif
