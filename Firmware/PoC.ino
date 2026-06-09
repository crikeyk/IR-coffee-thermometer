#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_MLX90614.h>

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define OLED_ADDRESS  0x3C
#define PIN_SCK  10
#define PIN_SDA   3

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Adafruit_MLX90614 mlx;

struct TempData {
  double ambient;
  double object;
  bool   valid;
};

TempData tempData = { 0, 0, false };
SemaphoreHandle_t tempMutex;

// ── Task 1: Read MLX90614 ────────────────────────────────────────
void taskSensor(void* pvParameters) {
  for (;;) {
    double amb = mlx.readAmbientTempC();
    double obj = mlx.readObjectTempC();
    bool valid = (amb > -40 && amb < 125 && obj > -70 && obj < 380);

    // Only update shared data if the read was good
    // Bad reads are silently dropped — display keeps showing last good values
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

// ── Task 2: Update display ───────────────────────────────────────
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
      display.println("AMB:");
      display.setCursor(28, 13);
      display.printf("%.2f C", snap.ambient);

      display.drawFastHLine(0, 23, SCREEN_WIDTH, SSD1306_WHITE);

      display.setCursor(0, 26);
      display.println("OBJ:");
      display.setCursor(28, 26);
      display.printf("%.2f C", snap.object);

      display.drawFastHLine(0, 36, SCREEN_WIDTH, SSD1306_WHITE);

      double delta = snap.object - snap.ambient;
      display.setCursor(0, 39);
      display.println("DELTA:");
      display.setCursor(40, 39);
      display.printf("%+.2f C", delta);

      display.drawFastHLine(0, 49, SCREEN_WIDTH, SSD1306_WHITE);

      display.setCursor(0, 52);
      display.printf("up: %lus  heap:%lu", millis() / 1000, esp_get_free_heap_size());
    }

    display.display();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ── Task 3: Watchdog ─────────────────────────────────────────────
void taskWatchdog(void* pvParameters) {
  for (;;) {
    TempData snap;
    if (xSemaphoreTake(tempMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      snap = tempData;
      xSemaphoreGive(tempMutex);
    }
    Serial.printf("heap: %lu  amb: %.2f  obj: %.2f  valid: %d\n",
                  esp_get_free_heap_size(),
                  snap.ambient, snap.object, snap.valid);
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

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(30, 28);
  display.println("Initialising...");
  display.display();

  // Do first read before tasks start — guarantees valid data from frame 1
  delay(200);  // give MLX a moment after init
  tempData.ambient = mlx.readAmbientTempC();
  tempData.object  = mlx.readObjectTempC();
  tempData.valid   = (tempData.ambient > -40 && tempData.ambient < 125 &&
                      tempData.object  > -70 && tempData.object  < 380);
  Serial.printf("First read: amb=%.2f obj=%.2f valid=%d\n",
                tempData.ambient, tempData.object, tempData.valid);

  tempMutex = xSemaphoreCreateMutex();

  xTaskCreate(taskSensor,   "Sensor",   4096, NULL, 2, NULL);
  xTaskCreate(taskDisplay,  "Display",  8192, NULL, 1, NULL);
  xTaskCreate(taskWatchdog, "Watchdog", 4096, NULL, 1, NULL);

  Serial.println("Tasks started");
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}