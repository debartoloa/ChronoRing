# Changelog

Tutte le modifiche rilevanti al progetto **ChronoRing** sono documentate in questo file.

Il formato è basato su [Keep a Changelog](https://keepachangelog.com/it/1.0.0/),
e questo progetto aderisce al [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2026-11-15

### Added
- **Architettura Core & Fail-Safe Relè (`RelayController`)**:
  - Inizializzazione anti-glitch all'avvio tramite costruttore pre-init (`__attribute__((constructor(101)))`) e `GPIO 44` (`RXD`) per evitare qualsiasi impulso spurio durante il boot.
  - Spegnimento automatico gestito via timer hardware ad alta precisione (`esp_timer` one-shot) con **Hard Cap di sicurezza a 10 secondi (`10000 ms`)**.
- **Persistenza su LittleFS (`ConfigManager`)**:
  - Salvataggio persistente di orari, durata impulso relè, stato di sospensione, credenziali Wi-Fi, password amministratore, nome dell'istituto e logo della scuola (`/config.json` e `/school_logo.bin`).
- **Rete, NTP & Scheduler (`NetworkManager`, `Scheduler`)**:
  - Connessione Wi-Fi STA con fallback automatico su Access Point `ChronoRing-Setup` (`192.168.4.1`) dopo 20 secondi.
  - Supporto mDNS (`http://chronoring.local`).
  - Sincronizzazione oraria NTP con gestione automatica ora solare/legale italiana (`CET-1CEST,M3.5.0,M10.5.0/3`).
  - Scheduler settimanale a 1 Hz (Lunedì–Sabato) con protezione anti-doppio scatto nello stesso minuto.
- **Web Server & Aggiornamento OTA (`WebServerHandler`)**:
  - Dashboard web responsiva protetta da password con autenticazione via cookie di sessione.
  - Pagina dedicata `/update` per l'aggiornamento firmware **Over-The-Air (OTA)** da browser con messa in sicurezza automatica del relè prima di `Update.begin()`.
  - Upload e conversione client-side RGB565 del logo scolastico per la schermata di Standby.
- **Interfaccia Touch LVGL v8 & Meteo (`DisplayTouchDriver`, `GuiManager`, `WeatherService`)**:
  - Driver `LovyanGFX` per display IPS `ILI9341V` (`320x240` Landscape) e touch capacitivo `FT6336G` (`0x38`) con buffer allocati in PSRAM OPI.
  - Widget meteo asincrono **Open-Meteo** con icone vettoriali moderne.
  - Schermata di **Standby (Screensaver)** automatica dopo 60 secondi di inattività touch con dimming PWM della retroilluminazione, orologio grande, Logo e Nome dell'Istituto.
- **Supporto Universale Multi-Board & Headless**:
  - Autorilevamento hardware del display all'avvio tramite probe I2C su `0x38` (`g_has_display`): avvio automatico in modalità **Headless** (solo Web/Scheduler/Relè) se lo schermo non è collegato.
  - Ambienti PlatformIO multi-board (`freenove_esp32_s3_n16r8`, `esp32_classic`, `esp32_c3`) con mappatura pin condizionale in `src/hardware_pins.h`.
- **Documentazione**:
  - Aggiunti `README.md`, `CHANGELOG.md`, `.gitignore` e logo ufficiale in `docs/chronoring_logo.jpg`.
