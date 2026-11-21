#!/usr/bin/env bash
# Genera un firmware de PRUEBA NES-only con Anemoia fuera del repositorio.
# No modifica ni publica el firmware Community estable.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-${RUNNER_TEMP:-/tmp}/cocodrilo-anemoia-playtest}"
SKETCH="$OUT/AnemoiaPlaytest"
ANEMOIA="$ROOT/staging/anemoia"

if [[ "$SKETCH" == "$ROOT"/* ]]; then
  echo "Error: generar el firmware experimental fuera del repositorio." >&2
  exit 1
fi

test -s "$ANEMOIA/src/core/bus.cpp"
test -s "$ANEMOIA/COCODRILO_NES_Anemoia.h"
test -s "$ROOT/src/community/AudioMAX98357.h"
mkdir -p "$SKETCH"
cp -R "$ANEMOIA/src" "$SKETCH/src"
cp "$ANEMOIA/COCODRILO_NES_Anemoia.h" "$SKETCH/"
cp "$ROOT/src/community/AudioMAX98357.h" "$SKETCH/"
cp "$ROOT/src/community/metronomo_wav_pcm.h" "$SKETCH/"
cp "$ROOT/config/partitions.csv" "$SKETCH/partitions.csv"
cp "$ANEMOIA/ANEMOIA_LICENSE_GPLv3.txt" "$OUT/ANEMOIA_LICENSE_GPLv3.txt"

cat > "$SKETCH/AnemoiaPlaytest.ino" <<'EOF'
// COCODRILO OS | Anemoia NES Playtest
// Independiente del firmware Community. SOLO pruebas de NES.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <SD_MMC.h>
#include <FS.h>

// Pinout físico probado de COCODRILO OS ESP32-S3 N16R8.
#define PIN_UP 1
#define PIN_DOWN 16
#define PIN_LEFT 7
#define PIN_RIGHT 15
#define BUTTON_PIN 2
#define PIN_VOLUMEN_PANEL 4
#define PIN_PAUSA 5
#define SD_MMC_CLK 39
#define SD_MMC_CMD 38
#define SD_MMC_D0 40

#include "AudioMAX98357.h"
#include "COCODRILO_NES_Anemoia.h"

static TFT_eSPI pantalla;
static bool tarjetaLista = false;
static constexpr size_t MAX_JUEGOS = 80;
static String juegos[MAX_JUEGOS];
static size_t juegosTotal = 0;
static size_t seleccionado = 0;
static size_t primero = 0;
static constexpr size_t FILAS = 7;

static void botones() {
  const uint8_t pins[] = {
    PIN_UP, PIN_DOWN, PIN_LEFT, PIN_RIGHT,
    BUTTON_PIN, PIN_VOLUMEN_PANEL, PIN_PAUSA
  };
  for (uint8_t p : pins) pinMode(p, INPUT_PULLUP);
}

static void mensaje(const char* a, const char* b = nullptr) {
  pantalla.fillScreen(TFT_BLACK);
  pantalla.setTextDatum(MC_DATUM);
  pantalla.setTextColor(TFT_GREEN, TFT_BLACK);
  pantalla.drawString(a, 160, 92, 2);
  if (b) {
    pantalla.setTextColor(TFT_WHITE, TFT_BLACK);
    pantalla.drawString(b, 160, 128, 1);
  }
  pantalla.setTextDatum(TL_DATUM);
}

static void buscarJuegos() {
  juegosTotal = 0;
  seleccionado = 0;
  primero = 0;
  fs::File dir = SD_MMC.open("/NES");
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }
  fs::File f = dir.openNextFile();
  while (f && juegosTotal < MAX_JUEGOS) {
    if (!f.isDirectory()) {
      String nombre = f.name();
      if (!nombre.startsWith("/")) nombre = String("/NES/") + nombre;
      String ext = nombre;
      ext.toLowerCase();
      if (ext.endsWith(".nes")) juegos[juegosTotal++] = nombre;
    }
    f.close();
    f = dir.openNextFile();
  }
  if (f) f.close();
  dir.close();
  for (size_t i = 0; i < juegosTotal; ++i) {
    for (size_t j = i + 1; j < juegosTotal; ++j) {
      if (juegos[j].compareTo(juegos[i]) < 0) {
        String tmp = juegos[i]; juegos[i] = juegos[j]; juegos[j] = tmp;
      }
    }
  }
}

static void dibujarLista() {
  pantalla.fillScreen(TFT_BLACK);
  pantalla.setTextDatum(TC_DATUM);
  pantalla.setTextColor(TFT_GREEN, TFT_BLACK);
  pantalla.drawString("ANEMOIA NES - PRUEBA", 160, 7, 2);
  pantalla.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  pantalla.drawString("A: jugar   MENU: recargar", 160, 224, 1);
  if (!tarjetaLista) {
    pantalla.setTextColor(TFT_RED, TFT_BLACK);
    pantalla.drawString("SIN TARJETA SD_MMC", 160, 105, 2);
  } else if (!juegosTotal) {
    pantalla.setTextColor(TFT_YELLOW, TFT_BLACK);
    pantalla.drawString("COLOCA .nes EN /NES/", 160, 105, 2);
  } else {
    if (seleccionado < primero) primero = seleccionado;
    if (seleccionado >= primero + FILAS) primero = seleccionado - FILAS + 1;
    pantalla.setTextDatum(TL_DATUM);
    for (size_t row = 0; row < FILAS && primero + row < juegosTotal; ++row) {
      size_t i = primero + row;
      uint16_t bg = i == seleccionado ? TFT_DARKGREY : TFT_BLACK;
      uint16_t fg = i == seleccionado ? TFT_YELLOW : TFT_WHITE;
      int y = 40 + (int)row * 25;
      if (i == seleccionado) pantalla.fillRoundRect(10, y - 3, 300, 23, 4, bg);
      String nombre = juegos[i];
      nombre = nombre.substring(nombre.lastIndexOf('/') + 1);
      if (nombre.length() > 34) nombre = nombre.substring(0, 34);
      pantalla.setTextColor(fg, bg);
      pantalla.drawString(nombre, 16, y, 1);
    }
  }
  pantalla.setTextDatum(TL_DATUM);
}

static void recargar() {
  tarjetaLista = SD_MMC.cardType() != CARD_NONE;
  if (tarjetaLista) buscarJuegos();
  else juegosTotal = 0;
  dibujarLista();
}

void setup() {
  Serial.begin(115200);
  botones();
  pantalla.init();
  pantalla.setRotation(1);
  pantalla.fillScreen(TFT_BLACK);
  mensaje("INICIANDO ANEMOIA", "Firmware de prueba NES");
  const bool audioOK = AudioMAX98357::begin();
  Serial.printf("[ANEMOIA TEST] Audio I2S: %s\n", audioOK ? "OK" : "NO");
  if (!SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0)) {
    Serial.println("[ANEMOIA TEST] No se pudieron fijar pines SD");
  }
  tarjetaLista = SD_MMC.begin("/sdcard", true);
  Serial.printf("[ANEMOIA TEST] SD_MMC: %s\n", tarjetaLista ? "OK" : "ERROR");
  if (tarjetaLista) buscarJuegos();
  dibujarLista();
}

void loop() {
  static bool prevArriba = false, prevAbajo = false, prevA = false, prevMenu = false;
  bool arriba = digitalRead(PIN_UP) == LOW;
  bool abajo = digitalRead(PIN_DOWN) == LOW;
  bool a = digitalRead(BUTTON_PIN) == LOW;
  bool menu = digitalRead(PIN_PAUSA) == LOW;
  if (menu && !prevMenu) recargar();
  if (tarjetaLista && juegosTotal) {
    if (arriba && !prevArriba) {
      seleccionado = (seleccionado + juegosTotal - 1) % juegosTotal;
      dibujarLista();
    }
    if (abajo && !prevAbajo) {
      seleccionado = (seleccionado + 1) % juegosTotal;
      dibujarLista();
    }
    if (a && !prevA) {
      String ruta = juegos[seleccionado];
      mensaje("CARGANDO NES", "MENU+B: pausa/guardar/salir");
      delay(150);
      while (digitalRead(BUTTON_PIN) == LOW) delay(5);
      Serial.printf("[ANEMOIA TEST] ROM: %s\n", ruta.c_str());
      CocodriloNES::ejecutar(ruta.c_str(), pantalla);
      // El adaptador devuelve los recursos TFT/Audio al launcher de prueba.
      delay(100);
      while (digitalRead(PIN_PAUSA) == LOW || digitalRead(BUTTON_PIN) == LOW) delay(5);
      dibujarLista();
    }
  }
  prevArriba = arriba;
  prevAbajo = abajo;
  prevA = a;
  prevMenu = menu;
  delay(8);
}
EOF

echo "[Anemoia] Sketch NES-only preparado: $SKETCH"
echo "[Anemoia] NO contiene Gnuboy/Nofrendo; NO cambia BIN estables."
