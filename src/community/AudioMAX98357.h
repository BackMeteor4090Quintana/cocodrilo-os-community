#ifndef COCODRILO_AUDIO_MAX98357_H
#define COCODRILO_AUDIO_MAX98357_H

#include <Arduino.h>
#include "driver/i2s.h"
#include <math.h>
#include "metronomo_wav_pcm.h"

namespace AudioMAX98357 {

// ============================================================
// MAX98357A — COCODRILO OS
// Driver I2S LEGACY.
//
// IMPORTANTE:
// Anemoia incluye driver/i2s.h (legacy). ESP-IDF no permite mezclar
// el driver legacy con i2s_std.h en el mismo firmware, aunque NES
// todavía no tenga audio. Por eso COCODRILO usa también legacy.
//
// Usamos I2S_NUM_1 para el audio general de COCODRILO OS.
// Anemoia tiene su salida de APU cableada a I2S_NUM_0, que dejaremos
// sin inicializar por ahora.
//
// Pinout:
// GPIO47 -> BCLK
// GPIO18 -> LRC / WS
// GPIO41 -> DIN
// I2S se comparte tambien con el PCM5102A.
// GPIO42 -> SD / ENABLE del MAX98357A (HIGH=bocina, LOW=mute).
// GPIO14 -> detector de audifonos (0=sin plug, 1=con plug).
// El PCM5102A permanece conectado al mismo I2S; al insertar audifonos
// se apaga solo el MAX y el audio continua por el PCM.
// ============================================================

static constexpr i2s_port_t I2S_PORT = I2S_NUM_1;

static constexpr int PIN_BCLK = 47;
static constexpr int PIN_LRC  = 18;
static constexpr int PIN_DIN  = 41;

// Enrutamiento automatico bocina / audifonos.
static constexpr int PIN_MAX_SD = 42;
static constexpr int PIN_AUDIFONOS_DETECT = 14;
static constexpr int NIVEL_AUDIFONOS_CONECTADOS = HIGH;
static constexpr uint32_t DEBOUNCE_AUDIFONOS_MS = 35;

static constexpr uint32_t SAMPLE_RATE = 44100;
static constexpr size_t FRAMES_BLOQUE = 256;
// Margen para el amplificador en tonos sintetizados (menu/juegos/metronomo).
// No afecta a la decodificacion ni al volumen de la musica MP3.
static constexpr int16_t AMPLITUD_MAX = 14500;

// Volumen digital tipo "potenciómetro".
// 0 = silencio, 10 = máximo.
static volatile uint8_t volumenPaso = 5;

static TaskHandle_t tareaAudio = nullptr;
static portMUX_TYPE muxAudio = portMUX_INITIALIZER_UNLOCKED;

static volatile uint32_t frecuenciaActual = 0;
static volatile int8_t clicSolicitado = -1; // -1 ninguno, 0 normal, 1 acento
static volatile bool habilitado = true;
static bool iniciado = false;
static volatile bool reinicioSolicitado = false;
static volatile uint32_t contadorErroresI2S = 0;
static volatile bool modoNESActivo = false;
static volatile bool tareaAudioPausadaParaNES = false;
static volatile uint32_t sampleRateI2SActual = SAMPLE_RATE;
static int16_t tablaSeno[256];
static bool tablaSenoLista = false;

// Estado del jack. El contacto probado en la consola entrega:
//   0 = sin audifonos
//   1 = audifonos conectados
static volatile bool audifonosConectados = false;
static int lecturaDetectorAnterior = -1;
static int lecturaDetectorEstable = -1;
static uint32_t cambioDetectorMs = 0;

static void aplicarRutaAudio(bool hayAudifonos, bool forzar = false) {
  const bool cambio = (audifonosConectados != hayAudifonos);
  audifonosConectados = hayAudifonos;

  // El PCM5102A siempre recibe el I2S compartido.
  // Solo apagamos/encendemos fisicamente el MAX98357A.
  digitalWrite(PIN_MAX_SD, hayAudifonos ? LOW : HIGH);

  if (cambio || forzar) {
    Serial.println(hayAudifonos
        ? "[AUDIO] Audifonos conectados -> MAX mute / PCM activo"
        : "[AUDIO] Sin audifonos -> MAX activo / PCM compartido");
  }
}

static void actualizarDetectorAudifonos(bool forzar = false) {
  const int lectura = digitalRead(PIN_AUDIFONOS_DETECT);

  // Primera lectura: aplicar inmediatamente para arrancar con la salida correcta.
  if (lecturaDetectorAnterior < 0 || lecturaDetectorEstable < 0) {
    lecturaDetectorAnterior = lectura;
    lecturaDetectorEstable = lectura;
    cambioDetectorMs = millis();
    aplicarRutaAudio(lectura == NIVEL_AUDIFONOS_CONECTADOS, true);
    return;
  }

  if (lectura != lecturaDetectorAnterior) {
    lecturaDetectorAnterior = lectura;
    cambioDetectorMs = millis();
  }

  if (forzar ||
      ((uint32_t)(millis() - cambioDetectorMs) >= DEBOUNCE_AUDIFONOS_MS &&
       lecturaDetectorEstable != lectura)) {
    lecturaDetectorEstable = lectura;
    aplicarRutaAudio(lectura == NIVEL_AUDIFONOS_CONECTADOS, forzar);
  }
}

static inline bool hayAudifonos() {
  return audifonosConectados;
}

static void prepararTablaSeno() {
  if (tablaSenoLista) return;

  for (int i = 0; i < 256; ++i) {
    const float angulo = (2.0f * PI * (float)i) / 256.0f;
    tablaSeno[i] = (int16_t)lroundf(sinf(angulo) * 32767.0f);
  }

  tablaSenoLista = true;
}

static void tareaSalida(void*) {
  static int16_t buffer[FRAMES_BLOQUE * 2]; // estéreo L/R

  uint32_t fase = 0;
  uint32_t frecuenciaAnterior = 0;
  int32_t amplitudActual = 0;
  const int16_t* clicActual = nullptr;
  uint16_t clicPos = MetronomoWav::LEN;

  static constexpr int32_t PASO_ENVOLVENTE = 24;

  for (;;) {
    actualizarDetectorAudifonos();

    // NO suspender esta tarea con vTaskSuspend mientras está dentro de
    // i2s_write(): el driver legacy puede tener un mutex/semaforo tomado y
    // el APU NES se quedaría esperando para siempre.
    //
    // En modo NES hacemos una pausa COOPERATIVA: terminamos la escritura
    // corriente, marcamos ACK y dejamos de tocar I2S hasta salir de NES.
    if (modoNESActivo) {
      tareaAudioPausadaParaNES = true;
      while (modoNESActivo) {
        // Aunque NES/GBC/Galeria sean quienes escriben el I2S, esta tarea
        // sigue vigilando el jack para mutear/desmutear la bocina al vuelo.
        actualizarDetectorAudifonos();
        vTaskDelay(pdMS_TO_TICKS(1));
      }
      tareaAudioPausadaParaNES = false;

      // Volvemos limpios al audio general.
      fase = 0;
      frecuenciaAnterior = 0;
      amplitudActual = 0;
      continue;
    }

    uint32_t freqSolicitada;
    bool activo;
    uint8_t vol;
    bool hacerReinicio;
    int8_t nuevoClic;

    portENTER_CRITICAL(&muxAudio);
    freqSolicitada = frecuenciaActual;
    activo = habilitado;
    vol = volumenPaso;
    hacerReinicio = reinicioSolicitado;
    nuevoClic = clicSolicitado;
    clicSolicitado = -1;
    reinicioSolicitado = false;
    portEXIT_CRITICAL(&muxAudio);

    if (vol > 10) vol = 10;
    if (nuevoClic >= 0) {
      clicActual = nuevoClic ? MetronomoWav::fuerte : MetronomoWav::normal;
      clicPos = 0;
    }

    // Reset limpio del stream cuando cambia el volumen.
    // No desinstalamos I2S: solo vaciamos DMA y reiniciamos
    // fase/envolvente para evitar que quede "sucio" después de
    // pasar por niveles altos.
    if (hacerReinicio) {
      memset(buffer, 0, sizeof(buffer));
      size_t escritosSilencio = 0;
      i2s_write(I2S_PORT, buffer, sizeof(buffer),
                &escritosSilencio, portMAX_DELAY);
      i2s_zero_dma_buffer(I2S_PORT);

      fase = 0;
      frecuenciaAnterior = freqSolicitada;
      amplitudActual = 0;

      // Un bloque muy corto de silencio antes del nuevo attack.
      i2s_write(I2S_PORT, buffer, sizeof(buffer),
                &escritosSilencio, portMAX_DELAY);
    }

    static const uint8_t escalaVol[11] = {
      0, 8, 16, 25, 35, 46, 58, 70, 81, 91, 100
    };

    const bool hayNota = activo && freqSolicitada > 0 && vol > 0;
    const int32_t amplitudObjetivo =
        hayNota ? ((int32_t)AMPLITUD_MAX * escalaVol[vol] / 100) : 0;

    uint32_t freqGeneracion = freqSolicitada;
    if (freqGeneracion == 0) freqGeneracion = frecuenciaAnterior;

    if (freqGeneracion == 0 && (!clicActual || clicPos >= MetronomoWav::LEN)) {
      memset(buffer, 0, sizeof(buffer));
      amplitudActual = 0;
      fase = 0;
    } else {
      const uint32_t pasoFase =
          (uint32_t)(((uint64_t)freqGeneracion << 32) / SAMPLE_RATE);

      for (size_t n = 0; n < FRAMES_BLOQUE; ++n) {
        if (amplitudActual < amplitudObjetivo) {
          amplitudActual += PASO_ENVOLVENTE;
          if (amplitudActual > amplitudObjetivo)
            amplitudActual = amplitudObjetivo;
        } else if (amplitudActual > amplitudObjetivo) {
          amplitudActual -= PASO_ENVOLVENTE;
          if (amplitudActual < amplitudObjetivo)
            amplitudActual = amplitudObjetivo;
        }

        fase += pasoFase;
        const uint8_t indice = (uint8_t)(fase >> 24);
        const int32_t seno = tablaSeno[indice];

        const int16_t muestra =
            (int16_t)((seno * amplitudActual) / 32767);

        int32_t mezcla = muestra;
        if (clicActual && clicPos < MetronomoWav::LEN) {
          // Compresion estatica exclusiva del clic PCM del metronomo.
          // Da presencia al cuerpo del clic y reduce los picos que
          // podrian sobrecargar la bocina. No altera otras fuentes de audio.
          // PCM original: pico +/-14500. Pico procesado: +/-10825.
          const int32_t original = (int32_t)clicActual[clicPos++];
          const int32_t magnitud = original < 0 ? -original : original;
          const int32_t comprimida = magnitud <= 5000
              ? (magnitud * 3) / 2
              : 7500 + ((magnitud - 5000) * 35) / 100;
          const int32_t clic = (original < 0 ? -comprimida : comprimida)
              * escalaVol[vol] / 100;
          mezcla += clic;
        }
        buffer[n * 2]     = (int16_t)mezcla;
        buffer[n * 2 + 1] = (int16_t)mezcla;
      }

      if (!hayNota && amplitudActual == 0) {
        fase = 0;
        frecuenciaAnterior = 0;
      } else if (freqSolicitada > 0) {
        frecuenciaAnterior = freqSolicitada;
      }
    }

    size_t escritos = 0;
    esp_err_t err = i2s_write(
        I2S_PORT,
        buffer,
        sizeof(buffer),
        &escritos,
        portMAX_DELAY
    );

    if (err != ESP_OK || escritos != sizeof(buffer)) {
      contadorErroresI2S++;

      // No inundar Serial: reportar cada 8 incidencias.
      if ((contadorErroresI2S & 0x7) == 1) {
        Serial.printf(
          "[MAX98357] aviso I2S: err=%d escritos=%u/%u total=%u\n",
          (int)err,
          (unsigned)escritos,
          (unsigned)sizeof(buffer),
          (unsigned)contadorErroresI2S
        );
      }

      vTaskDelay(pdMS_TO_TICKS(2));
    }
  }
}

static bool begin() {
  if (iniciado) return true;

  // GPIO42 controla SD/ENABLE del MAX. GPIO7 queda exclusivamente para LEFT.
  pinMode(PIN_MAX_SD, OUTPUT);
  digitalWrite(PIN_MAX_SD, HIGH);

  // El jack probado entrega LOW sin plug y HIGH con audifonos.
  // INPUT_PULLUP mantiene estable el nivel cuando el contacto se abre.
  pinMode(PIN_AUDIFONOS_DETECT, INPUT_PULLUP);
  actualizarDetectorAudifonos(true);

  prepararTablaSeno();

  i2s_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));

  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = SAMPLE_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 12;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;
  cfg.fixed_mclk = 0;

  esp_err_t err = i2s_driver_install(I2S_PORT, &cfg, 0, nullptr);
  if (err != ESP_OK) {
    Serial.printf("[MAX98357] i2s_driver_install fallo: %d\n", (int)err);
    return false;
  }

  i2s_pin_config_t pins;
  memset(&pins, 0, sizeof(pins));
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = PIN_BCLK;
  pins.ws_io_num = PIN_LRC;
  pins.data_out_num = PIN_DIN;
  pins.data_in_num = I2S_PIN_NO_CHANGE;

  err = i2s_set_pin(I2S_PORT, &pins);
  if (err != ESP_OK) {
    Serial.printf("[MAX98357] i2s_set_pin fallo: %d\n", (int)err);
    i2s_driver_uninstall(I2S_PORT);
    return false;
  }

  i2s_zero_dma_buffer(I2S_PORT);
  sampleRateI2SActual = SAMPLE_RATE;

  BaseType_t ok = xTaskCreatePinnedToCore(
      tareaSalida,
      "AudioMAX98357",
      4096,
      nullptr,
      2,
      &tareaAudio,
      0
  );

  if (ok != pdPASS) {
    Serial.println("[MAX98357] No se pudo crear tarea de audio");
    i2s_driver_uninstall(I2S_PORT);
    tareaAudio = nullptr;
    return false;
  }

  iniciado = true;

  Serial.println(
      "[AUDIO] I2S1 compartido MAX+PCM: BCLK=47 WS=18 DIN=41 | MAX_SD=42 | JACK=14"
  );
  return true;
}

