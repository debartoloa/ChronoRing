#include "network_manager.h"
#include "../hardware_pins.h"
#include "../config_manager.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <esp_sntp.h>

volatile bool NetworkManager::_staConnected = false;
volatile bool NetworkManager::_apActive = false;
volatile bool NetworkManager::_mdnsStarted = false;
volatile bool NetworkManager::_ntpConfigured = false;
volatile bool NetworkManager::_reconnectRequested = false;
TaskHandle_t NetworkManager::_netTaskHandle = nullptr;

constexpr const char* NetworkManager::MDNS_HOSTNAME;
constexpr const char* NetworkManager::FALLBACK_AP_SSID;
constexpr const char* NetworkManager::FALLBACK_AP_PASS;
constexpr uint32_t NetworkManager::WIFI_CONNECT_TIMEOUT_MS;
constexpr const char* NetworkManager::TZ_ITALY_POSIX;
constexpr const char* NetworkManager::NTP_SERVER_1;
constexpr const char* NetworkManager::NTP_SERVER_2;

void NetworkManager::begin() {
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    WiFi.setHostname(MDNS_HOSTNAME);

    // Configura subito il fuso orario POSIX anche prima della connessione
    setenv("TZ", TZ_ITALY_POSIX, 1);
    tzset();

    // Eseguiamo il primo tentativo di connessione o fallback AP all'avvio
    if (!connectToSavedWiFi(WIFI_CONNECT_TIMEOUT_MS)) {
        startFallbackAP();
    } else {
        setupMdnsAndNtp();
    }

    // Avvia il task FreeRTOS dedicato alla rete su CHRONO_NET_CORE (Core 0)
    if (_netTaskHandle == nullptr) {
        xTaskCreatePinnedToCore(
            NetworkManager::networkTask,
            "ChronoNetTask",
            4096,
            nullptr,
            1,
            &_netTaskHandle,
            CHRONO_NET_CORE
        );
    }
}

bool NetworkManager::connectToSavedWiFi(uint32_t timeoutMs) {
    SystemConfig cfg = ConfigManager::getConfig();
    if (strlen(cfg.wifiSsid) == 0) {
        Serial.println("[NET] Nessun SSID Wi-Fi configurato in /config.json.");
        _staConnected = false;
        return false;
    }

    Serial.printf("[NET] Connessione alla rete Wi-Fi '%s' in corso (timeout %u s)...\n",
                  cfg.wifiSsid, timeoutMs / 1000);

    WiFi.mode(_apActive ? WIFI_AP_STA : WIFI_STA);
    WiFi.begin(cfg.wifiSsid, cfg.wifiPassword);

    uint32_t startMs = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - startMs < timeoutMs)) {
        vTaskDelay(pdMS_TO_TICKS(250));
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        _staConnected = true;
        if (_apActive) {
            Serial.println("[NET] Connessione STA stabilita: disattivazione Access Point di fallback.");
            WiFi.softAPdisconnect(true);
            WiFi.mode(WIFI_STA);
            _apActive = false;
        }
        Serial.printf("[NET] Connesso! IP: %s | Hostname: http://%s.local | RSSI: %d dBm\n",
                      WiFi.localIP().toString().c_str(), MDNS_HOSTNAME, WiFi.RSSI());
        return true;
    }

    Serial.println("[NET] ATTENZIONE: Connessione Wi-Fi fallita entro il timeout di 20 secondi.");
    _staConnected = false;
    return false;
}

void NetworkManager::startFallbackAP() {
    if (_apActive) return;

    Serial.printf("[NET] Avvio Access Point di emergenza: SSID='%s'...\n", FALLBACK_AP_SSID);
    WiFi.mode(WIFI_AP_STA);

    bool ok = WiFi.softAP(FALLBACK_AP_SSID, FALLBACK_AP_PASS);
    if (ok) {
        _apActive = true;
        Serial.printf("[NET] Access Point attivo! Connettiti a '%s' e apri http://%s (oppure http://%s.local)\n",
                      FALLBACK_AP_SSID, WiFi.softAPIP().toString().c_str(), MDNS_HOSTNAME);
        setupMdnsAndNtp();
    } else {
        Serial.println("[NET] ERRORE CRITICO: Impossibile avviare il Fallback Access Point!");
    }
}

void NetworkManager::setupMdnsAndNtp() {
    if (!_mdnsStarted) {
        if (MDNS.begin(MDNS_HOSTNAME)) {
            MDNS.addService("http", "tcp", 80);
            _mdnsStarted = true;
            Serial.printf("[NET] mDNS avviato: http://%s.local\n", MDNS_HOSTNAME);
        } else {
            Serial.println("[NET] Avviso: Inizializzazione mDNS non riuscita.");
        }
    }

    if (_staConnected && !_ntpConfigured) {
        configTzTime(TZ_ITALY_POSIX, NTP_SERVER_1, NTP_SERVER_2);
        _ntpConfigured = true;
        Serial.printf("[NET] Client NTP avviato (%s, %s) | TZ: %s\n",
                      NTP_SERVER_1, NTP_SERVER_2, TZ_ITALY_POSIX);
    }
}

