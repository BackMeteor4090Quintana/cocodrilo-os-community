#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>
#include "SD_MMC.h"
#include <new>
#include "esp_timer.h"
#include "AudioMAX98357.h"

// COCODRILO OS <-> Anemoia-ESP32
// FIX STACK:
// - Cartridge y Bus viven en HEAP, no en el stack de loopTask.
// - src/core/cartridge.cpp usa un buffer CRC static de 4096 bytes.
//
// Mapeo actual:
// D-pad -> NES D-pad
// A -> NES A
// B -> NES B
// MENU corto -> START
// MENU + A -> SELECT
// MENU + B ~0.8 s y soltar -> menu de pausa/guardado

#if __has_include("bus.h") && __has_include("cartridge.h")
  // Layout FLAT: core Anemoia en la raiz del sketch (GitHub/Arduino).
  #define COCODRILO_ANEMOIA_DISPONIBLE 1
  #include "bus.h"
  #include "cartridge.h"
#elif __has_include("src/core/bus.h") && __has_include("src/core/cartridge.h")
  // Layout original de Anemoia dentro de src/.
  #define COCODRILO_ANEMOIA_DISPONIBLE 1
  #include "src/core/bus.h"
  #include "src/core/cartridge.h"
#else
  #define COCODRILO_ANEMOIA_DISPONIBLE 0
#endif

namespace CocodriloNES {

static constexpr uint32_t EXIT_CHORD_MS = 800;
static constexpr uint32_t FRAME_US = 16639; // ~60.098 FPS NTSC

static constexpr uint8_t NES_A      = (1 << 0);
static constexpr uint8_t NES_B      = (1 << 1);
static constexpr uint8_t NES_SELECT = (1 << 2);
static constexpr uint8_t NES_START  = (1 << 3);
static constexpr uint8_t NES_UP     = (1 << 4);
static constexpr uint8_t NES_DOWN   = (1 << 5);
static constexpr uint8_t NES_LEFT   = (1 << 6);
static constexpr uint8_t NES_RIGHT  = (1 << 7);

struct EstadoMenu {
  bool menuAnterior = false;
  bool chordSelect = false;
  bool chordExit = false;
  bool startPendiente = false;
  bool salidaArmada = false;
  uint32_t inicioSalida = 0;
};

static inline uint8_t leerControles(EstadoMenu &m, bool &salir) {
  salir = false;

  const bool arriba    = digitalRead(PIN_UP) == LOW;
  const bool abajo     = digitalRead(PIN_DOWN) == LOW;
  const bool izquierda = digitalRead(PIN_LEFT) == LOW;
  const bool derecha   = digitalRead(PIN_RIGHT) == LOW;
  const bool a         = digitalRead(BUTTON_PIN) == LOW;
  const bool b         = digitalRead(PIN_VOLUMEN_PANEL) == LOW;
  const bool menu      = digitalRead(PIN_PAUSA) == LOW;

  uint8_t out = 0;
  if (arriba)    out |= NES_UP;
  if (abajo)     out |= NES_DOWN;
  if (izquierda) out |= NES_LEFT;
  if (derecha)   out |= NES_RIGHT;

  // MENU empieza como posible START.
  if (menu && !m.menuAnterior) {
    m.startPendiente = true;
    m.chordSelect = false;
    m.chordExit = false;
    m.salidaArmada = false;
    m.inicioSalida = 0;
  }

  // MENU + B = EXIT. B se consume y NO llega al juego.
  // Hay que mantener ambos ~800 ms; la salida ocurre al soltar
  // cualquiera de los dos para evitar activaciones accidentales.
  if (menu && b) {
    m.startPendiente = false;
    m.chordExit = true;

    if (m.inicioSalida == 0) {
      m.inicioSalida = millis();
    }

    if ((millis() - m.inicioSalida) >= EXIT_CHORD_MS) {
      m.salidaArmada = true;
    }
  } else {
    // Si el combo ya cumplió el tiempo, al soltar MENU o B salimos.
    if (m.salidaArmada) {
      salir = true;
    }

    // Si B está solo, funciona como NES B normalmente.
    if (b && !menu) {
      out |= NES_B;
    }

    // Si se abandonó el combo antes de 800 ms, permitir volver a intentarlo
    // durante la misma pulsación de MENU.
    if (menu && !b && !m.salidaArmada) {
      m.inicioSalida = 0;
    }
  }

  // MENU + A = SELECT. A se consume y no llega como NES A.
  // B+MENU tiene prioridad sobre SELECT.
  if (menu && a && !b) {
    out |= NES_SELECT;
    m.chordSelect = true;
    m.startPendiente = false;
  } else if (a && !menu) {
    out |= NES_A;
  }

  // MENU corto = START, siempre que no se haya usado como modificador.
  if (!menu && m.menuAnterior) {
    if (m.startPendiente && !m.chordSelect && !m.chordExit && !m.salidaArmada) {
      out |= NES_START;
    }

    m.startPendiente = false;
    m.chordSelect = false;
    m.chordExit = false;
    m.inicioSalida = 0;
  }

  // Después de marcar salida ya no importa conservar los estados.
  if (salir) {
    m.startPendiente = false;
    m.chordSelect = false;
    m.chordExit = false;
    m.salidaArmada = false;
    m.inicioSalida = 0;
  }

  m.menuAnterior = menu;
  return out;
}

#if COCODRILO_ANEMOIA_DISPONIBLE

static inline void mostrarError(TFT_eSPI &tft, const char *l1, const char *l2 = nullptr) {
  tft.endWrite();
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_RED, TFT_BLACK);
  tft.drawString(l1, 160, 105, 2);
  if (l2) {
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(l2, 160, 132, 1);
  }
  delay(1100);
}



