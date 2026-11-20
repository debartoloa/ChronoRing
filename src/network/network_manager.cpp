#include "network_manager.h"
#include "../hardware_pins.h"
#include "../config_manager.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <ArduinoJson.h>
#include <esp_sntp.h>

static DNSServer s_dnsServer;
static const IPAddress AP_LOCAL_IP(192, 168, 4, 1);
static const IPAddress AP_GATEWAY_IP(192, 168, 4, 1);
static const IPAddress AP_SUBNET_MASK(255, 255, 255, 0);
static constexpr uint16_t CAPTIVE_DNS_PORT = 53;
static constexpr uint8_t  AP_FIXED_CHANNEL = 1;

volatile bool NetworkManager::_staConnected = false;
volatile bool NetworkManager::_apActive = false;
volatile bool NetworkManager::_mdnsStarted = false;
volatile bool NetworkManager::_ntpConfigured = false;
volatile bool NetworkManager::_reconnectRequested = false;
volatile bool NetworkManager::_apRestartRequested = false;
volatile bool NetworkManager::_forceStartApRequested = false;
volatile bool NetworkManager::_scanInProgress = false;
volatile uint32_t NetworkManager::_lastScanTimestampMs = 0;
volatile bool NetworkManager::_hasConnectedStaOnce = false;
volatile bool NetworkManager::_apTimedOut = false;
uint32_t NetworkManager::_apLastActivityMs = 0;
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

bool NetworkManager::isSavedNetworkVisible(const char* targetSsid, int32_t* outRssi) {
    if (!targetSsid || strlen(targetSsid) == 0) {
        return false;
    }

    // Garantiamo che l'interfaccia STA sia attiva per poter scansionare l'etere
    wifi_mode_t curMode = WiFi.getMode();
    if (curMode == WIFI_MODE_NULL) {
        WiFi.mode(WIFI_STA);
        vTaskDelay(pdMS_TO_TICKS(50));
    } else if (curMode == WIFI_MODE_AP) {
        WiFi.mode(WIFI_AP_STA);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    Serial.printf("[NET] Pre-scan Wi-Fi per verificare la presenza di '%s'...\n", targetSsid);
    int n = WiFi.scanNetworks(false, false, false, 150);
    bool found = false;
    int32_t bestRssi = -127;

    if (n > 0) {
        for (int i = 0; i < n; ++i) {
            if (WiFi.SSID(i) == targetSsid) {
                found = true;
                int32_t r = WiFi.RSSI(i);
                if (r > bestRssi) {
                    bestRssi = r;
                }
            }
        }
    }
    WiFi.scanDelete();

    if (outRssi) {
        *outRssi = bestRssi;
    }

    if (found) {
        Serial.printf("[NET] Rete '%s' rilevata (RSSI: %d dBm).\n", targetSsid, bestRssi);
    } else {
        Serial.printf("[NET] Rete '%s' NON trovata nell'etere.\n", targetSsid);
    }
    return found;
}

String NetworkManager::getOrStartWifiScanJson() {
    int16_t n = WiFi.scanComplete();

    if (n == WIFI_SCAN_RUNNING) {
        return "{\"status\":\"running\",\"networks\":[]}";
    }

    if (n >= 0) {
        _scanInProgress = false;
        _lastScanTimestampMs = millis();

        JsonDocument doc;
        doc["status"] = "done";
        JsonArray arr = doc["networks"].to<JsonArray>();

        for (int i = 0; i < n; ++i) {
            String ssid = WiFi.SSID(i);
            if (ssid.length() == 0) continue;

            int32_t rssi = WiFi.RSSI(i);
            bool open = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);

            // Deduplica eventuali ripetitori con lo stesso SSID mantenendo il segnale più forte
            bool duplicate = false;
            for (JsonObject existing : arr) {
                if (ssid == (existing["ssid"] | "")) {
                    duplicate = true;
                    if (rssi > (existing["rssi"] | -127)) {
                        existing["rssi"] = rssi;
                        existing["open"] = open;
                    }
                    break;
                }
            }
            if (!duplicate) {
                JsonObject obj = arr.add<JsonObject>();
                obj["ssid"] = ssid;
                obj["rssi"] = rssi;
                obj["open"] = open;
            }
        }
        WiFi.scanDelete();

        String out;
        serializeJson(doc, out);
        return out;
    }

    // n == WIFI_SCAN_FAILED (-2): nessuna scansione completata/in corso oppure scan scaduta
    if (_scanInProgress && (millis() - _lastScanTimestampMs > 1000)) {
        _scanInProgress = false;
        return "{\"status\":\"error\",\"networks\":[]}";
    }

    wifi_mode_t curMode = WiFi.getMode();
    if (curMode == WIFI_MODE_NULL) {
        WiFi.mode(WIFI_STA);
    } else if (curMode == WIFI_MODE_AP) {
        WiFi.mode(WIFI_AP_STA);
    }

    _scanInProgress = true;
    _lastScanTimestampMs = millis();

    // Avvio scansione asincrona (200 ms/canale -> timeout interno 4000 ms): ritorna subito senza bloccare HTTP
    int16_t startRes = WiFi.scanNetworks(true, false, false, 200);
    if (startRes == WIFI_SCAN_FAILED) {
        _scanInProgress = false;
        return "{\"status\":\"error\",\"networks\":[]}";
    }

    return "{\"status\":\"running\",\"networks\":[]}";
}

