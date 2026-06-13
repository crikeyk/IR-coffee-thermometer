#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_MLX90614.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

// ── Pin / display config ─────────────────────────────────────────
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define OLED_ADDRESS  0x3C
#define PIN_SCK       10
#define PIN_SDA        3

// ── WiFi config ──────────────────────────────────────────────────
#define AP_SSID       "IR-Thermometer"

// ── Quick-boot detection ─────────────────────────────────────────
// If the device was on for less than QUICKBOOT_WINDOW_MS on the previous
// boot, we treat this boot as the "second boot" and enable WiFi.
#define QUICKBOOT_WINDOW_MS  3000   // 3 seconds

// ── OLED graph config ────────────────────────────────────────────
// Graph occupies the bottom portion of the display.
// Top area: current temp reading (large text).
#define GRAPH_TOP     36            // y pixel where graph starts
#define GRAPH_HEIGHT  (SCREEN_HEIGHT - GRAPH_TOP)   // 28 px
#define GRAPH_WIDTH   SCREEN_WIDTH                  // 128 samples
#define GRAPH_AUTOSCALE_MARGIN_C  1.0   // pad min/max by this many °C

// ── Objects ──────────────────────────────────────────────────────
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Adafruit_MLX90614 mlx;
WebServer server(80);
DNSServer dnsServer;
Preferences prefs;

// ── Shared state ─────────────────────────────────────────────────
struct TempData {
  double ambient;
  double object;
  bool   valid;
};
TempData tempData = { 0, 0, false };
SemaphoreHandle_t tempMutex;

bool wifiEnabled = false;   // set in setup(), read-only after that

