#ifndef GESTION_SD_H
#define GESTION_SD_H

#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>

namespace GestionSD {

struct Informacion {
  bool montada;
  uint8_t tipo;
  uint64_t capacidad;
  uint64_t usado;
  uint64_t libre;
};

static bool montada_ = false;

inline void iniciar(bool montadaInicial) {
  montada_ = montadaInicial && SD_MMC.cardType() != CARD_NONE;
}

inline bool estaMontada() {
  return montada_ && SD_MMC.cardType() != CARD_NONE;
}

inline const char* nombreTipo(uint8_t tipo) {
  switch (tipo) {
    case CARD_MMC:  return "MMC";
    case CARD_SD:   return "SDSC";
    case CARD_SDHC: return "SDHC/SDXC";
    default:        return "Desconocida";
  }
}

inline Informacion obtenerInformacion() {
  Informacion info{};
  info.montada = estaMontada();
  if (!info.montada) return info;

  info.tipo = SD_MMC.cardType();
  info.capacidad = SD_MMC.cardSize();
  info.usado = SD_MMC.usedBytes();
  info.libre = info.capacidad > info.usado ? info.capacidad - info.usado : 0;
  return info;
}

inline bool crearCarpetaSiFalta(const char* ruta) {
  if (!estaMontada()) return false;
  if (SD_MMC.exists(ruta)) return true;
  return SD_MMC.mkdir(ruta);
}

inline bool prepararCarpetas() {
  if (!estaMontada()) return false;

  bool ok = true;
  ok &= crearCarpetaSiFalta("/video");
  ok &= crearCarpetaSiFalta("/images");
  ok &= crearCarpetaSiFalta("/wallpapers");
  ok &= crearCarpetaSiFalta("/update");
  return ok;
}

inline bool expulsar() {
  if (!estaMontada()) return true;
  SD_MMC.end();
  montada_ = false;
  return true;
}

} // namespace GestionSD

#endif
