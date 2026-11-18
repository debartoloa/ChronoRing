# Changelog

Tutte le modifiche rilevanti al progetto **ChronoRing** sono documentate in questo file.

Il formato è basato su [Keep a Changelog](https://keepachangelog.com/it/1.0.0/),
e questo progetto aderisce al [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.2.0] - 2026-11-18

### Added
- **Sicurezza Hotspot Access Point (SSID & Password WPA2 Personalizzabili)**:
  - Aggiunti `ap_ssid` e `ap_password` in `SystemConfig` / `/config.json` con validazione WPA2 (almeno 8 caratteri oppure vuoto per AP aperto).
  - Nuova sezione nella Dashboard Web e endpoint `POST /api/ap-config` per personalizzare il nome e la password dell'hotspot integrato con riavvio automatico dell'AP (`NetworkManager::requestApRestart()`).
- **Ripristino Impostazioni di Fabbrica (Factory Reset)**:
  - Aggiunto pulsante con doppia conferma nella Dashboard Web ed endpoint `POST /api/factory-reset` (`ConfigManager::resetToFactoryDefaults()`) che elimina logo/personalizzazioni, ripristina orari e credenziali di fabbrica (`ChronoRing-Setup` aperto, password web `admin`) e riavvia in sicurezza il dispositivo.

## [1.1.0] - 2026-11-18

### Added
- **Etichette Personalizzabili per ogni Campanella**:
  - Aggiunto campo `label` (max 30 caratteri visibili / buffer 32 byte) per ciascuna voce in `BellScheduleItem` e persistenza su `/config.json` in LittleFS, con fallback automatico a `"Campanella"` per configurazioni preesistenti.
  - Nuova colonna **Etichetta / Evento** modificabile nella tabella orari della Dashboard Web (`/api/config` e `/api/schedule`).
  - Scorrimento automatico (`LV_LABEL_LONG_SCROLL_CIRCULAR`) sul display touch LVGL per etichette lunghe nel box del prossimo suono.
- **Hotspot Wi-Fi `ChronoRing-Setup` Stabile, Pre-Scan & Captive Portal (`NetworkManager`, `WebServerHandler`)**:
  - Scansione preventiva all'avvio (`isSavedNetworkVisible`): se la rete Wi-Fi salvata non è presente nell'etere, attiva subito l'hotspot aperto `ChronoRing-Setup` senza attese inutili e senza channel-hopping STA in background.
  - Configurazione esplicita `192.168.4.1` (Gateway e Subnet `255.255.255.0`, canale fisso 1) con DHCP Server attivo e sospensione dei retry STA mentre un dispositivo è connesso all'hotspot.
  - Server DNS Captive Portal (`DNSServer` su porta 53 + route di rilevamento Android/iOS/Windows) per aprire automaticamente il pannello di ChronoRing appena ci si collega all'hotspot.
  - Scansione Wi-Fi asincrona non-bloccante (`WiFi.scanNetworks(true, ...)` protetta da grace period e debounce) con **riquadro popup modale** nella Dashboard Web (`🔍 Cerca Reti`) per selezionare la rete rilevata senza interrompere la connessione attiva.
- **Versione Firmware Integrata (`CHRONORING_FW_VERSION`)**:
  - Definito il numero di versione centralizzato in `hardware_pins.h` (`1.1.0`), stampato nel banner seriale all'avvio ed esposto via `/api/status` su Dashboard Web, pagina Login e pagina di aggiornamento OTA (`/update`).

### Changed
- **Scheduler Dinamico (`Scheduler`)**:
  - Rimossa la funzione con nomi cablati (`Scheduler::getBellEventName`): lo Scheduler utilizza ora esclusivamente l'etichetta definita dall'utente sia per il prossimo evento sul display sia per lo storico dell'ultimo squillo (`HH:MM (Etichetta)`).

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
