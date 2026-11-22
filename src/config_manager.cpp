#include "config_manager.h"
#include "hardware_pins.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <mbedtls/sha256.h>
#include <esp_system.h>
#include <esp_random.h>

SystemConfig ConfigManager::_config;
SemaphoreHandle_t ConfigManager::_mutex = nullptr;
bool ConfigManager::_initialized = false;
bool ConfigManager::_superPwdConfigured = false;
uint8_t ConfigManager::_superSalt[16] = {0};
uint8_t ConfigManager::_superHash[32] = {0};

constexpr const char* ConfigManager::CONFIG_FILE_PATH;
constexpr const char* ConfigManager::SCHOOL_LOGO_PATH;
constexpr const char* ConfigManager::DEFAULT_AP_SSID;
constexpr const char* ConfigManager::DEFAULT_AP_PASSWORD;
constexpr const char* ConfigManager::DEFAULT_WEB_PASSWORD;
constexpr uint32_t ConfigManager::DEFAULT_RELAY_PULSE_MS;
constexpr uint32_t ConfigManager::MAX_RELAY_PULSE_MS;
constexpr uint32_t ConfigManager::MIN_RELAY_PULSE_MS;
constexpr uint8_t ConfigManager::DAYS_MON_TO_SAT_MASK;
constexpr uint32_t ConfigManager::DEFAULT_DISPLAY_TIMEOUT_SEC;
constexpr uint32_t ConfigManager::MIN_DISPLAY_TIMEOUT_SEC;
constexpr uint32_t ConfigManager::MAX_DISPLAY_TIMEOUT_SEC;
constexpr uint16_t ConfigManager::DEFAULT_AP_TIMEOUT_MIN;
constexpr uint16_t ConfigManager::MAX_AP_TIMEOUT_MIN;
constexpr const char* ConfigManager::NVS_SEC_NAMESPACE;
constexpr size_t ConfigManager::SUPER_PWD_MIN_LEN;
constexpr size_t ConfigManager::SUPER_PWD_MAX_LEN;
constexpr uint16_t ConfigManager::SUPER_PWD_HASH_ROUNDS;

static bool computeSaltedSha256(const uint8_t salt[16], const char* password, uint8_t outHash[32]) {
    if (!salt || !password || !outHash) return false;
    size_t pwdLen = strlen(password);

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);

    uint8_t digest[32] = {0};
    for (uint16_t round = 0; round < ConfigManager::SUPER_PWD_HASH_ROUNDS; ++round) {
        if (mbedtls_sha256_starts_ret(&ctx, 0) != 0) {
            mbedtls_sha256_free(&ctx);
            return false;
        }
        if (round > 0 && mbedtls_sha256_update_ret(&ctx, digest, sizeof(digest)) != 0) {
            mbedtls_sha256_free(&ctx);
            return false;
        }
        if (mbedtls_sha256_update_ret(&ctx, salt, 16) != 0 ||
            mbedtls_sha256_update_ret(&ctx, reinterpret_cast<const unsigned char*>(password), pwdLen) != 0 ||
            mbedtls_sha256_finish_ret(&ctx, digest) != 0) {
            mbedtls_sha256_free(&ctx);
            return false;
        }
    }

    memcpy(outHash, digest, 32);
    memset(digest, 0, sizeof(digest));
    mbedtls_sha256_free(&ctx);
    return true;
}

static bool constantTimeEquals(const uint8_t* a, const uint8_t* b, size_t len) {
    uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) {
        diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    }
    return diff == 0;
}

uint32_t ConfigManager::clampRelayPulse(uint32_t pulseMs) {
    if (pulseMs < MIN_RELAY_PULSE_MS) return MIN_RELAY_PULSE_MS;
    if (pulseMs > MAX_RELAY_PULSE_MS) return MAX_RELAY_PULSE_MS;
    return pulseMs;
}

uint32_t ConfigManager::clampDisplayTimeout(uint32_t sec) {
    if (sec < MIN_DISPLAY_TIMEOUT_SEC) return MIN_DISPLAY_TIMEOUT_SEC;
    if (sec > MAX_DISPLAY_TIMEOUT_SEC) return MAX_DISPLAY_TIMEOUT_SEC;
    return sec;
}

