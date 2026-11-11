#include <Arduino.h>
#include "hardware_pins.h"
#include "config_manager.h"
#include "relay_controller.h"
#include "network/network_manager.h"
#include "network/weather_service.h"
#include "scheduler/scheduler.h"
#include "network/web_server_handler.h"
#include "gui/gui_manager.h"

/**
 * @file main.cpp
 * @brief ChronoRing - Firmware Universale Multi-Board con Autorilevamento Display Touch / Modalità Headless
 */

void printSystemBanner() {
    Serial.println("\n==================================================");
    Serial.println("             C H R O N O R I N G                  ");
    Serial.println("   Sistema Automazione Campanella Scolastica      ");
    Serial.printf("   Target Hardware : %s\n", BOARD_TARGET_NAME);
    Serial.printf("   Pin Relè        : GPIO %d (%s)\n",
                  RELAY_PIN, RELAY_ACTIVE_LOW ? "Active LOW" : "Active HIGH");
    Serial.println("==================================================");
    Serial.printf(" - MCU Model       : %s (Rev %d, %d Core @ %u MHz)\n",
                  ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores(), ESP.getCpuFreqMHz());
    Serial.printf(" - Flash Size      : %u MB\n", ESP.getFlashChipSize() / (1024 * 1024));
#ifdef BOARD_HAS_PSRAM
    Serial.printf(" - PSRAM Size      : %u KB (Free: %u KB)\n",
                  ESP.getPsramSize() / 1024, ESP.getFreePsram() / 1024);
#else
    Serial.println(" - PSRAM           : Non abilitata / Non presente");
#endif
    Serial.printf(" - Internal Heap   : %u KB (Free: %u KB)\n",
                  ESP.getHeapSize() / 1024, ESP.getFreeHeap() / 1024);
    Serial.println("==================================================");
}

void setup() {
    // 1. PRIORITÀ ASSOLUTA: Messa in sicurezza immediata del pin Relè (RELAY_PIN)
    // Eseguita come primissima istruzione nel setup() per forzare il GPIO al livello
    // inattivo puro prima di qualsiasi altra inizializzazione.
    RelayController::begin(RELAY_ACTIVE_LOW);

    // 2. Inizializzazione Seriale Diagnostica
    // NOTA: Su ESP32-S3 (ARDUINO_USB_CDC_ON_BOOT=1) usa esclusivamente la porta USB-CDC
    // nativa (GPIO 19/20) senza mai invocare Serial0.begin(), lasciando GPIO 43 al Relè.
    Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
    uint32_t serialWaitStart = millis();
    while (!Serial && (millis() - serialWaitStart < 2500)) {
        delay(10);
    }
#endif
    delay(200);

    printSystemBanner();

    // 3. Montaggio sicuro di LittleFS e caricamento /config.json
    if (!ConfigManager::begin()) {
        Serial.println("[MAIN] ATTENZIONE: Avvio con configurazione di emergenza in RAM.");
    }
    ConfigManager::printDiagnostics();

    // 4. Autorilevamento Hardware Display Touch (Probe I2C su 0x38 FT6336G):
    // - Se rilevato (ACK): imposta g_has_display = true, inizializza LovyanGFX, alloca i buffer e avvia il task LVGL.
    // - Se assente (NACK/timeout): imposta g_has_display = false e avvia in modalità Headless (solo Web/Scheduler).
    GuiManager::begin();

    // 5. Avvio Gestione Rete Wi-Fi, Fallback AP ("ChronoRing-Setup"), mDNS ("http://chronoring.local") e NTP
    NetworkManager::begin();

    // 6. Avvio Client Meteo Asincrono Open-Meteo (solo se il display è collegato)
    if (g_has_display) {
        WeatherService::begin();
    }

    // 7. Avvio Scheduler Settimanale della Campanella (Task FreeRTOS a 1 Hz)
    Scheduler::begin();

    // 8. Avvio Web Server Locale sulla Porta 80 + Dashboard + Pagina OTA (/update)
    WebServerHandler::begin();

    Serial.println("==================================================");
    Serial.printf("[MAIN] ChronoRing operativo in modalità: %s\n",
                  g_has_display ? "DISPLAY TOUCH (GUI LVGL + Web + Scheduler)"
                                : "HEADLESS (Solo Web Server + Scheduler + Relè)");
    Serial.printf(" - Dashboard Web : http://%s.local (oppure http://%s)\n",
                  NetworkManager::MDNS_HOSTNAME, NetworkManager::getIpAddress().c_str());
    Serial.printf(" - Pagina OTA    : http://%s.local/update\n", NetworkManager::MDNS_HOSTNAME);
    Serial.println(" - Comandi Seriali: 't'=Suona Ora, 's'=Stop Relè, 'w'=Forza Meteo, 'p'=Diagnostica");
    Serial.println("==================================================");
}

void loop() {
    // Interfaccia seriale diagnostica non bloccante
    if (Serial.available() > 0) {
        char cmd = Serial.read();
        switch (cmd) {
            case 't':
            case 'T': {
                uint32_t pulseMs = ConfigManager::getRelayPulseMs();
                Serial.printf("[TEST] Attivazione manuale relè (%u ms)...\n", pulseMs);
                RelayController::trigger(pulseMs);
                break;
            }
            case 's':
            case 'S': {
                RelayController::forceOff();
                break;
            }
            case 'w':
            case 'W': {
                Serial.println("[TEST] Richiesta aggiornamento immediato Open-Meteo...");
                WeatherService::requestImmediateUpdate();
                break;
            }
            case 'p':
            case 'P': {
                ConfigManager::printDiagnostics();
                WeatherData wd = WeatherService::getWeatherData();
                Serial.printf(" - Orario Attuale : %s (%s)\n",
                              NetworkManager::getFormattedTime().c_str(),
                              NetworkManager::getFormattedDate().c_str());
                Serial.printf(" - Rete           : %s | IP: %s\n",
                              NetworkManager::getNetworkModeString().c_str(),
                              NetworkManager::getIpAddress().c_str());
                Serial.printf(" - Prossimo Suono : %s\n", Scheduler::getNextBellString().c_str());
                Serial.printf(" - Meteo          : %s | %.1f C | Umidita %d%% | Vento %.1f km/h\n",
                              wd.conditionText, wd.temperatureC, wd.humidityPct, wd.windSpeedKmh);
                break;
            }
            default:
                break;
        }
    }

    vTaskDelay(pdMS_TO_TICKS(50));
}
