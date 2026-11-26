# Firmware Precompilati ChronoRing (`firmware/`)

Questa cartella contiene i file binari `.bin` precompilati e pronti per l'aggiornamento **Over-The-Air (OTA)** tramite l'interfaccia web (`/update`) oppure tramite tool seriale (`esptool.py` / Flash Download Tool) per tutte le schede supportate da **ChronoRing**.

I file vengono rigenerati e copiati automaticamente al termine di ogni compilazione tramite lo script PlatformIO [`../export_firmwares.py`](../export_firmwares.py).

## Tabella Binari per Scheda di Destinazione

| File Binario `.bin` | Ambiente PlatformIO | Microcontrollore Compatibile | Display Touch 2.8" | Pin Comando Relè | Note |
| :--- | :--- | :--- | :---: | :---: | :--- |
| **`chronoring_freenove_esp32s3_display.bin`** | `freenove_esp32_s3_n16r8` | **Freenove FNK0104B ESP32-S3-WROOM-1 N16R8** (`16 MB` Flash / `8 MB` OPI PSRAM) | **Sì** (`ILI9341V` SPI + Touch `FT6336G`, *auto-probe headless*) | **GPIO 44** (`RXD` connettore UART 4P, *Active HIGH*) | Firmware completo per la centralina con schermo touch o utilizzo senza schermo su ESP32-S3 |
| **`chronoring_esp32_classic_headless.bin`** | `esp32_classic` | **ESP32 Classic** (`ESP-WROOM-32` / DevKit V1 `4 MB` Flash) | **No** *(Modalità Headless / Web Dashboard)* | **GPIO 4** (*Active HIGH*) | Partizionamento Dual-OTA `min_spiffs.csv`, senza `LovyanGFX`/`lvgl` |
| **`chronoring_esp32c3_headless.bin`** | `esp32_c3` | **ESP32-C3** (SuperMini / DevKitM-1 RISC-V `4 MB` Flash) | **No** *(Modalità Headless / Web Dashboard)* | **GPIO 2** (*Active HIGH*) | Partizionamento Dual-OTA `min_spiffs.csv`, USB CDC abilitato, senza `LovyanGFX`/`lvgl` |

> Nella cartella sono presenti anche `bootloader.bin` e `partitions.bin` di riferimento per l'eventuale flash completo iniziale su schede ESP32-S3 vergini.

> ⚠️ **Importante per schede da `4 MB` (`ESP32 Classic` e `ESP32-C3`)**: la tabella partizioni predefinita di Arduino-ESP32 limita ogni slot OTA a ~`1.28 MB` e causa `Errore OTA: Not Enough Space`. I binari headless usano il partizionamento **`min_spiffs.csv`** (~`1.9 MB` per ciascuno slot OTA + ~`190 KB` per LittleFS) escludendo `LovyanGFX` e `lvgl` (`lib_ignore`). Al **primo passaggio** da un firmware con partizioni standard a `min_spiffs.csv` è obbligatorio eseguire il **flash via cavo USB** (es. `pio run -e esp32_classic -t upload` oppure tramite `esptool`/Web Flasher con la nuova tabella partizioni); tutti gli aggiornamenti successivi potranno poi avvenire comodamente via **Web OTA (`/update`)**.

## Aggiornamento Rapido via Web OTA (`/update`)

1. Scarica da questa cartella il file `.bin` corrispondente alla tua scheda.
2. Accedi alla Dashboard web di ChronoRing (`http://<IP_DISPOSITIVO>` oppure `http://chronoring.local`) e apri **⬆️ Aggiornamento OTA** (`/update`).
3. Seleziona il file `.bin` corretto e avvia il caricamento: il dispositivo verificherà la flash Dual-OTA e si riavvierà automaticamente.