uint16_t ConfigManager::clampApTimeout(uint16_t min) {
    if (min > MAX_AP_TIMEOUT_MIN) return MAX_AP_TIMEOUT_MIN;
    return min;
}

void ConfigManager::loadDefaults(SystemConfig& cfg) {
    cfg.wifiSsid[0] = '\0';
    cfg.wifiPassword[0] = '\0';
    strlcpy(cfg.apSsid, DEFAULT_AP_SSID, sizeof(cfg.apSsid));
    strlcpy(cfg.apPassword, DEFAULT_AP_PASSWORD, sizeof(cfg.apPassword));
    strlcpy(cfg.webPassword, DEFAULT_WEB_PASSWORD, sizeof(cfg.webPassword));
    cfg.schoolName[0] = '\0';
    cfg.relayPulseMs = DEFAULT_RELAY_PULSE_MS;
    cfg.suspended = false;
    cfg.displayTimeoutSec = DEFAULT_DISPLAY_TIMEOUT_SEC;
    cfg.displaySleepOff = false;
    cfg.apTimeoutMin = DEFAULT_AP_TIMEOUT_MIN;
    cfg.apLockAfterSta = true;
    cfg.schedule.clear();

    // Orari scolastici predefiniti (Lun-Sab):
    // 08:10, 09:10, 10:10, 10:55, 11:05, 12:10, 13:00
    struct DefaultBell {
        uint8_t h;
        uint8_t m;
        const char* label;
    };
    const DefaultBell defaultBells[] = {
        {8,  10, "Ingresso"},
        {9,  10, "2a Ora"},
        {10, 10, "3a Ora"},
        {10, 55, "Inizio Ricreazione"},
        {11, 5,  "Fine Ricreazione"},
        {12, 10, "5a Ora"},
        {13, 0,  "Uscita"}
    };

    for (const auto& t : defaultBells) {
        BellScheduleItem item;
        item.hour = t.h;
        item.minute = t.m;
        item.daysMask = DAYS_MON_TO_SAT_MASK;
        item.enabled = true;
        strlcpy(item.label, t.label, sizeof(item.label));
        cfg.schedule.push_back(item);
    }
}

bool ConfigManager::begin() {
    if (_mutex == nullptr) {
        _mutex = xSemaphoreCreateMutex();
        if (_mutex == nullptr) {
            Serial.println("[CONFIG] ERRORE CRITICO: Impossibile creare il mutex FreeRTOS!");
            return false;
        }
    }

    xSemaphoreTake(_mutex, portMAX_DELAY);

    // Carica sempre l'hash della Super Password Tecnico dalla partizione NVS separata
    loadSuperPasswordFromNvsInternal();

    if (!LittleFS.begin(true)) {
        Serial.println("[CONFIG] ERRORE CRITICO: Montaggio LittleFS fallito!");
        loadDefaults(_config);
        xSemaphoreGive(_mutex);
        return false;
    }

    Serial.printf("[CONFIG] LittleFS montato correttamente (Totale: %u KB, Usati: %u KB)\n",
                  LittleFS.totalBytes() / 1024, LittleFS.usedBytes() / 1024);

    if (!LittleFS.exists(CONFIG_FILE_PATH)) {
        Serial.println("[CONFIG] File /config.json non trovato. Creazione configurazione di default...");
        loadDefaults(_config);
        if (!saveToFileInternal(_config)) {
            Serial.println("[CONFIG] ERRORE: Salvataggio iniziale di /config.json fallito!");
            xSemaphoreGive(_mutex);
            return false;
        }
    } else {
        if (!loadFromFileInternal()) {
            Serial.println("[CONFIG] ATTENZIONE: /config.json corrotto o non valido. Ripristino valori di default...");
            loadDefaults(_config);
            saveToFileInternal(_config);
        }
    }

    _initialized = true;
    xSemaphoreGive(_mutex);
    return true;
}

