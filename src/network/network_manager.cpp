#include "network_manager.h"
#include "../hardware_pins.h"
#include "../config_manager.h"
#include "../gui/gui_manager.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <NetBIOS.h>
#include <DNSServer.h>
#include <ArduinoJson.h>
#include <esp_sntp.h>
#include <esp_wifi.h>
#include <mdns.h>
#include <esp_netif.h>
#include <esp_netif_net_stack.h>
#include <lwip/tcpip.h>
#include <lwip/netif.h>
#include <lwip/igmp.h>
#include <lwip/udp.h>
#include <lwip/pbuf.h>

static DNSServer s_dnsServer;
static const IPAddress AP_LOCAL_IP(192, 168, 4, 1);
static const IPAddress AP_GATEWAY_IP(192, 168, 4, 1);
static const IPAddress AP_SUBNET_MASK(255, 255, 255, 0);
static constexpr uint16_t CAPTIVE_DNS_PORT = 53;
static constexpr uint8_t  AP_FIXED_CHANNEL = 1;

volatile bool NetworkManager::_staConnected = false;
volatile bool NetworkManager::_apActive = false;
volatile bool NetworkManager::_mdnsStarted = false;
volatile bool NetworkManager::_nbnsStarted = false;
volatile bool NetworkManager::_ntpConfigured = false;
volatile bool NetworkManager::_reconnectRequested = false;
volatile bool NetworkManager::_apRestartRequested = false;
volatile bool NetworkManager::_forceStartApRequested = false;
volatile bool NetworkManager::_scanInProgress = false;
volatile uint32_t NetworkManager::_lastScanTimestampMs = 0;
volatile bool NetworkManager::_hasConnectedStaOnce = false;
volatile bool NetworkManager::_apTimedOut = false;
uint32_t NetworkManager::_apLastActivityMs = 0;
volatile bool NetworkManager::_mdnsRefreshRequested = false;
volatile bool NetworkManager::_staLostIpEvent = false;
volatile uint32_t NetworkManager::_staDisconnectCount = 0;
volatile uint32_t NetworkManager::_mdnsRestartCount = 0;
volatile uint32_t NetworkManager::_mdnsKeepaliveCount = 0;
volatile bool NetworkManager::_dnsEverWorked = false;
volatile uint8_t NetworkManager::_consecutiveHealthFails = 0;
volatile bool NetworkManager::_mcastRxEverWorked = false;
volatile uint8_t NetworkManager::_consecutiveMcastRxFails = 0;
volatile uint32_t NetworkManager::_groupRxPacketCount = 0;
volatile uint32_t NetworkManager::_lastGroupRxMs = 0;
uint32_t NetworkManager::_lastMdnsKeepaliveMs = 0;
bool NetworkManager::_wifiEventRegistered = false;
volatile bool NetworkManager::_persistIpRequested = false;
TaskHandle_t NetworkManager::_netTaskHandle = nullptr;

constexpr const char* NetworkManager::MDNS_HOSTNAME;
constexpr const char* NetworkManager::FALLBACK_AP_SSID;
constexpr const char* NetworkManager::FALLBACK_AP_PASS;
constexpr uint32_t NetworkManager::WIFI_CONNECT_TIMEOUT_MS;
constexpr uint32_t NetworkManager::MDNS_KEEPALIVE_INTERVAL_MS;
constexpr uint32_t NetworkManager::WIFI_HEALTH_CHECK_INTERVAL_MS;
constexpr uint8_t  NetworkManager::WIFI_ZOMBIE_FAIL_THRESHOLD;
constexpr uint8_t  NetworkManager::WIFI_MCAST_FAIL_THRESHOLD;
constexpr uint32_t NetworkManager::GROUP_RX_SOFT_TIMEOUT_MS;
constexpr uint32_t NetworkManager::GROUP_RX_HARD_TIMEOUT_MS;
constexpr const char* NetworkManager::TZ_ITALY_POSIX;
constexpr const char* NetworkManager::NTP_SERVER_1;
constexpr const char* NetworkManager::NTP_SERVER_2;