bool NetworkManager::connectToSavedWiFi(uint32_t timeoutMs) {
    SystemConfig cfg = ConfigManager::getConfig();
    if (strlen(cfg.wifiSsid) == 0) {
        Serial.println("[NET] Nessun SSID Wi-Fi configurato in /config.json.");
        _staConnected = false;
        return false;
    }

    // Verifica preventiva: evita di bloccare 20s o causare channel-hopping se la rete non esiste
    if (!isSavedNetworkVisible(cfg.wifiSsid)) {
        Serial.printf("[NET] Avvio immediato Hotspot '%s' (rete salvata non presente).\n", FALLBACK_AP_SSID);
        WiFi.setAutoReconnect(false);
        WiFi.disconnect(false, true);
        _staConnected = false;
        return false;
    }

    Serial.printf("[NET] Connessione alla rete Wi-Fi '%s' in corso (timeout %u s)...\n",
                  cfg.wifiSsid, timeoutMs / 1000);

    WiFi.mode(_apActive ? WIFI_AP_STA : WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(cfg.wifiSsid, cfg.wifiPassword);

    uint32_t startMs = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - startMs < timeoutMs)) {
        if (_apActive) {
            s_dnsServer.processNextRequest();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        _staConnected = true;
        _hasConnectedStaOnce = true;
        _apTimedOut = false;
        if (_apActive || WiFi.getMode() != WIFI_STA) {
            Serial.println("[NET] Connessione STA stabilita: disattivazione totale Access Point di fallback.");
            stopFallbackAP();
        }
        Serial.printf("[NET] Connesso! IP: %s | Hostname: http://%s.local | RSSI: %d dBm\n",
                      WiFi.localIP().toString().c_str(), MDNS_HOSTNAME, WiFi.RSSI());
        return true;
    }

    Serial.println("[NET] ATTENZIONE: Connessione Wi-Fi fallita entro il timeout. Arresto ricerca STA.");
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false, true);
    _staConnected = false;
    return false;
}