bool ConfigManager::loadFromFileInternal() {
    File file = LittleFS.open(CONFIG_FILE_PATH, FILE_READ);
    if (!file) {
        Serial.println("[CONFIG] Impossibile aprire /config.json in lettura.");
        return false;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
        Serial.printf("[CONFIG] Errore parsing JSON: %s\n", error.c_str());
        return false;
    }

    const char* ssid = doc["wifi_ssid"] | "";
    const char* wifiPwd = doc["wifi_password"] | "";
    strlcpy(_config.wifiSsid, ssid, sizeof(_config.wifiSsid));
    strlcpy(_config.wifiPassword, wifiPwd, sizeof(_config.wifiPassword));

    const char* apS = doc["ap_ssid"] | DEFAULT_AP_SSID;
    if (strlen(apS) == 0) {
        apS = DEFAULT_AP_SSID;
    }
    const char* apP = doc["ap_password"] | DEFAULT_AP_PASSWORD;
    if (strlen(apP) > 0 && strlen(apP) < 8) {
        // Standard WPA2 richiede minimo 8 caratteri; se invalida torna AP aperto
        apP = "";
    }
    strlcpy(_config.apSsid, apS, sizeof(_config.apSsid));
    strlcpy(_config.apPassword, apP, sizeof(_config.apPassword));

    const char* pwd = doc["web_password"] | DEFAULT_WEB_PASSWORD;
    strlcpy(_config.webPassword, pwd, sizeof(_config.webPassword));

    const char* sName = doc["school_name"] | "";
    strlcpy(_config.schoolName, sName, sizeof(_config.schoolName));

    uint32_t rawPulse = doc["relay_pulse_ms"] | (doc["relay_duration_ms"] | DEFAULT_RELAY_PULSE_MS);
    _config.relayPulseMs = clampRelayPulse(rawPulse);
    _config.suspended = doc["suspended"] | false;

    uint32_t rawDispSec = doc["display_timeout_sec"] | DEFAULT_DISPLAY_TIMEOUT_SEC;
    _config.displayTimeoutSec = clampDisplayTimeout(rawDispSec);
    _config.displaySleepOff = doc["display_sleep_off"] | false;

    uint16_t rawApMin = doc["ap_timeout_min"] | DEFAULT_AP_TIMEOUT_MIN;
    _config.apTimeoutMin = clampApTimeout(rawApMin);
    _config.apLockAfterSta = doc["ap_lock_after_sta"] | true;

    _config.schedule.clear();
    if (doc["schedule"].is<JsonArray>()) {
        JsonArray arr = doc["schedule"].as<JsonArray>();
        for (JsonObject obj : arr) {
            const char* timeStr = obj["time"] | "00:00";
            int h = 0, m = 0;
            if (sscanf(timeStr, "%d:%d", &h, &m) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60) {
                BellScheduleItem item;
                item.hour = static_cast<uint8_t>(h);
                item.minute = static_cast<uint8_t>(m);
                item.daysMask = obj["days_mask"] | DAYS_MON_TO_SAT_MASK;
                item.enabled = obj["enabled"] | true;
                const char* lbl = obj["label"] | "Campanella";
                if (strlen(lbl) == 0) {
                    lbl = "Campanella";
                }
                strlcpy(item.label, lbl, sizeof(item.label));
                _config.schedule.push_back(item);
            }
        }
    }

    if (_config.schedule.empty()) {
        Serial.println("[CONFIG] Lista orari vuota nel JSON, caricamento orari predefiniti.");
        SystemConfig defaultCfg;
        loadDefaults(defaultCfg);
        _config.schedule = defaultCfg.schedule;
    }

    return true;
}

static void populateJsonFromConfig(JsonDocument& doc, const SystemConfig& cfg,
                                   uint32_t clampedPulse, uint32_t clampedDispSec, uint16_t clampedApMin) {
    doc["wifi_ssid"] = cfg.wifiSsid;
    doc["wifi_password"] = cfg.wifiPassword;
    doc["ap_ssid"] = (strlen(cfg.apSsid) > 0) ? cfg.apSsid : ConfigManager::DEFAULT_AP_SSID;
    doc["ap_password"] = cfg.apPassword;
    doc["web_password"] = cfg.webPassword;
    doc["school_name"] = cfg.schoolName;
    doc["relay_pulse_ms"] = clampedPulse;
    doc["relay_duration_ms"] = clampedPulse;
    doc["suspended"] = cfg.suspended;
    doc["display_timeout_sec"] = clampedDispSec;
    doc["display_sleep_off"] = cfg.displaySleepOff;
    doc["ap_timeout_min"] = clampedApMin;
    doc["ap_lock_after_sta"] = cfg.apLockAfterSta;

    JsonArray arr = doc["schedule"].to<JsonArray>();
    for (const auto& item : cfg.schedule) {
        JsonObject obj = arr.add<JsonObject>();
        char timeBuf[6];
        snprintf(timeBuf, sizeof(timeBuf), "%02u:%02u", item.hour, item.minute);
        obj["time"] = timeBuf;
        obj["days_mask"] = item.daysMask;
        obj["enabled"] = item.enabled;
        obj["label"] = (strlen(item.label) > 0) ? item.label : "Campanella";
    }
}

