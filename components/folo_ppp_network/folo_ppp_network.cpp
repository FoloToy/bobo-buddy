/*
 * SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/*
 *  softAP to PPPoS Example (network_dce)
 */
#include "folo_ppp_network.hpp"

#include <driver/uart.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace esp_modem;
using namespace esp_modem::dce_factory;

class NetModule;
using NetDCE = DCE_T<NetModule>;

static const char *TAG = "modem_network_util";
static constexpr size_t kModemDteBufferSize = 2048;
static constexpr int kModemUartRxBufferSize = 16 * 1024;
static constexpr int kModemUartEventQueueSize = 64;
static constexpr int kModemUartRxFullThreshold = 32;
static constexpr uint32_t kLbsLocationTimeoutMs = 8000;
static constexpr int kLbsBusyCmeErrorCode = 105;
static constexpr int kLbsBusyMaxAttempts = 3;
static constexpr uint32_t kLbsBusyRetryDelayMs = 2000;
static SemaphoreHandle_t s_modem_command_mutex = nullptr;

static void ConfigureModemUartRxFullThreshold(uart_port_t port) {
    esp_err_t err = uart_set_rx_full_threshold(port, kModemUartRxFullThreshold);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set modem UART RX full threshold to %d: %s",
                 kModemUartRxFullThreshold, esp_err_to_name(err));
    }
}

static const char *CommandResultName(command_result result) {
    switch (result) {
    case command_result::OK:
        return "ok";
    case command_result::FAIL:
        return "fail";
    case command_result::TIMEOUT:
        return "timeout";
    default:
        return "unknown";
    }
}

static std::string TrimString(const std::string &value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
        ++start;
    }

    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(start, end - start);
}

static std::vector<std::string> SplitCsvFields(const std::string &payload) {
    std::vector<std::string> fields;
    std::string current;
    bool in_quotes = false;

    for (char ch : payload) {
        if (ch == '"') {
            in_quotes = !in_quotes;
            continue;
        }
        if (ch == ',' && !in_quotes) {
            fields.push_back(TrimString(current));
            current.clear();
            continue;
        }
        current.push_back(ch);
    }
    fields.push_back(TrimString(current));
    return fields;
}

static bool ParseIntField(const std::string &field, int *value, int base = 10) {
    if (value == nullptr) {
        return false;
    }
    std::string trimmed = TrimString(field);
    if (trimmed.empty()) {
        return false;
    }

    char *end = nullptr;
    long parsed = std::strtol(trimmed.c_str(), &end, base);
    if (end == trimmed.c_str() || *end != '\0') {
        return false;
    }
    *value = static_cast<int>(parsed);
    return true;
}

static std::string NormalizeHexField(const std::string &field) {
    std::string normalized = TrimString(field);
    if (normalized.rfind("0x", 0) == 0 || normalized.rfind("0X", 0) == 0) {
        normalized.erase(0, 2);
    }
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return normalized;
}

static std::string HexToDecimalString(const std::string &hex) {
    std::string normalized = NormalizeHexField(hex);
    if (normalized.empty()) {
        return "";
    }

    char *end = nullptr;
    unsigned long long parsed = std::strtoull(normalized.c_str(), &end, 16);
    if (end == normalized.c_str() || *end != '\0') {
        return "";
    }
    return std::to_string(parsed);
}

static bool ExtractResponsePayload(const std::string &out, const char *prefix,
                                   std::string &payload) {
    auto pattern_pos = out.find(prefix);
    if (pattern_pos == std::string::npos) {
        return false;
    }

    size_t start = pattern_pos + std::strlen(prefix);
    size_t end = out.find_first_of("\r\n", start);
    if (end == std::string::npos) {
        end = out.size();
    }
    payload = TrimString(out.substr(start, end - start));
    return true;
}

static void ParseCeregResponse(const std::string &out, lte_cell_info_t &info) {
    info.raw_cereg = TrimString(out);

    std::string payload;
    if (!ExtractResponsePayload(out, "+CEREG:", payload)) {
        return;
    }

    auto fields = SplitCsvFields(payload);
    if (fields.size() >= 2) {
        ParseIntField(fields[1], &info.reg_state);
    }
    if (fields.size() >= 4) {
        info.tac = NormalizeHexField(fields[2]);
        info.cell_id = NormalizeHexField(fields[3]);
        info.tac_dec = HexToDecimalString(info.tac);
        info.cell_id_dec = HexToDecimalString(info.cell_id);
    }
    if (fields.size() >= 5) {
        ParseIntField(fields[4], &info.act);
    }
}