void NetworkManager::startFallbackAP() {
    if (_apActive) return;

    SystemConfig cfg = ConfigManager::getConfig();
    const char* apSsid = (strlen(cfg.apSsid) > 0) ? cfg.apSsid : FALLBACK_AP_SSID;
    const char* apPass = (strlen(cfg.apPassword) >= 8) ? cfg.apPassword : nullptr;

    Serial.printf("[NET] Avvio Access Point di emergenza: SSID='%s' (%s)...\n",
                  apSsid, apPass ? "PROTETTO WPA2" : "AP aperto");

    // CRITICO: Ferma qualsiasi ricerca STA residua per evitare channel-hopping che rompe il DHCP su AP
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false, true);
    vTaskDelay(pdMS_TO_TICKS(100));

    WiFi.mode(WIFI_AP_STA);
    vTaskDelay(pdMS_TO_TICKS(50));

    bool ok = WiFi.softAP(apSsid, apPass, AP_FIXED_CHANNEL, 0, 4);
    if (ok) {
        vTaskDelay(pdMS_TO_TICKS(100));
        // Imposta esplicitamente IP 192.168.4.1 e avvia il DHCP Server interno (range 192.168.4.2 - 192.168.4.12)
        if (!WiFi.softAPConfig(AP_LOCAL_IP, AP_GATEWAY_IP, AP_SUBNET_MASK)) {
            Serial.println("[NET] AVVISO: softAPConfig ha restituito false, verifica netif AP.");
        }

        // Avvia Captive Portal DNS sulla porta 53 per indirizzare tutti i domini a 192.168.4.1
        s_dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
        s_dnsServer.start(CAPTIVE_DNS_PORT, "*", AP_LOCAL_IP);

        _apActive = true;
        _apTimedOut = false;
        _apLastActivityMs = millis();
        Serial.printf("[NET] Hotspot attivo! SSID: '%s' | IP/Gateway/DHCP: %s | Canale: %u\n",
                      apSsid, WiFi.softAPIP().toString().c_str(), AP_FIXED_CHANNEL);
        Serial.printf("[NET] Captive Portal DNS attivo -> apri http://%s (o http://%s.local)\n",
                      WiFi.softAPIP().toString().c_str(), MDNS_HOSTNAME);
        setupMdnsAndNtp();
    } else {
        Serial.println("[NET] ERRORE CRITICO: Impossibile avviare il Fallback Access Point!");
    }
}

void NetworkManager::stopFallbackAP() {
    s_dnsServer.stop();
    if (_apActive || WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA) {
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
    }
    _apActive = false;
}

void NetworkManager::requestManualApStart() {
    _apTimedOut = false;
    _forceStartApRequested = true;
}

bool NetworkManager::isApTimedOut() {
    return _apTimedOut;
}