bool ConfigManager::saveToFileInternal(const SystemConfig& cfg) {
    JsonDocument doc;
    populateJsonFromConfig(doc, cfg,
                           clampRelayPulse(cfg.relayPulseMs),
                           clampDisplayTimeout(cfg.displayTimeoutSec),
                           clampApTimeout(cfg.apTimeoutMin));

    File file = LittleFS.open(CONFIG_FILE_PATH, FILE_WRITE);
    if (!file) {
        Serial.println("[CONFIG] Impossibile aprire /config.json in scrittura.");
        return false;
    }

    size_t written = serializeJsonPretty(doc, file);
    file.close();

    if (written == 0) {
        Serial.println("[CONFIG] Errore durante la serializzazione di /config.json.");
        return false;
    }

    Serial.printf("[CONFIG] Configurazione salvata con successo (%u byte scritti).\n", written);
    return true;
}

SystemConfig ConfigManager::getConfig() {
    SystemConfig copy;
    if (_mutex && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        copy = _config;
        xSemaphoreGive(_mutex);
    }
    return copy;
}

bool ConfigManager::saveConfig(const SystemConfig& newConfig) {
    if (!_mutex) return false;
    bool ok = false;
    if (xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        _config = newConfig;
        _config.relayPulseMs = clampRelayPulse(_config.relayPulseMs);
        _config.displayTimeoutSec = clampDisplayTimeout(_config.displayTimeoutSec);
        _config.apTimeoutMin = clampApTimeout(_config.apTimeoutMin);
        ok = saveToFileInternal(_config);
        xSemaphoreGive(_mutex);
    }
    return ok;
}

bool ConfigManager::setSuspended(bool suspended) {
    if (!_mutex) return false;
    bool ok = false;
    if (xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        _config.suspended = suspended;
        ok = saveToFileInternal(_config);
        xSemaphoreGive(_mutex);
    }
    return ok;
}

bool ConfigManager::isSuspended() {
    bool susp = false;
    if (_mutex && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        susp = _config.suspended;
        xSemaphoreGive(_mutex);
    }
    return susp;
}

uint32_t ConfigManager::getRelayPulseMs() {
    uint32_t pulse = DEFAULT_RELAY_PULSE_MS;
    if (_mutex && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        pulse = _config.relayPulseMs;
        xSemaphoreGive(_mutex);
    }
    return pulse;
}

bool ConfigManager::setRelayPulseMs(uint32_t pulseMs) {
    if (!_mutex) return false;
    bool ok = false;
    if (xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        _config.relayPulseMs = clampRelayPulse(pulseMs);
        ok = saveToFileInternal(_config);
        xSemaphoreGive(_mutex);
    }
    return ok;
}

bool ConfigManager::checkWebPassword(const char* candidate) {
    if (!candidate || !_mutex) return false;
    bool match = false;
    if (xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        match = (strcmp(_config.webPassword, candidate) == 0);
        xSemaphoreGive(_mutex);
    }
    return match;
}

bool ConfigManager::setWebPassword(const char* newPassword) {
    if (!newPassword || strlen(newPassword) == 0 || !_mutex) return false;
    bool ok = false;
    if (xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        strlcpy(_config.webPassword, newPassword, sizeof(_config.webPassword));
        ok = saveToFileInternal(_config);
        xSemaphoreGive(_mutex);
    }
    return ok;
}