// ------------------------------------------------------------
// Modo NES:
// Anemoia produce PCM continuo a 44.1 kHz. En vez de instalar otro
// periférico I2S, comparte el MISMO I2S_NUM_1 del MAX98357A.
// Suspendemos temporalmente la tarea de tonos de COCODRILO para que
// el APU sea el único escritor del DMA mientras corre NES.
// ------------------------------------------------------------
static bool entrarModoNES() {
  if (!iniciado || tareaAudio == nullptr) {
    Serial.println("[MAX98357] NES audio: backend no iniciado");
    return false;
  }

  // Pedir que la tarea de tonos ceda I2S de forma COOPERATIVA.
  portENTER_CRITICAL(&muxAudio);
  frecuenciaActual = 0;
  reinicioSolicitado = false;
  modoNESActivo = true;
  portEXIT_CRITICAL(&muxAudio);

  // Esperar a que la tarea salga de cualquier i2s_write() que estuviera
  // ejecutando y confirme que ya no tocará el periférico.
  const uint32_t inicioEspera = millis();
  while (!tareaAudioPausadaParaNES && (millis() - inicioEspera) < 100) {
    vTaskDelay(pdMS_TO_TICKS(1));
  }

  if (!tareaAudioPausadaParaNES) {
    portENTER_CRITICAL(&muxAudio);
    modoNESActivo = false;
    portEXIT_CRITICAL(&muxAudio);
    Serial.println("[MAX98357] NES audio: timeout esperando liberar I2S");
    return false;
  }

  i2s_zero_dma_buffer(I2S_PORT);

  Serial.println("[MAX98357] I2S cedido al APU NES (pausa cooperativa)");
  return true;
}

