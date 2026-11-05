#ifndef MODO_TECNICO_H
#define MODO_TECNICO_H

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <SD_MMC.h>
#include "Bateria.h"

namespace ModoTecnico {

enum Pagina : uint8_t {
  PAG_BATERIA = 0,
  PAG_BOTONES,
  PAG_SD,
  PAG_WIFI,
  PAG_SISTEMA,
  PAG_TOTAL
};

inline bool detectarSolicitud(int pinMenu, int pinA,
                              unsigned long tiempoMs = 650UL) {
  if (digitalRead(pinMenu) != LOW || digitalRead(pinA) != LOW) return false;

  const unsigned long inicio = millis();
  while (millis() - inicio < tiempoMs) {
    if (digitalRead(pinMenu) != LOW || digitalRead(pinA) != LOW) return false;
    delay(5);
  }
  return true;
}

inline void esperarLiberacion(const int* pines, size_t cantidad) {
  bool alguno;
  do {
    alguno = false;
    for (size_t i = 0; i < cantidad; ++i) {
      if (digitalRead(pines[i]) == LOW) {
        alguno = true;
        break;
      }
    }
    delay(5);
  } while (alguno);
}

inline void encabezado(TFT_eSPI& tft, const char* titulo, uint8_t pagina) {
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, 320, 30, TFT_DARKGREY);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_DARKGREY);
  tft.drawString("MODO TECNICO", 160, 10, 2);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  tft.drawString(titulo, 160, 24, 2);

  tft.setTextDatum(BC_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString(
      String("< >  ") + String(pagina + 1) + "/" + String(PAG_TOTAL) +
      "   MENU: SALIR",
      160, 237, 1
  );
  tft.setTextDatum(TL_DATUM);
}

inline void etiqueta(TFT_eSPI& tft, int y, const char* nombre,
                     const String& valor, uint16_t color = TFT_WHITE) {
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString(nombre, 18, y, 2);
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(color, TFT_BLACK);
  tft.drawString(valor, 302, y, 2);
  tft.setTextDatum(TL_DATUM);
}

inline void dibujarBateria(TFT_eSPI& tft) {
  Bateria::Actualizar();
  encabezado(tft, "BATERIA", PAG_BATERIA);

  const float vBat = Bateria::getVoltaje();
  const float vCrudo = Bateria::getVoltajeCrudo();
  const float adcMv = Bateria::getAdcMilivoltios();
  const int calc = Bateria::getPorcentajeCalculado();
  const int mostrado = Bateria::getPorcentaje();

  etiqueta(tft, 52,  "ADC", String(adcMv, 1) + " mV", TFT_CYAN);
  etiqueta(tft, 78,  "Voltaje crudo", String(vCrudo, 3) + " V", TFT_CYAN);
  etiqueta(tft, 104, "Voltaje filtrado", String(vBat, 3) + " V", TFT_GREEN);
  etiqueta(tft, 130, "Porcentaje calc.", String(calc) + " %");
  etiqueta(tft, 156, "Porcentaje visible", String(mostrado) + " %");
  etiqueta(tft, 182, "Bateria", Bateria::hayBateria() ? "DETECTADA" : "NO",
           Bateria::hayBateria() ? TFT_GREEN : TFT_RED);
  const char* estado = Bateria::estaEstabilizando() ? "ESTABILIZANDO" : "NORMAL";
  const uint16_t colorEstado = Bateria::estaEstabilizando() ? TFT_CYAN : TFT_GREEN;
  etiqueta(tft, 208, "Estado", estado, colorEstado);
}

inline const char* estadoBoton(int pin) {
  return digitalRead(pin) == LOW ? "PULSADO" : "LIBRE";
}

inline void dibujarBotones(TFT_eSPI& tft,
                           int pinMenu, int pinA, int pinB,
                           int pinUp, int pinDown, int pinLeft, int pinRight) {
  encabezado(tft, "BOTONES", PAG_BOTONES);

  etiqueta(tft, 48,  "A", estadoBoton(pinA),
           digitalRead(pinA) == LOW ? TFT_YELLOW : TFT_GREEN);
  etiqueta(tft, 72,  "B", estadoBoton(pinB),
           digitalRead(pinB) == LOW ? TFT_YELLOW : TFT_GREEN);
  etiqueta(tft, 96,  "MENU", estadoBoton(pinMenu),
           digitalRead(pinMenu) == LOW ? TFT_YELLOW : TFT_GREEN);
  etiqueta(tft, 120, "ARRIBA", estadoBoton(pinUp),
           digitalRead(pinUp) == LOW ? TFT_YELLOW : TFT_GREEN);
  etiqueta(tft, 144, "ABAJO", estadoBoton(pinDown),
           digitalRead(pinDown) == LOW ? TFT_YELLOW : TFT_GREEN);
  etiqueta(tft, 168, "IZQUIERDA", estadoBoton(pinLeft),
           digitalRead(pinLeft) == LOW ? TFT_YELLOW : TFT_GREEN);
  etiqueta(tft, 192, "DERECHA", estadoBoton(pinRight),
           digitalRead(pinRight) == LOW ? TFT_YELLOW : TFT_GREEN);
}

inline String bytesLegibles(uint64_t bytes) {
  if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
    return String(bytes / (1024.0 * 1024.0 * 1024.0), 2) + " GB";
  }
  if (bytes >= 1024ULL * 1024ULL) {
    return String(bytes / (1024.0 * 1024.0), 1) + " MB";
  }
  return String((unsigned long)bytes) + " B";
}