void ConfigManager::loadSuperPasswordFromNvsInternal() {
    _superPwdConfigured = false;
    memset(_superSalt, 0, sizeof(_superSalt));
    memset(_superHash, 0, sizeof(_superHash));

    Preferences prefs;
    if (!prefs.begin(NVS_SEC_NAMESPACE, true)) {
        return;
    }

    size_t saltLen = prefs.getBytesLength("sp_salt");
    size_t hashLen = prefs.getBytesLength("sp_hash");
    if (saltLen == sizeof(_superSalt) && hashLen == sizeof(_superHash)) {
        size_t r1 = prefs.getBytes("sp_salt", _superSalt, sizeof(_superSalt));
        size_t r2 = prefs.getBytes("sp_hash", _superHash, sizeof(_superHash));
        if (r1 == sizeof(_superSalt) && r2 == sizeof(_superHash)) {
            _superPwdConfigured = true;
        }
    }
    prefs.end();

    if (!_superPwdConfigured) {
        memset(_superSalt, 0, sizeof(_superSalt));
        memset(_superHash, 0, sizeof(_superHash));
    }
}

bool ConfigManager::isSuperPasswordConfigured() {
    bool configured = false;
    if (_mutex && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        configured = _superPwdConfigured;
        xSemaphoreGive(_mutex);
    } else {
        configured = _superPwdConfigured;
    }
    return configured;
}

bool ConfigManager::checkSuperPassword(const char* candidate) {
    if (!candidate || strlen(candidate) == 0 || !_mutex) return false;

    uint8_t localSalt[16];
    uint8_t localHash[32];
    bool configured = false;

    if (xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        configured = _superPwdConfigured;
        if (configured) {
            memcpy(localSalt, _superSalt, sizeof(localSalt));
            memcpy(localHash, _superHash, sizeof(localHash));
        }
        xSemaphoreGive(_mutex);
    }

    if (!configured) {
        return false;
    }

    uint8_t computed[32] = {0};
    bool ok = computeSaltedSha256(localSalt, candidate, computed) &&
              constantTimeEquals(computed, localHash, sizeof(localHash));

    memset(localSalt, 0, sizeof(localSalt));
    memset(localHash, 0, sizeof(localHash));
    memset(computed, 0, sizeof(computed));
    return ok;
}

bool ConfigManager::setSuperPassword(const char* newSuperPassword) {
    if (!newSuperPassword || !_mutex) return false;
    size_t len = strlen(newSuperPassword);
    if (len < SUPER_PWD_MIN_LEN || len > SUPER_PWD_MAX_LEN) {
        return false;
    }

    uint8_t newSalt[16] = {0};
    uint8_t newHash[32] = {0};
    esp_fill_random(newSalt, sizeof(newSalt));
    if (!computeSaltedSha256(newSalt, newSuperPassword, newHash)) {
        memset(newSalt, 0, sizeof(newSalt));
        return false;
    }

    bool ok = false;
    if (xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        Preferences prefs;
        if (prefs.begin(NVS_SEC_NAMESPACE, false)) {
            size_t w1 = prefs.putBytes("sp_salt", newSalt, sizeof(newSalt));
            size_t w2 = prefs.putBytes("sp_hash", newHash, sizeof(newHash));
            prefs.end();
            if (w1 == sizeof(newSalt) && w2 == sizeof(newHash)) {
                memcpy(_superSalt, newSalt, sizeof(_superSalt));
                memcpy(_superHash, newHash, sizeof(_superHash));
                _superPwdConfigured = true;
                ok = true;
            }
        }
        xSemaphoreGive(_mutex);
    }

    memset(newSalt, 0, sizeof(newSalt));
    memset(newHash, 0, sizeof(newHash));
    if (ok) {
        Serial.println("[SEC] Super Password Tecnico salvata in NVS (SHA-256 + salt).");
    }
    return ok;
}

bool ConfigManager::clearSuperPassword() {
    if (!_mutex) return false;
    bool ok = false;
    if (xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        Preferences prefs;
        if (prefs.begin(NVS_SEC_NAMESPACE, false)) {
            prefs.clear();
            prefs.end();
            ok = true;
        }
        _superPwdConfigured = false;
        memset(_superSalt, 0, sizeof(_superSalt));
        memset(_superHash, 0, sizeof(_superHash));
        xSemaphoreGive(_mutex);
    }
    Serial.printf("[SEC] Rimozione Super Password Tecnico da NVS: %s\n", ok ? "OK" : "ERRORE");
    return ok;
}

