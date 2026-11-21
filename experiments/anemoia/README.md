# Recuperación de Anemoia para NES — fase 1

**Estado:** preparación y revisión, sin activar el núcleo en el firmware público. Esta carpeta no participa en el build de GitHub Actions.

## Objetivo

Evaluar el regreso de [Anemoia-ESP32](https://github.com/Shim06/Anemoia-ESP32) como núcleo NES de COCODRILO OS Community en ESP32-S3 N16R8, **sin perder los BIN estables actuales**, ni la experiencia de menú, audio, guardados y controles.

## Código ya disponible en este repositorio

- `staging/anemoia/COCODRILO_NES_Anemoia.h`: adaptador existente con controles, menú, gestión de cartucho, pantalla y audio.
- `staging/anemoia/src/core/`: fuentes existentes del núcleo Anemoia.
- `staging/anemoia/src/config.h`: configuración previa, con FRAMESKIP desactivado para diagnóstico.
- `src/community/AudioMAX98357.h`: I2S legacy, necesario para compatibilidad con la ruta previa de Anemoia.
- `staging/emuladores/COCODRILO_NES_RetroGo.h`: implementación actual funcional, **no sustituir aún**.
- `staging/cocodrilo-os.ino`: lanzador actual; **no modificar en esta fase**.
- `.github/workflows/community-firmware-build.yml`: build y publicación de BIN estable; **no activar Anemoia aquí en esta fase**.

## Comprobaciones importantes

1. **Licencias antes de enlazar:** Anemoia se identifica como GPLv3 y Gnuboy como GPLv2 en las notas del proyecto. **No enlazar ambos núcleos en el mismo firmware mientras no se verifique que las licencias exactas de los archivos permitan la combinación.** El cambio NES y la sustitución posterior de Gnuboy por un núcleo MIT GB/GBC podrían formar una migración coordinada. Esta carpeta es solo de preparación, no un certificado de compatibilidad legal.
2. **Audio:** revisar I2S_NUM_0 del APU Anemoia frente al bus I2S_NUM_1 de COCODRILO; conservar gestión cooperativa del MAX98357A y salida por audífonos.
3. **Pantalla:** verificar su framebuffer, RGB565, coordenadas y ruta DMA/SPI sin romper los menús TFT_eSPI.
4. **Controles:** preservar D-pad, A/B, MENU corto como START, MENU+A como SELECT y MENU+B sostenidos para menú.
5. **Guardados:** el adaptador Anemoia guarda estados usando CRC32. No asumir compatibilidad con estados de Nofrendo.
6. **Memoria:** conservar las medidas anteriores para Cartridge y Bus en heap, no en stack; verificar RAM/PSRAM.
7. **Rendimiento:** medir FPS en ESP32-S3, sin equiparar FPS objetivo al rendimiento observado.

## Orden de trabajo

- [x] Identificar el adaptador Anemoia y las fuentes ya existentes.
- [x] Identificar el lanzador NES estable y los puntos de integración del audio.
- [ ] Auditar las dependencias y símbolos necesarios para compilar Anemoia aislado.
- [ ] Preparar una compilación de diagnóstico *sin publicar BIN y sin cambiar la ruta estable*.
- [ ] Resolver compatibilidad de licencias entre todos los núcleos del firmware final.
- [ ] Validar ROM homebrew NES, sonido, guardados, pausa, regreso al launcher y FPS.
- [ ] Integrar en la compilación pública únicamente cuando exista una combinación redistribuible y probada.

## GB/GBC

Se mantiene `experiments/binjgb/` en pausa. **GBC sigue siendo obligatorio**. La implementación Gnuboy existente y los BIN históricos no se borran ni se modifican por esta fase.


## Recuperación técnica incorporada (21-11-2026)

Se incorporaron dos archivos operativos en `main`:

- [`tools/prepare_anemoia_diagnostic.sh`](../../tools/prepare_anemoia_diagnostic.sh): ensambla fuera del checkout un sketch Arduino con **todas las fuentes del núcleo Anemoia ya adaptado**, la licencia GPLv3 y la tabla de particiones de COCODRILO OS. No copia Gnuboy ni Nofrendo.
- [`.github/workflows/anemoia-diagnostic.yml`](../../.github/workflows/anemoia-diagnostic.yml): compilación **manual** del núcleo para ESP32-S3, Arduino-ESP32 3.2.1, TFT_eSPI 2.5.43, flash 16 MB y PSRAM OPI. No publica resultados en `firmware-builds` ni instala firmware.

**Ejecutar la verificación:** GitHub → Actions → **Anemoia NES diagnostic (no publish)** → Run workflow → `main`. El resultado de Actions determina si la compilación del núcleo está correcta; incorporar este workflow no equivale a haberla ejecutado.

**Fork revisado:** [rkindustriesYT/Anemoia-NES-Emulator-fixes](https://github.com/rkindustriesYT/Anemoia-NES-Emulator-fixes), que conserva la licencia GPLv3 de Anemoia. Sus arreglos de reinicio, configuración, pila y video son una referencia, pero nuestro núcleo tiene modificaciones extensas de COCODRILO en APU, bus, PPU, pantalla y audio; **no se sobrescribieron** sin comparar los cambios concretos.

**Límite de esta verificación:** comprueba que las fuentes pueden compilar y enlazarse en un sketch aislado; todavía no verifica el adaptador `COCODRILO_NES_Anemoia.h`, el lanzador, la reproducción de juegos, el audio MAX98357 ni los FPS. La migración definitiva sigue bloqueada hasta tener una integración completa probada y legalmente compatible.


## Firmware de prueba jugable NES-only

Se preparó la primera **versión de prueba independiente** de Anemoia, diferente del simple diagnóstico de compilación anterior:

- [Generador `tools/prepare_anemoia_playtest.sh`](../../tools/prepare_anemoia_playtest.sh): crea el sketch `AnemoiaPlaytest.ino` con el adaptador `COCODRILO_NES_Anemoia.h`, el núcleo adaptado, audio I2S de COCODRILO y la partición de 16 MB.
- [Workflow `anemoia-playtest.yml`](../../.github/workflows/anemoia-playtest.yml): ejecución manual `workflow_dispatch`; compila y adjunta archivos `.bin` y licencia GPLv3 a un **artefacto de Actions**, sin publicar ni sobrescribir la rama `firmware-builds`.

**Usar:** Actions → «Anemoia NES playtest (manual, no publish)» → Run workflow. Si sale verde, descargar el artefacto `cocodrilo-anemoia-nes-playtest` desde la ejecución. **El workflow aún tiene que ejecutarse y verificar su éxito: no afirmar BIN listo antes de la compilación.**

### Funcionalidad incluida en esta versión experimental

- ESP32-S3 N16R8, TFT_eSPI ST7789 (320×240 apaisado) con los pines `config/User_Setup.h`.
- SD_MMC 1-bit, CLK GPIO39, CMD GPIO38, D0 GPIO40. Navega `/NES/*.nes` (hasta 80 ROMs) con ARRIBA/ABAJO y abre con A.
- Dentro de NES: A/B, cruceta, MENU corto como START, MENU+A como SELECT, MENU+B sostenidos ~800 ms para menú de pausa, guardar, cargar o salir (adaptador original).
- MAX98357A / PCM5102A por I2S legacy de la consola; línea Serie a 115200 para medir FPS (cada 2 s).
- La aplicación de prueba no contiene Gnuboy ni Nofrendo y no modifica el firmware de producción. Binjgb/GB/GBC están **fuera de este BIN experimental**.

### Precauciones y pendientes

Esta versión experimental **no es el firmware completo de COCODRILO OS**. Instalarla en la placa sustituiría temporalmente la aplicación actual: **respaldar antes el firmware y datos importantes**. No se ha flasheado ni medido FPS en hardware y no se ha verificado todavía la compilación del playtest.

El código Anemoia existente usa estados de guardado por CRC32 en `/states`. No asumir que son compatibles con otros emuladores; hacer copia de esos estados antes de probar. No flashar esta prueba hasta confirmar que la compilación es exitosa y se comprende el procedimiento de instalación.

Las licencias del núcleo y del firmware de prueba se deben revisar antes de una distribución más amplia; el archivo GPLv3 del proyecto Anemoia se incluye en el artefacto. El firmware público principal sigue funcionando con sus núcleos anteriores y los BIN permanecen intactos.
