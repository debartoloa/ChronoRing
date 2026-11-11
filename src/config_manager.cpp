#include "config_manager.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

SystemConfig ConfigManager::_config;
SemaphoreHandle_t ConfigManager::_mutex = nullptr;
bool ConfigManager::_initialized = false;

constexpr const char* ConfigManager::CONFIG_FILE_PATH;
constexpr const char* ConfigManager::SCHOOL_LOGO_PATH;
constexpr const char* ConfigManager::DEFAULT_WEB_PASSWORD;
constexpr uint32_t ConfigManager::DEFAULT_RELAY_PULSE_MS;
constexpr uint32_t ConfigManager::MAX_RELAY_PULSE_MS;
constexpr uint32_t ConfigManager::MIN_RELAY_PULSE_MS;
constexpr uint8_t ConfigManager::DAYS_MON_TO_SAT_MASK;

uint32_t ConfigManager::clampRelayPulse(uint32_t pulseMs) {
    if (pulseMs < MIN_RELAY_PULSE_MS) return MIN_RELAY_PULSE_MS;
    if (pulseMs > MAX_RELAY_PULSE_MS) return MAX_RELAY_PULSE_MS;
    return pulseMs;
}

void ConfigManager::loadDefaults(SystemConfig& cfg) {
    cfg.wifiSsid[0] = '\0';
    cfg.wifiPassword[0] = '\0';
    strlcpy(cfg.webPassword, DEFAULT_WEB_PASSWORD, sizeof(cfg.webPassword));
    cfg.schoolName[0] = '\0';
    cfg.relayPulseMs = DEFAULT_RELAY_PULSE_MS;
    cfg.suspended = false;
    cfg.schedule.clear();

    // Orari scolastici predefiniti (Lun-Sab):
    // 08:10, 09:10, 10:10, 10:55, 11:05, 12:10, 13:00
    const uint8_t defaultTimes[][2] = {
        {8, 10},
        {9, 10},
        {10, 10},
        {10, 55},
        {11, 5},
        {12, 10},
        {13, 0}
    };

    for (const auto& t : defaultTimes) {
        BellScheduleItem item;
        item.hour = t[0];
        item.minute = t[1];
        item.daysMask = DAYS_MON_TO_SAT_MASK;
        item.enabled = true;
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

    const char* pwd = doc["web_password"] | DEFAULT_WEB_PASSWORD;
    strlcpy(_config.webPassword, pwd, sizeof(_config.webPassword));

    const char* sName = doc["school_name"] | "";
    strlcpy(_config.schoolName, sName, sizeof(_config.schoolName));

    uint32_t rawPulse = doc["relay_pulse_ms"] | (doc["relay_duration_ms"] | DEFAULT_RELAY_PULSE_MS);
    _config.relayPulseMs = clampRelayPulse(rawPulse);
    _config.suspended = doc["suspended"] | false;

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

bool ConfigManager::saveToFileInternal(const SystemConfig& cfg) {
    JsonDocument doc;

    doc["wifi_ssid"] = cfg.wifiSsid;
    doc["wifi_password"] = cfg.wifiPassword;
    doc["web_password"] = cfg.webPassword;
    doc["school_name"] = cfg.schoolName;
    doc["relay_pulse_ms"] = clampRelayPulse(cfg.relayPulseMs);
    doc["relay_duration_ms"] = clampRelayPulse(cfg.relayPulseMs);
    doc["suspended"] = cfg.suspended;

    JsonArray arr = doc["schedule"].to<JsonArray>();
    for (const auto& item : cfg.schedule) {
        JsonObject obj = arr.add<JsonObject>();
        char timeBuf[6];
        snprintf(timeBuf, sizeof(timeBuf), "%02u:%02u", item.hour, item.minute);
        obj["time"] = timeBuf;
        obj["days_mask"] = item.daysMask;
        obj["enabled"] = item.enabled;
    }

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
    Serial.printf(" - Password Web (len) : %u caratteri\n", strlen(cfg.webPassword));
    Serial.printf(" - Durata Impulso Relè: %u ms (Hard Cap: %u ms)\n", cfg.relayPulseMs, MAX_RELAY_PULSE_MS);
    Serial.printf(" - Stato Sospensione  : %s\n", cfg.suspended ? "SOSPESO (Campanella Disattivata)" : "ATTIVO");
    Serial.printf(" - Orari Programmati  : %u suoni giornalieri (Lun-Sab)\n", cfg.schedule.size());

    for (size_t i = 0; i < cfg.schedule.size(); ++i) {
        const auto& item = cfg.schedule[i];
        Serial.printf("   [%02u] %02u:%02u | Giorni Mask: 0x%02X | %s\n",
                      i + 1, item.hour, item.minute, item.daysMask,
                      item.enabled ? "ABILITATO" : "DISABILITATO");
    }
    Serial.println("==================================================");
}
