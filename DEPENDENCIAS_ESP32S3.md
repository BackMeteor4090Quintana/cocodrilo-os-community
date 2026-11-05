# Hardware y compilación — COCODRILO OS Community

Esta guía describe el entorno de desarrollo utilizado por COCODRILO OS Community para **ESP32-S3 N16R8**.

## Hardware objetivo

| Componente | Configuración |
| --- | --- |
| Microcontrolador | ESP32-S3 |
| Flash | 16 MB |
| PSRAM | 8 MB, OPI |
| Pantalla | ST7789, 240 × 320 píxeles en orientación horizontal |
| TFT MOSI / MISO / SCLK | GPIO 11 / 13 / 12 |
| TFT CS / DC / RST | GPIO 10 / 9 / 8 |

La configuración de pantalla usada por el proyecto está en [`config/User_Setup.h`](config/User_Setup.h). Las conexiones de otros periféricos deben coincidir con las del hardware para el que se diseñó el firmware.

## Dependencias

| Componente | Versión o fuente |
| --- | --- |
| Arduino-ESP32 | 3.2.1 |
| TFT_eSPI | 2.5.43 |
| TJpg_Decoder | 1.1.0 (referencia del proyecto) |
| dr_mp3 | 0.7.4, incluido en `src/community/dr_mp3.h` |
| lowzag | 2.3.0, incluido en `src/community/` |
| Retro-Go / Nofrendo / Gnuboy | Preparados por `tools/fetch_retrogo_gnuboy.sh` |

Otras bibliotecas como `WiFi`, `WebServer`, `DNSServer`, `SD_MMC`, `Preferences`, `SPI` y `FS` se distribuyen con el núcleo Arduino-ESP32.

## Compilación

La referencia reproducible del proceso se encuentra en [`.github/workflows/community-firmware-build.yml`](.github/workflows/community-firmware-build.yml). El workflow prepara el sketch a partir de `staging/cocodrilo-os.ino`, `src/community/`, adaptadores y recursos, instala dependencias y ejecuta `arduino-cli compile`.

La configuración de placa utilizada por el workflow es:

```text
esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=custom,PSRAM=opi
```

El proyecto utiliza [`config/partitions.csv`](config/partitions.csv) como tabla personalizada de particiones.

Para revisar referencias locales de dependencias:

```bash
python3 tools/check_dependencies.py
```

Esta comprobación estática no sustituye una compilación real.

## Instalación y compatibilidad

La rama [`firmware-builds`](../../tree/firmware-builds) alberga los artefactos publicados por el workflow. Antes de flashear, comprueba que el binario sea de una compilación satisfactoria y corresponda a la pantalla, memoria y conexiones de tu placa.

Las funciones de emulación, portadas y audio pueden variar entre versiones del firmware. Las adaptaciones de terceros mantienen sus licencias: consulta [avisos de terceros](THIRD_PARTY_NOTICES.md) y [emulación](EMULADORES_Y_LICENCIAS.md).