// ── Web page ─────────────────────────────────────────────────────
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>IR Thermometer</title>
  <style>
    *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }

    :root {
      --bg:        #0d0d0f;
      --surface:   #18181c;
      --border:    #2a2a32;
      --accent:    #f97316;
      --accent-lo: rgba(249,115,22,0.12);
      --cool:      #38bdf8;
      --text:      #f1f0ee;
      --muted:     #6b6b78;
      --label:     #9a9aa8;
    }

    body {
      font-family: 'SF Mono', 'Fira Code', 'Consolas', monospace;
      background: var(--bg);
      color: var(--text);
      min-height: 100vh;
      padding: 24px 16px 40px;
      display: flex;
      flex-direction: column;
      align-items: center;
      gap: 16px;
    }

    header {
      width: 100%;
      max-width: 560px;
      display: flex;
      align-items: baseline;
      gap: 10px;
      padding-bottom: 12px;
      border-bottom: 1px solid var(--border);
    }
    header h1 {
      font-size: 0.7rem;
      letter-spacing: 0.18em;
      text-transform: uppercase;
      color: var(--muted);
      font-weight: 400;
    }
    .live-dot {
      width: 6px; height: 6px;
      border-radius: 50%;
      background: #4ade80;
      flex-shrink: 0;
      animation: blink 2s ease-in-out infinite;
    }
    @keyframes blink { 0%,100%{opacity:1} 50%{opacity:.2} }

    /* ── Big readings row ── */
    .readings {
      width: 100%;
      max-width: 560px;
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 12px;
    }
    .card {
      background: var(--surface);
      border: 1px solid var(--border);
      border-radius: 12px;
      padding: 18px 20px 14px;
    }
    .card.primary {
      border-color: var(--accent);
      background: linear-gradient(145deg, #1c1510 0%, var(--surface) 100%);
    }
    .card-label {
      font-size: 0.6rem;
      letter-spacing: 0.2em;
      text-transform: uppercase;
      color: var(--label);
      margin-bottom: 6px;
    }
    .card-value {
      font-size: 2.8rem;
      font-weight: 300;
      letter-spacing: -0.03em;
      line-height: 1;
      color: var(--text);
    }
    .card.primary .card-value { color: var(--accent); }
    .card-unit {
      font-size: 0.9rem;
      color: var(--muted);
      margin-left: 2px;
    }
    .delta-row {
      width: 100%;
      max-width: 560px;
      background: var(--surface);
      border: 1px solid var(--border);
      border-radius: 12px;
      padding: 12px 20px;
      display: flex;
      align-items: center;
      justify-content: space-between;
    }
    .delta-row .card-label { margin: 0; }
    .delta-val {
      font-size: 1.4rem;
      font-weight: 300;
    }
    .pos { color: var(--accent); }
    .neg { color: var(--cool);  }

    /* ── Chart ── */
    .chart-wrap {
      width: 100%;
      max-width: 560px;
      background: var(--surface);
      border: 1px solid var(--border);
      border-radius: 12px;
      padding: 16px 20px 12px;
    }
    .chart-title {
      font-size: 0.6rem;
      letter-spacing: 0.18em;
      text-transform: uppercase;
      color: var(--muted);
      margin-bottom: 12px;
    }
    canvas {
      width: 100%;
      height: 180px;
      display: block;
    }

    .footer {
      font-size: 0.6rem;
      color: var(--muted);
      letter-spacing: 0.08em;
    }
  </style>
</head>
<body>

  <header>
    <div class="live-dot"></div>
    <h1>IR Thermometer</h1>
  </header>

  <div class="readings">
    <div class="card primary">
      <div class="card-label">Object</div>
      <div class="card-value" id="obj">--<span class="card-unit">°C</span></div>
    </div>
    <div class="card">
      <div class="card-label">Ambient</div>
      <div class="card-value" id="amb">--<span class="card-unit">°C</span></div>
    </div>
  </div>

  <div class="delta-row">
    <div class="card-label">Delta</div>
    <div class="delta-val" id="delta">-- °C</div>
  </div>

  <div class="chart-wrap">
    <div class="chart-title">History · last 60 s</div>
    <canvas id="chart"></canvas>
  </div>

  <div class="footer" id="status">connecting…</div>

<script>
  // ── History buffers (60 points, one per second) ───────────────
  const MAX_PTS = 60;
  const objHist = [];
  const ambHist = [];

  // ── Canvas chart ──────────────────────────────────────────────
  const canvas = document.getElementById('chart');
  const ctx    = canvas.getContext('2d');

  function resizeCanvas() {
    canvas.width  = canvas.offsetWidth  * devicePixelRatio;
    canvas.height = canvas.offsetHeight * devicePixelRatio;
  }
  window.addEventListener('resize', resizeCanvas);
  resizeCanvas();

  function drawChart() {
    const W = canvas.width;
    const H = canvas.height;
    ctx.clearRect(0, 0, W, H);

    if (objHist.length < 2) return;

    const combined = [...objHist, ...ambHist];
    let lo = Math.min(...combined);
    let hi = Math.max(...combined);
    if (hi - lo < 2) { lo -= 1; hi += 1; }   // minimum span
    const pad = (hi - lo) * 0.1;
    lo -= pad; hi += pad;

    const scaleY = v => H - ((v - lo) / (hi - lo)) * H;
    const scaleX = i => (i / (MAX_PTS - 1)) * W;

    // Grid lines (3)
    ctx.strokeStyle = 'rgba(255,255,255,0.06)';
    ctx.lineWidth = 1 * devicePixelRatio;
    [0.25, 0.5, 0.75].forEach(f => {
      const y = H * f;
      ctx.beginPath();
      ctx.moveTo(0, y);
      ctx.lineTo(W, y);
      ctx.stroke();

      // y-axis label
      const temp = lo + (hi - lo) * (1 - f);
      ctx.fillStyle = 'rgba(155,155,168,0.7)';
      ctx.font = `${10 * devicePixelRatio}px monospace`;
      ctx.fillText(temp.toFixed(1), 4 * devicePixelRatio, y - 3 * devicePixelRatio);
    });

    function plotLine(data, color, fillColor) {
      if (data.length < 2) return;
      const pts = data.map((v, i) => ({
        x: scaleX(i + (MAX_PTS - data.length)),
        y: scaleY(v)
      }));

      // Fill
      ctx.beginPath();
      ctx.moveTo(pts[0].x, H);
      pts.forEach(p => ctx.lineTo(p.x, p.y));
      ctx.lineTo(pts[pts.length - 1].x, H);
      ctx.closePath();
      ctx.fillStyle = fillColor;
      ctx.fill();

      // Line
      ctx.beginPath();
      pts.forEach((p, i) => i === 0 ? ctx.moveTo(p.x, p.y) : ctx.lineTo(p.x, p.y));
      ctx.strokeStyle = color;
      ctx.lineWidth = 2 * devicePixelRatio;
      ctx.lineJoin = 'round';
      ctx.stroke();
    }

    // Ambient behind, object in front
    plotLine(ambHist, '#38bdf8', 'rgba(56,189,248,0.07)');
    plotLine(objHist, '#f97316', 'rgba(249,115,22,0.12)');

    // Legend
    const lx = W - 80 * devicePixelRatio;
    const ly = 10 * devicePixelRatio;
    const fs = 9 * devicePixelRatio;
    ctx.font = `${fs}px monospace`;
    [[`#f97316`, 'Object'], [`#38bdf8`, 'Ambient']].forEach(([c, label], i) => {
      const y = ly + i * 14 * devicePixelRatio;
      ctx.fillStyle = c;
      ctx.fillRect(lx, y, 10 * devicePixelRatio, 2 * devicePixelRatio);
      ctx.fillStyle = 'rgba(155,155,168,0.9)';
      ctx.fillText(label, lx + 14 * devicePixelRatio, y + fs * 0.8);
    });
  }

  // ── DOM refs ──────────────────────────────────────────────────
  const elObj    = document.getElementById('obj');
  const elAmb    = document.getElementById('amb');
  const elDelta  = document.getElementById('delta');
  const elStatus = document.getElementById('status');

  function poll() {
    fetch('/data')
      .then(r => r.json())
      .then(d => {
        elObj.innerHTML  = `${d.object.toFixed(2)}<span class="card-unit">°C</span>`;
        elAmb.innerHTML  = `${d.ambient.toFixed(2)}<span class="card-unit">°C</span>`;

        const diff = d.object - d.ambient;
        const sign = diff >= 0 ? '+' : '';
        const cls  = diff >= 0 ? 'pos' : 'neg';
        elDelta.innerHTML = `<span class="${cls}">${sign}${diff.toFixed(2)} °C</span>`;

        objHist.push(d.object);
        ambHist.push(d.ambient);
        if (objHist.length > MAX_PTS) objHist.shift();
        if (ambHist.length > MAX_PTS) ambHist.shift();

        drawChart();
        elStatus.textContent = `updated ${new Date().toLocaleTimeString()}`;
      })
      .catch(() => { elStatus.textContent = 'connection lost — retrying…'; });
  }

  poll();
  setInterval(poll, 1000);
</script>
</body>
</html>
)rawliteral";

