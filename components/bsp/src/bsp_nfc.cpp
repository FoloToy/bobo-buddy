#include "bsp_nfc.h"
#include "bsp_nfc_policy.hpp"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "port_ws1850t.hpp"
#include "NfcAdapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <atomic>
#include <cctype>
#include <cstring>
static WS1850T reader(BSP_NFC_RESET);
static NfcAdapter adapter(&reader);
static bool ready;
static std::atomic<bool> running{false};
static SemaphoreHandle_t stopped;
static std::atomic<bool> active{false};
static bsp_nfc_callback_t callback;
static void *context;
static BspNfcPlacement placement;
static void worker(void *) {
    while (running) {
        if (!adapter.tagPresent()) {
            placement.observe("");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        NfcTag tag = adapter.read();
        std::string raw = tag.getUidString(), uid;
        for (unsigned char ch : raw) {
            if (std::isxdigit(ch))
                uid += static_cast<char>(std::toupper(ch));
            else if (ch != ' ' && ch != ':' && ch != '-') {
                uid.clear();
                break;
            }
        }
        if (uid.size() >= 8 && uid.size() <= 20 && uid.size() % 2 == 0 && placement.observe(uid)) {
            bsp_nfc_card_t card = {};
            strcpy(card.uid, uid.c_str());
            if (tag.hasNdefMessage()) {
                auto message = tag.getNdefMessage();
                card.has_ndef = true;
                if (message.getRecordCount()) {
                    auto record = message.getRecord(0);
                    auto type = record.getType(), payload = record.getPayloadAsString();
                    card.payload_too_large = payload.size() >= sizeof card.payload ||
                                             type.size() >= sizeof card.record_type;
                    if (!card.payload_too_large) {
                        memcpy(card.payload, payload.data(), payload.size());
                        memcpy(card.record_type, type.data(), type.size());
                    }
                }
            }
            if (callback)
                callback(&card, context);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    active = false;
    xSemaphoreGive(stopped);
    vTaskDelete(nullptr);
}
esp_err_t bsp_nfc_init() {
    if (ready)
        return ESP_OK;
    auto e = bsp_i2c_init();
    if (e != ESP_OK)
        return e;
    e = reader.init(BSP_NFC_ADDR, 0, BSP_I2C_PORT);
    if (e == ESP_OK) {
        stopped = xSemaphoreCreateBinary();
        if (!stopped)
            return ESP_ERR_NO_MEM;
        ready = true;
    }
    return e;
}
esp_err_t bsp_nfc_start(bsp_nfc_callback_t cb, void *user) {
    if (!ready || !cb)
        return ESP_ERR_INVALID_STATE;
    if (active.exchange(true))
        return ESP_ERR_INVALID_STATE;
    running = true;
    if (stopped)
        xSemaphoreTake(stopped, 0);
    callback = cb;
    context = user;
    placement.reset();
    if (xTaskCreate(worker, "bsp_nfc", 6144, nullptr, 4, nullptr) != pdPASS) {
        running = false;
        active = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
esp_err_t bsp_nfc_stop() {
    if (!active)
        return ESP_OK;
    running = false;
    return xSemaphoreTake(stopped, pdMS_TO_TICKS(2000)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}