static void esperarLiberarControlesNES() {
  while (digitalRead(PIN_UP) == LOW ||
         digitalRead(PIN_DOWN) == LOW ||
         digitalRead(PIN_LEFT) == LOW ||
         digitalRead(PIN_RIGHT) == LOW ||
         digitalRead(BUTTON_PIN) == LOW ||
         digitalRead(PIN_VOLUMEN_PANEL) == LOW ||
         digitalRead(PIN_PAUSA) == LOW) {
    delay(10);
  }
  delay(80);
}

static void rutaEstadoNES(const Cartridge *cart, char *salida, size_t tam) {
  if (!salida || tam == 0) return;
  if (!cart) {
    salida[0] = '\0';
    return;
  }
  snprintf(salida, tam, "/states/%08lX.state", (unsigned long)cart->CRC32);
}

static bool existeEstadoNES(const Cartridge *cart) {
  char rutaEstado[40];
  rutaEstadoNES(cart, rutaEstado, sizeof(rutaEstado));
  return rutaEstado[0] != '\0' && SD_MMC.exists(rutaEstado);
}

static void mensajeMenuNES(TFT_eSPI &tft,
                           const char *titulo,
                           const char *detalle = nullptr,
                           uint16_t color = TFT_GREEN) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(color, TFT_BLACK);
  tft.drawString(titulo, 160, 108, 2);

  if (detalle) {
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(detalle, 160, 135, 1);
  }

  tft.setTextDatum(TL_DATUM);
}

enum ResultadoMenuNES {
  NES_MENU_CONTINUAR = 0,
  NES_MENU_SALIR = 1
};

// Anemoia mantiene DMA/SPI configurados para enviar el framebuffer.
// Para que el menú de TFT_eSPI se vea, desmontamos DMA por completo.
// Al volver al juego lo inicializamos otra vez.
static void entrarModoMenuTFTNES(TFT_eSPI &tft) {
  // Esperar el último bloque NES y salir de la transacción larga de juego.
  // NO desmontamos DMA aquí: en ESP32-S3 desinicializar/reinicializar DMA
  // durante la pausa puede dejar la TFT sin aceptar los dibujos normales.
  tft.dmaWait();
  tft.endWrite();

  // Las primitivas del menú no necesitan el byte-swap del framebuffer NES.
  tft.setSwapBytes(false);
  delay(2);

  Serial.println("[NES][MENU] TFT lista para interfaz");
}

