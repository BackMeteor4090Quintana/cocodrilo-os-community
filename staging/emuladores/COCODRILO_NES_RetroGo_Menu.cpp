#include "COCODRILO_NES_RetroGo_Menu.h"

#include "SD_MMC.h"
#include "AudioMAX98357.h"
#include "CocodriloVolumeControl.h"
#include "BacklightControl.h"

extern "C" {
#include <nes/nes.h>
#include <nes/state.h>
}

namespace CocodriloNESMenu {

static int slotEstado = 1;

static void esperarSoltar(const Pines &p) {
  while (digitalRead(p.up) == LOW ||
         digitalRead(p.down) == LOW ||
         digitalRead(p.left) == LOW ||
         digitalRead(p.right) == LOW ||
         digitalRead(p.a) == LOW ||
         digitalRead(p.b) == LOW ||
         digitalRead(p.menu) == LOW) {
    delay(10);
  }
  delay(70);
}

static void rutasEstado(uint32_t checksum,
                        int slot,
                        char *rutaPosix,
                        size_t nPosix,
                        char *rutaSD,
                        size_t nSD,
                        char *rutaPPU = nullptr,
                        size_t nPPU = 0) {
  if (rutaPosix && nPosix) {
    snprintf(rutaPosix, nPosix,
             "/sdcard/states/%08lX.slot%d.state",
             (unsigned long)checksum,
             slot);
  }

  if (rutaSD && nSD) {
    snprintf(rutaSD, nSD,
             "/states/%08lX.slot%d.state",
             (unsigned long)checksum,
             slot);
  }

  if (rutaPPU && nPPU) {
    snprintf(rutaPPU, nPPU,
             "/sdcard/states/%08lX.slot%d.cocoppu",
             (unsigned long)checksum,
             slot);
  }
}


struct __attribute__((packed)) EstadoCoreCocodrilo {
  uint32_t magic;
  uint16_t version;
  uint16_t size;

  // PPU interno que SNSS no conserva completo.
  int32_t ppu_vaddr;
  int32_t ppu_vaddr_latch;
  int32_t ppu_vaddr_inc;
  uint8_t ppu_ctrl0;
  uint8_t ppu_ctrl1;
  uint8_t ppu_stat;
  uint8_t ppu_oam_addr;
  uint8_t ppu_nametab_base;
  uint8_t ppu_latch;
  uint8_t ppu_vdata_latch;
  uint8_t ppu_tile_xofs;
  uint8_t ppu_flipflop;
  int32_t ppu_obj_height;
  int32_t ppu_obj_base;
  int32_t ppu_bg_base;
  uint8_t ppu_left_bg_on;
  uint8_t ppu_left_obj_on;
  uint8_t ppu_bg_on;
  uint8_t ppu_obj_on;
  uint8_t ppu_vram_accessible;
  uint8_t ppu_vram_present;
  uint8_t reserved0[2];

  // Temporización global del frame.
  int32_t nes_scanline;
  int32_t nes_cycles;

  // Estado dinámico 6502 omitido por SNSS.
  uint8_t cpu_int_pending;
  uint8_t cpu_jammed;
  uint8_t reserved1[2];
  int32_t cpu_total_cycles;
  int32_t cpu_burn_cycles;

