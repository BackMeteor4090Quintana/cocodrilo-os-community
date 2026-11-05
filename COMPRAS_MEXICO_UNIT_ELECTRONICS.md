# 🇲🇽 Componentes en México — UNIT Electronics

Esta página reúne **los modelos de componentes que Coqui utilizó para construir COCODRILO OS**, con enlaces a productos de [UNIT Electronics](https://uelectronics.com/) para quienes quieran conseguirlos en México.

> **Solo para personas que compran en México.** Es una ayuda local para encontrar piezas; **no es una tienda obligatoria ni una lista universal de compras**. Si estás en otro país, utiliza [COMPONENTES.md](COMPONENTES.md) para buscar los componentes equivalentes en tu región.

**Confirmación del creador:** Coqui ha indicado que los componentes enlazados corresponden a los modelos que utilizó en su proyecto. Los enlaces son referencias para comprarlos en México; no implican patrocinio ni garantizan existencias, precio o compatibilidad con variantes distintas. Verifica siempre dimensiones, tensión y pines antes de comprar.

## Electrónica principal

| Componente | Enlace en UNIT Electronics | Consideraciones |
| --- | --- | --- |
| **ESP32-S3 N16R8** | [ESP32-S3 Dual USB-C — seleccionar variante N16R8](https://uelectronics.com/producto/esp32-s3-dual-usb-c-tarjeta-de-expansion/) | **No elegir N8R2**. Comprueba el pinout de la placa y que tengas accesibles GPIO38, 39 y 40 para SD_MMC. La versión exacta de la tarjeta puede diferir de nuestra consola. |
| **Pantalla ST7789, 240 × 320** | [Módulo TFT ST7789V 2.4″ / 2.8″](https://uelectronics.com/producto/modulo-tft-display-st7789v-240x320-spi-2-4%E2%80%B3-2-8/) | Comprobar la **variante SPI**, pines, alimentación y dimensiones; las variantes tienen conectores distintos. |
| **MAX98357A, amplificador I2S** | [Amplificador mono I2S MAX98357, 3 W](https://uelectronics.com/producto/max98357-amplificador-de-audio-mono-i2s-clase-d-3w/) | Para bocina; comprobar BCLK, LRC, DIN y SD/ENABLE del módulo. |
| **Botones de control** | [Push Button Grande 12 × 12 × 7 mm](https://uelectronics.com/producto/push-button-grande-12x12x7mm/) | El firmware principal utiliza **siete botones**: cuatro direcciones, A, B y MENU. El tamaño puede cambiar según tu carcasa. |
| **Tarjeta microSD** | [Kingston microSD 16 / 32 / 64 GB](https://uelectronics.com/producto/memoria-micro-sd-kingston-16-64-gb-clase-10/) | Elige la capacidad y formato compatibles con tu lector. |

## Batería, audífonos y brillo

| Componente | Enlace en UNIT Electronics | Consideraciones |
| --- | --- | --- |
| **Medición MAX17048 + circuito de carga** | [UNIT Cargador de Batería LiPo Boost & I2C](https://uelectronics.com/producto/unit-cargador-de-bateria-lipo-boost-i2c/) | Incluye MAX17048 y cargador; comprueba el cableado y la configuración de tu versión, especialmente alimentación, cargador e I²C. Confirmar sus tensiones y el circuito de alimentación. |
| **MAX17048, solo circuito integrado** | [MAX17048G+T10 SMD](https://uelectronics.com/producto/max17048gt10-smd-dfn-8-ep2x2/) | Es un **chip SMD**, no un módulo listo para cables. No sustituye un cargador. |
| **Salida de audífonos PCM5102A** | [DAC PCM5102A I2S](https://uelectronics.com/producto/pcm5102a-dac-interfaz-i2s-para-reproductor-de-formato-phat/) | Opcional; comprobar alimentación y conexión del jack/detección. |
| **MOSFET P para brillo** | [FDN338P SMD](https://uelectronics.com/producto/mosfet-p-20v-1-6a-smd-fdn338p/) | Circuito de control descrito en el código; comprobar especificaciones. Puede no haber existencias. |

## También necesitarás

Un altavoz compatible con MAX98357A, cable USB de datos, conectores, cables, resistencias **150 Ω y 100 kΩ** para el circuito opcional de brillo y, para uso portátil, una batería de una celda con carga, protección y regulación adecuadas. No se indican enlaces a artículos concretos de estas piezas porque no se ha confirmado en el catálogo una opción exacta para nuestro montaje.

**No compres por error un lector microSD exclusivamente SPI para sustituir el lector SD_MMC configurado en el firmware.** El proyecto utiliza los pines **CLK=39, CMD=38 y D0=40** en modo de 1 bit; una placa que ya tenga ranura microSD puede no necesitar lector aparte.

## Documentación

- [Lista general de componentes](COMPONENTES.md)
- [Conexiones GPIO de la consola](CONEXIONES.md)
- [Hardware y compilación](DEPENDENCIAS_ESP32S3.md)

Esta página es una selección comunitaria de enlaces para México; **no es un patrocinio ni una colaboración oficial con UNIT Electronics**. Los enlaces fueron reconocidos por el creador como referencias a los modelos utilizados en su montaje.