inline void dibujarSD(TFT_eSPI& tft) {
  encabezado(tft, "TARJETA SD", PAG_SD);

  const uint8_t tipo = SD_MMC.cardType();
  const bool montada = tipo != CARD_NONE;

  etiqueta(tft, 58, "Estado", montada ? "CONECTADA" : "NO DISPONIBLE",
           montada ? TFT_GREEN : TFT_RED);

  if (montada) {
    const char* nombre = "DESCONOCIDA";
    if (tipo == CARD_MMC) nombre = "MMC";
    else if (tipo == CARD_SD) nombre = "SDSC";
    else if (tipo == CARD_SDHC) nombre = "SDHC/SDXC";

    const uint64_t total = SD_MMC.cardSize();
    const uint64_t usado = SD_MMC.usedBytes();
    const uint64_t libre = total > usado ? total - usado : 0;

    etiqueta(tft, 90,  "Tipo", nombre);
    etiqueta(tft, 122, "Capacidad", bytesLegibles(total), TFT_CYAN);
    etiqueta(tft, 154, "Usado", bytesLegibles(usado), TFT_YELLOW);
    etiqueta(tft, 186, "Libre", bytesLegibles(libre), TFT_GREEN);
  }
}

inline void dibujarWiFi(TFT_eSPI& tft) {
  encabezado(tft, "WI-FI", PAG_WIFI);

  const bool conectado = WiFi.status() == WL_CONNECTED;
  etiqueta(tft, 58, "Estado", conectado ? "CONECTADO" : "DESCONECTADO",
           conectado ? TFT_GREEN : TFT_RED);

  if (conectado) {
    etiqueta(tft, 90,  "SSID", WiFi.SSID());
    etiqueta(tft, 122, "IP", WiFi.localIP().toString(), TFT_CYAN);
    etiqueta(tft, 154, "Senal", String(WiFi.RSSI()) + " dBm",
             WiFi.RSSI() > -65 ? TFT_GREEN : TFT_YELLOW);
    etiqueta(tft, 186, "MAC", WiFi.macAddress());
  }
}

inline void dibujarSistema(TFT_eSPI& tft) {
  encabezado(tft, "SISTEMA", PAG_SISTEMA);

  etiqueta(tft, 48,  "Temperatura", String(temperatureRead(), 1) + " C",
           TFT_YELLOW);
  etiqueta(tft, 76,  "Heap libre", bytesLegibles(ESP.getFreeHeap()), TFT_GREEN);
  etiqueta(tft, 104, "Heap minimo", bytesLegibles(ESP.getMinFreeHeap()));
  etiqueta(tft, 132, "PSRAM libre", bytesLegibles(ESP.getFreePsram()), TFT_GREEN);
  etiqueta(tft, 160, "PSRAM total", bytesLegibles(ESP.getPsramSize()));
  etiqueta(tft, 188, "CPU", String(ESP.getCpuFreqMHz()) + " MHz", TFT_CYAN);
  etiqueta(tft, 214, "Flash", bytesLegibles(ESP.getFlashChipSize()));
}

inline void ejecutar(TFT_eSPI& tft,
                     int pinMenu, int pinA, int pinB,
                     int pinUp, int pinDown, int pinLeft, int pinRight) {
  const int pines[] = {
    pinMenu, pinA, pinB, pinUp, pinDown, pinLeft, pinRight
  };
  esperarLiberacion(pines, sizeof(pines) / sizeof(pines[0]));
  Bateria::ComenzarMedicion();

  Pagina pagina = PAG_BATERIA;
  bool previoMenu = false;
  bool previoIzq = false;
  bool previoDer = false;
  unsigned long ultimoDibujo = 0;

  while (true) {
    const bool menu = digitalRead(pinMenu) == LOW;
    const bool izq = digitalRead(pinLeft) == LOW;
    const bool der = digitalRead(pinRight) == LOW;

    if (menu && !previoMenu) {
      while (digitalRead(pinMenu) == LOW) delay(5);
      break;
    }

    if (izq && !previoIzq) {
      pagina = static_cast<Pagina>(
          (static_cast<int>(pagina) + PAG_TOTAL - 1) % PAG_TOTAL
      );
      ultimoDibujo = 0;
    }

    if (der && !previoDer) {
      pagina = static_cast<Pagina>(
          (static_cast<int>(pagina) + 1) % PAG_TOTAL
      );
      ultimoDibujo = 0;
    }

    previoMenu = menu;
    previoIzq = izq;
    previoDer = der;

    const unsigned long ahora = millis();
    const unsigned long intervalo =
        pagina == PAG_BOTONES ? 80UL : 500UL;

    if (ultimoDibujo == 0 || ahora - ultimoDibujo >= intervalo) {
      ultimoDibujo = ahora;

      switch (pagina) {
        case PAG_BATERIA:
          dibujarBateria(tft);
          break;
        case PAG_BOTONES:
          dibujarBotones(
              tft, pinMenu, pinA, pinB,
              pinUp, pinDown, pinLeft, pinRight
          );
          break;
        case PAG_SD:
          dibujarSD(tft);
          break;
        case PAG_WIFI:
          dibujarWiFi(tft);
          break;
        case PAG_SISTEMA:
          dibujarSistema(tft);
          break;
        default:
          break;
      }
    }

    delay(8);
  }

  esperarLiberacion(pines, sizeof(pines) / sizeof(pines[0]));
  Bateria::PrepararParaMenu();
  tft.fillScreen(TFT_BLACK);
}

} // namespace ModoTecnico

#endif