void NetworkManager::requestReconnect() {
    _reconnectRequested = true;
}

void NetworkManager::networkTask(void* pvParameters) {
    uint32_t lastRetryMs = millis();

    for (;;) {
        if (_reconnectRequested) {
            _reconnectRequested = false;
            Serial.println("[NET] Riconnessione Wi-Fi richiesta dall'utente...");
            WiFi.disconnect(false);
            vTaskDelay(pdMS_TO_TICKS(200));
            if (connectToSavedWiFi(WIFI_CONNECT_TIMEOUT_MS)) {
                setupMdnsAndNtp();
            } else {
                startFallbackAP();
            }
            lastRetryMs = millis();
        }

        wl_status_t status = WiFi.status();
        if (status == WL_CONNECTED) {
            if (!_staConnected) {
                _staConnected = true;
                Serial.printf("[NET] Connessione Wi-Fi ripristinata! IP: %s\n",
                              WiFi.localIP().toString().c_str());
                setupMdnsAndNtp();
                if (_apActive) {
                    WiFi.softAPdisconnect(true);
                    WiFi.mode(WIFI_STA);
                    _apActive = false;
                }
            }
        } else {
            if (_staConnected) {
                _staConnected = false;
                Serial.println("[NET] Connessione Wi-Fi persa. Attivazione AP di emergenza...");
                startFallbackAP();
            }

            // Se siamo in fallback AP ma c'è un SSID salvato, riprova silenziosamente ogni 60 secondi
            SystemConfig cfg = ConfigManager::getConfig();
            if (strlen(cfg.wifiSsid) > 0 && (millis() - lastRetryMs >= 60000)) {
                lastRetryMs = millis();
                Serial.printf("[NET] Tentativo periodico di riconnessione a '%s'...\n", cfg.wifiSsid);
                WiFi.begin(cfg.wifiSsid, cfg.wifiPassword);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

bool NetworkManager::getCurrentLocalTime(struct tm& timeinfo) {
    if (!getLocalTime(&timeinfo, 10)) {
        return false;
    }
    // Verifica che l'anno sia valido (>= 2024)
    return (timeinfo.tm_year + 1900) >= 2024;
}

bool NetworkManager::isTimeSynced() {
    struct tm timeinfo;
    return getCurrentLocalTime(timeinfo);
}

String NetworkManager::getFormattedTime() {
    struct tm timeinfo;
    if (!getCurrentLocalTime(timeinfo)) {
        return "--:--:--";
    }
    char buf[12];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    return String(buf);
}

String NetworkManager::getFormattedHoursMinutes() {
    struct tm timeinfo;
    if (!getCurrentLocalTime(timeinfo)) {
        return "--:--";
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
    return String(buf);
}

String NetworkManager::getFormattedSeconds() {
    struct tm timeinfo;
    if (!getCurrentLocalTime(timeinfo)) {
        return ":--";
    }
    char buf[6];
    snprintf(buf, sizeof(buf), ":%02d", timeinfo.tm_sec);
    return String(buf);
}

String NetworkManager::getFormattedDate() {
    struct tm timeinfo;
    if (!getCurrentLocalTime(timeinfo)) {
        return "In attesa di sincronizzazione NTP";
    }
    static const char* giorni[] = {"Dom", "Lun", "Mar", "Mer", "Gio", "Ven", "Sab"};
    char buf[32];
    int wday = (timeinfo.tm_wday >= 0 && timeinfo.tm_wday <= 6) ? timeinfo.tm_wday : 0;
    snprintf(buf, sizeof(buf), "%s %02d/%02d/%04d",
             giorni[wday], timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900);
    return String(buf);
}

bool NetworkManager::isStaConnected() {
    return _staConnected && (WiFi.status() == WL_CONNECTED);
}

bool NetworkManager::isApActive() {
    return _apActive;
}

String NetworkManager::getIpAddress() {
    if (isStaConnected()) {
        return WiFi.localIP().toString();
    }
    if (_apActive) {
        return WiFi.softAPIP().toString();
    }
    return "0.0.0.0";
}

String NetworkManager::getNetworkModeString() {
    if (isStaConnected()) {
        return String("Wi-Fi STA (") + WiFi.SSID() + ")";
    }
    if (_apActive) {
        return String("Access Point (") + FALLBACK_AP_SSID + ")";
    }
    return "Disconnesso";
}

int32_t NetworkManager::getRssi() {
    if (isStaConnected()) {
        return WiFi.RSSI();
    }
    return 0;
}
