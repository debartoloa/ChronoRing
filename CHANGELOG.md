# Changelog

Tutte le modifiche rilevanti al progetto **ChronoRing** sono documentate in questo file.

Il formato è basato su [Keep a Changelog](https://keepachangelog.com/it/1.0.0/),
e questo progetto aderisce al [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.4.0] - 2026-11-22

### Added
- **Super Password Tecnico / Manutentore (`ConfigManager`, `WebServerHandler`, `main.cpp`)**:
  - Salvataggio isolato nella partizione **NVS** (`cr_sec`) tramite **salt casuale da 16 byte** (`esp_fill_random`) e **SHA-256 iterato (`1000` round)** con confronto a tempo costante (`mbedtls`). Non viene mai esposta in `chronoring-backup.json`, non è hardcoded nel firmware e sopravvive al normale *Ripristino di Fabbrica* della scuola.
  - **Wizard Una Tantum (`/setup-super`, `POST /api/super-setup`)**: dopo il primo login (su scheda vergine o appena aggiornata), guida il tecnico all'impostazione obbligatoria della Super Password (`8..63` caratteri) e opzionalmente della password gestori scuola, autobloccandosi poi in modo permanente.
  - **Login trasparente a doppio ruolo (`ADMIN` / `SUPERADMIN`)** con protezione **Anti Brute-Force** (`5` tentativi falliti → blocco per `60` secondi con HTTP `429`).
  - **Area Riservata Tecnico** in Dashboard (visibile esclusivamente in sessione `SUPERADMIN` con badge `🛠️ MODALITÀ TECNICO`): consente di reimpostare la password dimenticata dalla scuola senza perdere la sessione, ruotare la Super Password (`POST /api/super-password`) o eseguire il **Reset Totale** inclusa la NVS (`POST /api/factory-reset-full`).
  - **Recupero fisico USB-C** tramite doppio invio seriale di `'R'` maiuscolo entro 5 secondi e inclusione della firma binaria `FW_VERSION_SIGNATURE` nel `.bin`.

## [1.3.2] - 2026-11-22

### Fixed
- **Risolto il blocco del multicast `chronoring.local` dopo ~2 ore (IGMP Snooping Pruning & GTK Rekey)**:
  - Diagnosi diretta sulla scheda in rete (`192.168.8.76`): confermato che server HTTP, ping e query mDNS unicast sulla porta `5353` continuavano a rispondere, mentre il router/AP smetteva di inoltrare i pacchetti multicast `224.0.0.251:5353` verso l'ESP32-S3 dopo il timeout della tabella IGMP snooping (e dopo eventuali rotazioni chiave Wi-Fi GTK / `MDNS.end()` periodici).
  - Rimosso il teardown periodico `MDNS.end()` che inviava pacchetti `IGMP Leave` al router e risolto il race condizionale post-boot di `_mdnsRefreshRequested`.
  - Aggiunto keepalive nativo ogni **45 secondi** sul thread TCPIP lwIP (`s_mdnsIgmpKeepaliveCb`): verifica/ripristina l'iscrizione a `224.0.0.251`, trasmette un `IGMP Membership Report` (`igmp_report_groups`) e invia un annuncio mDNS *unsolicited* A-record conforme RFC 6762 (`TTL 120s`, `TTL IP 255`) senza conflitti con il PCB di `ESPmDNS`.
  - Esteso il Watchdog ogni **5 minuti** con sonda reale di ricezione multicast (`mdns_query_ptr("_http", "_tcp")`): se altri responder nella LAN diventano improvvisamente irraggiungibili (`3` controlli consecutivi, dopo almeno un riscontro positivo), forza `IGMP rejoin` prima ed eventuale riassociazione STA poi per ripristinare il filtro/chiave WPA2 GTK.

### Added
- **Fallback NetBIOS (`NBNS`) per Windows (`NetworkManager`)**:
  - Avviato `NBNS.begin("chronoring")` in modalità STA per consentire l'accesso dalla LAN Windows anche tramite `http://chronoring/` (UDP 137 broadcast) oltre a `http://chronoring.local/`.
- **Diagnostica IGMP/Multicast nel Report Seriale (`main.cpp`)**:
  - Aggiunti il contatore `keepalive IGMP/mDNS` e lo stato `peer mDNS RX` al comando seriale `'p'`.

## [1.3.1] - 2026-11-22

### Fixed
- **Stabilità Rete Wi-Fi & Raggiungibilità `chronoring.local` su Uptime H24 (`NetworkManager`)**:
  - Disattivato il risparmio energetico radio Modem Sleep (`WiFi.setSleep(false)` e `esp_wifi_set_ps(WIFI_PS_NONE)`) all'avvio, alla riconnessione e allo spegnimento SoftAP per impedire la perdita dei pacchetti multicast mDNS (`224.0.0.251:5353`) su sessioni prolungate di più giorni.
  - Registrato handler eventi Wi-Fi (`ARDUINO_EVENT_WIFI_STA_GOT_IP`, `LOST_IP`, `DISCONNECTED`) con riavvio automatico e pulito di `MDNS` (`MDNS.end()` + `MDNS.begin()` + `addService`) ad ogni rinnovo DHCP/cambio IP e periodicamente ogni 30 minuti (`MDNS_REFRESH_INTERVAL_MS`).
  - Verifica esplicita di `WiFi.localIP() != 0.0.0.0` in `isStaConnected()` e mantenimento di `WiFi.setAutoReconnect(true)` quando l'Hotspot AP è bloccato (`isApLocked()`), evitando stalli nella riconnessione automatica.
  - Aggiunto **Watchdog adattivo anti-stato "zombie"** (`WIFI_HEALTH_CHECK_INTERVAL_MS` = 5 min): rileva eventuali espulsioni silenti dal router/DHCP dopo che la rete era funzionante e forza il ripristino pulito della sessione STA.
- **Hardening HTTPS Meteo & Riduzione Frammentazione Heap GUI (`WeatherService`, `GuiManager`)**:
  - Aumentato lo stack di `ChronoWeatherTask` a `8192` byte e forzato `http.setReuse(false)` + `client.stop()` su ogni percorso di uscita in `fetchFromOpenMeteo()`.
  - Portato il refresh testuale LVGL in `GuiManager::refreshDynamicData()` a max `1 Hz` tramite buffer su stack e controllo `strcmp` prima di aggiornare le label, saltando i ridisegni quando il display è spento al `0%`.

### Added
- **Mostra Indirizzo IP su Display Touch & Diagnostica Seriale Estesa (`GuiManager`, `main.cpp`)**:
  - Toccando la barra superiore (Header) sul display touch da 2.8", la data lascia temporaneamente spazio per 10 secondi all'indirizzo `IP: xxx.xxx.xxx.xxx` (utile nelle reti scolastiche che filtrano `.local`).
  - Aggiunto comando seriale `'m'` per forzare il riavvio mDNS e arricchito il comando `'p'` con uptime, Heap libero/minimo storico, RSSI e contatori disconnessioni Wi-Fi/mDNS.

## [1.3.0] - 2026-11-20

### Added
- **Gestione Timeout Display Configurabile (Standby vs Spegnimento Totale)**:
  - Aggiunti `display_timeout_sec` (`10..3600` secondi, default `60 s`) e `display_sleep_off` (`false` = Standby orologio/logo a luminosità ridotta, `true` = Spegnimento totale retroilluminazione `0%` su schermo nero) in `/config.json` e nella Dashboard Web (`POST /api/display-settings`).
  - Risveglio immediato dello schermo in entrambe le modalità tramite tocco touch oppure al verificarsi di un evento (attivazione relè campanella o cambio stato da Web).
- **Backup & Ripristino Configurazione (`Export / Import JSON`)**:
  - Endpoint `GET /api/backup` e pulsante **Scarica Backup (.json)** per esportare orari, etichette e tutte le impostazioni in `chronoring-backup.json`.
  - Endpoint `POST /api/restore` (upload multipart con validazione rigorosa e controllo campi) e pulsante **Ripristina da Backup** nella Dashboard Web, con riavvio automatico al termine.
- **Blocco / Timeout Sicurezza dell'Hotspot Access Point (`NetworkManager`)**:
  - Aggiunti `ap_lock_after_sta` (`true` di default: spegne totalmente la SoftAP in modalità STA e blocca la riapertura automatica di hotspot non presidiati se cade temporaneamente la rete scolastica) e `ap_timeout_min` (`0..120` min, default `15 min`: spegne l'hotspot dopo il timeout senza client connessi), configurabili da Web (`POST /api/ap-policy`) e riattivabili via comando seriale `'a'`.
- **Programmazione Orari & Giorni per Singola Campanella (`Dashboard Web`, `Scheduler`)**:
  - Il pulsante `+ Aggiungi Orario` crea ora una riga pulita (orario ed etichetta vuoti con placeholder guida), evidenzia visivamente la riga in modifica (`.editing-row` + animazione) e sposta automaticamente scroll e focus sul campo orario; aggiunta validazione prima del salvataggio.
  - Selettore giorni per singola campanella (`Lun`–`Dom`) con 7 pillole cliccabili e 4 preset rapidi 1-clic (`Lun-Ven`, `Lun-Sab`, `Lun-Mer-Ven`, `Mar-Gio-Sab`), con supporto bitmask completo a 7 giorni nello `Scheduler` e chunking multi-pacchetto su `POST /api/schedule`.

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