static void ParseCopsResponse(const std::string &out, lte_cell_info_t &info) {
    info.raw_cops = TrimString(out);

    std::string payload;
    if (!ExtractResponsePayload(out, "+COPS:", payload)) {
        return;
    }

    auto fields = SplitCsvFields(payload);
    if (fields.size() >= 3) {
        info.operator_numeric = fields[2];
        if (info.operator_numeric.size() >= 5) {
            info.mcc = info.operator_numeric.substr(0, 3);
            info.mnc = info.operator_numeric.substr(3);
        }
    }
    if (fields.size() >= 4 && info.act < 0) {
        ParseIntField(fields[3], &info.act);
    }
}

static void ParseCesqResponse(const std::string &out, lte_cell_info_t &info) {
    info.raw_cesq = TrimString(out);

    std::string payload;
    if (!ExtractResponsePayload(out, "+CESQ:", payload)) {
        return;
    }

    auto fields = SplitCsvFields(payload);
    if (fields.size() < 6) {
        return;
    }

    ParseIntField(fields[4], &info.cesq_rsrq);
    ParseIntField(fields[5], &info.cesq_rsrp);
    if (info.cesq_rsrq >= 0 && info.cesq_rsrq <= 34) {
        info.rsrq_db_x2 = info.cesq_rsrq - 39;
    }
    if (info.cesq_rsrp >= 0 && info.cesq_rsrp <= 97) {
        info.rsrp_dbm = info.cesq_rsrp - 140;
    }
}

static bool IsDecimalNumberString(const std::string &value) {
    std::string trimmed = TrimString(value);
    if (trimmed.empty()) {
        return false;
    }

    bool seen_digit = false;
    bool seen_dot = false;
    for (size_t i = 0; i < trimmed.size(); ++i) {
        unsigned char ch = static_cast<unsigned char>(trimmed[i]);
        if ((trimmed[i] == '+' || trimmed[i] == '-') && i == 0) {
            continue;
        }
        if (std::isdigit(ch)) {
            seen_digit = true;
            continue;
        }
        if (trimmed[i] == '.' && !seen_dot) {
            seen_dot = true;
            continue;
        }
        return false;
    }
    return seen_digit;
}

static bool ParseMlbsLocResponse(const std::string &out, lte_lbs_location_t &location) {
    std::string payload;
    if (!ExtractResponsePayload(out, "+MLBSLOC:", payload)) {
        return false;
    }

    location.raw_mlbsloc = TrimString(out);
    auto fields = SplitCsvFields(payload);
    if (fields.empty() || !ParseIntField(fields[0], &location.status)) {
        return false;
    }
    if (fields.size() >= 3 && IsDecimalNumberString(fields[1]) &&
        IsDecimalNumberString(fields[2])) {
        location.longitude = fields[1];
        location.latitude = fields[2];
    }
    return true;
}

static bool ParseCmeErrorResponse(const std::string &out, int *cme_error) {
    std::string payload;
    if (!ExtractResponsePayload(out, "+CME ERROR:", payload)) {
        return false;
    }
    return ParseIntField(payload, cme_error);
}

static bool IsSuccessfulLbsLocation(const lte_lbs_location_t &location) {
    return location.status == 100 && !location.longitude.empty() && !location.latitude.empty();
}

static std::string SanitizeLogValue(const std::string &value, size_t max_len = 160) {
    std::string sanitized;
    sanitized.reserve(std::min(value.size(), max_len));
    for (unsigned char ch : value) {
        if (sanitized.size() >= max_len) {
            sanitized += "...";
            break;
        }
        if (ch == '\r' || ch == '\n') {
            sanitized += '|';
        } else if (ch < 0x20) {
            sanitized += '?';
        } else {
            sanitized.push_back(static_cast<char>(ch));
        }
    }
    return sanitized;
}