static void volverModoJuegoTFTNES(TFT_eSPI &tft) {
  tft.dmaWait();
  tft.endWrite();
  tft.fillScreen(TFT_BLACK);

  // El framebuffer de Anemoia vuelve a usar RGB565 con byte swap por SPI.
  tft.setSwapBytes(true);
  tft.startWrite();
  tft.setAddrWindow(32, 0, 256, 240);

  Serial.println("[NES][MENU] regreso a video NES");
}

static ResultadoMenuNES abrirMenuNES(Bus *nes,
                                     TFT_eSPI &tft,
                                     bool audioNESActivo) {
  if (!nes || !nes->cart) return NES_MENU_CONTINUAR;

  // Detener por completo DMA antes de dibujar la interfaz del menú.
  // endWrite() solo no era suficiente: el juego quedaba pausado pero
  // el último framebuffer permanecía congelado en pantalla.
  entrarModoMenuTFTNES(tft);

  // El emulador queda detenido mientras estamos aquí. Cortamos únicamente
  // la salida física para que no quede un fragmento de audio repitiéndose.
  nes->cpu.apu.setOutputEnabled(false);

  esperarLiberarControlesNES();

  static const char *opciones[] = {
    "CONTINUAR",
    "GUARDAR PARTIDA",
    "CARGAR PARTIDA",
    "SALIR DEL JUEGO"
  };
  constexpr int NUM_OPCIONES = 4;

  int seleccion = 0;
  bool arribaAnterior = false;
  bool abajoAnterior = false;
  bool aAnterior = false;
  bool bAnterior = false;

  auto dibujarMenu = [&]() {
    tft.fillScreen(TFT_BLACK);

    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString("COCODRILO NES", 160, 30, 2);

    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString("PARTIDA", 160, 50, 1);

    const bool hayEstado = existeEstadoNES(nes->cart);

    for (int i = 0; i < NUM_OPCIONES; ++i) {
      const int y = 82 + i * 31;
      const bool sel = (i == seleccion);

      if (sel) {
        tft.fillRoundRect(45, y - 11, 230, 24, 5, TFT_DARKGREY);
        tft.drawRoundRect(45, y - 11, 230, 24, 5, TFT_CYAN);
      }

      uint16_t color = sel ? TFT_CYAN : TFT_WHITE;
      if (i == 2 && !hayEstado) color = TFT_DARKGREY;

      tft.setTextColor(color, sel ? TFT_DARKGREY : TFT_BLACK);
      tft.drawString(opciones[i], 160, y, 1);
    }

    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString("A: elegir   B: volver", 160, 218, 1);
    tft.setTextDatum(TL_DATUM);
  };

  dibujarMenu();

  while (true) {
    const bool arriba = digitalRead(PIN_UP) == LOW;
    const bool abajo  = digitalRead(PIN_DOWN) == LOW;
    const bool a      = digitalRead(BUTTON_PIN) == LOW;
    const bool b      = digitalRead(PIN_VOLUMEN_PANEL) == LOW;

    if (arriba && !arribaAnterior) {
      seleccion = (seleccion + NUM_OPCIONES - 1) % NUM_OPCIONES;
      dibujarMenu();
    }

    if (abajo && !abajoAnterior) {
      seleccion = (seleccion + 1) % NUM_OPCIONES;
      dibujarMenu();
    }

    if (b && !bAnterior) {
      esperarLiberarControlesNES();

      if (audioNESActivo) {
        nes->cpu.apu.setOutputEnabled(true);
      }

      volverModoJuegoTFTNES(tft);
      return NES_MENU_CONTINUAR;
    }

    if (a && !aAnterior) {
      if (seleccion == 0) {
        esperarLiberarControlesNES();

        if (audioNESActivo) {
          nes->cpu.apu.setOutputEnabled(true);
        }

        volverModoJuegoTFTNES(tft);
        return NES_MENU_CONTINUAR;
      }

      if (seleccion == 1) {
        mensajeMenuNES(tft, "GUARDANDO...", nullptr, TFT_CYAN);
        nes->saveState();

        const bool ok = existeEstadoNES(nes->cart);
        mensajeMenuNES(
            tft,
            ok ? "PARTIDA GUARDADA" : "ERROR AL GUARDAR",
            ok ? "Estado guardado en microSD" : "No se pudo crear el archivo",
            ok ? TFT_GREEN : TFT_RED
        );
        delay(850);
        esperarLiberarControlesNES();

        if (audioNESActivo) {
          nes->cpu.apu.setOutputEnabled(true);
        }

        volverModoJuegoTFTNES(tft);
        return NES_MENU_CONTINUAR;
      }

      if (seleccion == 2) {
        if (!existeEstadoNES(nes->cart)) {
          mensajeMenuNES(tft, "SIN PARTIDA GUARDADA",
                         "Guarda primero una partida", TFT_YELLOW);
          delay(850);
          esperarLiberarControlesNES();
          dibujarMenu();
        } else {
          mensajeMenuNES(tft, "CARGANDO...", nullptr, TFT_CYAN);
          nes->loadState();
          mensajeMenuNES(tft, "PARTIDA CARGADA",
                         "Continuando desde el guardado", TFT_GREEN);
          delay(850);
          esperarLiberarControlesNES();

          if (audioNESActivo) {
            nes->cpu.apu.setOutputEnabled(true);
          }

          volverModoJuegoTFTNES(tft);
          return NES_MENU_CONTINUAR;
        }
      }

      if (seleccion == 3) {
        esperarLiberarControlesNES();

        // DMA se mantuvo inicializado durante el menú. Dejamos una
        // transacción válida para que el epílogo de ejecutar() haga la
        // limpieza final una sola vez.
        tft.setSwapBytes(true);
        tft.startWrite();
        tft.setAddrWindow(32, 0, 256, 240);
        return NES_MENU_SALIR;
      }
    }

    arribaAnterior = arriba;
    abajoAnterior = abajo;
    aAnterior = a;
    bAnterior = b;
    delay(12);
  }
}

