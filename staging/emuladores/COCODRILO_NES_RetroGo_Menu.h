#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

namespace CocodriloNESMenu {

enum Accion {
  CONTINUAR = 0,
  CARGADO,
  REINICIAR,
  SALIR
};

struct Pines {
  int up;
  int down;
  int left;
  int right;
  int a;
  int b;
  int menu;
};

using RedibujarFondoFn = void (*)(TFT_eSPI &tft);

// El core llama esto SOLO cuando ya pausó emulación/audio.
// El callback vuelve a pintar el último frame del juego para limpiar el menú
// sin usar rellenos sólidos, que son incompatibles con la sesión larga NES.
Accion abrir(TFT_eSPI &tft,
             const Pines &pines,
             uint32_t romChecksum,
             RedibujarFondoFn redibujarFondo);

} // namespace CocodriloNESMenu
