#include "bsp_4g.h"
#include "bsp_pins.h"
#include "bsp_modem_guard.hpp"
#include "folo_ppp_network.hpp"
#include "esp_netif.h"
#include "esp_event.h"
#include "driver/uart.h"
#include "freertos/event_groups.h"
#include <atomic>
#include <cstring>
#include "freertos/task.h"
#include "esp_log.h"
static std::atomic<bool> started{false}, connected{false};
static esp_netif_t *netif;
static EventGroupHandle_t events;
static esp_event_handler_instance_t got_ip, lost_ip;
static constexpr EventBits_t CONNECTED = 1;
static void ip_event(void *, esp_event_base_t, int32_t id, void *data) {
    if (id == IP_EVENT_PPP_GOT_IP && data &&
        static_cast<ip_event_got_ip_t *>(data)->esp_netif == netif) {
        connected = true;
        xEventGroupSetBits(events, CONNECTED);
    } else if (id == IP_EVENT_PPP_LOST_IP) {
        connected = false;
        xEventGroupClearBits(events, CONNECTED);
    }
}
bool bsp_4g_is_started() { return started; }
bool bsp_4g_is_connected() { return connected; }
static void cleanup() {
    connected = false;
    if (got_ip) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_PPP_GOT_IP, got_ip);
        got_ip = nullptr;
    }
    if (lost_ip) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_PPP_LOST_IP, lost_ip);
        lost_ip = nullptr;
    }
    modem_deinit_network();
    if (netif) {
        esp_netif_destroy(netif);
        netif = nullptr;
    }
    if (events) {
        vEventGroupDelete(events);
        events = nullptr;
    }
    started = false;
}
esp_err_t bsp_4g_start(const char *apn, int sim_slot) {
    if (!apn || !apn[0] || strlen(apn) > 63 || sim_slot < 0 || sim_slot > 1)
        return ESP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lock(bsp_modem_mutex());
    if (started || uart_is_driver_installed(BSP_MODEM_UART))
        return ESP_ERR_INVALID_STATE;
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK)
        return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        return err;
    events = xEventGroupCreate();
    if (!events)
        return ESP_ERR_NO_MEM;
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_PPP();
    netif = esp_netif_new(&cfg);
    if (!netif) {
        vEventGroupDelete(events);
        events = nullptr;
        return ESP_ERR_NO_MEM;
    }
    started = true;
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_PPP_GOT_IP, ip_event, nullptr,
                                              &got_ip);
    if (err == ESP_OK)
        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_PPP_LOST_IP, ip_event, nullptr,
                                                  &lost_ip);
    if (err == ESP_OK)
        err = modem_init_network(netif, BSP_MODEM_TX, BSP_MODEM_RX, BSP_MODEM_INITIAL_BAUD, 5,
                                 sim_slot, apn);
    if (err == ESP_OK) {
        bool entered = false;
        for (int attempt = 0; attempt < 10 && !entered; attempt++) {
            bool synced = modem_check_sync();
            if (!synced) {
                modem_stop_network();
                synced = modem_check_sync();
            }
            ESP_LOGI("bsp_4g", "startup attempt=%d modem_sync=%d", attempt + 1, synced);
            if (synced && modem_check_signal())
                entered = modem_start_network();
            if (!entered)
                vTaskDelay(pdMS_TO_TICKS(1000));
        }
        if (!entered)
            err = ESP_FAIL;
    }
    if (err != ESP_OK) {
        cleanup();
    }
    return err;
}
esp_err_t bsp_4g_wait_connected(uint32_t timeout_ms) {
    std::lock_guard<std::mutex> lock(bsp_modem_mutex());
    if (!started)
        return ESP_ERR_INVALID_STATE;
    return (xEventGroupWaitBits(events, CONNECTED, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms)) &
            CONNECTED)
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}
esp_err_t bsp_4g_stop() {
    std::lock_guard<std::mutex> lock(bsp_modem_mutex());
    if (!started)
        return ESP_OK;
    modem_stop_network();
    cleanup();
    return ESP_OK;
}
int bsp_4g_signal_quality() {
    std::lock_guard<std::mutex> lock(bsp_modem_mutex());
    return started ? modem_get_signal_quality() : -1;
}
