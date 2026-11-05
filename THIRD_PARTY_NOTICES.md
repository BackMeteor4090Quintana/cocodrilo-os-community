# Avisos de terceros — COCODRILO OS Community

**Estado: revisión de licencias pendiente. Este documento no aprueba todavía
la publicación ni distribución del firmware completo.**

## Componentes utilizados por el firmware actual

| Componente | Origen | Licencia y atribuciones |
| --- | --- | --- |
| **dr_mp3 v0.7.4** | [mackron/dr_libs](https://github.com/mackron/dr_libs/blob/master/dr_mp3.h) | David Reid; elección dominio público o **MIT-0**. Incluye código derivado de minimp3 bajo CC0. Se conserva el aviso completo en `src/community/dr_mp3.h`. |
| Retro-Go / Nofrendo (NES) | [ducalex/retro-go](https://github.com/ducalex/retro-go) | Proyecto general **GPLv2**; archivos Nofrendo (por ejemplo `nes/nes.c` y `cpu.c`) declaran **GNU Library GPL versión 2** de Matthew Conte. Conservar avisos por archivo y COPYING. |
| Retro-Go / Gnuboy (GB/GBC) | [ducalex/retro-go](https://github.com/ducalex/retro-go) | Proyecto general GPLv2; comprobar avisos por archivo de Gnuboy y COPYING. |
| TFT_eSPI | [Bodmer/TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) | BSD/FreeBSD y avisos de componentes heredados. |
| TJpg_Decoder | [Bodmer/TJpg_Decoder](https://github.com/Bodmer/TJpg_Decoder) | BSD/FreeBSD y avisos de ChaN. |
| lowzag 2.3.0 (portadas JPEG SOF2) | [lowzag/lowzag](https://github.com/lowzag/lowzag) | **MIT**, copyright (c) 2026 Tom (defensiblelogic.com); código C99. [Texto original](licenses/lowzag-MIT.txt). |
| Arduino-ESP32 3.2.1 | [Espressif/arduino-esp32](https://github.com/espressif/arduino-esp32) | La versión 3.2.1 se identifica como LGPL-2.1 en el registro de componentes de Espressif; algunos archivos pueden tener licencias propias. |
| ESP-IDF (incluido a través de Arduino-ESP32) | [Espressif/esp-idf](https://github.com/espressif/esp-idf) | Predomina Apache-2.0, con archivos bajo otras licencias y avisos específicos. Revisar componentes enlazados y posible excepción de bibliotecas de sistema. |

### Textos incluidos

- [Retro-Go GPLv2](licenses/Retro-Go-GPL-2.0.txt)
- [Nofrendo: GNU Library GPL versión 2](licenses/Nofrendo-LGPL-2.0.txt)
  — texto **distinto** de GPLv2, exigido por los encabezados de múltiples archivos.
- [Arduino-ESP32: LGPL versión 2.1](licenses/Arduino-LGPL-2.1.txt)
  — texto de referencia; determinar las condiciones exactas de la versión 3.2.1.
- [ESP-IDF: Apache 2.0](licenses/ESP-IDF-Apache-2.0.txt)
  — licencia principal de partes de ESP-IDF, no sustituye avisos de terceros.
- [TFT_eSPI](licenses/TFT_eSPI-license.txt)
- [TJpg_Decoder](licenses/TJpg_Decoder-license.txt)
- [lowzag MIT](licenses/lowzag-MIT.txt)
- **dr_mp3 MIT-0 / dominio público:** avisos íntegros al final de
  [dr_mp3.h](src/community/dr_mp3.h), sin eliminarlos ni cambiarlos.

## Sustitución del decodificador JPEG de portadas

El firmware actual **ya no incluye JPEGDEC**; se cambió por **lowzag**
para portadas JPEG progresivas (SOF2) a escala 1/8 y por TJpg_Decoder
para las portadas JPEG baseline. La biblioteca lowzag es MIT y se
almacena sin alterar en `src/community/lowzag.c` y `lowzag.h`, con
la adaptación de callbacks en `PortadaLowzag.h`.

**Falta una compilación y prueba real de portadas** (incluidos JPEG
progresivos grandes y cambio rápido de canción). La eliminación de
JPEGDEC del árbol actual reduce el conflicto Apache-2.0 identificado,
pero queda pendiente auditar otras dependencias y el historial Git.

## Sustitución del decodificador MP3

La integración Community cambió de **codec-helix** a **dr_mp3**. El firmware
actual no incluye `MP3DecoderHelix.h`, no enlaza código Helix/RPSL y
`.github/workflows/community-firmware-build.yml` dejó de descargar
`pschatzmann/codec-helix`. Por tanto, el conflicto concreto de
GPLv3 + RPSL del antiguo decodificador **se eliminó de la compilación actual**.

Este cambio no demuestra por sí solo que **todo el firmware** sea compatible
con GPLv2: deben revisarse también los avisos concretos de Retro-Go,
las bibliotecas restantes. La presencia de obras de
distintas licencias en el repositorio no implica automáticamente que puedan
combinarse y distribuirse en un único binario.

## Historial del conflicto JPEGDEC / GPLv2

Se verificó que JPEGDEC 1.6.1 declaraba Apache-2.0 y que Retro-Go
declara GPLv2 en su README, con avisos LGPLv2 en Nofrendo. Esto
motivó la sustitución por **lowzag MIT** en el código actual de Community.
**Esta sección es histórica:** JPEGDEC ya no es una dependencia del
workflow de compilación. Sus revisiones antiguas siguen en Git y
deben auditarse antes de hacer público el repositorio.

## Fuentes conservadas pero no compiladas en el firmware principal

| Componente | Licencia | Ubicación |
| --- | --- | --- |
| Anemoia-ESP32 | GPL-3.0 | `staging/anemoia/`, separado de la compilación principal |

- [Licencia completa de Anemoia](licenses/Anemoia-GPL-3.0.txt).

## Auditoría ampliada del script Retro-Go (4 de noviembre de 2026)

El script `tools/fetch_retrogo_gnuboy.sh` descarga en cada CI la
**rama por defecto sin fijar un commit**; copia todos los `.c` y `.h`
de `retro-core/components/gnuboy`, y el árbol completo
`retro-core/components/nofrendo` (salvo docs/CREDITS/CMakeLists.txt),
incluyendo aproximadamente 60 mappers. Se reproducen sus `COPYING`.

**Avisos vistos directamente**:
- El README de Retro-Go indica GPLv2 salvo excepciones. El fichero
  `gnuboy/COPYING` contiene GPLv2; los principales archivos C de Gnuboy
  no declaran una cláusula «or later» en sus encabezados examinados.
- Nofrendo (`nofrendo.c`, `nes/nes.c`, `cpu.c`, `ppu.c`,
  `apu.c`, `rom.c`, `dis.c` y mappers `map000.c`,
  `map004.c`, `map185.c`, `map231.c`) declara
  expresamente GNU Library GPL **versión 2** de Matthew Conte.
  La sección 3 de LGPLv2 autoriza optar por GPLv2 o posterior
  **para la copia correspondiente**, con las modificaciones
  de avisos que establece esa sección. No demuestra que todas
  las contribuciones posteriores de Retro-Go admitan esa opción.
- `nofrendo/database.h` declara procedencia de una base de datos
  Mesen/Nestopia/NesCartDB, y `gnuboy/tables.h` contiene paletas
  extraídas de BIOS de GBC. Auditar derechos y procedencia de
  estos datos por separado antes de distribuir.
- El script aplica optimizaciones O3, ajustes IRAM_ATTR, jump table
  6502 y parches PPU/OAM. Son **modificaciones al código de terceros**
  que deben preservarse en la fuente redistribuida.

**Estado:** no se ha encontrado un permiso general GPLv3 para todo el
código GNUBOY/Retro-Go. Tampoco hay incompatibilidad demostrada **entre
los núcleos NES y GB/GBC por sí solos**. Queda por identificar las
condiciones de todos los archivos copiados, fijar la versión y auditar
los datos de origen externo.

## Importancia de la plataforma Arduino-ESP32 / ESP-IDF

Este punto es **alto para distribuir binarios**. Arduino-ESP32
incluye código LGPL-2.1 y su base ESP-IDF contiene mayoritariamente
código Apache-2.0, además de bibliotecas BSD/MIT y otros avisos.
No se puede concluir compatibilidad del firmware unido GPLv2-only +
Apache-2.0 sin identificar los objetos y condiciones efectivamente
enlazados y evaluar si hay una excepción válida de bibliotecas del
sistema u otra autorización. No hay motivo para sustituir el core
ni auditar cada herramienta que solo se usa para construir: el
alcance es **el código y los objetos redistribuidos dentro del BIN**
y las obligaciones aplicables de fuente/atribuciones.

Referencias:
- https://www.espressif.com/en/products/sdks/esp-idf
- https://github.com/espressif/esp-idf/blob/master/docs/en/COPYRIGHT.rst
- https://www.gnu.org/licenses/gpl-faq.en.html#SystemLibraryException
- https://opensource.org/license/lgpl-2-0

## Verificación específica NES/GBC (4 de noviembre de 2026)

**Decisión técnica provisional:** conservar los núcleos y sus funciones
sin cambios mientras el proyecto continúe en desarrollo privado. No
aprobar todavía la redistribución pública de ese firmware combinado.

- Retro-Go declara GPLv2 como regla general; sus excepciones expresas
  incluyen componentes distintos de Gnuboy y Nofrendo. Gnuboy
  conserva `COPYING` GPLv2 y Nofrendo contiene encabezados GNU
  Library GPLv2. No se detectó una incompatibilidad nueva
  **entre ambos núcleos por sí mismos**.
- `retro-core/components/nofrendo/database.h` contiene una base de
  identificadores CRC y configuraciones de cartuchos, expresamente
  derivada de MesenDB, Nestopia, NesCartDB y NewRisingSun. El código
  `nes/rom.c` la incluye y consulta: **sí es parte funcional del
  núcleo NES actual**, no un recurso opcional sin uso.
  El repositorio fuente `SourMesen/Mesen` declara GPLv3 o posterior
  para Mesen. No se halló un aviso específico de licencia en el
  encabezado de `MesenDB.txt`; esto **no basta para asegurar** que
  la base de datos esté bajo GPLv3, pero tampoco que se pueda
  redistribuir bajo GPLv2. Dado que combina aportaciones de distintas
  fuentes, hay que pedir aclaración de derechos o localizar una
  base alternativa con permiso claro antes de publicar.
- `retro-core/components/gnuboy/tables.h` contiene las tablas de
  color de la implementación. Sus comentarios atribuyen las paletas
  específicas de juegos a las del BIOS de GBC. Son valores de
  colores y no hay evidencia en este examen de que el proyecto
  contenga una ROM BIOS completa. El origen plantea una duda
  de derechos de datos que merece aclaración, sin inferir
  automáticamente infracción o prohibición de redistribución.
- Las optimizaciones/perfiles aplicados en
  `tools/fetch_retrogo_gnuboy.sh` no cambian los permisos de terceros
  y deben distribuirse como modificaciones al código GPL/LGPL.

**Acción recomendada:** conservar implementación funcional para
pruebas privadas; solicitar confirmación del permiso de los datos
de `database.h` y de las paletas antes de publicar; no eliminar
archivos de soporte sin pruebas de regresión de juegos, guardados
y colores. También sigue pendiente la revisión del enlace
GPLv2 / ESP-IDF Apache-2.0.

Fuentes:
- https://github.com/ducalex/retro-go/blob/master/README.md#license
- https://github.com/ducalex/retro-go/blob/master/retro-core/components/nofrendo/database.h
- https://github.com/ducalex/retro-go/blob/master/retro-core/components/gnuboy/tables.h
- https://github.com/SourMesen/Mesen/blob/master/README.md
- https://github.com/SourMesen/Mesen/blob/master/GUI.NET/Dependencies/MesenDB.txt

## Licencias que aún impiden cerrar la auditoría

**Finalidad del proyecto:** COCODRILO OS Community es DIY, gratuito y
orientado a compartir conocimiento con la cultura maker. Esa intención
no exime del cumplimiento de GPL/LGPL/MIT/BSD/Apache al distribuir
el código o un firmware, ni permite añadir una prohibición de
reventa a los componentes ajenos bajo esas licencias.

**Textos que faltaban y ahora están archivados:**
la **GNU Library GPL v2** específica de Nofrendo, la **LGPL 2.1**
de Arduino-ESP32 y la **Apache 2.0** de partes de ESP-IDF.
La carpeta `licenses/` proporciona textos de referencia, no una
certificación automática de compatibilidad.

**Pendiente / prioridad:**

1. **Crítico antes de distribución pública de BIN:** determinar las
   licencias exactas de los componentes ESP-IDF enlazados con el
   código GPLv2 y si existe una excepción o permiso aplicable.
   La excepción de bibliotecas de sistema no se debe dar por válida
   automáticamente para firmware embebido.
2. **Crítico antes de distribución de fuentes o BIN:** identificar las
   condiciones de las aportaciones posteriores en Nofrendo y Gnuboy.
   En Nofrendo hay encabezados Library GPLv2 junto a un `COPYING`
   GPLv2 en Retro-Go; ambos textos se deben identificar por separado.
3. **Crítico para datos redistribuidos:** revisar la procedencia y
   derechos sobre `nofrendo/database.h` (Mesen/Nestopia/NesCartDB)
   y `gnuboy/tables.h` (tablas de paletas atribuidas al BIOS GBC);
   también otros gráficos, fuentes y efectos de sonido del proyecto.
4. **Obligatorio para aportaciones nuevas:** elegir una licencia para
   los archivos originales de COCODRILO OS Community y delimitar
   el material de terceros. Aún no existe un `LICENSE` raíz.
   No se asignó ninguna licencia sin consentimiento del autor.
5. **Obligatorio para reproducibilidad:** fijar revisiones de las
   dependencias descargadas por el script de emuladores, preservar
   los cambios locales y reunir los avisos de sus versiones exactas.
6. **Independiente de la licencia del BIN:** revisar todo el historial,
   ramas y archivos multimedia antes de cambiar el repositorio
   privado a público.

**Hechos comprobados:** Retro-Go declara GPLv2 como licencia general,
con excepciones (por ejemplo fMSX bajo términos no comerciales, que no
figura entre los dos núcleos copiados por el script actual); los
archivos de Nofrendo revisados citan GNU Library GPL versión 2.
El archivo `idf_component.yml` de Arduino-ESP32 declara LGPL-2.1;
el catálogo oficial de la versión 3.2.1 también la identifica así.
ESP-IDF declara Apache-2.0 como licencia del código original, con
múltiples excepciones y componentes de terceros.

**Fuentes para contrastar:**
- https://github.com/ducalex/retro-go/blob/master/README.md#license
- https://opensource.org/license/lgpl-2-0
- https://components.espressif.com/components/espressif/arduino-esp32/versions/3.2.1
- https://github.com/espressif/esp-idf/blob/master/docs/en/COPYRIGHT.rst
- https://www.gnu.org/licenses/gpl-faq.en.html#SystemLibraryException

## Obligaciones pendientes para lanzar una versión pública

1. Comprobar licencia exacta de cada archivo compilado y todos los recursos;
   preservar autores y avisos, incluso en el ZIP/BIN cuando corresponda.
2. Fijar las revisiones de Retro-Go y demás dependencias para reproducibilidad.
3. Confirmar que se puede distribuir el firmware combinado respetando
   GPLv2, LGPLv2, MIT y licencias de la plataforma.
4. Publicar código fuente correspondiente, modificaciones y scripts de
   compilación exigidos por las licencias aplicables.
5. Elegir licencia explícita para el código original de Cocodrilo OS y
   documentar por separado los gráficos de autoría propia.
6. Verificar compilación, rendimiento MP3 y hardware real antes de distribuir.

Esto es un registro técnico de diligencia, no un dictamen jurídico.
