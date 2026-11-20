# 🗺️ Roadmap di Sviluppo — ChronoRing

Questo documento raccoglie le funzionalità pianificate e le proposte di evoluzione futura per **ChronoRing**.

---

## 🔒 1. Sicurezza di Rete e Protezione dell'Infrastruttura Scolastica *(Pianificato)*

Poiché ChronoRing opera collegato alla rete Wi-Fi dell'istituto scolastico, l'obiettivo è ridurre al minimo la superficie d'attacco ed evitare che il dispositivo possa diventare un punto d'ingresso non autorizzato:

- **Hashing delle Password su LittleFS**: sostituire il salvataggio in chiaro della password Web in `/config.json` con hash crittografico + salt (`SHA-256` / `PBKDF2`) e impedire l'esposizione di credenziali Wi-Fi nei log seriali.
- **Protezione Sessioni & Anti-Brute-Force sul Login**:
  - Limitazione dei tentativi errati (*rate limiting* temporaneo) su `/api/login`.
  - Token di sessione con scadenza automatica (TTL) e flag di sicurezza più restrittivi sui cookie.
- **Accesso Web Crittografato (HTTPS / TLS Locale)**: valutare l'introduzione di certificato autofirmato locale (o supporto reverse-proxy) per cifrare password e sessioni in transito sulla LAN scolastica.
- **Blocco / Timeout dell'Hotspot `ChronoRing-Setup`** ✅ *(Implementato in v1.3.0)*:
  - Disattivazione totale dell'interfaccia SoftAP quando la scheda è connessa in modalità STA alla rete scolastica e blocco automatico della riapertura su cadute temporanee del router (`ap_lock_after_sta`).
  - Spegnimento automatico dell'hotspot di emergenza dopo un intervallo di inattività configurabile (`ap_timeout_min`, default 15 minuti), evitando hotspot aperti non presidiati.
- **Integrità Aggiornamenti OTA**: verifica firma/checksum dell'immagine `firmware.bin` prima di applicare l'aggiornamento Over-The-Air.

---

## 🧩 2. Supporto Completo per Tutte le 4 Famiglie ESP32 *(Pianificato)*

Estendere e verificare la piena compatibilità del firmware (inclusi i relativi ambienti `platformio.ini` e i binari precompilati) su tutta la famiglia Espressif:

- **ESP32-S3** *(già completo e verificato su Freenove FNK0104B)*
- **ESP32 Classico (`WROOM-32` / `WROVER`)**: test completo su hardware reale (Modalità Headless e Display SPI esterno) e generazione del binario dedicato.
- **ESP32-C3 (`RISC-V Single-Core`)**: verifica stabilità FreeRTOS single-core (`CHRONO_NET_CORE = 0`, `CHRONO_APP_CORE = 0`), consumi e gestione memoria RAM senza PSRAM.
- **ESP32-S2 (`Single-Core Xtensa`)**: aggiunta dell'ambiente `[env:esp32_s2]` in `platformio.ini`, mappatura dedicata in `src/hardware_pins.h` (incluso `RELAY_PIN` e USB-CDC nativa) e test completo in modalità Headless.
- **Allineamento con `ESP32-S3-Web-Flasher`**: organizzazione dei binari precompilati per tutte e 4 le architetture (`esp32/`, `esp32s2/`, `esp32s3/`, `esp32c3/`) pronti per il flash diretto da browser.

---

## 💡 3. Idee e Miglioramenti Proposti *(DA VALIDARE)*

Le seguenti funzionalità sono proposte aggiuntive pensate per l'utilizzo quotidiano a scuola e richiedono tua approvazione prima dello sviluppo:

- **Blocco PIN sul Display Touch** `[DA VALIDARE]`
  - Aggiungere un codice PIN rapido (es. 4 cifre) prima di poter premere **SUONA ORA** o **SOSPENDI** direttamente dallo schermo touch, evitando attivazioni accidentali o scherzi da parte degli studenti se il display è accessibile in corridoio.
- **Profili Orari Multipli & Calendario Festività** `[DA VALIDARE]`
  - Supporto a più profili salvabili (es. *Orario Normale*, *Orario Ridotto*, *Sabato*, *Sessione Esami*) attivabili con un clic dalla Dashboard o in base al giorno della settimana.
  - Inserimento di date/intervalli di chiusura automatica (es. *Vacanze di Natale*, *Pasqua*, *Festa Patronale*) per sospendere la campanella senza interventi manuali.
- **Modulo RTC Hardware di Backup (`DS3231` via I2C)** `[DA VALIDARE]`
  - Supporto opzionale a un modulo RTC con batteria tampone collegato al connettore I2C (GPIO 15/16): garantisce l'ora esatta e la continuità degli squilli anche in caso di blackout elettrico seguito da assenza prolungata di connessione Internet/NTP.
- **Backup & Ripristino Configurazione (`Export / Import JSON`)** ✅ *(Implementato in v1.3.0)*
  - Pulsanti nella Dashboard Web per scaricare sul PC un file di backup (`chronoring-backup.json`) con tutti gli orari, le etichette e le impostazioni, e ricaricarlo su un dispositivo nuovo o dopo un ripristino di fabbrica.
- **Campanella Musicale / Annunci Audio tramite Codec `ES8311`** `[DA VALIDARE]`
  - Sfruttare il codec audio I2S `ES8311` e l'amplificatore `NS4150B` già presenti a bordo della Freenove ESP32-S3 per riprodurre un file audio/melodia `.mp3` o `.wav` (da LittleFS o MicroSD) al posto o in affiancamento al relè della campanella tradizionale.
- **Registro Storico degli Squilli (Log Eventi)** `[DA VALIDARE]`
  - Tabella consultabile dalla Dashboard Web con data, ora, etichetta e tipo di attivazione (*Automatico* vs *Manuale*) degli ultimi 20–50 squilli eseguiti.
