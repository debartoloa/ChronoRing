#include "weather_service.h"
#include "network_manager.h"
#include "../hardware_pins.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

WeatherData WeatherService::_data = {false, 0.0f, 0, 0.0f, -1, "In attesa...", 0};
SemaphoreHandle_t WeatherService::_mutex = nullptr;
TaskHandle_t WeatherService::_taskHandle = nullptr;
volatile bool WeatherService::_forceUpdate = true;

constexpr const char* WeatherService::OPEN_METEO_URL;
constexpr uint32_t WeatherService::UPDATE_INTERVAL_MS;
constexpr uint32_t WeatherService::RETRY_INTERVAL_MS;

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

void WeatherService::begin() {
    if (_mutex == nullptr) {
        _mutex = xSemaphoreCreateMutex();
    }

    if (_taskHandle == nullptr) {
        xTaskCreatePinnedToCore(
            WeatherService::weatherTask,
            "ChronoWeatherTask",
            6144,
            nullptr,
            1, // Priorità bassa in background
            &_taskHandle,
            CHRONO_NET_CORE
        );
        Serial.printf("[WEATHER] Servizio Meteo Open-Meteo avviato su Core %d.\n", CHRONO_NET_CORE);
    }
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

void WeatherService::weatherTask(void* pvParameters) {
    uint32_t lastAttemptMs = 0;
    bool lastFetchSucceeded = false;

    for (;;) {
        if (NetworkManager::isStaConnected()) {
            uint32_t now = millis();
            uint32_t interval = lastFetchSucceeded ? UPDATE_INTERVAL_MS : RETRY_INTERVAL_MS;

            if (_forceUpdate || (now - lastAttemptMs >= interval)) {
                _forceUpdate = false;
                lastAttemptMs = now;
                lastFetchSucceeded = fetchFromOpenMeteo();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

bool WeatherService::fetchFromOpenMeteo() {
    Serial.println("[WEATHER] Richiesta dati meteo da Open-Meteo in corso...");

    WiFiClientSecure client;
    client.setInsecure(); // Evita blocchi dovuti a scadenza bundle CA root su MCU embedded

    HTTPClient http;
    http.setTimeout(8000);

    if (!http.begin(client, OPEN_METEO_URL)) {
        Serial.println("[WEATHER] Impossibile inizializzare HTTPClient.");
        return false;
    }

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("[WEATHER] Errore HTTP GET: codice %d\n", httpCode);
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        Serial.printf("[WEATHER] Errore parsing JSON Open-Meteo: %s\n", err.c_str());
        return false;
    }

    JsonObject current = doc["current"];
    if (current.isNull()) {
        Serial.println("[WEATHER] Oggetto 'current' mancante nella risposta JSON.");
        return false;
    }

    float temp   = current["temperature_2m"] | 0.0f;
    int humidity = current["relative_humidity_2m"] | 0;
    float wind   = current["wind_speed_10m"] | 0.0f;
    int wmoCode  = current["weather_code"] | 0;
    const char* cond = wmoCodeToItalian(wmoCode);

    if (_mutex && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        _data.valid = true;
        _data.temperatureC = temp;
        _data.humidityPct = humidity;
        _data.windSpeedKmh = wind;
        _data.weatherCode = wmoCode;
        strlcpy(_data.conditionText, cond, sizeof(_data.conditionText));
        _data.lastUpdateMs = millis();
        xSemaphoreGive(_mutex);
    }

    Serial.printf("[WEATHER] Meteo aggiornato: %.1f C | Umidita: %d%% | Vento: %.1f km/h | %s (WMO %d)\n",
                  temp, humidity, wind, cond, wmoCode);
    return true;
}
