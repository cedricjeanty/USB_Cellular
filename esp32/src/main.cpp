#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SPI.h>
#include <SdFat.h>
#include "USB.h"
#include "USBMSC.h"

// ── Pin assignments ────────────────────────────────────────────────────────────
#define PIN_I2C_SCL  7   // OLED CLK
#define PIN_I2C_SDA  8   // OLED DATA

#define PIN_SD_CS    10  // SD chip select
#define PIN_SD_MOSI  11
#define PIN_SD_MISO  12
#define PIN_SD_SCK   13

// ── Display ────────────────────────────────────────────────────────────────────
#define SCREEN_W    128
#define SCREEN_H     64
#define OLED_ADDR  0x3C
#define OLED_RESET  -1

Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, OLED_RESET);

// ── SD + USB MSC ───────────────────────────────────────────────────────────────
// With ARDUINO_USB_CDC_ON_BOOT=1, Serial is the TinyUSB CDC port.
// Serial0 is the hardware UART (GPIO 43/44).
#define DBG Serial

static SPIClass spi(HSPI);
static SdFs    sd;
static uint32_t sd_sectors = 0;

USBMSC MSC;

// Raw sector read — called by TinyUSB when the host reads from the drive.
// Note: `offset` is always 0 and bufsize is always a multiple of 512 in
// practice, but we handle the general case with sector arithmetic.
static int32_t msc_read(uint32_t lba, uint32_t offset, void* buf, uint32_t bufsize) {
    (void)offset;
    uint32_t count = bufsize / 512;
    if (!sd.card()->readSectors(lba, (uint8_t*)buf, count)) return -1;
    return bufsize;
}

// Raw sector write — called by TinyUSB when the host writes to the drive.
static int32_t msc_write(uint32_t lba, uint32_t offset, uint8_t* buf, uint32_t bufsize) {
    (void)offset;
    uint32_t count = bufsize / 512;
    if (!sd.card()->writeSectors(lba, buf, count)) return -1;
    sd.card()->syncDevice();
    return bufsize;
}

static bool msc_start_stop(uint8_t power_condition, bool start, bool load_eject) {
    (void)power_condition; (void)start; (void)load_eject;
    return true;
}

// ── Display helper ─────────────────────────────────────────────────────────────
static void disp(const char* line1, const char* line2 = nullptr) {
    display.clearDisplay();
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(14, 6);
    display.print("AirBridge");
    display.drawLine(0, 26, 127, 26, SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 32);
    display.print(line1);
    if (line2) { display.setCursor(0, 48); display.print(line2); }
    display.display();
}

// ── Arduino entry points ───────────────────────────────────────────────────────
void setup() {
    // Start USB first so the CDC port is available before we print anything.
    // MSC will be re-registered below after SD init; beginning USB here just
    // brings up the CDC interface so the host can open the port.
    USB.begin();
    DBG.begin(115200);
    // Wait up to 3 s for a CDC terminal to connect (skipped automatically if
    // no host opens the port, so normal operation is not blocked).
    uint32_t t0 = millis();
    while (!DBG && (millis() - t0) < 3000) { delay(10); }

    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        // No display — spin with debug output only
        for (;;) { DBG.println("SSD1306 init failed"); delay(1000); }
    }
    disp("Init SD card...");

    spi.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);

    SdSpiConfig sdConfig(PIN_SD_CS, DEDICATED_SPI, SD_SCK_MHZ(25), &spi);
    if (!sd.begin(sdConfig)) {
        disp("SD init failed!", "Check SPI wiring");
        DBG.println("SD init failed");
        for (;;) {}
    }

    sd_sectors = sd.card()->sectorCount();
    uint32_t sd_mb = sd_sectors / 2048UL;
    DBG.printf("SD card OK: %lu sectors (%lu MB)\n", sd_sectors, sd_mb);

    // Register USB MSC endpoints — USB.begin() was already called at the top
    // of setup() for CDC, so MSC.begin() here adds the mass-storage interface.
    MSC.vendorID("AirBridg");    // max 8 chars
    MSC.productID("SD Storage"); // max 16 chars
    MSC.productRevision("1.0");  // max 4 chars
    MSC.onRead(msc_read);
    MSC.onWrite(msc_write);
    MSC.onStartStop(msc_start_stop);
    MSC.mediaPresent(true);
    MSC.begin(sd_sectors, 512);

    char buf[24];
    snprintf(buf, sizeof(buf), "SD: %lu MB", sd_mb);
    disp("USB drive ready", buf);
    DBG.println("USB MSC started");
}

void loop() {
    // TinyUSB MSC is handled entirely in the background by the IDF USB task.
    delay(1000);
}