bool ConfigManager::setWiFiCredentials(const char* ssid, const char* password) {
    if (!ssid || !_mutex) return false;
    bool ok = false;
    if (xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        strlcpy(_config.wifiSsid, ssid, sizeof(_config.wifiSsid));
        if (password) {
            strlcpy(_config.wifiPassword, password, sizeof(_config.wifiPassword));
        } else {
            _config.wifiPassword[0] = '\0';
        }
        ok = saveToFileInternal(_config);
        xSemaphoreGive(_mutex);
    }
    return ok;
}

bool ConfigManager::setApCredentials(const char* apSsid, const char* apPassword) {
    if (!apSsid || !_mutex) return false;

    String cleanSsid = String(apSsid);
    cleanSsid.trim();
    if (cleanSsid.length() == 0 || cleanSsid.length() > 31) {
        return false;
    }

    size_t passLen = apPassword ? strlen(apPassword) : 0;
    if (passLen > 0 && (passLen < 8 || passLen > 63)) {
        return false;
    }

    bool ok = false;
    if (xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        strlcpy(_config.apSsid, cleanSsid.c_str(), sizeof(_config.apSsid));
        if (passLen > 0) {
            strlcpy(_config.apPassword, apPassword, sizeof(_config.apPassword));
        } else {
            _config.apPassword[0] = '\0';
        }
        ok = saveToFileInternal(_config);
        xSemaphoreGive(_mutex);
    }
    return ok;
}

bool ConfigManager::resetToFactoryDefaults() {
    if (!_mutex) return false;
    bool ok = false;
    if (xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        if (LittleFS.exists(SCHOOL_LOGO_PATH)) {
            LittleFS.remove(SCHOOL_LOGO_PATH);
        }
        loadDefaults(_config);
        ok = saveToFileInternal(_config);
        xSemaphoreGive(_mutex);
    }
    Serial.printf("[CONFIG] Ripristino impostazioni di fabbrica: %s\n", ok ? "OK" : "ERRORE");
    return ok;
}

bool ConfigManager::setDisplaySettings(uint32_t timeoutSec, bool sleepOff) {
    if (!_mutex) return false;
    bool ok = false;
    if (xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        _config.displayTimeoutSec = clampDisplayTimeout(timeoutSec);
        _config.displaySleepOff = sleepOff;
        ok = saveToFileInternal(_config);
        xSemaphoreGive(_mutex);
    }
    return ok;
}

uint32_t ConfigManager::getDisplayTimeoutSec() {
    uint32_t sec = DEFAULT_DISPLAY_TIMEOUT_SEC;
    if (_mutex && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        sec = clampDisplayTimeout(_config.displayTimeoutSec);
        xSemaphoreGive(_mutex);
    }
    return sec;
}

bool ConfigManager::isDisplaySleepOff() {
    bool off = false;
    if (_mutex && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        off = _config.displaySleepOff;
        xSemaphoreGive(_mutex);
    }
    return off;
}

bool ConfigManager::setApPolicy(uint16_t apTimeoutMin, bool apLockAfterSta) {
    if (!_mutex) return false;
    bool ok = false;
    if (xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        _config.apTimeoutMin = clampApTimeout(apTimeoutMin);
        _config.apLockAfterSta = apLockAfterSta;
        ok = saveToFileInternal(_config);
        xSemaphoreGive(_mutex);
    }
    return ok;
}

uint16_t ConfigManager::getApTimeoutMin() {
    uint16_t min = DEFAULT_AP_TIMEOUT_MIN;
    if (_mutex && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        min = clampApTimeout(_config.apTimeoutMin);
        xSemaphoreGive(_mutex);
    }
    return min;
}

bool ConfigManager::isApLockAfterSta() {
    bool locked = true;
    if (_mutex && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        locked = _config.apLockAfterSta;
        xSemaphoreGive(_mutex);
    }
    return locked;
}

