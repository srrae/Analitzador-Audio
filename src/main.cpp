// ============================================================================
//  ANALITZADOR D'AUDIO EN TEMPS REAL - ESP32-S3
//  Processadors Digitals - UPC ESEIAAT
//  Joel Serrano & Ana Jimenez
// ----------------------------------------------------------------------------
//  Funcions: captura I2S + FFT + frequencia dominant + nivell dBFS + BPM
//            display OLED + LED RGB + boto pausa + interficie web
// ============================================================================

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <driver/i2s.h>
#include <arduinoFFT.h>
#include <WiFi.h>
#include <WebServer.h>
#include <string.h>

// ===================== CONFIGURACIO WIFI =====================
const char* WIFI_SSID = "MOVISTAR_B2B0";
const char* WIFI_PASS = "owmG926WEvJuXqzLPnXn";

// ===================== PINS =====================
#define OLED_SDA  8
#define OLED_SCL  9
#define OLED_ADDR 0x3C
#define I2S_WS    5
#define I2S_SCK   6
#define I2S_SD    7
#define I2S_PORT  I2S_NUM_0
#define BTN_PIN   4
// LED RGB integrat: RGB_BUILTIN (GPIO 48), no cal cablejar

// ===================== AUDIO / FFT =====================
#define SAMPLE_RATE   16000
#define SAMPLES       1024
#define NUM_BANDS     48
#define BINS_PER_BAND 8
#define SILENCE_DB    -55.0   // llindar per sota del qual no hi ha pic valid
#define MIN_BIN       6       // ignora greus/rumble per sota de ~94 Hz (filtre passa-alt)

// ===================== OBJECTES GLOBALS =====================
Adafruit_SSD1306 display(128, 64, &Wire, -1);
double vReal[SAMPLES];
double vImag[SAMPLES];
ArduinoFFT<double> FFT(vReal, vImag, SAMPLES, SAMPLE_RATE);
int32_t i2sBuffer[SAMPLES];

const double FFT_FULLSCALE = (SAMPLES / 2.0) * 8388608.0;  // (SAMPLES/2) * 2^23

// Valors compartits (mostrats / enviats a la web)
double g_freq = 0;
double g_dB = -120;
int    g_bpm = 0;
bool   g_frozen = false;
int    g_spectrum[NUM_BANDS] = {0};

WebServer server(80);
String wifiIP = "--";

// ===================== BOTO =====================
bool lastBtn = HIGH;
unsigned long lastBtnTime = 0;

void checkButton() {
  bool s = digitalRead(BTN_PIN);
  if (s == LOW && lastBtn == HIGH && (millis() - lastBtnTime > 250)) {
    g_frozen = !g_frozen;
    lastBtnTime = millis();
  }
  lastBtn = s;
}

// ===================== DETECCIO BPM =====================
#define LEVEL_HISTORY    32
#define INTERVAL_HISTORY 6
double levelHistory[LEVEL_HISTORY] = {0};
int levelIdx = 0;
unsigned long beatIntervals[INTERVAL_HISTORY] = {0};
int intervalIdx = 0;
unsigned long lastBeatTime = 0;
unsigned long lastValidBeat = 0;
bool wasAboveThreshold = false;
int currentBPM = 0;

void detectBeat(double currentDB) {
  levelHistory[levelIdx] = currentDB;
  levelIdx = (levelIdx + 1) % LEVEL_HISTORY;
  double sum = 0;
  for (int i = 0; i < LEVEL_HISTORY; i++) sum += levelHistory[i];
  double avg = sum / LEVEL_HISTORY;
  double threshold = avg + 5.0;
  unsigned long now = millis();
  bool isAbove = (currentDB > threshold) && (currentDB > -50);
  if (isAbove && !wasAboveThreshold) {
    if (lastBeatTime > 0) {
      unsigned long interval = now - lastBeatTime;
      if (interval > 270 && interval < 1500) {
        beatIntervals[intervalIdx] = interval;
        intervalIdx = (intervalIdx + 1) % INTERVAL_HISTORY;
        lastValidBeat = now;
        unsigned long sorted[INTERVAL_HISTORY];
        memcpy(sorted, beatIntervals, sizeof(beatIntervals));
        for (int i = 0; i < INTERVAL_HISTORY - 1; i++)
          for (int j = 0; j < INTERVAL_HISTORY - 1 - i; j++)
            if (sorted[j] > sorted[j + 1]) {
              unsigned long t = sorted[j]; sorted[j] = sorted[j + 1]; sorted[j + 1] = t;
            }
        unsigned long median = sorted[INTERVAL_HISTORY / 2];
        if (median > 0) currentBPM = 60000 / median;
      }
    }
    lastBeatTime = now;
  }
  wasAboveThreshold = isAbove;
  if (now - lastValidBeat > 3000) currentBPM = 0;
}

