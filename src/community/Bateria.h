// ============================================
// Bateria.h - MAX17048
// ESP32-S3 | I2C SDA=GPIO17 SCL=GPIO6 | MAX17048 0x36
//
// Usa el SOC real calculado por ModelGauge.
// Mantiene la API usada por COCODRILO OS:
// - Porcentaje de bateria.
// - Voltaje de celda.
// - Avisos 20/10/5.
// - Apagado seguro opcional.
// ============================================

#ifndef BATERIA_H
#define BATERIA_H

#include <Arduino.h>
#include <Wire.h>
#include <TFT_eSPI.h>
#include "driver/rtc_io.h"

namespace Bateria {

// ------------------------------------------------
// HARDWARE - MAX17048
// ------------------------------------------------
static constexpr int PIN_SDA = 17;
static constexpr int PIN_SCL = 6;
static constexpr uint8_t MAX17048_ADDR = 0x36;

// Bus I2C dedicado al MAX17048.
// Usamos el controlador I2C #1 para que otras librerias que usen Wire
// no puedan cambiar los pines/frecuencia del medidor de bateria.
static TwoWire WireBateria(1);

static constexpr gpio_num_t PIN_BOTON_WAKEUP = GPIO_NUM_5;

// ------------------------------------------------
// MUESTREO / LIMITES
// ------------------------------------------------
static constexpr unsigned long INTERVALO_LECTURA_MS = 1000UL;
static constexpr float VOLTAJE_VALIDO_MIN = 3.00f;
static constexpr float VOLTAJE_VALIDO_MAX = 4.60f;
// Por debajo de 3.00 V consideramos que la bateria no esta conectada
// o que la lectura no es util. No se borra el ultimo valor valido.

static constexpr int ALERTA_20 = 20;
static constexpr int ALERTA_10 = 10;
static constexpr int ALERTA_5  = 5;
static constexpr unsigned long DURACION_AVISO_MS = 2500UL;

// Mantener false durante pruebas. Cambiar a true cuando quieras
// activar deep-sleep automatico por bateria critica.
static constexpr bool APAGADO_BATERIA_CRITICA = false;
static constexpr float VOLTAJE_APAGADO = 3.30f;
static constexpr int PORCENTAJE_APAGADO = 1;
static constexpr int LECTURAS_APAGADO = 3;

// ------------------------------------------------
// ESTADO
// ------------------------------------------------
static unsigned long ultimaLectura = 0;
static bool max17048Presente = false;
static bool bateriaDetectada = false;
static bool medicionIniciada = false;
static bool porcentajeInicialFijado = false;
static bool apagadoPendiente = false;

static float voltajeActual = 0.0f;
static float porcentajeCrudoActual = 0.0f;
static float crateActual = 0.0f;
static int porcentajeCalculado = 0;
static int porcentajeMostrado = 0;

static int confirmacionesApagado = 0;
static int fallosI2CConsecutivos = 0;

static bool aviso20Mostrado = false;
static bool aviso10Mostrado = false;
static bool aviso5Mostrado = false;
static int avisoNivelActivo = 0;
static unsigned long avisoHasta = 0;

// ------------------------------------------------
// I2C MAX17048
// ------------------------------------------------
static bool leerRegistro16(uint8_t reg, uint16_t &valor) {
  WireBateria.beginTransmission(MAX17048_ADDR);
  WireBateria.write(reg);
  if (WireBateria.endTransmission(false) != 0) return false;

  const int recibidos = WireBateria.requestFrom((int)MAX17048_ADDR, 2);
  if (recibidos != 2) return false;

  valor = ((uint16_t)WireBateria.read() << 8) | (uint16_t)WireBateria.read();
  return true;
}

static bool detectarMAX17048() {
  WireBateria.beginTransmission(MAX17048_ADDR);
  const uint8_t error = WireBateria.endTransmission();

  if (error == 0) return true;

  Serial.printf("Bateria: probe 0x36 fallo, error I2C=%u\n", error);
  return false;
}

static bool leerMAX17048(float &voltaje, float &soc, float &crate) {
  uint16_t rawV = 0;
  uint16_t rawSOC = 0;
  uint16_t rawCRATE = 0;

  if (!leerRegistro16(0x02, rawV)) return false;      // VCELL
  if (!leerRegistro16(0x04, rawSOC)) return false;    // SOC
  if (!leerRegistro16(0x16, rawCRATE)) return false;  // CRATE

  voltaje = (float)rawV * 0.000078125f;
  soc = (float)rawSOC / 256.0f;
  crate = (float)((int16_t)rawCRATE) * 0.208f;

  return true;
}

// ------------------------------------------------
// AVISOS
// ------------------------------------------------
static void reiniciarAvisosBateria() {
  aviso20Mostrado = false;
  aviso10Mostrado = false;
  aviso5Mostrado = false;
  avisoNivelActivo = 0;
  avisoHasta = 0;
}

static void activarAvisoBateria(int nivel) {
  avisoNivelActivo = nivel;
  avisoHasta = millis() + DURACION_AVISO_MS;

  if (nivel <= ALERTA_20) aviso20Mostrado = true;
  if (nivel <= ALERTA_10) aviso10Mostrado = true;
  if (nivel <= ALERTA_5)  aviso5Mostrado = true;

  Serial.printf("Bateria: aviso unico %d%%\n", nivel);
}

static void actualizarAvisosBateria() {
  if (!porcentajeInicialFijado || !bateriaDetectada) return;

  if (porcentajeMostrado <= ALERTA_5 && !aviso5Mostrado) {
    activarAvisoBateria(ALERTA_5);
  } else if (porcentajeMostrado <= ALERTA_10 && !aviso10Mostrado) {
    activarAvisoBateria(ALERTA_10);
  } else if (porcentajeMostrado <= ALERTA_20 && !aviso20Mostrado) {
    activarAvisoBateria(ALERTA_20);
  }
}

static void dibujarAvisoBateriaBaja(TFT_eSprite &spr) {
  if (avisoNivelActivo <= 0) return;

  if ((int32_t)(avisoHasta - millis()) <= 0) {
    avisoNivelActivo = 0;
    avisoHasta = 0;
    return;
  }

  constexpr int ancho = 238;
  constexpr int alto = 24;
  constexpr int x = (320 - ancho) / 2;
  constexpr int y = 212;

  spr.fillRoundRect(x, y, ancho, alto, 5, TFT_RED);
  spr.drawRoundRect(x, y, ancho, alto, 5, TFT_WHITE);

  spr.setTextColor(TFT_WHITE, TFT_RED);
  spr.setTextDatum(MC_DATUM);

  const char *mensaje =
      avisoNivelActivo <= ALERTA_5  ? "BATERIA 5% - CARGA YA" :
      avisoNivelActivo <= ALERTA_10 ? "BATERIA 10% - CARGA PRONTO" :
                                      "BATERIA 20% - BATERIA BAJA";

  spr.drawString(mensaje, x + ancho / 2, y + alto / 2);
  spr.setTextDatum(TL_DATUM);
}

// ------------------------------------------------
// DIBUJO
// ------------------------------------------------
static void dibujarIcono(TFT_eSprite &spr, int x, int y, int porcentaje) {
  constexpr int ancho = 24;
  constexpr int alto = 12;
  constexpr int terminalAncho = 2;
  constexpr int terminalAlto = 6;

  uint16_t colorRelleno = TFT_RED;
  if (porcentaje > 50) colorRelleno = TFT_GREEN;
  else if (porcentaje > 20) colorRelleno = TFT_YELLOW;

  spr.drawRect(x, y, ancho, alto, TFT_WHITE);
  spr.fillRect(x + ancho, y + (alto - terminalAlto) / 2,
               terminalAncho, terminalAlto, TFT_WHITE);

  const int relleno = map(constrain(porcentaje, 0, 100), 0, 100, 0, ancho - 4);

  if (relleno > 0) {
    spr.fillRect(x + 2, y + 2, relleno, alto - 4, colorRelleno);
  }

  if (relleno < ancho - 4) {
    spr.fillRect(x + 2 + relleno, y + 2,
                 (ancho - 4) - relleno, alto - 4, TFT_BLACK);
  }

  spr.setTextColor(TFT_WHITE, TFT_BLACK);
  spr.setTextDatum(MR_DATUM);
  spr.drawString(String(porcentaje) + "%", x - 4, y + alto / 2);
  spr.setTextDatum(TL_DATUM);
}

static void dibujarEsperando(TFT_eSprite &spr, int x, int y) {
  constexpr int ancho = 24;
  constexpr int alto = 12;

  spr.drawRect(x, y, ancho, alto, TFT_WHITE);
  spr.fillRect(x + ancho, y + 3, 2, 6, TFT_WHITE);

  const int fase = (millis() / 350UL) % 3UL;
  for (int i = 0; i < 3; ++i) {
    const uint16_t color = (i == fase) ? TFT_CYAN : TFT_DARKGREY;
    spr.fillCircle(x + 6 + i * 6, y + 6, 1, color);
  }

  spr.setTextFont(1);
  spr.setTextSize(1);
  spr.setTextDatum(MR_DATUM);
  spr.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  spr.drawString("...", x - 4, y + alto / 2);
  spr.setTextDatum(TL_DATUM);
}

// ------------------------------------------------
// APAGADO
// ------------------------------------------------
static void apagarPorBateriaBaja(TFT_eSprite &spr, TFT_eSPI &tft) {
  spr.fillSprite(TFT_BLACK);
  spr.setTextDatum(MC_DATUM);
  spr.setTextColor(TFT_RED, TFT_BLACK);
  spr.drawString("BATERIA CRITICA", 160, 100);
  spr.drawString("Apagando...", 160, 120);
  spr.setTextColor(TFT_WHITE, TFT_BLACK);
  spr.drawString("Presiona MENU para", 160, 150);
  spr.drawString("encender al cargar", 160, 165);
  spr.setTextDatum(TL_DATUM);
  spr.pushSprite(0, 0);

  delay(2500);
  Serial.println("Bateria: apagado por nivel critico");
  Serial.flush();

  tft.writecommand(0x28);
  esp_sleep_enable_ext0_wakeup(PIN_BOTON_WAKEUP, 0);
  rtc_gpio_pullup_en(PIN_BOTON_WAKEUP);
  rtc_gpio_pulldown_dis(PIN_BOTON_WAKEUP);
  esp_deep_sleep_start();
}

// ------------------------------------------------
// API PUBLICA
// ------------------------------------------------
static void Init() {
  // Misma inicializacion que funciono en el sketch minimo:
  // SDA=17, SCL=42, 50 kHz.
  WireBateria.end();
  delay(10);

  const bool i2cOK = WireBateria.begin(PIN_SDA, PIN_SCL, 50000);
  WireBateria.setTimeOut(50);
  delay(200);

  if (!i2cOK) {
    max17048Presente = false;
    Serial.println("Bateria: no se pudo iniciar I2C dedicado");
  }

  if (i2cOK) max17048Presente = detectarMAX17048();
  bateriaDetectada = false;
  voltajeActual = 0.0f;
  porcentajeCrudoActual = 0.0f;
  crateActual = 0.0f;
  porcentajeCalculado = 0;
  porcentajeMostrado = 0;
  medicionIniciada = true;
  porcentajeInicialFijado = false;
  apagadoPendiente = false;
  confirmacionesApagado = 0;
  fallosI2CConsecutivos = 0;
  reiniciarAvisosBateria();

  if (max17048Presente) {
    Serial.println("Bateria: MAX17048 detectado en 0x36 (SDA17/SCL6)");
  } else {
    Serial.println("Bateria: MAX17048 NO detectado");
  }

  // Fuerza una primera lectura inmediata.
  delay(20);
  ultimaLectura = millis() - INTERVALO_LECTURA_MS;
}

static void ComenzarMedicion() {
  medicionIniciada = true;
  ultimaLectura = millis() - INTERVALO_LECTURA_MS;
}

static void PrepararParaMenu() {
  medicionIniciada = true;
  ultimaLectura = millis() - INTERVALO_LECTURA_MS;
}

static void Actualizar() {
  if (!medicionIniciada) return;

  const unsigned long ahora = millis();
  if ((unsigned long)(ahora - ultimaLectura) < INTERVALO_LECTURA_MS) return;
  ultimaLectura = ahora;

  // Si aun no lo hemos visto, reintentamos. Una falla temporal NO borra
  // la ultima lectura valida del HUD.
  if (!max17048Presente) {
    max17048Presente = detectarMAX17048();
    if (!max17048Presente) {
      ++fallosI2CConsecutivos;
      return;
    }
  }

  float v = 0.0f;
  float soc = 0.0f;
  float crate = 0.0f;

  if (!leerMAX17048(v, soc, crate)) {
    ++fallosI2CConsecutivos;

    // No hacemos desaparecer el medidor por un timeout aislado.
    // Tras varias fallas soltamos la marca de presencia para que el
    // siguiente ciclo vuelva a detectar el MAX17048.
    if (fallosI2CConsecutivos >= 3) {
      max17048Presente = false;
    }
    return;
  }

  fallosI2CConsecutivos = 0;

  if (v < VOLTAJE_VALIDO_MIN || v > VOLTAJE_VALIDO_MAX) {
    // Si todavia no hubo una lectura valida, no mostramos bateria.
    // Si ya la hubo, conservamos voltaje/SOC/HUD anteriores.
    Serial.printf("Bateria: VCELL ignorado %.3f V (sin bateria o fuera de rango)\n", v);
    return;
  }

  voltajeActual = v;
  porcentajeCrudoActual = soc;
  crateActual = crate;

  porcentajeCalculado = constrain((int)lroundf(soc), 0, 100);
  porcentajeMostrado = porcentajeCalculado;

  // La primera lectura valida aparece inmediatamente; despues se actualiza a 1 Hz.
  bateriaDetectada = true;
  porcentajeInicialFijado = true;

  // Avisos emergentes desactivados; se conserva el indicador de bateria.
  // actualizarAvisosBateria();

  if (porcentajeMostrado <= PORCENTAJE_APAGADO || voltajeActual <= VOLTAJE_APAGADO) {
    ++confirmacionesApagado;
    if (confirmacionesApagado >= LECTURAS_APAGADO) apagadoPendiente = true;
  } else {
    confirmacionesApagado = 0;
    apagadoPendiente = false;
  }

  Serial.printf("Bateria: %.3f V  %.1f%%  HUD=%d%%  CRATE=%.2f %%/h\n",
                voltajeActual,
                porcentajeCrudoActual,
                porcentajeMostrado,
                crateActual);
}

static void Dibujar(TFT_eSprite &spr, int x, int y) {
  // Con MAX17048 no hace falta mostrar una animacion de "calculando".
  // Hasta tener la primera lectura valida simplemente no dibujamos nada.
  if (!porcentajeInicialFijado || !bateriaDetectada) return;

  dibujarIcono(spr, x, y, porcentajeMostrado);

  // Los avisos grandes solo se dibujan en un framebuffer de pantalla completa.
  if (spr.width() >= 300 && spr.height() >= 220) {
    // dibujarAvisoBateriaBaja(spr);  // Desactivado
  }
}

static void Tick(TFT_eSprite &spr, TFT_eSPI &tft, int x, int y) {
  Actualizar();

  if (APAGADO_BATERIA_CRITICA && apagadoPendiente) {
    apagarPorBateriaBaja(spr, tft);
  }

  Dibujar(spr, x, y);
}

static float getVoltaje() {
  return voltajeActual;
}

static float getVoltajeCrudo() {
  return voltajeActual;
}

// Compatibilidad con interfaces antiguas.
static float getAdcMilivoltios() {
  return voltajeActual * 1000.0f;
}

static float getCorrienteMa() {
  return 0.0f;
}

static float getPotenciaMw() {
  return 0.0f;
}

static float getShuntMilivoltios() {
  return 0.0f;
}

static float getVoltajeBus() {
  return voltajeActual;
}

// Nombre legado: ahora refleja presencia del MAX17048.
static bool ina219Detectado() {
  return max17048Presente;
}

static bool max17048Detectado() {
  return max17048Presente;
}

static float getChargeRatePorHora() {
  return crateActual;
}

static int getPorcentaje() {
  return porcentajeMostrado;
}

static int getPorcentajeCalculado() {
  return porcentajeCalculado;
}

static float getPorcentajeCrudo() {
  return porcentajeCrudoActual;
}

static bool hayBateria() {
  return bateriaDetectada;
}

// Compatibilidad: COCODRILO OS ya no intenta detectar el cargador.
static bool estaCargando() {
  return false;
}

static bool estaEstabilizando() {
  return medicionIniciada && !porcentajeInicialFijado;
}

static bool medicionLista() {
  return porcentajeInicialFijado;
}

static bool medicionEstaIniciada() {
  return medicionIniciada;
}

// Compatibilidad: el anuncio/evento de carga fue retirado.
static bool eventoCargaDetectada() {
  return false;
}

static void restablecer() {
  porcentajeInicialFijado = false;
  bateriaDetectada = false;
  confirmacionesApagado = 0;
  fallosI2CConsecutivos = 0;
  reiniciarAvisosBateria();
  ultimaLectura = millis() - INTERVALO_LECTURA_MS;
}

// Con MAX17048 no hay curva voltaje->porcentaje que recalibrar.
// Se fuerza una lectura nueva del SOC ModelGauge.
static void recalibrarDesdeVoltajeActual() {
  ultimaLectura = millis() - INTERVALO_LECTURA_MS;
  Actualizar();
}

}  // namespace Bateria

#endif  // BATERIA_H