String ConfigManager::exportConfigJson() {
    SystemConfig cfg = getConfig();
    JsonDocument doc;
    doc["chronoring_backup"] = true;
    doc["fw_version"] = CHRONORING_FW_VERSION;
    populateJsonFromConfig(doc, cfg,
                           clampRelayPulse(cfg.relayPulseMs),
                           clampDisplayTimeout(cfg.displayTimeoutSec),
                           clampApTimeout(cfg.apTimeoutMin));
    String out;
    serializeJsonPretty(doc, out);
    return out;
}

bool ConfigManager::importConfigJson(const String& jsonStr, String& errorOut) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, jsonStr);
    if (err || !doc.is<JsonObject>()) {
        errorOut = String("JSON non valido: ") + (err ? err.c_str() : "formato errato");
        return false;
    }

    if (!doc["schedule"].is<JsonArray>() && !(doc["chronoring_backup"] | false)) {
        errorOut = "Il file non sembra un backup valido di ChronoRing.";
        return false;
    }

    SystemConfig newCfg = getConfig();

    if (doc["wifi_ssid"].is<const char*>()) {
        strlcpy(newCfg.wifiSsid, doc["wifi_ssid"] | "", sizeof(newCfg.wifiSsid));
    }
    if (doc["wifi_password"].is<const char*>()) {
        strlcpy(newCfg.wifiPassword, doc["wifi_password"] | "", sizeof(newCfg.wifiPassword));
    }
    if (doc["ap_ssid"].is<const char*>()) {
        String s = String(doc["ap_ssid"] | DEFAULT_AP_SSID);
        s.trim();
        if (s.length() >= 1 && s.length() <= 31) {
            strlcpy(newCfg.apSsid, s.c_str(), sizeof(newCfg.apSsid));
        }
    }
    if (doc["ap_password"].is<const char*>()) {
        const char* apPwd = doc["ap_password"] | "";
        size_t len = strlen(apPwd);
        if (len == 0 || (len >= 8 && len <= 63)) {
            strlcpy(newCfg.apPassword, apPwd, sizeof(newCfg.apPassword));
        }
    }
    if (doc["web_password"].is<const char*>()) {
        const char* wp = doc["web_password"] | "";
        if (strlen(wp) >= 3) {
            strlcpy(newCfg.webPassword, wp, sizeof(newCfg.webPassword));
        }
    }
    if (doc["school_name"].is<const char*>()) {
        strlcpy(newCfg.schoolName, doc["school_name"] | "", sizeof(newCfg.schoolName));
    }
    if (!doc["relay_pulse_ms"].isNull() || !doc["relay_duration_ms"].isNull()) {
        uint32_t rawP = doc["relay_pulse_ms"] | (doc["relay_duration_ms"] | DEFAULT_RELAY_PULSE_MS);
        newCfg.relayPulseMs = clampRelayPulse(rawP);
    }
    if (!doc["suspended"].isNull()) {
        newCfg.suspended = doc["suspended"] | false;
    }
    if (!doc["display_timeout_sec"].isNull()) {
        newCfg.displayTimeoutSec = clampDisplayTimeout(doc["display_timeout_sec"] | DEFAULT_DISPLAY_TIMEOUT_SEC);
    }
    if (!doc["display_sleep_off"].isNull()) {
        newCfg.displaySleepOff = doc["display_sleep_off"] | false;
    }
    if (!doc["ap_timeout_min"].isNull()) {
        newCfg.apTimeoutMin = clampApTimeout(doc["ap_timeout_min"] | DEFAULT_AP_TIMEOUT_MIN);
    }
    if (!doc["ap_lock_after_sta"].isNull()) {
        newCfg.apLockAfterSta = doc["ap_lock_after_sta"] | true;
    }

    if (doc["schedule"].is<JsonArray>()) {
        std::vector<BellScheduleItem> parsed;
        for (JsonObject obj : doc["schedule"].as<JsonArray>()) {
            const char* timeStr = obj["time"] | "";
            int h = -1, m = -1;
            if (sscanf(timeStr, "%d:%d", &h, &m) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60) {
                BellScheduleItem item;
                item.hour = static_cast<uint8_t>(h);
                item.minute = static_cast<uint8_t>(m);
                item.daysMask = obj["days_mask"] | DAYS_MON_TO_SAT_MASK;
                item.enabled = obj["enabled"] | true;
                String lblStr = String(obj["label"] | "Campanella");
                lblStr.trim();
                const char* finalLbl = lblStr.length() > 0 ? lblStr.c_str() : "Campanella";
                strlcpy(item.label, finalLbl, sizeof(item.label));
                parsed.push_back(item);
            }
        }
        if (!parsed.empty()) {
            newCfg.schedule = parsed;
        }
    }

    if (!saveConfig(newCfg)) {
        errorOut = "Errore salvataggio configurazione importata su LittleFS.";
        return false;
    }
    return true;
}

