/*
 * SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/*
 * softAP to PPPoS Example (network_dce)
 */

#pragma once

#include <charconv>
#include <list>
#include <memory>
#include <string>
#include <utility>

#include "esp_modem_config.h"
#include "cxx_include/esp_modem_dte.hpp"
#include "cxx_include/esp_modem_api.hpp"
#include "cxx_include/esp_modem_dce_factory.hpp"

/**
 * @brief Initialize a singleton covering the PPP network provided by the connected modem device
 *
 * @param netif Already created network interface in PPP mode
 *
 * @return ESP_OK on success
 */
esp_err_t modem_init_network(esp_netif_t *netif, int tx_io_num, int rx_io_num, int baud_rate,
                             int task_priority, int sim_num, const char *apn);

/**
 * @brief Destroys the single network DCE
 */
void modem_deinit_network();

/**
 * @brief Starts the PPP network
 */
bool modem_start_network();

/**
 * @brief Resumes PPP when the modem is already in data mode
 */
bool modem_resume_network();

/**
 * @brief Stops the PPP network
 */
bool modem_stop_network();

bool modem_check_sync();

void modem_reset();

bool modem_check_signal();

/**
 * @brief Gets the signal quality of the modem
 *
 * @return Signal quality as an integer, or -1 on error
 */
int modem_get_signal_quality();

bool modem_refresh_signal_quality(int *rssi);

typedef struct {
    int rssi = -1;
    int ber = -1;
    int reg_state = -1;
    int act = -1;
    int cesq_rsrq = -1;
    int cesq_rsrp = -1;
    int rsrq_db_x2 = 0;
    int rsrp_dbm = 0;
    std::string operator_numeric;
    std::string mcc;
    std::string mnc;
    std::string tac;
    std::string tac_dec;
    std::string cell_id;
    std::string cell_id_dec;
    std::string raw_cereg;
    std::string raw_cops;
    std::string raw_cesq;
} lte_cell_info_t;

bool modem_refresh_lte_cell_info(lte_cell_info_t *cell_info);
std::string modem_lte_cell_info_to_json(const lte_cell_info_t &cell_info);

typedef struct {
    int rssi = -1;
    int ber = -1;
    int status = -1;
    int cme_error = -1;
    int attempts = 0;
    bool busy = false;
    std::string longitude;
    std::string latitude;
    std::string raw_mlbsloc;
} lte_lbs_location_t;

bool modem_refresh_lte_lbs_location(lte_lbs_location_t *location);
std::string modem_lte_lbs_location_to_json(const lte_lbs_location_t &location);

/**
 * @brief Gets the IMEI of the modem
 *
 * @return IMEI as a string, or "N/A" on error
 */
std::string modem_get_imei();

/**
 * @brief Gets the IMSI of the modem
 *
 * @return IMSI as a string, or "N/A" on error
 */
std::string modem_get_imsi();

/**
 * @brief Gets the ICCID of the modem
 *
 * @return ICCID as a string, or "N/A" on error
 */
std::string modem_get_iccid();

/**
 * @brief Gets the network registration state of the modem
 *
 * @return Network registration state as an integer, or -1 on error
 */
int modem_get_network_state();