// ===================== ESPECTRE PER BANDES =====================
void computeSpectrum() {
  for (int b = 0; b < NUM_BANDS; b++) {
    double maxBin = 0;
    for (int i = 0; i < BINS_PER_BAND; i++) {
      int bin = b * BINS_PER_BAND + i + 1;
      if (bin < SAMPLES / 2 && vReal[bin] > maxBin) maxBin = vReal[bin];
    }
    double bandDB = (maxBin > 0) ? 20.0 * log10(maxBin / FFT_FULLSCALE) : -100;
    int h = (int)((bandDB + 75) / 70.0 * 100);
    if (h < 0) h = 0;
    if (h > 100) h = 100;
    g_spectrum[b] = h;
  }
}

// ===================== LED RGB SEGONS NIVELL =====================
void updateRGBLed(double dB) {
  float t = (dB - (-40)) / 30.0;   // -40 dB = verd, -10 dB = vermell
  if (t < 0) t = 0;
  if (t > 1) t = 1;
  neopixelWrite(RGB_BUILTIN, (uint8_t)(80 * t), (uint8_t)(80 * (1 - t)), 0);
}

// ===================== I2S =====================
void setupI2S() {
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4, .dma_buf_len = 256,
    .use_apll = false, .tx_desc_auto_clear = false, .fixed_mclk = 0
  };
  i2s_pin_config_t pins = {
    .bck_io_num = I2S_SCK, .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE, .data_in_num = I2S_SD
  };
  i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
  i2s_set_pin(I2S_PORT, &pins);
  i2s_zero_dma_buffer(I2S_PORT);
}

