#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

/**
 * @file weather_service.h
 * @brief Client HTTPS asincrono su Core 0 per Open-Meteo REST API con macchina a stati esplicita
 *        (IDLE -> LOADING -> SUCCESS | ERROR) e diagnosi della causa reale dell'errore.
 */

enum class WeatherState : uint8_t {
    IDLE = 0,   // Servizio non ancora avviato (es. modalità Headless)
    LOADING,    // Richiesta HTTP in corso
    SUCCESS,    // Ultimo recupero riuscito
    ERROR       // Ultimo recupero fallito (vedi WeatherError)
};

enum class WeatherError : uint8_t {
    NONE = 0,
    WIFI_DISCONNECTED,  // Nessun IP assegnato / STA non connessa
    DNS_NO_INTERNET,    // hostByName fallito (firewall/proxy scolastico o rete senza Internet)
    TIMEOUT,            // Connessione rifiutata, timeout o API che non risponde
    HTTP_STATUS,        // Risposta HTTP diversa da 200 (404, 429, 500...)
    JSON_INVALID        // Risposta malformata / non JSON (es. pagina di login proxy)
};

struct WeatherData {
    bool valid;              // true se esiste almeno un dato meteo recuperato con successo
    float temperatureC;
    int humidityPct;
    float windSpeedKmh;
    int weatherCode;
    char conditionText[24];
    uint32_t lastUpdateMs;   // millis() dell'ultimo recupero riuscito
    WeatherState state;
    WeatherError error;
    int httpCode;            // Codice HTTP o codice errore HTTPClient (<0)
    uint32_t lastAttemptMs;  // millis() dell'ultimo tentativo (riuscito o fallito)
    char errorShort[28];     // Messaggio breve per il display 2.8"
    char errorDetail[96];    // Dettaglio tecnico per la Dashboard Web
};

class WeatherService {
public:
    static constexpr const char* OPEN_METEO_HOST = "api.open-meteo.com";
    static constexpr const char* OPEN_METEO_URL =
        "https://api.open-meteo.com/v1/forecast?latitude=39.815&longitude=16.205&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m&timezone=auto";

    static constexpr uint32_t UPDATE_INTERVAL_MORNING_MS = 60UL * 60UL * 1000UL;       // 1 ora (07:00-13:59)
    static constexpr uint32_t UPDATE_INTERVAL_OFFPEAK_MS = 3UL * 60UL * 60UL * 1000UL; // 3 ore (14:00-06:59 o no NTP)
    static constexpr uint32_t RETRY_INTERVAL_MS          = 60UL * 1000UL;              // Backoff base 1 minuto
    static constexpr uint32_t RETRY_MAX_BACKOFF_MS       = 15UL * 60UL * 1000UL;       // Cap backoff 15 minuti (o HTTP 429)

    /**
     * @brief Avvia il task FreeRTOS asincrono sul Core 0 per il recupero meteo.
     */
    static void begin();

    /**
     * @brief true se il servizio meteo è attivo (avviato solo con display collegato).
     */
    static bool isEnabled();

    /**
     * @brief Restituisce una copia thread-safe dello stato e degli ultimi dati meteo.
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

    static const char* stateToString(WeatherState state);
    static const char* errorToCode(WeatherError error);

private:
    static WeatherData _data;
    static SemaphoreHandle_t _mutex;
    static TaskHandle_t _taskHandle;
    static volatile bool _forceUpdate;

    static void weatherTask(void* pvParameters);
    static uint32_t computeSuccessIntervalMs();
    static bool fetchFromOpenMeteo();
    static void setLoading();
    static void setError(WeatherError err, int httpCode, const char* shortMsg, const char* detail);
};
