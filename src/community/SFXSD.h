#pragma once

#include <Arduino.h>
#include "FS.h"
#include "SD_MMC.h"
#include "AudioMAX98357.h"

// Reproductor muy ligero de efectos WAV desde la SD.
// - No bloquea el loop del juego: reproduce en una tarea aparte.
// - PCM sin compresión, mono/estéreo, 8 o 16 bits.
// - 8 kHz .. 48 kHz.
// - Si la SD/archivo/formato falla, el juego puede usar su sonido heredado.
namespace SFXSD {

enum Resultado : uint8_t {
  INICIADO,
  OCUPADO,
  SIN_SD,
  NO_EXISTE,
  FORMATO_INVALIDO,
  ERROR_TAREA
};

static TaskHandle_t tarea = nullptr;
static volatile bool reproduciendo = false;
static volatile bool detenerSolicitado = false;
static char rutaActual[128] = {0};

struct WavInfo {
  uint16_t canales = 0;
  uint16_t bits = 0;
  uint32_t sampleRate = 0;
  uint32_t dataOffset = 0;
  uint32_t dataSize = 0;
};

static uint16_t leerLE16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t leerLE32(const uint8_t *p) {
  return (uint32_t)p[0] |
         ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static bool leerExacto(File &f, uint8_t *dst, size_t n) {
  return f.read(dst, n) == n;
}

static bool inspeccionarWav(File &f, WavInfo &info) {
  uint8_t cab[12];
  if (!leerExacto(f, cab, sizeof(cab))) return false;
  if (memcmp(cab, "RIFF", 4) != 0 || memcmp(cab + 8, "WAVE", 4) != 0) return false;

  bool tieneFmt = false;
  bool tieneData = false;

  while (f.available()) {
    uint8_t ch[8];
    if (!leerExacto(f, ch, sizeof(ch))) break;

    const uint32_t tam = leerLE32(ch + 4);
    const uint32_t inicio = (uint32_t)f.position();

    if (memcmp(ch, "fmt ", 4) == 0) {
      if (tam < 16) return false;
      uint8_t fmt[16];
      if (!leerExacto(f, fmt, sizeof(fmt))) return false;

      const uint16_t formato = leerLE16(fmt + 0);
      info.canales = leerLE16(fmt + 2);
      info.sampleRate = leerLE32(fmt + 4);
      info.bits = leerLE16(fmt + 14);

      if (formato != 1) return false; // solo PCM
      if (info.canales < 1 || info.canales > 2) return false;
      if (info.bits != 8 && info.bits != 16) return false;
      if (info.sampleRate < 8000 || info.sampleRate > 48000) return false;

      tieneFmt = true;
    } else if (memcmp(ch, "data", 4) == 0) {
      info.dataOffset = inicio;
      info.dataSize = tam;
      tieneData = true;
    }

    uint32_t siguiente = inicio + tam + (tam & 1U);
    if (siguiente > (uint32_t)f.size()) return false;
    if (!f.seek(siguiente)) return false;

    if (tieneFmt && tieneData) return true;
  }

  return tieneFmt && tieneData;
}

static void tareaReproduccion(void*) {
  char ruta[sizeof(rutaActual)];
  strncpy(ruta, rutaActual, sizeof(ruta) - 1);
  ruta[sizeof(ruta) - 1] = '\0';

  File f = SD_MMC.open(ruta, FILE_READ);
  if (!f) {
    reproduciendo = false;
    tarea = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  WavInfo info;
  if (!inspeccionarWav(f, info) || !f.seek(info.dataOffset)) {
    f.close();
    reproduciendo = false;
    tarea = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  if (!AudioMAX98357::entrarModoGaleria()) {
    f.close();
    reproduciendo = false;
    tarea = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  if (!AudioMAX98357::setSampleRateModoEmulador(info.sampleRate)) {
    AudioMAX98357::salirModoGaleria();
    f.close();
    reproduciendo = false;
    tarea = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  static uint8_t entrada[1024];
  static int16_t salida[512]; // 256 frames estéreo

  const uint16_t bytesPorMuestra = info.bits / 8;
  const uint16_t bytesPorFrame = bytesPorMuestra * info.canales;
  uint32_t restante = info.dataSize;

  while (restante >= bytesPorFrame && !detenerSolicitado) {
    uint16_t frames = 256;
    const uint32_t disponibles = restante / bytesPorFrame;
    if (frames > disponibles) frames = (uint16_t)disponibles;

    const size_t bytesLeer = (size_t)frames * bytesPorFrame;
    const size_t leidos = f.read(entrada, bytesLeer);
    if (leidos != bytesLeer) break;

    for (uint16_t n = 0; n < frames; ++n) {
      int16_t l = 0;
      int16_t r = 0;

      if (info.bits == 8) {
        const uint8_t *p = entrada + n * bytesPorFrame;
        l = (int16_t)(((int)p[0] - 128) << 8);
        r = (info.canales == 2)
              ? (int16_t)(((int)p[1] - 128) << 8)
              : l;
      } else {
        const uint8_t *p = entrada + n * bytesPorFrame;
        l = (int16_t)leerLE16(p);
        r = (info.canales == 2)
              ? (int16_t)leerLE16(p + 2)
              : l;
      }

      salida[n * 2] = l;
      salida[n * 2 + 1] = r;
    }

    if (!AudioMAX98357::escribirPCMEstereo16(salida, (size_t)frames * 2)) break;
    restante -= bytesLeer;
  }

  AudioMAX98357::salirModoGaleria();
  f.close();

  detenerSolicitado = false;
  reproduciendo = false;
  tarea = nullptr;
  vTaskDelete(nullptr);
}

static inline bool ocupado() {
  return reproduciendo;
}

static bool reproduciendoRuta(const char *ruta) {
  if (!reproduciendo || !ruta) return false;
  return strncmp(rutaActual, ruta, sizeof(rutaActual)) == 0;
}

static void detener() {
  if (!reproduciendo) return;
  detenerSolicitado = true;
}

static void detenerSiRuta(const char *ruta) {
  if (reproduciendoRuta(ruta)) detenerSolicitado = true;
}

static Resultado reproducir(const char *ruta, bool interrumpirActual = false) {
  if (!ruta || !ruta[0]) return NO_EXISTE;
  if (SD_MMC.cardType() == CARD_NONE) return SIN_SD;
  if (!SD_MMC.exists(ruta)) return NO_EXISTE;

  if (reproduciendo) {
    if (!interrumpirActual) return OCUPADO;

    detenerSolicitado = true;
    // Un bloque PCM normal dura apenas unos milisegundos. Esperamos un
    // margen pequeño para que la tarea cierre el WAV, vacíe DMA y libere
    // I2S antes de arrancar inmediatamente el nuevo efecto.
    const uint32_t limite = millis() + 60UL;
    while (reproduciendo && (int32_t)(limite - millis()) > 0) {
      vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (reproduciendo) return OCUPADO;
  }

  // Validación rápida antes de crear la tarea. De esta forma el llamador
  // todavía puede caer al buzzer si el WAV no es compatible.
  File prueba = SD_MMC.open(ruta, FILE_READ);
  if (!prueba) return NO_EXISTE;
  WavInfo info;
  const bool valido = inspeccionarWav(prueba, info);
  prueba.close();
  if (!valido) return FORMATO_INVALIDO;

  strncpy(rutaActual, ruta, sizeof(rutaActual) - 1);
  rutaActual[sizeof(rutaActual) - 1] = '\0';
  detenerSolicitado = false;
  reproduciendo = true;

  BaseType_t ok = xTaskCreatePinnedToCore(
      tareaReproduccion,
      "SFX_SD",
      4096,
      nullptr,
      3,
      &tarea,
      0
  );

  if (ok != pdPASS) {
    reproduciendo = false;
    tarea = nullptr;
    return ERROR_TAREA;
  }

  return INICIADO;
}

} // namespace SFXSD
