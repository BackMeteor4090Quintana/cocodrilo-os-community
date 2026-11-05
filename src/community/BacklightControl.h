#pragma once

#include <Arduino.h>
#include <Preferences.h>

// ============================================================
// COCODRILO OS - CONTROL DE BACKLIGHT
//
// Hardware previsto:
//   ST7789 con un unico pin BL positivo.
//   P-MOSFET high-side (FDN338P o equivalente logic-level):
//
//     3.3V ---- Source
//                P-MOSFET
//     BL  ----- Drain
//
//     GPIO21 -- 150R -- Gate
//                      |
//                    100k
//                      |
//                    3.3V
//
// El P-MOSFET es activo en LOW:
//   Gate LOW  = backlight encendido
//   Gate HIGH = backlight apagado
//
// Para desactivar completamente este modulo por software, cambia a 0:
// ============================================================
#define COCODRILO_BACKLIGHT_CONTROL_ENABLED 1

namespace BacklightControl {

static constexpr int PIN = 21;
static constexpr uint32_t PWM_HZ = 20000;
static constexpr uint8_t PWM_BITS = 8;
static constexpr int NIVEL_MIN = 1;
static constexpr int NIVEL_MAX = 10;

// Curva perceptual expresada como porcentaje PWM de ENCENDIDO.
// Como el P-MOSFET es activo en LOW, luego se invierte para LEDC.
static constexpr uint8_t ON_DUTY[NIVEL_MAX + 1] = {
  0,
  4, 14, 23, 36, 54, 78, 108, 145, 194, 255
};

inline int &nivelRef() {
  static int nivel = NIVEL_MAX;
  return nivel;
}

inline bool &pwmListoRef() {
  static bool listo = false;
  return listo;
}

inline int limitar(int nivel) {
  return constrain(nivel, NIVEL_MIN, NIVEL_MAX);
}

inline uint8_t dutySalida(int nivel) {
  nivel = limitar(nivel);
  // P-MOS high-side: LOW enciende, HIGH apaga.
  return (uint8_t)(255U - ON_DUTY[nivel]);
}

inline void cargar() {
  Preferences prefs;
  prefs.begin("arcade", true);
  nivelRef() = limitar((int)prefs.getUChar("brillo", NIVEL_MAX));
  prefs.end();
  Serial.printf("[BRILLO] guardado=%d/10\n", nivelRef());
}

inline void guardar() {
  Preferences prefs;
  prefs.begin("arcade", false);
  prefs.putUChar("brillo", (uint8_t)nivelRef());
  prefs.end();
}

inline int actual() {
  return nivelRef();
}

// Se llama ANTES de tft.init(). Con el pull-up fisico de 100k, HIGH mantiene
// el P-MOSFET apagado mientras la pantalla aun no esta inicializada.
inline void prepararAntesTFT() {
#if COCODRILO_BACKLIGHT_CONTROL_ENABLED
  pinMode(PIN, OUTPUT);
  digitalWrite(PIN, HIGH);
#endif
}

inline void aplicar() {
#if COCODRILO_BACKLIGHT_CONTROL_ENABLED
  if (!pwmListoRef()) return;
  ledcWrite(PIN, dutySalida(nivelRef()));
#endif
}

inline void iniciar() {
#if COCODRILO_BACKLIGHT_CONTROL_ENABLED
  pinMode(PIN, OUTPUT);
  digitalWrite(PIN, HIGH);

  pwmListoRef() = ledcAttach(PIN, PWM_HZ, PWM_BITS);
  if (!pwmListoRef()) {
    // Si LEDC falla, dejamos la pantalla visible a brillo completo.
    digitalWrite(PIN, LOW);
    Serial.println("[BRILLO] ERROR LEDC; P-MOS fijo encendido");
    return;
  }

  aplicar();
  Serial.printf("[BRILLO] P-MOS GPIO%d nivel=%d/10 dutySalida=%u\n",
                PIN, nivelRef(), (unsigned)dutySalida(nivelRef()));
#else
  Serial.println("[BRILLO] control PWM desactivado en BacklightControl.h");
#endif
}

inline void set(int nivel, bool persistir = true) {
  nivel = limitar(nivel);
  if (nivel == nivelRef()) return;

  nivelRef() = nivel;
  aplicar();

  if (persistir) guardar();
  Serial.printf("[BRILLO] nivel=%d/10\n", nivelRef());
}

} // namespace BacklightControl
