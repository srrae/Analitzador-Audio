# Analitzador d'Àudio en Temps Real (ESP32-S3)

**Assignatura:** Processadors Digitals  
**Autors:** Joel Serrano, Ana Jimenez  

---

## Descripció del Projecte

Aquest projecte consisteix en un **analitzador d'àudio digital en temps real** basat en el microcontrolador **ESP32-S3**. El sistema captura el so ambient, en processa el senyal digitalment per extreure'n informació espectral i mètrica, i ho mostra de manera dual: de forma local a través d'una pantalla OLED i un LED RGB, i de forma remota mitjançant un escriptori web interactiu basat en finestres.

El principi de disseny clau és l'**eficiència en el repartiment de càrrega**: l'ESP32-S3 realitza tot el processament d'àudio pesat (DSP), mentre que el navegador web es limita a rebre les dades masegades en format JSON i a renderitzar la visualització gràfica.

---

## Arquitectura del Sistema

El flux del senyal segueix el següent recorregut del domini físic al digital:

1. **Captura:** El micròfon MEMS **INMP441** digitalitza l'àudio i l'envia per bus **I2S** a l'ESP32-S3.
2. **Processament (ESP32-S3):** * S'aplica una finestra de **Hamming** per reduir la fuita espectral (*spectral leakage*).
   * Es calcula la **FFT** (Transformada Ràpida de Fourier) sobre un bloc de 1024 mostres a 16 kHz.
   * Es calcula la freqüència dominant amb **interpolació parabòlica** per augmentar la resolució.
   * Es calculen els **dBFS** i s'executa un algorisme dinàmic de detecció de **BPM** amb filtre de mediana.
3. **Comunicació:** L'ESP32 actua com a servidor HTTP de tipus *polling* i serveix un JSON cada 120 ms.
4. **Interfície Web:** El navegador rep el JSON, dibuixa l'espectre en un canvas i calcula la nota musical equivalent mitjançant la fórmula MIDI.

---

## Tecnologies i Busos Utilitzats

| Perifèric / Component | Bus / Protocol | Funció / Detall |
| :--- | :--- | :--- |
| **Micròfon INMP441** | I2S | Captura d'àudio digital (SCK, WS, SD) immune al soroll elèctric. |
| **Pantalla OLED SSD1306** | I2C | Visualització local de freqüència (Hz), nivell (dB) i BPM (SDA, SCL). |
| **LED RGB WS2812** | RMT | Indicador visual de potència (verd a vermell) amb polsos de temporització precisa. |
| **Interfície Web** | WiFi (STA) + HTTP | Servidor web que allotja un panell en JS pur i rep dades via JSON. |

---

## Especificacions del Càlcul DSP

* **Freqüència de mostreig:** 16.000 Hz (Freqüència de Nyquist de 8.000 Hz, ideal per a rang musical).
* **Mida del bloc (SAMPLES):** 1024 mostres (~64 ms d'àudio per bloc).
* **Resolució nativa:** 15,625 Hz per casella (*bin*) ($16000 / 1024$).
* **Filtre Passa-Alt:** Ignora els greus per sota de ~94 Hz per evitar el soroll de fons elèctric o mecànic.
* **Compressió per a la web:** Els 512 bins útils de la FFT s'agrupen en 48 bandes per reduir l'ample de banda de la xarxa.

---

## Característiques de la Interfície Web

La interfície d'usuari s'ha dissenyat com un **sistema operatiu web minimalista** amb les següents funcionalitats:
* **Entorn de finestres flotants:** Les finestres de Freqüència, Nota Musical, Mesures i Espectre es poden arrossegar, redimensionar, maximitzar o tancar amb un sistema de snap intel·ligent.
* **Mode Pausa:** El sistema compta amb un botó físic (GPIO 4) amb control de debounce que congela els gràfics tant a la pantalla OLED com a la web.
* **Afinador de precisió:** Mostra la desviació en cents respecte a la nota de referència afinada (La4 = 440 Hz).

---

## Estructura del Codi Font

El projecte està desenvolupat sota l'ecosistema PlatformIO:
* `src/main.cpp`: Codi principal de l'ESP32-S3 (configuració d'I2S, algorisme FFT, càlcul de BPM, control del LED/OLED i servidor web HTTP).
* `INDEX_HTML`: Macro que allotja tot el codi web (HTML, CSS i JS asíncron) emmagatzemat a la memòria flash (`PROGMEM`) per optimitzar l'ús de la memòria RAM.