bool NetworkManager::isApLocked() {
    return _hasConnectedStaOnce && ConfigManager::isApLockAfterSta();
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

void NetworkManager::requestApRestart() {
    _apRestartRequested = true;
}

void NetworkManager::networkTask(void* pvParameters) {
    uint32_t lastRetryMs = millis();
    uint32_t disconnectStartMs = 0;

    for (;;) {
        if (_apActive) {
            s_dnsServer.processNextRequest();
        }

        // Timeout di sicurezza per eventuali scansioni asincrone mai lette dal client
        if (_scanInProgress) {
            int16_t sc = WiFi.scanComplete();
            if (sc >= 0 && (millis() - _lastScanTimestampMs >= 15000)) {
                _scanInProgress = false;
                WiFi.scanDelete();
            } else if (sc == WIFI_SCAN_FAILED && (millis() - _lastScanTimestampMs >= 15000)) {
                _scanInProgress = false;
            }
        }

        if (_forceStartApRequested) {
            _forceStartApRequested = false;
            if (!_staConnected) {
                Serial.println("[NET] Attivazione manuale Hotspot Access Point richiesta.");
                startFallbackAP();
            } else {
                Serial.println("[NET] Richiesta attivazione AP ignorata: già connesso in STA.");
            }
        }

        if (_apRestartRequested) {
            _apRestartRequested = false;
            if (_apActive) {
                Serial.println("[NET] Riavvio Hotspot Access Point con le nuove credenziali...");
                vTaskDelay(pdMS_TO_TICKS(400)); // Lascia completare la risposta HTTP prima di chiudere il vecchio AP
                stopFallbackAP();
                vTaskDelay(pdMS_TO_TICKS(150));
                startFallbackAP();
            }
        }

        if (_reconnectRequested) {
            _reconnectRequested = false;
            disconnectStartMs = 0;
            Serial.println("[NET] Riconnessione Wi-Fi richiesta dall'utente...");
            vTaskDelay(pdMS_TO_TICKS(400)); // Lascia completare l'invio della risposta HTTP alla Dashboard
            WiFi.disconnect(false);
            vTaskDelay(pdMS_TO_TICKS(200));
            if (connectToSavedWiFi(WIFI_CONNECT_TIMEOUT_MS)) {
                setupMdnsAndNtp();
            } else if (!isApLocked() && !_apTimedOut) {
                startFallbackAP();
            }
            lastRetryMs = millis();
        }

        wl_status_t status = WiFi.status();
        bool scanGraceActive = _scanInProgress ||
                               (WiFi.scanComplete() == WIFI_SCAN_RUNNING) ||
                               (_lastScanTimestampMs > 0 && (millis() - _lastScanTimestampMs < 5000));

        if (status == WL_CONNECTED) {
            disconnectStartMs = 0;
            _hasConnectedStaOnce = true;
            _apTimedOut = false;
            if (!_staConnected) {
                _staConnected = true;
                Serial.printf("[NET] Connessione Wi-Fi ripristinata! IP: %s\n",
                              WiFi.localIP().toString().c_str());
                setupMdnsAndNtp();
            }
            if (_apActive || (!scanGraceActive && WiFi.getMode() != WIFI_STA)) {
                Serial.println("[NET] Rete STA attiva: disattivazione interfaccia SoftAP per sicurezza.");
                stopFallbackAP();
                WiFi.setAutoReconnect(true);
            }
        } else {
            // Se l'Hotspot AP è attivo, controlla il timeout di inattività senza client collegati
            if (_apActive) {
                if (WiFi.softAPgetStationNum() > 0) {
                    _apLastActivityMs = millis();
                } else {
                    uint16_t apTmoMin = ConfigManager::getApTimeoutMin();
                    if (apTmoMin > 0 && (millis() - _apLastActivityMs >= static_cast<uint32_t>(apTmoMin) * 60000UL)) {
                        Serial.printf("[NET] Timeout Hotspot AP (%u min senza client): spegnimento automatico per sicurezza.\n",
                                      apTmoMin);
                        stopFallbackAP();
                        _apTimedOut = true;
                    }
                }
            }

            if (_staConnected) {
                if (scanGraceActive) {
                    // Durante o subito dopo una scansione Wi-Fi non consideriamo caduta la connessione
                    disconnectStartMs = 0;
                } else {
                    if (disconnectStartMs == 0) {
                        disconnectStartMs = millis();
                    } else if (millis() - disconnectStartMs >= 8000) {
                        disconnectStartMs = 0;
                        _staConnected = false;
                        if (isApLocked()) {
                            Serial.println("[NET] Connessione Wi-Fi persa (>8s). Hotspot AP bloccato per sicurezza (attesa ritorno rete STA).");
                            stopFallbackAP();
                            WiFi.setAutoReconnect(true);
                        } else {
                            Serial.println("[NET] Connessione Wi-Fi persa (>8s). Attivazione AP di emergenza...");
                            startFallbackAP();
                        }
                        lastRetryMs = millis();
                    }
                }
            } else {
                disconnectStartMs = 0;

                // Riprova ogni 60s SOLO se nessun utente è attualmente collegato all'Hotspot,
                // nessuna scansione manuale è in corso e la rete salvata torna visibile.
                if (!scanGraceActive && (!_apActive || WiFi.softAPgetStationNum() == 0)) {
                    SystemConfig cfg = ConfigManager::getConfig();
                    if (strlen(cfg.wifiSsid) > 0 && (millis() - lastRetryMs >= 60000)) {
                        lastRetryMs = millis();
                        if (isSavedNetworkVisible(cfg.wifiSsid)) {
                            Serial.printf("[NET] Rete '%s' nuovamente visibile: tentativo di riconnessione...\n", cfg.wifiSsid);
                            if (connectToSavedWiFi(WIFI_CONNECT_TIMEOUT_MS)) {
                                setupMdnsAndNtp();
                            } else if (!isApLocked() && !_apTimedOut) {
                                startFallbackAP();
                            }
                        }
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(_apActive ? 50 : 1000));
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
        return String("Access Point (") + WiFi.softAPSSID() + ")";
    }
    if (_apTimedOut) {
        return "Hotspot Spento (Timeout)";
    }
    if (isApLocked()) {
        return "Riconnessione Wi-Fi (AP Bloccato)";
    }
    return "Disconnesso";
}

int32_t NetworkManager::getRssi() {
    if (isStaConnected()) {
        return WiFi.RSSI();
    }
    return 0;
}