static void salirModoNES() {
  if (!modoNESActivo) return;

  // Vaciar restos PCM del APU y restaurar 44.1 kHz antes de devolver
  // el periférico a la tarea de audio general.
  i2s_zero_dma_buffer(I2S_PORT);
  if (sampleRateI2SActual != SAMPLE_RATE) {
    if (i2s_set_clk(
            I2S_PORT,
            SAMPLE_RATE,
            I2S_BITS_PER_SAMPLE_16BIT,
            I2S_CHANNEL_STEREO) == ESP_OK) {
      sampleRateI2SActual = SAMPLE_RATE;
    }
  }

  portENTER_CRITICAL(&muxAudio);
  frecuenciaActual = 0;
  reinicioSolicitado = true;
  modoNESActivo = false;
  portEXIT_CRITICAL(&muxAudio);

  // Esperar brevemente a que la tarea confirme que volvió.
  const uint32_t inicioEspera = millis();
  while (tareaAudioPausadaParaNES && (millis() - inicioEspera) < 100) {
    vTaskDelay(pdMS_TO_TICKS(1));
  }

  Serial.println("[MAX98357] I2S devuelto a COCODRILO OS");
}

static inline bool enModoNES() {
  return modoNESActivo;
}


// ------------------------------------------------------------
// Modo emulador genérico (GB/GBC, futuros cores).
// Reutiliza exactamente la cesión I2S probada por NES, sin cambiar
// el comportamiento de Anemoia.
// ------------------------------------------------------------
static inline bool entrarModoEmulador() {
  return entrarModoNES();
}

