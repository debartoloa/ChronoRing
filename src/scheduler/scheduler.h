#pragma once

#include <Arduino.h>

/**
 * @file scheduler.h
 * @brief Scheduler settimanale della campanella ChronoRing (Task FreeRTOS a 1 Hz)
 */

class Scheduler {
public:
    /**
     * @brief Avvia il task FreeRTOS dello scheduler (controllo ogni 1000 ms).
     */
    static void begin();

    /**
     * @brief Restituisce una stringa descrittiva del prossimo suono programmato per oggi (usata da Web UI).
     */
    static String getNextBellString();

    /**
     * @brief Ottiene i dettagli formattati per il display LVGL: nome evento (es. "Ricreazione 10:55") e countdown (es. "-11m 45s").
     * @param eventLabel [out] Etichetta del prossimo evento.
     * @param countdownStr [out] Conto alla rovescia formattato.
     * @return true se esiste un prossimo evento nella giornata odierna, false altrimenti.
     */
    static bool getNextBellDetails(String& eventLabel, String& countdownStr);

    /**
     * @brief Restituisce l'orario dell'ultimo suono attivato automaticamente ("HH:MM" o "Nessuno").
     */
    static String getLastTriggeredString();

private:
    static TaskHandle_t _schedulerTaskHandle;
    static int _lastTriggeredYday;
    static int _lastTriggeredHour;
    static int _lastTriggeredMin;
    static char _lastTriggeredStr[16];

    static void schedulerTask(void* pvParameters);
    static void checkAndTriggerBell();
    static const char* getBellEventName(uint8_t hour, uint8_t minute);
};
