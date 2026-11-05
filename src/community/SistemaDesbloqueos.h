#ifndef SISTEMA_DESBLOQUEOS_H
#define SISTEMA_DESBLOQUEOS_H

#include <Arduino.h>
#include <Preferences.h>
#include "SistemaLogros.h"

// Prueba temporal: permite seleccionar el premio secreto sin obtener el logro.
// Cambia a 0 antes de la version final.
#define PRUEBA_AVENTURERO_ESTELAR 0
#define PRUEBA_PERSONAJES_PRINCESITA 0
#define PRUEBA_PRINCESITA_REAL 0

enum RecompensaDesbloqueoID : uint8_t {
  RECOMPENSA_MAPA_NOCHE = 0,
  RECOMPENSA_MAPA_NIEVE,
  RECOMPENSA_MAPA_BOSQUE,
  RECOMPENSA_PERSONAJE_SECRETO,
  RECOMPENSA_TOTAL
};

enum MapaPrincesitaID : uint8_t {
  MAPA_DIA = 0,
  MAPA_NOCHE,
  MAPA_BOSQUE,
  MAPA_NIEVE,
  MAPA_PRINCESITA_TOTAL
};

enum PersonajePrincesitaID : uint8_t {
  PERSONAJE_AVENTURERO_SOLECITO = 0,
  PERSONAJE_AVENTURERO_CELESTE,
  PERSONAJE_AVENTURERO_BOSCOSO,
  PERSONAJE_AVENTURERO_RUBI,
  PERSONAJE_AVENTURERO_AMATISTA,
  PERSONAJE_AVENTURERO_ECLIPSE,
  PERSONAJE_AVENTURERO_COPITO,
  PERSONAJE_PRINCESITA_REAL,
  PERSONAJE_AVENTURERO_ESTELAR,
  PERSONAJE_PRINCESITA_TOTAL
};

class SistemaDesbloqueos {
 public:
  bool mapaDisponible(uint8_t mapa, const SistemaLogros& logros) const {
    switch (mapa) {
      case MAPA_DIA: return true;
      case MAPA_NOCHE:
        return logros.obtenido(JUEGO_ASTRONAUTA, PQ_LOGRO_PILOTO_NOVATO);
      case MAPA_NIEVE:
        return logros.obtenido(JUEGO_ASTRONAUTA, PQ_LOGRO_EXPLORADOR_ESPACIAL);
      case MAPA_BOSQUE:
        return logros.obtenido(JUEGO_ASTRONAUTA, PQ_LOGRO_SIN_UNA_GOTA);
      default: return false;
    }
  }

  bool personajeDisponible(uint8_t personaje, const SistemaLogros& logros) const {
    switch (personaje) {
      case PERSONAJE_AVENTURERO_SOLECITO: return true;
      case PERSONAJE_AVENTURERO_CELESTE:
        return PRUEBA_PERSONAJES_PRINCESITA || logros.obtenido(JUEGO_PRINCESITA, LOGRO_PRIMER_VUELO);
      case PERSONAJE_AVENTURERO_BOSCOSO:
        return PRUEBA_PERSONAJES_PRINCESITA || logros.obtenido(JUEGO_PRINCESITA, LOGRO_ESPIRITU_BOSQUE);
      case PERSONAJE_AVENTURERO_RUBI:
        return PRUEBA_PERSONAJES_PRINCESITA || logros.obtenido(JUEGO_PRINCESITA, LOGRO_REINO_CONGELADO);
      case PERSONAJE_AVENTURERO_AMATISTA:
        return PRUEBA_PERSONAJES_PRINCESITA || logros.obtenido(JUEGO_PRINCESITA, LOGRO_MAESTRO_VUELO);
      case PERSONAJE_AVENTURERO_ECLIPSE:
        return PRUEBA_PERSONAJES_PRINCESITA || logros.obtenido(JUEGO_PRINCESITA, LOGRO_HIJO_NOCHE);
      case PERSONAJE_AVENTURERO_COPITO:
        return PRUEBA_PERSONAJES_PRINCESITA || logros.obtenido(JUEGO_PRINCESITA, LOGRO_PLUMAS_NIEVE);
      case PERSONAJE_PRINCESITA_REAL:
        return PRUEBA_PRINCESITA_REAL || logros.obtenido(JUEGO_PRINCESITA, LOGRO_RESCATE_REAL);
      case PERSONAJE_AVENTURERO_ESTELAR:
        return PRUEBA_AVENTURERO_ESTELAR ||
               logros.obtenido(JUEGO_ASTRONAUTA, PQ_LOGRO_ASTRONAUTA_LEGENDARIO);
      default: return false;
    }
  }