  // Estado dinámico del APU. SNSS sólo vuelve a escribir registros.
  rectangle_t apu_rectangle[2];
  triangle_t apu_triangle;
  noise_t apu_noise;
  dmc_t apu_dmc;
  uint8_t apu_control_reg;
  uint8_t reserved2[3];
  int32_t apu_prev_sample;
  uint32_t apu_fc_state;
  uint32_t apu_fc_step;
  uint32_t apu_fc_cycles;
  uint8_t apu_fc_irq_occurred;
  uint8_t apu_fc_disable_irq;
  uint8_t reserved3[2];
};

static constexpr uint32_t PPU_COCO_MAGIC = 0x434F434F; // "COCO"
static constexpr uint16_t PPU_COCO_VERSION = 2;

struct EstadoVivoCocodrilo {
  ppu_t ppu;
  nes6502_t cpu;
  apu_t apu;
  mem_t mem;
  uint8_t mapper_state[512];
  bool mapper_state_valido;
  int32_t nes_scanline;
  int32_t nes_cycles;
};

static void capturarEstadoVivo(EstadoVivoCocodrilo &v) {
  nes_t *machine = nes_getptr();
  v.ppu = *machine->ppu;
  v.cpu = *machine->cpu;
  v.apu = *machine->apu;
  v.mem = *machine->mem;
  memset(v.mapper_state, 0, sizeof(v.mapper_state));
  v.mapper_state_valido = machine->mapper && machine->mapper->get_state;
  if (v.mapper_state_valido) {
    machine->mapper->get_state(v.mapper_state);
  }
  v.nes_scanline = machine->scanline;
  v.nes_cycles = machine->cycles;
}

static void restaurarEstadoVivo(const EstadoVivoCocodrilo &v) {
  nes_t *machine = nes_getptr();
  *machine->ppu = v.ppu;
  *machine->cpu = v.cpu;
  *machine->apu = v.apu;
  *machine->mem = v.mem;

  // Reafirmar el estado privado del mapper después de restaurar
  // los mapas CPU/PPU. Algunos mappers guardan registros en static.
  if (v.mapper_state_valido &&
      machine->mapper &&
      machine->mapper->set_state) {
    machine->mapper->set_state((uint8_t*)v.mapper_state);

    // set_state() de algunos mappers sólo restaura sus registros internos
    // y puede tocar parcialmente el banking. Dejamos exactamente los
    // punteros de memoria que estaban activos al entrar al menú.
    *machine->ppu = v.ppu;
    *machine->mem = v.mem;
  }

  machine->scanline = v.nes_scanline;
  machine->cycles = v.nes_cycles;
}

static bool guardarEstadoPPUDesdeSnapshot(uint32_t checksum,
                                          int slot,
                                          const EstadoVivoCocodrilo &v) {
  char rutaPPU[128];
  rutasEstado(checksum, slot, nullptr, 0, nullptr, 0,
              rutaPPU, sizeof(rutaPPU));

  EstadoCoreCocodrilo e = {};
  e.magic = PPU_COCO_MAGIC;
  e.version = PPU_COCO_VERSION;
  e.size = sizeof(e);

  const ppu_t *p = &v.ppu;
  e.ppu_vaddr = p->vaddr;
  e.ppu_vaddr_latch = p->vaddr_latch;
  e.ppu_vaddr_inc = p->vaddr_inc;
  e.ppu_ctrl0 = p->ctrl0;
  e.ppu_ctrl1 = p->ctrl1;
  e.ppu_stat = p->stat;
  e.ppu_oam_addr = p->oam_addr;
  e.ppu_nametab_base = p->nametab_base;
  e.ppu_latch = p->latch;
  e.ppu_vdata_latch = p->vdata_latch;
  e.ppu_tile_xofs = p->tile_xofs;
  e.ppu_flipflop = p->flipflop;
  e.ppu_obj_height = p->obj_height;
  e.ppu_obj_base = p->obj_base;
  e.ppu_bg_base = p->bg_base;
  e.ppu_left_bg_on = p->left_bg_on;
  e.ppu_left_obj_on = p->left_obj_on;
  e.ppu_bg_on = p->bg_on;
  e.ppu_obj_on = p->obj_on;
  e.ppu_vram_accessible = p->vram_accessible;
  e.ppu_vram_present = p->vram_present;

  e.nes_scanline = v.nes_scanline;
  e.nes_cycles = v.nes_cycles;

  e.cpu_int_pending = v.cpu.int_pending;
  e.cpu_jammed = v.cpu.jammed;
  e.cpu_total_cycles = (int32_t)v.cpu.total_cycles;
  e.cpu_burn_cycles = (int32_t)v.cpu.burn_cycles;

  e.apu_rectangle[0] = v.apu.rectangle[0];
  e.apu_rectangle[1] = v.apu.rectangle[1];
  e.apu_triangle = v.apu.triangle;
  e.apu_noise = v.apu.noise;
  e.apu_dmc = v.apu.dmc;
  e.apu_control_reg = v.apu.control_reg;
  e.apu_prev_sample = v.apu.prev_sample;
  e.apu_fc_state = v.apu.fc.state;
  e.apu_fc_step = v.apu.fc.step;
  e.apu_fc_cycles = v.apu.fc.cycles;
  e.apu_fc_irq_occurred = v.apu.fc.irq_occurred;
  e.apu_fc_disable_irq = v.apu.fc.disable_irq;

  FILE *fp = fopen(rutaPPU, "wb");
  if (!fp) return false;
  const bool ok = fwrite(&e, sizeof(e), 1, fp) == 1;
  fclose(fp);
  return ok;
}

static bool cargarEstadoPPU(uint32_t checksum, int slot) {
  nes_t *machine = nes_getptr();
  if (!machine || !machine->ppu || !machine->cpu || !machine->apu) return false;

  char rutaPPU[128];
  rutasEstado(checksum, slot, nullptr, 0, nullptr, 0,
              rutaPPU, sizeof(rutaPPU));

  FILE *fp = fopen(rutaPPU, "rb");
  if (!fp) return false;

  EstadoCoreCocodrilo e = {};
  const bool leido = fread(&e, sizeof(e), 1, fp) == 1;
  fclose(fp);

  if (!leido ||
      e.magic != PPU_COCO_MAGIC ||
      e.version != PPU_COCO_VERSION ||
      e.size != sizeof(e)) {
    return false;
  }

  ppu_t *p = machine->ppu;
  p->vaddr = e.ppu_vaddr;
  p->vaddr_latch = e.ppu_vaddr_latch;
  p->vaddr_inc = e.ppu_vaddr_inc;
  p->ctrl0 = e.ppu_ctrl0;
  p->ctrl1 = e.ppu_ctrl1;
  p->stat = e.ppu_stat;
  p->oam_addr = e.ppu_oam_addr;
  p->nametab_base = e.ppu_nametab_base;
  p->latch = e.ppu_latch;
  p->vdata_latch = e.ppu_vdata_latch;
  p->tile_xofs = e.ppu_tile_xofs;
  p->flipflop = e.ppu_flipflop;
  p->obj_height = e.ppu_obj_height;
  p->obj_base = e.ppu_obj_base;
  p->bg_base = e.ppu_bg_base;
  p->left_bg_on = e.ppu_left_bg_on;
  p->left_obj_on = e.ppu_left_obj_on;
  p->bg_on = e.ppu_bg_on;
  p->obj_on = e.ppu_obj_on;
  p->vram_accessible = e.ppu_vram_accessible;
  p->vram_present = e.ppu_vram_present;

  // Se recalcula en el frame reanudado.
  p->strikeflag = false;
  p->strike_cycle = (uint32)-1;

  machine->scanline = e.nes_scanline;
  machine->cycles = e.nes_cycles;

  machine->cpu->int_pending = e.cpu_int_pending;
  machine->cpu->jammed = e.cpu_jammed;
  machine->cpu->total_cycles = e.cpu_total_cycles;
  machine->cpu->burn_cycles = e.cpu_burn_cycles;

  machine->apu->rectangle[0] = e.apu_rectangle[0];
  machine->apu->rectangle[1] = e.apu_rectangle[1];
  machine->apu->triangle = e.apu_triangle;
  machine->apu->noise = e.apu_noise;
  machine->apu->dmc = e.apu_dmc;
  machine->apu->control_reg = e.apu_control_reg;
  machine->apu->prev_sample = e.apu_prev_sample;
  machine->apu->fc.state = e.apu_fc_state;
  machine->apu->fc.step = e.apu_fc_step;
  machine->apu->fc.cycles = e.apu_fc_cycles;
  machine->apu->fc.irq_occurred = e.apu_fc_irq_occurred;
  machine->apu->fc.disable_irq = e.apu_fc_disable_irq;

  return true;
}

static bool tieneEstadoPPU(uint32_t checksum, int slot) {
  char rutaPPU[128];
  rutasEstado(checksum, slot, nullptr, 0, nullptr, 0,
              rutaPPU, sizeof(rutaPPU));

  FILE *fp = fopen(rutaPPU, "rb");
  if (!fp) return false;

  EstadoCoreCocodrilo e = {};
  const bool leido = fread(&e, sizeof(e), 1, fp) == 1;
  fclose(fp);

  return leido &&
         e.magic == PPU_COCO_MAGIC &&
         e.version == PPU_COCO_VERSION &&
         e.size == sizeof(e);
}

static bool existeEstado(uint32_t checksum, int slot) {
  char rutaSD[96];
  rutasEstado(checksum, slot, nullptr, 0, rutaSD, sizeof(rutaSD));
  return SD_MMC.exists(rutaSD);
}

static bool guardarEstado(uint32_t checksum, int slot) {
  if (!SD_MMC.exists("/states")) {
    SD_MMC.mkdir("/states");
  }

  nes_t *machine = nes_getptr();
  if (!machine || !machine->ppu || !machine->cpu || !machine->apu) return false;

  // state_save() no debe poder alterar el juego en ejecución.
  // Capturamos todo el estado dinámico antes de entrar al serializer nativo.
  static EstadoVivoCocodrilo vivo;
  capturarEstadoVivo(vivo);

  Serial.printf("[NOFRENDO] save mapper=%d %s get=%s set=%s\n",
                machine->mapper ? machine->mapper->number : -1,
                (machine->mapper && machine->mapper->name) ? machine->mapper->name : "?",
                (machine->mapper && machine->mapper->get_state) ? "si" : "no",
                (machine->mapper && machine->mapper->set_state) ? "si" : "no");

  char rutaPosix[112];
  rutasEstado(checksum, slot, rutaPosix, sizeof(rutaPosix), nullptr, 0);

  const int rc = state_save(rutaPosix);

  // Restaurar SIEMPRE, incluso si falla la escritura.
  restaurarEstadoVivo(vivo);

  if (rc != 0) return false;

  // El sidecar representa el instante anterior al serializer nativo.
  return guardarEstadoPPUDesdeSnapshot(checksum, slot, vivo);
}

static bool cargarEstado(uint32_t checksum, int slot) {
  char rutaPosix[112];
  rutasEstado(checksum, slot, rutaPosix, sizeof(rutaPosix), nullptr, 0);
  if (state_load(rutaPosix) != 0) return false;
  return cargarEstadoPPU(checksum, slot);
}

static void textoConSombra(TFT_eSPI &tft,
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

static void mensaje(TFT_eSPI &tft,
                    RedibujarFondoFn redibujarFondo,
                    const char *titulo,
                    const char *detalle = nullptr,
                    uint16_t color = TFT_GREEN) {
  if (redibujarFondo) redibujarFondo(tft);

  tft.setTextDatum(MC_DATUM);
  textoConSombra(tft, titulo, 160, 104, 2, color);

  if (detalle) {
    textoConSombra(tft, detalle, 160, 132, 1, TFT_WHITE);
  }

  tft.setTextDatum(TL_DATUM);
}

Accion abrir(TFT_eSPI &tft,
             const Pines &p,
             uint32_t romChecksum,
             RedibujarFondoFn redibujarFondo) {
  esperarSoltar(p);

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

  int seleccion = 0;
  bool confirmando = false;
  bool confirmarSi = false;
  int accionPendiente = -1;

  bool prevUp = false;
  bool prevDown = false;
  bool prevLeft = false;
  bool prevRight = false;
  bool prevA = false;
  bool prevB = false;

  auto dibujar = [&]() {
    if (redibujarFondo) redibujarFondo(tft);

    const bool hayEstado = existeEstado(romChecksum, slotEstado);
    const bool estadoCompleto =
        hayEstado && tieneEstadoPPU(romChecksum, slotEstado);

    tft.setTextDatum(MC_DATUM);

    textoConSombra(tft, "COCODRILO NES", 160, 18, 2, TFT_CYAN);

    char slotTxt[32];
    snprintf(slotTxt, sizeof(slotTxt),
             "SLOT %d  %s",
             slotEstado,
             !hayEstado ? "VACIO" :
             (estadoCompleto ? "OK" : "LEGACY"));
    textoConSombra(tft, slotTxt, 160, 40, 1, TFT_LIGHTGREY);

    for (int i = 0; i < NUM_ITEMS; ++i) {
      const int y = 62 + i * 21;
      char linea[40];

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
      if (i == 2 && !hayEstado) color = TFT_DARKGREY;

      textoConSombra(tft, linea, 160, y, 1, color);
    }

    if (confirmando && accionPendiente >= 0) {
      char pregunta[54];
      snprintf(pregunta, sizeof(pregunta), "%s?", items[accionPendiente]);
      textoConSombra(tft, pregunta, 120, 218, 1, TFT_YELLOW);
      textoConSombra(tft, "SI", 205, 218, 1,
                     confirmarSi ? TFT_CYAN : TFT_WHITE);
      textoConSombra(tft, "NO", 242, 218, 1,
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
      textoConSombra(tft, ayuda, 160, 218, 1, TFT_LIGHTGREY);
    }

    tft.setTextDatum(TL_DATUM);
  };

  auto ejecutarConfirmado = [&](int opcion) -> int {
    if (opcion == 0) {
      esperarSoltar(p);
      return (int)CONTINUAR;
    }

    if (opcion == 1) {
      mensaje(tft, redibujarFondo, "GUARDANDO...", nullptr, TFT_CYAN);
      const bool ok = guardarEstado(romChecksum, slotEstado);

      mensaje(tft,
              redibujarFondo,
              ok ? "ESTADO GUARDADO" : "ERROR AL GUARDAR",
              ok ? "Guardado en microSD" : "No se pudo crear el archivo",
              ok ? TFT_GREEN : TFT_RED);

      delay(650);
      esperarSoltar(p);
      return -1;
    }

    if (opcion == 2) {
      if (!existeEstado(romChecksum, slotEstado)) {
        mensaje(tft, redibujarFondo,
                "SLOT VACIO",
                "Elige otro slot o guarda primero",
                TFT_YELLOW);
        delay(650);
        esperarSoltar(p);
        return -1;
      }

      if (!tieneEstadoPPU(romChecksum, slotEstado)) {
        mensaje(tft, redibujarFondo,
                "GUARDADO LEGACY",
                "Vuelve a guardar este slot",
                TFT_YELLOW);
        delay(900);
        esperarSoltar(p);
        return -1;
      }

      mensaje(tft, redibujarFondo, "CARGANDO...", nullptr, TFT_CYAN);
      const bool ok = cargarEstado(romChecksum, slotEstado);

      if (ok) {
        esperarSoltar(p);
        return (int)CARGADO;
      }

      mensaje(tft, redibujarFondo, "ERROR AL CARGAR", nullptr, TFT_RED);
      delay(650);
      esperarSoltar(p);
      return -1;
    }

    if (opcion == 5) {
      nes_reset(true);
      esperarSoltar(p);
      return (int)REINICIAR;
    }

    if (opcion == 6) {
      esperarSoltar(p);
      return (int)SALIR;
    }

    return -1;
  };

  dibujar();

  for (;;) {
    const bool up = digitalRead(p.up) == LOW;
    const bool down = digitalRead(p.down) == LOW;
    const bool left = digitalRead(p.left) == LOW;
    const bool right = digitalRead(p.right) == LOW;
    const bool a = digitalRead(p.a) == LOW;
    const bool b = digitalRead(p.b) == LOW;

    if (confirmando) {
      if ((left && !prevLeft) || (right && !prevRight)) {
        confirmarSi = !confirmarSi;
        dibujar();
      }

      if (b && !prevB) {
        confirmando = false;
        accionPendiente = -1;
        confirmarSi = false;
        dibujar();
      }

      if (a && !prevA) {
        if (!confirmarSi) {
          confirmando = false;
          accionPendiente = -1;
          dibujar();
        } else {
          const int resultado = ejecutarConfirmado(accionPendiente);
          if (resultado >= 0) return (Accion)resultado;

          confirmando = false;
          accionPendiente = -1;
          confirmarSi = false;
          dibujar();
        }
      }
    } else {
      if (up && !prevUp) {
        seleccion = (seleccion + NUM_ITEMS - 1) % NUM_ITEMS;
        dibujar();
      }

      if (down && !prevDown) {
        seleccion = (seleccion + 1) % NUM_ITEMS;
        dibujar();
      }

      if ((left && !prevLeft) || (right && !prevRight)) {
        if (seleccion == ITEM_VOLUMEN) {
          uint8_t vol = CocodriloVolumeControl::get();
          if (left && vol > 0) --vol;
          if (right && vol < 10) ++vol;
          CocodriloVolumeControl::set(vol);
          dibujar();
        } else if (seleccion == ITEM_BRILLO) {
          int brillo = BacklightControl::actual();
          if (left && brillo > 1) --brillo;
          if (right && brillo < 10) ++brillo;
          BacklightControl::set(brillo);
          dibujar();
        } else if (seleccion == 1 || seleccion == 2) {
          if (left) {
            slotEstado = slotEstado > 1 ? slotEstado - 1 : 3;
          } else {
            slotEstado = slotEstado < 3 ? slotEstado + 1 : 1;
          }
          dibujar();
        }
      }

      if (b && !prevB) {
        esperarSoltar(p);
        return CONTINUAR;
      }

      if (a && !prevA) {
        if (seleccion == ITEM_VOLUMEN || seleccion == ITEM_BRILLO) {
          // Volumen y brillo se modifican con izquierda/derecha y son inmediatos.
        } else if (seleccion == 2 &&
                   !existeEstado(romChecksum, slotEstado)) {
          mensaje(tft, redibujarFondo,
                  "SLOT VACIO",
                  "Elige otro slot o guarda primero",
                  TFT_YELLOW);
          delay(650);
          esperarSoltar(p);
          dibujar();
        } else {
          accionPendiente = seleccion;
          confirmarSi = false; // NO por defecto para evitar dobles pulsaciones.
          confirmando = true;
          dibujar();
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

} // namespace CocodriloNESMenu
