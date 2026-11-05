#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <time.h>
#include "BacklightControl.h"
#include "SD_MMC.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "AudioMAX98357.h"
#include "CocodriloVolumeControl.h"
#include "Bateria.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"

extern "C" {
#include <gnuboy.h>
}

// ============================================================
// COCODRILO OS <-> Retro-Go / Gnuboy
// V3.5: MENU FAST + VOLUMEN + CONFIRMACIONES + BATERIA + VIDEO 266x240 DMA
// - Mantiene video, controles y carga SD originales.
// - GB/GBC funcional dentro de COCODRILO OS.
// - Mantiene framebuffer + soundbuffer.
// - Sin pruebas de diagnostico ni spam por Serial.
// - Audio físico MAX98357 + pantalla 266x240 centrada.
// ============================================================

namespace CocodriloGBC {

static constexpr int GBC_PIN_UP    = 1;
static constexpr int GBC_PIN_DOWN  = 16;
static constexpr int GBC_PIN_LEFT  = 7;
static constexpr int GBC_PIN_RIGHT = 15;
static constexpr int GBC_PIN_A     = 2;
static constexpr int GBC_PIN_B     = 4;
static constexpr int GBC_PIN_MENU  = 5;

static constexpr uint32_t FRAME_US = 16742;
static constexpr uint32_t EXIT_CHORD_MS = 800;

static uint16_t *framebuffer = nullptr;
static constexpr int GBC_BLOCK_LINES = 48;
static uint16_t *bloqueDMA[2] = {nullptr, nullptr};
static bool tftDMAActivo = false;
static int16_t *soundbuffer = nullptr;
static constexpr size_t SOUNDBUFFER_FRAMES = 2048;
static bool videoCallbackVioFrame = false;
static bool audioGBCActivo = false;
static RingbufHandle_t audioRing = nullptr;
static TaskHandle_t audioTask = nullptr;
static volatile bool audioTaskSalir = false;

static int slotEstado = 1;
static char rutaSRAMActual[320] = {0};
static char rutaEstadoActual[3][320] = {{0}};

static void videoCallback(void *buffer) {
  (void)buffer;
  videoCallbackVioFrame = true;
}

static void tareaAudioGBC(void *param) {
  (void)param;

  while (!audioTaskSalir) {
    size_t bytes = 0;
    void *item = xRingbufferReceive(audioRing, &bytes, pdMS_TO_TICKS(20));
    if (!item) continue;

    if (audioGBCActivo && bytes >= sizeof(int16_t)) {
      AudioMAX98357::escribirPCMEstereo16(
          (const int16_t*)item,
          bytes / sizeof(int16_t));
    }

    vRingbufferReturnItem(audioRing, item);
  }

  audioTask = nullptr;
  vTaskDelete(nullptr);
}

static bool iniciarAudioAsincronoGBC() {
  if (audioRing || audioTask) return true;

  audioRing = xRingbufferCreate(24576, RINGBUF_TYPE_BYTEBUF);
  if (!audioRing) return false;

  audioTaskSalir = false;

  BaseType_t ok = xTaskCreatePinnedToCore(
      tareaAudioGBC,
      "GBC_Audio",
      4096,
      nullptr,
      3,
      &audioTask,
      0);

  if (ok != pdPASS) {
    vRingbufferDelete(audioRing);
    audioRing = nullptr;
    audioTask = nullptr;
    return false;
  }

  return true;
}

static void detenerAudioAsincronoGBC() {
  audioTaskSalir = true;

  const uint32_t t0 = millis();
  while (audioTask != nullptr && (millis() - t0) < 150) {
    vTaskDelay(pdMS_TO_TICKS(1));
  }

  if (audioRing) {
    vRingbufferDelete(audioRing);
    audioRing = nullptr;
  }
}

static void audioCallback(void *buffer, size_t length) {
  if (!audioGBCActivo || !audioRing || !buffer || length == 0) return;

  const size_t bytes = length * sizeof(int16_t);

  // No bloquear nunca a Gnuboy. Si el consumidor se atrasa,
  // descartamos este bloque de PCM antes que meter un tirón al juego.
  xRingbufferSend(audioRing, buffer, bytes, 0);
}

// Escalado por bloques + doble buffer DMA.
// Mientras el SPI transmite un bloque, la CPU prepara el siguiente.
// 266x240 / 48 líneas => 5 transferencias por frame.
static uint8_t mapaX266[266];
static uint8_t mapaY240[240];
static bool mapasEscalaListos = false;

static void prepararMapasEscala() {
  if (mapasEscalaListos) return;

  for (int x = 0; x < 266; ++x) {
    mapaX266[x] = (uint8_t)((x * GB_WIDTH) / 266);
  }

  for (int y = 0; y < 240; ++y) {
    mapaY240[y] = (uint8_t)((y * GB_HEIGHT) / 240);
  }

  mapasEscalaListos = true;
}

static inline void prepararBloque(uint16_t *dstBase, int y0, int lineas) {
  int syAnterior = -1;
  uint16_t *filaAnterior = nullptr;

  for (int by = 0; by < lineas; ++by) {
    const int y = y0 + by;
    const int sy = mapaY240[y];
    uint16_t *dst = dstBase + by * 266;

    if (sy == syAnterior && filaAnterior) {
      memcpy(dst, filaAnterior, 266 * sizeof(uint16_t));
    } else {
      const uint16_t *src = framebuffer + sy * GB_WIDTH;

      for (int x = 0; x < 266; ++x) {
        dst[x] = src[mapaX266[x]];
      }

      syAnterior = sy;
      filaAnterior = dst;
    }
  }
}

static inline void dibujarFrame266x240(TFT_eSPI &tft) {
  if (!framebuffer || !bloqueDMA[0]) return;

  prepararMapasEscala();

  if (!tftDMAActivo || !bloqueDMA[1]) {
    for (int y0 = 0; y0 < 240; y0 += GBC_BLOCK_LINES) {
      const int lineas =
          ((y0 + GBC_BLOCK_LINES) <= 240) ? GBC_BLOCK_LINES : (240 - y0);
      prepararBloque(bloqueDMA[0], y0, lineas);
      tft.pushImage(27, y0, 266, lineas, bloqueDMA[0]);
    }
    return;
  }

  int sel = 0;

  for (int y0 = 0; y0 < 240; y0 += GBC_BLOCK_LINES) {
    const int lineas =
        ((y0 + GBC_BLOCK_LINES) <= 240) ? GBC_BLOCK_LINES : (240 - y0);

    uint16_t *buf = bloqueDMA[sel];
    prepararBloque(buf, y0, lineas);
    tft.pushImageDMA(27, y0, 266, lineas, buf);
    sel ^= 1;
  }

  // Igual que NES: dejar el último bloque en vuelo para solapar con Gnuboy.
}

static void liberarVideoGBC(TFT_eSPI &tft, bool swapAnterior) {
  // Mismo patrón de salida probado en NES: terminar primero cualquier DMA,
  // cerrar la sesión larga SPI, desmontar DMA y sólo entonces devolver la
  // TFT al estado normal del launcher.
  if (tftDMAActivo) {
    tft.dmaWait();
    tft.endWrite();
    tft.deInitDMA();
  } else {
    tft.endWrite();
  }

  tft.setSwapBytes(swapAnterior);

  if (bloqueDMA[0]) {
    heap_caps_free(bloqueDMA[0]);
    bloqueDMA[0] = nullptr;
  }
  if (bloqueDMA[1]) {
    heap_caps_free(bloqueDMA[1]);
    bloqueDMA[1] = nullptr;
  }
  if (framebuffer) {
    heap_caps_free(framebuffer);
    framebuffer = nullptr;
  }

  tftDMAActivo = false;
  tft.fillScreen(TFT_BLACK);
}

static void limpiarZonaHUDGBC(TFT_eSPI &tft, int x, int y, int w, int h) {
  if (!bloqueDMA[0] || w <= 0 || h <= 0) return;
  if (tftDMAActivo) tft.dmaWait();

  memset(bloqueDMA[0], 0, (size_t)w * h * sizeof(uint16_t));
  tft.setAddrWindow(x, y, w, h);
  if (tftDMAActivo) {
    tft.pushPixelsDMA(bloqueDMA[0], w * h);
    tft.dmaWait();
  } else {
    tft.pushPixels(bloqueDMA[0], w * h);
  }
}

static void dibujarBateriaGBC(TFT_eSPI &tft) {
  static constexpr int X = 293, W = 27;
  limpiarZonaHUDGBC(tft, X, 198, W, 42);

  if (!Bateria::hayBateria()) return;

  char bat[8];
  snprintf(bat, sizeof(bat), "%d%%", Bateria::getPorcentaje());

  const bool swapAntes = tft.getSwapBytes();
  tft.setSwapBytes(false);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE);
  tft.drawString(bat, X + W / 2, 214, 1);

