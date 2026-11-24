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

struct SavedWifiNetwork {
    char ssid[33];
    char password[65];
};

struct SystemConfig {
    char wifiSsid[64];
    char wifiPassword[64];
    std::vector<SavedWifiNetwork> savedWifiNetworks;
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
    bool staticIpEnabled;       // true = usa configurazione IP statico in STA invece del DHCP
    char staticIp[20];
    char staticGateway[20];
    char staticSubnet[20];
    char staticDns1[20];
    char staticDns2[20];
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
    static constexpr const char* NVS_SEC_NAMESPACE        = "cr_sec";
    static constexpr const char* NVS_NET_NAMESPACE        = "cr_net";
    static constexpr size_t MAX_SAVED_WIFI_NETWORKS       = 10;
    static constexpr size_t SUPER_PWD_MIN_LEN             = 8;
    static constexpr size_t SUPER_PWD_MAX_LEN             = 63;
    static constexpr uint16_t SUPER_PWD_HASH_ROUNDS       = 1000;

    static bool begin();
    static SystemConfig getConfig();
    static bool saveConfig(const SystemConfig& newConfig);

    static bool setSuspended(bool suspended);
    static bool isSuspended();

    static uint32_t getRelayPulseMs();
    static bool setRelayPulseMs(uint32_t pulseMs);

    static bool checkWebPassword(const char* candidate);
    static bool setWebPassword(const char* newPassword);

    static bool isSuperPasswordConfigured();
    static bool checkSuperPassword(const char* candidate);
    static bool setSuperPassword(const char* newSuperPassword);
    static bool clearSuperPassword();

    static bool setWiFiCredentials(const char* ssid, const char* password);
    static bool removeSavedWiFi(const char* ssid);
    static bool promoteSavedWiFi(const char* ssid);
    static bool getSavedWiFiPassword(const char* ssid, char* outPwd, size_t outSize);
    static bool setStaticIpConfig(bool enabled, const char* ip, const char* gateway,
                                  const char* subnet, const char* dns1, const char* dns2);
    static bool saveLastKnownNetInfo(const char* ssid, const char* ip,
                                     const char* gateway, const char* subnet);
    static bool getLastKnownNetInfo(String& ssidOut, String& ipOut,
                                    String& gatewayOut, String& subnetOut);
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
    static bool _superPwdConfigured;
    static uint8_t _superSalt[16];
    static uint8_t _superHash[32];

    static void loadDefaults(SystemConfig& cfg);
    static void loadSuperPasswordFromNvsInternal();
    static bool loadFromFileInternal();
    static bool saveToFileInternal(const SystemConfig& cfg);
    static uint32_t clampRelayPulse(uint32_t pulseMs);
    static uint32_t clampDisplayTimeout(uint32_t sec);
    static uint16_t clampApTimeout(uint16_t min);
};