// Ajusta el reloj físico I2S mientras el emulador posee el bus.
// Útil para acompasar audio con cores que todavía corren por debajo
// de su refresh nominal sin bloquear ni re-muestrear en CPU.
static bool setSampleRateModoEmulador(uint32_t hz) {
  if (!modoNESActivo) return false;
  if (hz < 8000) hz = 8000;
  if (hz > 48000) hz = 48000;
  if (hz == sampleRateI2SActual) return true;

  const esp_err_t err = i2s_set_clk(
      I2S_PORT,
      hz,
      I2S_BITS_PER_SAMPLE_16BIT,
      I2S_CHANNEL_STEREO);

  if (err != ESP_OK) {
    Serial.printf("[MAX98357] cambio sample rate fallo: %d\n", (int)err);
    return false;
  }

  sampleRateI2SActual = hz;
  i2s_zero_dma_buffer(I2S_PORT);
  Serial.printf("[MAX98357] sample rate emulador: %u Hz\n", (unsigned)hz);
  return true;
}

static inline void salirModoEmulador() {
  salirModoNES();
}

// Compatibilidad con Galería WAV y Reproductor MP3.
// Ambos ya usan el mismo mecanismo cooperativo para tomar I2S_NUM_1.
static inline bool entrarModoGaleria() {
  return entrarModoNES();
}

