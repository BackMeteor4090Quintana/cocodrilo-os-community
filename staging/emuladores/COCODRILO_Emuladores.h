#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>
#include "SD_MMC.h"
#include "COCODRILO_GBC_RetroGo.h"

// NESLauncher vive dentro de cocodrilo-os.ino y se define más abajo.
// Solo adelantamos su función pública para poder reutilizarla.
namespace NESLauncher {
  void entrar();
}

namespace EmuladoresLauncher {

// Pines actuales de COCODRILO OS.
static constexpr int PIN_UP_E    = 1;
static constexpr int PIN_DOWN_E  = 16;
static constexpr int PIN_A_E     = 2;
static constexpr int PIN_MENU_E  = 5;

static void esperarSoltar() {
  while (digitalRead(PIN_UP_E) == LOW ||
         digitalRead(PIN_DOWN_E) == LOW ||
         digitalRead(PIN_A_E) == LOW ||
         digitalRead(PIN_MENU_E) == LOW) {
    delay(5);
  }
  delay(50);
}

static void dibujarSelector(TFT_eSPI &tft, int sel) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString("EMULADORES", 160, 18, 2);
  tft.drawFastHLine(34, 43, 252, TFT_DARKGREY);

  const char *opciones[2] = {"Nintendo NES", "Game Boy / GBC"};
  const int ys[2] = {92, 142};

  for (int i = 0; i < 2; ++i) {
    if (i == sel) {
      tft.fillRoundRect(45, ys[i] - 14, 230, 34, 7, TFT_DARKGREY);
      tft.setTextColor(TFT_YELLOW, TFT_DARKGREY);
    } else {
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
    }
    tft.drawString(opciones[i], 160, ys[i], 2);
  }

  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("A: abrir   MENU: volver", 160, 216, 1);
  tft.setTextDatum(TL_DATUM);
}

static bool terminaEnGB(const String &ruta) {
  String s = ruta;
  s.toLowerCase();
  return s.endsWith(".gb") || s.endsWith(".gbc");
}

static String nombreBonitoGB(const String &ruta) {
  int slash = ruta.lastIndexOf('/');
  String n = (slash >= 0) ? ruta.substring(slash + 1) : ruta;
  if (n.endsWith(".gbc") || n.endsWith(".GBC")) n.remove(n.length() - 4);
  else if (n.endsWith(".gb") || n.endsWith(".GB")) n.remove(n.length() - 3);
  return n;
}

static constexpr int MAX_ROMS_GB = 80;
static String nombresGB[MAX_ROMS_GB];
static String rutasGB[MAX_ROMS_GB];
static int cantidadGB = 0;
static int seleccionGB = 0;
static int primeraGB = 0;
static constexpr int VISIBLES_GB = 7;

static void cargarDirectorioGB(const char *dirPath) {
  if (cantidadGB >= MAX_ROMS_GB) return;

  File dir = SD_MMC.open(dirPath);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }

  File f = dir.openNextFile();
  while (f && cantidadGB < MAX_ROMS_GB) {
    if (!f.isDirectory()) {
      String ruta = String(f.name());
      if (!ruta.startsWith("/")) {
        ruta = String(dirPath) + "/" + ruta;
      }
      if (terminaEnGB(ruta)) {
        rutasGB[cantidadGB] = ruta;
        nombresGB[cantidadGB] = nombreBonitoGB(ruta);
        cantidadGB++;
      }
    }
    f.close();
    f = dir.openNextFile();
  }
  dir.close();
}

static void cargarRomsGB() {
  cantidadGB = 0;
  seleccionGB = 0;
  primeraGB = 0;

  cargarDirectorioGB("/gb");
  cargarDirectorioGB("/gbc");

  for (int i = 0; i < cantidadGB - 1; ++i) {
    for (int j = i + 1; j < cantidadGB; ++j) {
      if (nombresGB[j].compareTo(nombresGB[i]) < 0) {
        String tn = nombresGB[i]; nombresGB[i] = nombresGB[j]; nombresGB[j] = tn;
        String tr = rutasGB[i];   rutasGB[i]   = rutasGB[j];   rutasGB[j]   = tr;
      }
    }
  }
}

