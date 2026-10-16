#pragma once
#include "esp_err.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    char uid[21];
    char record_type[65];
    char payload[513];
    bool has_ndef;
    bool payload_too_large;
} bsp_nfc_card_t;
// Called on NFC worker task. Copy/enqueue the event; do not perform network or playback here.
typedef void (*bsp_nfc_callback_t)(const bsp_nfc_card_t *card, void *user);
esp_err_t bsp_nfc_init(void);
esp_err_t bsp_nfc_start(bsp_nfc_callback_t callback, void *user);
esp_err_t bsp_nfc_stop(void);
#ifdef __cplusplus
}
#endif
