# Avisos de terceros — COCODRILO OS Community

COCODRILO OS Community combina código original de Coqui, ofrecido bajo [MIT](LICENSE), con bibliotecas y componentes de otros proyectos. **Cada componente de terceros conserva sus propios derechos y condiciones de licencia**, incluidos sus avisos de copyright.

## Componentes y atribuciones

| Componente | Origen | Condiciones identificadas |
| --- | --- | --- |
| Retro-Go y Gnuboy (GB/GBC) | [ducalex/retro-go](https://github.com/ducalex/retro-go) | Retro-Go declara GPL v2 en general, con excepciones por componente; Gnuboy incluye su `COPYING` GPL v2 |
| Nofrendo (NES) | [Retro-Go](https://github.com/ducalex/retro-go) / autores originales | Varios archivos citan GNU Library GPL versión 2 de Matthew Conte; otros avisos pueden corresponder a contribuciones adicionales |
| Anemoia-ESP32 | [Shim06/Anemoia-ESP32](https://github.com/Shim06/Anemoia-ESP32) | GPL v3; código mantenido por separado en `staging/anemoia/` |
| TFT_eSPI | [Bodmer/TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) | BSD/FreeBSD y avisos de componentes incluidos |
| TJpg_Decoder | [Bodmer/TJpg_Decoder](https://github.com/Bodmer/TJpg_Decoder) | BSD/FreeBSD y avisos de ChaN |
| dr_mp3 0.7.4 | [mackron/dr_libs](https://github.com/mackron/dr_libs) | David Reid, MIT-0 o dominio público; incluye partes derivadas de minimp3 bajo CC0 |
| lowzag 2.3.0 | [lowzag/lowzag](https://github.com/lowzag/lowzag) | MIT; copyright © 2026 Tom (defensiblelogic.com) |
| Arduino-ESP32 3.2.1 | [Espressif](https://github.com/espressif/arduino-esp32) | LGPL 2.1 para el componente declarado; pueden existir licencias distintas por archivo |
| ESP-IDF | [Espressif](https://github.com/espressif/esp-idf) | Principalmente Apache 2.0, además de otras licencias por componente |

## Textos de licencias

- [Retro-Go — GPL v2](licenses/Retro-Go-GPL-2.0.txt)
- [Nofrendo — GNU Library GPL v2](licenses/Nofrendo-LGPL-2.0.txt)
- [Arduino-ESP32 — LGPL 2.1](licenses/Arduino-LGPL-2.1.txt)
- [ESP-IDF — Apache 2.0](licenses/ESP-IDF-Apache-2.0.txt)
- [TFT_eSPI](licenses/TFT_eSPI-license.txt)
- [TJpg_Decoder](licenses/TJpg_Decoder-license.txt)
- [lowzag — MIT](licenses/lowzag-MIT.txt)
- [Anemoia — GPL v3](licenses/Anemoia-GPL-3.0.txt)
- [dr_mp3](src/community/dr_mp3.h) — avisos incorporados en la cabecera

Los avisos incluidos en cada archivo fuente prevalecen para ese archivo. Este índice no sustituye las licencias originales.

## Distribución de código y firmware

La integración de los emuladores aplica modificaciones a código externo mediante [`tools/fetch_retrogo_gnuboy.sh`](tools/fetch_retrogo_gnuboy.sh). Las obligaciones de publicación del código fuente correspondiente y las atribuciones siguen siendo aplicables cuando correspondan.

La combinación de fuentes GPL v2 y componentes Apache 2.0 de ESP-IDF puede plantear incompatibilidad en determinados casos de enlace y redistribución; la presencia de bibliotecas con licencias diferentes **no implica automáticamente** que el firmware completo sea redistribuible. Es necesario evaluar las versiones y los objetos enlazados, así como las excepciones aplicables.

Algunos datos de emulación también requieren una revisión específica de derechos: `nofrendo/database.h` incluye identificadores y configuraciones con aportaciones de otras bases (Mesen, Nestopia, NesCartDB), y `gnuboy/tables.h` atribuye paletas de color a datos de Game Boy Color. No debe presumirse una autorización de redistribución independiente por el hecho de estar incluidos en un núcleo.

El código Anemoia bajo GPL v3 permanece separado de la compilación principal Retro-Go. Su licencia no reemplaza ni modifica la GPL v2 de otros proyectos.

No se incluyen ROMs, BIOS ni juegos comerciales en este repositorio por diseño.

## Referencias

- [Retro-Go: repositorio y licencia](https://github.com/ducalex/retro-go)
- [Licencias y copyright de ESP-IDF](https://github.com/espressif/esp-idf/blob/master/docs/en/COPYRIGHT.rst)
- [Compatibilidad de licencias GNU](https://www.gnu.org/licenses/gpl-faq.html)
- [Licencia Apache 2.0](https://www.apache.org/licenses/LICENSE-2.0)

Este documento registra atribuciones y cuestiones de cumplimiento; no concede permisos que correspondan a titulares de terceros ni constituye un dictamen jurídico.
