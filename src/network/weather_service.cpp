#include "weather_service.h"
#include "network_manager.h"
#include "../hardware_pins.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

WeatherData WeatherService::_data = {};
SemaphoreHandle_t WeatherService::_mutex = nullptr;
TaskHandle_t WeatherService::_taskHandle = nullptr;
volatile bool WeatherService::_forceUpdate = true;

constexpr const char* WeatherService::OPEN_METEO_HOST;
constexpr const char* WeatherService::OPEN_METEO_URL;
constexpr uint32_t WeatherService::UPDATE_INTERVAL_MORNING_MS;
constexpr uint32_t WeatherService::UPDATE_INTERVAL_OFFPEAK_MS;
constexpr uint32_t WeatherService::RETRY_INTERVAL_MS;
constexpr uint32_t WeatherService::RETRY_MAX_BACKOFF_MS;

static uint32_t nonZeroMillis() {
    uint32_t now = millis();
    return (now != 0) ? now : 1;
}

const char* WeatherService::wmoCodeToItalian(int code) {
    switch (code) {
        case 0:  return "Sereno";
        case 1:  return "Poco Nuvoloso";
        case 2:  return "Parz. Nuvoloso";
        case 3:  return "Nuvoloso";
        case 45:
        case 48: return "Nebbia";
        case 51:
        case 53:
        case 55:
        case 56:
        case 57: return "Pioviggine";
        case 61:
        case 63:
        case 65:
        case 66:
        case 67: return "Pioggia";
        case 71:
        case 73:
        case 75:
        case 77: return "Neve";
        case 80:
        case 81:
        case 82: return "Rovesci";
        case 85:
        case 86: return "Nevicate";
        case 95:
        case 96:
        case 99: return "Temporale";
        default: return "Variabile";
    }
}

const char* WeatherService::stateToString(WeatherState state) {
    switch (state) {
        case WeatherState::LOADING: return "loading";
        case WeatherState::SUCCESS: return "success";
        case WeatherState::ERROR:   return "error";
        default:                    return "idle";
    }
}

const char* WeatherService::errorToCode(WeatherError error) {
    switch (error) {
        case WeatherError::WIFI_DISCONNECTED: return "wifi_disconnected";
        case WeatherError::DNS_NO_INTERNET:   return "dns_no_internet";
        case WeatherError::TIMEOUT:           return "timeout";
        case WeatherError::HTTP_STATUS:       return "http_status";
        case WeatherError::JSON_INVALID:      return "json_invalid";
        default:                              return "none";
    }
}

void WeatherService::begin() {
    if (_mutex == nullptr) {
        _mutex = xSemaphoreCreateMutex();
    }

    if (_mutex && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        _data.state = WeatherState::LOADING;
        _data.error = WeatherError::NONE;
        _data.weatherCode = -1;
        strlcpy(_data.conditionText, "In attesa...", sizeof(_data.conditionText));
        xSemaphoreGive(_mutex);
    }

    if (_taskHandle == nullptr) {
        xTaskCreatePinnedToCore(
            WeatherService::weatherTask,
            "ChronoWeatherTask",
            8192,
            nullptr,
            1, // Priorità bassa in background
            &_taskHandle,
            CHRONO_NET_CORE
        );
        Serial.printf("[WEATHER] Servizio Meteo Open-Meteo avviato su Core %d.\n", CHRONO_NET_CORE);
    }
}

bool WeatherService::isEnabled() {
    return _taskHandle != nullptr;
}

void WeatherService::requestImmediateUpdate() {
    _forceUpdate = true;
}

WeatherData WeatherService::getWeatherData() {
    WeatherData copy;
    if (_mutex && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        copy = _data;
        xSemaphoreGive(_mutex);
    } else {
        copy = _data;
    }
    return copy;
}

void WeatherService::setLoading() {
    if (_mutex && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        _data.state = WeatherState::LOADING;
        _data.error = WeatherError::NONE;
        _data.httpCode = 0;
        _data.errorShort[0] = '\0';
        _data.errorDetail[0] = '\0';
        _data.lastAttemptMs = nonZeroMillis();
        xSemaphoreGive(_mutex);
    }
}

