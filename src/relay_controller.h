#pragma once

#include <Arduino.h>
#include <esp_timer.h>
#include "hardware_pins.h"

/**
 * @file relay_controller.h
 * @brief Controllo sicuro e non bloccante del relè campanella tramite esp_timer
 */

class RelayController {
public:
    static constexpr uint32_t HARD_CAP_MAX_PULSE_MS = 10000; // Limite rigido hardware/software: 10 secondi

    /**
     * @brief Inizializza il GPIO del relè garantendo l'assenza di glitch o scatti indesiderati al boot.
     * @param activeLow true se il modulo relè si eccita con segnale LOW (default), false se Active HIGH.
     */
    static void begin(bool activeLow = RELAY_ACTIVE_LOW);

    /**
     * @brief Attiva il relè per la durata specificata in modo totalmente non bloccante.
     * @param durationMs Durata dell'impulso in millisecondi (limitata automaticamente a HARD_CAP_MAX_PULSE_MS).
     * @return true se l'attivazione è stata avviata, false se il relè è già attivo o non inizializzato.
     */
    static bool triggerPulse(uint32_t durationMs);

    /**
     * @brief Alias diretto di triggerPulse() per compatibilità con lo Scheduler.
     */
    static inline bool trigger(uint32_t durationMs) {
        return triggerPulse(durationMs);
    }

    /**
     * @brief Disattiva immediatamente il relè e arresta il timer di sicurezza.
     */
    static void forceOff();

    /**
     * @brief Restituisce true se il relè è attualmente eccitato.
     */
    static bool isActive();

    /**
     * @brief Restituisce il livello logico corrispondente allo stato INATTIVO (spento).
     */
    static uint8_t getInactiveLevel();

    /**
     * @brief Restituisce il livello logico corrispondente allo stato ATTIVO (acceso).
     */
    static uint8_t getActiveLevel();

private:
    static bool _activeLow;
    static volatile bool _isActive;
    static bool _initialized;
    static esp_timer_handle_t _pulseTimer;
    static portMUX_TYPE _spinlock;

    static void writeSafeInactive();
    static void writeActive();
    static void IRAM_ATTR onTimerCallback(void* arg);
};
