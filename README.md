# 🐊 COCODRILO OS Community

### Un sistema de entretenimiento, juegos y herramientas para ESP32-S3

**Creado por Coqui. Hecho para aprender, experimentar y compartir. 💚**

COCODRILO OS Community es una edición abierta de COCODRILO OS para una consola portátil basada en **ESP32-S3 N16R8**. Reúne una interfaz gráfica, juegos, herramientas, música y emulación en un mismo proyecto, con espacio para que la comunidad explore su código y contribuya.

## ✨ Características

- **Interfaz gráfica:** un menú para navegar entre aplicaciones desde la pantalla del dispositivo.
- **Juegos y experiencias:** Princesita, Astronauta, Cuatete, Memorama y Zona Astro.
- **Música y multimedia:** reproductor de audio, galería y herramientas musicales.
- **Emulación:** integración de NES, Game Boy y Game Boy Color mediante proyectos de código abierto.
- **Funciones del dispositivo:** soporte de almacenamiento SD, controles y utilidades para la consola.

> Community continúa en desarrollo. La disponibilidad y el funcionamiento de algunas funciones pueden variar según la compilación y el hardware utilizado.

## 🔧 Hardware

| Componente | Configuración objetivo |
| --- | --- |
| Microcontrolador | ESP32-S3 |
| Memoria | N16R8: 16 MB de flash y 8 MB de PSRAM |
| Pantalla | ST7789 de 240 × 320, en orientación horizontal (320 × 240) |
| Entorno | Arduino-ESP32 |
| Gráficos | TFT_eSPI |

¿Quieres construir tu propia consola? Consulta la **[lista de componentes](COMPONENTES.md)** y el **[mapa de conexiones GPIO](CONEXIONES.md)**.

🇲🇽 **Para México:** también puedes consultar los [enlaces de referencia a componentes en UNIT Electronics](COMPRAS_MEXICO_UNIT_ELECTRONICS.md). Para la configuración TFT revisa [`config/User_Setup.h`](config/User_Setup.h), y para las bibliotecas y compilación, [`DEPENDENCIAS_ESP32S3.md`](DEPENDENCIAS_ESP32S3.md).

## 🚀 Cómo obtener COCODRILO OS Community

### Código fuente

Puedes explorar el código y las herramientas de compilación en la rama **`main`**.

### Firmware

El proyecto cuenta con un [workflow de compilación para ESP32-S3](.github/workflows/community-firmware-build.yml). Las compilaciones generadas se publican en la rama [`firmware-builds`](../../tree/firmware-builds).

Antes de instalar un archivo BIN, confirma que corresponda a tu placa y pantalla. Si no conoces el procedimiento de flasheo de tu dispositivo, consulta primero su documentación; una configuración incorrecta puede impedir que arranque.

### Compilar por tu cuenta

El proyecto utiliza **Arduino-ESP32 3.2.1**, **TFT_eSPI 2.5.43** y las demás dependencias descritas en [la guía técnica](DEPENDENCIAS_ESP32S3.md). El [workflow](.github/workflows/community-firmware-build.yml) muestra los pasos de preparación y compilación utilizados.

## 🎮 Emuladores

COCODRILO OS Community integra adaptaciones de **Retro-Go**, **Nofrendo** y **Gnuboy** para NES, Game Boy y Game Boy Color.

Los proyectos originales pertenecen a sus respectivos autores. Esta edición no incluye ROMs ni BIOS comerciales; utiliza únicamente juegos y archivos que tengas derecho a usar.

Para más información sobre las adaptaciones y sus licencias, consulta [EMULADORES_Y_LICENCIAS.md](EMULADORES_Y_LICENCIAS.md).

## 🤝 Comunidad y contribuciones

¡Las ideas, las pruebas en hardware y las mejoras son bienvenidas! Puedes revisar el código, reportar problemas desde [Issues](../../issues) y proponer cambios mediante pull requests.

Si compartes una mejora, procura explicar qué hace, en qué hardware la probaste y qué dependencias utiliza.

## 💚 Créditos

**COCODRILO OS y la integración Community — Coqui.**

Gracias a las personas y comunidades que mantienen los proyectos que hacen posible esta edición, entre ellos:

- Espressif — Arduino-ESP32 y ESP-IDF.
- Bodmer — TFT_eSPI y TJpg_Decoder.
- Retro-Go y sus colaboradores — integración y núcleos de emulación.
- Autores de Nofrendo y Gnuboy.
- David Reid — dr_mp3.
- Autores y colaboradores de lowzag.

Puedes consultar las atribuciones y avisos completos en [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## ⚖️ Licencia

El **código y la documentación originales propiedad de Coqui** están disponibles bajo la **licencia MIT**. Consulta el archivo [LICENSE](LICENSE).

Los emuladores, bibliotecas, adaptaciones derivadas de código de terceros y otros recursos mantienen sus licencias y derechos respectivos. La licencia MIT del trabajo original no sustituye esas condiciones. Consulta [los avisos de terceros](THIRD_PARTY_NOTICES.md) y la carpeta [licenses/](licenses/).

---

**Hecho con cariño por Coqui. 🐊💚**