  tft.setTextDatum(TL_DATUM);
  tft.setSwapBytes(swapAntes);
}

static void dibujarHUDGBCCompleto(TFT_eSPI &tft) {
  dibujarBateriaGBC(tft);
}

struct EstadoMenu {
  bool menuAnterior = false;
  bool chordArmado = false;
  uint32_t inicioChord = 0;
  bool startPendiente = false;
  bool selectUsado = false;
};

static int leerControles(EstadoMenu &m, bool &abrirMenu) {
  abrirMenu = false;

  const bool up    = digitalRead(GBC_PIN_UP) == LOW;
  const bool down  = digitalRead(GBC_PIN_DOWN) == LOW;
  const bool left  = digitalRead(GBC_PIN_LEFT) == LOW;
  const bool right = digitalRead(GBC_PIN_RIGHT) == LOW;
  const bool a     = digitalRead(GBC_PIN_A) == LOW;
  const bool b     = digitalRead(GBC_PIN_B) == LOW;
  const bool menu  = digitalRead(GBC_PIN_MENU) == LOW;

  int pad = 0;
  if (up)    pad |= GB_PAD_UP;
  if (down)  pad |= GB_PAD_DOWN;
  if (left)  pad |= GB_PAD_LEFT;
  if (right) pad |= GB_PAD_RIGHT;

  if (menu && !m.menuAnterior) {
    m.startPendiente = true;
    m.selectUsado = false;
    m.chordArmado = false;
    m.inicioChord = 0;
  }

  // MENU+B mantenidos abre el menú in-game al soltar.
  if (menu && b) {
    m.startPendiente = false;
    if (m.inicioChord == 0) m.inicioChord = millis();
    if (millis() - m.inicioChord >= EXIT_CHORD_MS) {
      m.chordArmado = true;
    }
  } else {
    if (m.chordArmado) {
      abrirMenu = true;
      m.chordArmado = false;
      m.inicioChord = 0;
    }
    if (b && !menu) pad |= GB_PAD_B;
    if (menu && !b && !m.chordArmado) m.inicioChord = 0;
  }

  // MENU+A conserva SELECT.
  if (menu && a && !b) {
    pad |= GB_PAD_SELECT;
    m.selectUsado = true;
    m.startPendiente = false;
  } else if (a && !menu) {
    pad |= GB_PAD_A;
  }

  // Toque corto a MENU conserva START.
  if (!menu && m.menuAnterior) {
    if (m.startPendiente && !m.selectUsado && !m.chordArmado) {
      pad |= GB_PAD_START;
    }
    m.startPendiente = false;
    m.selectUsado = false;
    m.inicioChord = 0;
  }

  m.menuAnterior = menu;
  return pad;
}

