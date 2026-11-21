# Migración experimental GB/GBC → binjgb

**Estado:** fase de preparación; el firmware publicado sigue usando Gnuboy. Este directorio no se copia al sketch por el workflow y no modifica los BIN.

## Objetivo

Conservar compatibilidad con ROMs `.gb` y `.gbc` en ESP32-S3 N16R8, audio MAX98357A/I2S, controles actuales, pantalla ST7789, guardados y el menú dentro del juego. Sustituir el núcleo solo después de demostrar la compilación y el funcionamiento en la consola.

**Origen candidato:** [binji/binjgb](https://github.com/binji/binjgb), emulador C GB/GBC bajo MIT. Su README reconoce soporte CGB todavía imperfecto; el rendimiento en ESP32-S3 no está verificado.

## Integración actual identificada

- `staging/emuladores/COCODRILO_Emuladores.h`: navegador de ROMs `/gb` y `/gbc`; invoca `CocodriloGBC::ejecutar(ruta, tft)`.
- `staging/emuladores/COCODRILO_GBC_RetroGo.h`: adaptador Gnuboy, controles, escalado 160×144 a 266×240, audio asíncrono y menús de guardado.
- `src/community/AudioMAX98357.h`: salida PCM I2S.
- `tools/fetch_retrogo_gnuboy.sh`: descarga los núcleos existentes; **no modificar aún**.
- `.github/workflows/community-firmware-build.yml`: prepara el sketch y publica BIN en `firmware-builds`; **no activar binjgb aquí hasta compilar y validar**.

## Contrato de compatibilidad

1. Mantener el navegador y las rutas SD existentes; nueva implementación de `ejecutar(const char*, TFT_eSPI&)` solo al habilitar explícitamente el núcleo nuevo.
2. No modificar el mapeo físico de botones ni perder MENU+B (menú), MENU+A (SELECT) o MENU (START).
3. Producir RGB565 160×144 para conservar inicialmente el blitter y la visualización.
4. Convertir y alimentar audio PCM en el formato que espera `AudioMAX98357::escribirPCMEstereo16`.
5. Guardar SRAM por juego; considerar compatibilidad de formato antes de leer/sobrescribir partidas creadas por Gnuboy. Guardados de estado de Gnuboy **no son intercambiables** por defecto.
6. Respetar memoria interna/PSRAM, DMA y liberación completa al salir al lanzador.

## Primer experimento reproducible

```bash
# Desde la raíz del repositorio:
git ls-remote https://github.com/binji/binjgb.git HEAD
# Copia la SHA de 40 caracteres que obtengas:
bash tools/prepare_binjgb_experiment.sh <SHA_COMPLETA>
```

Esto descarga el código upstream a un directorio temporal independiente del sketch, verifica el commit y conserva su licencia para la auditoría. **No modifica `main`, los archivos del firmware ni las ramas remotas.** No importaremos automáticamente todo el árbol ni sus submódulos: primero hay que identificar qué fuentes de la CPU/PPU/APU son autocontenidas y qué bibliotecas son solo del frontend SDL.

## Criterios de aceptación antes de migrar

- [ ] Identificar la API de carga de ROM, stepping de fotogramas, video, entradas, audio y persistencia; revisar licencias de dependencias.
- [ ] Crear adaptador binjgb aislado, con compilación optativa y Gnuboy por defecto.
- [ ] Compilar ESP32-S3 N16R8 sin alterar la publicación estable.
- [ ] Ejecutar ROMs de prueba homebrew GB y GBC; validar pantalla, controles, sonido, SRAM y salida al menú.
- [ ] Medir FPS reales, tiempos de emulación/video y RAM/PSRAM en consola.
- [ ] Activar el nuevo núcleo solo cuando las pruebas sean satisfactorias; verificar la distribución del código correspondiente y los avisos.

**Nota:** MIT del nuevo núcleo no resuelve por sí sola la situación de licencias del firmware NES ni la de todos los componentes enlazados. No retirar avisos o fuentes de Gnuboy mientras haya BIN existentes derivados de él.
