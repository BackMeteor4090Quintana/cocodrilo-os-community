# Emulación NES, Game Boy y Game Boy Color

COCODRILO OS Community incorpora adaptadores para emulación en ESP32-S3, con integración de controles, pantalla, audio y menús.

## Componentes

| Sistema | Proyecto de origen | Integración |
| --- | --- | --- |
| NES | Nofrendo, distribuido mediante [Retro-Go](https://github.com/ducalex/retro-go) | `staging/emuladores/COCODRILO_NES_RetroGo.h` y archivos de menú |
| Game Boy / Game Boy Color | Gnuboy, distribuido mediante [Retro-Go](https://github.com/ducalex/retro-go) | `staging/emuladores/COCODRILO_GBC_RetroGo.h` |
| NES alternativo | [Anemoia-ESP32](https://github.com/Shim06/Anemoia-ESP32) | Código separado en `staging/anemoia/`, no incluido en la compilación principal |

Los archivos `staging/emuladores/COCODRILO_Emuladores.h`, `COCODRILO_NES_RetroGo_Menu.h` y `COCODRILO_NES_RetroGo_Menu.cpp` contienen funciones auxiliares de selección, pausa y guardado.

El script [`tools/fetch_retrogo_gnuboy.sh`](tools/fetch_retrogo_gnuboy.sh) descarga los núcleos Retro-Go y aplica adaptaciones para ESP32-S3, incluidas optimizaciones y compatibilidad de memoria. El script utiliza la rama remota de Retro-Go sin fijar un commit, por lo que las versiones obtenidas podrían cambiar.

## Licencias y autores

- **Retro-Go / Gnuboy:** el proyecto Retro-Go indica GPL v2 como regla general, con excepciones por componente.
- **Nofrendo:** varios archivos originales indican GNU Library GPL versión 2 de Matthew Conte; se conservan los avisos correspondientes.
- **Anemoia-ESP32:** GPL v3, por separado.
- **Integración de COCODRILO OS:** adaptaciones propias de Coqui, respetando los derechos de los proyectos de origen.

Los textos de licencias y referencias figuran en [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) y [licenses/](licenses/). La licencia MIT de los archivos originales de Coqui no sustituye las obligaciones de las partes GPL y LGPL.

La compatibilidad de redistribución de ciertos componentes de Retro-Go con partes de ESP-IDF requiere evaluación por versión y por archivos efectivamente enlazados. También existen datos incorporados en `nofrendo/database.h` y paletas de `gnuboy/tables.h` cuya procedencia y autorización de redistribución deben verificarse para una distribución de firmware.

## Juegos

**Este repositorio no incluye ROMs ni BIOS comerciales.** Utiliza solamente juegos y archivos cuyo uso esté autorizado.

La compatibilidad y el rendimiento de cada emulador dependen de la compilación, la tarjeta SD, el hardware y los títulos utilizados.
