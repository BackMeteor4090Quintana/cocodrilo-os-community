#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <Preferences.h>
#include <SD_MMC.h>
#include <WiFi.h>

// Prueba de compilación de hardware y core: NO es el firmware Community.
TFT_eSPI pantalla = TFT_eSPI();
void setup() {
  Serial.begin(115200);
  pantalla.init();
  pantalla.setRotation(1);
  pantalla.fillScreen(TFT_BLACK);
  pantalla.setTextColor(TFT_WHITE, TFT_BLACK);
  pantalla.drawString("Cocodrilo OS - prueba ESP32-S3", 4, 4, 2);
}
void loop() { delay(1000); }
