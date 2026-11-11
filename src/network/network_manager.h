#pragma once

#include <Arduino.h>
#include <time.h>

/**
 * @file network_manager.h
 * @brief Gestione Wi-Fi (STA + Fallback AP su Core 0), mDNS (chronoring.local) e sincronizzazione NTP POSIX
 */

class NetworkManager {
public:
    static constexpr const char* MDNS_HOSTNAME       = "chronoring";
    static constexpr const char* FALLBACK_AP_SSID    = "ChronoRing-Setup";
    static constexpr const char* FALLBACK_AP_PASS    = ""; // AP aperto per configurazione immediata
    static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;

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

    static bool isStaConnected();
    static bool isApActive();
    static String getIpAddress();
    static String getNetworkModeString();
    static int32_t getRssi();

private:
    static volatile bool _staConnected;
    static volatile bool _apActive;
    static volatile bool _mdnsStarted;
    static volatile bool _ntpConfigured;
    static volatile bool _reconnectRequested;
    static TaskHandle_t _netTaskHandle;

    static void networkTask(void* pvParameters);
    static bool connectToSavedWiFi(uint32_t timeoutMs);
    static void startFallbackAP();
    static void setupMdnsAndNtp();
};
