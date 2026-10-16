#include "bsp_power.hpp"
#include "bsp_audio.h"
#include "bsp_nfc.h"
#include "bsp_touch.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <cmath>
static QueueHandle_t cards;
static void scanned(const bsp_nfc_card_t *card, void *) {
    if (xQueueSend(cards, card, 0) != pdTRUE)
        ESP_LOGW("demo", "NFC event queue full");
}
static void touched(bsp_touch_pad_t pad, bool pressed, void *) {
    ESP_LOGI("demo", "touch=%d pressed=%d", pad, pressed);
}
static void card_worker(void *) {
    bsp_nfc_card_t card;
    int16_t tone[2400];
    for (int i = 0; i < 2400; i++)
        tone[i] = static_cast<int16_t>(2500 * sin(2 * 3.141592653589793 * 1200 * i / 24000.0));
    while (true) {
        if (xQueueReceive(cards, &card, portMAX_DELAY) == pdTRUE) {
            // Feedback is local. Application/cloud transport consumes UID independently.
            esp_err_t beep = bsp_audio_write(tone, sizeof tone);
            ESP_LOGI("demo", "card uid=%s ndef=%d oversized=%d beep=%s", card.uid, card.has_ndef,
                     card.payload_too_large, esp_err_to_name(beep));
        }
    }
}
extern "C" void app_main() {
    ESP_ERROR_CHECK(bsp_power_init());
    ESP_ERROR_CHECK(bsp_audio_init());
    ESP_ERROR_CHECK(bsp_audio_open(24000, 16, 1));
    ESP_ERROR_CHECK(bsp_audio_set_volume(50));
    cards = xQueueCreate(8, sizeof(bsp_nfc_card_t));
    assert(cards);
    assert(xTaskCreate(card_worker, "card_feedback", 6144, nullptr, 4, nullptr) == pdPASS);
    ESP_ERROR_CHECK(bsp_touch_start(touched, nullptr));
    ESP_ERROR_CHECK(bsp_nfc_init());
    ESP_ERROR_CHECK(bsp_nfc_start(scanned, nullptr));
    ESP_LOGI("demo", "Bobo BSP ready: NFC / touch / local feedback. Cloud is application-owned.");
}
