#pragma once

#include <Arduino.h>
#include <time.h>
#include "../config_manager.h"

/**
 * @file network_manager.h
 * @brief Gestione Wi-Fi (STA + Fallback AP su Core 0), mDNS (chronoring.local) e sincronizzazione NTP POSIX
 */

class NetworkManager {
public:
    static constexpr const char* MDNS_HOSTNAME              = "chronoring";
    static constexpr const char* FALLBACK_AP_SSID           = "ChronoRing-Setup";
    static constexpr const char* FALLBACK_AP_PASS           = ""; // AP aperto per configurazione immediata
    static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS       = 20000;
    static constexpr uint32_t MDNS_KEEPALIVE_INTERVAL_MS    = 45UL * 1000UL;        // 45s (IGMP snooping & RFC 6762 keepalive)
    static constexpr uint32_t WIFI_HEALTH_CHECK_INTERVAL_MS = 5UL * 60UL * 1000UL;  // 5 minuti
    static constexpr uint8_t  WIFI_ZOMBIE_FAIL_THRESHOLD    = 4;                    // ~20 min di fallimenti DNS consecutivi
    static constexpr uint8_t  WIFI_MCAST_FAIL_THRESHOLD     = 3;                    // ~15 min di assenza RX multicast (GTK)

    // Stringa POSIX per fuso orario Italiano (CET/CEST con cambio ora solare/legale automatico)
    static constexpr const char* TZ_ITALY_POSIX      = "CET-1CEST,M3.5.0,M10.5.0/3";
    static constexpr const char* NTP_SERVER_1        = "pool.ntp.org";
    static constexpr const char* NTP_SERVER_2        = "time.nist.gov";

    /**
     * @brief Avvia la gestione di rete e il task di monitoraggio sul Core 0.
     */
    static void begin();

    /**
     * @brief Richiede un nuovo tentativo di connessione Wi-Fi (es. dopo cambio credenziali dal Web).
     */
    static void requestReconnect();

    /**
     * @brief Richiede il riavvio dell'Access Point per applicare nuovo SSID o Password AP.
     */
    static void requestApRestart();

    /**
     * @brief Arresta completamente l'Access Point di fallback e passa a WIFI_STA puro.
     */
    static void stopFallbackAP();

    /**
     * @brief Richiede l'attivazione manuale dell'Hotspot AP (es. via comando seriale dopo timeout).
     */
    static void requestManualApStart();

    static bool isApTimedOut();
    static bool isApLocked();

    /**
     * @brief Restituisce true se l'orologio RTC interno è sincronizzato via NTP (anno >= 2024).
     */
    static bool isTimeSynced();

    /**
     * @brief Ottiene la struttura tm dell'orario locale corrente.
     * @return true se l'orario è valido e sincronizzato, false altrimenti.
     */
    static bool getCurrentLocalTime(struct tm& timeinfo);

    /**
     * @brief Restituisce l'ora formattata "HH:MM:SS" (o "--:--:--" se non sincronizzato).
     */
    static String getFormattedTime();

    /**
     * @brief Restituisce solo ore e minuti "HH:MM" (o "--:--") per il font grande dell'orologio.
     */
    static String getFormattedHoursMinutes();

    /**
     * @brief Restituisce solo i secondi ":SS" (o ":--") per il font secondario dell'orologio.
     */
    static String getFormattedSeconds();

    /**
     * @brief Restituisce la data formattata "GG/MM/AAAA" con giorno della settimana.
     */
    static String getFormattedDate();

    /**
     * @brief Esegue una scansione preventiva per verificare se l'SSID salvato è visibile nell'etere.
     */
    static bool isSavedNetworkVisible(const char* targetSsid, int32_t* outRssi = nullptr);

    /**
     * @brief Gestisce la scansione Wi-Fi asincrona non-bloccante per la Dashboard Web.
     *        Avvia la scansione se non attiva e restituisce JSON {"status":"running"|"done","networks":[...]}.
     */
    static String getOrStartWifiScanJson();

    static bool isStaConnected();
    static bool isApActive();
    static String getIpAddress();
    static String getGatewayIp();
    static String getSubnetMask();
    static String getDns1Ip();
    static String getDns2Ip();
    static String getNetworkModeString();
    static int32_t getRssi();
    static void requestMdnsRefresh();
    static void reportInternetHealth(bool ok);
    static void notifyWifiEvent(int eventId);
    static uint32_t getStaDisconnectCount();
    static uint32_t getMdnsRestartCount();
    static uint32_t getMdnsKeepaliveCount();
    static bool hasMulticastRxPeers();

private:
    static volatile bool _staConnected;
    static volatile bool _apActive;
    static volatile bool _mdnsStarted;
    static volatile bool _nbnsStarted;
    static volatile bool _ntpConfigured;
    static volatile bool _reconnectRequested;
    static volatile bool _apRestartRequested;
    static volatile bool _forceStartApRequested;
    static volatile bool _scanInProgress;
    static volatile uint32_t _lastScanTimestampMs;
    static volatile bool _hasConnectedStaOnce;
    static volatile bool _apTimedOut;
    static uint32_t _apLastActivityMs;
    static volatile bool _mdnsRefreshRequested;
    static volatile bool _staLostIpEvent;
    static volatile uint32_t _staDisconnectCount;
    static volatile uint32_t _mdnsRestartCount;
    static volatile uint32_t _mdnsKeepaliveCount;
    static volatile bool _dnsEverWorked;
    static volatile uint8_t _consecutiveHealthFails;
    static volatile bool _mcastRxEverWorked;
    static volatile uint8_t _consecutiveMcastRxFails;
    static uint32_t _lastMdnsKeepaliveMs;
    static bool _wifiEventRegistered;
    static volatile bool _persistIpRequested;
    static TaskHandle_t _netTaskHandle;

    static void networkTask(void* pvParameters);
    static void persistCurrentIpIfChanged();
    static bool connectSingleSsid(const char* ssid, const char* password,
                                  const SystemConfig& cfg, uint32_t timeoutMs);
    static bool connectToSavedWiFi(uint32_t timeoutMs, bool skipPreScan = false);
    static void startFallbackAP();
    static void setupMdnsAndNtp(bool forceMdnsRestart = false);
    static void restartMdns();
    static void sendMdnsAndIgmpKeepalive(bool forceIgmpRejoin = false);
    static void disableWifiPowerSave();
};
