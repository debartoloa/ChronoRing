#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

/**
 * @file weather_service.h
 * @brief Client HTTPS asincrono su Core 0 per Open-Meteo REST API
 */

struct WeatherData {
    bool valid;
    float temperatureC;
    int humidityPct;
    float windSpeedKmh;
    int weatherCode;
    char conditionText[24];
    uint32_t lastUpdateMs;
};

class WeatherService {
public:
    static constexpr const char* OPEN_METEO_URL =
        "https://api.open-meteo.com/v1/forecast?latitude=39.815&longitude=16.205&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m&timezone=auto";

    static constexpr uint32_t UPDATE_INTERVAL_MS = 30UL * 60UL * 1000UL; // 30 minuti
    static constexpr uint32_t RETRY_INTERVAL_MS  = 60UL * 1000UL;        // 1 minuto se fallisce

    /**
     * @brief Avvia il task FreeRTOS asincrono sul Core 0 per il recupero meteo.
     */
    static void begin();

    /**
     * @brief Restituisce una copia thread-safe degli ultimi dati meteo ricevuti.
     */
    static WeatherData getWeatherData();

    /**
     * @brief Forza un aggiornamento meteo al prossimo ciclo del task su Core 0.
     */
    static void requestImmediateUpdate();

    /**
     * @brief Traduce il codice WMO Open-Meteo in una descrizione concisa in italiano.
     */
    static const char* wmoCodeToItalian(int code);

private:
    static WeatherData _data;
    static SemaphoreHandle_t _mutex;
    static TaskHandle_t _taskHandle;
    static volatile bool _forceUpdate;

    static void weatherTask(void* pvParameters);
    static bool fetchFromOpenMeteo();
};
