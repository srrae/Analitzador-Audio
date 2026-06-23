# Analitzador d'Àudio en Temps Real (ESP32-S3)

**Assignatura:** Processadors Digitals  
**Institució:** UPC ESEIAAT (Terrassa)  
**Autors:** Joel Serrano & Ana Jimenez  

---

## Descripció del Projecte

[cite_start]Aquest projecte consisteix en un **analitzador d'àudio digital en temps real** basat en el microcontrolador **ESP32-S3**[cite: 1, 6]. [cite_start]El sistema captura el so ambient, en processa el senyal digitalment per extreure'n informació espectral i mètrica, i ho mostra de manera dual: de forma local a través d'una pantalla OLED i un LED RGB, i de forma remota mitjançant un escriptori web interactiu basat en finestres[cite: 36].

[cite_start]El principi de disseny clau és l'**eficiència en el repartiment de càrrega**: l'ESP32-S3 realitza tot el processament d'àudio pesat (DSP), mentre que el navegador web es limita a rebre les dades masegades en format JSON i a renderitzar la visualització gràfica[cite: 7, 27].

---

## Arquitectura del Sistema

[cite_start]El flux del senyal segueix el següent recorregut del domini físic al digital[cite: 3]:

1. [cite_start]**Captura:** El micròfon MEMS **INMP441** digitalitza l'àudio i l'envia per bus **I2S** a l'ESP32-S3[cite: 9, 10].
2. [cite_start]**Processament (ESP32-S3):** * S'aplica una finestra de **Hamming** per reduir la fuita espectral (*spectral leakage*)[cite: 16, 17].
   * [cite_start]Es calcula la **FFT** (Transformada Ràpida de Fourier) sobre un bloc de 1024 mostres a 16 kHz[cite: 14, 18].
   * [cite_start]Es calcula la freqüència dominant amb **interpolació parabòlica** per augmentar la resolució[cite: 20].
   * [cite_start]Es calculen els **dBFS** [cite: 21] [cite_start]i s'executa un algorisme dinàmic de detecció de **BPM** amb filtre de mediana[cite: 22].
3. [cite_start]**Comunicació:** L'ESP32 actua com a servidor HTTP de tipus *polling* i serveix un JSON cada 120 ms[cite: 33, 34].
4. [cite_start]**Interfície Web:** El navegador rep el JSON [cite: 27][cite_start], dibuixa l'espectre en un canvas [cite: 28] [cite_start]i calcula la nota musical equivalent mitjançant la fórmula MIDI[cite: 29].

---

## Tecnologies i Busos Utilitzats

| Perifèric / Component | Bus / Protocol | Funció / Detall |
| :--- | :--- | :--- |
| **Micròfon INMP441** | I2S | [cite_start]Captura d'àudio digital (SCK, WS, SD) immune al soroll elèctric[cite: 10, 11]. |
| **Pantalla OLED SSD1306** | I2C | [cite_start]Visualització local de freqüència (Hz), nivell (dB) i BPM (SDA, SCL)[cite: 36]. |
| **LED RGB WS2812** | RMT | [cite_start]Indicador visual de potència (verd a vermell) amb polsos de temporització precisa[cite: 36]. |
| **Interfície Web** | WiFi (STA) + HTTP | [cite_start]Servidor web que allotja un panell en JS pur i rep dades via JSON[cite: 32, 33, 34]. |

---

## Especificacions del Càlcul DSP

* [cite_start]**Freqüència de mostreig:** 16.000 Hz (Freqüència de Nyquist de 8.000 Hz, ideal per a rang musical)[cite: 15].
* [cite_start]**Mida del bloc (SAMPLES):** 1024 mostres (~64 ms d'àudio per bloc)[cite: 14].
* [cite_start]**Resolució nativa:** 15,625 Hz per casella (*bin*) ($16000 / 1024$)[cite: 19].
* [cite_start]**Filtre Passa-Alt:** Ignora els greus per sota de ~94 Hz per evitar el soroll de fons elèctric o mecànic[cite: 20].
* [cite_start]**Compressió per a la web:** Els 512 bins útils de la FFT s'agrupen en 48 bandes per reduir l'ample de banda de la xarxa[cite: 19, 23].

---

## Característiques de la Interfície Web

La interfície d'usuari s'ha dissenyat com un **sistema operatiu web minimalista** amb les següents funcionalitats:
* [cite_start]**Entorn de finestres flotants:** Les finestres de Freqüència, Nota Musical, Mesures i Espectre es poden arrossegar, redimensionar, maximitzar o tancar amb un sistema de snap intel·ligent[cite: 28].
* [cite_start]**Mode Pausa:** El sistema compta amb un botó físic (GPIO 4) amb control de debounce que congela els gràfics tant a la pantalla OLED com a la web[cite: 22].
* [cite_start]**Afinador de precisió:** Mostra la desviació en cents respecte a la nota de referència afinada (La4 = 440 Hz)[cite: 29, 31].

---

## Estructura del Codi Font

El projecte està desenvolupat sota l'ecosistema PlatformIO:
* `src/main.cpp`: Codi principal de l'ESP32-S3 (configuració d'I2S, algorisme FFT, càlcul de BPM, control del LED/OLED i servidor web HTTP).
* `INDEX_HTML`: Macro que allotja tot el codi web (HTML, CSS i JS asíncron) emmagatzemat a la memòria flash (`PROGMEM`) per optimitzar l'ús de la memòria RAM.