static void mostrarError(TFT_eSPI &tft, const char *titulo, const char *detalle = nullptr) {
  tft.endWrite();
  tft.fillScreen(TFT_BLACK);
  tft.startWrite();
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_RED, TFT_BLACK);
  tft.drawString(titulo, 160, 105, 2);
  if (detalle) {
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(detalle, 160, 132, 1);
  }
  tft.setTextDatum(TL_DATUM);
  delay(1200);
}


static void esperarSoltarBotones() {
  while (digitalRead(GBC_PIN_UP) == LOW ||
         digitalRead(GBC_PIN_DOWN) == LOW ||
         digitalRead(GBC_PIN_LEFT) == LOW ||
         digitalRead(GBC_PIN_RIGHT) == LOW ||
         digitalRead(GBC_PIN_A) == LOW ||
         digitalRead(GBC_PIN_B) == LOW ||
         digitalRead(GBC_PIN_MENU) == LOW) {
    delay(8);
  }
  delay(40);
}

static void nombreSeguroROM(const char *ruta, char *out, size_t n) {
  if (!out || n == 0) return;

  const char *base = strrchr(ruta ? ruta : "", '/');
  base = base ? base + 1 : (ruta ? ruta : "game");

  size_t len = strnlen(base, n - 1);
  const char *dot = strrchr(base, '.');
  if (dot && dot > base) len = (size_t)(dot - base);
  if (len >= n) len = n - 1;

  size_t j = 0;
  for (size_t i = 0; i < len && j + 1 < n; ++i) {
    char c = base[i];
    const bool ok =
        (c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == ' ';
    out[j++] = ok ? c : '_';
  }
  out[j] = 0;

  if (j == 0) snprintf(out, n, "game");
}

static void prepararRutasGuardado(const char *rutaROM) {
  SD_MMC.mkdir("/saves");
  SD_MMC.mkdir("/saves/gbc");
  SD_MMC.mkdir("/states");
  SD_MMC.mkdir("/states/gbc");

  char nombre[128];
  nombreSeguroROM(rutaROM, nombre, sizeof(nombre));

  snprintf(rutaSRAMActual, sizeof(rutaSRAMActual),
           "/sdcard/saves/gbc/%s.sav", nombre);

  for (int i = 0; i < 3; ++i) {
    snprintf(rutaEstadoActual[i], sizeof(rutaEstadoActual[i]),
             "/sdcard/states/gbc/%s.state%d", nombre, i + 1);
  }
}

static bool guardarSRAM(bool forzar = false) {
  if (!rutaSRAMActual[0]) return false;
  if (!forzar && !gnuboy_sram_dirty()) return true;
  return gnuboy_save_sram(rutaSRAMActual, forzar) == 0;
}

static bool guardarEstadoSlot(int slot) {
  if (slot < 1 || slot > 3) return false;
  guardarSRAM(false);
  return gnuboy_save_state(rutaEstadoActual[slot - 1]) == 0;
}

static bool cargarEstadoSlot(int slot) {
  if (slot < 1 || slot > 3) return false;
  return gnuboy_load_state(rutaEstadoActual[slot - 1]) == 0;
}

static bool existeEstadoSlot(int slot) {
  if (slot < 1 || slot > 3) return false;
  const char *p = rutaEstadoActual[slot - 1];
  if (strncmp(p, "/sdcard", 7) == 0) p += 7;
  return SD_MMC.exists(p);
}

enum AccionMenuGBC {
  MENU_GBC_CONTINUAR,
  MENU_GBC_GUARDAR,
  MENU_GBC_CARGAR,
  MENU_GBC_RESET,
  MENU_GBC_SALIR
};

static void textoConSombraGBC(TFT_eSPI &tft,
                              const char *txt,
                              int x,
                              int y,
                              int fuente,
                              uint16_t color) {
  tft.setTextColor(TFT_BLACK);
  tft.drawString(txt, x - 1, y, fuente);
  tft.drawString(txt, x + 1, y, fuente);
  tft.drawString(txt, x, y - 1, fuente);
  tft.drawString(txt, x, y + 1, fuente);
  tft.setTextColor(color);
  tft.drawString(txt, x, y, fuente);
}

static void redibujarFondoMenuGBC(TFT_eSPI &tft) {
  if (!framebuffer) return;

  const bool swapAntes = tft.getSwapBytes();
  tft.setSwapBytes(true);

  // Misma idea probada en NES: volver a pintar el último frame congelado
  // usando la ruta normal del emulador, sin fillScreen/fillRect.
  dibujarFrame266x240(tft);
  if (tftDMAActivo) tft.dmaWait();

  tft.setSwapBytes(swapAntes);
}

static void dibujarMenuGBC(TFT_eSPI &tft,
                           int seleccion,
                           bool confirmando = false,
                           bool confirmarSi = false,
                           int accionPendiente = -1,
                           const char *mensaje = nullptr) {
  static const char *items[] = {
    "CONTINUAR",
    "GUARDAR ESTADO",
    "CARGAR ESTADO",
    "VOLUMEN",
    "BRILLO",
    "REINICIAR JUEGO",
    "SALIR A EMULADORES"
  };
  static constexpr int NUM_ITEMS = 7;
  static constexpr int ITEM_VOLUMEN = 3;
  static constexpr int ITEM_BRILLO = 4;

  redibujarFondoMenuGBC(tft);
  tft.setTextDatum(MC_DATUM);

  textoConSombraGBC(tft, "COCODRILO / GAME BOY", 160, 18, 2, TFT_CYAN);

  char slotTxt[40];
  snprintf(slotTxt, sizeof(slotTxt), "SLOT %d  %s",
           slotEstado,
           existeEstadoSlot(slotEstado) ? "GUARDADO" : "VACIO");
  textoConSombraGBC(tft, slotTxt, 160, 40, 1, TFT_LIGHTGREY);

  for (int i = 0; i < NUM_ITEMS; ++i) {
    const int y = 62 + i * 21;
    char linea[42];

    if (i == ITEM_VOLUMEN) {
      snprintf(linea, sizeof(linea), "%c VOLUMEN  %u/10",
               i == seleccion ? '>' : ' ',
               (unsigned)CocodriloVolumeControl::get());
    } else if (i == ITEM_BRILLO) {
      snprintf(linea, sizeof(linea), "%c BRILLO   %d/10",
               i == seleccion ? '>' : ' ',
               BacklightControl::actual());
    } else {
      snprintf(linea, sizeof(linea), "%c %s",
               i == seleccion ? '>' : ' ',
               items[i]);
    }

    uint16_t color = (i == seleccion) ? TFT_CYAN : TFT_WHITE;
    if (i == 2 && !existeEstadoSlot(slotEstado)) color = TFT_DARKGREY;

    textoConSombraGBC(tft, linea, 160, y, 1, color);
  }

  if (mensaje && *mensaje) {
    textoConSombraGBC(tft, mensaje, 160, 218, 1, TFT_GREEN);
  } else if (confirmando && accionPendiente >= 0) {
    char pregunta[54];
    snprintf(pregunta, sizeof(pregunta), "%s?", items[accionPendiente]);

    textoConSombraGBC(tft, pregunta, 116, 218, 1, TFT_YELLOW);
    textoConSombraGBC(tft, "SI", 205, 218, 1,
                      confirmarSi ? TFT_CYAN : TFT_WHITE);
    textoConSombraGBC(tft, "NO", 244, 218, 1,
                      confirmarSi ? TFT_WHITE : TFT_CYAN);
  } else {
    const char *ayuda =
        seleccion == ITEM_VOLUMEN
            ? "IZQ/DER volumen   B volver"
            : (seleccion == ITEM_BRILLO
                  ? "IZQ/DER brillo   B volver"
                  : ((seleccion == 1 || seleccion == 2)
                        ? "IZQ/DER slot   A elegir   B volver"
                        : "A elegir   B volver"));
    textoConSombraGBC(tft, ayuda, 160, 218, 1, TFT_LIGHTGREY);
  }

  tft.setTextDatum(TL_DATUM);
}

static AccionMenuGBC abrirMenuGBC(TFT_eSPI &tft) {
  // Arquitectura rápida probada en NES/GBC:
  // mantener abierta la sesión larga y sólo esperar DMA antes del menú.
  if (tftDMAActivo) tft.dmaWait();

  guardarSRAM(false);
  esperarSoltarBotones();

  static constexpr int NUM_ITEMS = 7;
  static constexpr int ITEM_VOLUMEN = 3;
  static constexpr int ITEM_BRILLO = 4;

  int seleccion = 0;
  bool confirmando = false;
  bool confirmarSi = false;
  int accionPendiente = -1;

  bool prevUp = false, prevDown = false, prevLeft = false;
  bool prevRight = false, prevA = false, prevB = false;

  dibujarMenuGBC(tft, seleccion);

  auto ejecutarConfirmado = [&](int opcion) -> int {
    if (opcion == 0) {
      esperarSoltarBotones();
      return (int)MENU_GBC_CONTINUAR;
    }

    if (opcion == 1) {
      const bool ok = guardarEstadoSlot(slotEstado);
      dibujarMenuGBC(tft, seleccion, false, false, -1,
                     ok ? "ESTADO GUARDADO" : "ERROR GUARDANDO");
      delay(450);
      esperarSoltarBotones();
      return -1;
    }

    if (opcion == 2) {
      if (!existeEstadoSlot(slotEstado)) {
        dibujarMenuGBC(tft, seleccion, false, false, -1, "SLOT VACIO");
        delay(450);
        esperarSoltarBotones();
        return -1;
      }

      const bool ok = cargarEstadoSlot(slotEstado);
      if (ok) {
        esperarSoltarBotones();
        return (int)MENU_GBC_CARGAR;
      }

      dibujarMenuGBC(tft, seleccion, false, false, -1, "ERROR CARGANDO");
      delay(450);
      esperarSoltarBotones();
      return -1;
    }

    if (opcion == 5) {
      gnuboy_reset(true);
      gnuboy_load_sram(rutaSRAMActual);
      esperarSoltarBotones();
      return (int)MENU_GBC_RESET;
    }

    if (opcion == 6) {
      guardarSRAM(true);
      esperarSoltarBotones();
      return (int)MENU_GBC_SALIR;
    }

    return -1;
  };

  while (true) {
    const bool up    = digitalRead(GBC_PIN_UP) == LOW;
    const bool down  = digitalRead(GBC_PIN_DOWN) == LOW;
    const bool left  = digitalRead(GBC_PIN_LEFT) == LOW;
    const bool right = digitalRead(GBC_PIN_RIGHT) == LOW;
    const bool a     = digitalRead(GBC_PIN_A) == LOW;
    const bool b     = digitalRead(GBC_PIN_B) == LOW;

    if (confirmando) {
      if ((left && !prevLeft) || (right && !prevRight)) {
        confirmarSi = !confirmarSi;
        dibujarMenuGBC(
            tft, seleccion, true, confirmarSi, accionPendiente);
      }

      if (b && !prevB) {
        confirmando = false;
        confirmarSi = false;
        accionPendiente = -1;
        dibujarMenuGBC(tft, seleccion);
      }

      if (a && !prevA) {
        if (!confirmarSi) {
          confirmando = false;
          accionPendiente = -1;
          dibujarMenuGBC(tft, seleccion);
        } else {
          const int resultado = ejecutarConfirmado(accionPendiente);
          if (resultado >= 0) return (AccionMenuGBC)resultado;

          confirmando = false;
          confirmarSi = false;
          accionPendiente = -1;
          dibujarMenuGBC(tft, seleccion);
        }
      }
    } else {
      if (up && !prevUp) {
        seleccion = (seleccion + NUM_ITEMS - 1) % NUM_ITEMS;
        dibujarMenuGBC(tft, seleccion);
      }

      if (down && !prevDown) {
        seleccion = (seleccion + 1) % NUM_ITEMS;
        dibujarMenuGBC(tft, seleccion);
      }

      if ((left && !prevLeft) || (right && !prevRight)) {
        if (seleccion == ITEM_VOLUMEN) {
          uint8_t vol = CocodriloVolumeControl::get();
          if (left && vol > 0) --vol;
          if (right && vol < 10) ++vol;
          CocodriloVolumeControl::set(vol);
          dibujarMenuGBC(tft, seleccion);
        } else if (seleccion == ITEM_BRILLO) {
          int brillo = BacklightControl::actual();
          if (left && brillo > 1) --brillo;
          if (right && brillo < 10) ++brillo;
          BacklightControl::set(brillo);
          dibujarMenuGBC(tft, seleccion);
        } else if (seleccion == 1 || seleccion == 2) {
          if (left) slotEstado = slotEstado > 1 ? slotEstado - 1 : 3;
          else slotEstado = slotEstado < 3 ? slotEstado + 1 : 1;
          dibujarMenuGBC(tft, seleccion);
        }
      }

      if (b && !prevB) {
        esperarSoltarBotones();
        return MENU_GBC_CONTINUAR;
      }

      if (a && !prevA) {
        if (seleccion == ITEM_VOLUMEN || seleccion == ITEM_BRILLO) {
          // Volumen y brillo son inmediatos con izquierda/derecha.
        } else if (seleccion == 2 && !existeEstadoSlot(slotEstado)) {
          dibujarMenuGBC(tft, seleccion, false, false, -1, "SLOT VACIO");
          delay(450);
          esperarSoltarBotones();
          dibujarMenuGBC(tft, seleccion);
        } else {
          accionPendiente = seleccion;
          confirmarSi = false; // NO por defecto contra pulsaciones accidentales.
          confirmando = true;
          dibujarMenuGBC(
              tft, seleccion, true, confirmarSi, accionPendiente);
        }
      }
    }

    prevUp = up;
    prevDown = down;
    prevLeft = left;
    prevRight = right;
    prevA = a;
    prevB = b;
    delay(10);
  }
}

static void rutaVFS(const char *rutaSD, char *out, size_t n) {
  if (!rutaSD || !out || n == 0) return;
  if (strncmp(rutaSD, "/sdcard/", 8) == 0) {
    snprintf(out, n, "%s", rutaSD);
  } else if (rutaSD[0] == '/') {
    snprintf(out, n, "/sdcard%s", rutaSD);
  } else {
    snprintf(out, n, "/sdcard/%s", rutaSD);
  }
}

static bool ejecutar(const char *ruta, TFT_eSPI &tft) {
  if (!ruta || !SD_MMC.exists(ruta)) {
    mostrarError(tft, "ROM GB/GBC NO ENCONTRADA", ruta);
    return false;
  }

  Serial.printf("[GBC] ROM: %s\n", ruta);
  Serial.printf("[GBC] heap=%u PSRAM libre=%u\n",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getFreePsram());

  framebuffer = (uint16_t*)heap_caps_malloc(
      GB_WIDTH * GB_HEIGHT * sizeof(uint16_t),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

  if (!framebuffer) {
    framebuffer = (uint16_t*)malloc(GB_WIDTH * GB_HEIGHT * sizeof(uint16_t));
  }

  if (!framebuffer) {
    mostrarError(tft, "SIN RAM PARA FRAMEBUFFER");
    return false;
  }

  // Dos bloques DMA de 266x48. Mientras SPI envía uno,
  // la CPU escala el siguiente.
  const size_t bloqueBytes = 266 * GBC_BLOCK_LINES * sizeof(uint16_t);
  bloqueDMA[0] = (uint16_t*)heap_caps_malloc(
      bloqueBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  bloqueDMA[1] = (uint16_t*)heap_caps_malloc(
      bloqueBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  tftDMAActivo = false;

  if (!bloqueDMA[0] || !bloqueDMA[1]) {
    if (bloqueDMA[0]) free(bloqueDMA[0]);
    if (bloqueDMA[1]) free(bloqueDMA[1]);
    bloqueDMA[0] = nullptr;
    bloqueDMA[1] = nullptr;
    free(framebuffer);
    framebuffer = nullptr;
    mostrarError(tft, "SIN RAM DMA PARA VIDEO GBC");
    return false;
  }

  // Igualar el entorno que SI funciono en el repo aislado:
  // Gnuboy genera audio internamente aunque todavia no lo enviemos al MAX98357.
  // Reservamos un buffer estereo S16 de 2048 frames.
  const size_t soundBytes = SOUNDBUFFER_FRAMES * 2 * sizeof(int16_t);
  soundbuffer = (int16_t*)heap_caps_malloc(
      soundBytes,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

  if (!soundbuffer) {
    soundbuffer = (int16_t*)malloc(soundBytes);
  }

  if (!soundbuffer) {
    if (bloqueDMA[0]) free(bloqueDMA[0]);
    if (bloqueDMA[1]) free(bloqueDMA[1]);
    bloqueDMA[0] = nullptr;
    bloqueDMA[1] = nullptr;
    free(framebuffer);
    framebuffer = nullptr;
    mostrarError(tft, "SIN RAM PARA AUDIO GBC");
    return false;
  }

  memset(soundbuffer, 0, soundBytes);
  const int initRc = gnuboy_init(
      44100,
      GB_AUDIO_STEREO_S16,
      GB_PIXEL_565_LE,
      &videoCallback,
      &audioCallback);

  if (initRc < 0) {
    free(soundbuffer);
    soundbuffer = nullptr;
    if (bloqueDMA[0]) free(bloqueDMA[0]);
    if (bloqueDMA[1]) free(bloqueDMA[1]);
    bloqueDMA[0] = nullptr;
    bloqueDMA[1] = nullptr;
    free(framebuffer);
    framebuffer = nullptr;
    mostrarError(tft, "GNUBOY INIT FALLO");
    return false;
  }

  gnuboy_set_framebuffer(framebuffer);
  gnuboy_set_soundbuffer(soundbuffer, SOUNDBUFFER_FRAMES);

  char rutaPosix[320];
  rutaVFS(ruta, rutaPosix, sizeof(rutaPosix));

  Serial.printf("[GBC] VFS: %s\n", rutaPosix);

  const int loadRc = gnuboy_load_rom_file(rutaPosix);

  if (loadRc < 0) {
    gnuboy_free_rom();
    free(soundbuffer);
    soundbuffer = nullptr;
    if (bloqueDMA[0]) free(bloqueDMA[0]);
    if (bloqueDMA[1]) free(bloqueDMA[1]);
    bloqueDMA[0] = nullptr;
    bloqueDMA[1] = nullptr;
    free(framebuffer);
    framebuffer = nullptr;
    mostrarError(tft, "GNUBOY NO CARGO ROM");
    return false;
  }
  gnuboy_reset(true);

  prepararRutasGuardado(ruta);
  gnuboy_load_sram(rutaSRAMActual);

  audioGBCActivo = AudioMAX98357::entrarModoEmulador();
  if (audioGBCActivo) {
    if (!iniciarAudioAsincronoGBC()) {
      audioGBCActivo = false;
      AudioMAX98357::salirModoEmulador();
      Serial.println("[GBC] audio async no disponible; continúa sin sonido");
    }
  } else {
    Serial.println("[GBC] Audio MAX98357 no disponible; continúa sin sonido");
  }


  // Core listo: entrar directamente al loop normal.

  const bool swapAntes = tft.getSwapBytes();
  tft.endWrite();
  tft.fillScreen(TFT_BLACK);
  tft.setSwapBytes(true);

  // Escalado grande: 160x144 -> 266x240, ocupando toda la altura.
  // DMA permite solapar transferencia SPI y preparación del siguiente bloque.
  // Community usa el blitter sin DMA por compatibilidad con TFT_eSPI
  // del Library Manager. Conservamos los dos buffers y el camino DMA
  // para restaurarlo una vez validado sobre el mismo hardware.
  tftDMAActivo = false;
  Serial.println("[GBC VIDEO] escalado 266x240, SPI convencional (DMA desactivado)");

  tft.startWrite();

  EstadoMenu menu;
  menu.menuAnterior = digitalRead(GBC_PIN_MENU) == LOW;

  int padAnterior = -1;
  uint64_t siguiente = esp_timer_get_time() + FRAME_US;
  uint64_t fpsInicio = esp_timer_get_time();
  uint32_t frames = 0;
  uint64_t perfCoreUs = 0;
  uint64_t perfVideoUs = 0;
  uint32_t perfMuestras = 0;
  uint32_t bateriaHUDAnterior = 0;

  Bateria::PrepararParaMenu();
  Bateria::Actualizar();
  dibujarHUDGBCCompleto(tft);
  bateriaHUDAnterior = millis();

  while (true) {
    bool abrirMenu = false;
    const int pad = leerControles(menu, abrirMenu);

    if (abrirMenu) {
      gnuboy_set_pad(0);
      padAnterior = 0;

      const bool swapMenuAnterior = tft.getSwapBytes();
      tft.setSwapBytes(false);

      const AccionMenuGBC accion = abrirMenuGBC(tft);

      tft.setSwapBytes(swapMenuAnterior);

      if (accion == MENU_GBC_SALIR) {
        break;
      }

      Bateria::Actualizar();
      dibujarHUDGBCCompleto(tft);
      bateriaHUDAnterior = millis();

      // El menú puede durar bastante. Reiniciar reloj evita catch-up.
      siguiente = esp_timer_get_time() + FRAME_US;
      fpsInicio = esp_timer_get_time();
      frames = 0;
      perfCoreUs = 0;
      perfVideoUs = 0;
      perfMuestras = 0;

      menu.menuAnterior = digitalRead(GBC_PIN_MENU) == LOW;
      continue;
    }

    if (pad != padAnterior) {
      gnuboy_set_pad(pad);
      padAnterior = pad;
    }

    videoCallbackVioFrame = false;

    const uint64_t perfCoreInicio = esp_timer_get_time();
    gnuboy_run(true);
    const uint64_t perfCoreFin = esp_timer_get_time();
    perfCoreUs += perfCoreFin - perfCoreInicio;

    if (videoCallbackVioFrame) {
      const uint64_t perfVideoInicio = esp_timer_get_time();
      dibujarFrame266x240(tft);
      const uint64_t perfVideoFin = esp_timer_get_time();
      perfVideoUs += perfVideoFin - perfVideoInicio;
    }

    ++perfMuestras;
    frames++;

    const uint32_t ahoraMs = millis();

    if ((uint32_t)(ahoraMs - bateriaHUDAnterior) >= 1000U) {
      bateriaHUDAnterior = ahoraMs;
      Bateria::Actualizar();
      dibujarBateriaGBC(tft);
    }

    const uint64_t ahora = esp_timer_get_time();
    if (ahora < siguiente) {
      delayMicroseconds((uint32_t)(siguiente - ahora));
    }

    const uint64_t despues = esp_timer_get_time();
    if (despues > siguiente + FRAME_US * 3ULL) {
      siguiente = despues + FRAME_US;
    } else {
      siguiente += FRAME_US;
    }

    if (despues - fpsInicio >= 2000000ULL) {
      const float secs = (float)(despues - fpsInicio) / 1000000.0f;
      const float coreMs = perfMuestras
          ? (float)perfCoreUs / (float)perfMuestras / 1000.0f
          : 0.0f;
      const float videoMs = perfMuestras
          ? (float)perfVideoUs / (float)perfMuestras / 1000.0f
          : 0.0f;

      const float fpsReal = frames / secs;
      Serial.printf("[GBC] FPS=%.2f core=%.2fms video=%.2fms heap=%u psram=%u\n",
                    fpsReal,
                    coreMs,
                    videoMs,
                    (unsigned)ESP.getFreeHeap(),
                    (unsigned)ESP.getFreePsram());

      frames = 0;
      perfCoreUs = 0;
      perfVideoUs = 0;
      perfMuestras = 0;
      fpsInicio = despues;
    }

    taskYIELD();
  }

  // Salida con el mismo orden que la ruta NES estable.
  gnuboy_set_pad(0);

  guardarSRAM(true);

  audioGBCActivo = false;
  detenerAudioAsincronoGBC();
  AudioMAX98357::salirModoEmulador();

  if (tftDMAActivo) {
    tft.dmaWait();
  }

  gnuboy_free_rom();

  liberarVideoGBC(tft, swapAntes);

  if (soundbuffer) {
    heap_caps_free(soundbuffer);
    soundbuffer = nullptr;
  }

  Serial.printf("[GBC] salida limpia heap=%u PSRAM=%u\n",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getFreePsram());
  return true;
}

} // namespace CocodriloGBC