// ── Handlers ─────────────────────────────────────────────────────
void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleData() {
  TempData snap;
  if (xSemaphoreTake(tempMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    snap = tempData;
    xSemaphoreGive(tempMutex);
  }
  char buf[72];
  snprintf(buf, sizeof(buf),
           "{\"ambient\":%.2f,\"object\":%.2f,\"valid\":%s}",
           snap.ambient, snap.object, snap.valid ? "true" : "false");
  server.send(200, "application/json", buf);
}

void handleCaptive() {
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(302, "text/plain", "");
}

// ── OLED rolling graph state ─────────────────────────────────────
static float  graphBuf[GRAPH_WIDTH];   // circular buffer of object temps
static int    graphHead  = 0;          // next write index
static int    graphCount = 0;          // how many valid samples
static float  graphMin   = 1e9f;
static float  graphMax   = -1e9f;

void graphPush(float val) {
  graphBuf[graphHead] = val;
  graphHead = (graphHead + 1) % GRAPH_WIDTH;
  if (graphCount < GRAPH_WIDTH) graphCount++;

  // Recompute min/max over entire buffer
  graphMin = 1e9f; graphMax = -1e9f;
  int start = (graphHead - graphCount + GRAPH_WIDTH) % GRAPH_WIDTH;
  for (int i = 0; i < graphCount; i++) {
    float v = graphBuf[(start + i) % GRAPH_WIDTH];
    if (v < graphMin) graphMin = v;
    if (v > graphMax) graphMax = v;
  }
}

// Draw the rolling graph into the lower portion of the display buffer.
// Call AFTER display.clearDisplay() and before display.display().
void drawOledGraph() {
  if (graphCount < 2) return;

  float lo = graphMin - GRAPH_AUTOSCALE_MARGIN_C;
  float hi = graphMax + GRAPH_AUTOSCALE_MARGIN_C;
  if (hi - lo < 1.0f) { lo -= 0.5f; hi += 0.5f; }

  int start = (graphHead - graphCount + GRAPH_WIDTH) % GRAPH_WIDTH;

  // Map count samples across SCREEN_WIDTH pixels
  int prevX = -1, prevY = -1;
  for (int px = 0; px < SCREEN_WIDTH; px++) {
    // which sample index corresponds to pixel px?
    int si = (int)((float)px / (SCREEN_WIDTH - 1) * (graphCount - 1) + 0.5f);
    float v = graphBuf[(start + si) % GRAPH_WIDTH];
    int py = GRAPH_TOP + GRAPH_HEIGHT - 1
             - (int)(((v - lo) / (hi - lo)) * (GRAPH_HEIGHT - 1));
    py = constrain(py, GRAPH_TOP, SCREEN_HEIGHT - 1);

    if (prevX >= 0) {
      display.drawLine(prevX, prevY, px, py, SSD1306_WHITE);
    }
    prevX = px; prevY = py;
  }

  // Y-axis labels (min / max) at right edge, tiny font
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  // max label top-right
  char buf[10];
  snprintf(buf, sizeof(buf), "%.1f", hi - GRAPH_AUTOSCALE_MARGIN_C);
  display.setCursor(SCREEN_WIDTH - 6 * strlen(buf), GRAPH_TOP);
  display.print(buf);
  // min label bottom-right
  snprintf(buf, sizeof(buf), "%.1f", lo + GRAPH_AUTOSCALE_MARGIN_C);
  display.setCursor(SCREEN_WIDTH - 6 * strlen(buf), SCREEN_HEIGHT - 8);
  display.print(buf);
}

// ── Task 1: Sensor ───────────────────────────────────────────────
void taskSensor(void* pvParameters) {
  for (;;) {
    double amb = mlx.readAmbientTempC();
    double obj = mlx.readObjectTempC();
    bool valid = (amb > -40 && amb < 125 && obj > -70 && obj < 380);

    if (valid) {
      if (xSemaphoreTake(tempMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        tempData.ambient = amb;
        tempData.object  = obj;
        tempData.valid   = true;
        xSemaphoreGive(tempMutex);
      }
      graphPush((float)obj);   // feed rolling graph (no mutex needed — only this task writes)
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// ── Task 2: Display ──────────────────────────────────────────────
void taskDisplay(void* pvParameters) {
  for (;;) {
    TempData snap;
    if (xSemaphoreTake(tempMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      snap = tempData;
      xSemaphoreGive(tempMutex);
    }

    display.clearDisplay();

    if (!snap.valid) {
      display.setTextSize(1);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(20, 28);
      display.println("Sensor error!");
      display.display();
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    // ── Top section: big object temp ─────────────────────────────
    // e.g. "36.75" in size-2 font (12px tall), centred vertically in top area
    display.setTextColor(SSD1306_WHITE);

    // Label "OBJ" tiny
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print("OBJ");

    // Value in size-2 font
    char valBuf[12];
    snprintf(valBuf, sizeof(valBuf), "%.2f", snap.object);
    display.setTextSize(2);
    // size-2 char is 12px tall; centre it in the GRAPH_TOP area (36px)
    int16_t ty = (GRAPH_TOP - 16) / 2;
    display.setCursor(22, ty);
    display.print(valBuf);

    // "C" unit small
    display.setTextSize(1);
    display.setCursor(22 + strlen(valBuf) * 12, ty + 4);
    display.print("C");

    // Divider
    display.drawFastHLine(0, GRAPH_TOP - 2, SCREEN_WIDTH, SSD1306_WHITE);

    // ── Lower section: rolling graph ─────────────────────────────
    drawOledGraph();

    display.display();
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// ── Task 3: Web server + DNS ─────────────────────────────────────
void taskWebServer(void* pvParameters) {
  for (;;) {
    dnsServer.processNextRequest();
    server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

// ── Task 4: Watchdog ─────────────────────────────────────────────
void taskWatchdog(void* pvParameters) {
  for (;;) {
    TempData snap;
    if (xSemaphoreTake(tempMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      snap = tempData;
      xSemaphoreGive(tempMutex);
    }
    Serial.printf("heap: %lu  amb: %.2f  obj: %.2f  valid: %d  wifi: %d\n",
                  esp_get_free_heap_size(),
                  snap.ambient, snap.object, snap.valid, wifiEnabled);
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

// ── Quick-boot detection via NVS ─────────────────────────────────
// We write a flag at boot and clear it after QUICKBOOT_WINDOW_MS.
// If the flag is already set when we boot, the previous boot was short → enable WiFi.
bool detectQuickBoot() {
  prefs.begin("boot", false);
  bool flagSet = prefs.getBool("qb", false);
  // Immediately (re-)set the flag for this boot
  prefs.putBool("qb", true);
  prefs.end();
  return flagSet;   // true = previous boot was shorter than QUICKBOOT_WINDOW_MS
}

void clearQuickBootFlag() {
  prefs.begin("boot", false);
  prefs.putBool("qb", false);
  prefs.end();
}

// ── Setup ────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nESP32-C3 IR Thermometer boot");

  // ── Quick-boot detection ──────────────────────────────────────
  wifiEnabled = detectQuickBoot();
  Serial.printf("Quick-boot flag was %s → WiFi %s\n",
                wifiEnabled ? "SET" : "clear",
                wifiEnabled ? "ENABLED" : "disabled");

  // ── Hardware init ─────────────────────────────────────────────
  Wire.begin(PIN_SDA, PIN_SCK);
  Wire.setClock(400000);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("HALTED: SSD1306 not found");
    for (;;) delay(2000);
  }
  Serial.println("Display OK");

  if (!mlx.begin()) {
    Serial.println("HALTED: MLX90614 not found");
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(10, 28);
    display.println("MLX90614 not found");
    display.display();
    for (;;) delay(2000);
  }
  Serial.println("MLX90614 OK");

  // First sensor read
  delay(200);
  tempData.ambient = mlx.readAmbientTempC();
  tempData.object  = mlx.readObjectTempC();
  tempData.valid   = (tempData.ambient > -40 && tempData.ambient < 125 &&
                      tempData.object  > -70 && tempData.object  < 380);

  // ── WiFi + web server (only if quick-boot detected) ───────────
  if (wifiEnabled) {
    WiFi.softAP(AP_SSID);   // no password — open network
    IPAddress ip = WiFi.softAPIP();
    Serial.printf("AP: %s  IP: %s\n", AP_SSID, ip.toString().c_str());

    dnsServer.start(53, "*", ip);

    server.on("/",                           handleRoot);
    server.on("/data",                       handleData);
    server.on("/hotspot-detect.html",        handleCaptive);
    server.on("/library/test/success.html",  handleCaptive);
    server.on("/generate_204",               handleCaptive);
    server.on("/gen_204",                    handleCaptive);
    server.on("/connecttest.txt",            handleCaptive);
    server.on("/redirect",                   handleCaptive);
    server.on("/success.txt",               handleCaptive);
    server.on("/ncsi.txt",                   handleCaptive);
    server.onNotFound(handleCaptive);
    server.begin();
    Serial.println("Web server started");

    // Show WiFi info on OLED
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("WiFi enabled!");
    display.println("");
    display.println(AP_SSID);
    display.println("(no password)");
    display.println("");
    display.println(ip.toString());
    display.display();
    delay(4000);
  } else {
    // Make sure WiFi radio is fully off to save power
    WiFi.mode(WIFI_OFF);
    Serial.println("WiFi off");
  }

  // ── Mutex + tasks ─────────────────────────────────────────────
  tempMutex = xSemaphoreCreateMutex();

  xTaskCreate(taskSensor,  "Sensor",  4096, NULL, 2, NULL);
  xTaskCreate(taskDisplay, "Display", 8192, NULL, 1, NULL);
  xTaskCreate(taskWatchdog,"Watchdog",4096, NULL, 1, NULL);

  if (wifiEnabled) {
    xTaskCreate(taskWebServer, "WebServer", 4096, NULL, 1, NULL);
  }

  // ── Clear the quick-boot flag after the safe window ───────────
  // Anything still running after QUICKBOOT_WINDOW_MS is a "normal" boot.
  // We do this in setup() on the main task — it's still within the Arduino
  // task, which persists. A small blocking delay here is fine because the
  // FreeRTOS tasks above are already running on their own stacks.
  delay(QUICKBOOT_WINDOW_MS);
  clearQuickBootFlag();
  Serial.println("Quick-boot flag cleared (normal boot confirmed)");
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
