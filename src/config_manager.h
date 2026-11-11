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
};

struct SystemConfig {
    char wifiSsid[64];
    char wifiPassword[64];
    char webPassword[32];
    char schoolName[64];
    uint32_t relayPulseMs;
    bool suspended;
    std::vector<BellScheduleItem> schedule;
};

class ConfigManager {
public:
    static constexpr const char* CONFIG_FILE_PATH = "/config.json";
    static constexpr const char* SCHOOL_LOGO_PATH = "/school_logo.bin";
    static constexpr const char* DEFAULT_WEB_PASSWORD = "admin";
    static constexpr uint32_t DEFAULT_RELAY_PULSE_MS = 3000;
    static constexpr uint32_t MAX_RELAY_PULSE_MS = 10000; // Hard cap di sicurezza: 10s
    static constexpr uint32_t MIN_RELAY_PULSE_MS = 200;
    static constexpr uint8_t DAYS_MON_TO_SAT_MASK = 0x7E; // 0b01111110 (Lunedì -> Sabato)

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
};