void WeatherService::setError(WeatherError err, int httpCode, const char* shortMsg, const char* detail) {
    if (_mutex && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        _data.state = WeatherState::ERROR;
        _data.error = err;
        _data.httpCode = httpCode;
        _data.lastAttemptMs = nonZeroMillis();
        strlcpy(_data.errorShort, shortMsg ? shortMsg : "Errore meteo", sizeof(_data.errorShort));
        strlcpy(_data.errorDetail, detail ? detail : "", sizeof(_data.errorDetail));
        xSemaphoreGive(_mutex);
    }
    Serial.printf("[WEATHER] ERRORE [%s]: %s | %s\n", errorToCode(err),
                  shortMsg ? shortMsg : "", detail ? detail : "");
}

uint32_t WeatherService::computeSuccessIntervalMs() {
    struct tm ti;
    if (NetworkManager::getCurrentLocalTime(ti) && ti.tm_hour >= 7 && ti.tm_hour < 14) {
        return UPDATE_INTERVAL_MORNING_MS; // 1 ora nella fascia scolastica mattutina (07:00-13:59)
    }
    return UPDATE_INTERVAL_OFFPEAK_MS;     // 3 ore pomeriggio/sera/notte o senza NTP
}

void WeatherService::weatherTask(void* pvParameters) {
    (void)pvParameters;
    uint32_t lastAttemptMs = 0;
    bool lastFetchSucceeded = false;
    uint8_t consecutiveFails = 0;

    for (;;) {
        if (!NetworkManager::isStaConnected()) {
            // Stato esplicito: nessun IP assegnato -> errore "Wi-Fi non connesso" (impostato una sola volta)
            WeatherData cur = getWeatherData();
            if (!(cur.state == WeatherState::ERROR && cur.error == WeatherError::WIFI_DISCONNECTED)) {
                setError(WeatherError::WIFI_DISCONNECTED, 0, "Wi-Fi non connesso",
                         NetworkManager::isApActive()
                             ? "Modalita Hotspot AP: la scheda non e connessa alla rete Wi-Fi (nessun IP STA)"
                             : "Nessun indirizzo IP assegnato: connessione Wi-Fi STA assente");
            }
            consecutiveFails = 0;
            _forceUpdate = true; // Al ritorno della rete aggiorna subito senza attendere l'intervallo
        } else {
            uint32_t now = millis();
            bool forced = _forceUpdate;
            uint32_t interval = RETRY_INTERVAL_MS;

            if (lastFetchSucceeded) {
                interval = computeSuccessIntervalMs();
            } else {
                WeatherData cur = getWeatherData();
                if (cur.httpCode == 429) {
                    interval = RETRY_MAX_BACKOFF_MS;
                } else {
                    uint8_t shift = (consecutiveFails > 0)
                                        ? ((consecutiveFails - 1 > 4) ? 4 : (consecutiveFails - 1))
                                        : 0;
                    uint32_t backoff = RETRY_INTERVAL_MS << shift;
                    interval = (backoff > RETRY_MAX_BACKOFF_MS) ? RETRY_MAX_BACKOFF_MS : backoff;
                }
            }

            if (forced || (now - lastAttemptMs >= interval)) {
                _forceUpdate = false;
                if (forced) {
                    consecutiveFails = 0;
                }
                lastAttemptMs = now;
                lastFetchSucceeded = fetchFromOpenMeteo();
                if (lastFetchSucceeded) {
                    consecutiveFails = 0;
                } else if (consecutiveFails < 10) {
                    consecutiveFails++;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

bool WeatherService::fetchFromOpenMeteo() {
    Serial.println("[WEATHER] Richiesta dati meteo da Open-Meteo in corso...");
    setLoading();

    // 1. Verifica DNS esplicita: distingue reti scolastiche con firewall/proxy o senza accesso Internet
    IPAddress apiIp;
    if (WiFi.hostByName(OPEN_METEO_HOST, apiIp) != 1 || apiIp == IPAddress((uint32_t)0)) {
        NetworkManager::reportInternetHealth(false);
        char det[96];
        snprintf(det, sizeof(det), "DNS: impossibile risolvere %s (firewall/proxy o rete senza Internet)",
                 OPEN_METEO_HOST);
        setError(WeatherError::DNS_NO_INTERNET, 0, "No Internet/Firewall", det);
        return false;
    }

    WiFiClientSecure client;
    client.setInsecure(); // Evita blocchi dovuti a scadenza bundle CA root su MCU embedded

    HTTPClient http;
    http.setConnectTimeout(8000);
    http.setTimeout(8000);
    http.setReuse(false);

    if (!http.begin(client, OPEN_METEO_URL)) {
        http.end();
        client.stop();
        setError(WeatherError::TIMEOUT, 0, "API non raggiungibile",
                 "HTTPClient: inizializzazione della connessione HTTPS fallita");
        return false;
    }

    int httpCode = http.GET();
    if (httpCode < 0) {
        // Errore di trasporto: connessione rifiutata, handshake TLS fallito o timeout
        String errStr = HTTPClient::errorToString(httpCode);
        http.end();
        client.stop();
        NetworkManager::reportInternetHealth(false);

        const char* shortMsg = (httpCode == HTTPC_ERROR_READ_TIMEOUT) ? "API Timeout"
                             : (httpCode == HTTPC_ERROR_CONNECTION_REFUSED) ? "API non raggiungibile"
                             : "API non risponde";
        char det[96];
        snprintf(det, sizeof(det), "%s (codice %d) verso %s (IP %s)",
                 errStr.c_str(), httpCode, OPEN_METEO_HOST, apiIp.toString().c_str());
        setError(WeatherError::TIMEOUT, httpCode, shortMsg, det);
        return false;
    }

    if (httpCode != HTTP_CODE_OK) {
        http.end();
        client.stop();
        NetworkManager::reportInternetHealth(true); // Internet raggiungibile: errore applicativo del server
        char shortMsg[28];
        snprintf(shortMsg, sizeof(shortMsg), "Errore HTTP %d", httpCode);
        char det[96];
        snprintf(det, sizeof(det), "Open-Meteo ha risposto con stato HTTP %d (%s)", httpCode,
                 httpCode == 429 ? "troppe richieste" : (httpCode >= 500 ? "errore server" : "richiesta rifiutata"));
        setError(WeatherError::HTTP_STATUS, httpCode, shortMsg, det);
        return false;
    }

    String payload = http.getString();
    http.end();
    client.stop();

    NetworkManager::reportInternetHealth(true);

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    JsonObject current = err ? JsonObject() : doc["current"].as<JsonObject>();
    if (err || current.isNull()) {
        char det[96];
        snprintf(det, sizeof(det), "Risposta non valida: %s (%u byte ricevuti)",
                 err ? err.c_str() : "campo 'current' mancante", (unsigned)payload.length());
        setError(WeatherError::JSON_INVALID, httpCode, "Dati JSON errati", det);
        return false;
    }

    float temp   = current["temperature_2m"] | 0.0f;
    int humidity = current["relative_humidity_2m"] | 0;
    float wind   = current["wind_speed_10m"] | 0.0f;
    int wmoCode  = current["weather_code"] | 0;
    const char* cond = wmoCodeToItalian(wmoCode);

    if (_mutex && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        uint32_t now = nonZeroMillis();
        _data.valid = true;
        _data.temperatureC = temp;
        _data.humidityPct = humidity;
        _data.windSpeedKmh = wind;
        _data.weatherCode = wmoCode;
        strlcpy(_data.conditionText, cond, sizeof(_data.conditionText));
        _data.lastUpdateMs = now;
        _data.lastAttemptMs = now;
        _data.state = WeatherState::SUCCESS;
        _data.error = WeatherError::NONE;
        _data.httpCode = httpCode;
        _data.errorShort[0] = '\0';
        _data.errorDetail[0] = '\0';
        xSemaphoreGive(_mutex);
    }

    Serial.printf("[WEATHER] Meteo aggiornato: %.1f C | Umidita: %d%% | Vento: %.1f km/h | %s (WMO %d)\n",
                  temp, humidity, wind, cond, wmoCode);
    return true;
}
