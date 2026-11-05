# 🔌 Conexiones de COCODRILO OS Community

Mapa de GPIO de la configuración actual para **ESP32-S3 N16R8**, extraído de `staging/cocodrilo-os.ino`, `config/User_Setup.h`, `src/community/Bateria.h`, `AudioMAX98357.h` y `BacklightControl.h`.

> **Atención:** es el pinout del firmware, no un esquema eléctrico completo validado para cualquier módulo ESP32-S3. Verifica tensión, alimentación, tierra común, niveles lógicos y el pinout físico de cada módulo antes de cablear. Todos los GPIO trabajan con lógica de 3.3 V; no apliques 5 V a las entradas.

## Pantalla TFT ST7789 (SPI)

| Señal TFT | GPIO ESP32-S3 |
| --- | --- |
| MOSI / SDA (datos SPI) | **11** |
| MISO / SDO (si el módulo lo expone) | **13** |
| SCLK / SCK | **12** |
| CS | **10** |
| DC / A0 | **9** |
| RST / RES | **8** |
| BL (retroiluminación) | Mediante circuito MOSFET controlado por **GPIO21** |

La configuración corresponde a 240 × 320 píxeles con rotación horizontal. `TFT_eSPI` usa hasta 80 MHz en el bus SPI, por lo que las conexiones deben ser cortas y adecuadas. La alimentación de la pantalla depende del módulo concreto.

## Botones de control

| Función | GPIO |
| --- | --- |
| Arriba | **1** |
| Abajo | **16** |
| Izquierda | **7** |
| Derecha | **15** |
| A / OK | **2** |
| B / panel de volumen | **4** |
| MENU / pausa | **5** |

El código principal configura estos botones como `INPUT_PULLUP`: cada pulsador normalmente abierto se conecta **entre el GPIO correspondiente y GND**. Una pulsación se detecta cuando la entrada pasa a LOW.

**Nota sobre GPIO1:** el sketch conserva comentarios y rutinas antiguas para una cruceta analógica por resistencias. La inicialización principal actual utiliza cuatro botones digitales independientes. Esta guía documenta ese mapa digital.

**Nota sobre GPIO7:** algunos fragmentos heredados llaman `BUZZER_PIN=7`, pero el firmware principal reserva GPIO7 para **Izquierda** y vuelve a configurarlo como entrada. **No conectes simultáneamente un buzzer y el botón izquierdo al GPIO7.** El audio principal se maneja mediante I2S.

## Tarjeta microSD (SD_MMC, 1 bit)

| Señal microSD | GPIO |
| --- | --- |
| CLK | **39** |
| CMD | **38** |
| D0 / DAT0 | **40** |

El firmware usa `SD_MMC.setPins(39, 38, 40)` y `SD_MMC.begin("/sdcard", true)`. Esta conexión **no comparte** los pines SPI de la pantalla. Usa un lector compatible con SD_MMC a niveles de 3.3 V y las resistencias pull-up necesarias según tu módulo y la especificación SD; no todos los lectores SPI sirven para SD_MMC.

## Audio I2S

| Señal | GPIO | Destino |
| --- | --- | --- |
| BCLK / BCK | **47** | MAX98357A y PCM5102A |
| LRC / WS / LRCK | **18** | MAX98357A y PCM5102A |
| DATA / DIN | **41** | MAX98357A y PCM5102A |
| SD / ENABLE | **42** | MAX98357A |
| Detección de audífonos | **14** | Contacto de detección del jack |

El código activa el MAX98357A con GPIO42 en HIGH y lo silencia en LOW. La detección del jack usa HIGH = audífonos conectados. El DAC PCM5102A sigue recibiendo I2S cuando se silencia el amplificador.

El **altavoz** se conecta a las salidas del módulo MAX98357A según su hoja de datos, **no directamente a un GPIO**. Para audífonos, respeta la alimentación y el esquema de salida del módulo DAC y del jack.

## Batería MAX17048 (I²C)

| Señal | GPIO ESP32-S3 |
| --- | --- |
| SDA | **17** |
| SCL | **6** |
| Dirección I²C | **0x36** |

El MAX17048 utiliza un bus I²C dedicado configurado en el firmware. Su conexión a la batería y la alimentación del módulo dependen de la placa de medición. El sensor no reemplaza al cargador ni al circuito de protección.

El código reserva **GPIO5** para despertar del modo de suspensión mediante MENU, si esa funcionalidad está habilitada.

## Retroiluminación mediante MOSFET P

Esquema lógico descrito por `BacklightControl.h`:

```text
3.3 V ----- Source (MOSFET P)
              Drain -------- BL de la pantalla

GPIO21 -- 150 ohm -- Gate
                       |
                     100 kohm
                       |
                     3.3 V
```

La compuerta en LOW enciende el backlight; HIGH lo apaga. **Comprueba la tensión y corriente del LED de retroiluminación y la idoneidad del MOSFET**; el dibujo representa la intención del código, no sustituye el diseño eléctrico de la placa.

## Alimentación y precauciones

- Conecta **GND común** entre los módulos que comparten señales.
- No deduzcas el pin de alimentación a partir del número de GPIO. La alimentación puede variar entre módulos y placas.
- No conectes nunca una batería LiPo directamente al GPIO ni supongas que el MAX17048 integra carga o protección.
- Comprueba que el hardware real coincide con el esquema antes de encender.
- Para diferencias entre versiones, las definiciones de los archivos fuente son la referencia principal.

Consulta [COMPONENTES.md](COMPONENTES.md) para el listado de piezas y [config/User_Setup.h](config/User_Setup.h) para la configuración TFT.