// ===================== PAGINA WEB =====================
const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="ca"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Analitzador d'àudio</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link href="https://fonts.googleapis.com/css2?family=Inter:wght@200;300;400;600&family=JetBrains+Mono:wght@400;500&display=swap" rel="stylesheet">
<style>
*{margin:0;padding:0;box-sizing:border-box}
html,body{height:100%;overflow:hidden}
body{background:#000;color:#fff;font-family:'Inter',-apple-system,system-ui,sans-serif;
-webkit-font-smoothing:antialiased;position:relative;user-select:none}
body::before{content:'';position:fixed;inset:0;z-index:0;opacity:0.55;
background-color:#000;
background-image:
 radial-gradient(circle at 18% 22%, rgba(93,202,165,.07), transparent 38%),
 radial-gradient(circle at 82% 75%, rgba(93,202,165,.05), transparent 38%),
 url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='300' height='300' viewBox='0 0 300 300'%3E%3Cg fill='none' stroke='%231e2b27' stroke-width='1.2'%3E%3Cpath d='M20 0 V60 H80 V120 M20 60 H-10 M80 120 H140 V200'/%3E%3Cpath d='M150 0 V40 H210 V100 M210 100 H260'/%3E%3Cpath d='M0 150 H50 V210 H110 M50 210 V260'/%3E%3Cpath d='M300 80 H240 V140 H180 M240 140 V190'/%3E%3Cpath d='M120 300 V250 H60 V190'/%3E%3Cpath d='M200 300 V260 H270 V210'/%3E%3Cpath d='M280 30 H230 V70'/%3E%3C/g%3E%3Cg fill='%231e2b27'%3E%3Ccircle cx='20' cy='60' r='3'/%3E%3Ccircle cx='80' cy='120' r='3'/%3E%3Ccircle cx='210' cy='100' r='3'/%3E%3Ccircle cx='50' cy='210' r='3'/%3E%3Ccircle cx='240' cy='140' r='3'/%3E%3Ccircle cx='60' cy='190' r='3'/%3E%3Ccircle cx='270' cy='210' r='3'/%3E%3Ccircle cx='150' cy='40' r='3'/%3E%3C/g%3E%3C/svg%3E");
background-size:cover, cover, 300px 300px}
/* Barra superior */
.topbar{position:fixed;top:0;left:0;right:0;height:44px;z-index:9000;display:flex;
align-items:center;justify-content:space-between;padding:0 18px;
background:rgba(8,12,11,.6);backdrop-filter:blur(12px);border-bottom:1px solid rgba(93,202,165,.12)}
.brand{font-size:11px;letter-spacing:3px;text-transform:uppercase;color:#9aa;font-weight:600}
.status{font-size:10px;letter-spacing:2px;color:#4ade80;display:flex;align-items:center;gap:7px;font-weight:500}
.status.frozen{color:#fbbf24}
.dot{width:7px;height:7px;border-radius:50%;background:currentColor}
.dot.live{animation:pulse 1.4s infinite}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.2}}
/* Escriptori */
.desktop{position:fixed;top:44px;left:0;right:0;bottom:58px;z-index:1;overflow:hidden}
/* Finestra */
.win{position:absolute;min-width:200px;min-height:140px;display:flex;flex-direction:column;
background:rgba(12,18,16,.72);backdrop-filter:blur(16px);
border:1px solid rgba(93,202,165,.22);border-radius:16px;overflow:hidden;
box-shadow:0 18px 50px rgba(0,0,0,.55), 0 0 0 .5px rgba(255,255,255,.03) inset}
.win.hidden{display:none}
.win-bar{height:38px;flex-shrink:0;display:flex;align-items:center;justify-content:space-between;
padding:0 6px 0 14px;cursor:grab;background:rgba(255,255,255,.03);border-bottom:1px solid rgba(255,255,255,.05)}
.win-bar:active{cursor:grabbing}
.win-title{font-size:10px;letter-spacing:2px;text-transform:uppercase;color:#5DCAA5;font-family:'JetBrains Mono',monospace}
.win-ctrl{display:flex;gap:7px;align-items:center}
.cbtn{width:13px;height:13px;border-radius:50%;border:none;cursor:pointer;padding:0;transition:transform .1s}
.cbtn:hover{transform:scale(1.18)}
.c-min{background:#fbbf24}.c-max{background:#4ade80}.c-close{background:#ff5f57}
.win-body{flex:1;overflow:hidden;padding:20px;display:flex;flex-direction:column;justify-content:center;position:relative}
/* Resize handles */
.rz{position:absolute;z-index:5}
.rz-r{right:0;top:0;width:7px;height:100%;cursor:ew-resize}
.rz-b{bottom:0;left:0;width:100%;height:7px;cursor:ns-resize}
.rz-br{right:0;bottom:0;width:16px;height:16px;cursor:nwse-resize}
.rz-l{left:0;top:0;width:7px;height:100%;cursor:ew-resize}
.rz-t{top:0;left:0;width:100%;height:7px;cursor:ns-resize}
/* Snap preview */
.snap-prev{position:fixed;z-index:8000;background:rgba(93,202,165,.16);
border:2px solid rgba(93,202,165,.6);border-radius:14px;pointer-events:none;display:none;
transition:all .12s ease}
/* Dock */
.dock{position:fixed;bottom:0;left:0;right:0;height:58px;z-index:9000;display:flex;
align-items:center;justify-content:center;gap:10px;
background:rgba(8,12,11,.6);backdrop-filter:blur(12px);border-top:1px solid rgba(93,202,165,.12)}
.dock-item{display:flex;align-items:center;gap:8px;padding:8px 16px;border-radius:11px;cursor:pointer;
font-size:12px;color:#aaa;border:1px solid transparent;transition:all .15s;background:rgba(255,255,255,.03)}
.dock-item:hover{background:rgba(93,202,165,.1);color:#fff}
.dock-item.active{border-color:rgba(93,202,165,.4);color:#5DCAA5}
.dock-dot{width:6px;height:6px;border-radius:50%;background:#5DCAA5;opacity:0;transition:opacity .15s}
.dock-item.active .dock-dot{opacity:1}
/* Continguts */
.freq{font-size:64px;font-weight:200;letter-spacing:-3px;line-height:1;text-align:center}
.freq .unit{font-size:20px;color:#666;font-weight:300;margin-left:6px;letter-spacing:0}
.freq.silent{color:#444}
.note{font-size:66px;font-weight:200;line-height:1;text-align:center}
.note .oct{font-size:28px;color:#5DCAA5;font-weight:300}
.note.silent{color:#444}
.cents{margin-top:18px}
.cents-bar{position:relative;height:7px;background:rgba(255,255,255,.08);border-radius:4px;max-width:280px;margin:0 auto}
.cents-center{position:absolute;left:50%;top:-4px;width:2px;height:15px;background:#5DCAA5;transform:translateX(-50%)}
.cents-needle{position:absolute;top:-3px;width:9px;height:13px;border-radius:3px;background:#fff;transform:translateX(-50%);transition:left .1s,background .1s}
.cents-lbl{font-size:11px;color:#888;margin-top:11px;text-align:center;font-family:'JetBrains Mono',monospace}
.meta-row{display:flex;justify-content:center;gap:48px}
.metric{text-align:center}
.metric .label{font-size:9px;letter-spacing:2px;color:#666;text-transform:uppercase;margin-bottom:7px}
.metric .val{font-size:34px;font-weight:200}
canvas{width:100%;height:100%;display:block}
.win-body.specbody{padding:14px}
</style></head><body>

<div class="topbar">
<div class="brand">Analitzador d'àudio · FFT</div>
<div class="status" id="status"><span class="dot live"></span><span id="stext">EN VIU</span></div>
</div>

<div class="desktop" id="desktop">
  <!-- Finestra Frequencia -->
  <div class="win" id="win-freq" style="left:30px;top:24px;width:300px;height:200px">
    <div class="win-bar"><span class="win-title">Frequencia</span>
      <div class="win-ctrl"><button class="cbtn c-min"></button><button class="cbtn c-max"></button><button class="cbtn c-close"></button></div></div>
    <div class="win-body">
      <div class="freq" id="freqBox"><span id="freq">--</span><span class="unit">Hz</span></div>
    </div>
    <div class="rz rz-l"></div><div class="rz rz-r"></div><div class="rz rz-t"></div><div class="rz rz-b"></div><div class="rz rz-br"></div>
  </div>
  <!-- Finestra Nota -->
  <div class="win" id="win-note" style="left:360px;top:24px;width:320px;height:280px">
    <div class="win-bar"><span class="win-title">Nota musical</span>
      <div class="win-ctrl"><button class="cbtn c-min"></button><button class="cbtn c-max"></button><button class="cbtn c-close"></button></div></div>
    <div class="win-body">
      <div class="note" id="noteBox"><span id="note">--</span><span class="oct" id="oct"></span></div>
      <div class="cents">
        <div class="cents-bar"><div class="cents-center"></div><div class="cents-needle" id="needle" style="left:50%"></div></div>
        <div class="cents-lbl" id="centsLbl">-- cents</div>
      </div>
    </div>
    <div class="rz rz-l"></div><div class="rz rz-r"></div><div class="rz rz-t"></div><div class="rz rz-b"></div><div class="rz rz-br"></div>
  </div>
  <!-- Finestra Mesures -->
  <div class="win" id="win-meta" style="left:30px;top:248px;width:300px;height:180px">
    <div class="win-bar"><span class="win-title">Mesures</span>
      <div class="win-ctrl"><button class="cbtn c-min"></button><button class="cbtn c-max"></button><button class="cbtn c-close"></button></div></div>
    <div class="win-body">
      <div class="meta-row">
        <div class="metric"><div class="label">Nivell</div><div class="val"><span id="db">--</span> dB</div></div>
        <div class="metric"><div class="label">BPM</div><div class="val" id="bpm">--</div></div>
      </div>
    </div>
    <div class="rz rz-l"></div><div class="rz rz-r"></div><div class="rz rz-t"></div><div class="rz rz-b"></div><div class="rz rz-br"></div>
  </div>
  <!-- Finestra Espectre -->
  <div class="win" id="win-spec" style="left:360px;top:320px;width:420px;height:300px">
    <div class="win-bar"><span class="win-title">Espectre FFT</span>
      <div class="win-ctrl"><button class="cbtn c-min"></button><button class="cbtn c-max"></button><button class="cbtn c-close"></button></div></div>
    <div class="win-body specbody"><canvas id="spec"></canvas></div>
    <div class="rz rz-l"></div><div class="rz rz-r"></div><div class="rz rz-t"></div><div class="rz rz-b"></div><div class="rz rz-br"></div>
  </div>
</div>

<div class="snap-prev" id="snapPrev"></div>

<div class="dock" id="dock">
  <div class="dock-item active" data-w="freq"><span class="dock-dot"></span>Frequencia</div>
  <div class="dock-item active" data-w="note"><span class="dock-dot"></span>Nota</div>
  <div class="dock-item active" data-w="meta"><span class="dock-dot"></span>Mesures</div>
  <div class="dock-item active" data-w="spec"><span class="dock-dot"></span>Espectre</div>
</div>

<script>
const desk=document.getElementById('desktop');
const snapPrev=document.getElementById('snapPrev');
let zTop=10;

function deskRect(){return desk.getBoundingClientRect();}

// ---- Gestio de finestres ----
const wins={};
['freq','note','meta','spec'].forEach(id=>{
  const el=document.getElementById('win-'+id);
  wins[id]={el,prev:null,max:false};
  el.addEventListener('pointerdown',()=>focusWin(id),true);
  // controls
  el.querySelector('.c-close').addEventListener('click',e=>{e.stopPropagation();setOpen(id,false);});
  el.querySelector('.c-min').addEventListener('click',e=>{e.stopPropagation();setOpen(id,false);});
  el.querySelector('.c-max').addEventListener('click',e=>{e.stopPropagation();toggleMax(id);});
  // doble clic barra -> maximitzar
  el.querySelector('.win-bar').addEventListener('dblclick',()=>toggleMax(id));
  // drag
  makeDraggable(id);
  makeResizable(id);
});
focusWin('spec');

function focusWin(id){wins[id].el.style.zIndex=++zTop;}

function setOpen(id,open){
  wins[id].el.classList.toggle('hidden',!open);
  document.querySelector('.dock-item[data-w="'+id+'"]').classList.toggle('active',open);
  if(open){focusWin(id);if(id==='spec')setTimeout(rsz,50);}
}
document.querySelectorAll('.dock-item').forEach(d=>{
  d.addEventListener('click',()=>{
    const id=d.dataset.w, isOpen=!wins[id].el.classList.contains('hidden');
    setOpen(id,!isOpen);
  });
});

function toggleMax(id){
  const w=wins[id], el=w.el, dr=deskRect();
  if(w.max){
    el.style.left=w.prev.l+'px';el.style.top=w.prev.t+'px';
    el.style.width=w.prev.w+'px';el.style.height=w.prev.h+'px';
    w.max=false;
  }else{
    w.prev={l:el.offsetLeft,t:el.offsetTop,w:el.offsetWidth,h:el.offsetHeight};
    el.style.left='0px';el.style.top='0px';
    el.style.width=dr.width+'px';el.style.height=dr.height+'px';
    w.max=true;
  }
  if(id==='spec')setTimeout(rsz,60);
}

// ---- Arrossegar amb snap ----
function makeDraggable(id){
  const el=wins[id].el, bar=el.querySelector('.win-bar');
  let sx,sy,ox,oy,dragging=false,snapTarget=null;
  bar.addEventListener('pointerdown',e=>{
    if(e.target.classList.contains('cbtn'))return;
    dragging=true;sx=e.clientX;sy=e.clientY;ox=el.offsetLeft;oy=el.offsetTop;
    if(wins[id].max){ // si esta maximitzada, restaura al agafar
      const w=wins[id];w.max=false;
      el.style.width=(w.prev?w.prev.w:300)+'px';el.style.height=(w.prev?w.prev.h:200)+'px';
      ox=e.clientX-deskRect().left-100;oy=10;
    }
    bar.setPointerCapture(e.pointerId);focusWin(id);
  });
  bar.addEventListener('pointermove',e=>{
    if(!dragging)return;
    el.style.left=(ox+e.clientX-sx)+'px';
    el.style.top=(oy+e.clientY-sy)+'px';
    snapTarget=detectSnap(e.clientX,e.clientY);
    showSnapPrev(snapTarget);
  });
  bar.addEventListener('pointerup',e=>{
    if(!dragging)return;dragging=false;
    snapPrev.style.display='none';
    if(snapTarget){applySnap(id,snapTarget);}
    snapTarget=null;
  });
}

function detectSnap(x,y){
  const dr=deskRect(), M=40;
  const left=x<dr.left+M, right=x>dr.right-M, top=y<dr.top+M, bottom=y>dr.bottom-M;
  if(top&&left)return'tl'; if(top&&right)return'tr';
  if(bottom&&left)return'bl'; if(bottom&&right)return'br';
  if(left)return'l'; if(right)return'r'; if(top)return'max';
  return null;
}
function snapRect(t){
  const dr=deskRect(), W=dr.width, H=dr.height;
  const m={l:[0,0,W/2,H], r:[W/2,0,W/2,H], max:[0,0,W,H],
    tl:[0,0,W/2,H/2], tr:[W/2,0,W/2,H/2], bl:[0,H/2,W/2,H/2], br:[W/2,H/2,W/2,H/2]};
  return m[t];
}
function showSnapPrev(t){
  if(!t){snapPrev.style.display='none';return;}
  const dr=deskRect(), r=snapRect(t);
  snapPrev.style.display='block';
  snapPrev.style.left=(dr.left+r[0])+'px';
  snapPrev.style.top=(dr.top+r[1])+'px';
  snapPrev.style.width=r[2]+'px';
  snapPrev.style.height=r[3]+'px';
}
function applySnap(id,t){
  const el=wins[id].el, r=snapRect(t);
  el.style.left=r[0]+'px';el.style.top=r[1]+'px';
  el.style.width=r[2]+'px';el.style.height=r[3]+'px';
  wins[id].max=(t==='max');
  if(id==='spec')setTimeout(rsz,60);
}

// ---- Redimensionar ----
function makeResizable(id){
  const el=wins[id].el;
  el.querySelectorAll('.rz').forEach(h=>{
    h.addEventListener('pointerdown',e=>{
      e.stopPropagation();
      const sx=e.clientX,sy=e.clientY,ow=el.offsetWidth,oh=el.offsetHeight,ol=el.offsetLeft,ot=el.offsetTop;
      const cls=h.className;
      h.setPointerCapture(e.pointerId);focusWin(id);
      function mv(ev){
        const dx=ev.clientX-sx,dy=ev.clientY-sy;
        if(cls.includes('rz-r')||cls.includes('rz-br'))el.style.width=Math.max(200,ow+dx)+'px';
        if(cls.includes('rz-b')||cls.includes('rz-br'))el.style.height=Math.max(140,oh+dy)+'px';
        if(cls.includes('rz-l')){el.style.width=Math.max(200,ow-dx)+'px';el.style.left=(ol+dx)+'px';}
        if(cls.includes('rz-t')){el.style.height=Math.max(140,oh-dy)+'px';el.style.top=(ot+dy)+'px';}
        if(id==='spec')rsz();
      }
      function up(ev){h.releasePointerCapture(e.pointerId);
        h.removeEventListener('pointermove',mv);h.removeEventListener('pointerup',up);}
      h.addEventListener('pointermove',mv);h.addEventListener('pointerup',up);
    });
  });
}

// ---- Nota musical ----
const NOTES=['Do','Do#','Re','Re#','Mi','Fa','Fa#','Sol','Sol#','La','La#','Si'];
function freqToNote(f){
  if(f<=0)return null;
  const midi=12*Math.log2(f/440)+69, nearest=Math.round(midi);
  const cents=Math.round((midi-nearest)*100);
  return {name:NOTES[((nearest%12)+12)%12],octave:Math.floor(nearest/12)-1,cents};
}

// ---- Grafica espectre ----
const cv=document.getElementById('spec'),cx=cv.getContext('2d');
let spec=new Array(48).fill(0),disp=new Array(48).fill(0);
const SR=16000,NB=48,BINS=8,BINHZ=SR/1024,FMAX=NB*BINS*BINHZ;
let W=0,H=0;
function rsz(){const r=window.devicePixelRatio||1;W=cv.clientWidth;H=cv.clientHeight;
cv.width=W*r;cv.height=H*r;cx.setTransform(r,0,0,r,0,0);}
addEventListener('resize',rsz);setTimeout(rsz,80);
const PADL=42,PADB=24,PADT=8,PADR=8;
function draw(){
  if(W>0){
    cx.clearRect(0,0,W,H);
    const plotW=W-PADL-PADR,plotH=H-PADT-PADB;
    cx.font="9px 'JetBrains Mono',monospace";cx.textBaseline="middle";
    for(let d=0;d>=-80;d-=20){
      const y=PADT+(-d/80)*plotH;
      cx.strokeStyle='rgba(255,255,255,0.06)';cx.beginPath();cx.moveTo(PADL,y);cx.lineTo(PADL+plotW,y);cx.stroke();
      cx.fillStyle='#666';cx.textAlign='right';cx.fillText(d+'',PADL-5,y);
    }
    cx.textBaseline="top";cx.textAlign="center";
    for(let f=0;f<=6000;f+=1500){
      const x=PADL+(f/FMAX)*plotW;
      cx.strokeStyle='rgba(255,255,255,0.04)';cx.beginPath();cx.moveTo(x,PADT);cx.lineTo(x,PADT+plotH);cx.stroke();
      cx.fillStyle='#666';cx.fillText((f/1000)+'k',x,PADT+plotH+5);
    }
    const n=spec.length,step=plotW/(n-1);
    cx.beginPath();
    for(let i=0;i<n;i++){disp[i]+=(spec[i]-disp[i])*0.25;
      const x=PADL+i*step,y=PADT+(1-disp[i]/100)*plotH;if(i===0)cx.moveTo(x,y);else cx.lineTo(x,y);}
    cx.lineTo(PADL+plotW,PADT+plotH);cx.lineTo(PADL,PADT+plotH);cx.closePath();
    const g=cx.createLinearGradient(0,PADT,0,PADT+plotH);
    g.addColorStop(0,'rgba(93,202,165,0.35)');g.addColorStop(1,'rgba(93,202,165,0.02)');
    cx.fillStyle=g;cx.fill();
    cx.beginPath();
    for(let i=0;i<n;i++){const x=PADL+i*step,y=PADT+(1-disp[i]/100)*plotH;if(i===0)cx.moveTo(x,y);else cx.lineTo(x,y);}
    cx.strokeStyle='#5DCAA5';cx.lineWidth=1.5;cx.stroke();
  }
  requestAnimationFrame(draw);
}
draw();

// ---- Dades ----
let busy=false;
async function upd(){
  if(busy)return;busy=true;
  try{
    const ctrl=new AbortController();const to=setTimeout(()=>ctrl.abort(),800);
    const d=await(await fetch('/data',{signal:ctrl.signal,cache:'no-store'})).json();
    clearTimeout(to);
    const fb=document.getElementById('freqBox');
    if(d.freq>0){document.getElementById('freq').textContent=Math.round(d.freq);fb.classList.remove('silent');}
    else{document.getElementById('freq').textContent='--';fb.classList.add('silent');}
    const nb=document.getElementById('noteBox'),nd=freqToNote(d.freq);
    if(nd){document.getElementById('note').textContent=nd.name;document.getElementById('oct').textContent=nd.octave;
      nb.classList.remove('silent');
      const needle=document.getElementById('needle');needle.style.left=Math.max(0,Math.min(100,50+nd.cents))+'%';
      needle.style.background=Math.abs(nd.cents)<=5?'#4ade80':'#fff';
      document.getElementById('centsLbl').textContent=(nd.cents>0?'+':'')+nd.cents+' cents';
    }else{document.getElementById('note').textContent='--';document.getElementById('oct').textContent='';
      nb.classList.add('silent');document.getElementById('needle').style.left='50%';
      document.getElementById('centsLbl').textContent='-- cents';}
    document.getElementById('db').textContent=d.db.toFixed(1);
    document.getElementById('bpm').textContent=d.bpm>0?d.bpm:'--';
    if(d.spec)spec=d.spec;
    const st=document.getElementById('status'),dot=st.querySelector('.dot');
    if(d.frozen){st.classList.add('frozen');document.getElementById('stext').textContent='PAUSA';dot.classList.remove('live');}
    else{st.classList.remove('frozen');document.getElementById('stext').textContent='EN VIU';dot.classList.add('live');}
  }catch(e){}
  finally{busy=false;}
}
setInterval(upd,120);upd();
</script></body></html>
)HTML";
void handleRoot() { server.send_P(200, "text/html", INDEX_HTML); }

void handleData() {
  char buf[512];
  int len = snprintf(buf, sizeof(buf),
    "{\"freq\":%.1f,\"db\":%.1f,\"bpm\":%d,\"frozen\":%s,\"spec\":[",
    g_freq, g_dB, g_bpm, g_frozen ? "true" : "false");
  for (int i = 0; i < NUM_BANDS; i++)
    len += snprintf(buf + len, sizeof(buf) - len, "%d%s", g_spectrum[i], (i < NUM_BANDS - 1) ? "," : "");
  snprintf(buf + len, sizeof(buf) - len, "]}");
  server.send(200, "application/json", buf);
}

// ===================== SETUP =====================
void setup() {
  Serial.begin(115200);
  delay(1000);
  pinMode(BTN_PIN, INPUT_PULLUP);
  neopixelWrite(RGB_BUILTIN, 0, 30, 0);

  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("ERROR OLED"); while (true) delay(100);
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 28);
  display.println("Connectant WiFi");
  display.display();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int att = 0;
  while (WiFi.status() != WL_CONNECTED && att < 40) { delay(400); Serial.print("."); att++; }
  if (WiFi.status() == WL_CONNECTED) {
    wifiIP = WiFi.localIP().toString();
    Serial.println("\nWeb: http://" + wifiIP);
    server.on("/", handleRoot);
    server.on("/data", handleData);
    server.begin();
  } else { wifiIP = "OFFLINE"; }

  setupI2S();
  delay(300);
}

// ===================== LOOP =====================
void loop() {
  size_t bytesRead = 0;
  // Timeout de 100ms (no portMAX_DELAY) perque no bloquegi el servidor web
  esp_err_t res = i2s_read(I2S_PORT, i2sBuffer, sizeof(i2sBuffer), &bytesRead, 100 / portTICK_PERIOD_MS);
  if (res != ESP_OK || bytesRead == 0) {
    server.handleClient();   // aten la web encara que no hi hagi audio
    return;
  }
  int n = bytesRead / sizeof(int32_t);
  for (int i = 0; i < n; i++) { vReal[i] = (double)(i2sBuffer[i] >> 8); vImag[i] = 0.0; }

  FFT.windowing(FFTWindow::Hamming, FFTDirection::Forward);
  FFT.compute(FFTDirection::Forward);
  FFT.complexToMagnitude();

  server.handleClient();   // aten la web entre la FFT i la resta

  // Cerca del pic IGNORANT els greus (filtre passa-alt a partir de MIN_BIN)
  // Busquem el bin amb mes magnitud per sobre del rumble de baixa frequencia
  double maxMag = 0;
  int peakBin = 0;
  for (int i = MIN_BIN; i < SAMPLES / 2; i++) {
    if (vReal[i] > maxMag) { maxMag = vReal[i]; peakBin = i; }
  }

  // Interpolacio parabolica al voltant del pic per millorar la resolucio
  double peakFreq = 0;
  if (peakBin > 0 && peakBin < SAMPLES / 2 - 1) {
    double y0 = vReal[peakBin - 1], y1 = vReal[peakBin], y2 = vReal[peakBin + 1];
    double denom = (y0 - 2 * y1 + y2);
    double delta = (denom != 0) ? 0.5 * (y0 - y2) / denom : 0;
    peakFreq = (peakBin + delta) * ((double)SAMPLE_RATE / SAMPLES);
  }

  double dB = (maxMag > 0) ? 20.0 * log10(maxMag / FFT_FULLSCALE) : -120.0;

  // Suprimeix el pic si nomes hi ha soroll de fons
  if (dB < SILENCE_DB || peakFreq < 0) peakFreq = 0;

  checkButton();

  if (!g_frozen) {
    g_freq = peakFreq;
    g_dB = dB;
    detectBeat(dB);
    g_bpm = currentBPM;
    computeSpectrum();
  }

  updateRGBLed(g_dB);

  // OLED
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  if (g_frozen) display.print("[ PAUSA ]");
  else display.print(wifiIP);
  display.setTextSize(2);
  display.setCursor(0, 14);
  if (g_freq > 0) { display.print(g_freq, 0); display.print(" Hz"); }
  else            { display.print("-- Hz"); }
  display.setTextSize(1);
  display.setCursor(0, 40);
  display.print("Niv: "); display.print(g_dB, 1); display.print(" dB");
  display.setCursor(0, 52);
  display.print("BPM: ");
  if (g_bpm > 0) display.print(g_bpm); else display.print("--");
  display.display();

  server.handleClient();
  yield();   // dona temps a la pila WiFi/TCP per processar connexions
}