// Hook leggero su netif->input per rilevare in tempo reale la ricezione di qualsiasi
// frame Ethernet L2 Broadcast (FF:..) o Multicast (01:..) senza dipendere da altri peer mDNS.
static netif_input_fn s_origStaInput = nullptr;
static err_t s_staGroupRxSpyInput(struct pbuf* p, struct netif* inp) {
    if (p != nullptr && p->len >= 14 && p->payload != nullptr) {
        const uint8_t* ethDest = static_cast<const uint8_t*>(p->payload);
        if ((ethDest[0] & 0x01) != 0) {
            NetworkManager::recordGroupRxPacket();
        }
    }
    return s_origStaInput ? s_origStaInput(p, inp) : ERR_IF;
}

// Eseguito sul thread TCPIP lwIP per evitare data-race su netif/IGMP/PCB:
// invia un IGMP Membership Report aggiornato e un annuncio mDNS unsolicited (RFC 6762).
static void s_mdnsIgmpKeepaliveCb(void* arg) {
    bool forceRejoin = (arg != nullptr);

    esp_netif_t* staNetif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!staNetif) return;

    struct netif* lwipNetif = static_cast<struct netif*>(esp_netif_get_netif_impl(staNetif));
    if (!lwipNetif || !netif_is_up(lwipNetif) || ip4_addr_isany_val(*netif_ip4_addr(lwipNetif))) {
        return;
    }

    if (lwipNetif->input != nullptr && lwipNetif->input != s_staGroupRxSpyInput) {
        s_origStaInput = lwipNetif->input;
        lwipNetif->input = s_staGroupRxSpyInput;
    }

    ip4_addr_t mdnsGroup;
    IP4_ADDR(&mdnsGroup, 224, 0, 0, 251);

    if (forceRejoin) {
        igmp_leavegroup_netif(lwipNetif, &mdnsGroup);
        igmp_joingroup_netif(lwipNetif, &mdnsGroup);
    } else if (igmp_lookfor_group(lwipNetif, &mdnsGroup) == nullptr) {
        igmp_joingroup_netif(lwipNetif, &mdnsGroup);
    }
    igmp_report_groups(lwipNetif);

    size_t hlen = strlen(NetworkManager::MDNS_HOSTNAME);
    if (hlen == 0 || hlen > 63) return;

    uint8_t pkt[96];
    // Header DNS mDNS: ID=0, Flags=0x8400 (QR=1, AA=1), QDCOUNT=0, ANCOUNT=1, NSCOUNT=0, ARCOUNT=0
    pkt[0]  = 0x00; pkt[1]  = 0x00;
    pkt[2]  = 0x84; pkt[3]  = 0x00;
    pkt[4]  = 0x00; pkt[5]  = 0x00;
    pkt[6]  = 0x00; pkt[7]  = 0x01;
    pkt[8]  = 0x00; pkt[9]  = 0x00;
    pkt[10] = 0x00; pkt[11] = 0x00;

    size_t pos = 12;
    pkt[pos++] = static_cast<uint8_t>(hlen);
    memcpy(&pkt[pos], NetworkManager::MDNS_HOSTNAME, hlen);
    pos += hlen;
    pkt[pos++] = 5;
    pkt[pos++] = 'l'; pkt[pos++] = 'o'; pkt[pos++] = 'c'; pkt[pos++] = 'a'; pkt[pos++] = 'l';
    pkt[pos++] = 0x00;

    // RR Type A (0x0001), Class IN + Cache-Flush (0x8001), TTL 120s (0x00000078), RDLENGTH 4
    pkt[pos++] = 0x00; pkt[pos++] = 0x01;
    pkt[pos++] = 0x80; pkt[pos++] = 0x01;
    pkt[pos++] = 0x00; pkt[pos++] = 0x00; pkt[pos++] = 0x00; pkt[pos++] = 0x78;
    pkt[pos++] = 0x00; pkt[pos++] = 0x04;
    memcpy(&pkt[pos], &netif_ip4_addr(lwipNetif)->addr, 4);
    pos += 4;

    struct udp_pcb* pcb = udp_new();
    if (!pcb) return;

    pcb->local_port = 5353;
    pcb->ttl = 255;
#if LWIP_MULTICAST_TX_OPTIONS
    pcb->mcast_ttl = 255;
#endif

    struct pbuf* p = pbuf_alloc(PBUF_TRANSPORT, static_cast<u16_t>(pos), PBUF_RAM);
    if (p) {
        memcpy(p->payload, pkt, pos);
        ip_addr_t mdnsDst;
        IP_ADDR4(&mdnsDst, 224, 0, 0, 251);
        udp_sendto_if_src(pcb, p, &mdnsDst, 5353, lwipNetif, netif_ip_addr4(lwipNetif));
        pbuf_free(p);
    }
    udp_remove(pcb);
}