  void begin() {
    Preferences p;
    p.begin("desbloq_arcade", true);
    recompensasPendientes_ = p.getUChar("pend", 0);
    p.end();
  }

  void registrarLogroAstronauta(uint8_t logro) {
    int8_t recompensa = -1;
    switch (logro) {
      case PQ_LOGRO_PILOTO_NOVATO: recompensa = RECOMPENSA_MAPA_NOCHE; break;
      case PQ_LOGRO_EXPLORADOR_ESPACIAL: recompensa = RECOMPENSA_MAPA_NIEVE; break;
      case PQ_LOGRO_SIN_UNA_GOTA: recompensa = RECOMPENSA_MAPA_BOSQUE; break;
      case PQ_LOGRO_ASTRONAUTA_LEGENDARIO: recompensa = RECOMPENSA_PERSONAJE_SECRETO; break;
      default: return;
    }
    recompensasPendientes_ |= (uint8_t)(1U << recompensa);
    guardarPendientes();
  }

  bool hayRecompensasPendientes() const { return recompensasPendientes_ != 0; }

  uint8_t tomarRecompensaPendiente() {
    for (uint8_t i = 0; i < RECOMPENSA_TOTAL; ++i) {
      const uint8_t mask = (uint8_t)(1U << i);
      if (recompensasPendientes_ & mask) {
        recompensasPendientes_ &= (uint8_t)~mask;
        guardarPendientes();
        return i;
      }
    }
    return RECOMPENSA_TOTAL;
  }

  const char* tituloRecompensa(uint8_t recompensa) const {
    switch (recompensa) {
      case RECOMPENSA_MAPA_NOCHE: return "MAPA NOCHE";
      case RECOMPENSA_MAPA_NIEVE: return "MAPA NIEVE";
      case RECOMPENSA_MAPA_BOSQUE: return "BOSQUE MAGICO";
      case RECOMPENSA_PERSONAJE_SECRETO: return "PERSONAJE SECRETO";
      default: return "RECOMPENSA";
    }
  }

  const char* detalleRecompensa(uint8_t recompensa) const {
    switch (recompensa) {
      case RECOMPENSA_MAPA_NOCHE:
      case RECOMPENSA_MAPA_NIEVE:
      case RECOMPENSA_MAPA_BOSQUE: return "Nuevo mapa en Princesita";
      case RECOMPENSA_PERSONAJE_SECRETO: return "Descubrelo en Princesita";
      default: return "";
    }
  }

  void restablecer() {
    recompensasPendientes_ = 0;
    Preferences p;
    p.begin("desbloq_arcade", false);
    p.clear();
    p.end();
  }

  uint8_t siguienteMapaDisponible(uint8_t actual, int8_t direccion,
                                  const SistemaLogros& logros) const {
    for (uint8_t intento = 0; intento < MAPA_PRINCESITA_TOTAL; ++intento) {
      actual = (actual + (direccion < 0 ? MAPA_PRINCESITA_TOTAL - 1 : 1))
               % MAPA_PRINCESITA_TOTAL;
      if (mapaDisponible(actual, logros)) return actual;
    }
    return MAPA_DIA;
  }

  uint8_t siguientePersonajeDisponible(uint8_t actual, int8_t direccion,
                                       const SistemaLogros& logros) const {
    for (uint8_t intento = 0; intento < PERSONAJE_PRINCESITA_TOTAL; ++intento) {
      actual = (actual + (direccion < 0 ? PERSONAJE_PRINCESITA_TOTAL - 1 : 1))
               % PERSONAJE_PRINCESITA_TOTAL;
      if (personajeDisponible(actual, logros)) return actual;
    }
    return PERSONAJE_AVENTURERO_SOLECITO;
  }

 private:
  uint8_t recompensasPendientes_ = 0;

  void guardarPendientes() {
    Preferences p;
    p.begin("desbloq_arcade", false);
    p.putUChar("pend", recompensasPendientes_);
    p.end();
  }
};

#endif
