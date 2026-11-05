#ifndef SISTEMA_LOGROS_H
#define SISTEMA_LOGROS_H

#include <Arduino.h>
#include <Preferences.h>

enum JuegoLogros : uint8_t {
  JUEGO_PRINCESITA = 0,
  JUEGO_ASTRONAUTA,
  JUEGO_LOGROS_TOTAL
};

enum LogroPrincesitaID : uint8_t {
  LOGRO_PRIMER_VUELO = 0, LOGRO_NOCHE_DESPIERTA, LOGRO_ESPIRITU_BOSQUE,
  LOGRO_REINO_CONGELADO, LOGRO_RESCATE_REAL, LOGRO_HIJO_NOCHE,
  LOGRO_PLUMAS_NIEVE, LOGRO_VUELO_INTERMINABLE, LOGRO_MAESTRO_VUELO,
  LOGRO_PRINCESITA_TOTAL
};

enum LogroAstronautaID : uint8_t {
  PQ_LOGRO_PRIMER_ALUNIZAJE = 0, PQ_LOGRO_PILOTO_NOVATO,
  PQ_LOGRO_EXPLORADOR_ESPACIAL, PQ_LOGRO_CORTO_GRAN_SALTO,
  PQ_LOGRO_ASTRONAUTA_LEGENDARIO, PQ_LOGRO_ATERRIZAJE_PERFECTO,
  PQ_LOGRO_SALTO_DE_FE, PQ_LOGRO_SIN_UNA_GOTA, PQ_LOGRO_EN_RESERVA,
  PQ_LOGRO_TOTAL
};

struct DefinicionLogro {
  const char* nombre;
  const char* descripcion;
  bool secreto;
};

static const DefinicionLogro LOGROS_PRINCESITA[LOGRO_PRINCESITA_TOTAL] = {
  {"Explorador Celeste", "Juega tu primera partida", false},
  {"Vuelo de dia", "Llega a 15 en el mapa Dia", false},
  {"Explorador Boscoso", "Llega a 25 en el mapa Dia", false},
  {"Explorador Rubi", "Llega a 35 en el mapa Dia", false},
  {"Princesita Real", "Rescatala en el nivel 50 de Dia", false},
  {"Explorador Eclipse", "Llega a 25 en el mapa Noche", false},
  {"Explorador Copito", "Llega a 25 en el mapa Nieve", false},
  {"Vuelo interminable", "Llega a 50 en un mapa infinito", false},
  {"Explorador Amatista", "Llega a 100 en un mapa infinito", false}
};

static const DefinicionLogro LOGROS_ASTRONAUTA[PQ_LOGRO_TOTAL] = {
  {"Primer alunizaje", "Aterriza en una plataforma nueva", false},
  {"Mapa Noche", "Encadena 15 aterrizajes", false},
  {"Mapa Nieve", "Encadena 20 aterrizajes", false},
  {"Corto gran salto", "Encadena 25 aterrizajes", false},
  {"Secreto legendario", "Encadena 30 aterrizajes - Recompensa: ???", true},
  {"Aterrizaje perfecto", "Aterriza con velocidad muy baja", false},
  {"Salto de fe", "Saltate la siguiente plataforma", false},
  {"Mapa Bosque Magico", "Aterriza vivo con cero combustible", false},
  {"En reserva", "Aterriza con 20% o menos", false}
};

class SistemaLogros {
 public:
  void begin() {
    Preferences p;
    p.begin("logros_arcade", false);
    bits_[JUEGO_PRINCESITA] = p.getUInt("prin", 0);
    bits_[JUEGO_ASTRONAUTA] = p.getUInt("astro", 0);
    // Eliminar el registro legado de Hechicera en consolas ya instaladas,
    // conservando sin cambios los logros de los otros dos juegos.
    if (p.isKey("hechi")) p.remove("hechi");
    p.end();
  }

  bool desbloquear(uint8_t juego, uint8_t id) {
    if (juego >= JUEGO_LOGROS_TOTAL || id >= total(juego)) return false;
    const uint32_t mask = 1UL << id;
    if (bits_[juego] & mask) return false;
    bits_[juego] |= mask;
    pendientes_[juego] |= mask;
    guardar(juego);
    return true;
  }

  bool obtenido(uint8_t juego, uint8_t id) const {
    return juego < JUEGO_LOGROS_TOTAL && id < total(juego) && (bits_[juego] & (1UL << id));
  }

  uint8_t contar(uint8_t juego) const {
    uint8_t n = 0;
    for (uint8_t i = 0; i < total(juego); ++i) if (obtenido(juego, i)) ++n;
    return n;
  }

  bool hayPendientes(uint8_t juego) const {
    return juego < JUEGO_LOGROS_TOTAL && pendientes_[juego] != 0;
  }

  uint8_t tomarPendiente(uint8_t juego) {
    if (juego >= JUEGO_LOGROS_TOTAL) return 0;
    for (uint8_t i = 0; i < total(juego); ++i) {
      const uint32_t mask = 1UL << i;
      if (pendientes_[juego] & mask) {
        pendientes_[juego] &= ~mask;
        return i;
      }
    }
    return 0;
  }

  uint8_t total(uint8_t juego) const {
    switch (juego) {
      case JUEGO_PRINCESITA: return LOGRO_PRINCESITA_TOTAL;
      case JUEGO_ASTRONAUTA: return PQ_LOGRO_TOTAL;
      default: return 0;
    }
  }

  const DefinicionLogro& definicion(uint8_t juego, uint8_t id) const {
    switch (juego) {
      case JUEGO_ASTRONAUTA: return LOGROS_ASTRONAUTA[id];
      default: return LOGROS_PRINCESITA[id];
    }
  }

  void restablecer() {
    for (uint8_t i = 0; i < JUEGO_LOGROS_TOTAL; ++i) {
      bits_[i] = 0;
      pendientes_[i] = 0;
    }
    Preferences p;
    p.begin("logros_arcade", false);
    p.clear();
    p.end();
  }

  const char* nombreJuego(uint8_t juego) const {
    switch (juego) {
      case JUEGO_ASTRONAUTA: return "ASTRONAUTA";
      default: return "PRINCESITA";
    }
  }

 private:
  uint32_t bits_[JUEGO_LOGROS_TOTAL] = {0,0};
  uint32_t pendientes_[JUEGO_LOGROS_TOTAL] = {0,0};

  void guardar(uint8_t juego) {
    static const char* keys[JUEGO_LOGROS_TOTAL] = {"prin", "astro"};
    Preferences p;
    p.begin("logros_arcade", false);
    p.putUInt(keys[juego], bits_[juego]);
    p.end();
  }
};

#endif