static std::string JsonEscape(const std::string &value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (unsigned char ch : value) {
        switch (ch) {
        case '"':
            escaped += "\\\"";
            break;
        case '\\':
            escaped += "\\\\";
            break;
        case '\b':
            escaped += "\\b";
            break;
        case '\f':
            escaped += "\\f";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            if (ch < 0x20) {
                char buf[7];
                std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
                escaped += buf;
            } else {
                escaped.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    return escaped;
}

static void AppendJsonName(std::string &json, bool &first, const char *name) {
    if (!first) {
        json += ",";
    }
    first = false;
    json += "\"";
    json += name;
    json += "\":";
}

static void AppendJsonInt(std::string &json, bool &first, const char *name, int value) {
    AppendJsonName(json, first, name);
    json += std::to_string(value);
}

static void AppendJsonNumberString(std::string &json, bool &first, const char *name,
                                   const std::string &value) {
    if (value.empty()) {
        return;
    }
    AppendJsonName(json, first, name);
    json += value;
}

static void AppendJsonString(std::string &json, bool &first, const char *name,
                             const std::string &value) {
    if (value.empty()) {
        return;
    }
    AppendJsonName(json, first, name);
    json += "\"";
    json += JsonEscape(value);
    json += "\"";
}

std::string modem_lte_cell_info_to_json(const lte_cell_info_t &cell_info) {
    std::string json = "{";
    bool first = true;

    AppendJsonString(json, first, "radio", "lte");
    AppendJsonString(json, first, "mcc", cell_info.mcc);
    AppendJsonString(json, first, "mnc", cell_info.mnc);
    AppendJsonNumberString(json, first, "tac", cell_info.tac_dec);
    AppendJsonNumberString(json, first, "cell_id", cell_info.cell_id_dec);
    if (cell_info.cesq_rsrp >= 0 && cell_info.cesq_rsrp <= 97) {
        AppendJsonInt(json, first, "rsrp_dbm", cell_info.rsrp_dbm);
    }

    json += "}";
    return json;
}

std::string modem_lte_lbs_location_to_json(const lte_lbs_location_t &location) {
    std::string json = "{";
    bool first = true;

    AppendJsonString(json, first, "type", "lbs");
    if (location.status >= 0) {
        AppendJsonInt(json, first, "status", location.status);
    }
    AppendJsonNumberString(json, first, "longitude", location.longitude);
    AppendJsonNumberString(json, first, "latitude", location.latitude);
    if (location.cme_error >= 0) {
        AppendJsonInt(json, first, "cme_error", location.cme_error);
    }
    if (location.attempts > 0) {
        AppendJsonInt(json, first, "attempts", location.attempts);
    }

    json += "}";
    return json;
}

/**
 * @brief The PPP network is a singleton, allocate statically here
 */

/**
 * @brief Custom factory for creating NetDCE and NetModule
 */
class NetDCE_Factory : public Factory {
  public:
    template <typename Module, typename... Args>
    static DCE_T<Module> *create(const config *cfg, Args &&...args) {
        return build_generic_DCE<Module>(cfg, std::forward<Args>(args)...);
    }

    template <typename Module, typename... Args>
    static std::shared_ptr<Module> create_module(const config *cfg, Args &&...args) {
        return build_shared_module<Module>(cfg, std::forward<Args>(args)...);
    }
};

/**
 * @brief This is an example of implementing minimal network module functionality
 *
 * This does only include those AT commands, that are needed for setting the network up
 * and also initialization part (set pin, ...)
 */
class NetModule : public ModuleIf {
  public:
    explicit NetModule(std::shared_ptr<DTE> dte, const esp_modem_dce_config *cfg)
        : dte(std::move(dte)), apn(std::string(cfg->apn)) {}

    [[nodiscard]] bool setup_data_mode() override {
        PdpContext pdp(apn);
        if (set_pdp_context(pdp) != command_result::OK) {
            return false;
        }
        return true;
    }

    bool set_mode(modem_mode mode) override {
        switch (mode) {
        case esp_modem::modem_mode::DATA_MODE:
            if (set_data_mode() != command_result::OK) {
                return resume_data_mode() == command_result::OK;
            }
            return true;
        case esp_modem::modem_mode::COMMAND_MODE:
            return set_command_mode() == command_result::OK;
        default:
            return false;
        }
    }

    bool sync() { return dce_commands::sync(dte.get()) == command_result::OK; }

    void reset() { dce_commands::reset(dte.get()); }

    int get_signal_quality() {
        int rssi, ber;
        if (dce_commands::get_signal_quality(dte.get(), rssi, ber) != command_result::OK) {
            return -1;
        }
        return rssi;
    }

    std::string get_imei() {
        std::string imei;
        if (dce_commands::get_imei(dte.get(), imei) != command_result::OK) {
            return "N/A";
        }
        return imei;
    }

    std::string get_imsi() {
        std::string imsi;
        if (dce_commands::get_imsi(dte.get(), imsi) != command_result::OK) {
            return "N/A";
        }
        return imsi;
    }

    int get_network_registration_state() {
        // AT+CEREG?
        // +CEREG: 0,1
        // OK
        std::string out;
        auto ret = dce_commands::at(dte.get(), "AT+CEREG?", out, 5000);
        if (ret != command_result::OK) {
            return -1; // Error
        }
        constexpr std::string_view pattern = "+CEREG: ";
        auto pattern_pos = out.find(pattern);
        if (pattern_pos == std::string::npos) {
            return -1; // Error
        }

        // Find the position of the first comma after pattern
        auto comma_pos = out.find(',', pattern_pos + pattern.size());
        if (comma_pos == std::string::npos) {
            return -1; // Error
        }

        // Extract the second parameter (after the comma)
        auto reg_state_pos = comma_pos + 1;
        auto end_pos = out.find_first_of("\r\n", reg_state_pos);
        if (end_pos == std::string::npos) {
            end_pos = out.size();
        }

        try {
            // Convert the extracted string to integer
            return std::stoi(out.substr(reg_state_pos, end_pos - reg_state_pos));
        } catch (const std::exception &) {
            return -1; // Error in conversion
        }
    }

    void get_iccid(std::string &iccid) {
        // AT+MCCID
        // +MCCID: 89861118216007272115
        // OK
        std::string out;
        auto ret = dce_commands::at(dte.get(), "AT+MCCID", out, 5000);
        if (ret != command_result::OK) {
            iccid = "N/A";
            return;
        }
        constexpr std::string_view pattern = "+MCCID:";
        auto pattern_pos = out.find(pattern);
        if (pattern_pos == std::string::npos) {
            iccid = "N/A";
            return;
        }

        // Extract ICCID substring after "+MCCID:"
        auto start = pattern_pos + pattern.size();
        auto end = out.find('\n', start);
        if (end == std::string::npos) {
            end = out.size();
        }
        iccid = out.substr(start, end - start);
        // Remove leading/trailing whitespace
        iccid.erase(0, iccid.find_first_not_of(" \r\n\t"));
        iccid.erase(iccid.find_last_not_of(" \r\n\t") + 1);
        if (iccid.empty()) {
            iccid = "N/A";
            return;
        }
    }

    bool check_signal() {
        int rssi, ber;
        if (dce_commands::get_signal_quality(dte.get(), rssi, ber) != command_result::OK) {
            return false;
        }
        return rssi != 99 && rssi > 5;
    }

    bool get_lte_cell_info(lte_cell_info_t &info) {
        int rssi = -1;
        int ber = -1;
        if (dce_commands::get_signal_quality(dte.get(), rssi, ber) == command_result::OK) {
            info.rssi = rssi;
            info.ber = ber;
        }

        std::string out;
        static_cast<void>(dce_commands::at(dte.get(), "AT+CEREG=2", out, 1000));

        out.clear();
        if (dce_commands::at(dte.get(), "AT+CEREG?", out, 2000) == command_result::OK) {
            ParseCeregResponse(out, info);
        }
        out.clear();
        static_cast<void>(dce_commands::at(dte.get(), "AT+CEREG=0", out, 1000));

        out.clear();
        static_cast<void>(dce_commands::at(dte.get(), "AT+COPS=3,2", out, 1000));

        out.clear();
        if (dce_commands::at(dte.get(), "AT+COPS?", out, 2000) == command_result::OK) {
            ParseCopsResponse(out, info);
        }

        out.clear();
        if (dce_commands::at(dte.get(), "AT+CESQ", out, 2000) == command_result::OK) {
            ParseCesqResponse(out, info);
        }

        return info.rssi >= 0 || info.reg_state >= 0 || !info.operator_numeric.empty() ||
               !info.cell_id.empty() || info.cesq_rsrp >= 0 || info.cesq_rsrq >= 0;
    }

    bool get_lte_lbs_location(lte_lbs_location_t &location) {
        int rssi = -1;
        int ber = -1;
        if (dce_commands::get_signal_quality(dte.get(), rssi, ber) == command_result::OK) {
            location.rssi = rssi;
            location.ber = ber;
        }

        if (!configure_lbs_location()) {
            return false;
        }

        for (int attempt = 1; attempt <= kLbsBusyMaxAttempts; ++attempt) {
            location.attempts = attempt;
            command_result ret = run_lbs_location_command(location);
            bool success = ret == command_result::OK && IsSuccessfulLbsLocation(location);
            ESP_LOGI(TAG,
                     "WN_DIAG phase=lte_lbs_command attempt=%d result=%s status=%d "
                     "cme_error=%d busy=%d raw=%s",
                     attempt, success ? "ok" : CommandResultName(ret), location.status,
                     location.cme_error, location.busy ? 1 : 0,
                     SanitizeLogValue(location.raw_mlbsloc).c_str());
            if (success) {
                return true;
            }
            if (!location.busy || attempt >= kLbsBusyMaxAttempts) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(kLbsBusyRetryDelayMs));
        }

        return false;
    }

    [[maybe_unused]] bool init_sim(const std::string &pin) {
        // switch to command mode (in case we were in PPP mode)
        static_cast<void>(set_command_mode()); // ignore the potential failure, as we might be in
                                               // command mode after startup
        bool is_pin_ok;
        if (read_pin(is_pin_ok) != command_result::OK) {
            return false;
        }
        if (!is_pin_ok) {
            if (set_pin(pin) != command_result::OK) {
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (read_pin(is_pin_ok) != command_result::OK || !is_pin_ok) {
                return false;
            }
        }
        return true;
    }

  private:
    std::shared_ptr<DTE> dte;
    std::string apn;
    bool lbs_location_configured_ = false;

    bool configure_lbs_location() {
        if (lbs_location_configured_) {
            return true;
        }

        std::string out;
        auto rat_result = dce_commands::at(dte.get(), "AT+MPSRAT=\"LTE\"", out, 3000);
        if (rat_result != command_result::OK) {
            ESP_LOGW(TAG, "WN_DIAG phase=lte_lbs_config cmd=MPSRAT result=%s continue=1",
                     CommandResultName(rat_result));
        }

        out.clear();
        auto method_result = dce_commands::at(dte.get(), "AT+MLBSCFG=\"method\",40", out, 3000);
        if (method_result != command_result::OK) {
            ESP_LOGW(TAG, "WN_DIAG phase=lte_lbs_config cmd=method result=%s",
                     CommandResultName(method_result));
            return false;
        }

        out.clear();
        auto nearby_result = dce_commands::at(dte.get(), "AT+MLBSCFG=\"nearbtsen\",1", out, 3000);
        if (nearby_result != command_result::OK) {
            ESP_LOGW(TAG, "WN_DIAG phase=lte_lbs_config cmd=nearbtsen result=%s continue=1",
                     CommandResultName(nearby_result));
        }

        lbs_location_configured_ = true;
        return true;
    }

    command_result run_lbs_location_command(lte_lbs_location_t &location) {
        location.status = -1;
        location.cme_error = -1;
        location.busy = false;
        location.longitude.clear();
        location.latitude.clear();
        location.raw_mlbsloc.clear();

        std::string response_buffer;
        command_result command_status = command_result::TIMEOUT;
        auto ret = dte->command(
            "AT+MLBSLOC\r",
            [&](uint8_t *data, size_t len) {
                if (data == nullptr || len == 0) {
                    return command_result::TIMEOUT;
                }

                response_buffer.append(reinterpret_cast<char *>(data), len);
                location.raw_mlbsloc = TrimString(response_buffer);

                int cme_error = -1;
                if (ParseCmeErrorResponse(response_buffer, &cme_error)) {
                    location.cme_error = cme_error;
                    location.busy = cme_error == kLbsBusyCmeErrorCode;
                    command_status = command_result::FAIL;
                    return command_result::FAIL;
                }
                if (response_buffer.find("ERROR") != std::string::npos) {
                    command_status = command_result::FAIL;
                    return command_result::FAIL;
                }
                if (ParseMlbsLocResponse(response_buffer, location)) {
                    command_status = command_result::OK;
                    return command_result::OK;
                }

                return command_result::TIMEOUT;
            },
            kLbsLocationTimeoutMs);

        if (command_status == command_result::TIMEOUT) {
            command_status = ret;
        }
        location.raw_mlbsloc = TrimString(response_buffer);
        return command_status;
    }

    [[nodiscard]] command_result set_pdp_context(PdpContext &pdp) {
        return dce_commands::set_pdp_context(dte.get(), pdp);
    }
    [[nodiscard]] command_result set_pin(const std::string &pin) {
        return dce_commands::set_pin(dte.get(), pin);
    }
    [[nodiscard]] command_result read_pin(bool &pin_ok) {
        return dce_commands::read_pin(dte.get(), pin_ok);
    }

  public:
    [[nodiscard]] command_result set_data_mode() { return dce_commands::set_data_mode(dte.get()); }
    [[nodiscard]] command_result resume_data_mode() {
        return dce_commands::resume_data_mode(dte.get());
    }
    [[nodiscard]] command_result set_command_mode() {
        return dce_commands::set_command_mode(dte.get());
    }
};

/**
 * @brief Local network object used to setup PPP network
 */
NetDCE *dce = nullptr;

/**
 * @brief Create a UART DTE object
 *
 * @param config DTE configuration
 * @return std::shared_ptr<DTE> created DTE object
 */

esp_err_t modem_init_network(esp_netif_t *netif, int tx_io_num, int rx_io_num, int baud_rate,
                             int task_priority, int sim_num, const char *apn) {
    // configure
    esp_modem_dte_config_t dte_config = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte_config.uart_config.rx_io_num = rx_io_num;
    dte_config.uart_config.tx_io_num = tx_io_num;
    dte_config.uart_config.baud_rate = baud_rate;
    dte_config.dte_buffer_size = kModemDteBufferSize;
    dte_config.uart_config.rx_buffer_size = kModemUartRxBufferSize;
    dte_config.uart_config.event_queue_size = kModemUartEventQueueSize;
    dte_config.task_priority = task_priority;
    esp_modem_dce_config dce_config = ESP_MODEM_DCE_DEFAULT_CONFIG(apn);

    // create DTE and minimal network DCE
    auto uart_dte = create_uart_dte(&dte_config);
    ConfigureModemUartRxFullThreshold(dte_config.uart_config.port_num);

    if (sim_num < 0 || sim_num > 1) {
        ESP_LOGE(TAG, "Invalid SIM number: %d. Must be 0 or 1.", sim_num);
        return ESP_ERR_INVALID_ARG;
    }

    // 设置波特率为921600
    uart_dte->command("AT+SINGLESIM=" + std::to_string(sim_num) + "\r", nullptr,
                      500);                             // ML307C 需要手动切换SIM2
    uart_dte->command("AT+IPR=921600\r", nullptr, 500); // 提高波特率到921600，提高数据传输速度

    // 销毁原有的DTE
    uart_dte.reset();
    uart_dte = nullptr;

    // create a new DTE with the new baud rate
    dte_config.uart_config.baud_rate = 921600;
    uart_dte = create_uart_dte(&dte_config);
    ConfigureModemUartRxFullThreshold(dte_config.uart_config.port_num);

    // create the specific device (and initialize it)
    auto dev = NetDCE_Factory::create_module<NetModule>(&dce_config, uart_dte, netif);

    // now create the DCE from our already existent device
    dce = NetDCE_Factory::create<NetModule>(&dce_config, uart_dte, netif, dev);

    if (dce == nullptr) {
        return ESP_FAIL;
    }
    if (s_modem_command_mutex == nullptr) {
        s_modem_command_mutex = xSemaphoreCreateMutex();
        if (s_modem_command_mutex == nullptr) {
            ESP_LOGW(TAG, "Failed to create modem command mutex");
        }
    }
    return ESP_OK;
}

int modem_get_signal_quality() { return dce->get_module()->get_signal_quality(); }

bool modem_refresh_signal_quality(int *rssi) {
    if (rssi == nullptr || dce == nullptr || dce->get_module() == nullptr ||
        s_modem_command_mutex == nullptr) {
        ESP_LOGW(TAG, "WN_DIAG phase=lte_signal_refresh result=skipped reason=not_ready");
        return false;
    }
    if (xSemaphoreTake(s_modem_command_mutex, 0) != pdTRUE) {
        ESP_LOGW(TAG, "WN_DIAG phase=lte_signal_refresh result=skipped reason=busy");
        return false;
    }

    int refreshed_rssi = -1;
    bool refreshed = false;
    int64_t start_ms = esp_timer_get_time() / 1000;

    auto pause_result = dce->pause_netif(true, false, 1000);
    if (pause_result == command_result::OK) {
        refreshed_rssi = dce->get_module()->get_signal_quality();
        auto resume_result = dce->pause_netif(false, false);
        if (resume_result != command_result::OK) {
            ESP_LOGW(TAG, "WN_DIAG phase=lte_signal_refresh action=resume result=%s force_resume=1",
                     CommandResultName(resume_result));
            dce->pause_netif(false, true);
        } else if (refreshed_rssi >= 0) {
            *rssi = refreshed_rssi;
            refreshed = true;
        }
    } else {
        ESP_LOGW(TAG, "WN_DIAG phase=lte_signal_refresh action=pause result=%s force_resume=1",
                 CommandResultName(pause_result));
        dce->pause_netif(false, true);
    }

    int duration_ms = (int)(esp_timer_get_time() / 1000 - start_ms);
    ESP_LOGI(TAG, "WN_DIAG phase=lte_signal_refresh result=%s rssi=%d duration_ms=%d",
             refreshed ? "ok" : "failed", refreshed_rssi, duration_ms);

    xSemaphoreGive(s_modem_command_mutex);
    return refreshed;
}

bool modem_refresh_lte_cell_info(lte_cell_info_t *cell_info) {
    if (cell_info == nullptr || dce == nullptr || dce->get_module() == nullptr ||
        s_modem_command_mutex == nullptr) {
        ESP_LOGW(TAG, "WN_DIAG phase=lte_cell_refresh result=skipped reason=not_ready");
        return false;
    }
    if (xSemaphoreTake(s_modem_command_mutex, 0) != pdTRUE) {
        ESP_LOGW(TAG, "WN_DIAG phase=lte_cell_refresh result=skipped reason=busy");
        return false;
    }

    lte_cell_info_t refreshed_info;
    bool refreshed = false;
    int64_t start_ms = esp_timer_get_time() / 1000;

    auto pause_result = dce->pause_netif(true, false, 1000);
    if (pause_result == command_result::OK) {
        refreshed = dce->get_module()->get_lte_cell_info(refreshed_info);
        auto resume_result = dce->pause_netif(false, false);
        if (resume_result != command_result::OK) {
            refreshed = false;
            ESP_LOGW(TAG, "WN_DIAG phase=lte_cell_refresh action=resume result=%s force_resume=1",
                     CommandResultName(resume_result));
            dce->pause_netif(false, true);
        } else if (refreshed) {
            *cell_info = refreshed_info;
        }
    } else {
        ESP_LOGW(TAG, "WN_DIAG phase=lte_cell_refresh action=pause result=%s force_resume=1",
                 CommandResultName(pause_result));
        dce->pause_netif(false, true);
    }

    int duration_ms = (int)(esp_timer_get_time() / 1000 - start_ms);
    ESP_LOGI(TAG,
             "WN_DIAG phase=lte_cell_refresh result=%s rssi=%d reg_state=%d "
             "operator=%s tac=%s cell_id=%s rsrp_dbm=%d duration_ms=%d",
             refreshed ? "ok" : "failed", refreshed_info.rssi, refreshed_info.reg_state,
             refreshed_info.operator_numeric.c_str(), refreshed_info.tac.c_str(),
             refreshed_info.cell_id.c_str(), refreshed_info.rsrp_dbm, duration_ms);

    xSemaphoreGive(s_modem_command_mutex);
    return refreshed;
}

bool modem_refresh_lte_lbs_location(lte_lbs_location_t *location) {
    if (location == nullptr || dce == nullptr || dce->get_module() == nullptr ||
        s_modem_command_mutex == nullptr) {
        ESP_LOGW(TAG, "WN_DIAG phase=lte_lbs_refresh result=skipped reason=not_ready");
        return false;
    }
    if (xSemaphoreTake(s_modem_command_mutex, 0) != pdTRUE) {
        ESP_LOGW(TAG, "WN_DIAG phase=lte_lbs_refresh result=skipped reason=busy");
        return false;
    }

    lte_lbs_location_t refreshed_location;
    bool refreshed = false;
    int64_t start_ms = esp_timer_get_time() / 1000;

    auto pause_result = dce->pause_netif(true, false, 1000);
    if (pause_result == command_result::OK) {
        refreshed = dce->get_module()->get_lte_lbs_location(refreshed_location);
        auto resume_result = dce->pause_netif(false, false);
        if (resume_result != command_result::OK) {
            refreshed = false;
            ESP_LOGW(TAG, "WN_DIAG phase=lte_lbs_refresh action=resume result=%s force_resume=1",
                     CommandResultName(resume_result));
            dce->pause_netif(false, true);
        } else {
            *location = refreshed_location;
        }
    } else {
        ESP_LOGW(TAG, "WN_DIAG phase=lte_lbs_refresh action=pause result=%s force_resume=1",
                 CommandResultName(pause_result));
        dce->pause_netif(false, true);
    }

    int duration_ms = (int)(esp_timer_get_time() / 1000 - start_ms);
    ESP_LOGI(TAG,
             "WN_DIAG phase=lte_lbs_refresh result=%s rssi=%d status=%d "
             "cme_error=%d busy=%d attempts=%d longitude=%s latitude=%s duration_ms=%d",
             refreshed ? "ok" : "failed", refreshed_location.rssi, refreshed_location.status,
             refreshed_location.cme_error, refreshed_location.busy ? 1 : 0,
             refreshed_location.attempts, refreshed_location.longitude.c_str(),
             refreshed_location.latitude.c_str(), duration_ms);

    xSemaphoreGive(s_modem_command_mutex);
    return refreshed;
}

std::string modem_get_imei() {
    if (dce == nullptr || dce->get_module() == nullptr)
        return "";
    return dce->get_module()->get_imei();
}

std::string modem_get_imsi() {
    if (dce == nullptr || dce->get_module() == nullptr)
        return "";
    return dce->get_module()->get_imsi();
}

int modem_get_network_state() {
    if (dce == nullptr || dce->get_module() == nullptr)
        return -1;
    return dce->get_module()->get_network_registration_state();
}

std::string modem_get_iccid() {
    if (dce == nullptr || dce->get_module() == nullptr)
        return "N/A";
    std::string iccid;
    dce->get_module()->get_iccid(iccid);
    if (iccid.empty()) {
        return "N/A";
    }
    return iccid;
}

bool modem_start_network() {
    if (dce == nullptr)
        return false;
    return dce->set_mode(esp_modem::modem_mode::DATA_MODE);
}

bool modem_resume_network() {
    if (dce == nullptr)
        return false;
    return dce->set_mode(esp_modem::modem_mode::RESUME_DATA_MODE);
}

bool modem_stop_network() {
    if (dce == nullptr)
        return false;
    return dce->set_mode(esp_modem::modem_mode::COMMAND_MODE);
}

void modem_deinit_network() {
    if (dce) {
        delete dce;
        dce = nullptr;
    }
}

bool modem_check_sync() {
    if (dce == nullptr || dce->get_module() == nullptr)
        return false;
    return dce->get_module()->sync();
}

void modem_reset() {
    if (dce && dce->get_module()) {
        dce->get_module()->reset();
    }
}

bool modem_check_signal() {
    if (dce == nullptr || dce->get_module() == nullptr)
        return false;
    return dce->get_module()->check_signal();
}
