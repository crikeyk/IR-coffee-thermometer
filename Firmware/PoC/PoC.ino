#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_MLX90614.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define OLED_ADDRESS  0x3C
#define PIN_SCK  10
#define PIN_SDA   3

#define AP_SSID     "IR-Thermometer"
#define AP_PASSWORD "12345678"

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Adafruit_MLX90614 mlx;
WebServer server(80);
DNSServer dnsServer;

struct TempData {
  double ambient;
  double object;
  bool   valid;
};

TempData tempData = { 0, 0, false };
SemaphoreHandle_t tempMutex;

// ── Web page ─────────────────────────────────────────────────────
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>IR Thermometer</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, sans-serif;
      background: #111;
      color: #fff;
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      min-height: 100vh;
      padding: 24px;
    }
    h1 { font-size: 1.2rem; color: #888; margin-bottom: 32px; letter-spacing: 0.1em; text-transform: uppercase; }
    .cards { display: flex; flex-direction: column; gap: 16px; width: 100%; max-width: 340px; }
    .card {
      background: #1e1e1e;
      border-radius: 16px;
      padding: 24px;
      display: flex;
      flex-direction: column;
      gap: 6px;
    }
    .card.highlight { border: 1px solid #ff6b35; }
    .label { font-size: 0.75rem; color: #666; text-transform: uppercase; letter-spacing: 0.1em; }
    .value { font-size: 3.5rem; font-weight: 200; letter-spacing: -0.02em; }
    .unit  { font-size: 1.2rem; color: #888; }
    .delta-card {
      background: #1a1a2e;
      border-radius: 16px;
      padding: 16px 24px;
      display: flex;
      justify-content: space-between;
      align-items: center;
      width: 100%;
      max-width: 340px;
    }
    .delta-label { font-size: 0.75rem; color: #666; text-transform: uppercase; letter-spacing: 0.1em; }
    .delta-value { font-size: 1.8rem; font-weight: 300; }
    .positive { color: #ff6b35; }
    .negative { color: #4fc3f7; }
    .status { margin-top: 24px; font-size: 0.7rem; color: #444; }
    .dot { display: inline-block; width: 6px; height: 6px; border-radius: 50%; background: #4caf50; margin-right: 6px; animation: pulse 2s infinite; }
    @keyframes pulse { 0%,100%{opacity:1} 50%{opacity:0.3} }
  </style>
</head>
<body>
  <h1>IR Thermometer</h1>
  <div class="cards">
    <div class="card">
      <div class="label">Ambient</div>
      <div><span class="value" id="amb">--.-</span><span class="unit"> °C</span></div>
    </div>
    <div class="card highlight">
      <div class="label">Object</div>
      <div><span class="value" id="obj">--.-</span><span class="unit"> °C</span></div>
    </div>
  </div>
  <div class="delta-card" style="margin-top:16px">
    <div class="delta-label">Delta</div>
    <div class="delta-value" id="delta">--.-<span style="font-size:1rem;color:#888"> °C</span></div>
  </div>
  <div class="status"><span class="dot"></span><span id="status">Connecting...</span></div>
  <script>
    const amb    = document.getElementById('amb');
    const obj    = document.getElementById('obj');
    const delta  = document.getElementById('delta');
    const status = document.getElementById('status');

    // Fall back to polling since SSE and synchronous WebServer
    // don't mix well — phone gets a fresh read every second
    function poll() {
      fetch('/data')
        .then(r => r.json())
        .then(d => {
          amb.textContent = d.ambient.toFixed(2);
          obj.textContent = d.object.toFixed(2);
          const diff = d.object - d.ambient;
          const sign = diff >= 0 ? '+' : '';
          delta.innerHTML = `<span class="${diff>=0?'positive':'negative'}">${sign}${diff.toFixed(2)}</span><span style="font-size:1rem;color:#888"> °C</span>`;
          status.textContent = `Last update: ${new Date().toLocaleTimeString()}`;
        })
        .catch(() => { status.textContent = 'Connection lost — retrying...'; });
    }

    poll();
    setInterval(poll, 1000);
  </script>
</body>
</html>
)rawliteral";

// ── Handlers ─────────────────────────────────────────────────────
void handleRoot() {
  server.send(200, "text/html", INDEX_HTML);
}

void handleData() {
  TempData snap;
  if (xSemaphoreTake(tempMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    snap = tempData;
    xSemaphoreGive(tempMutex);
  }
  char buf[64];
  snprintf(buf, sizeof(buf),
           "{\"ambient\":%.2f,\"object\":%.2f,\"valid\":%s}",
           snap.ambient, snap.object, snap.valid ? "true" : "false");
  server.send(200, "application/json", buf);
}

void handleCaptive() {
  // Redirect all captive portal probes to root
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(302, "text/plain", "");
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
    }
    vTaskDelay(pdMS_TO_TICKS(100));
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
    } else {
      display.setTextSize(1);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(25, 0);
      display.println("IR Thermometer");
      display.drawFastHLine(0, 10, SCREEN_WIDTH, SSD1306_WHITE);

      display.setCursor(0, 13);
      display.print("AMB:");
      display.setCursor(28, 13);
      display.printf("%.2f C", snap.ambient);

      display.drawFastHLine(0, 23, SCREEN_WIDTH, SSD1306_WHITE);

      display.setCursor(0, 26);
      display.print("OBJ:");
      display.setCursor(28, 26);
      display.printf("%.2f C", snap.object);

      display.drawFastHLine(0, 36, SCREEN_WIDTH, SSD1306_WHITE);

      double delta = snap.object - snap.ambient;
      display.setCursor(0, 39);
      display.print("DELTA:");
      display.setCursor(40, 39);
      display.printf("%+.2f C", delta);

      display.drawFastHLine(0, 49, SCREEN_WIDTH, SSD1306_WHITE);

      display.setCursor(0, 52);
      display.printf("up:%lus heap:%lu", millis() / 1000, esp_get_free_heap_size());
    }

    display.display();
    vTaskDelay(pdMS_TO_TICKS(100));
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
    Serial.printf("heap: %lu  amb: %.2f  obj: %.2f  valid: %d\n",
                  esp_get_free_heap_size(), snap.ambient, snap.object, snap.valid);
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// ── Setup ────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\nESP32-C3 IR Thermometer boot");

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

  // First read
  delay(200);
  tempData.ambient = mlx.readAmbientTempC();
  tempData.object  = mlx.readObjectTempC();
  tempData.valid   = (tempData.ambient > -40 && tempData.ambient < 125 &&
                      tempData.object  > -70 && tempData.object  < 380);
  Serial.printf("First read: amb=%.2f obj=%.2f valid=%d\n",
                tempData.ambient, tempData.object, tempData.valid);

  // Start AP
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  IPAddress ip = WiFi.softAPIP();
  Serial.printf("AP: %s  IP: %s\n", AP_SSID, ip.toString().c_str());

  // DNS wildcard — all queries return our IP
  dnsServer.start(53, "*", ip);

  // Routes — register all the URLs iOS/Android use for connectivity checks
  server.on("/", handleRoot);
  server.on("/data", handleData);

  // Apple captive portal probes
  server.on("/hotspot-detect.html",        handleCaptive);
  server.on("/library/test/success.html",  handleCaptive);

  // Android/Google captive portal probes
  server.on("/generate_204",               handleCaptive);
  server.on("/gen_204",                    handleCaptive);
  server.on("/connecttest.txt",            handleCaptive);
  server.on("/redirect",                   handleCaptive);
  server.on("/success.txt",                handleCaptive);
  server.on("/ncsi.txt",                   handleCaptive);  // Windows

  // Catch-all for anything else
  server.onNotFound(handleCaptive);

  server.begin();
  Serial.println("Server started");

  // Show info on OLED
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("WiFi Hotspot:");
  display.println(AP_SSID);
  display.println(AP_PASSWORD);
  display.println("");
  display.println("Browse to:");
  display.println(ip.toString());
  display.display();
  delay(4000);

  tempMutex = xSemaphoreCreateMutex();

  xTaskCreate(taskSensor,    "Sensor",    4096, NULL, 2, NULL);
  xTaskCreate(taskDisplay,   "Display",   8192, NULL, 1, NULL);
  xTaskCreate(taskWebServer, "WebServer", 4096, NULL, 1, NULL);
  xTaskCreate(taskWatchdog,  "Watchdog",  4096, NULL, 1, NULL);

  Serial.println("Tasks started");
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}