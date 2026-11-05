# 🐊 Componentes de COCODRILO OS Community

Esta lista corresponde al hardware identificado en el código fuente de **COCODRILO OS Community** para ESP32-S3 N16R8. Distingue los elementos principales de los opcionales y evita recomendar modelos o conexiones que el proyecto no documenta.

## Componentes principales

| Cantidad | Componente | Uso |
| --- | --- | --- |
| 1 | **ESP32-S3 N16R8** (16 MB de flash, 8 MB de PSRAM OPI) | Microcontrolador principal |
| 1 | **Pantalla TFT ST7789 240 × 320** con interfaz SPI | Interfaz gráfica horizontal (320 × 240) |
| 1 | **Lector o ranura microSD compatible con SD_MMC de 1 bit** | Música, imágenes, archivos y juegos |
| 1 | **Tarjeta microSD** | Almacenamiento de archivos del usuario |
| 7 | **Pulsadores normalmente abiertos** | Arriba, abajo, izquierda, derecha, A/OK, B/volumen y MENU |
| 1 | **Amplificador I2S MAX98357A** | Audio hacia altavoz |
| 1 | **Altavoz compatible con la salida del amplificador** | Reproducción de sonido |
| 1 | **Cable USB de datos y alimentación compatible con la placa** | Programación y pruebas |

Los botones de la configuración principal se leen con `INPUT_PULLUP` y son activos en nivel bajo. El hardware físico debe utilizar el voltaje lógico apropiado para el ESP32-S3.

## Alimentación y control de batería

| Cantidad | Componente | Uso |
| --- | --- | --- |
| 1 | **Medidor de batería MAX17048** | Porcentaje y voltaje de batería por I²C |
| 1 | **Batería Li-ion/LiPo de una celda**, si la consola es portátil | Alimentación portátil |
| Según diseño | **Circuito de carga, protección y regulación apropiado** | Gestión segura de energía |

**Importante:** el software identifica el MAX17048, pero no documenta un cargador ni una placa de protección concreta. El MAX17048 **no carga ni protege la batería**. La elección y el cableado de carga, protección y alimentación deben verificarse para la placa usada. No conectes una celda directamente a un pin de 3.3 V ni al USB.

## Brillo de pantalla

El firmware contempla control PWM de retroiluminación con:

- 1 MOSFET de canal P de lado alto, **FDN338P o equivalente adecuado**.
- 1 resistencia de **150 Ω** para la compuerta.
- 1 resistencia de **100 kΩ** de pull-up de compuerta a 3.3 V.

Este circuito está descrito en [`src/community/BacklightControl.h`](src/community/BacklightControl.h). Verifica la compatibilidad del panel y del MOSFET antes de montarlo.

## Audio por audífonos (opcional, contemplado en el código)

| Cantidad | Componente | Uso |
| --- | --- | --- |
| 1 | **DAC I2S PCM5102A** | Salida de audio hacia audífonos |
| 1 | **Jack de audífonos con contacto de detección** | Cambiar entre bocina y audífonos |
| Según montaje | Cableado, conectores y componentes de alimentación del módulo | Integración física |

El MAX98357A y el PCM5102A comparten BCLK, WS y datos I2S. El firmware apaga la salida del MAX98357A cuando detecta audífonos. El jack debe proporcionar la señal de detección usada por el código.

## Material de montaje

Cables de conexión, conectores, placa de montaje o PCB, tornillos, soporte y carcasa según la construcción. Sus cantidades no están definidas en el firmware.

## ¿Qué no está especificado?

El código no basta para identificar un modelo exacto de placa ESP32-S3, cargador, batería, regulador, jack o altavoz, ni para garantizar que todos esos elementos estén incluidos en un kit. Debes comprobar sus fichas técnicas, niveles de tensión y consumo.

Consulta [CONEXIONES.md](CONEXIONES.md) para el pinout obtenido del firmware y [DEPENDENCIAS_ESP32S3.md](DEPENDENCIAS_ESP32S3.md) para las bibliotecas de software.

**No se incluyen ROMs, BIOS ni archivos comerciales.**
