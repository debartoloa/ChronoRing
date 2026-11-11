#include "relay_controller.h"
#include "gui/gui_manager.h"
#include <driver/gpio.h>

// ============================================================================
// PRE-INIT HARDWARE LOCK (Eseguito PRIMA di initArduino() e prima di setup())
// ============================================================================
// Questo costruttore C++ con priorità 101 viene eseguito nei primissimi
// millisecondi di avvio dell'applicazione (centinaia di ms prima di setup()),
// forzando immediatamente PIN_RELAY_TRIGGER (GPIO 44 RXD su ESP32-S3) allo
// stato di riposo sicuro con pull-down hardware attivo.
__attribute__((constructor(101))) static void chronoEarlyRelaySafeLock() {
    const gpio_num_t pin = static_cast<gpio_num_t>(PIN_RELAY_TRIGGER);
    const uint32_t safeLevel = RELAY_ACTIVE_LOW ? 1 : 0;

    gpio_set_level(pin, safeLevel);

    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << PIN_RELAY_TRIGGER);
    io_conf.pull_down_en = RELAY_ACTIVE_LOW ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE;
    io_conf.pull_up_en   = RELAY_ACTIVE_LOW ? GPIO_PULLUP_ENABLE    : GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);

    gpio_set_level(pin, safeLevel);
}

bool RelayController::_activeLow = RELAY_ACTIVE_LOW;
volatile bool RelayController::_isActive = false;
bool RelayController::_initialized = false;
esp_timer_handle_t RelayController::_pulseTimer = nullptr;
portMUX_TYPE RelayController::_spinlock = portMUX_INITIALIZER_UNLOCKED;
constexpr uint32_t RelayController::HARD_CAP_MAX_PULSE_MS;

uint8_t RelayController::getInactiveLevel() {
    return _activeLow ? HIGH : LOW;
}

uint8_t RelayController::getActiveLevel() {
    return _activeLow ? LOW : HIGH;
}

void RelayController::writeSafeInactive() {
    gpio_set_level(static_cast<gpio_num_t>(PIN_RELAY_TRIGGER), getInactiveLevel());
    _isActive = false;
}

void RelayController::writeActive() {
    gpio_set_level(static_cast<gpio_num_t>(PIN_RELAY_TRIGGER), getActiveLevel());
    _isActive = true;
}

void IRAM_ATTR RelayController::onTimerCallback(void* arg) {
    portENTER_CRITICAL_ISR(&_spinlock);
    writeSafeInactive();
    portEXIT_CRITICAL_ISR(&_spinlock);

    if (g_has_display) {
        gui_set_bell_ringing(false);
    }
}

void RelayController::begin(bool activeLow) {
    _activeLow = activeLow;
    const gpio_num_t relayPin = static_cast<gpio_num_t>(PIN_RELAY_TRIGGER);

    // 1. SICUREZZA ANTI-SCATTO ALL'AVVIO:
    // Forziamo il registro di output del GPIO al livello INATTIVO PRIMA di abilitare il driver di output.
    // In questo modo non si verifica alcun impulso spurio durante la transizione da alta impedenza a OUTPUT.
    gpio_set_level(relayPin, getInactiveLevel());

    // 2. Configurazione atomica del pad GPIO in modalità Push-Pull Output
    // con pull-up/pull-down coerente con lo stato di riposo per ulteriore sicurezza.
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << PIN_RELAY_TRIGGER);
    io_conf.pull_down_en = _activeLow ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE;
    io_conf.pull_up_en   = _activeLow ? GPIO_PULLUP_ENABLE    : GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);

    // Ribadiamo il livello inattivo dopo la configurazione del registro IO_MUX
    writeSafeInactive();

    // 3. Creazione del timer ad alta precisione esp_timer (one-shot) per lo spegnimento automatico
    if (_pulseTimer == nullptr) {
        esp_timer_create_args_t timer_args = {};
        timer_args.callback = &RelayController::onTimerCallback;
        timer_args.arg = nullptr;
        timer_args.dispatch_method = ESP_TIMER_TASK;
        timer_args.name = "relay_safety_tmr";

        esp_err_t err = esp_timer_create(&timer_args, &_pulseTimer);
        if (err != ESP_OK) {
            Serial.printf("[RELAY] ERRORE CRITICO: Creazione esp_timer fallita (err=0x%X)\n", err);
            return;
        }
    }

    _initialized = true;
    Serial.printf("[RELAY] Inizializzato in sicurezza su GPIO %d (Logica: %s, Riposo=%s)\n",
                  PIN_RELAY_TRIGGER,
                  _activeLow ? "Active LOW" : "Active HIGH",
                  getInactiveLevel() == HIGH ? "HIGH" : "LOW");
}

bool RelayController::triggerPulse(uint32_t durationMs) {
    if (!_initialized || _pulseTimer == nullptr) {
        Serial.println("[RELAY] Impossibile attivare: controller non inizializzato!");
        return false;
    }

    if (durationMs == 0) {
        return false;
    }

    // Applicazione rigorosa dell'Hard Cap di sicurezza a 10 secondi (10000 ms)
    if (durationMs > HARD_CAP_MAX_PULSE_MS) {
        Serial.printf("[RELAY] ATTENZIONE: Durata richiesta (%u ms) eccede l'Hard Cap! Limitata a %u ms.\n",
                      durationMs, HARD_CAP_MAX_PULSE_MS);
        durationMs = HARD_CAP_MAX_PULSE_MS;
    }

    portENTER_CRITICAL(&_spinlock);
    if (_isActive) {
        portEXIT_CRITICAL(&_spinlock);
        Serial.println("[RELAY] Impulso ignorato: il relè è già attivo.");
        return false;
    }

    writeActive();
    portEXIT_CRITICAL(&_spinlock);

    // Avvio del timer one-shot in microsecondi
    uint64_t durationUs = static_cast<uint64_t>(durationMs) * 1000ULL;
    esp_err_t err = esp_timer_start_once(_pulseTimer, durationUs);
    if (err != ESP_OK) {
        // Fallback immediato di sicurezza: se il timer non parte, spegniamo subito il relè!
        portENTER_CRITICAL(&_spinlock);
        writeSafeInactive();
        portEXIT_CRITICAL(&_spinlock);
        Serial.printf("[RELAY] ERRORE avvio timer (0x%X)! Relè disattivato immediatamente per sicurezza.\n", err);
        return false;
    }

    if (g_has_display) {
        gui_set_bell_ringing(true);
    }

    Serial.printf("[RELAY] >>> CAMPANELLA ATTIVATA per %u ms <<<\n", durationMs);
    return true;
}

void RelayController::forceOff() {
    if (_pulseTimer != nullptr && esp_timer_is_active(_pulseTimer)) {
        esp_timer_stop(_pulseTimer);
    }

    portENTER_CRITICAL(&_spinlock);
    writeSafeInactive();
    portEXIT_CRITICAL(&_spinlock);

    if (g_has_display) {
        gui_set_bell_ringing(false);
    }

    Serial.println("[RELAY] Relè disattivato manualmente (forceOff).");
}

bool RelayController::isActive() {
    return _isActive;
}
