#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <time.h>
#include "SD_MMC.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "AudioMAX98357.h"
#include "Bateria.h"
#include "COCODRILO_NES_RetroGo_Menu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"

extern "C" {
#include <nofrendo.h>
#include <nes/nes.h>
#include <nes/input.h>
}

// ============================================================
// COCODRILO OS <-> Retro-Go / Nofrendo
// V0.2 FAST VIDEO - BUS STYLE ANEMOIA
//
// OBJETIVO DE ESTA VERSION:
// - Cargar .nes desde SD_MMC
// - Video nativo 256x240 centrado en TFT 320x240
// - Conversión PAL8 -> RGB565
// - Doble buffer DMA de 48 líneas
// - Framebuffer PAL8 en RAM interna cuando cabe
// - Conversión PAL8 -> RGB565 optimizada
// - Controles
// - MENU+B ~0.8 s -> menú de guardado/carga
// - FPS por Serial
//
// INCLUIDO:
// - Audio físico MAX98357 asíncrono
//
// SIGUIENTE:
// - Save states / SRAM
// - Menú in-game de guardado/carga
//
// Anemoia queda en /backup y NO se borra todavía.
// ============================================================

namespace CocodriloNES {

static constexpr uint32_t EXIT_CHORD_MS = 800;
static constexpr int NES_DMA_LINES = 48;
static constexpr int NES_W = 256;
static constexpr int NES_H = 240;
static constexpr int NES_PITCH_COCODRILO = NES_SCREEN_PITCH;

static nes_t *nesCore = nullptr;
static TFT_eSPI *tftCore = nullptr;

static uint8_t *framePAL = nullptr;
static uint16_t *dmaBuf[2] = {nullptr, nullptr};
static uint16_t pal565[256] = {0};

static bool dmaActivo = false;
static bool swapAnterior = false;
static bool framePALInterno = false;

static bool audioNESActivo = false;
static RingbufHandle_t audioRingNES = nullptr;
static TaskHandle_t audioTaskNES = nullptr;
static volatile bool audioTaskNESSalir = false;

// Nofrendo mezcla internamente a 32 kHz. Como el core hoy corre alrededor
// de 48 FPS frente a 60 Hz nominales, reproducimos a ~80% del clock para
// mantener continuidad y sincronía aproximada sin resampling pesado.
static constexpr uint32_t NOFRENDO_APU_RATE = 32000;
static constexpr uint32_t NOFRENDO_I2S_RATE = 25600;
static constexpr size_t NES_AUDIO_RING_BYTES = 24576;

static void tareaAudioNES(void *param) {
  (void)param;

  while (!audioTaskNESSalir) {
    size_t bytes = 0;
    void *item = xRingbufferReceive(audioRingNES, &bytes, pdMS_TO_TICKS(20));
    if (!item) continue;

    if (audioNESActivo && bytes >= sizeof(int16_t)) {
      AudioMAX98357::escribirPCMEstereo16(
          (const int16_t*)item,
          bytes / sizeof(int16_t));
    }

    vRingbufferReturnItem(audioRingNES, item);
  }

  audioTaskNES = nullptr;
  vTaskDelete(nullptr);
}

static bool iniciarAudioAsincronoNES() {
  if (audioRingNES || audioTaskNES) return true;

  audioRingNES = xRingbufferCreate(NES_AUDIO_RING_BYTES, RINGBUF_TYPE_BYTEBUF);
  if (!audioRingNES) return false;

  audioTaskNESSalir = false;

  BaseType_t ok = xTaskCreatePinnedToCore(
      tareaAudioNES,
      "NES_Audio",
      4096,
      nullptr,
      3,
      &audioTaskNES,
      0);

  if (ok != pdPASS) {
    vRingbufferDelete(audioRingNES);
    audioRingNES = nullptr;
    audioTaskNES = nullptr;
    return false;
  }

  return true;
}

static void detenerAudioAsincronoNES() {
  audioTaskNESSalir = true;

  const uint32_t t0 = millis();
  while (audioTaskNES != nullptr && (millis() - t0) < 150) {
    vTaskDelay(pdMS_TO_TICKS(1));
  }

  if (audioRingNES) {
    vRingbufferDelete(audioRingNES);
    audioRingNES = nullptr;
  }
}

static inline void enviarAudioFrameNES() {
  if (!audioNESActivo || !audioRingNES || !nesCore || !nesCore->apu ||
      !nesCore->apu->buffer || nesCore->apu->samples_per_frame <= 0) {
    return;
  }

  const size_t muestras =
      (size_t)nesCore->apu->samples_per_frame * (nesCore->apu->stereo ? 2u : 1u);
  const size_t bytes = muestras * sizeof(int16_t);

  // Nunca frenar la emulación por audio. Si la salida se atrasa,
  // descartamos el bloque más nuevo antes que introducir un tirón de vídeo.
  xRingbufferSend(audioRingNES, nesCore->apu->buffer, bytes, 0);
}

static void mostrarError(TFT_eSPI &tft, const char *l1, const char *l2 = nullptr) {
  if (dmaActivo) {
    tft.dmaWait();
    tft.endWrite();
  }

  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_RED, TFT_BLACK);
  tft.drawString(l1, 160, 104, 2);

