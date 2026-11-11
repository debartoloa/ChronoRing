#include "scheduler.h"
#include "../hardware_pins.h"
#include "../config_manager.h"
#include "../relay_controller.h"
#include "../network/network_manager.h"

TaskHandle_t Scheduler::_schedulerTaskHandle = nullptr;
int Scheduler::_lastTriggeredYday = -1;
int Scheduler::_lastTriggeredHour = -1;
int Scheduler::_lastTriggeredMin  = -1;
char Scheduler::_lastTriggeredStr[16] = "Nessuno";

void Scheduler::begin() {
    if (_schedulerTaskHandle == nullptr) {
        xTaskCreatePinnedToCore(
            Scheduler::schedulerTask,
            "ChronoSchedTask",
            4096,
            nullptr,
            2, // Priorità superiore rispetto ai task di background
            &_schedulerTaskHandle,
            CHRONO_APP_CORE
        );
        Serial.printf("[SCHEDULER] Task Scheduler settimanale avviato su Core %d (frequenza: 1 Hz).\n", CHRONO_APP_CORE);
    }
}

void Scheduler::schedulerTask(void* pvParameters) {
    TickType_t lastWakeTime = xTaskGetTickCount();
    for (;;) {
        checkAndTriggerBell();
        vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(1000));
    }
}

const char* Scheduler::getBellEventName(uint8_t hour, uint8_t minute) {
    if (hour == 8  && minute == 10) return "Ingresso";
    if (hour == 9  && minute == 10) return "2a Ora";
    if (hour == 10 && minute == 10) return "3a Ora";
    if (hour == 10 && minute == 55) return "Ricreazione";
    if (hour == 11 && minute == 5)  return "Fine Ricr.";
    if (hour == 11 && minute == 10) return "4a Ora";
    if (hour == 12 && minute == 10) return "5a Ora";
    if (hour == 13 && minute == 0)  return "Uscita";
    return "Campanella";
}

void Scheduler::checkAndTriggerBell() {
    struct tm timeinfo;
    if (!NetworkManager::getCurrentLocalTime(timeinfo)) {
        return;
    }

    // 1. Verifica giorni lavorativi scolastici: Lunedì (1) -> Sabato (6). Domenica = 0.
    int wday = timeinfo.tm_wday;
    if (wday < 1 || wday > 6) {
        return;
    }

    SystemConfig cfg = ConfigManager::getConfig();

    // 2. Verifica sospensione globale
    if (cfg.suspended) {
        return;
    }

    // 3. Evita doppi scatti nello stesso minuto dello stesso giorno dell'anno
    if (timeinfo.tm_yday == _lastTriggeredYday &&
        timeinfo.tm_hour == _lastTriggeredHour &&
        timeinfo.tm_min  == _lastTriggeredMin) {
        return;
    }

    if (timeinfo.tm_sec > 15) {
        return;
    }

    uint8_t todayBit = (1 << wday);

    // 4. Scansione degli orari programmati in config.json
    for (const auto& item : cfg.schedule) {
        if (!item.enabled) {
            continue;
        }

        if ((item.daysMask & todayBit) == 0) {
            continue;
        }

        if (item.hour == static_cast<uint8_t>(timeinfo.tm_hour) &&
            item.minute == static_cast<uint8_t>(timeinfo.tm_min)) {

            _lastTriggeredYday = timeinfo.tm_yday;
            _lastTriggeredHour = timeinfo.tm_hour;
            _lastTriggeredMin  = timeinfo.tm_min;
            snprintf(_lastTriggeredStr, sizeof(_lastTriggeredStr), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);

            Serial.printf("[SCHEDULER] >>> SQUILLO AUTOMATICO DELLE %02d:%02d (Durata: %u ms) <<<\n",
                          timeinfo.tm_hour, timeinfo.tm_min, cfg.relayPulseMs);

            RelayController::trigger(cfg.relayPulseMs);
            break;
        }
    }
}

bool Scheduler::getNextBellDetails(String& eventLabel, String& countdownStr) {
    SystemConfig cfg = ConfigManager::getConfig();
    if (cfg.suspended) {
        eventLabel = "SUONI AUTOMATICI SOSPESI";
        countdownStr = "";
        return false;
    }

    struct tm timeinfo;
    if (!NetworkManager::getCurrentLocalTime(timeinfo)) {
        eventLabel = "Attesa NTP...";
        countdownStr = "--m --s";
        return false;
    }

    int wday = timeinfo.tm_wday;
    if (wday < 1 || wday > 6) {
        eventLabel = "Domenica";
        countdownStr = "Lun 08:10";
        return false;
    }

    uint8_t todayBit = (1 << wday);
    int currentSecOfDay = timeinfo.tm_hour * 3600 + timeinfo.tm_min * 60 + timeinfo.tm_sec;
    int bestSecOfDay = 24 * 3600 + 1;
    uint8_t bestH = 0, bestM = 0;

    for (const auto& item : cfg.schedule) {
        if (!item.enabled) continue;
        if ((item.daysMask & todayBit) == 0) continue;

        int itemSecOfDay = item.hour * 3600 + item.minute * 60;
        if (itemSecOfDay > currentSecOfDay && itemSecOfDay < bestSecOfDay) {
            bestSecOfDay = itemSecOfDay;
            bestH = item.hour;
            bestM = item.minute;
        }
    }

    if (bestSecOfDay <= 24 * 3600) {
        char labelBuf[36];
        snprintf(labelBuf, sizeof(labelBuf), "%s %02u:%02u",
                 getBellEventName(bestH, bestM), bestH, bestM);
        eventLabel = String(labelBuf);

        int diffSec = bestSecOfDay - currentSecOfDay;
        int remH = diffSec / 3600;
        int remM = (diffSec % 3600) / 60;
        int remS = diffSec % 60;

        char cdBuf[24];
        if (remH > 0) {
            snprintf(cdBuf, sizeof(cdBuf), "-%dh %02dm %02ds", remH, remM, remS);
        } else {
            snprintf(cdBuf, sizeof(cdBuf), "-%02dm %02ds", remM, remS);
        }
        countdownStr = String(cdBuf);
        return true;
    }

    eventLabel = "Fine lezioni oggi";
    countdownStr = "A domani";
    return false;
}

String Scheduler::getNextBellString() {
    String label, cd;
    if (getNextBellDetails(label, cd)) {
        return label + " (" + cd + ")";
    }
    return label;
}

String Scheduler::getLastTriggeredString() {
    return String(_lastTriggeredStr);
}
