<div align="center">
  <img src="docs/chronoring_logo.jpg" alt="ChronoRing Logo" width="220" style="border-radius: 24px;" />
  <h1>🔔 ChronoRing</h1>
  <p><strong>Sistema Intelligente di Automazione per Campanella Scolastica su architettura ESP32</strong></p>

  <p>
    <img src="https://img.shields.io/badge/PlatformIO-ESP32--S3%20%7C%20ESP32%20%7C%20ESP32--C3-orange?style=for-the-badge&logo=platformio" alt="PlatformIO" />
    <img src="https://img.shields.io/badge/Framework-Arduino%20%2F%20FreeRTOS-00979D?style=for-the-badge&logo=arduino" alt="Arduino" />
    <img src="https://img.shields.io/badge/GUI-LVGL%20v8.3%20%2B%20LovyanGFX-10b981?style=for-the-badge" alt="LVGL" />
    <img src="https://img.shields.io/badge/License-MIT-blue?style=for-the-badge" alt="License" />
  </p>
</div>

---

## 📖 Descrizione del Progetto

**ChronoRing** è un sistema completo, affidabile e autonomo per la gestione automatizzata della campanella scolastica, progettato originariamente per la scheda **Freenove ESP32-S3 Display 2.8" (FNK0104B - N16R8)** ma dotato di **architettura universale Multi-Board & Headless**.

All'avvio, il firmware esegue una scansione hardware automatica sul bus I2C (`0x38`):
- **🖥️ Modalità Display Touch**: se rileva lo schermo con controller touch capacitivo `FT6336G`, avvia l'interfaccia grafica interattiva **LVGL v8** a 60 FPS, il client meteo **Open-Meteo** e la schermata di **Standby (Screensaver)** personalizzabile con Logo e Nome dell'Istituto.
- **⚡ Modalità Headless**: se nessun display è collegato (o se compilato su schede classiche `ESP32` / `ESP32-C3` / `ESP32-C6`), disattiva automaticamente i buffer grafici e opera come centralina pura tramite **Dashboard Web**, **Scheduler NTP** e **Relè**.

---

## ✨ Caratteristiche Principali

- **🕒 Sincronizzazione Oraria NTP & Fuso Orario Italiano Automatico**
  - Sincronizzazione con `pool.ntp.org` e `time.nist.gov`.
  - Gestione nativa del passaggio automatico tra **Ora Solare (CET)** e **Ora Legale (CEST)** tramite regola POSIX `CET-1CEST,M3.5.0,M10.5.0/3`.
- **📅 Scheduler Settimanale (Lunedì – Sabato)**
  - Programmazione oraria salvata in modo persistente su filesystem **LittleFS** (`/config.json`).
  - Calcolo in tempo reale del prossimo suono con conto alla rovescia.
  - Interruttore di **Sospensione Globale** (per vacanze, festività o sospensione attività didattiche) sincronizzato istantaneamente tra Display Touch e Dashboard Web.
- **🛡️ Sicurezza Hardware del Relè (Anti-Blocco)**
  - Spegnimento automatico gestito tramite timer hardware ad alta precisione (`esp_timer` one-shot).
  - **Hard Cap di sicurezza a 10 secondi (`10000 ms`)**: impedisce che la campanella resti incastrata o attiva oltre il limite massimo anche in caso di configurazione errata.