static inline void salirModoGaleria() {
  salirModoNES();
}


// Escribe PCM estéreo S16 al mismo I2S_NUM_1 del MAX98357.
// "muestras" = cantidad total de int16_t (L,R,L,R...), que es
// precisamente lo que entrega Gnuboy en su callback.
static bool escribirPCMEstereo16(const int16_t *pcm, size_t muestras) {
  if (!pcm || muestras == 0 || !modoNESActivo) return false;

  uint8_t vol = volumenPaso;
  if (vol > 10) vol = 10;

  // 0..10 -> 0..100%. Procesamos por bloques para no reservar RAM grande.
  static int16_t temp[FRAMES_BLOQUE * 2];

  size_t pos = 0;
  while (pos < muestras) {
    size_t n = muestras - pos;
    if (n > (FRAMES_BLOQUE * 2)) n = FRAMES_BLOQUE * 2;

    if (!habilitado || vol == 0) {
      memset(temp, 0, n * sizeof(int16_t));
    } else {
      for (size_t i = 0; i < n; ++i) {
        // +10% para audio PCM de emuladores, con saturacion controlada.
        int32_t v = ((int32_t)pcm[pos + i] * (int32_t)vol * 11) / 100;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        temp[i] = (int16_t)v;
      }
    }

    size_t escritos = 0;
    const size_t bytes = n * sizeof(int16_t);
    esp_err_t err = i2s_write(
        I2S_PORT,
        temp,
        bytes,
        &escritos,
        portMAX_DELAY
    );

    if (err != ESP_OK || escritos != bytes) {
      contadorErroresI2S++;
      return false;
    }

    pos += n;
  }

  return true;
}

static inline void setVolumeStep(uint8_t paso) {
  if (paso > 10) paso = 10;

  bool cambio = false;

  portENTER_CRITICAL(&muxAudio);
  if (volumenPaso != paso) {
    volumenPaso = paso;
    reinicioSolicitado = true;
    cambio = true;
  }
  portEXIT_CRITICAL(&muxAudio);

  if (cambio) {
    Serial.printf("[MAX98357] Volumen digital: %u/10 (reset stream)\n",
                  (unsigned)paso);
  }
}

static inline uint8_t getVolumeStep() {
  return volumenPaso;
}

static inline void setEnabled(bool estado) {
  portENTER_CRITICAL(&muxAudio);
  const bool cambio = (habilitado != estado);
  habilitado = estado;

  if (!estado) {
    frecuenciaActual = 0;
  } else if (cambio) {
    reinicioSolicitado = true;
  }
  portEXIT_CRITICAL(&muxAudio);
}

static inline bool isEnabled() {
  return habilitado;
}

static inline void clickMetronomo(bool fuerte) {
  if (!iniciado) return;
  portENTER_CRITICAL(&muxAudio);
  clicSolicitado = fuerte ? 1 : 0;
  portEXIT_CRITICAL(&muxAudio);
}

static inline void tone(uint32_t frecuencia) {
  if (!iniciado) return;

  portENTER_CRITICAL(&muxAudio);
  frecuenciaActual = habilitado ? frecuencia : 0;
  portEXIT_CRITICAL(&muxAudio);
}

static inline void stop() {
  portENTER_CRITICAL(&muxAudio);
  frecuenciaActual = 0;
  portEXIT_CRITICAL(&muxAudio);
}

} // namespace AudioMAX98357

#endif