static bool ejecutar(const char *ruta, TFT_eSPI &tft) {
  if (!ruta || !SD_MMC.exists(ruta)) {
    mostrarError(tft, "ROM NO ENCONTRADA", ruta);
    return false;
  }

  Serial.printf("[NES] heap antes: %u, stack libre: %u\n",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));

  // IMPORTANTE: objetos grandes en heap.
  Cartridge *cart = new (std::nothrow) Cartridge(ruta, ROMBackend::LRU);
  if (!cart) {
    mostrarError(tft, "SIN RAM PARA ROM");
    return false;
  }

  if (!cart->isValid()) {
    delete cart;
    mostrarError(tft, "MAPPER NO SOPORTADO");
    return false;
  }

  Bus *nes = new (std::nothrow) Bus();
  if (!nes) {
    delete cart;
    mostrarError(tft, "SIN RAM PARA NES");
    return false;
  }

  nes->connectScreen(&tft);
  nes->insertCartridge(cart);
  nes->reset();

  // El volumen NES toma una foto del volumen global al entrar.
  // 0/10 = mute, 10/10 = 100% del mezclador de Anemoia para dejar headroom.
  const uint8_t volumenSistemaNES = AudioMAX98357::getVolumeStep();
  const uint8_t volumenAPU =
      (uint8_t)((uint16_t)volumenSistemaNES * 100U / 10U);
  nes->cpu.apu.setVolume(volumenAPU);

  Serial.printf("[NES] audio APU: volumen sistema=%u/10, APU=%u%%\n",
                (unsigned)volumenSistemaNES,
                (unsigned)volumenAPU);

  Serial.printf("[NES] heap listo: %u, stack libre: %u\n",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));

  // TFT_eSPI pushPixels() recibe RGB565 en uint16_t. En ESP32-S3 debemos
  // intercambiar los bytes al enviarlos por SPI. Guardamos el estado para
  // no alterar los colores del resto de COCODRILO OS al salir del emulador.
  const bool swapBytesAnterior = tft.getSwapBytes();
  tft.setSwapBytes(true);

  tft.endWrite();
  tft.fillScreen(TFT_BLACK);
  tft.initDMA();
  tft.startWrite();
  tft.setAddrWindow(32, 0, 256, 240);

  EstadoMenu estadoMenu;
  estadoMenu.menuAnterior = digitalRead(PIN_PAUSA) == LOW;

  const bool audioNESActivo = AudioMAX98357::entrarModoNES();
  Serial.println(audioNESActivo
      ? "[NES] MAX98357A conectado al APU"
      : "[NES] APU activo, pero MAX no pudo ceder I2S");

  // El APU ya no corre en una tarea independiente. Cpu6502 lo avanza
  // exactamente a CPU/2, evitando música acelerada, carreras y overflow
  // de stack en la antigua tarea NES_APU. La salida física se habilita
  // solo si el MAX98357A cedió correctamente I2S.
  nes->cpu.apu.setOutputEnabled(audioNESActivo);
  Serial.println("[NES] APU sincronizado al reloj CPU/2 (sin task independiente)");

  uint64_t siguienteFrame = esp_timer_get_time() + FRAME_US;
  uint32_t fpsFrames = 0;
  uint64_t fpsInicio = esp_timer_get_time();

  while (true) {
    bool salir = false;
    nes->controller = leerControles(estadoMenu, salir);

    if (salir) {
      Serial.println("[NES] MENU+B mantenidos -> abrir menu");

      const ResultadoMenuNES resultadoMenu =
          abrirMenuNES(nes, tft, audioNESActivo);

      if (resultadoMenu == NES_MENU_SALIR) {
        Serial.println("[NES] salida elegida desde menu");
        break;
      }

      // El menú puede tardar varios segundos. Reiniciamos el reloj para
      // que el emulador no intente recuperar frames atrasados al volver.
      siguienteFrame = esp_timer_get_time() + FRAME_US;

      // Limpiar el detector de combos para que soltar MENU/B no genere START.
      estadoMenu = EstadoMenu{};
      estadoMenu.menuAnterior = digitalRead(PIN_PAUSA) == LOW;
      continue;
    }

    // En Anemoia Bus::clock() procesa UN FRAME completo.
    nes->clock();
    fpsFrames++;

    const uint64_t ahora = esp_timer_get_time();
    if (ahora < siguienteFrame) {
      delayMicroseconds((uint32_t)(siguienteFrame - ahora));
    }

    // Evita que una pausa/retraso grande genere una ráfaga de frames.
    const uint64_t despues = esp_timer_get_time();
    if (despues > siguienteFrame + (FRAME_US * 3ULL)) {
      siguienteFrame = despues + FRAME_US;
    } else {
      siguienteFrame += FRAME_US;
    }

    // Diagnóstico temporal: confirma que el gameplay permanezca ~60.1 FPS.
    const uint64_t fpsAhora = esp_timer_get_time();
    if ((fpsAhora - fpsInicio) >= 2000000ULL) {
      const float segundos = (float)(fpsAhora - fpsInicio) / 1000000.0f;
      const float fps = (float)fpsFrames / segundos;
      Serial.printf("[NES] FPS reales=%.2f\n", fps);
      fpsFrames = 0;
      fpsInicio = fpsAhora;
    }

    // Dejar respirar al RTOS sin añadir una pausa perceptible.
    taskYIELD();
  }

  // Esperar cualquier transferencia pendiente antes de devolver la TFT al OS.
  // NES usa DMA directo; COCODRILO OS vuelve luego a pushSprite normal.
  tft.dmaWait();
  tft.endWrite();

  // Cortar primero las escrituras del APU y luego devolver I2S a COCODRILO OS.
  nes->cpu.apu.setOutputEnabled(false);

  if (audioNESActivo) {
    AudioMAX98357::salirModoNES();
  }

  // Desacoplar DMA del bus SPI. Si lo dejamos activo, el estado lógico puede
  // regresar al menú mientras la TFT conserva el último frame del NES.
  tft.deInitDMA();

  // Restaurar el orden de bytes que usaba COCODRILO OS antes de entrar al NES.
  tft.setSwapBytes(swapBytesAnterior);

  delete nes;
  delete cart;

  // Limpieza visual explícita: la pantalla siguiente será redibujada por
  // el navegador NES / menú de COCODRILO inmediatamente después de retornar.
  tft.fillScreen(TFT_BLACK);

  Serial.printf("[NES] salida limpia, heap: %u, stack libre: %u\n",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));

  return true;
}

#else

static bool ejecutar(const char *, TFT_eSPI &) {
  Serial.println("NES: falta el nucleo Anemoia en la carpeta src/");
  return false;
}

#endif

} // namespace CocodriloNES