bool ConfigManager::setSchoolName(const char* schoolName) {
    if (!schoolName || !_mutex) return false;
    bool ok = false;
    if (xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        strlcpy(_config.schoolName, schoolName, sizeof(_config.schoolName));
        ok = saveToFileInternal(_config);
        xSemaphoreGive(_mutex);
    }
    return ok;
}

String ConfigManager::getSchoolName() {
    String name = "";
    if (_mutex && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        name = String(_config.schoolName);
        xSemaphoreGive(_mutex);
    }
    return name;
}

bool ConfigManager::hasSchoolLogo() {
    bool exists = false;
    if (_mutex && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        exists = LittleFS.exists(SCHOOL_LOGO_PATH);
        xSemaphoreGive(_mutex);
    }
    return exists;
}

bool ConfigManager::removeSchoolLogo() {
    bool ok = false;
    if (_mutex && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        if (LittleFS.exists(SCHOOL_LOGO_PATH)) {
            ok = LittleFS.remove(SCHOOL_LOGO_PATH);
        } else {
            ok = true;
        }
        xSemaphoreGive(_mutex);
    }
    return ok;
}

void ConfigManager::printDiagnostics() {
    SystemConfig cfg = getConfig();
    Serial.println("==================================================");
    Serial.println("    CHRONORING - DIAGNOSTICA (/config.json)       ");
    Serial.println("==================================================");
    Serial.printf(" - Nome Istituto      : %s\n", strlen(cfg.schoolName) > 0 ? cfg.schoolName : "<Non impostato>");
    Serial.printf(" - Logo Scuola        : %s\n", hasSchoolLogo() ? "Presente (/school_logo.bin)" : "Non caricato");
    Serial.printf(" - Wi-Fi SSID         : %s\n", strlen(cfg.wifiSsid) > 0 ? cfg.wifiSsid : "<Non configurato - Fallback AP>");
    Serial.printf(" - Hotspot AP SSID    : %s (%s | Timeout: %u min | Blocco post-STA: %s)\n",
                  strlen(cfg.apSsid) > 0 ? cfg.apSsid : DEFAULT_AP_SSID,
                  strlen(cfg.apPassword) >= 8 ? "PROTETTO WPA2" : "APERTO",
                  cfg.apTimeoutMin,
                  cfg.apLockAfterSta ? "SI" : "NO");
    Serial.printf(" - Display Timeout    : %u s (%s)\n",
                  cfg.displayTimeoutSec,
                  cfg.displaySleepOff ? "Spegnimento totale" : "Standby orologio/logo");
    Serial.printf(" - Password Web (len) : %u caratteri\n", strlen(cfg.webPassword));
    Serial.printf(" - Super Password     : %s\n",
                  isSuperPasswordConfigured() ? "CONFIGURATA (NVS SHA-256)" : "NON CONFIGURATA (Wizard attivo)");
    Serial.printf(" - Durata Impulso Relè: %u ms (Hard Cap: %u ms)\n", cfg.relayPulseMs, MAX_RELAY_PULSE_MS);
    Serial.printf(" - Stato Sospensione  : %s\n", cfg.suspended ? "SOSPESO (Campanella Disattivata)" : "ATTIVO");
    Serial.printf(" - Orari Programmati  : %u suoni giornalieri (Lun-Sab)\n", cfg.schedule.size());

    for (size_t i = 0; i < cfg.schedule.size(); ++i) {
        const auto& item = cfg.schedule[i];
        Serial.printf("   [%02u] %02u:%02u | %-20s | Giorni Mask: 0x%02X | %s\n",
                      i + 1, item.hour, item.minute, item.label, item.daysMask,
                      item.enabled ? "ABILITATO" : "DISABILITATO");
    }
    Serial.println("==================================================");
}