  if (l2) {
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(l2, 160, 132, 1);
  }

  tft.setTextDatum(TL_DATUM);
  delay(1200);
}

static void rutaVFS(const char *rutaSD, char *out, size_t n) {
  if (!out || n == 0) return;

  if (!rutaSD || !rutaSD[0]) {
    out[0] = 0;
    return;
  }

  if (strncmp(rutaSD, "/sdcard/", 8) == 0) {
    snprintf(out, n, "%s", rutaSD);
  } else if (rutaSD[0] == '/') {
    snprintf(out, n, "/sdcard%s", rutaSD);
  } else {
    snprintf(out, n, "/sdcard/%s", rutaSD);
  }
}

static bool prepararVideo(TFT_eSPI &tft) {
  const size_t bytesDMA = NES_W * NES_DMA_LINES * sizeof(uint16_t);
  const size_t bytesPAL = NES_PITCH_COCODRILO * NES_H;

  // Reservar DMA primero para asegurar memoria DMA-capable.
  dmaBuf[0] = (uint16_t*)heap_caps_malloc(
      bytesDMA,
      MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  dmaBuf[1] = (uint16_t*)heap_caps_malloc(
      bytesDMA,
      MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

  if (!dmaBuf[0] || !dmaBuf[1]) return false;

  // El PPU toca este framebuffer continuamente. RAM interna primero.
  framePAL = (uint8_t*)heap_caps_malloc(
      bytesPAL,
      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

  framePALInterno = (framePAL != nullptr);

  if (!framePAL) {
    framePAL = (uint8_t*)heap_caps_malloc(
        bytesPAL,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }

  if (!framePAL) return false;

  memset(framePAL, 0, bytesPAL);

  Serial.printf("[NOFRENDO] video DMA48x2=%u framebuffer=%u (%s)\n",
                (unsigned)(bytesDMA * 2),
                (unsigned)bytesPAL,
                framePALInterno ? "RAM INTERNA" : "PSRAM");

  uint16_t *p = (uint16_t*)nofrendo_buildpalette(NES_PALETTE_PVM, 16);
  if (!p) return false;

  memcpy(pal565, p, sizeof(pal565));
  free(p);

  swapAnterior = tft.getSwapBytes();
  tft.setSwapBytes(true);
  tft.endWrite();
  tft.fillScreen(TFT_BLACK);
  // Community: la TFT_eSPI instalada por Arduino CLI puede usar una ruta
  // DMA distinta de la copia local del firmware original. La consola
  // reporto sonido con pantalla negra; forzamos el blitter SPI convencional
  // hasta validar el DMA en hardware. A 80 MHz sigue siendo utilizable.
  // El blitCallback ya tiene fallback por pushPixels() sin DMA.
  dmaActivo = false;
  Serial.println("[NOFRENDO] video SPI convencional (DMA desactivado)");
  tft.startWrite();

  return true;
}

static void liberarVideo(TFT_eSPI &tft) {
  if (dmaActivo) {
    tft.dmaWait();
    tft.endWrite();
    tft.deInitDMA();
  } else {
    tft.endWrite();
  }

  tft.setSwapBytes(swapAnterior);

  if (dmaBuf[0]) {
    heap_caps_free(dmaBuf[0]);
    dmaBuf[0] = nullptr;
  }
  if (dmaBuf[1]) {
    heap_caps_free(dmaBuf[1]);
    dmaBuf[1] = nullptr;
  }
  if (framePAL) {
    heap_caps_free(framePAL);
    framePAL = nullptr;
  }

  dmaActivo = false;
  framePALInterno = false;
  tft.fillScreen(TFT_BLACK);
}

#pragma GCC push_options
#pragma GCC optimize ("O3")
static inline __attribute__((always_inline)) void prepararBloquePAL(
    uint16_t * __restrict__ dst,
    const uint8_t * __restrict__ bmp,
    int y0,
    int lineas) {

  const uint16_t * __restrict__ pal = pal565;

  for (int by = 0; by < lineas; ++by) {
    const uint8_t * __restrict__ src =
        bmp + ((y0 + by) * NES_PITCH_COCODRILO) + NES_SCREEN_OVERDRAW;
    uint16_t * __restrict__ d = dst + (by * NES_W);

    // 256 exacto: desenrollado x8.
    for (int x = 0; x < NES_W; x += 8) {
      d[x + 0] = pal[src[x + 0]];
      d[x + 1] = pal[src[x + 1]];
      d[x + 2] = pal[src[x + 2]];
      d[x + 3] = pal[src[x + 3]];
      d[x + 4] = pal[src[x + 4]];
      d[x + 5] = pal[src[x + 5]];
      d[x + 6] = pal[src[x + 6]];
      d[x + 7] = pal[src[x + 7]];
    }
  }
}
#pragma GCC pop_options

// Nofrendo llama este callback al terminar el frame.
static void blitCallback(uint8 *bmp) {
  if (!bmp || !tftCore || !dmaBuf[0] || !dmaBuf[1]) return;

  int sel = 0;

  // Una sola ventana para todo el frame. Los bloques DMA se envían
  // consecutivamente y el ST7789 avanza su dirección automáticamente.
  if (dmaActivo) {
    tftCore->dmaWait();
  }
  tftCore->setAddrWindow(32, 0, NES_W, NES_H);

  for (int y0 = 0; y0 < NES_H; y0 += NES_DMA_LINES) {
    const int lineas =
        ((y0 + NES_DMA_LINES) <= NES_H)
            ? NES_DMA_LINES
            : (NES_H - y0);

    uint16_t *buf = dmaBuf[sel];

    // Mientras el DMA anterior trabaja, la CPU convierte el siguiente bloque.
    prepararBloquePAL(buf, bmp, y0, lineas);

    if (dmaActivo) {
      tftCore->dmaWait();
      tftCore->pushPixelsDMA(buf, NES_W * lineas);
    } else {
      tftCore->pushPixels(buf, NES_W * lineas);
    }

    sel ^= 1;
  }

  // No esperamos aquí el último DMA: Nofrendo puede empezar a emular
  // el siguiente frame mientras la TFT termina de transmitir este bloque.
  // El siguiente blit sincroniza antes de reutilizar la ventana/buffers.
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

  const bool arriba    = digitalRead(PIN_UP) == LOW;
  const bool abajo     = digitalRead(PIN_DOWN) == LOW;
  const bool izquierda = digitalRead(PIN_LEFT) == LOW;
  const bool derecha   = digitalRead(PIN_RIGHT) == LOW;
  const bool a         = digitalRead(BUTTON_PIN) == LOW;
  const bool b         = digitalRead(PIN_VOLUMEN_PANEL) == LOW;
  const bool menu      = digitalRead(PIN_PAUSA) == LOW;

  int out = 0;

  if (arriba)    out |= NES_PAD_UP;
  if (abajo)     out |= NES_PAD_DOWN;
  if (izquierda) out |= NES_PAD_LEFT;
  if (derecha)   out |= NES_PAD_RIGHT;

  if (menu && !m.menuAnterior) {
    m.startPendiente = true;
    m.selectUsado = false;
    m.chordArmado = false;
    m.inicioChord = 0;
  }

  // MENU+B mantenidos ~0.8s -> abrir menú al soltar.
  if (menu && b) {
    m.startPendiente = false;

    if (m.inicioChord == 0) {
      m.inicioChord = millis();
    }

    if (millis() - m.inicioChord >= EXIT_CHORD_MS) {
      m.chordArmado = true;
    }
  } else {
    if (m.chordArmado) {
      abrirMenu = true;
      m.chordArmado = false;
      m.inicioChord = 0;
    }

    if (b && !menu) {
      out |= NES_PAD_B;
    }

    if (menu && !b && !m.chordArmado) {
      m.inicioChord = 0;
    }
  }

  // MENU+A = SELECT.
  if (menu && a && !b) {
    out |= NES_PAD_SELECT;
    m.selectUsado = true;
    m.startPendiente = false;
  } else if (a && !menu) {
    out |= NES_PAD_A;
  }

  // Toque corto MENU = START.
  if (!menu && m.menuAnterior) {
    if (m.startPendiente && !m.selectUsado && !m.chordArmado) {
      out |= NES_PAD_START;
    }

    m.startPendiente = false;
    m.selectUsado = false;
    m.inicioChord = 0;
  }

  m.menuAnterior = menu;
  return out;
}

static void redibujarFondoMenu(TFT_eSPI &tft) {
  if (!framePAL) return;

  const bool swap = tft.getSwapBytes();
  tft.setSwapBytes(true);

  // Reutilizar exactamente el blitter rápido del NES para volver a pintar
  // el último frame congelado y borrar restos del menú sin fillScreen().
  blitCallback(framePAL);
  if (dmaActivo) tft.dmaWait();

  tft.setSwapBytes(swap);
}

static void limpiarPantallaNegraDMA(TFT_eSPI &tft) {
  if (!dmaBuf[0]) return;

  // dmaBuf mide 256x48. Con negro (0x0000) podemos reutilizarlo para las tres
  // zonas de la TFT: borde izquierdo 32, centro 256 y borde derecho 32.
  memset(dmaBuf[0], 0, NES_W * NES_DMA_LINES * sizeof(uint16_t));

  for (int y0 = 0; y0 < NES_H; y0 += NES_DMA_LINES) {
    const int lineas =
        ((y0 + NES_DMA_LINES) <= NES_H)
            ? NES_DMA_LINES
            : (NES_H - y0);

    if (dmaActivo) tft.dmaWait();
    tft.setAddrWindow(0, y0, 32, lineas);
    if (dmaActivo) tft.pushPixelsDMA(dmaBuf[0], 32 * lineas);
    else tft.pushPixels(dmaBuf[0], 32 * lineas);

    if (dmaActivo) tft.dmaWait();
    tft.setAddrWindow(32, y0, NES_W, lineas);
    if (dmaActivo) tft.pushPixelsDMA(dmaBuf[0], NES_W * lineas);
    else tft.pushPixels(dmaBuf[0], NES_W * lineas);

    if (dmaActivo) tft.dmaWait();
    tft.setAddrWindow(32 + NES_W, y0, 32, lineas);
    if (dmaActivo) tft.pushPixelsDMA(dmaBuf[0], 32 * lineas);
    else tft.pushPixels(dmaBuf[0], 32 * lineas);
  }

  if (dmaActivo) tft.dmaWait();
}

static void limpiarZonaHUDNES(TFT_eSPI &tft, int x, int y, int w, int h) {
  if (!dmaBuf[0] || w <= 0 || h <= 0) return;
  if (dmaActivo) tft.dmaWait();

  memset(dmaBuf[0], 0, (size_t)w * h * sizeof(uint16_t));
  tft.setAddrWindow(x, y, w, h);
  if (dmaActivo) {
    tft.pushPixelsDMA(dmaBuf[0], w * h);
    tft.dmaWait();
  } else {
    tft.pushPixels(dmaBuf[0], w * h);
  }
}

static void dibujarBateriaNES(TFT_eSPI &tft) {
  static constexpr int X = 288, W = 32;
  limpiarZonaHUDNES(tft, X, 198, W, 42);

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

static void dibujarHUDNESCompleto(TFT_eSPI &tft) {
  dibujarBateriaNES(tft);
}

static void limpiarBordesNegrosDMA(TFT_eSPI &tft) {
  if (!dmaBuf[0]) return;

  static constexpr int BORDE_W = 32;
  static constexpr int BLOQUE_H = NES_DMA_LINES;

  // Negro RGB565 = 0x0000. Reutilizamos el buffer DMA del NES para limpiar
  // exclusivamente las columnas laterales, sin fillRect() ni cambiar la
  // transacción larga que mantiene el rendimiento.
  memset(dmaBuf[0], 0, BORDE_W * BLOQUE_H * sizeof(uint16_t));

  for (int y0 = 0; y0 < NES_H; y0 += BLOQUE_H) {
    const int lineas =
        ((y0 + BLOQUE_H) <= NES_H) ? BLOQUE_H : (NES_H - y0);
    const uint32_t pixeles = BORDE_W * lineas;

    if (dmaActivo) tft.dmaWait();
    tft.setAddrWindow(0, y0, BORDE_W, lineas);
    if (dmaActivo) tft.pushPixelsDMA(dmaBuf[0], pixeles);
    else tft.pushPixels(dmaBuf[0], pixeles);

    if (dmaActivo) tft.dmaWait();
    tft.setAddrWindow(32 + NES_W, y0, BORDE_W, lineas);
    if (dmaActivo) tft.pushPixelsDMA(dmaBuf[0], pixeles);
    else tft.pushPixels(dmaBuf[0], pixeles);
  }

  if (dmaActivo) tft.dmaWait();
}

static uint32_t idRomGuardado(const char *ruta) {
  if (nesCore && nesCore->cart && nesCore->cart->checksum != 0) {
    return nesCore->cart->checksum;
  }

  // Fallback estable si el CRC del core llega en cero.
  uint32_t h = 2166136261u;
  const unsigned char *p = (const unsigned char *)(ruta ? ruta : "");
  while (*p) {
    h ^= (uint32_t)(*p++);
    h *= 16777619u;
  }
  return h ? h : 1u;
}

static bool ejecutar(const char *ruta, TFT_eSPI &tft) {
  if (!ruta || !SD_MMC.exists(ruta)) {
    mostrarError(tft, "ROM NES NO ENCONTRADA", ruta);
    return false;
  }

  Serial.println("[NOFRENDO] === COCODRILO NES Retro-Go V0.2 BUS STYLE ANEMOIA ===");
  Serial.printf("[NOFRENDO] ROM: %s\n", ruta);
  Serial.printf("[NOFRENDO] heap antes=%u PSRAM=%u\n",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getFreePsram());

  if (!prepararVideo(tft)) {
    liberarVideo(tft);
    mostrarError(tft, "SIN RAM PARA VIDEO NES");
    return false;
  }

  tftCore = &tft;

  // Nofrendo genera PCM estéreo cada frame; la salida física se desacopla
  // mediante ring buffer + tarea I2S para no bloquear el loop del core.
  nesCore = nes_init(SYS_DETECT, NOFRENDO_APU_RATE, true, nullptr);

  if (!nesCore) {
    tftCore = nullptr;
    liberarVideo(tft);
    mostrarError(tft, "NOFRENDO INIT FALLO");
    return false;
  }

  char rutaROM[384];
  rutaVFS(ruta, rutaROM, sizeof(rutaROM));

  const int carga = nes_loadfile(rutaROM);

  if (carga < 0) {
    nes_shutdown();
    nesCore = nullptr;
    tftCore = nullptr;
    liberarVideo(tft);

    if (carga == -2)
      mostrarError(tft, "MAPPER NO SOPORTADO");
    else if (carga == -3)
      mostrarError(tft, "ROM REQUIERE BIOS");
    else
      mostrarError(tft, "NOFRENDO NO CARGO ROM");

    return false;
  }

  nesCore->blit_func = blitCallback;
  nes_setvidbuf(framePAL);

  audioNESActivo = AudioMAX98357::entrarModoEmulador();
  if (audioNESActivo) {
    if (!AudioMAX98357::setSampleRateModoEmulador(NOFRENDO_I2S_RATE) ||
        !iniciarAudioAsincronoNES()) {
      audioNESActivo = false;
      detenerAudioAsincronoNES();
      AudioMAX98357::salirModoEmulador();
      Serial.println("[NOFRENDO] audio async no disponible; continúa sin sonido");
    } else {
      Serial.printf("[NOFRENDO] audio MAX98357 APU=%u Hz I2S=%u Hz\n",
                    (unsigned)NOFRENDO_APU_RATE,
                    (unsigned)NOFRENDO_I2S_RATE);
    }
  } else {
    Serial.println("[NOFRENDO] MAX98357 no disponible; continúa sin sonido");
  }

  // Retro-Go hace dos frames sin dibujo antes de empezar.
  nes_emulate(false);
  nes_emulate(false);

  EstadoMenu menu;
  menu.menuAnterior = digitalRead(PIN_PAUSA) == LOW;

  uint64_t siguiente =
      esp_timer_get_time() + (1000000ULL / (uint64_t)nesCore->refresh_rate);

  uint32_t frames = 0;
  uint64_t fpsInicio = esp_timer_get_time();
  uint32_t bateriaHUDAnterior = 0;

  // Asegurar que el medidor continúe actualizándose durante el emulador.
  Bateria::PrepararParaMenu();
  Bateria::Actualizar();
  dibujarHUDNESCompleto(tft);
  bateriaHUDAnterior = millis();

  while (true) {
    bool abrirMenu = false;
    const int botones = leerControles(menu, abrirMenu);

    if (abrirMenu) {
      input_update(0, 0);

      const bool audioAntesMenu = audioNESActivo;
      audioNESActivo = false;

      if (dmaActivo) {
        tft.dmaWait();
      }

      // Reafirmar modo de color normal del ST7789 sin desmontar la sesión
      // larga SPI/DMA. El menú no debe heredar ni dejar INVON activo.
      tft.writecommand(TFT_INVOFF);
      tft.writecommand(TFT_INVOFF);

      // Mantener intacta la transacción SPI/DMA del emulador.
      // El menú puede dibujar sobre la misma sesión; desmontar y volver a
      // arrancar startWrite/endWrite estaba dejando al TFT_eSPI en un estado
      // más lento al regresar al gameplay.
      const bool swapMenuAnterior = tft.getSwapBytes();
      tft.setSwapBytes(false);

      const CocodriloNESMenu::Pines pinesMenu = {
        PIN_UP,
        PIN_DOWN,
        PIN_LEFT,
        PIN_RIGHT,
        BUTTON_PIN,
        PIN_VOLUMEN_PANEL,
        PIN_PAUSA
      };

      const uint32_t checksum = idRomGuardado(ruta);

      const CocodriloNESMenu::Accion accion =
          CocodriloNESMenu::abrir(
              tft, pinesMenu, checksum, redibujarFondoMenu);

      tft.setSwapBytes(swapMenuAnterior);

      // Algunas primitivas del menú, ejecutadas dentro de la sesión larga,
      // pueden dejar el controlador en inversión. Reafirmar INVOFF antes
      // de devolver el bus al blitter NES.
      tft.writecommand(TFT_INVOFF);
      tft.writecommand(TFT_INVOFF);

      if (accion == CocodriloNESMenu::SALIR) {
        Serial.println("[NOFRENDO] menu -> salir");
        audioNESActivo = audioAntesMenu;
        break;
      }

      // El blitter NES reescribe inmediatamente los 256 px centrales.
      // Restaurar los bordes por la misma ruta DMA del gameplay evita usar
      // primitivas TFT normales dentro de la sesión larga.
      limpiarBordesNegrosDMA(tft);
      Bateria::Actualizar();
      dibujarHUDNESCompleto(tft);
      bateriaHUDAnterior = millis();
      audioNESActivo = audioAntesMenu;

      siguiente =
          esp_timer_get_time() + (1000000ULL / (uint64_t)nesCore->refresh_rate);
      fpsInicio = esp_timer_get_time();
      frames = 0;
      menu.menuAnterior = digitalRead(PIN_PAUSA) == LOW;
      continue;
    }

    input_update(0, botones);
    nes_emulate(true);
    enviarAudioFrameNES();
    frames++;

    const uint32_t ahoraMs = millis();

    // Batería: mantener el ritmo previo, pero redibujar solo su zona.
    if ((uint32_t)(ahoraMs - bateriaHUDAnterior) >= 1000U) {
      bateriaHUDAnterior = ahoraMs;
      Bateria::Actualizar();
      dibujarBateriaNES(tft);
    }

    const uint64_t frameUS =
        1000000ULL / (uint64_t)nesCore->refresh_rate;

    uint64_t ahora = esp_timer_get_time();
    if (ahora < siguiente) {
      delayMicroseconds((uint32_t)(siguiente - ahora));
    }

    ahora = esp_timer_get_time();

    if (ahora > siguiente + frameUS * 3ULL) {
      siguiente = ahora + frameUS;
    } else {
      siguiente += frameUS;
    }

    if (ahora - fpsInicio >= 2000000ULL) {
      const float seg = (float)(ahora - fpsInicio) / 1000000.0f;
      const float fpsReal = (float)frames / seg;
      Serial.printf(
          "[NOFRENDO] FPS reales=%.2f  heap=%u PSRAM=%u\n",
          fpsReal,
          (unsigned)ESP.getFreeHeap(),
          (unsigned)ESP.getFreePsram());

      frames = 0;
      fpsInicio = ahora;
    }

    taskYIELD();
  }

  input_update(0, 0);

  audioNESActivo = false;
  detenerAudioAsincronoNES();
  AudioMAX98357::salirModoEmulador();

  if (dmaActivo) {
    tft.dmaWait();
  }

  nes_shutdown();
  nesCore = nullptr;
  tftCore = nullptr;

  liberarVideo(tft);

  Serial.printf("[NOFRENDO] salida limpia heap=%u PSRAM=%u\n",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getFreePsram());

  return true;
}

} // namespace CocodriloNES
