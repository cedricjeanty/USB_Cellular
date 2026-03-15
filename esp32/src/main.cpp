#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ── Pin assignments ────────────────────────────────────────────────────────────
#define PIN_I2C_SCL  7   // OLED CLK
#define PIN_I2C_SDA  8   // OLED DATA

// ── Display ────────────────────────────────────────────────────────────────────
#define SCREEN_W    128
#define SCREEN_H     64
#define OLED_ADDR  0x3C
#define OLED_RESET  -1   // shared with MCU reset

Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, OLED_RESET);

// ── Helpers ────────────────────────────────────────────────────────────────────
static void display_splash() {
    display.clearDisplay();

    // Title – large text
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(14, 6);
    display.print("AirBridge");

    // Divider
    display.drawLine(0, 26, 127, 26, SSD1306_WHITE);

    // Subtitle
    display.setTextSize(1);
    display.setCursor(32, 32);
    display.print("ESP32-S3");

    // Status
    display.setCursor(16, 50);
    display.print("Initializing...");

    display.display();
}

// ── Arduino entry points ───────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);

    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);

    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        Serial.println("SSD1306 init failed — check wiring");
        pinMode(LED_BUILTIN, OUTPUT);
        for (;;) {
            digitalWrite(LED_BUILTIN, HIGH); delay(200);
            digitalWrite(LED_BUILTIN, LOW);  delay(200);
        }
    }

    Serial.println("SSD1306 OK");
    display_splash();
}

void loop() {
    // Nothing to do yet — splash stays on screen.
}