static void dibujarBrowserGB(TFT_eSPI &tft) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString("GAME BOY / GBC", 160, 10, 2);
  tft.drawFastHLine(18, 34, 284, TFT_DARKGREY);

  if (cantidadGB == 0) {
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.drawString("NO HAY ROMS", 160, 100, 2);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString("Usa /gb/ o /gbc/ en la SD", 160, 132, 1);
    tft.drawString("MENU: volver", 160, 216, 1);
    tft.setTextDatum(TL_DATUM);
    return;
  }

  if (seleccionGB < primeraGB) primeraGB = seleccionGB;
  if (seleccionGB >= primeraGB + VISIBLES_GB)
    primeraGB = seleccionGB - VISIBLES_GB + 1;

  tft.setTextDatum(TL_DATUM);
  for (int fila = 0; fila < VISIBLES_GB; ++fila) {
    const int i = primeraGB + fila;
    if (i >= cantidadGB) break;

    const int y = 47 + fila * 23;
    if (i == seleccionGB) {
      tft.fillRoundRect(13, y - 2, 294, 20, 4, TFT_DARKGREY);
      tft.setTextColor(TFT_YELLOW, TFT_DARKGREY);
    } else {
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
    }

    String n = nombresGB[i];
    if (n.length() > 34) n = n.substring(0, 34);
    tft.drawString(n, 20, y, 1);
  }

  tft.setTextDatum(BC_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("A: jugar   MENU: volver", 160, 236, 1);
  tft.setTextDatum(TL_DATUM);
}

static void abrirGB(TFT_eSPI &tft) {
  esperarSoltar();
  cargarRomsGB();
  dibujarBrowserGB(tft);

  bool upPrev = false, downPrev = false, aPrev = false, menuPrev = false;

  while (true) {
    const bool up = digitalRead(PIN_UP_E) == LOW;
    const bool down = digitalRead(PIN_DOWN_E) == LOW;
    const bool a = digitalRead(PIN_A_E) == LOW;
    const bool menu = digitalRead(PIN_MENU_E) == LOW;

    if (menu && !menuPrev) {
      esperarSoltar();
      return;
    }

    if (cantidadGB > 0) {
      bool cambio = false;

      if (up && !upPrev) {
        seleccionGB = (seleccionGB + cantidadGB - 1) % cantidadGB;
        cambio = true;
      }
      if (down && !downPrev) {
        seleccionGB = (seleccionGB + 1) % cantidadGB;
        cambio = true;
      }

      if (cambio) dibujarBrowserGB(tft);

      if (a && !aPrev) {
        const String ruta = rutasGB[seleccionGB];
        esperarSoltar();

        Serial.printf("[EMU] Lanzando GB/GBC: %s\n", ruta.c_str());
        CocodriloGBC::ejecutar(ruta.c_str(), tft);

        // Al volver del core reconstruimos el browser.
        esperarSoltar();
        dibujarBrowserGB(tft);
      }
    }

    upPrev = up;
    downPrev = down;
    aPrev = a;
    menuPrev = menu;
    delay(5);
  }
}

static void entrar(TFT_eSPI &tft) {
  esperarSoltar();

  int seleccion = 0;
  bool upPrev = false, downPrev = false, aPrev = false, menuPrev = false;

  dibujarSelector(tft, seleccion);

  while (true) {
    const bool up = digitalRead(PIN_UP_E) == LOW;
    const bool down = digitalRead(PIN_DOWN_E) == LOW;
    const bool a = digitalRead(PIN_A_E) == LOW;
    const bool menu = digitalRead(PIN_MENU_E) == LOW;

    if (menu && !menuPrev) {
      esperarSoltar();
      return;
    }

    if ((up && !upPrev) || (down && !downPrev)) {
      seleccion ^= 1;
      dibujarSelector(tft, seleccion);
    }

    if (a && !aPrev) {
      esperarSoltar();

      if (seleccion == 0) {
        // Conserva exactamente el navegador NES RetroGo.
        NESLauncher::entrar();
        return;
      }

      abrirGB(tft);
      dibujarSelector(tft, seleccion);
    }

    upPrev = up;
    downPrev = down;
    aPrev = a;
    menuPrev = menu;
    delay(5);
  }
}

} // namespace EmuladoresLauncher