- **🌐 Web Dashboard & Aggiornamento Firmware OTA**
  - Pannello di controllo responsivo protetto da password amministratore e sistema **Super Password Tecnico di recupero** in NVS (`SHA-256 + salt` iterato con Wizard iniziale una tantum, protezione anti brute-force e area riservata manutentore per reimpostare la password scuola dimenticata), accessibile tramite **`http://chronoring.local`** (mDNS + keepalive IGMP/RFC 6762 ogni 45s), **`http://chronoring`** (NetBIOS su Windows) o IP diretto (mostrabile anche toccando l'header sul display).
  - Gestione in tempo reale di: *Suona Ora (Test Manuale)*, *Sospensione*, *Tabella Orari*, *Durata Impulso Relè*, *Credenziali Wi-Fi*, *Logo e Nome della Scuola*.
  - **Fallback Access Point automatico con Blocco/Timeout di Sicurezza**: se la rete Wi-Fi configurata non viene rilevata nell'etere (verifica preventiva con pre-scan), ChronoRing avvia l'hotspot **`ChronoRing-Setup`** (SSID e password WPA2 personalizzabili dalla Dashboard) su IP **`192.168.4.1`** con DHCP Server e **Captive Portal** integrato. Una volta connesso alla rete scolastica (`STA`), l'hotspot si spegne totalmente e rimane bloccato per sicurezza (`ap_lock_after_sta`), oltre a spegnersi automaticamente dopo un timeout di inattività configurabile senza client connessi (`15 min` di default).
  - **Backup / Ripristino (`.json`), Factory Reset** e gestione completa di: *Suona Ora*, *Sospensione*, *Tabella Orari con etichette*, *Scansione reti Wi-Fi popup*, *Durata Impulso Relè*, *Logo e Nome della Scuola*.
  - Pagina dedicata **`/update`** per l'aggiornamento del firmware **Over-The-Air (OTA)** direttamente dal browser caricando il file `.bin`.
- **🎨 Interfaccia Touch LVGL v8 & Standby / Spegnimento Display Configurabile**
  - Layout scuro ad alto contrasto (`320x240` Landscape) con orologio digitale grande, countdown prossima campanella, widget meteo **Open-Meteo** con icone vettoriali moderne e configurazione Wi-Fi da schermo.
  - **Standby o Spegnimento Display con Timeout Personalizzabile**: dalla Dashboard puoi scegliere la durata del timeout (`10–3600 s`, default `60 s`) e decidere se mostrare la schermata di **Standby** (orologio + Logo e Nome dell'Istituto a luminosità ridotta) oppure **spegnere completamente** la retroilluminazione (`0%`). In entrambi i casi lo schermo si riaccende subito al tocco o su evento campanella/sistema.

---

## 🔌 Schema di Collegamento Hardware (Freenove FNK0104B ESP32-S3)

| Periferica | Funzione | Pin ESP32-S3 | Note |
| :--- | :--- | :---: | :--- |
| **Modulo Relè** | Comando Campanella | **GPIO 44 (`RXD`)** | Pin `RX` del connettore UART a 4 pin della scheda |
| **Display ILI9341V** | SPI MOSI / MISO / SCLK | GPIO 11 / 13 / 12 | Bus SPI2 (`HSPI`) @ 40 MHz |
| **Display ILI9341V** | CS / DC / Backlight | GPIO 10 / 46 / 45 | Backlight PWM (Canale LEDC 7) |
| **Touch FT6336G** | I2C SDA / SCL | GPIO 16 / 15 | Indirizzo I2C `0x38` @ 400 kHz |
| **Touch FT6336G** | RST / INT | GPIO 18 / 17 | Usato per l'autorilevamento all'avvio |
| **LED RGB On-Board** | Stato Operativo | GPIO 42 | Verde fisso = Attivo, Rosso fisso = Sospeso |

### Ambienti Multi-Board Supportati in `platformio.ini`

| Ambiente PlatformIO | Scheda / Architettura | Pin Relè Default (`RELAY_PIN`) |
| :--- | :--- | :---: |
| `freenove_esp32_s3_n16r8` *(default)* | Freenove ESP32-S3 (16MB Flash + 8MB PSRAM OPI) | `GPIO 44` (`RX`) |
| `esp32_classic` | ESP32 WROOM / DevKit V1 (Dual-Core Xtensa) | `GPIO 4` |
| `esp32_c3` | ESP32-C3 SuperMini / DevKitM-1 (Single-Core RISC-V) | `GPIO 2` |

---

## 🖨️ Hardware & 3D Enclosure

Il repository include i file del **case 3D compatto** progettato per la scheda **Freenove ESP32-S3 Display 2.8" (`FNK0104B`)** con collegamento del modulo relè su porta **UART** (`GPIO 44` / `RXD`):

- **Stampa diretta (`.stl`)**: [`hardware/enclosure/stl/ChronoRing_BOX.stl`](./hardware/enclosure/stl/ChronoRing_BOX.stl)
- **Sorgente Fusion 360 (`.f3d`)**: [`hardware/enclosure/cad/ChronoRing_BOX.f3d`](./hardware/enclosure/cad/ChronoRing_BOX.f3d)
- **Profilo testato**: **Anycubic Slicer**, filamento **eSUN PETG Basic**, **5 perimetri** e impostazioni standard PETG.

👉 Consulta la guida completa nella cartella [**`hardware/enclosure/`**](./hardware/enclosure/).

---

## ⚠️ Nota Importante sul Relè all'Avvio (Connettore UART a 4 Pin)

Sulla scheda **Freenove ESP32-S3**, il connettore UART a 4 pin espone `5V`, `GND`, `TX (GPIO 43)` e `RX (GPIO 44)`:
- **`GPIO 43` (`U0TXD`)** è cablato nel silicio dell'ESP32-S3 come uscita seriale del **ROM Bootloader**. Nei primissimi millisecondi di accensione (prima ancora che il firmware venga caricato), il chip porta `GPIO 43` a **3,3V** per trasmettere il log di avvio della ROM.
- Se utilizzi un modulo relè **Active HIGH** (che scatta quando riceve `3,3V`), quel breve segnale del bootloader può far eccitare il relè per una frazione di secondo durante il riavvio.
- Per eliminare completamente qualsiasi scatto al riavvio hai due soluzioni immediate:
  1. **Spostare il filo del segnale sul pin accanto (`RX` / `GPIO 44`)** dello stesso connettore a 4 pin e impostare `#define RELAY_PIN 44` in `src/hardware_pins.h` (poiché `RX` è un ingresso durante il boot e non trasmette dati).
  2. **Oppure usare la logica `Active LOW`**: se il modulo relè dispone del ponticello `H/L` (High/Low Trigger), impostalo su **`L` (Low Trigger)** e imposta `RELAY_ACTIVE_LOW = true` in `src/hardware_pins.h`. In questo modo i `3,3V` presenti su `GPIO 43` durante il boot corrisponderanno esattamente allo stato **Relè Spento**.

---

## ⚡ Primo Caricamento su ESP32 Vergine (Senza Installare Software — Web Flasher)

Se hai una scheda **ESP32-S3 vergine** (mai programmata prima) e vuoi installare **ChronoRing** senza installare Arduino IDE o PlatformIO, puoi utilizzare direttamente dal browser il tool ufficiale **[ESP32-S3-Web-Flasher](https://github.com/debartoloa/ESP32-S3-Web-Flasher)**:

> 🌐 **Apri lo strumento Web Flasher su GitHub Pages:** **[https://debartoloa.github.io/ESP32-S3-Web-Flasher/](https://debartoloa.github.io/ESP32-S3-Web-Flasher/)**

### 1. Requisiti
- Un browser desktop con supporto **Web Serial API**: **Google Chrome**, **Microsoft Edge** oppure **Opera**.
- Un **cavo USB dati** collegato alla porta USB della scheda ESP32-S3.
- Scarica sul tuo PC il file [`firmware/firmware.bin`](./firmware/firmware.bin) dalla cartella [`firmware/`](./firmware) di questo repository *(nella stessa cartella trovi anche `bootloader.bin` e `partitions.bin` in caso di caricamento manuale avanzato via `esptool`)*.

### 2. Procedura Guidata a 3 Step

1. **STEP 1 — DISPOSITIVO:**
   - Collega la scheda ESP32-S3 via USB al computer.
   - Nella pagina del Web Flasher clicca su **"Seleziona Dispositivo USB"** e scegli la porta seriale (`COMx` / `USB JTAG/serial debug unit`).
   - La famiglia chip viene auto-rilevata (`ESP32-S3 (bootloader 0x0000)`).
   - *Nota su schede vergini:* se la scheda non entra automaticamente in modalità programmazione, tieni premuto il tasto **`BOOT`**, premi e rilascia **`RESET`**, poi rilascia **`BOOT`** e riprova la connessione.
2. **STEP 2 — FIRMWARE:**
   - Seleziona la scheda **"Carica tuo .bin"**.
   - Seleziona dal tuo PC il file **`firmware.bin`** appena scaricato da [`firmware/`](./firmware).
   - Lascia attiva la spunta **"Aggiungi automaticamente `bootloader.bin`, `partitions.bin` e `boot_app0.bin` per primo flash (offset `0x10000`)"**: in questo modo lo strumento comporrà in automatico l'intera mappa di memoria necessaria al primo avvio:

     | Indirizzo Flash | File Caricato | Descrizione |
     | :--- | :--- | :--- |
     | `0x0000` | `bootloader.bin` | Bootloader ESP32-S3 *(aggiunto in automatico)* |
     | `0x8000` | `partitions.bin` | Tabella partizioni Dual-OTA *(aggiunta in automatico)* |
     | `0xE000` | `boot_app0.bin` | Selettore partizione OTA *(aggiunto in automatico)* |
     | `0x10000` | **`firmware.bin`** | Firmware applicativo **ChronoRing** |

3. **STEP 3 — FLASH:**
   - Lascia attiva l'opzione **"Cancella memoria e installa da zero (consigliato al primo flash)"**.
   - Premi **"Flash Firmware"** e attendi il completamento al 100%: al termine l'ESP32 si riavvierà automaticamente ed entrerà in funzione.

### 3. Primo Avvio & Aggiornamenti Futuri (OTA)
- Al primo avvio, non essendo presente alcuna rete Wi-Fi configurata, ChronoRing attiverà l'hotspot **`ChronoRing-Setup`** su **`http://192.168.4.1`** per la configurazione guidata.
- Per tutti gli **aggiornamenti successivi**, non servirà più collegare il cavo USB: basterà aprire la pagina **`http://chronoring.local/update`** dalla Dashboard Web e caricare direttamente il solo file **`firmware.bin`**.

---

## 🚀 Compilazione e Installazione (Sviluppatori / PlatformIO)

### Prerequisiti
- [Visual Studio Code](https://code.visualstudio.com/) con estensione [PlatformIO IDE](https://platformio.org/).

### Comandi Rapidi CLI
```bash
# Compilazione per la scheda principale Freenove ESP32-S3 N16R8
pio run -e freenove_esp32_s3_n16r8

# Caricamento firmware via USB
pio run -e freenove_esp32_s3_n16r8 -t upload

# Apertura Monitor Seriale (115200 baud)
pio device monitor
```

### Comandi Diagnostici da Monitor Seriale (`115200 baud`)
- `t` : Attiva manualmente la campanella per la durata configurata (`Suona Ora`).
- `s` : Arresta immediatamente il relè (`Force Off`).
- `w` : Forza un aggiornamento immediato dei dati meteo da Open-Meteo.
- `a` : Riattiva manualmente l'Hotspot AP (se spento per timeout).
- `p` : Stampa il report diagnostico completo (Orario, Rete, Prossimo Suono, Meteo, LittleFS).

---

## 📂 Struttura del Progetto

```text
Esp32-Campanella/
├── docs/
│   └── chronoring_logo.jpg          # Logo ufficiale ChronoRing
├── firmware/                        # Binari precompilati pronti per il flash / OTA
│   ├── bootloader.bin
│   ├── firmware.bin
│   └── partitions.bin
├── hardware/
│   └── enclosure/                   # Case 3D per Freenove ESP32-S3 Display 2.8"
│       ├── cad/                     # File sorgenti CAD modificabili (.f3d Fusion 360)
│       │   └── ChronoRing_BOX.f3d
│       ├── stl/                     # Modelli pronti per slicing e stampa 3D (.stl)
│       │   └── ChronoRing_BOX.stl
│       └── README.md                # Parametri di stampa testati e istruzioni
├── include/
│   └── lv_conf.h                    # Configurazione LVGL v8.3 (PSRAM / SRAM dinamica)
├── src/
│   ├── gui/
│   │   ├── display_touch_driver.h   # Probe I2C 0x38, driver LovyanGFX ILI9341V + FT6336G
│   │   ├── display_touch_driver.cpp
│   │   ├── gui_manager.h            # Interfaccia LVGL, icone meteo vettoriali e Standby
│   │   └── gui_manager.cpp
│   ├── network/
│   │   ├── network_manager.h        # Gestione Wi-Fi STA, Fallback AP, mDNS e NTP POSIX
│   │   ├── network_manager.cpp
│   │   ├── weather_service.h        # Client HTTPS asincrono Open-Meteo
│   │   ├── weather_service.cpp
│   │   ├── web_server_handler.h     # Dashboard Web, API REST JSON, Upload Logo e OTA
│   │   └── web_server_handler.cpp
│   ├── scheduler/
│   │   ├── scheduler.h              # Motore orario settimanale a 1 Hz (Lun-Sab)
│   │   └── scheduler.cpp
│   ├── config_manager.h             # Persistenza /config.json e /school_logo.bin su LittleFS
│   ├── config_manager.cpp
│   ├── hardware_pins.h              # Mappatura pin condizionale Multi-Board
│   ├── relay_controller.h           # Driver Relè fail-safe con esp_timer e Hard Cap 10s
│   ├── relay_controller.cpp
│   └── main.cpp                     # Entry point del sistema
├── partitions_16MB.csv              # Tabella partizioni custom 16MB (Dual OTA + 7.8MB LittleFS)
└── platformio.ini                   # Configurazione ambienti Multi-Board PlatformIO
```

---

## 🔐 Credenziali Predefinite

- **Access Point di Emergenza Wi-Fi**:
  - **SSID**: `ChronoRing-Setup` *(rete aperta, senza password)*
  - **IP / DHCP Server**: `192.168.4.1` *(assegna automaticamente `192.168.4.2+` ai client)*
  - **URL**: `http://192.168.4.1` *(o apertura automatica tramite Captive Portal)*
- **Dashboard Web (`http://chronoring.local`)**:
  - **Password Amministratore di default**: `admin` *(modificabile dalla Dashboard)*
