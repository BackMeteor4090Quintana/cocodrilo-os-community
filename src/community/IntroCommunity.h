#pragma once
// Pantalla de bienvenida neutral de Cocodrilo OS Community.
// No utiliza imágenes, nombres, dedicatorias ni datos personales.
// Requiere un objeto TFT_eSPI ya inicializado por el firmware anfitrión.

#include <Arduino.h>
#include <TFT_eSPI.h>

namespace CocodriloCommunityIntro {

inline void dibujar(TFT_eSPI& pantalla) {
  pantalla.fillScreen(TFT_BLACK);
  pantalla.setTextDatum(MC_DATUM);
  pantalla.setTextColor(TFT_CYAN, TFT_BLACK);
  pantalla.drawString("COCODRILO OS", pantalla.width() / 2, pantalla.height() / 2 - 28, 4);
  pantalla.setTextColor(TFT_WHITE, TFT_BLACK);
  pantalla.drawString("COMMUNITY", pantalla.width() / 2, pantalla.height() / 2 + 6, 2);
  pantalla.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  pantalla.drawString("Pulsa un boton para continuar", pantalla.width() / 2, pantalla.height() / 2 + 36, 2);
  pantalla.setTextDatum(TL_DATUM);
}

} // namespace CocodriloCommunityIntro
