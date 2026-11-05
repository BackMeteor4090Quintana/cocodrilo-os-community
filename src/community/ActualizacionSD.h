#ifndef ACTUALIZACION_SD_H
#define ACTUALIZACION_SD_H

#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>
#include <Update.h>

namespace ActualizacionSD {

static constexpr const char* RUTA_FIRMWARE = "/update/firmware.bin";
static constexpr const char* RUTA_INFO = "/update/update.txt";
static constexpr const char* RUTA_INSTALADO = "/update/firmware.installed";

// Cambia esta versión cada vez que publiques un firmware nuevo.
static constexpr const char* VERSION_ACTUAL = "1.0.3-test4";

struct Paquete {
  bool sdDisponible = false;
  bool firmwareEncontrado = false;
  bool firmwareValido = false;
  size_t tamano = 0;

  String version = "Desconocida";
  String nombre = "Actualizacion del sistema";
  String notas = "";
  String error = "";
};

enum ResultadoInstalacion : uint8_t {
  INSTALACION_OK = 0,
  ERROR_SD,
  ERROR_ARCHIVO,
  ERROR_CABECERA,
  ERROR_ESPACIO,
  ERROR_ESCRITURA,
  ERROR_FINALIZACION,
  ERROR_INCOMPLETA
};

using CallbackProgreso = void (*)(size_t escritos, size_t total);

inline String limpiar(String valor) {
  valor.trim();
  if (valor.startsWith("\"") && valor.endsWith("\"") && valor.length() >= 2) {
    valor = valor.substring(1, valor.length() - 1);
  }
  return valor;
}

inline void leerInfo(Paquete& paquete) {
  if (!SD_MMC.exists(RUTA_INFO)) return;

  File archivo = SD_MMC.open(RUTA_INFO, FILE_READ);
  if (!archivo) return;

  while (archivo.available()) {
    String linea = archivo.readStringUntil('\n');
    linea.trim();
    if (linea.length() == 0 || linea.startsWith("#") || linea.startsWith(";")) {
      continue;
    }

    const int separador = linea.indexOf('=');
    if (separador <= 0) continue;

    String clave = linea.substring(0, separador);
    String valor = linea.substring(separador + 1);
    clave.trim();
    clave.toLowerCase();
    valor = limpiar(valor);

    if (clave == "version") paquete.version = valor;
    else if (clave == "name" || clave == "nombre") paquete.nombre = valor;
    else if (clave == "notes" || clave == "notas") paquete.notas = valor;
  }

  archivo.close();
}

inline Paquete buscar() {
  Paquete paquete;
  paquete.sdDisponible = SD_MMC.cardType() != CARD_NONE;

  if (!paquete.sdDisponible) {
    paquete.error = "Tarjeta SD no disponible";
    return paquete;
  }

  if (!SD_MMC.exists(RUTA_FIRMWARE)) {
    paquete.error = "No existe /update/firmware.bin";
    leerInfo(paquete);
    return paquete;
  }

  File firmware = SD_MMC.open(RUTA_FIRMWARE, FILE_READ);
  if (!firmware) {
    paquete.error = "No se pudo abrir firmware.bin";
    return paquete;
  }

  paquete.firmwareEncontrado = true;
  paquete.tamano = firmware.size();

  if (paquete.tamano < 1024) {
    paquete.error = "El archivo es demasiado pequeno";
    firmware.close();
    leerInfo(paquete);
    return paquete;
  }

  const int primerByte = firmware.read();
  firmware.close();

  // Las imágenes de aplicación ESP32 comienzan normalmente con 0xE9.
  if (primerByte != 0xE9) {
    paquete.error = "Cabecera de firmware invalida";
    leerInfo(paquete);
    return paquete;
  }

  paquete.firmwareValido = true;
  leerInfo(paquete);
  return paquete;
}

inline bool mismaVersion(const Paquete& paquete) {
  return paquete.version.length() > 0 &&
         paquete.version != "Desconocida" &&
         paquete.version == VERSION_ACTUAL;
}

inline ResultadoInstalacion instalar(const Paquete& paquete,
                                     CallbackProgreso callback = nullptr) {
  if (SD_MMC.cardType() == CARD_NONE) return ERROR_SD;
  if (!paquete.firmwareEncontrado) return ERROR_ARCHIVO;
  if (!paquete.firmwareValido) return ERROR_CABECERA;

  File firmware = SD_MMC.open(RUTA_FIRMWARE, FILE_READ);
  if (!firmware) return ERROR_ARCHIVO;

  const size_t total = firmware.size();

  if (!Update.begin(total, U_FLASH)) {
    firmware.close();
    return ERROR_ESPACIO;
  }

  uint8_t buffer[4096];
  size_t escritos = 0;

  while (firmware.available()) {
    const size_t disponibles = firmware.available();
    const size_t porLeer = min(sizeof(buffer), disponibles);
    const size_t leidos = firmware.read(buffer, porLeer);

    if (leidos == 0) {
      Update.abort();
      firmware.close();
      return ERROR_ESCRITURA;
    }

    const size_t grabados = Update.write(buffer, leidos);
    if (grabados != leidos) {
      Update.abort();
      firmware.close();
      return ERROR_ESCRITURA;
    }

    escritos += grabados;
    if (callback) callback(escritos, total);
    delay(1);
  }

  firmware.close();

  if (!Update.end(true)) {
    return ERROR_FINALIZACION;
  }

  if (!Update.isFinished()) {
    return ERROR_INCOMPLETA;
  }

  // Evita instalar otra vez el mismo paquete al volver a entrar al menú.
  if (SD_MMC.exists(RUTA_INSTALADO)) {
    SD_MMC.remove(RUTA_INSTALADO);
  }
  SD_MMC.rename(RUTA_FIRMWARE, RUTA_INSTALADO);

  return INSTALACION_OK;
}

inline const char* textoError(ResultadoInstalacion resultado) {
  switch (resultado) {
    case ERROR_SD: return "Tarjeta SD no disponible";
    case ERROR_ARCHIVO: return "No se pudo abrir el firmware";
    case ERROR_CABECERA: return "El firmware no es valido";
    case ERROR_ESPACIO: return "No hay particion OTA suficiente";
    case ERROR_ESCRITURA: return "Fallo al escribir la actualizacion";
    case ERROR_FINALIZACION: return "No se pudo validar la imagen";
    case ERROR_INCOMPLETA: return "La escritura quedo incompleta";
    default: return "Error desconocido";
  }
}

} // namespace ActualizacionSD

#endif