static void s_onWifiEvent(WiFiEvent_t event) {
    NetworkManager::notifyWifiEvent(static_cast<int>(event));
}

void NetworkManager::notifyWifiEvent(int eventId) {
    switch (static_cast<WiFiEvent_t>(eventId)) {
        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
            WiFi.setSleep(false);
            break;
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            _staLostIpEvent = false;
            _mdnsRefreshRequested = true;
            _persistIpRequested = true;
            break;
        case ARDUINO_EVENT_WIFI_STA_LOST_IP:
            _staLostIpEvent = true;
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            _staDisconnectCount = _staDisconnectCount + 1;
            break;
        default:
            break;
    }
}

void NetworkManager::disableWifiPowerSave() {
    // ChronoRing è alimentato da rete: disabilita sempre il Modem Sleep per non perdere pacchetti multicast mDNS
    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);
}

void NetworkManager::begin() {
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    WiFi.setHostname(MDNS_HOSTNAME);

    if (!_wifiEventRegistered) {
        WiFi.onEvent(s_onWifiEvent);
        _wifiEventRegistered = true;
    }

    disableWifiPowerSave();

    // Configura subito il fuso orario POSIX anche prima della connessione
    setenv("TZ", TZ_ITALY_POSIX, 1);
    tzset();

    // Eseguiamo il primo tentativo di connessione o fallback AP all'avvio
    if (!connectToSavedWiFi(WIFI_CONNECT_TIMEOUT_MS)) {
        startFallbackAP();
    } else {
        setupMdnsAndNtp(true);
        _mdnsRefreshRequested = false; // Evita un secondo restart mDNS 10ms dopo il boot per l'evento GOT_IP
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

bool NetworkManager::connectSingleSsid(const char* ssid, const char* password,
                                       const SystemConfig& cfg, uint32_t timeoutMs) {
    if (!ssid || strlen(ssid) == 0) {
        return false;
    }

    Serial.printf("[NET] Connessione alla rete Wi-Fi '%s' in corso (timeout %u s)...\n",
                  ssid, timeoutMs / 1000);

    WiFi.mode(_apActive ? WIFI_AP_STA : WIFI_STA);
    if (cfg.staticIpEnabled) {
        IPAddress sip, sgw, smask, sdns1, sdns2;
        if (sip.fromString(cfg.staticIp) &&
            sgw.fromString(cfg.staticGateway) &&
            smask.fromString(cfg.staticSubnet)) {
            if (strlen(cfg.staticDns1) > 0) sdns1.fromString(cfg.staticDns1);
            if (strlen(cfg.staticDns2) > 0) sdns2.fromString(cfg.staticDns2);
            if (!WiFi.config(sip, sgw, smask, sdns1, sdns2)) {
                Serial.println("[NET] ERRORE applicazione IP statico, uso DHCP.");
                WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
            } else {
                Serial.printf("[NET] IP Statico applicato: %s (GW %s)\n", cfg.staticIp, cfg.staticGateway);
            }
        } else {
            Serial.println("[NET] Configurazione IP statico non valida, uso DHCP.");
            WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
        }
    } else {
        WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
    }
    WiFi.setAutoReconnect(true);
    disableWifiPowerSave();
    WiFi.begin(ssid, password ? password : "");

    uint32_t startMs = millis();
    while ((WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress(0, 0, 0, 0)) &&
           (millis() - startMs < timeoutMs)) {
        if (_apActive) {
            s_dnsServer.processNextRequest();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
        _staConnected = true;
        _staLostIpEvent = false;
        _hasConnectedStaOnce = true;
        _apTimedOut = false;
        _consecutiveHealthFails = 0;
        if (_apActive || WiFi.getMode() != WIFI_STA) {
            Serial.println("[NET] Connessione STA stabilita: disattivazione totale Access Point di fallback.");
            stopFallbackAP();
        }
        disableWifiPowerSave();
        _persistIpRequested = false;
        persistCurrentIpIfChanged();
        Serial.printf("[NET] Connesso a '%s'! IP: %s | Hostname: http://%s.local | RSSI: %d dBm\n",
                      ssid, WiFi.localIP().toString().c_str(), MDNS_HOSTNAME, WiFi.RSSI());
        return true;
    }

    Serial.printf("[NET] ATTENZIONE: Connessione a '%s' fallita entro il timeout.\n", ssid);
    if (_apActive || (!isApLocked() && !_apTimedOut)) {
        // Evita channel-hopping mentre l'Hotspot di emergenza è attivo
        WiFi.setAutoReconnect(false);
        WiFi.disconnect(false, true);
    } else {
        // Con AP bloccato manteniamo autoReconnect attivo affinché lo stack possa riagganciarsi
        WiFi.disconnect(false, false);
        WiFi.setAutoReconnect(true);
    }
    _staConnected = false;
    return false;
}

bool NetworkManager::connectToSavedWiFi(uint32_t timeoutMs, bool skipPreScan) {
    SystemConfig cfg = ConfigManager::getConfig();
    std::vector<SavedWifiNetwork> candidates = cfg.savedWifiNetworks;
    if (candidates.empty() && strlen(cfg.wifiSsid) > 0) {
        SavedWifiNetwork fallback;
        strlcpy(fallback.ssid, cfg.wifiSsid, sizeof(fallback.ssid));
        strlcpy(fallback.password, cfg.wifiPassword, sizeof(fallback.password));
        candidates.push_back(fallback);
    }

    if (candidates.empty()) {
        Serial.println("[NET] Nessun SSID Wi-Fi configurato in /config.json.");
        _staConnected = false;
        return false;
    }

    if (skipPreScan) {
        if (connectSingleSsid(candidates[0].ssid, candidates[0].password, cfg, timeoutMs)) {
            return true;
        }
        if (candidates.size() <= 1) {
            return false;
        }
    }

    wifi_mode_t curMode = WiFi.getMode();
    if (curMode == WIFI_MODE_NULL) {
        WiFi.mode(WIFI_STA);
        vTaskDelay(pdMS_TO_TICKS(50));
    } else if (curMode == WIFI_MODE_AP) {
        WiFi.mode(WIFI_AP_STA);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    Serial.printf("[NET] Scansione etere per %u reti Wi-Fi salvate...\n", (unsigned)candidates.size());
    int n = WiFi.scanNetworks(false, false, false, 150);

    struct MatchedWifi {
        size_t index;
        int32_t bestRssi;
    };
    std::vector<MatchedWifi> visibleMatches;

    if (n > 0) {
        for (size_t idx = 0; idx < candidates.size(); ++idx) {
            if (skipPreScan && idx == 0) continue;
            int32_t bestRssi = -127;
            bool found = false;
            for (int i = 0; i < n; ++i) {
                if (WiFi.SSID(i) == candidates[idx].ssid) {
                    found = true;
                    int32_t r = WiFi.RSSI(i);
                    if (r > bestRssi) {
                        bestRssi = r;
                    }
                }
            }
            if (found) {
                visibleMatches.push_back({idx, bestRssi});
            }
        }
    }
    WiFi.scanDelete();

    for (size_t i = 0; i < visibleMatches.size(); ++i) {
        size_t best = i;
        for (size_t j = i + 1; j < visibleMatches.size(); ++j) {
            if (visibleMatches[j].bestRssi > visibleMatches[best].bestRssi ||
                (visibleMatches[j].bestRssi == visibleMatches[best].bestRssi &&
                 visibleMatches[j].index < visibleMatches[best].index)) {
                best = j;
            }
        }
        if (best != i) {
            MatchedWifi tmp = visibleMatches[i];
            visibleMatches[i] = visibleMatches[best];
            visibleMatches[best] = tmp;
        }
    }

    if (visibleMatches.empty()) {
        Serial.println("[NET] Nessuna delle reti Wi-Fi salvate rilevata nell'etere.");
        if (!isApLocked() && !_apTimedOut) {
            WiFi.setAutoReconnect(false);
            WiFi.disconnect(false, true);
        }
        _staConnected = false;
        return false;
    }

    for (const auto& m : visibleMatches) {
        const SavedWifiNetwork& net = candidates[m.index];
        Serial.printf("[NET] Rete salvata visibile: '%s' (%ld dBm) -> tentativo connessione...\n",
                      net.ssid, (long)m.bestRssi);
        if (connectSingleSsid(net.ssid, net.password, cfg, timeoutMs)) {
            ConfigManager::promoteSavedWiFi(net.ssid);
            return true;
        }
    }

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
        disableWifiPowerSave();
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

void NetworkManager::sendMdnsAndIgmpKeepalive(bool forceIgmpRejoin) {
    tcpip_callback(s_mdnsIgmpKeepaliveCb, forceIgmpRejoin ? reinterpret_cast<void*>(1) : nullptr);
    _lastMdnsKeepaliveMs = millis();
    _mdnsKeepaliveCount = _mdnsKeepaliveCount + 1;
}

void NetworkManager::restartMdns() {
    if (_mdnsStarted) {
        MDNS.end();
        _mdnsStarted = false;
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (MDNS.begin(MDNS_HOSTNAME)) {
        MDNS.setInstanceName("ChronoRing");
        MDNS.addService("http", "tcp", 80);
        _mdnsStarted = true;
        _mdnsRestartCount = _mdnsRestartCount + 1;
        _lastMdnsKeepaliveMs = millis();
        if (_staConnected) {
            sendMdnsAndIgmpKeepalive(false);
        }
        Serial.printf("[NET] mDNS (ri)avviato: http://%s.local | IP: %s (refresh #%lu)\n",
                      MDNS_HOSTNAME, getIpAddress().c_str(), (unsigned long)_mdnsRestartCount);
    } else {
        Serial.println("[NET] Avviso: Inizializzazione mDNS non riuscita.");
    }
}

void NetworkManager::setupMdnsAndNtp(bool forceMdnsRestart) {
    if (forceMdnsRestart || !_mdnsStarted) {
        restartMdns();
    }

    if (_staConnected) {
        if (forceMdnsRestart && _nbnsStarted) {
            NBNS.end();
            _nbnsStarted = false;
        }
        if (!_nbnsStarted && NBNS.begin(MDNS_HOSTNAME)) {
            _nbnsStarted = true;
            Serial.printf("[NET] NetBIOS avviato: http://%s\n", MDNS_HOSTNAME);
        }
    }

    if (_staConnected && !_ntpConfigured) {
        configTzTime(TZ_ITALY_POSIX, NTP_SERVER_1, NTP_SERVER_2);
        _ntpConfigured = true;
        Serial.printf("[NET] Client NTP avviato (%s, %s) | TZ: %s\n",
                      NTP_SERVER_1, NTP_SERVER_2, TZ_ITALY_POSIX);
    }
}

void NetworkManager::requestMdnsRefresh() {
    _mdnsRefreshRequested = true;
}

void NetworkManager::reportInternetHealth(bool ok) {
    if (ok) {
        _dnsEverWorked = true;
        _consecutiveHealthFails = 0;
    } else {
        if (_dnsEverWorked && _consecutiveHealthFails < 255) {
            _consecutiveHealthFails = _consecutiveHealthFails + 1;
        }
    }
}

uint32_t NetworkManager::getStaDisconnectCount() {
    return _staDisconnectCount;
}

uint32_t NetworkManager::getMdnsRestartCount() {
    return _mdnsRestartCount;
}

uint32_t NetworkManager::getMdnsKeepaliveCount() {
    return _mdnsKeepaliveCount;
}

bool NetworkManager::hasMulticastRxPeers() {
    return _mcastRxEverWorked;
}

void NetworkManager::recordGroupRxPacket() {
    _groupRxPacketCount = _groupRxPacketCount + 1;
    uint32_t now = millis();
    _lastGroupRxMs = (now != 0) ? now : 1;
}

uint32_t NetworkManager::getGroupRxPacketCount() {
    return _groupRxPacketCount;
}

int32_t NetworkManager::getLastGroupRxAgeSec() {
    uint32_t lastMs = _lastGroupRxMs;
    if (lastMs == 0) {
        return -1;
    }
    return static_cast<int32_t>((millis() - lastMs) / 1000UL);
}

void NetworkManager::requestReconnect() {
    _reconnectRequested = true;
}

void NetworkManager::requestApRestart() {
    _apRestartRequested = true;
}

void NetworkManager::networkTask(void* pvParameters) {
    (void)pvParameters;
    uint32_t lastRetryMs = millis();
    uint32_t lastHealthCheckMs = millis();
    uint32_t lastGroupCheckMs = millis();
    uint8_t groupRxRecoveryStage = 0;
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
                setupMdnsAndNtp(true);
                _mdnsRefreshRequested = false;
            } else if (!isApLocked() && !_apTimedOut) {
                startFallbackAP();
            }
            lastRetryMs = millis();
            lastHealthCheckMs = millis();
        }

        wl_status_t status = WiFi.status();
        bool scanGraceActive = _scanInProgress ||
                               (WiFi.scanComplete() == WIFI_SCAN_RUNNING) ||
                               (_lastScanTimestampMs > 0 && (millis() - _lastScanTimestampMs < 5000));
        bool linkUp = (status == WL_CONNECTED) &&
                      (WiFi.localIP() != IPAddress(0, 0, 0, 0)) &&
                      !_staLostIpEvent;

        if (linkUp) {
            disconnectStartMs = 0;
            _hasConnectedStaOnce = true;
            _apTimedOut = false;
            if (!_staConnected) {
                _staConnected = true;
                _consecutiveHealthFails = 0;
                _consecutiveMcastRxFails = 0;
                disableWifiPowerSave();
                Serial.printf("[NET] Connessione Wi-Fi ripristinata! IP: %s\n",
                              WiFi.localIP().toString().c_str());
                _persistIpRequested = false;
                groupRxRecoveryStage = 0;
                uint32_t nowRx = millis();
                _lastGroupRxMs = (nowRx != 0) ? nowRx : 1;
                persistCurrentIpIfChanged();
                setupMdnsAndNtp(true);
                _mdnsRefreshRequested = false;
                lastHealthCheckMs = millis();
                lastGroupCheckMs = millis();
            }
            if (_persistIpRequested) {
                _persistIpRequested = false;
                persistCurrentIpIfChanged();
            }
            if (_apActive || (!scanGraceActive && WiFi.getMode() != WIFI_STA)) {
                Serial.println("[NET] Rete STA attiva: disattivazione interfaccia SoftAP per sicurezza.");
                stopFallbackAP();
                WiFi.setAutoReconnect(true);
                disableWifiPowerSave();
                _mdnsRefreshRequested = true;
            }

            // Riavvia mDNS SOLO quando richiesto da evento GOT_IP / cambio interfaccia oppure se non avviato
            // (niente MDNS.end() periodico che invia IGMP Leave al router)
            if (_mdnsRefreshRequested || !_mdnsStarted) {
                _mdnsRefreshRequested = false;
                disableWifiPowerSave();
                setupMdnsAndNtp(true);
            }

            // Ogni 45s invia IGMP Membership Report + annuncio mDNS unsolicited per prevenire pruning IGMP snooping
            if (_mdnsStarted && !scanGraceActive &&
                (millis() - _lastMdnsKeepaliveMs >= MDNS_KEEPALIVE_INTERVAL_MS)) {
                disableWifiPowerSave();
                sendMdnsAndIgmpKeepalive(false);
            }

            // Watchdog L2 Broadcast/Multicast RX (rileva blocco chiave WPA2 GTK anche senza altri peer mDNS)
            uint32_t nowNet = millis();
            if (_lastGroupRxMs != 0 && (nowNet - _lastGroupRxMs < GROUP_RX_SOFT_TIMEOUT_MS)) {
                groupRxRecoveryStage = 0;
            }
            if (_mdnsStarted && !scanGraceActive && _groupRxPacketCount > 0 && _lastGroupRxMs != 0 &&
                (nowNet - lastGroupCheckMs >= 60000UL)) {
                lastGroupCheckMs = nowNet;
                uint32_t silentMs = nowNet - _lastGroupRxMs;
                if (silentMs >= GROUP_RX_HARD_TIMEOUT_MS && groupRxRecoveryStage >= 1) {
                    Serial.printf("[NET] Watchdog Multicast/GTK: 0 frame broadcast/multicast ricevuti da %lus -> riassociazione STA per rinnovo GTK...\n",
                                  (unsigned long)(silentMs / 1000UL));
                    _consecutiveHealthFails = 0;
                    _consecutiveMcastRxFails = 0;
                    _staConnected = false;
                    WiFi.disconnect(false, false);
                    vTaskDelay(pdMS_TO_TICKS(500));
                    if (connectToSavedWiFi(WIFI_CONNECT_TIMEOUT_MS, true)) {
                        setupMdnsAndNtp(true);
                        _mdnsRefreshRequested = false;
                    }
                    groupRxRecoveryStage = 0;
                    uint32_t resetNow = millis();
                    _lastGroupRxMs = (resetNow != 0) ? resetNow : 1;
                    lastRetryMs = resetNow;
                    lastHealthCheckMs = resetNow;
                } else if (silentMs >= GROUP_RX_SOFT_TIMEOUT_MS && groupRxRecoveryStage == 0) {
                    Serial.printf("[NET] Avviso Watchdog Multicast/GTK: nessun frame broadcast/multicast da %lus -> IGMP rejoin + restart mDNS...\n",
                                  (unsigned long)(silentMs / 1000UL));
                    sendMdnsAndIgmpKeepalive(true);
                    setupMdnsAndNtp(true);
                    groupRxRecoveryStage = 1;
                }
            }

            // Watchdog adattivo DNS: rileva stato "zombie" DHCP/DNS senza connettività L3
            if (!scanGraceActive && (millis() - lastHealthCheckMs >= WIFI_HEALTH_CHECK_INTERVAL_MS)) {
                lastHealthCheckMs = millis();
                disableWifiPowerSave();

                IPAddress gw = WiFi.gatewayIP();
                IPAddress resolvedIp;
                bool dnsOk = (gw != IPAddress(0, 0, 0, 0)) && (WiFi.hostByName(NTP_SERVER_1, resolvedIp) == 1);
                reportInternetHealth(dnsOk);

                if (!dnsOk && _dnsEverWorked) {
                    Serial.printf("[NET] Avviso health-check Wi-Fi: risoluzione DNS fallita (%u/%u).\n",
                                  _consecutiveHealthFails, WIFI_ZOMBIE_FAIL_THRESHOLD);
                }

                if (_consecutiveHealthFails >= WIFI_ZOMBIE_FAIL_THRESHOLD) {
                    Serial.println("[NET] Watchdog STA: rilevato stato Wi-Fi zombie, riavvio associazione...");
                    _consecutiveHealthFails = 0;
                    _consecutiveMcastRxFails = 0;
                    _staConnected = false;
                    WiFi.disconnect(false, false);
                    vTaskDelay(pdMS_TO_TICKS(500));
                    if (connectToSavedWiFi(WIFI_CONNECT_TIMEOUT_MS, true)) {
                        setupMdnsAndNtp(true);
                        _mdnsRefreshRequested = false;
                    }
                    groupRxRecoveryStage = 0;
                    lastRetryMs = millis();
                    lastHealthCheckMs = millis();
                }
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
                    if ((strlen(cfg.wifiSsid) > 0 || !cfg.savedWifiNetworks.empty()) &&
                        (millis() - lastRetryMs >= 60000)) {
                        lastRetryMs = millis();
                        if (connectToSavedWiFi(WIFI_CONNECT_TIMEOUT_MS, false)) {
                            setupMdnsAndNtp(true);
                            _mdnsRefreshRequested = false;
                        } else if (!isApLocked() && !_apTimedOut) {
                            startFallbackAP();
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
    return _staConnected && (WiFi.status() == WL_CONNECTED) && (WiFi.localIP() != IPAddress(0, 0, 0, 0));
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

String NetworkManager::getGatewayIp() {
    return isStaConnected() ? WiFi.gatewayIP().toString() : "";
}

String NetworkManager::getSubnetMask() {
    return isStaConnected() ? WiFi.subnetMask().toString() : "";
}

String NetworkManager::getDns1Ip() {
    if (!isStaConnected()) return "";
    IPAddress d = WiFi.dnsIP(0);
    return (d != IPAddress(0, 0, 0, 0)) ? d.toString() : "";
}

String NetworkManager::getDns2Ip() {
    if (!isStaConnected()) return "";
    IPAddress d = WiFi.dnsIP(1);
    return (d != IPAddress(0, 0, 0, 0)) ? d.toString() : "";
}

void NetworkManager::persistCurrentIpIfChanged() {
    if (WiFi.status() != WL_CONNECTED) return;
    IPAddress local = WiFi.localIP();
    if (local == IPAddress(0, 0, 0, 0)) return;

    ConfigManager::saveLastKnownNetInfo(
        WiFi.SSID().c_str(),
        local.toString().c_str(),
        WiFi.gatewayIP().toString().c_str(),
        WiFi.subnetMask().toString().c_str()
    );
    GuiManager::notifyIpAcquired();
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
