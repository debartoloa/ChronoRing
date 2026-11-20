#pragma once

#include <Arduino.h>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

/**
 * @file config_manager.h
 * @brief Gestione thread-safe della configurazione persistente di ChronoRing su LittleFS (/config.json e /school_logo.bin)
 */

struct BellScheduleItem {
    uint8_t hour;
    uint8_t minute;
    uint8_t daysMask; // Bitmask giorni: Bit 0 = Dom, Bit 1 = Lun ... Bit 6 = Sab (Lun-Sab = 0b01111110 = 0x7E)
    bool enabled;
    char label[32];   // Etichetta personalizzata (es. "1a Ora", "Inizio Ricreazione", "Uscita")
};

struct SystemConfig {
    char wifiSsid[64];
    char wifiPassword[64];
    char apSsid[32];
    char apPassword[64];
    char webPassword[32];
    char schoolName[64];
    uint32_t relayPulseMs;
    bool suspended;
    uint32_t displayTimeoutSec; // Timeout inattività display (10..3600s, default 60s)
    bool displaySleepOff;       // false = Standby orologio/logo, true = Spegnimento retroilluminazione
    uint16_t apTimeoutMin;      // Timeout spegnimento AP senza client (0..120 min, 0 = mai, default 15)
    bool apLockAfterSta;        // true = spegne e blocca riapertura AP dopo connessione STA alla rete scolastica
    std::vector<BellScheduleItem> schedule;
};

class ConfigManager {
public:
    static constexpr const char* CONFIG_FILE_PATH = "/config.json";
    static constexpr const char* SCHOOL_LOGO_PATH = "/school_logo.bin";
    static constexpr const char* DEFAULT_AP_SSID = "ChronoRing-Setup";
    static constexpr const char* DEFAULT_AP_PASSWORD = ""; // Vuoto = AP aperto di fabbrica
    static constexpr const char* DEFAULT_WEB_PASSWORD = "admin";
    static constexpr uint32_t DEFAULT_RELAY_PULSE_MS = 3000;
    static constexpr uint32_t MAX_RELAY_PULSE_MS = 10000; // Hard cap di sicurezza: 10s
    static constexpr uint32_t MIN_RELAY_PULSE_MS = 200;
    static constexpr uint8_t DAYS_MON_TO_SAT_MASK = 0x7E; // 0b01111110 (Lunedì -> Sabato)
    static constexpr uint32_t DEFAULT_DISPLAY_TIMEOUT_SEC = 60;
    static constexpr uint32_t MIN_DISPLAY_TIMEOUT_SEC     = 10;
    static constexpr uint32_t MAX_DISPLAY_TIMEOUT_SEC     = 3600;
    static constexpr uint16_t DEFAULT_AP_TIMEOUT_MIN      = 15;
    static constexpr uint16_t MAX_AP_TIMEOUT_MIN          = 120;

    static bool begin();
    static SystemConfig getConfig();
    static bool saveConfig(const SystemConfig& newConfig);

    static bool setSuspended(bool suspended);
    static bool isSuspended();

    static uint32_t getRelayPulseMs();
    static bool setRelayPulseMs(uint32_t pulseMs);

    static bool checkWebPassword(const char* candidate);
    static bool setWebPassword(const char* newPassword);

    static bool setWiFiCredentials(const char* ssid, const char* password);
    static bool setApCredentials(const char* apSsid, const char* apPassword);
    static bool resetToFactoryDefaults();

    static bool setDisplaySettings(uint32_t timeoutSec, bool sleepOff);
    static uint32_t getDisplayTimeoutSec();
    static bool isDisplaySleepOff();

    static bool setApPolicy(uint16_t apTimeoutMin, bool apLockAfterSta);
    static uint16_t getApTimeoutMin();
    static bool isApLockAfterSta();

    static String exportConfigJson();
    static bool importConfigJson(const String& jsonStr, String& errorOut);

    static bool setSchoolName(const char* schoolName);
    static String getSchoolName();
    static bool hasSchoolLogo();
    static bool removeSchoolLogo();

    static void printDiagnostics();

private:
    static SystemConfig _config;
    static SemaphoreHandle_t _mutex;
    static bool _initialized;

    static void loadDefaults(SystemConfig& cfg);
    static bool loadFromFileInternal();
    static bool saveToFileInternal(const SystemConfig& cfg);
    static uint32_t clampRelayPulse(uint32_t pulseMs);
    static uint32_t clampDisplayTimeout(uint32_t sec);
    static uint16_t clampApTimeout(uint16_t min);
};
