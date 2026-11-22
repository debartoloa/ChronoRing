# 🖨️ ChronoRing — Case 3D (Enclosure)

Contenitore compatto progettato su misura per la scheda **Freenove ESP32-S3 Display 2.8" (`FNK0104B`)**, con alloggiamento dedicato e passaggio/collegamento per il modulo relè connesso alla porta **UART a 4 pin** (`GPIO 44` / `RXD`).

---

## 📂 Struttura dei File

| Cartella / File | Formato | Scopo | Link Diretto |
| :--- | :--- | :--- | :--- |
| **`stl/ChronoRing_BOX.stl`** | `.stl` | Modello pronto per lo slicing e la stampa 3D diretta | [`ChronoRing_BOX.stl`](./stl/ChronoRing_BOX.stl) |
| **`cad/ChronoRing_BOX.f3d`** | `.f3d` | Sorgente parametrico modificabile in **Autodesk Fusion 360** *(eventuali esportazioni `.step` possono essere salvate nella stessa cartella)* | [`ChronoRing_BOX.f3d`](./cad/ChronoRing_BOX.f3d) |

---

## ⚙️ Parametri di Stampa 3D Testati

I parametri seguenti sono stati testati sul campo per garantire resistenza meccanica, tolleranze precise su display/connettori e durabilità in ambiente scolastico:

| Parametro | Valore Testato | Note |
| :--- | :--- | :--- |
| **Slicer** | **Anycubic Slicer** | Profilo standard per il materiale |
| **Filamento** | **eSUN PETG Basic** | Resistente al calore e agli urti rispetto al PLA standard |
| **Pareti / Perimetri** | **5 perimetri** | Consigliato per maggiore robustezza strutturale delle pareti e dei punti di fissaggio |
| **Altri parametri** | **Standard profilo PETG** | Altezza layer, riempimento, temperature e velocità predefinite del profilo PETG |

---

## 🛠️ Come Usare i File

### 1. Stampa Diretta (`.stl`)
1. Apri [`stl/ChronoRing_BOX.stl`](./stl/ChronoRing_BOX.stl) in **Anycubic Slicer** (o nello slicer compatibile della tua stampante, es. PrusaSlicer, OrcaSlicer, Bambu Studio).
2. Seleziona il profilo standard **PETG** (consigliato **eSUN PETG Basic**).
3. Imposta **5 perimetri / pareti**.
4. Esegui lo slicing mantenendo gli altri parametri standard del profilo PETG e avvia la stampa.

### 2. Modifica Parametrica in Autodesk Fusion 360 (`.f3d`)
1. Apri [`cad/ChronoRing_BOX.f3d`](./cad/ChronoRing_BOX.f3d) in **Autodesk Fusion 360** (*File → Apri → Seleziona dal computer*).
2. Adatta eventuali fori di fissaggio a parete, aperture per cavi/pressacavi UART o tolleranze in base al tuo modulo relè.
3. Esporta il risultato aggiornato come **`.stl` / `.3mf`** in `hardware/enclosure/stl/` per la stampa (ed eventualmente come **`.step`** in `hardware/enclosure/cad/`).
