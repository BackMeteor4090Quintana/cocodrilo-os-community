// ============================================================
//  ARCADE CONSOLA — Launcher unificado
//  Inicio directo al menu de aplicaciones
//  Juegos disponibles: Princesita Astro, Memorama Astro, Zona Astro
//  Incluye juegos, Personalizacion y Galeria MJPEG
//  ESP32-S3 N16R8 + ST7789 320x240 — TFT_eSPI
// ============================================================
#include <TFT_eSPI.h>

#include <SPI.h>
#include <math.h>
#include <new>
#include <pgmspace.h>
#include "FS.h"
#include "SD_MMC.h"
#include <TJpg_Decoder.h>
#include "PortadaLowzag.h" // Progressive SOF2, MIT
#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <time.h>
#include <esp_heap_caps.h>
#include "freertos/queue.h"


#include "icono_metronomo.h"
#include "icono_afinador.h"
#include "iconos_juegos_push.h" // iconos 48x48 precalculados con color transparente
#include "iconos_personalizados.h"
#include "Bateria.h"
#include "ModoTecnico.h"
#include "QR.h"
#include "GestionSD.h"
#include "ActualizacionSD.h"
#include "AvisoActualizacionWiFi.h"




#include "SistemaLogros.h"
#include "SistemaDesbloqueos.h"
#include "wallpaper_default.h"
#include "AudioMAX98357.h"
#include "CocodriloVolumeControl.h"
#include "BacklightControl.h"
#include "COCODRILO_GBC_RetroGo.h"
#include "AstronautaSprites.h"
#include "SFXSD.h"

// Decoder MP3: SOLO decodifica. La salida sigue usando el I2S legacy de
// AudioMAX98357 para no mezclar APIs I2S nuevas con COCODRILO/Anemoia.
// El codigo dr_mp3 se incluye desde src/community.
#define DR_MP3_NO_STDIO
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"
#include "MP3PCMFrameInfo.h"
#define COCODRILO_TIENE_MP3_DRMP3 1


// Volumen global 0..10 para todo el arcade.
// 0 = OFF; 1..10 = niveles digitales del MAX98357A/I2S.
#define VOLUME_PASOS_MAX 10
int volumenPaso = 5;
bool sonidoActivo = true;


// ------------------------------------------------------------
// MAX98357A - puente entre el volumen existente de COCODRILO OS
// y el "potenciómetro" digital por software 0..10.
// El firmware histórico usa volumenPaso en distintos rangos según
// el juego/panel; normalizamos conservadoramente aquí.
// ------------------------------------------------------------
static inline uint8_t cocodriloVolumenMAXDesdePaso(int paso) {
  if (paso < 0) paso = 0;
  if (paso > VOLUME_PASOS_MAX) paso = VOLUME_PASOS_MAX;
  return (uint8_t)paso;   // COCODRILO OS ahora usa directamente 0..10
}

static inline void cocodriloAplicarVolumenMAX() {
  AudioMAX98357::setVolumeStep(cocodriloVolumenMAXDesdePaso(volumenPaso));
  AudioMAX98357::setEnabled(volumenPaso > 0);
}


// Estado temporal de arranque de COCODRILO OS.
bool arranqueSilencioso = false;
bool modoTecnicoSolicitado = false;
bool sdDetectadaArranque = false;

// Wi-Fi opcional de COCODRILO OS.
bool wifiActivado = false;
bool wifiConfigurado = false;
bool portalWiFiActivo = false;
bool credencialesWiFiRecibidas = false;
String wifiSSIDGuardado;
String wifiClaveGuardada;
String estadoWiFiArranque = "Desactivado";
bool avisoActualizacionConsultado = false;
bool avisoActualizacionMostrado = false;



// Wi-Fi de arranque no bloqueante.
// La BIOS espera poco; después la asociación continúa en segundo plano.
static bool wifiIntentoIniciado = false;
static unsigned long wifiUltimoIntentoMs = 0;
static bool ntpSolicitadoSegundoPlano = false;
static bool wifiConectadoReportado = false;

// Wi-Fi: dejar terminar UNA asociación antes de lanzar otra.
// El ESP32 ya tiene auto-reconnect habilitado; este reintento manual es
// únicamente un salvavidas si después de mucho tiempo sigue desconectado.
static constexpr uint32_t WIFI_REINTENTO_FONDO_MS = 45000UL;

// Bloqueo de inicio mediante PIN de cuatro dígitos.
bool bloqueoPinActivo = false;
uint16_t pinGuardado = 0;
uint8_t pinEdicion[4] = {0, 0, 0, 0};
int posicionPin = 0;
bool editandoPinNuevo = false;
int opcionMenuPin = 0;

int paginaAcerca = 0;
constexpr int TOTAL_PAGINAS_ACERCA = 3;

bool horaSincronizada = false;
int minutoEncabezadoAnterior = -1;
wl_status_t estadoWiFiEncabezadoAnterior = WL_NO_SHIELD;

WebServer servidorWiFi(80);
DNSServer dnsWiFi;

static inline uint8_t arcadeVolumenDuty() {
  return sonidoActivo ? 128 : 0;  // onda cuadrada fija al 50 %
}

static inline String arcadeNombreVolumen() {
  if (volumenPaso <= 0) return String("OFF");
  return String(volumenPaso) + "/10";
}

// Control centralizado para todos los sonidos heredados tipo buzzer.
// DEBUG AUDIO: mide cuántas órdenes recibe el backend y cuántas son redundantes.
// El descarte real de redundancias se hace también dentro de AudioMAX98357,
// usando el estado verdadero del backend (seguro al entrar/salir de NES).
static inline void arcadeBuzzerWriteStable(uint8_t pinOrChannel, uint32_t frecuencia) {
  (void)pinOrChannel;

  if (!sonidoActivo && frecuencia != 0) frecuencia = 0;

  static uint32_t dbgInicioMs = 0;
  static uint32_t dbgUltimaFrecuencia = UINT32_MAX;
  static uint32_t dbgLlamadas = 0;
  static uint32_t dbgCambios = 0;
  static uint32_t dbgRepetidas = 0;
  static uint32_t dbgOn = 0;
  static uint32_t dbgOff = 0;

  const uint32_t ahoraMs = millis();
  if (dbgInicioMs == 0) dbgInicioMs = ahoraMs;

  dbgLlamadas++;
  if (frecuencia != dbgUltimaFrecuencia) {
    dbgCambios++;
    if (frecuencia == 0) dbgOff++;
    else dbgOn++;
    dbgUltimaFrecuencia = frecuencia;
  } else {
    dbgRepetidas++;
  }

  if ((uint32_t)(ahoraMs - dbgInicioMs) >= 1000UL) {
    Serial.printf(
      "[AUDIO][CALLS] llamadas=%u cambios=%u repetidas=%u ON=%u OFF=%u freq=%u\n",
      (unsigned)dbgLlamadas,
      (unsigned)dbgCambios,
      (unsigned)dbgRepetidas,
      (unsigned)dbgOn,
      (unsigned)dbgOff,
      (unsigned)frecuencia
    );
    dbgInicioMs = ahoraMs;
    dbgLlamadas = dbgCambios = dbgRepetidas = dbgOn = dbgOff = 0;
  }

  if (frecuencia == 0) {
    AudioMAX98357::stop();
  } else {
    AudioMAX98357::tone(frecuencia);
  }
}

#define DEBUG_SISTEMA 1
#define DEBUG_INTERVALO_MS 5000UL

// ════════════════════════════════════════════════════════════
//  FONDO DE PANTALLA DESDE LA SD (JPEG con TJpg_Decoder)
//  Los fondos deben ser JPG/JPEG de 320x240. JPEG no utiliza
//  transparencia, pero para wallpapers no hace falta y evita
//  las incompatibilidades que aparecieron con algunos PNG.
// ════════════════════════════════════════════════════════════
// SD integrada de la ESP32-S3 en modo SD_MMC de 1 bit.
// Pines del lector interno:
#define SD_MMC_CLK 39
#define SD_MMC_CMD 38
#define SD_MMC_D0  40

#define RUTA_FONDO_PREDETERMINADO "/wallpaper.jpg"
#define CARPETA_WALLPAPERS "/wallpapers"
#define MAX_WALLPAPERS 20
#define MAX_RUTA_WALLPAPER 96

Preferences preferenciasArcade;

// ════════════════════════════════════════════════════════════
//  SISTEMA DE LOGROS GLOBAL (ver SistemaLogros.h)
// ════════════════════════════════════════════════════════════
SistemaLogros sistemaLogros;
SistemaDesbloqueos sistemaDesbloqueos;
uint8_t juegoLogrosSeleccionado = JUEGO_PRINCESITA;
int logroSeleccionado = 0;
int logroPrimerVisible = 0;
uint8_t ultimoLogroPrincesita = LOGRO_PRIMER_VUELO;

static bool logroPrincesitaObtenido(uint8_t id) { return sistemaLogros.obtenido(JUEGO_PRINCESITA, id); }
static bool concederLogroPrincesita(uint8_t id) { return sistemaLogros.desbloquear(JUEGO_PRINCESITA, id); }
static int contarLogrosPrincesita() { return sistemaLogros.contar(JUEGO_PRINCESITA); }
static bool hayLogrosPrincesitaPendientes() { return sistemaLogros.hayPendientes(JUEGO_PRINCESITA); }
static uint8_t tomarSiguienteLogroPrincesitaPendiente() { return sistemaLogros.tomarPendiente(JUEGO_PRINCESITA); }
static void evaluarLogrosPrincesita(uint8_t mapa, int puntuacion, bool rescate = false) {
  if (mapa == 0) {
    if (puntuacion >= 15) concederLogroPrincesita(LOGRO_NOCHE_DESPIERTA);
    if (puntuacion >= 25) concederLogroPrincesita(LOGRO_ESPIRITU_BOSQUE);
    if (puntuacion >= 35) concederLogroPrincesita(LOGRO_REINO_CONGELADO);
    if (rescate && puntuacion >= 50) concederLogroPrincesita(LOGRO_RESCATE_REAL);
  } else {
    if (puntuacion >= 50) concederLogroPrincesita(LOGRO_VUELO_INTERMINABLE);
    if (puntuacion >= 100) concederLogroPrincesita(LOGRO_MAESTRO_VUELO);
  }
  if (mapa == 1 && puntuacion >= 25) concederLogroPrincesita(LOGRO_HIJO_NOCHE);
  if (mapa == 3 && puntuacion >= 25) concederLogroPrincesita(LOGRO_PLUMAS_NIEVE);
}
String rutaWallpaperActual = RUTA_FONDO_PREDETERMINADO;
char rutasWallpapers[MAX_WALLPAPERS][MAX_RUTA_WALLPAPER];
int cantidadWallpapers = 0;
int wallpaperSeleccionado = 0;

// ── Personalización persistente ─────────────────────────────
static const uint16_t COLORES_CURSOR[] = {
  TFT_YELLOW, TFT_GREEN, TFT_BLUE, TFT_RED,
  TFT_MAGENTA, TFT_ORANGE, TFT_WHITE, TFT_CYAN
};
static const char* NOMBRES_COLORES_CURSOR[] = {
  "Amarillo", "Verde", "Azul", "Rojo",
  "Morado", "Naranja", "Blanco", "Cian"
};
static const int CANT_COLORES_CURSOR = sizeof(COLORES_CURSOR) / sizeof(COLORES_CURSOR[0]);
int indiceColorCursor = 0;
uint16_t colorCursor = TFT_YELLOW;

void cargarPersonalizacion() {
  preferenciasArcade.begin("arcade", true);
  indiceColorCursor = preferenciasArcade.getUChar("cursor", 0);
  preferenciasArcade.end();

  if (indiceColorCursor < 0 || indiceColorCursor >= CANT_COLORES_CURSOR) {
    indiceColorCursor = 0;
  }
  colorCursor = COLORES_CURSOR[indiceColorCursor];
  BacklightControl::cargar();
  Serial.printf("Cursor: %s\n", NOMBRES_COLORES_CURSOR[indiceColorCursor]);
}

void guardarColorCursor() {
  preferenciasArcade.begin("arcade", false);
  preferenciasArcade.putUChar("cursor", (uint8_t)indiceColorCursor);
  preferenciasArcade.end();
  colorCursor = COLORES_CURSOR[indiceColorCursor];
}


// Sprite que recibe los bloques RGB565 producidos por TJpg_Decoder.
TFT_eSprite *jpgDestino = nullptr;
static int jpgBloquesDibujados = 0;
static int jpgMaxX = 0;
static int jpgMaxY = 0;

// TJpg_Decoder llama esta función por cada bloque MCU del JPEG.
bool jpgDibujarBloque(int16_t x, int16_t y,
                      uint16_t w, uint16_t h,
                      uint16_t *bitmap) {
  if (!jpgDestino || !bitmap) return false;

  // Detiene la decodificación si el bloque empieza fuera del destino.
  if (x >= 320 || y >= 240) return false;

  // Los wallpapers exigidos son 320x240, así que normalmente no hay
  // recorte. Estas comprobaciones protegen los bordes por seguridad.
  uint16_t anchoVisible = w;
  uint16_t altoVisible = h;
  if (x + anchoVisible > 320) anchoVisible = 320 - x;
  if (y + altoVisible > 240) altoVisible = 240 - y;

  // Para un bloque recortado horizontalmente el stride original sería w.
  // Con imágenes exactas de 320x240 los bloques MCU encajan sin recorte.
  if (anchoVisible == w) {
    jpgDestino->pushImage(x, y, anchoVisible, altoVisible, bitmap);
  } else {
    for (uint16_t fila = 0; fila < altoVisible; ++fila) {
      jpgDestino->pushImage(x, y + fila, anchoVisible, 1,
                            bitmap + fila * w);
    }
  }

  jpgBloquesDibujados++;
  if (x + anchoVisible > jpgMaxX) jpgMaxX = x + anchoVisible;
  if (y + altoVisible > jpgMaxY) jpgMaxY = y + altoVisible;

  if ((jpgBloquesDibujados & 15) == 0) yield();
  return true;
}

bool esArchivoJPG(const char *nombre) {
  if (!nombre) return false;
  String n = String(nombre);
  n.toLowerCase();
  return n.endsWith(".jpg") || n.endsWith(".jpeg");
}


bool dibujarWallpaperDefault(TFT_eSprite &destino) {
  if (destino.getColorDepth() != 16) {
    Serial.printf("Wallpaper interno rechazado: canvas=%d bpp\n",
                  destino.getColorDepth());
    return false;
  }

  uint16_t *pixeles = reinterpret_cast<uint16_t*>(destino.getPointer());
  if (!pixeles) {
    Serial.println("Wallpaper interno: sprite sin memoria");
    return false;
  }

  memcpy_P(
    pixeles,
    wallpaper_default,
    WALLPAPER_DEFAULT_W * WALLPAPER_DEFAULT_H * sizeof(uint16_t)
  );

  Serial.println("Wallpaper interno de COCODRILO OS cargado");
  return true;
}

// Intenta usar el wallpaper elegido de la SD. Si la tarjeta, la ruta o el
// JPEG no están disponibles, utiliza siempre el wallpaper interno oficial.
bool dibujarFondoDesdeSD(const char *ruta, TFT_eSprite &destino);

bool dibujarWallpaperActivo(const char *ruta, TFT_eSprite &destino) {
  if (ruta && SD_MMC.cardType() != CARD_NONE &&
      SD_MMC.exists(ruta) && esArchivoJPG(ruta)) {
    if (dibujarFondoDesdeSD(ruta, destino)) {
      return true;
    }
  }

  return dibujarWallpaperDefault(destino);
}


// Dibuja un JPEG 320x240 de la SD sobre el sprite indicado.
bool dibujarFondoDesdeSD(const char *ruta, TFT_eSprite &destino) {
  if (!ruta || !SD_MMC.exists(ruta)) {
    Serial.printf("Wallpaper inexistente: %s\n", ruta ? ruta : "(null)");
    return false;
  }

  if (!esArchivoJPG(ruta)) {
    Serial.printf("Wallpaper rechazado: debe ser JPG/JPEG: %s\n", ruta);
    return false;
  }

  if (destino.getColorDepth() != 16) {
    Serial.printf("Wallpaper rechazado: canvas=%d bpp; se requieren 16 bpp\n",
                  destino.getColorDepth());
    return false;
  }

  digitalWrite(TFT_CS, HIGH);

  uint16_t ancho = 0;
  uint16_t alto = 0;
  TJpgDec.getFsJpgSize(&ancho, &alto, ruta, SD_MMC);

  Serial.printf("JPG %s: %ux%u, heap=%u, bloque=%u, canvas=%d bpp\n",
                ruta, ancho, alto,
                ESP.getFreeHeap(),
                heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                destino.getColorDepth());

  if (ancho != 320 || alto != 240) {
    Serial.println("Wallpaper rechazado: debe medir exactamente 320x240");
    return false;
  }

  jpgDestino = &destino;
  jpgBloquesDibujados = 0;
  jpgMaxX = 0;
  jpgMaxY = 0;

  // TJpg_Decoder entrega bloques RGB565. En TFT_eSPI el intercambio
  // de bytes debe estar activo al copiar esos bloques al sprite.
  destino.setSwapBytes(true);
  // La galería cambia este ajuste para dibujar directamente en la TFT.
  // Lo restauramos explícitamente para evitar doble intercambio de bytes
  // y fondos con colores corruptos al volver al launcher.
  TJpgDec.setSwapBytes(false);
  TJpgDec.setJpgScale(1);
  TJpgDec.setCallback(jpgDibujarBloque);

  Serial.printf("Comienza JPG: %s\n", ruta);
  Serial.flush();

  uint16_t rc = TJpgDec.drawFsJpg(0, 0, ruta, SD_MMC);

  destino.setSwapBytes(false);
  jpgDestino = nullptr;

  Serial.printf("Termina JPG: %s, rc=%u, bloques=%d, cobertura=%dx%d\n",
                ruta, rc, jpgBloquesDibujados, jpgMaxX, jpgMaxY);
  Serial.flush();

  // JDR_OK vale 0. También validamos que los bloques cubrieran la imagen.
  if (rc != 0 || jpgMaxX < 320 || jpgMaxY < 240) {
    Serial.printf("JPG incompleto: %s, rc=%u, cobertura=%dx%d\n",
                  ruta, rc, jpgMaxX, jpgMaxY);
    return false;
  }

  return true;
}


// ── Wallpaper persistente y catálogo desde /wallpapers ──────

void cargarWallpaperGuardado() {
  preferenciasArcade.begin("arcade", true);
  rutaWallpaperActual = preferenciasArcade.getString(
    "wallpaper", RUTA_FONDO_PREDETERMINADO
  );
  preferenciasArcade.end();

  const bool sdDisponible = SD_MMC.cardType() != CARD_NONE;
  const bool wallpaperSDValido =
      sdDisponible &&
      SD_MMC.exists(rutaWallpaperActual.c_str()) &&
      esArchivoJPG(rutaWallpaperActual.c_str());

  if (!wallpaperSDValido) {
    Serial.printf("Wallpaper SD no disponible: %s. Usando fondo interno de COCODRILO OS\n",
                  rutaWallpaperActual.c_str());
  } else {
    Serial.printf("Wallpaper activo desde SD: %s\n",
                  rutaWallpaperActual.c_str());
  }
}

bool guardarWallpaperActual(const char *ruta) {
  if (!ruta || !SD_MMC.exists(ruta)) return false;

  rutaWallpaperActual = ruta;
  preferenciasArcade.begin("arcade", false);
  size_t escritos = preferenciasArcade.putString("wallpaper", rutaWallpaperActual);
  preferenciasArcade.end();

  Serial.printf("Wallpaper guardado: %s\n", rutaWallpaperActual.c_str());
  return escritos > 0;
}

void agregarWallpaperCatalogo(const char *ruta) {
  if (!ruta || cantidadWallpapers >= MAX_WALLPAPERS) return;

  for (int i = 0; i < cantidadWallpapers; ++i) {
    if (strcmp(rutasWallpapers[i], ruta) == 0) return;
  }

  strncpy(rutasWallpapers[cantidadWallpapers], ruta, MAX_RUTA_WALLPAPER - 1);
  rutasWallpapers[cantidadWallpapers][MAX_RUTA_WALLPAPER - 1] = '\0';
  ++cantidadWallpapers;
}

void escanearWallpapers() {
  cantidadWallpapers = 0;

  if (SD_MMC.exists(RUTA_FONDO_PREDETERMINADO)) {
    agregarWallpaperCatalogo(RUTA_FONDO_PREDETERMINADO);
  }

  File carpeta = SD_MMC.open(CARPETA_WALLPAPERS);
  if (carpeta && carpeta.isDirectory()) {
    File archivo = carpeta.openNextFile();
    while (archivo && cantidadWallpapers < MAX_WALLPAPERS) {
      if (!archivo.isDirectory() && esArchivoJPG(archivo.name())) {
        String nombre = archivo.name();
        String ruta;
        if (nombre.startsWith("/")) ruta = nombre;
        else ruta = String(CARPETA_WALLPAPERS) + "/" + nombre;
        agregarWallpaperCatalogo(ruta.c_str());
      }
      archivo.close();
      archivo = carpeta.openNextFile();
    }
    carpeta.close();
  }

  // Asegura que el wallpaper activo aparezca aunque esté fuera de /wallpapers.
  if (SD_MMC.exists(rutaWallpaperActual.c_str())) {
    agregarWallpaperCatalogo(rutaWallpaperActual.c_str());
  }

  wallpaperSeleccionado = 0;
  for (int i = 0; i < cantidadWallpapers; ++i) {
    if (rutaWallpaperActual == rutasWallpapers[i]) {
      wallpaperSeleccionado = i;
      break;
    }
  }

  Serial.printf("Wallpapers encontrados: %d\n", cantidadWallpapers);
}

// Controles y pantalla compartidos por el launcher y los juegos.
#define BUTTON_PIN  2   // Botón OK
#define PIN_VOLUMEN_PANEL 4   // abre/cierra el panel de volumen desde el menu

// ── Buzzer (pasivo, usa LEDC del ESP32) ──────────────────────
#define BUZZER_PIN     7
#define BUZZER_CH      0   // canal LEDC (0-15)
#define BUZZER_RES     8   // resolución de duty (bits)

// Notas usadas por el jingle de la intro
#define NOTE_CS4 277
#define NOTE_D4  294
#define NOTE_E4  330
#define NOTE_FS4 370
#define NOTE_GS4 415
#define NOTE_A4  440
#define NOTE_B4  494
#define NOTE_CS5 554
#define NOTE_D5  587
#define NOTE_E5  659

TFT_eSPI    tft    = TFT_eSPI();
TFT_eSprite canvas = TFT_eSprite(&tft);

// Visibilidad calculada una vez por frame.
bool bateriaVisibleFrame = false;

// Sprite exclusivo para los juegos que dibujan directamente sobre la TFT.
TFT_eSprite bateriaDirecta = TFT_eSprite(&tft);

// En los juegos con framebuffer, el medidor se dibuja dentro del mismo sprite
// antes de enviarlo a la pantalla.
static inline void bateriaPushSprite(TFT_eSprite &sprite, int16_t x, int16_t y) {
  if (bateriaVisibleFrame && y == 0 && sprite.width() >= 300) {
    // Algunos juegos, especialmente Princesita y Astronauta, terminan
    // el frame con setTextSize(2/3/4). Bateria::Tick usa el estado actual
    // del sprite, por eso el porcentaje heredaba ese tamaño y se hacía enorme.
    //
    // Fijamos explícitamente el estado tipográfico antes de dibujar la batería.
    sprite.setTextFont(1);
    sprite.setTextSize(1);
    sprite.setTextDatum(TL_DATUM);

    Bateria::Tick(sprite, tft, sprite.width() - 32, 4);

    // Estado neutro para el siguiente frame.
    sprite.setTextFont(1);
    sprite.setTextSize(1);
    sprite.setTextDatum(TL_DATUM);
  }

  sprite.pushSprite(x, y);
}

// ── Resolución ──
#define SCR_W  320
#define SCR_H  240

// ── Colores ──
#define COL_CLOUD      0xFFFF
#define COL_CLOUD_SHD  0xC618
#define COL_GROUND     0xD600
#define COL_GRASS      0x2D05
#define COL_GRASS_LT   0x4628
#define COL_PIPE       0x0580
#define COL_PIPE_LT    0x07E0
#define COL_PIPE_DK    0x0300
#define COL_PIPE_CAP   0x07A0
#define COL_PIPE_CAP_H 0x0FE0
#define COL_BIRD_Y     0xFFE0
#define COL_BIRD_YD    0xFCC0
#define COL_BIRD_OR    0xFD00
#define COL_BIRD_W     0xFFFF
#define COL_BIRD_BL    0x0000
#define COL_BIRD_BK    0xFB60
#define COL_GAME_OVER  0xF800

// Colores magenta/rosa para el pájaro femenino (Astro)
#define COL_BIRD_M     0xF81F
#define COL_BIRD_MD    0xC01F
#define COL_BIRD_MP    0xFBB7

// ── Parámetros usados solo para dibujar el fondo decorativo ──
#define PIPE_WIDTH     38
#define GROUND_H       30
#define GROUND_Y       (SCR_H - GROUND_H)

// ── Estado general del launcher ──
enum GameState {
  STATE_START,            // "Pulsa el boton para jugar"
  STATE_MENU,                 // Menú con la lista de juegos
  STATE_ACHIEVEMENTS,         // Logros de Princesita desde el launcher
  STATE_CONFIG_MAIN,          // Personalización: menú principal
  STATE_CONFIG_WALLPAPER,     // Selector de fondos desde /wallpapers
  STATE_CONFIG_CURSOR,        // Selector de color del cursor
  STATE_CONFIG_ICON_APP,      // Selección de aplicación para cambiar icono
  STATE_CONFIG_ICON_PICK,     // Selector de icono para la aplicación
  STATE_CONFIG_WIFI,          // Ajustes y configuración Wi-Fi
  STATE_CONFIG_WIFI_PORTAL,   // Portal local COCODRILO-SETUP
  STATE_CONFIG_SD,            // Informacion y expulsion segura de la SD
  STATE_CONFIG_UPDATES,       // Estado online + QR de descargas
  STATE_CONFIG_PIN,           // Activar/desactivar y cambiar PIN
  STATE_CONFIG_PIN_EDIT,      // Editor de PIN de 4 dígitos
  STATE_CONFIG_RESET,         // Confirmación de restablecimiento
  STATE_CONFIG_ACERCA,        // Pantallas Acerca de
  STATE_CONFIG_BRILLO,        // Brillo de pantalla por PWM en GPIO21
  STATE_GAME_PRINCESITA,
  STATE_GAME_MEMORAMA,
  STATE_GAME_ZONA,
  STATE_GAME_CUATETE,
// TODO aplicado automáticamente: llamar a drawPauseMenu() al entrar en la pausa de Astronauta.

  STATE_GAME_ASTRONAUTA,
  STATE_EMULADORES,
  STATE_NES_BROWSER,
  STATE_GBC_BROWSER,
  STATE_GALERIA,
  STATE_MP3,
  STATE_METRONOMO,
  STATE_AFINADOR,
};

GameState gameState;

// Retorno robusto desde emuladores: NES/GBC pueden dejar DMA/SPI en un estado
// que algunas versiones de TFT_eSPI no recuperan de forma fiable para pushSprite().
// Guardamos esta marca en RTC RAM, reiniciamos el ESP32 al salir del core y
// en setup() entramos directo al menu con TFT/SPI inicializados desde cero.
RTC_DATA_ATTR uint32_t cocodriloRetornoNES = 0;
static constexpr uint32_t COCODRILO_RETORNO_NES_MAGIC = 0x4E455352UL; // "NESR"


// ════════════════════════════════════════════════════════════
//  AUDITORÍA DE RENDIMIENTO
//  Reporta cada 5 s, sin imprimir en cada frame:
//   - FPS reales (llamadas update por segundo)
//   - tiempo total de update()
//   - tiempo ocupado por bateriaPushSprite(canvas, )
//   - tiempo restante: lógica + dibujo dentro del canvas
//  PERF_RENDIMIENTO = 0 elimina completamente la instrumentación.
// ════════════════════════════════════════════════════════════
#define PERF_RENDIMIENTO 1
#define PERF_INTERVALO_MS 5000UL

#if PERF_RENDIMIENTO
struct PerfilRendimiento {
  uint32_t inicioVentanaMs = 0;
  uint32_t frames = 0;
  uint64_t updateUs = 0;
  uint64_t pushUs = 0;
  uint32_t pushes = 0;
  uint32_t updateMaxUs = 0;
  uint32_t pushMaxUs = 0;
};

static PerfilRendimiento perfil;
static int perfilEstado = STATE_MENU;

static const char* nombreEstadoPerfil(int estado) {
  switch (estado) {
    case STATE_GAME_PRINCESITA: return "Princesita";
    case STATE_GAME_ASTRONAUTA: return "Astronauta";
    case STATE_GAME_CUATETE:    return "Cuatete";
    case STATE_GAME_MEMORAMA:   return "Memorama";
    case STATE_GAME_ZONA:       return "Zona Astro";
    default:                    return "Launcher";
  }
}

static bool esEstadoJuego(int estado) {
  return estado == STATE_GAME_PRINCESITA ||
         estado == STATE_GAME_ASTRONAUTA ||
         estado == STATE_GAME_CUATETE ||
         estado == STATE_GAME_MEMORAMA ||
         estado == STATE_GAME_ZONA;
}

// Reloj independiente para juegos nativos.
// La TFT puede quedarse a 80 MHz: la velocidad del gameplay ya no depende
// de cuánto tarde en terminar cada push a pantalla.
//
// 45 Hz es el punto medio inicial entre:
// - 50 Hz actuales, que a 80 MHz se sienten acelerados.
// - la caída perceptible que aparece al bajar físicamente el SPI a 65 MHz.
//
// Para afinar después solo cambia este número; NO hace falta tocar la TFT.
static constexpr uint32_t JUEGO_NATIVO_FRAME_US = 22222UL; // ~45.0 Hz
static constexpr uint32_t OS_FRAME_US           = 20000UL; // 50 Hz launcher/UI

static void reiniciarPerfil(int estado) {
  perfil = PerfilRendimiento{};
  perfil.inicioVentanaMs = millis();
  perfilEstado = estado;
}

static void reportarPerfilSiCorresponde() {
  if (!esEstadoJuego(gameState)) return;

  if (perfil.inicioVentanaMs == 0 || perfilEstado != gameState) {
    reiniciarPerfil(gameState);
    return;
  }

  const uint32_t ahora = millis();
  const uint32_t ventanaMs = ahora - perfil.inicioVentanaMs;
  if (ventanaMs < PERF_INTERVALO_MS || perfil.frames == 0) return;

  const float fps = (perfil.frames * 1000.0f) / ventanaMs;
  const float updatePromMs = (perfil.updateUs / (float)perfil.frames) / 1000.0f;
  const float pushTotalPromFrameMs = (perfil.pushUs / (float)perfil.frames) / 1000.0f;
  const float pushPromMs = perfil.pushes ? (perfil.pushUs / (float)perfil.pushes) / 1000.0f : 0.0f;
  float restoPromMs = updatePromMs - pushTotalPromFrameMs;
  if (restoPromMs < 0.0f) restoPromMs = 0.0f;
  const float periodoRealMs = ventanaMs / (float)perfil.frames;
  const float usoUpdatePct = periodoRealMs > 0.0f ? (updatePromMs * 100.0f / periodoRealMs) : 0.0f;
  const float usoPushPct = updatePromMs > 0.0f ? (pushTotalPromFrameMs * 100.0f / updatePromMs) : 0.0f;

  Serial.println();
  Serial.println("======== RENDIMIENTO JUEGO ========");
  Serial.printf("Juego               : %s\n", nombreEstadoPerfil(gameState));
  Serial.printf("Ventana             : %lu ms\n", (unsigned long)ventanaMs);
  Serial.printf("Frames/update       : %lu\n", (unsigned long)perfil.frames);
  Serial.printf("FPS reales          : %.1f\n", fps);
  Serial.printf("Periodo real/frame  : %.2f ms\n", periodoRealMs);
  Serial.printf("update() promedio   : %.2f ms\n", updatePromMs);
  Serial.printf("update() maximo     : %.2f ms\n", perfil.updateMaxUs / 1000.0f);
  Serial.printf("pushSprite/frame    : %.2f ms (%.1f %% de update)\n",
                pushTotalPromFrameMs, usoPushPct);
  Serial.printf("pushSprite/llamada  : %.2f ms\n", pushPromMs);
  Serial.printf("pushSprite maximo   : %.2f ms\n", perfil.pushMaxUs / 1000.0f);
  Serial.printf("Pushes por frame    : %.2f\n", perfil.pushes / (float)perfil.frames);
  Serial.printf("Logica+dibujo canvas: %.2f ms\n", restoPromMs);
  Serial.printf("CPU ocupada update  : %.1f %% del periodo real\n", usoUpdatePct);
  Serial.println("===================================");

  reiniciarPerfil(gameState);
}

static inline void perfilRegistrarUpdate(uint32_t duracionUs) {
  if (!esEstadoJuego(gameState)) return;
  if (perfil.inicioVentanaMs == 0 || perfilEstado != gameState) {
    reiniciarPerfil(gameState);
  }
  perfil.frames++;
  perfil.updateUs += duracionUs;
  if (duracionUs > perfil.updateMaxUs) perfil.updateMaxUs = duracionUs;
}

static inline void perfilPushSprite(int16_t x, int16_t y) {
  if (!esEstadoJuego(gameState)) {
    bateriaPushSprite(canvas, x, y);
    return;
  }

  if (perfil.inicioVentanaMs == 0 || perfilEstado != gameState) {
    reiniciarPerfil(gameState);
  }

  const uint32_t t0 = micros();
  bateriaPushSprite(canvas, x, y);
  const uint32_t dt = micros() - t0;
  perfil.pushUs += dt;
  perfil.pushes++;
  if (dt > perfil.pushMaxUs) perfil.pushMaxUs = dt;
}
#else
static inline void reportarPerfilSiCorresponde() {}
static inline void perfilRegistrarUpdate(uint32_t) {}
static inline void perfilPushSprite(int16_t x, int16_t y) { bateriaPushSprite(canvas, x, y); }
#endif

// ════════════════════════════════════════════════════════════
//  DIAGNÓSTICO DEL SISTEMA POR MONITOR SERIE
//  DEBUG_SISTEMA = 0 desactiva por completo los reportes.
// ════════════════════════════════════════════════════════════
#if DEBUG_SISTEMA
static uint32_t debugUltimoReporte = 0;
static inline void debugRegistrarFrame() {}

static void debugActualizar() {
  const uint32_t ahora = millis();
  if (ahora - debugUltimoReporte < DEBUG_INTERVALO_MS) return;

  const float temperatura = temperatureRead();

  Serial.println();
  Serial.println("========== DIAGNOSTICO ==========");
  Serial.printf("CPU Temp   : %.1f C\n", temperatura);
  Serial.printf("Estado     : %d\n", (int)gameState);
  Serial.printf("Heap libre : %u bytes\n", (unsigned)ESP.getFreeHeap());
  Serial.printf("Heap minimo: %u bytes\n", (unsigned)ESP.getMinFreeHeap());
  Serial.printf("PSRAM libre: %u bytes\n", (unsigned)ESP.getFreePsram());
  Serial.printf("PSRAM total: %u bytes\n", (unsigned)ESP.getPsramSize());
  Serial.println("=================================");

  debugUltimoReporte = ahora;
}
#else
static inline void debugRegistrarFrame() {}
static inline void debugActualizar() {}
#endif

// La bandera que usan los juegos para pedirle al launcher que
// vuelva al menú (se activa desde el "Salir" del menú de pausa
// de cada juego, en MENU).
bool exitRequested = false;
void volverAlMenu() { exitRequested = true; }

bool btnPrev = false, btnCurrent = false;

// ════════════════════════════════════════════════════════════
//  VOLUMEN — ÚNICO Y COMPARTIDO por el menú y los juegos
//  (antes cada juego tenía su propia copia de volumenPaso, así
//  que cambiar el volumen en un juego no afectaba a los demás y
//  al volver al menú se perdía. Ahora todos leen/escriben esta
//  misma variable, y el nivel se conserva al cambiar de juego).
// ════════════════════════════════════════════════════════════
// Volumen digital compartido 0..10.
// Todos los juegos comparten volumenPaso.


// Escribe el duty correspondiente al volumen actual en el chopper.
// Se llama desde el menú principal y desde cada juego (misma
// variable volumenPaso, así que siempre queda sincronizado).
void aplicarVolumenChopper() {
  if (volumenPaso < 0) volumenPaso = 0;
  if (volumenPaso > VOLUME_PASOS_MAX) volumenPaso = VOLUME_PASOS_MAX;
  sonidoActivo = (volumenPaso > 0);
  cocodriloAplicarVolumenMAX();
  if (!sonidoActivo) arcadeBuzzerWriteStable(7, 0);
}

// Puente único usado por los emuladores Retro-Go.
// La implementación vive en el .ino para tocar exactamente la misma
// instancia de AudioMAX98357 que posee el I2S y el PCM del emulador.
namespace CocodriloVolumeControl {

uint8_t get() {
  if (volumenPaso < 0) return 0;
  if (volumenPaso > VOLUME_PASOS_MAX) return VOLUME_PASOS_MAX;
  return (uint8_t)volumenPaso;
}

void set(uint8_t paso) {
  if (paso > VOLUME_PASOS_MAX) paso = VOLUME_PASOS_MAX;
  volumenPaso = (int)paso;
  aplicarVolumenChopper();
}

} // namespace CocodriloVolumeControl


// ── Gradiente cielo (para intro 1) ──
uint16_t skyColors[GROUND_Y];

// ════════════════════════════════════════════════════════════
//  CRUCETA (D-PAD) — PREPARADA PARA EL FUTURO
//  4 botones digitales independientes con INPUT_PULLUP.
//  Se lee cada frame desde loop() pero de momento no dispara
//  ninguna acción: cuando tengas el juego, rellena accionCruceta().
// ════════════════════════════════════════════════════════════
#define PIN_UP        1
#define PIN_DOWN      16
#define PIN_LEFT      7
#define PIN_RIGHT     15
#define MUESTRAS_CRUCETA   8
#define DEBOUNCE_CRUCETA_MS 40

const int CRUCETA_ADC_NINGUNO   = 4095;
const int CRUCETA_ADC_DERECHA   = 3250;  // 33k
const int CRUCETA_ADC_IZQUIERDA = 2100;  // 10k
const int CRUCETA_ADC_ABAJO     = 920;   // 3.3k
const int CRUCETA_ADC_ARRIBA    = 315;   // 1k

const int CRUCETA_UMBRAL_DER_NINGUNO = (CRUCETA_ADC_NINGUNO + CRUCETA_ADC_DERECHA) / 2;
const int CRUCETA_UMBRAL_IZQ_DER     = (CRUCETA_ADC_DERECHA + CRUCETA_ADC_IZQUIERDA) / 2;
const int CRUCETA_UMBRAL_ABAJO_IZQ   = (CRUCETA_ADC_IZQUIERDA + CRUCETA_ADC_ABAJO) / 2;
const int CRUCETA_UMBRAL_ARR_ABAJO   = (CRUCETA_ADC_ABAJO + CRUCETA_ADC_ARRIBA) / 2;

enum Direccion { DIR_NINGUNO, DIR_ARRIBA, DIR_ABAJO, DIR_IZQUIERDA, DIR_DERECHA };

Direccion crucetaActual   = DIR_NINGUNO;
Direccion crucetaAnterior = DIR_NINGUNO;
unsigned long crucetaUltimoCambio = 0;

// Prototipos manuales: evitan que el generador automático de
// prototipos de Arduino inserte sus propias declaraciones ANTES
// de "enum Direccion" (arriba), lo cual rompe la compilación
// porque en ese punto del archivo Direccion todavía no existe.
Direccion clasificarCruceta(int valor);
Direccion leerCrucetaDigital();
void accionCruceta(Direccion d);

int leerCrucetaADC() {
  // Compatibilidad con los lectores antiguos basados en ADC.
  // Los botones son activos en LOW por INPUT_PULLUP.
  // Se devuelven valores equivalentes a la antigua escalera resistiva.
  if (digitalRead(PIN_UP) == LOW)    return CRUCETA_ADC_ARRIBA;
  if (digitalRead(PIN_DOWN) == LOW)  return CRUCETA_ADC_ABAJO;
  if (digitalRead(PIN_LEFT) == LOW)  return CRUCETA_ADC_IZQUIERDA;
  if (digitalRead(PIN_RIGHT) == LOW) return CRUCETA_ADC_DERECHA;
  return CRUCETA_ADC_NINGUNO;
}

Direccion leerCrucetaDigital() {
  if (digitalRead(PIN_UP)==LOW) return DIR_ARRIBA;
  if (digitalRead(PIN_DOWN)==LOW) return DIR_ABAJO;
  if (digitalRead(PIN_LEFT)==LOW) return DIR_IZQUIERDA;
  if (digitalRead(PIN_RIGHT)==LOW) return DIR_DERECHA;
  return DIR_NINGUNO;
}

Direccion clasificarCruceta(int valor) {
  if (valor > CRUCETA_UMBRAL_DER_NINGUNO) return DIR_NINGUNO;
  if (valor > CRUCETA_UMBRAL_IZQ_DER)     return DIR_DERECHA;
  if (valor > CRUCETA_UMBRAL_ABAJO_IZQ)   return DIR_IZQUIERDA;
  if (valor > CRUCETA_UMBRAL_ARR_ABAJO)   return DIR_ABAJO;
  return DIR_ARRIBA;
}

// Aquí enganchas, en el futuro, lo que deba pasar al mover la cruceta.
// Por ahora no hace nada: solo queda listo para usarse.
void accionCruceta(Direccion d) {
  switch (d) {
    case DIR_ARRIBA:    break;
    case DIR_ABAJO:     break;
    case DIR_IZQUIERDA: break;
    case DIR_DERECHA:   break;
    default:            break;
  }
}

// Llamar una vez por frame (ya se hace desde loop()).
void actualizarCruceta() {
  int valor = leerCrucetaADC();
  Direccion leida = leerCrucetaDigital();

  if (leida != crucetaActual) {
    crucetaUltimoCambio = millis();
    crucetaActual = leida;
  }

  if ((millis() - crucetaUltimoCambio) > DEBOUNCE_CRUCETA_MS && crucetaActual != crucetaAnterior) {
    crucetaAnterior = crucetaActual;
    if (crucetaActual != DIR_NINGUNO) accionCruceta(crucetaActual);
  }
}

// ════════════════════════════════════════════════════════════
//  SONIDO
// ════════════════════════════════════════════════════════════
void buzzerOn(int freq) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  arcadeBuzzerWriteStable(BUZZER_PIN, freq);
#else
  arcadeBuzzerWriteStable(BUZZER_CH, freq);
  ledcWrite(BUZZER_CH, arcadeVolumenDuty());
#endif
}
void buzzerOff() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  arcadeBuzzerWriteStable(BUZZER_PIN, 0);
#else
#endif
}

// ════════════════════════════════════════════════════════════
//  UTILIDADES DE DIBUJO
// ════════════════════════════════════════════════════════════
void precalcSky() {
  for (int y = 0; y < GROUND_Y; y++) {
    float t = (float)y / GROUND_Y;
    uint8_t r = (uint8_t)(31 * t * t);
    uint8_t g = (uint8_t)(9  + 11 * t);
    uint8_t b = (uint8_t)(31 - 31 * t * t);
    skyColors[y] = canvas.color565(r << 3, g << 2, b << 3);
  }
}

void canvasDrawSky() {
  for (int y = 0; y < GROUND_Y; y++)
    canvas.drawFastHLine(0, y, SCR_W, skyColors[y]);
}

void canvasDrawSun() {
  canvas.fillCircle(SCR_W - 45, 35, 18, 0xFD20);
  canvas.fillCircle(SCR_W - 45, 35, 14, 0xFFE0);
  canvas.fillCircle(SCR_W - 45, 35, 10, 0xFFFF);
}


// Configuracion de Princesita.
// modoJuego selecciona uno de los personajes de Princesita.
// mapaJuego: 0 = Dia, 1 = Noche, 2 = Bosque Magico, 3 = Nieve.
uint8_t modoJuego = 0;
uint8_t mapaJuego = 0;
int opcionMenuPrincipal = 0;
int opcionConfigPrincesita = 0;
bool botonBPrincesitaPrev = false;

static const int16_t nightStarX[] = {
  10, 20, 34, 48, 60, 76, 91, 110, 126, 140,
  155, 170, 184, 195, 210, 224, 235, 250, 264, 278,
  290, 300, 16, 44, 82, 118, 150, 188, 228, 270
};

static const int8_t nightStarY[] = {
  80, 25, 61, 15, 42, 72, 30, 40, 12, 63,
  50, 20, 76, 35, 55, 16, 10, 44, 68, 28,
  52, 70, 48, 87, 58, 83, 32, 64, 88, 81
};

constexpr int NIGHT_STAR_COUNT =
    sizeof(nightStarX) / sizeof(nightStarX[0]);

static const int16_t fireflyBaseX[] = {
  25, 62, 104, 146, 188, 228, 270, 303, 82, 248
};

static const int16_t fireflyBaseY[] = {
  139, 158, 132, 169, 145, 161, 128, 154, 181, 178
};

constexpr int FIREFLY_COUNT =
    sizeof(fireflyBaseX) / sizeof(fireflyBaseX[0]);

void canvasDrawNightHills() {
  const uint16_t colinaLejana = canvas.color565(24, 18, 52);
  const uint16_t colinaCerca  = canvas.color565(15, 14, 35);
  const uint16_t arbol        = canvas.color565(8, 10, 20);

  canvas.fillTriangle(-25, GROUND_Y, 55, 108, 130, GROUND_Y, colinaLejana);
  canvas.fillTriangle(70, GROUND_Y, 165, 96, 250, GROUND_Y, colinaLejana);
  canvas.fillTriangle(190, GROUND_Y, 278, 112, 355, GROUND_Y, colinaLejana);

  canvas.fillTriangle(-45, GROUND_Y, 45, 132, 130, GROUND_Y, colinaCerca);
  canvas.fillTriangle(80, GROUND_Y, 180, 121, 270, GROUND_Y, colinaCerca);
  canvas.fillTriangle(220, GROUND_Y, 305, 135, 370, GROUND_Y, colinaCerca);

  for (int x = 8; x < SCR_W; x += 31) {
    const int alto = 15 + ((x * 7) % 18);
    canvas.fillRect(x - 1, GROUND_Y - alto, 3, alto, arbol);
    canvas.fillTriangle(
      x - 9, GROUND_Y - alto + 8,
      x + 9, GROUND_Y - alto + 8,
      x, GROUND_Y - alto - 7,
      arbol
    );
    canvas.fillTriangle(
      x - 7, GROUND_Y - alto + 1,
      x + 7, GROUND_Y - alto + 1,
      x, GROUND_Y - alto - 12,
      arbol
    );
  }
}

void canvasDrawNightSky() {
  for (int y = 0; y < GROUND_Y; ++y) {
    const float t = static_cast<float>(y) / GROUND_Y;

    const uint8_t r = 3 + static_cast<uint8_t>(18.0f * t);
    const uint8_t g = 5 + static_cast<uint8_t>(8.0f * t);
    const uint8_t b = 25 + static_cast<uint8_t>(15.0f * t);

    canvas.drawFastHLine(
      0, y, SCR_W,
      canvas.color565(r, g, b)
    );
  }

  const unsigned long ahora = millis();

  for (int i = 0; i < NIGHT_STAR_COUNT; ++i) {
    const float fase = ahora * (0.0012f + (i % 5) * 0.00018f) + i * 0.71f;
    const float pulso = 0.5f + 0.5f * sinf(fase);

    const uint8_t brillo = 120 + static_cast<uint8_t>(135.0f * pulso);
    const uint16_t color = canvas.color565(brillo, brillo, 255);

    canvas.drawPixel(nightStarX[i], nightStarY[i], color);

    if (pulso > 0.82f && (i % 4 == 0)) {
      canvas.drawPixel(nightStarX[i] - 1, nightStarY[i], color);
      canvas.drawPixel(nightStarX[i] + 1, nightStarY[i], color);
      canvas.drawPixel(nightStarX[i], nightStarY[i] - 1, color);
      canvas.drawPixel(nightStarX[i], nightStarY[i] + 1, color);
    }
  }

  // Estrella fugaz ocasional. Repite aproximadamente cada 18 segundos.
  const unsigned long ciclo = ahora % 18000UL;
  if (ciclo < 700UL) {
    const float progreso = ciclo / 700.0f;
    const int sx = 35 + static_cast<int>(progreso * 170.0f);
    const int sy = 22 + static_cast<int>(progreso * 55.0f);

    for (int i = 0; i < 15; ++i) {
      const uint16_t color =
          (i < 4) ? TFT_WHITE : canvas.color565(120, 150, 220);
      canvas.drawPixel(sx - i, sy - i / 3, color);
    }
  }

  canvasDrawNightHills();
}

void canvasDrawMoon() {
  const int mx = SCR_W - 43;
  const int my = 35;

  // Halo suave hecho con circulos discretos.
  canvas.drawCircle(mx, my, 24, canvas.color565(45, 48, 88));
  canvas.drawCircle(mx, my, 22, canvas.color565(68, 69, 105));
  canvas.drawCircle(mx, my, 20, canvas.color565(105, 105, 130));

  canvas.fillCircle(mx, my, 17, canvas.color565(255, 248, 205));
  canvas.fillCircle(mx + 6, my - 5, 14, canvas.color565(9, 8, 31));

  // Pequeño brillo en el borde inferior.
  canvas.drawArc(
    mx, my, 17, 15, 110, 215,
    canvas.color565(255, 255, 235),
    canvas.color565(9, 8, 31)
  );
}

void canvasDrawNightFireflies() {
  if (mapaJuego != 1 && mapaJuego != 2) return;

  const unsigned long ahora = millis();

  for (int i = 0; i < FIREFLY_COUNT; ++i) {
    const float fase = ahora * 0.0014f + i * 1.37f;
    const float faseY = ahora * 0.0010f + i * 0.83f;

    const int x =
        fireflyBaseX[i] +
        static_cast<int>(sinf(fase) * (5 + i % 4));

    const int y =
        fireflyBaseY[i] +
        static_cast<int>(sinf(faseY) * (3 + i % 3));

    const float pulso = 0.5f + 0.5f * sinf(fase * 1.7f);

    if (pulso < 0.22f) continue;

    const uint8_t verde = 150 + static_cast<uint8_t>(100.0f * pulso);
    const uint16_t halo = (mapaJuego == 2)
        ? canvas.color565(10, verde / 2, verde / 2)
        : canvas.color565(70, verde / 2, 10);
    const uint16_t centro = (mapaJuego == 2)
        ? canvas.color565(130, verde, 245)
        : canvas.color565(220, verde, 70);

    if (pulso > 0.72f) {
      canvas.drawPixel(x - 1, y, halo);
      canvas.drawPixel(x + 1, y, halo);
      canvas.drawPixel(x, y - 1, halo);
      canvas.drawPixel(x, y + 1, halo);
    }

    canvas.drawPixel(x, y, centro);
  }
}


// ── Mapa 2: Bosque Mágico ───────────────────────────────────
// Todo el escenario se construye con primitivas para no gastar memoria
// adicional. Las tuberías y sus colisiones permanecen sin cambios.
void canvasDrawMagicForest() {
  // Degradado turquesa oscuro entre los árboles.
  for (int y = 0; y < GROUND_Y; ++y) {
    const float t = static_cast<float>(y) / GROUND_Y;
    const uint8_t r = 3  + static_cast<uint8_t>(4.0f  * t);
    const uint8_t g = 24 + static_cast<uint8_t>(42.0f * t);
    const uint8_t b = 30 + static_cast<uint8_t>(34.0f * t);
    canvas.drawFastHLine(0, y, SCR_W, canvas.color565(r, g, b));
  }

  const uint16_t troncoLejano = canvas.color565(7, 38, 39);
  const uint16_t troncoMedio  = canvas.color565(8, 49, 41);
  const uint16_t hojasOscuras = canvas.color565(5, 43, 31);
  const uint16_t hojasClaras  = canvas.color565(8, 67, 42);

  // Rayos de luz detrás de todos los elementos jugables.
  canvas.fillTriangle(84, 0, 125, 0, 178, GROUND_Y, canvas.color565(18, 82, 80));
  canvas.fillTriangle(183, 0, 207, 0, 235, GROUND_Y, canvas.color565(16, 70, 72));

  // Árboles lejanos para dar profundidad.
  for (int x = -12; x < SCR_W + 25; x += 42) {
    const int variacion = (x * 13) & 15;
    canvas.fillRect(x + 13, 25 + variacion, 12, GROUND_Y - 25 - variacion, troncoLejano);
    canvas.fillCircle(x + 19, 30 + variacion, 25, hojasOscuras);
  }

  // Grandes troncos laterales, dejando libre el centro del vuelo.
  canvas.fillRect(0, 42, 20, GROUND_Y - 42, troncoMedio);
  canvas.fillCircle(10, 50, 36, hojasOscuras);
  canvas.fillCircle(30, 18, 45, hojasClaras);
  canvas.fillRect(SCR_W - 18, 35, 18, GROUND_Y - 35, troncoMedio);
  canvas.fillCircle(SCR_W - 8, 42, 35, hojasOscuras);
  canvas.fillCircle(SCR_W - 38, 12, 43, hojasClaras);

  // Enredaderas del techo, puramente decorativas.
  for (int x = 18; x < SCR_W; x += 47) {
    const int largo = 11 + ((x * 5) % 18);
    canvas.drawFastVLine(x, 0, largo, canvas.color565(20, 105, 55));
    canvas.fillCircle(x - 2, largo - 2, 2, canvas.color565(30, 135, 65));
    canvas.fillCircle(x + 2, largo - 6, 2, canvas.color565(30, 135, 65));
  }

  // Siluetas suaves de arbustos al fondo.
  for (int x = 0; x < SCR_W; x += 20) {
    const int alto = 7 + ((x * 3) % 12);
    canvas.fillCircle(x + 5, GROUND_Y - alto, 10, hojasOscuras);
  }
}

void canvasDrawMagicForestDecor() {
  if (mapaJuego != 2) return;

  const uint16_t tallo = canvas.color565(20, 105, 55);
  const uint16_t hongoTurquesa = canvas.color565(31, 220, 210);
  const uint16_t hongoMorado = canvas.color565(178, 82, 255);
  const uint16_t cristal = canvas.color565(52, 240, 232);
  const uint16_t brillo = canvas.color565(205, 255, 245);

  // Hongos y flores pequeños: no invaden el hueco de las tuberías.
  const int hx[] = {26, 72, 118, 164, 216, 278, 305};
  for (int i = 0; i < 7; ++i) {
    const int x = hx[i];
    const int y = GROUND_Y - 2;
    canvas.drawFastVLine(x, y - 7, 7, tallo);
    canvas.fillEllipse(x, y - 8, 5, 3, (i & 1) ? hongoMorado : hongoTurquesa);
    canvas.drawPixel(x - 2, y - 9, brillo);
  }

  // Cristales turquesa en grupos discretos.
  const int cx[] = {48, 145, 252};
  for (int i = 0; i < 3; ++i) {
    const int x = cx[i];
    canvas.fillTriangle(x, GROUND_Y - 2, x + 4, GROUND_Y - 16, x + 8, GROUND_Y - 2, cristal);
    canvas.drawLine(x + 4, GROUND_Y - 14, x + 4, GROUND_Y - 4, brillo);
  }

  // Mariposa/hada ocasional atravesando el fondo.
  const unsigned long ciclo = millis() % 12000UL;
  if (ciclo < 7000UL) {
    const int x = -10 + static_cast<int>((ciclo / 7000.0f) * 340.0f);
    const int y = 72 + static_cast<int>(sinf(ciclo * 0.008f) * 16.0f);
    canvas.fillCircle(x, y, 1, TFT_WHITE);
    canvas.fillCircle(x - 3, y - 1, 2, hongoTurquesa);
    canvas.fillCircle(x + 3, y - 1, 2, hongoTurquesa);
    canvas.drawPixel(x - 5, y, brillo);
    canvas.drawPixel(x + 5, y, brillo);
  }
}

// ── Mapa 3: Nieve ──────────────────────────────────────────
// Fondo invernal construido con primitivas. Los pipes conservan
// exactamente sus sprites, posiciones y colisiones originales.
void canvasDrawSnowWorld() {
  // Degradado azul muy claro.
  for (int y = 0; y < GROUND_Y; ++y) {
    const float t = static_cast<float>(y) / GROUND_Y;
    const uint8_t r = 118 + static_cast<uint8_t>(62.0f * t);
    const uint8_t g = 177 + static_cast<uint8_t>(48.0f * t);
    const uint8_t b = 220 + static_cast<uint8_t>(28.0f * t);
    canvas.drawFastHLine(0, y, SCR_W, canvas.color565(r, g, b));
  }

  const uint16_t montanaLejana = canvas.color565(125, 158, 190);
  const uint16_t montanaCerca  = canvas.color565(91, 126, 160);
  const uint16_t nieveSombra   = canvas.color565(205, 226, 240);
  const uint16_t nieveClara    = canvas.color565(247, 252, 255);
  const uint16_t pinoLejano    = canvas.color565(42, 91, 104);
  const uint16_t pinoCerca     = canvas.color565(28, 70, 79);

  // Montañas lejanas y sus cumbres nevadas.
  canvas.fillTriangle(-35, GROUND_Y, 45, 79, 132, GROUND_Y, montanaLejana);
  canvas.fillTriangle(62, GROUND_Y, 156, 65, 250, GROUND_Y, montanaLejana);
  canvas.fillTriangle(185, GROUND_Y, 276, 86, 355, GROUND_Y, montanaLejana);
  canvas.fillTriangle(21, 105, 45, 79, 70, 108, nieveClara);
  canvas.fillTriangle(126, 91, 156, 65, 184, 94, nieveClara);
  canvas.fillTriangle(251, 107, 276, 86, 300, 111, nieveClara);

  // Colinas más cercanas.
  canvas.fillTriangle(-55, GROUND_Y, 45, 130, 140, GROUND_Y, montanaCerca);
  canvas.fillTriangle(85, GROUND_Y, 188, 118, 280, GROUND_Y, montanaCerca);
  canvas.fillTriangle(220, GROUND_Y, 306, 135, 370, GROUND_Y, montanaCerca);
  canvas.drawLine(-5, GROUND_Y - 2, 45, 130, nieveSombra);
  canvas.drawLine(45, 130, 96, GROUND_Y - 2, nieveSombra);

  // Bosque de pinos al fondo.
  for (int x = 4; x < SCR_W + 20; x += 27) {
    const int alto = 24 + ((x * 7) % 22);
    const int baseY = GROUND_Y;
    const uint16_t verde = (x & 1) ? pinoLejano : pinoCerca;
    canvas.fillRect(x - 1, baseY - alto + 12, 3, alto - 12, canvas.color565(74, 75, 70));
    canvas.fillTriangle(x - 10, baseY - 5, x + 10, baseY - 5, x, baseY - alto, verde);
    canvas.fillTriangle(x - 8, baseY - 15, x + 8, baseY - 15, x, baseY - alto - 4, verde);
    canvas.drawLine(x - 7, baseY - 15, x + 7, baseY - 15, nieveClara);
    canvas.drawLine(x - 9, baseY - 5, x + 9, baseY - 5, nieveSombra);
  }
}

void canvasDrawSnowDecor() {
  if (mapaJuego != 3) return;

  const unsigned long ahora = millis();
  const uint16_t copoClaro = canvas.color565(255, 255, 255);
  const uint16_t copoSuave = canvas.color565(210, 235, 250);

  // Copos deterministas y animados. Se dibujan antes de los pipes.
  for (int i = 0; i < 34; ++i) {
    const int velocidad = 14 + (i % 5) * 5;
    const int fase = static_cast<int>((ahora / velocidad) + i * 37);
    const int x = (i * 67 + fase / 9) % (SCR_W + 24) - 12;
    const int y = fase % (GROUND_Y + 28) - 14;
    const int deriva = static_cast<int>(sinf(ahora * 0.0012f + i * 0.73f) * 5.0f);
    const int sx = x + deriva;
    const uint16_t color = (i % 3 == 0) ? copoClaro : copoSuave;

    canvas.drawPixel(sx, y, color);
    if (i % 7 == 0) {
      canvas.drawPixel(sx - 1, y, color);
      canvas.drawPixel(sx + 1, y, color);
      canvas.drawPixel(sx, y - 1, color);
      canvas.drawPixel(sx, y + 1, color);
    }
  }

  // Pequeños montículos de nieve junto al suelo, detrás de los pipes.
  const uint16_t sombra = canvas.color565(190, 220, 238);
  const uint16_t nieve = canvas.color565(246, 252, 255);
  for (int x = -8; x < SCR_W; x += 38) {
    canvas.fillEllipse(x + 12, GROUND_Y - 1, 16, 5, sombra);
    canvas.fillEllipse(x + 10, GROUND_Y - 3, 14, 4, nieve);
  }
}

void canvasDrawBackgroundSelected() {
  if (mapaJuego == 1) {
    canvasDrawNightSky();
    canvasDrawMoon();
  } else if (mapaJuego == 2) {
    canvasDrawMagicForest();
  } else if (mapaJuego == 3) {
    canvasDrawSnowWorld();
  } else {
    canvasDrawSky();
    canvasDrawSun();
  }
}

// Cuenta regresiva al reanudar desde pausa.
unsigned long resumeCountdownStart = 0;
int resumeCountdownShown = -1;

void canvasDrawCloud(int cx, int cy, int cw, int ch) {
  int hw = cw/2, hh = ch/2;

  const uint16_t nube = (mapaJuego == 1)
      ? canvas.color565(28, 28, 52)
      : ((mapaJuego == 3) ? canvas.color565(226, 240, 250) : COL_CLOUD);
  const uint16_t sombra = (mapaJuego == 1)
      ? canvas.color565(16, 16, 34)
      : ((mapaJuego == 3) ? canvas.color565(174, 205, 226) : COL_CLOUD_SHD);
  const uint16_t brillo = (mapaJuego == 1)
      ? canvas.color565(40, 40, 66)
      : ((mapaJuego == 3) ? TFT_WHITE : 0xEF7B);

  canvas.fillEllipse(cx+2,       cy+3,      hw,       hh,       sombra);
  canvas.fillEllipse(cx-hw/2+2, cy+hh/2+3, hw*3/4,   hh*3/4,   sombra);
  canvas.fillEllipse(cx+hw/2+2, cy+hh/2+3, hw*3/4,   hh*3/4,   sombra);
  canvas.fillEllipse(cx,         cy,         hw,       hh,       nube);
  canvas.fillEllipse(cx-hw/2,   cy+hh/2,   hw*3/4,   hh*3/4,   nube);
  canvas.fillEllipse(cx+hw/2,   cy+hh/2,   hw*3/4,   hh*3/4,   nube);
  canvas.fillEllipse(cx-hw/4,   cy-hh/4,   hw/3,     hh/3,     brillo);
}

void canvasDrawGround() {
  const uint16_t suelo = (mapaJuego == 2)
      ? canvas.color565(18, 42, 31)
      : ((mapaJuego == 3) ? canvas.color565(164, 198, 219) : COL_GROUND);
  const uint16_t pasto = (mapaJuego == 2)
      ? canvas.color565(18, 105, 54)
      : ((mapaJuego == 3) ? canvas.color565(224, 241, 250) : COL_GRASS);
  const uint16_t pastoClaro = (mapaJuego == 2)
      ? canvas.color565(48, 190, 102)
      : ((mapaJuego == 3) ? TFT_WHITE : COL_GRASS_LT);

  canvas.fillRect(0, GROUND_Y, SCR_W, GROUND_H, suelo);
  canvas.fillRect(0, GROUND_Y, SCR_W, 10, pasto);
  for (int x = 0; x < SCR_W; x += 20)
    canvas.fillTriangle(x, GROUND_Y+10, x+10, GROUND_Y+2, x+20, GROUND_Y+10, pastoClaro);
  for (int x = 0; x < SCR_W; x += 20)
    canvas.drawFastVLine(x+10, GROUND_Y+12, 8, (mapaJuego == 2) ? canvas.color565(10, 29, 24) : ((mapaJuego == 3) ? canvas.color565(132, 172, 198) : COL_GROUND - 0x0841));
  canvas.drawFastHLine(0, GROUND_Y, SCR_W, pastoClaro);
}

void canvasDrawPipe(int px, int topH, int botY, int botH) {
  int pw = PIPE_WIDTH;
  if (topH > 0) {
    canvas.fillRect(px+3,    0,       pw-6, topH, COL_PIPE);
    canvas.fillRect(px+3,    0,       3,    topH, COL_PIPE_DK);
    canvas.fillRect(px+pw-6, 0,       3,    topH, COL_PIPE_LT);
    canvas.drawFastVLine(px+pw/2, 0, topH, COL_PIPE_LT);
    canvas.fillRect(px,      topH-14, pw,   14,   COL_PIPE_CAP);
    canvas.fillRect(px,      topH-14, 3,    14,   COL_PIPE_DK);
    canvas.fillRect(px+pw-3, topH-14, 3,    14,   COL_PIPE_LT);
    canvas.fillRect(px+2,    topH-14, pw-4, 3,    COL_PIPE_CAP_H);
    canvas.drawRect(px,      topH-14, pw,   14,   COL_PIPE_DK);
  }
  if (botH > 0) {
    canvas.fillRect(px,      botY,    pw,   14,   COL_PIPE_CAP);
    canvas.fillRect(px,      botY,    3,    14,   COL_PIPE_DK);
    canvas.fillRect(px+pw-3, botY,    3,    14,   COL_PIPE_LT);
    canvas.fillRect(px+2,    botY+11, pw-4, 2,    COL_PIPE_DK);
    canvas.drawRect(px,      botY,    pw,   14,   COL_PIPE_DK);
    int bodyY = botY+14, bodyH = botH-14;
    if (bodyH > 0) {
      canvas.fillRect(px+3,    bodyY, pw-6, bodyH, COL_PIPE);
      canvas.fillRect(px+3,    bodyY, 3,    bodyH, COL_PIPE_DK);
      canvas.fillRect(px+pw-6, bodyY, 3,    bodyH, COL_PIPE_LT);
      canvas.drawFastVLine(px+pw/2, bodyY, bodyH,  COL_PIPE_LT);
    }
  }
}

// ── Pájaro original (amarillo) ────────────────────────────────
void canvasDrawBird(int bx, int by, int frame, float angle) {
  int cx = bx, cy = by + (int)(angle * 0.12f);
  canvas.fillEllipse(cx+2, cy+2, 9, 7, TFT_DARKGREY);
  canvas.fillEllipse(cx,   cy,   9, 7, COL_BIRD_Y);
  for (int i = 2; i >= 0; i--)
    canvas.drawFastHLine(cx-8+i, cy+4+i, 17-i*2, COL_BIRD_YD);
  if (frame == 0) {
    canvas.fillTriangle(cx-3,cy+3,cx+4,cy+3,cx,cy+8, COL_BIRD_OR);
    canvas.drawTriangle(cx-3,cy+3,cx+4,cy+3,cx,cy+8, COL_BIRD_YD);
  } else if (frame == 1) {
    canvas.fillTriangle(cx-3,cy,  cx+4,cy,  cx,cy+5, COL_BIRD_OR);
    canvas.drawTriangle(cx-3,cy,  cx+4,cy,  cx,cy+5, COL_BIRD_YD);
  } else {
    canvas.fillTriangle(cx-3,cy-3,cx+4,cy-3,cx,cy+2, COL_BIRD_OR);
    canvas.drawTriangle(cx-3,cy-3,cx+4,cy-3,cx,cy+2, COL_BIRD_YD);
  }
  canvas.fillCircle(cx+5, cy-2, 3, COL_BIRD_W);
  canvas.fillCircle(cx+6, cy-2, 1, COL_BIRD_BL);
  canvas.drawPixel(cx+5, cy-3,     COL_BIRD_W);
  canvas.fillTriangle(cx+8,cy,cx+14,cy-1,cx+8,cy+2, COL_BIRD_BK);
  canvas.drawLine(cx+8,cy,cx+14,cy-1, 0xFD60);
  canvas.drawEllipse(cx, cy, 9, 7, COL_BIRD_YD);
}

// ── Pájaro magenta (Astro) ───────────────────────────────────
void canvasDrawBirdMagenta(int bx, int by, int frame, float angle) {
  int cx = bx, cy = by + (int)(angle * 0.12f);
  canvas.fillEllipse(cx+2, cy+2, 9, 7, TFT_DARKGREY);
  canvas.fillEllipse(cx,   cy,   9, 7, COL_BIRD_M);
  for (int i = 2; i >= 0; i--)
    canvas.drawFastHLine(cx-8+i, cy+4+i, 17-i*2, COL_BIRD_MD);
  if (frame == 0) {
    canvas.fillTriangle(cx-3,cy+3,cx+4,cy+3,cx,cy+8, COL_BIRD_OR);
    canvas.drawTriangle(cx-3,cy+3,cx+4,cy+3,cx,cy+8, COL_BIRD_MD);
  } else if (frame == 1) {
    canvas.fillTriangle(cx-3,cy,  cx+4,cy,  cx,cy+5, COL_BIRD_OR);
    canvas.drawTriangle(cx-3,cy,  cx+4,cy,  cx,cy+5, COL_BIRD_MD);
  } else {
    canvas.fillTriangle(cx-3,cy-3,cx+4,cy-3,cx,cy+2, COL_BIRD_OR);
    canvas.drawTriangle(cx-3,cy-3,cx+4,cy-3,cx,cy+2, COL_BIRD_MD);
  }
  canvas.fillCircle(cx+5, cy-2, 3, COL_BIRD_W);
  canvas.fillCircle(cx+6, cy-2, 1, COL_BIRD_BL);
  canvas.drawPixel(cx+5, cy-3,     COL_BIRD_W);
  canvas.fillTriangle(cx+8,cy,cx+14,cy-1,cx+8,cy+2, COL_BIRD_MD);
  canvas.drawLine(cx+8,cy,cx+14,cy-1, COL_BIRD_MP);
  canvas.drawEllipse(cx, cy, 9, 7, COL_BIRD_MD);
  // Moñito encima de la cabeza
  canvas.fillCircle(cx+2, cy-8, 3, 0xF81F);
  canvas.fillCircle(cx-1, cy-9, 2, 0xFBB7);
  canvas.fillCircle(cx+2, cy-8, 1, 0xFFFF);
}

// ════════════════════════════════════════════════════════════
//  CORAZÓN / ESTRELLA
// ════════════════════════════════════════════════════════════
void canvasDrawHeart(int cx, int cy, int sz, uint16_t col) {
  int r = sz / 2;
  canvas.fillCircle(cx - r/2, cy - r/4, r/2, col);
  canvas.fillCircle(cx + r/2, cy - r/4, r/2, col);
  canvas.fillTriangle(cx - r, cy - r/4, cx + r, cy - r/4, cx, cy + r*3/4, col);
}

static const int16_t starX[] = {20,60,110,170,210,35,140,195,80,155};
static const int8_t  starY[] = {25,15,40, 20, 55,60, 10, 35,50, 65};

void canvasDrawStar(int cx, int cy, int r, uint16_t col) {
  canvas.drawLine(cx-r, cy, cx+r, cy, col);
  canvas.drawLine(cx, cy-r, cx, cy+r, col);
  canvas.drawLine(cx-r/2, cy-r/2, cx+r/2, cy+r/2, col);
  canvas.drawLine(cx+r/2, cy-r/2, cx-r/2, cy+r/2, col);
}

// ════════════════════════════════════════════════════════════
//  PANTALLA DE INICIO — "Presiona A"
// ════════════════════════════════════════════════════════════
void drawStartScreen() {
  dibujarWallpaperActivo(rutaWallpaperActual.c_str(), canvas);

  // Prompt de entrada estilo consola.
  // El ancho puede cambiar sin perder el centrado horizontal.
  const int16_t promptW = 196;
  const int16_t promptH = 48;
  const int16_t promptX = (SCR_W - promptW) / 2;
  const int16_t promptY = 176;
  const int16_t promptR = 20;

  const uint16_t panelColor = canvas.color565(8, 13, 28);
  const uint16_t glowOscuro = canvas.color565(22, 70, 145);
  const uint16_t glowClaro  = canvas.color565(70, 145, 255);
  const uint16_t bordeClaro = canvas.color565(120, 190, 255);
  const uint16_t azulCaps   = canvas.color565(35, 145, 255);
  const uint16_t azulCaps2  = canvas.color565(20, 110, 225);

  // Marco exterior más grande.
  canvas.drawRoundRect(promptX - 3, promptY - 3,
                       promptW + 6, promptH + 6,
                       promptR + 3, glowOscuro);
  canvas.drawRoundRect(promptX - 1, promptY - 1,
                       promptW + 2, promptH + 2,
                       promptR + 1, glowClaro);

  // Panel principal.
  canvas.fillRoundRect(promptX, promptY,
                       promptW, promptH,
                       promptR, panelColor);
  canvas.drawRoundRect(promptX, promptY,
                       promptW, promptH,
                       promptR, bordeClaro);

  // Cápsula azul izquierda, más redondeada.
  const int16_t capsX = promptX + 6;
  const int16_t capsY = promptY + 6;
  const int16_t capsW = 52;
  const int16_t capsH = promptH - 12;
  const int16_t capsR = 16;

  canvas.fillRoundRect(capsX, capsY, capsW, capsH, capsR, azulCaps2);
  canvas.drawRoundRect(capsX, capsY, capsW, capsH, capsR, glowClaro);

  // Botón A dentro de la cápsula.
  const int16_t aX = capsX + capsW / 2;
  const int16_t aY = capsY + capsH / 2;

  canvas.fillCircle(aX, aY, 12, azulCaps);
  canvas.drawCircle(aX, aY, 12, TFT_WHITE);
  canvas.drawCircle(aX, aY, 10, bordeClaro);

  canvas.setTextFont(1);
  canvas.setTextSize(2);
  canvas.setTextDatum(MC_DATUM);
  canvas.setTextColor(TFT_WHITE, azulCaps);
  canvas.drawString("A", aX, aY, 2);

  // Texto principal.
  canvas.setTextDatum(ML_DATUM);
  canvas.setTextColor(TFT_WHITE, panelColor);
  canvas.drawString("Presiona A", promptX + 68, promptY + promptH / 2, 2);

  // Dejamos el estado tipográfico neutro para las siguientes pantallas.
  canvas.setTextDatum(TL_DATUM);
  canvas.setTextFont(1);
  canvas.setTextSize(1);

  perfilPushSprite(0, 0);
}

// NOTA: el setup() y loop() de este launcher están al FINAL del
// archivo ArcadeConsola.ino (después de los namespaces de cada
// juego), porque necesitan llamar a Princesita::init(), etc.

// ============================================================
//  FLAPPY BIRD  ♥  Edicion Community  ♥
//  ESP32-S3 Zero + ST7789 2.0" (240x320) — TFT_eSPI
//  Sin parpadeo: TFT_eSprite como framebuffer
//  Entrada: cruceta ADC en GPIO1 para menus y boton A en GPIO2
//           para confirmar y saltar.
//  Buzzer pasivo en GPIO4
//  TFT: MOSI=11 SCLK=12 CS=10 DC=9 RST=8 (definidos en User_Setup.h)
// ============================================================

// NOTA: TFT_eSPI, SPI y math.h ya se incluyen una sola vez en
// ArcadeConsola.ino. Copia splash_data.h y jaula_img.h a la carpeta
// del sketch del launcher (son los mismos que ya usabas aquí).
#include "splash_data.h"   // Imagen de pantalla de inicio (320x240 RGB565)
#include "jaula_img.h"      // Jaula RGB565 (64x90) para pantalla de rescate
#include <esp_heap_caps.h>

void beepMenuMove();

namespace Princesita {

// ── Dirección de la cruceta (declarada arriba de todo para que el
//    prototipo automático que genera el IDE de Arduino no quede
//    ANTES de esta definición; si no, "DirCruceta" no se reconoce) ──
enum DirCruceta { DIR_NINGUNO, DIR_ARRIBA, DIR_ABAJO, DIR_IZQUIERDA, DIR_DERECHA };

// ── Botón individual (opcional, además de la cruceta) ────────
#define BUTTON_PIN  2

// ── Cruceta (D-pad) de 4 botones por escalera de resistencias en un solo ADC ──
#define CRUCETA_PIN 1        // GPIO1 = ADC1_CH0 en el ESP32-S3
#define CRUCETA_MUESTRAS 8
#define CRUCETA_DEBOUNCE_MS 40

const int ADC_NINGUNO   = 3687;  // medido en este ESP32
const int ADC_DERECHA   = 2927;  // reescalado proporcional (33k)  // 33k
const int ADC_IZQUIERDA = 1891;  // reescalado proporcional (10k)  // 10k
const int ADC_ABAJO     = 828;   // reescalado proporcional (3.3k)   // 3.3k
const int ADC_ARRIBA    = 284;   // reescalado proporcional (1k)   // 1k

const int UMBRAL_DER_NINGUNO = (ADC_NINGUNO + ADC_DERECHA) / 2;
const int UMBRAL_IZQ_DER     = (ADC_DERECHA + ADC_IZQUIERDA) / 2;
const int UMBRAL_ABAJO_IZQ   = (ADC_IZQUIERDA + ADC_ABAJO) / 2;
const int UMBRAL_ARR_ABAJO   = (ADC_ABAJO + ADC_ARRIBA) / 2;

// ── Buzzer (pasivo, usa LEDC del ESP32) ──────────────────────
#define BUZZER_PIN     7
#define BUZZER_CH      0   // canal LEDC (0-15)
#define BUZZER_RES     8   // resolución de duty (bits)

// ── Volumen: transistor T2 "chopper" que regula el voltaje que
//    llega al buzzer, vía PWM de alta frecuencia + filtro RC ────
// PIN_VOLUME_CHOPPER, VOLUME_PASOS_MAX y volumenPaso ahora son
// globales y compartidos (ver bloque "SONIDO" cerca del inicio
// del archivo) para que el volumen sea el mismo en todo el arcade.

// ── Botón de pausa ────────────────────────────────────────────
#define PIN_PAUSA  5

bool pausaPrev = false;

enum PauseState { PAUSE_MENU, PAUSE_VOLUMEN };
PauseState pauseState = PAUSE_MENU;

enum PauseOption { PAUSA_SALIR, PAUSA_VOLUMEN_OPT };
PauseOption pausaSeleccion = PAUSA_SALIR;

bool botonOkPausaPrev = false;

// Frecuencias de notas en Hz
#define NOTE_C4   262
#define NOTE_E4   330
#define NOTE_G4   392
#define NOTE_A4   440
#define NOTE_C5   523
#define NOTE_CS5  554
#define NOTE_E5   659
#define NOTE_G5   784
#define NOTE_B4   494
#define NOTE_F4   349

// ===== Melodía pantalla de inicio =====
bool startMusicPlaying = false;
int musicIndex = 0;
unsigned long nextNoteTime = 0;

#define NOTE_REST 0
#define NOTE_DB3  138
#define NOTE_EB3  155
#define NOTE_F3   174
#define NOTE_G3   196
#define NOTE_AB3  207
#define NOTE_BB3  233
#define NOTE_DB4  277
#define NOTE_EB4  311
#define NOTE_F4   349
#define NOTE_G4   392
#define NOTE_AB4  415
#define NOTE_BB4  466
#define NOTE_EB5  622

const int BPM = 135;
const int NEGRA = 60000 / BPM;
const int CORCHEA = NEGRA / 2;
const int SEMICORCHEA = NEGRA / 4;
const int BLANCA = NEGRA * 2;
const int BLANCA_PUNTO = NEGRA * 3;
const int TRESILLO_CORCHEA = NEGRA / 3;

// Melodía reconstruida de tu dictado
int melodia[] = {
  NOTE_G4,
  NOTE_BB4,
  NOTE_G4,
  NOTE_F4,
  NOTE_F4, NOTE_G4, NOTE_AB4, NOTE_G4,
  NOTE_AB4,
  NOTE_BB4,
  NOTE_G4,
  NOTE_F4,
  NOTE_EB4, NOTE_DB4,
  NOTE_EB4,
  NOTE_BB3,
  
  NOTE_BB4,
  NOTE_EB5,
  NOTE_BB4, NOTE_EB4,
  NOTE_EB4, NOTE_BB4,
  NOTE_REST,
  NOTE_AB4,
  NOTE_G4,
  NOTE_F4, NOTE_EB4,

  NOTE_REST,

  NOTE_BB3, NOTE_AB3, NOTE_G3,
  NOTE_G3, NOTE_F3, NOTE_EB3,
  NOTE_F3,

  NOTE_F4, NOTE_F4,
  NOTE_F4, NOTE_DB4,
  NOTE_DB4,
  NOTE_REST, NOTE_DB4,
  NOTE_DB4, NOTE_F4,
  NOTE_AB4,
  NOTE_G4, NOTE_EB4,
  NOTE_EB4,

  NOTE_REST,
  NOTE_AB3, NOTE_G3, NOTE_AB3,
  NOTE_F3, NOTE_EB3, NOTE_DB3,

  NOTE_REST,
  NOTE_F4, NOTE_F4,
  NOTE_F4, NOTE_DB4,
  NOTE_DB4,
  NOTE_REST, NOTE_DB4,
  NOTE_DB4, NOTE_F4,
  NOTE_AB4, NOTE_G4,
  NOTE_BB4
};

int duraciones[] = {
  CORCHEA, CORCHEA + BLANCA + BLANCA,
  NEGRA,
  BLANCA_PUNTO,
  CORCHEA, CORCHEA, CORCHEA, CORCHEA,
  NEGRA,
  BLANCA_PUNTO + BLANCA,
  NEGRA,
  BLANCA_PUNTO + NEGRA,
  NEGRA, NEGRA,
  BLANCA + CORCHEA,
  CORCHEA + BLANCA_PUNTO + BLANCA_PUNTO + BLANCA,
  
  NEGRA,
  BLANCA_PUNTO + BLANCA,
  NEGRA, BLANCA_PUNTO + NEGRA,
  NEGRA, NEGRA + CORCHEA,
  SEMICORCHEA, CORCHEA + SEMICORCHEA,
  BLANCA + BLANCA,
  NEGRA, BLANCA_PUNTO + BLANCA_PUNTO, 
  
  BLANCA_PUNTO,

  TRESILLO_CORCHEA, TRESILLO_CORCHEA, TRESILLO_CORCHEA,
  TRESILLO_CORCHEA, TRESILLO_CORCHEA, TRESILLO_CORCHEA,
  NEGRA + BLANCA,

  NEGRA, TRESILLO_CORCHEA,
  NEGRA, BLANCA + BLANCA,
  BLANCA,
  NEGRA, TRESILLO_CORCHEA,
  NEGRA, TRESILLO_CORCHEA,
  NEGRA,
  NEGRA, TRESILLO_CORCHEA,
  BLANCA_PUNTO,

  BLANCA,
  TRESILLO_CORCHEA, TRESILLO_CORCHEA, TRESILLO_CORCHEA,
  TRESILLO_CORCHEA, TRESILLO_CORCHEA, TRESILLO_CORCHEA,

  BLANCA,
  NEGRA, TRESILLO_CORCHEA,
  NEGRA, BLANCA + BLANCA,
  BLANCA,
  NEGRA, TRESILLO_CORCHEA,
  NEGRA, TRESILLO_CORCHEA,
  NEGRA, NEGRA,
  BLANCA_PUNTO + BLANCA_PUNTO 
};


const int COMPAS_4_4 = NEGRA * 4;
const int PAUSA_ENTRE_REPETICIONES = COMPAS_4_4 * 4; // 4 compases

void playStartMusic() {

  if (!startMusicPlaying) {
    startMusicPlaying = true;
    musicIndex = 0;
    nextNoteTime = millis();
  }

  if (millis() >= nextNoteTime) {

    if (musicIndex >= sizeof(melodia) / sizeof(melodia[0])) {
      buzzerOff();

      // Espera 4 compases antes de repetir
      musicIndex = 0;
      nextNoteTime = millis() + PAUSA_ENTRE_REPETICIONES;
      return;
    }

    if (melodia[musicIndex] == NOTE_REST) {
      buzzerOff();
    } else {
      buzzerOn(melodia[musicIndex]);
    }

    nextNoteTime = millis() + duraciones[musicIndex];
    musicIndex++;
  }
}


// ── Lectura independiente de entradas ──
// La cruceta se usa exclusivamente para navegar.
// El boton A (GPIO2) se usa para confirmar y saltar.
int leerADCCruceta() {
  long suma = 0;
  for (int i = 0; i < CRUCETA_MUESTRAS; i++) {
    suma += leerCrucetaADC();
    delayMicroseconds(200);
  }
  return suma / CRUCETA_MUESTRAS;
}

bool crucetaPresionada() {
  int valor = leerADCCruceta();
  return (valor <= UMBRAL_DER_NINGUNO);  // cualquier dirección != NINGUNO
}

bool leerBoton() {
  // Solo el boton A. La cruceta ya no confirma ni hace saltar.
  return digitalRead(BUTTON_PIN) == LOW;
}

// ── Dirección de la cruceta (para navegar menús) ──────────────
// Distinto de leerBoton(): acá nos interesa CUÁL dirección, no solo
// si "algo" fue presionado, y se dispara una sola vez por cambio
// (como un stick de menú, no como el botón de salto).
DirCruceta clasificarDireccion(int valor) {
  if (valor > UMBRAL_DER_NINGUNO) return DIR_NINGUNO;
  if (valor > UMBRAL_IZQ_DER)     return DIR_DERECHA;
  if (valor > UMBRAL_ABAJO_IZQ)   return DIR_IZQUIERDA;
  if (valor > UMBRAL_ARR_ABAJO)   return DIR_ABAJO;
  return DIR_ARRIBA;
}

DirCruceta leerDireccionCruceta() {
  static DirCruceta dirActual   = DIR_NINGUNO;
  static DirCruceta dirAnterior = DIR_NINGUNO;
  static unsigned long ultimoCambio = 0;

  int valor = leerADCCruceta();
  DirCruceta leido = clasificarDireccion(valor);

  if (leido != dirActual) {
    ultimoCambio = millis();
    dirActual = leido;
  }

  if ((millis() - ultimoCambio) > CRUCETA_DEBOUNCE_MS && dirActual != dirAnterior) {
    dirAnterior = dirActual;
    return dirActual;   // dispara una vez por cambio estable (incluye DIR_NINGUNO al soltar)
  }
  return DIR_NINGUNO;
}

// aplicarVolumenChopper() ahora es global (ver inicio del archivo);
// se sigue llamando igual desde aquí, sin cambios en el resto del juego.

void buzzerOn(int freq) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  arcadeBuzzerWriteStable(BUZZER_PIN, freq);
#else
  arcadeBuzzerWriteStable(BUZZER_CH, freq);
  ledcWrite(BUZZER_CH, arcadeVolumenDuty());
#endif
}
void buzzerOff() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  arcadeBuzzerWriteStable(BUZZER_PIN, 0);
#else
#endif
}

unsigned long jumpSoundUntil = 0;

void soundJump() {
  // Sonido no bloqueante: evita congelar la física durante 70 ms.
  buzzerOn(NOTE_E5);
  jumpSoundUntil = millis() + 55;
}

void updateJumpSound() {
  if (jumpSoundUntil != 0 && (long)(millis() - jumpSoundUntil) >= 0) {
    buzzerOff();
    jumpSoundUntil = 0;
  }
}

void soundPoint() {
  buzzerOn(NOTE_G5); delay(60);
  buzzerOn(NOTE_C5); delay(40);
  buzzerOff();
}

void soundDie() {
  for (int f = 600; f > 150; f -= 30) {
    buzzerOn(f);
    delay(12);
  }
  buzzerOff();
}

// Jingle de fanfarria corto para el rescate (NO bloqueante)
const int rescueMelody[] = {
  NOTE_A4, NOTE_A4, NOTE_A4, NOTE_CS5,
  NOTE_A4, NOTE_CS5, NOTE_CS5, NOTE_CS5,
  NOTE_E5, NOTE_CS5
};

const int rescueDur[] = {
  150, 150, 150, 450,
  450, 150, 150, 150,
  450, 900
};

const int RESCUE_MELODY_LEN = sizeof(rescueMelody) / sizeof(rescueMelody[0]);

bool  rescueMusicPlaying   = false;
int   rescueMusicIndex     = 0;
unsigned long rescueNextNoteTime = 0;
bool  rescueNoteIsGap      = false; // true = estamos en el pequeño silencio entre notas

// Inicia el jingle (equivalente a lo que antes hacía la llamada bloqueante)
void soundRescue() {
  rescueMusicPlaying = true;
  rescueMusicIndex   = 0;
  rescueNoteIsGap     = false;
  rescueNextNoteTime  = millis();
}

// Se debe llamar en cada frame (como playStartMusic) para avanzar el jingle
void updateRescueMusic() {
  if (!rescueMusicPlaying) return;
  if (millis() < rescueNextNoteTime) return;

  if (rescueMusicIndex >= RESCUE_MELODY_LEN) {
    buzzerOff();
    rescueMusicPlaying = false;
    return;
  }

  if (!rescueNoteIsGap) {
    // Suena la nota actual
    buzzerOn(rescueMelody[rescueMusicIndex]);
    rescueNextNoteTime = millis() + rescueDur[rescueMusicIndex];
    rescueNoteIsGap = true;
  } else {
    // Pequeño silencio entre notas (20ms, como el delay(20) original)
    buzzerOff();
    rescueNextNoteTime = millis() + 20;
    rescueNoteIsGap = false;
    rescueMusicIndex++;
  }
}
  
// ─────────────────────────────────────────────────────────────

// tft y canvas son los objetos COMPARTIDOS del launcher (ArcadeConsola.ino)

// ── Resolución ──
#define SCR_W  320
#define SCR_H  240

// ── Colores ──
#define COL_CLOUD      0xFFFF
#define COL_CLOUD_SHD  0xC618
#define COL_GROUND     0xD600
#define COL_GRASS      0x2D05
#define COL_GRASS_LT   0x4628
#define COL_PIPE       0x0580
#define COL_PIPE_LT    0x07E0
#define COL_PIPE_DK    0x0300
#define COL_PIPE_CAP   0x07A0
#define COL_PIPE_CAP_H 0x0FE0
#define COL_BIRD_Y     0xFFE0
#define COL_BIRD_YD    0xFCC0
#define COL_BIRD_OR    0xFD00
#define COL_BIRD_W     0xFFFF
#define COL_BIRD_BL    0x0000
#define COL_BIRD_BK    0xFB60
#define COL_SCORE      0xFFFF
#define COL_SCORE_SHD  0x0000
#define COL_GAME_OVER  0xF800
#define COL_PANEL      0xD6C4

// Colores magenta/rosa para el pájaro femenino (Astro)
#define COL_BIRD_M     0xF81F
#define COL_BIRD_MD    0xC01F
#define COL_BIRD_MP    0xFBB7

// ── Parámetros del juego ──
#define GRAVITY        0.38f
#define JUMP_FORCE    -5.9f
#define PIPE_SPEED     2.8f
#define PIPE_GAP       60  // ← esto es lo que controla qué tan ancho es el hueco // más chico = hueco más angosto = más difícil
#define PIPE_WIDTH     38
#define PIPE_INTERVAL  135
#define GROUND_H       30
#define BIRD_SIZE      16
#define MAX_PIPES      3
#define GROUND_Y       (SCR_H - GROUND_H)

// Puntuación para activar la pantalla de rescate
#define RESCUE_SCORE   50

// ── Estructuras ──
struct Cloud { float x, y; uint8_t w, h, speed; };
#define MAX_CLOUDS 4
Cloud clouds[MAX_CLOUDS];

struct Pipe { float x; int gapY; bool scored; };
Pipe pipes[MAX_PIPES];

struct Bird { float x, y, vy, angle; int frame, animTick; };

// ── Partículas (estela de aire + ráfaga de salto) ─────────────
#define MAX_PARTICLES 25
struct Particle { float x, y, vx, vy; uint16_t color; int8_t life, maxLife; uint8_t type; bool active; };
Particle particles[MAX_PARTICLES];
uint8_t trailTick = 0;

#define C_SMOKE     COL_CLOUD_SHD   // gris claro, estela de aire
#define C_EXPLODE1  COL_BIRD_Y      // amarillo
#define C_EXPLODE2  COL_BIRD_OR     // naranja
#define C_EXPLODE3  COL_BIRD_W      // blanco
Bird bird;

// ── Gradiente cielo ──
uint16_t skyColors[GROUND_Y];

// ── Estado ──
enum GameState {
  STATE_START,
  STATE_MAIN_MENU,
  STATE_CONFIG_MENU,
  STATE_PLAYING,
  STATE_RESUME_COUNTDOWN,
  STATE_DEAD,
  STATE_OVER,
  STATE_RESCUED,
  STATE_ACHIEVEMENT_UNLOCK,
  STATE_PAUSED
};
GameState gameState;

int   score = 0, bestScore = 0;
float groundScroll = 0;
bool  btnPrev = false, btnCurrent = false;


unsigned long rescueStart  = 0;   // ← timing pantalla rescate
bool rescueJinglePlayed    = false; // ← para no repetir el jingle de rescate

// ════════════════════════════════════════════════════════════
//  UTILIDADES DE DIBUJO
// ════════════════════════════════════════════════════════════

void precalcSky() {
  for (int y = 0; y < GROUND_Y; y++) {
    float t = (float)y / GROUND_Y;
    uint8_t r = (uint8_t)(31 * t * t);
    uint8_t g = (uint8_t)(9  + 11 * t);
    uint8_t b = (uint8_t)(31 - 31 * t * t);
    skyColors[y] = canvas.color565(r << 3, g << 2, b << 3);
  }
}

uint16_t lerpColor(uint16_t a, uint16_t b, float t) {
  if (t <= 0.0f) return a;
  if (t >= 1.0f) return b;
  uint8_t r1 = (a >> 11) & 0x1F, g1 = (a >> 5) & 0x3F, b1 = a & 0x1F;
  uint8_t r2 = (b >> 11) & 0x1F, g2 = (b >> 5) & 0x3F, b2 = b & 0x1F;
  uint8_t r  = r1 + (uint8_t)((r2 - r1) * t);
  uint8_t g  = g1 + (uint8_t)((g2 - g1) * t);
  uint8_t bv = b1 + (uint8_t)((b2 - b1) * t);
  return (r << 11) | (g << 5) | bv;
}

void canvasDrawSky() {
  for (int y = 0; y < GROUND_Y; y++)
    canvas.drawFastHLine(0, y, SCR_W, skyColors[y]);
}

void canvasDrawSun() {
  canvas.fillCircle(SCR_W - 45, 35, 18, 0xFD20);
  canvas.fillCircle(SCR_W - 45, 35, 14, 0xFFE0);
  canvas.fillCircle(SCR_W - 45, 35, 10, 0xFFFF);
}

void canvasDrawCloud(int cx, int cy, int cw, int ch) {
  int hw = cw/2, hh = ch/2;
  canvas.fillEllipse(cx+2,      cy+3,       hw,     hh,     COL_CLOUD_SHD);
  canvas.fillEllipse(cx-hw/2+2, cy+hh/2+3,  hw*3/4, hh*3/4, COL_CLOUD_SHD);
  canvas.fillEllipse(cx+hw/2+2, cy+hh/2+3,  hw*3/4, hh*3/4, COL_CLOUD_SHD);
  canvas.fillEllipse(cx,        cy,          hw,     hh,     COL_CLOUD);
  canvas.fillEllipse(cx-hw/2,   cy+hh/2,     hw*3/4, hh*3/4, COL_CLOUD);
  canvas.fillEllipse(cx+hw/2,   cy+hh/2,     hw*3/4, hh*3/4, COL_CLOUD);
  canvas.fillEllipse(cx-hw/4,   cy-hh/4,     hw/3,   hh/3,   0xEF7B);
}

void canvasDrawGround() {
  int gs = (int)groundScroll % 20;

  const uint16_t suelo = (mapaJuego == 1)
      ? canvas.color565(45, 22, 55)
      : COL_GROUND;
  const uint16_t pasto = (mapaJuego == 1)
      ? canvas.color565(38, 85, 55)
      : COL_GRASS;
  const uint16_t pastoClaro = (mapaJuego == 1)
      ? canvas.color565(70, 125, 82)
      : COL_GRASS_LT;

  canvas.fillRect(0, GROUND_Y, SCR_W, GROUND_H, suelo);
  canvas.fillRect(0, GROUND_Y, SCR_W, 10, pasto);

  for (int x = -gs; x < SCR_W; x += 20) {
    canvas.fillTriangle(
      x, GROUND_Y + 10,
      x + 10, GROUND_Y + 2,
      x + 20, GROUND_Y + 10,
      pastoClaro
    );
  }

  for (int x = -gs; x < SCR_W; x += 20) {
    canvas.drawFastVLine(x + 10, GROUND_Y + 12, 8, suelo);
  }

  canvas.drawFastHLine(0, GROUND_Y, SCR_W, pastoClaro);
}

void canvasDrawPipe(int px, int topH, int botY, int botH) {
  int pw = PIPE_WIDTH;
  if (topH > 0) {
    canvas.fillRect(px+3,    0,       pw-6, topH, COL_PIPE);
    canvas.fillRect(px+3,    0,       3,    topH, COL_PIPE_DK);
    canvas.fillRect(px+pw-6, 0,       3,    topH, COL_PIPE_LT);
    canvas.drawFastVLine(px+pw/2, 0, topH, COL_PIPE_LT);
    canvas.fillRect(px,      topH-14, pw,   14,   COL_PIPE_CAP);
    canvas.fillRect(px,      topH-14, 3,    14,   COL_PIPE_DK);
    canvas.fillRect(px+pw-3, topH-14, 3,    14,   COL_PIPE_LT);
    canvas.fillRect(px+2,    topH-14, pw-4, 3,    COL_PIPE_CAP_H);
    canvas.drawRect(px,      topH-14, pw,   14,   COL_PIPE_DK);
  }
  if (botH > 0) {
    canvas.fillRect(px,      botY,    pw,   14,   COL_PIPE_CAP);
    canvas.fillRect(px,      botY,    3,    14,   COL_PIPE_DK);
    canvas.fillRect(px+pw-3, botY,    3,    14,   COL_PIPE_LT);
    canvas.fillRect(px+2,    botY+11, pw-4, 2,    COL_PIPE_DK);
    canvas.drawRect(px,      botY,    pw,   14,   COL_PIPE_DK);
    int bodyY = botY+14, bodyH = botH-14;
    if (bodyH > 0) {
      canvas.fillRect(px+3,    bodyY, pw-6, bodyH, COL_PIPE);
      canvas.fillRect(px+3,    bodyY, 3,    bodyH, COL_PIPE_DK);
      canvas.fillRect(px+pw-6, bodyY, 3,    bodyH, COL_PIPE_LT);
      canvas.drawFastVLine(px+pw/2, bodyY, bodyH,  COL_PIPE_LT);
    }
  }
}

// ── Pájaro original (amarillo) ────────────────────────────────
void canvasDrawBird(int bx, int by, int frame, float angle) {
  int cx = bx, cy = by + (int)(angle * 0.12f);
  canvas.fillEllipse(cx+2, cy+2, 9, 7, TFT_DARKGREY);
  canvas.fillEllipse(cx,   cy,   9, 7, COL_BIRD_Y);
  for (int i = 2; i >= 0; i--)
    canvas.drawFastHLine(cx-8+i, cy+4+i, 17-i*2, COL_BIRD_YD);
  if (frame == 0) {
    canvas.fillTriangle(cx-3,cy+3,cx+4,cy+3,cx,cy+8, COL_BIRD_OR);
    canvas.drawTriangle(cx-3,cy+3,cx+4,cy+3,cx,cy+8, COL_BIRD_YD);
  } else if (frame == 1) {
    canvas.fillTriangle(cx-3,cy,  cx+4,cy,  cx,cy+5, COL_BIRD_OR);
    canvas.drawTriangle(cx-3,cy,  cx+4,cy,  cx,cy+5, COL_BIRD_YD);
  } else {
    canvas.fillTriangle(cx-3,cy-3,cx+4,cy-3,cx,cy+2, COL_BIRD_OR);
    canvas.drawTriangle(cx-3,cy-3,cx+4,cy-3,cx,cy+2, COL_BIRD_YD);
  }
  canvas.fillCircle(cx+5, cy-2, 3, COL_BIRD_W);
  canvas.fillCircle(cx+6, cy-2, 1, COL_BIRD_BL);
  canvas.drawPixel(cx+5, cy-3,     COL_BIRD_W);
  canvas.fillTriangle(cx+8,cy,cx+14,cy-1,cx+8,cy+2, COL_BIRD_BK);
  canvas.drawLine(cx+8,cy,cx+14,cy-1, 0xFD60);
  canvas.drawEllipse(cx, cy, 9, 7, COL_BIRD_YD);
}

// ── Pájaro magenta (Astro) — mirando a la IZQUIERDA ──────────
void canvasDrawBirdMagenta(int bx, int by, int frame, float angle) {
  int cx = bx, cy = by + (int)(angle * 0.12f);
  canvas.fillEllipse(cx-2, cy+2, 9, 7, TFT_DARKGREY);
  canvas.fillEllipse(cx,   cy,   9, 7, COL_BIRD_M);
  for (int i = 2; i >= 0; i--)
    canvas.drawFastHLine(cx-8+i, cy+4+i, 17-i*2, COL_BIRD_MD);
  if (frame == 0) {
    canvas.fillTriangle(cx+3,cy+3,cx-4,cy+3,cx,cy+8, COL_BIRD_OR);
    canvas.drawTriangle(cx+3,cy+3,cx-4,cy+3,cx,cy+8, COL_BIRD_MD);
  } else if (frame == 1) {
    canvas.fillTriangle(cx+3,cy,  cx-4,cy,  cx,cy+5, COL_BIRD_OR);
    canvas.drawTriangle(cx+3,cy,  cx-4,cy,  cx,cy+5, COL_BIRD_MD);
  } else {
    canvas.fillTriangle(cx+3,cy-3,cx-4,cy-3,cx,cy+2, COL_BIRD_OR);
    canvas.drawTriangle(cx+3,cy-3,cx-4,cy-3,cx,cy+2, COL_BIRD_MD);
  }
  canvas.fillCircle(cx-5, cy-2, 3, COL_BIRD_W);
  canvas.fillCircle(cx-6, cy-2, 1, COL_BIRD_BL);
  canvas.drawPixel(cx-5, cy-3,     COL_BIRD_W);
  canvas.fillTriangle(cx-8,cy,cx-14,cy-1,cx-8,cy+2, COL_BIRD_MD);
  canvas.drawLine(cx-8,cy,cx-14,cy-1, COL_BIRD_MP);
  canvas.drawEllipse(cx, cy, 9, 7, COL_BIRD_MD);
  // Moñito encima de la cabeza
  canvas.fillCircle(cx-2, cy-8, 3, 0xF81F);
  canvas.fillCircle(cx+1, cy-9, 2, 0xFBB7);
  canvas.fillCircle(cx-2, cy-8, 1, 0xFFFF);
}

// ── Personajes desbloqueables de Princesita ─────────────────
void canvasDrawBirdTematico(int bx, int by, int frame, float angle,
                            uint16_t cuerpo, uint16_t sombra,
                            uint16_t ala, uint16_t detalle,
                            uint8_t accesorio) {
  int cx = bx, cy = by + (int)(angle * 0.12f);
  canvas.fillEllipse(cx+2, cy+2, 9, 7, sombra);
  canvas.fillEllipse(cx, cy, 9, 7, cuerpo);
  int alaY = (frame == 0) ? cy+8 : ((frame == 1) ? cy+5 : cy+2);
  canvas.fillTriangle(cx-3,cy+2,cx+4,cy+2,cx,alaY, ala);
  canvas.drawTriangle(cx-3,cy+2,cx+4,cy+2,cx,alaY, detalle);
  canvas.fillCircle(cx+5, cy-2, 3, COL_BIRD_W);
  canvas.fillCircle(cx+6, cy-2, 1, COL_BIRD_BL);
  canvas.fillTriangle(cx+8,cy,cx+14,cy-1,cx+8,cy+2, COL_BIRD_OR);
  canvas.drawEllipse(cx, cy, 9, 7, detalle);

  if (accesorio == 1) { // Boscoso: hoja en la cabeza
    canvas.fillEllipse(cx-1, cy-9, 4, 2, canvas.color565(70,190,75));
    canvas.drawLine(cx-4, cy-9, cx+2, cy-7, canvas.color565(35,105,45));
  } else if (accesorio == 2) { // Eclipse: ojos violetas
    canvas.fillCircle(cx+6, cy-2, 2, canvas.color565(180,60,255));
    canvas.drawPixel(cx+6, cy-2, TFT_WHITE);
  } else if (accesorio == 3) { // Copito: gorrito de nieve
    canvas.fillCircle(cx, cy-9, 3, TFT_WHITE);
    canvas.drawFastHLine(cx-4, cy-7, 8, canvas.color565(170,225,255));
  } else if (accesorio == 4) { // Princesita: corona
    canvas.fillTriangle(cx-4,cy-8,cx-2,cy-13,cx,cy-8, TFT_YELLOW);
    canvas.fillTriangle(cx,cy-8,cx+2,cy-13,cx+4,cy-8, TFT_YELLOW);
    canvas.drawFastHLine(cx-4,cy-8,9,0xFD20);
  }
}

void canvasDrawBirdEstelar(int bx, int by, int frame, float angle) {
  int cx = bx, cy = by + (int)(angle * 0.12f);
  const uint16_t azulNoche = canvas.color565(20, 35, 85);
  const uint16_t azulPanel = canvas.color565(25, 85, 160);
  const uint16_t cian = canvas.color565(50, 225, 255);
  canvas.fillCircle(cx, cy - 1, 12, canvas.color565(205, 225, 238));
  canvas.drawCircle(cx, cy - 1, 12, TFT_WHITE);
  canvas.fillEllipse(cx, cy, 8, 6, azulNoche);
  int alaY = (frame == 0) ? cy + 7 : ((frame == 1) ? cy + 4 : cy + 1);
  canvas.fillTriangle(cx-4, cy+2, cx+3, cy+2, cx, alaY+3, azulPanel);
  canvas.drawLine(cx-3,cy+3,cx,alaY+2,cian);
  canvas.drawLine(cx,cy+3,cx+2,alaY+1,canvas.color565(155,70,255));
  canvas.fillRoundRect(cx+2, cy-6, 8, 7, 3, canvas.color565(20, 80, 130));
  canvas.drawFastHLine(cx+4, cy-5, 4, cian);
  canvas.fillTriangle(cx+9,cy,cx+15,cy-1,cx+9,cy+2, COL_BIRD_OR);
  canvas.fillRoundRect(cx-12, cy-5, 5, 10, 2, TFT_DARKGREY);
  canvas.fillTriangle(cx-12,cy+3,cx-18,cy+5,cx-11,cy+7,
                      frame == 1 ? cian : canvas.color565(115,70,255));
  canvas.drawPixel(cx-4,cy-2,TFT_WHITE);
}

const char* nombrePersonajePrincesita(uint8_t personaje) {
  switch (personaje) {
    case PERSONAJE_AVENTURERO_CELESTE: return "Coqui Celeste";
    case PERSONAJE_AVENTURERO_BOSCOSO: return "Coqui Boscoso";
    case PERSONAJE_AVENTURERO_RUBI: return "Coqui Rubi";
    case PERSONAJE_AVENTURERO_AMATISTA: return "Coqui Amatista";
    case PERSONAJE_AVENTURERO_ECLIPSE: return "Coqui Eclipse";
    case PERSONAJE_AVENTURERO_COPITO: return "Coqui Copito";
    case PERSONAJE_PRINCESITA_REAL: return "Princesita Astro";
    case PERSONAJE_AVENTURERO_ESTELAR: return "Coqui Estelar";
    default: return "Coqui Solecito";
  }
}

void canvasDrawBirdSeleccionado(int bx, int by, int frame, float angle) {
  switch (modoJuego) {
    case PERSONAJE_AVENTURERO_CELESTE:
      canvasDrawBirdTematico(bx,by,frame,angle,canvas.color565(75,190,245),canvas.color565(25,90,155),TFT_WHITE,TFT_CYAN,0); break;
    case PERSONAJE_AVENTURERO_BOSCOSO:
      canvasDrawBirdTematico(bx,by,frame,angle,canvas.color565(65,175,70),canvas.color565(20,80,35),canvas.color565(105,205,75),canvas.color565(30,120,45),1); break;
    case PERSONAJE_AVENTURERO_RUBI:
      canvasDrawBirdTematico(bx,by,frame,angle,canvas.color565(220,45,45),canvas.color565(100,15,20),canvas.color565(255,135,40),canvas.color565(255,210,55),0); break;
    case PERSONAJE_AVENTURERO_AMATISTA:
      canvasDrawBirdTematico(bx,by,frame,angle,canvas.color565(145,65,205),canvas.color565(55,20,95),canvas.color565(210,110,255),canvas.color565(85,225,255),0); break;
    case PERSONAJE_AVENTURERO_ECLIPSE:
      canvasDrawBirdTematico(bx,by,frame,angle,canvas.color565(25,20,45),TFT_BLACK,canvas.color565(75,35,110),canvas.color565(185,65,255),2); break;
    case PERSONAJE_AVENTURERO_COPITO:
      canvasDrawBirdTematico(bx,by,frame,angle,TFT_WHITE,canvas.color565(120,175,205),canvas.color565(180,230,255),TFT_CYAN,3); break;
    case PERSONAJE_PRINCESITA_REAL:
      canvasDrawBirdTematico(bx,by,frame,angle,COL_BIRD_M,COL_BIRD_MD,COL_BIRD_OR,0xFBB7,4); break;
    case PERSONAJE_AVENTURERO_ESTELAR:
      canvasDrawBirdEstelar(bx,by,frame,angle); break;
    default:
      canvasDrawBird(bx, by, frame, angle); break;
  }
}

// ════════════════════════════════════════════════════════════
//  CORAZÓN
// ════════════════════════════════════════════════════════════
void canvasDrawHeart(int cx, int cy, int sz, uint16_t col) {
  int r = sz / 2;
  canvas.fillCircle(cx - r/2, cy - r/4, r/2, col);
  canvas.fillCircle(cx + r/2, cy - r/4, r/2, col);
  canvas.fillTriangle(cx - r, cy - r/4, cx + r, cy - r/4, cx, cy + r*3/4, col);
}

// ── Jaula colgando de la esquina superior derecha ────────────
// x,y = posición de la esquina superior izquierda de la imagen.
// Con y negativo, la jaula queda "cortada" por el borde de arriba,
// dando el efecto de estar colgada del filo de la pantalla.
// Se dibuja pixel a pixel para lograr transparencia real en el blanco
// (esta versión de TFT_eSPI no soporta el color transparente en pushImage de sprites).
void canvasDrawCage(int x, int y) {
  for (int j = 0; j < CAGE_H; j++) {
    int py = y + j;
    if (py < 0 || py >= SCR_H) continue;
    for (int i = 0; i < CAGE_W; i++) {
      uint16_t c = cageImg[j * CAGE_W + i];
      if (c == 0xFFFF) continue; // blanco = transparente, se salta
      int px = x + i;
      if (px < 0 || px >= SCR_W) continue;
      canvas.drawPixel(px, py, c);
    }
  }
}

void cargarConfiguracionPrincesita() {
  preferenciasArcade.begin("princesita", true);
  modoJuego = preferenciasArcade.getUChar("modo", 0);
  mapaJuego = preferenciasArcade.getUChar("mapa", 0);
  preferenciasArcade.end();

  if (modoJuego >= PERSONAJE_PRINCESITA_TOTAL ||
      !sistemaDesbloqueos.personajeDisponible(modoJuego, sistemaLogros)) {
    modoJuego = PERSONAJE_AVENTURERO_SOLECITO;
  }
  if (mapaJuego >= MAPA_PRINCESITA_TOTAL ||
      !sistemaDesbloqueos.mapaDisponible(mapaJuego, sistemaLogros)) {
    mapaJuego = MAPA_DIA;
  }
}

void guardarConfiguracionPrincesita() {
  preferenciasArcade.begin("princesita", false);
  preferenciasArcade.putUChar("modo", modoJuego);
  preferenciasArcade.putUChar("mapa", mapaJuego);
  preferenciasArcade.end();
}

void drawPrincesitaPanelBase(const char* titulo) {
  canvasDrawBackgroundSelected();

  if (mapaJuego != 2) for (int i = 0; i < MAX_CLOUDS; ++i) {
    canvasDrawCloud(
      static_cast<int>(clouds[i].x),
      static_cast<int>(clouds[i].y),
      clouds[i].w,
      clouds[i].h
    );
  }

  canvasDrawGround();
  canvasDrawMagicForestDecor();
  canvasDrawSnowDecor();
  canvasDrawNightFireflies();

  canvas.fillRoundRect(31, 31, SCR_W - 62, 178, 12, 0x2104);
  canvas.drawRoundRect(31, 31, SCR_W - 62, 178, 12, TFT_WHITE);

  canvas.setTextDatum(MC_DATUM);
  canvas.setTextColor(TFT_WHITE);
  canvas.drawString(titulo, SCR_W / 2, 52, 2);
}

void drawPrincesitaMainMenu() {
  drawPrincesitaPanelBase("PRINCESITA");

  static const char* opciones[] = {
    "EMPEZAR",
    "CONFIGURACION"
  };

  for (int i = 0; i < 2; ++i) {
    const int y = 103 + i * 50;
    const bool seleccionada = opcionMenuPrincipal == i;

    if (seleccionada) {
      canvas.fillRoundRect(65, y - 18, SCR_W - 130, 37, 8, TFT_BLACK);
      canvas.drawRoundRect(65, y - 18, SCR_W - 130, 37, 8, COL_BIRD_M);
    }

    canvas.setTextColor(seleccionada ? COL_BIRD_M : TFT_WHITE);
    canvas.drawString(
      String(seleccionada ? "> " : "  ") + opciones[i],
      SCR_W / 2, y, 2
    );
  }

  canvas.setTextColor(TFT_LIGHTGREY);
  canvas.drawString("Arriba/Abajo: elegir   A: aceptar", SCR_W / 2, 194, 1);
  canvas.setTextDatum(TL_DATUM);
  perfilPushSprite(0, 0);
}

void drawPrincesitaConfigMenu() {
  drawPrincesitaPanelBase("CONFIGURACION");

  static const char* etiquetas[] = {
    "Personaje",
    "Mapa",
    "Volver"
  };

  for (int i = 0; i < 3; ++i) {
    const int y = 91 + i * 43;
    const bool seleccionada = opcionConfigPrincesita == i;

    if (seleccionada) {
      canvas.fillRoundRect(48, y - 15, SCR_W - 96, 31, 7, TFT_BLACK);
      canvas.drawRoundRect(48, y - 15, SCR_W - 96, 31, 7, COL_BIRD_M);
    }

    canvas.setTextColor(seleccionada ? COL_BIRD_M : TFT_WHITE);

    String valor;
    if (i == 0) {
      valor = nombrePersonajePrincesita(modoJuego);
    } else if (i == 1) {
      if (mapaJuego == 0) valor = "Dia";
      else if (mapaJuego == 1) valor = "Noche";
      else if (mapaJuego == 2) valor = "Bosque";
      else valor = "Nieve";
    }

    String linea = etiquetas[i];
    if (i < 2) linea += ": " + valor;

    canvas.drawString(
      String(seleccionada ? "> " : "  ") + linea,
      SCR_W / 2, y, 2
    );
  }

  // Previsualizacion animada del personaje seleccionado.
  if (opcionConfigPrincesita == 0) {
    const int previewFrame = (millis() / 140) % 3;
    canvas.fillRoundRect(SCR_W - 86, 42, 58, 42, 7, TFT_BLACK);
    canvas.drawRoundRect(SCR_W - 86, 42, 58, 42, 7, COL_BIRD_M);
    canvasDrawBirdSeleccionado(SCR_W - 57, 63, previewFrame, -8);
    canvas.setTextDatum(MC_DATUM);
    canvas.setTextColor(TFT_LIGHTGREY);
    canvas.drawString("PREVIA", SCR_W - 57, 80, 1);
  }

  canvas.setTextColor(TFT_LIGHTGREY);
  canvas.drawString(
    "Izq/Der: cambiar   A: aceptar   MENU: volver",
    SCR_W / 2, 207, 1
  );

  canvas.setTextColor(TFT_ORANGE);
  if (!sistemaDesbloqueos.mapaDisponible(MAPA_NOCHE, sistemaLogros))
    canvas.drawString("Mapas extra: logros de Astronauta", SCR_W / 2, 68, 1);


  canvas.setTextDatum(TL_DATUM);
  perfilPushSprite(0, 0);
}

// ════════════════════════════════════════════════════════════
//  PANTALLA DE INICIO
// ════════════════════════════════════════════════════════════
// Rectángulo aproximado del botón "JUGAR" dibujado dentro del splash
#define JUGAR_BTN_X   116
#define JUGAR_BTN_Y   183
#define JUGAR_BTN_W    88
#define JUGAR_BTN_H    27

void drawStartScreen() {
  // Marca de tiempo desde que entramos a STATE_START, para el fade-in
  static unsigned long startScreenBegin = 0;
  static bool wasInStart = false;
  if (!wasInStart) { startScreenBegin = millis(); wasInStart = true; }
  unsigned long t = millis() - startScreenBegin;

  // 1) Imagen splash pixel art de fondo, a pantalla completa
  canvas.setSwapBytes(true);   // corrige orden de bytes RGB565 del array
  canvas.pushImage(0, 0, SPLASH_W, SPLASH_H, splashImage);
  canvas.setSwapBytes(false);  // restaurar para el resto de los dibujos

  // 2) Resplandor pulsante alrededor del botón "JUGAR" para invitar a presionar
  float pulse = 0.5f + 0.5f * sinf(millis() * 0.006f);
  uint8_t glowW = (uint8_t)(2 + pulse * 2);
  uint16_t glowColor = canvas.color565(255, 255, (uint8_t)(180 + 75 * pulse));
  for (uint8_t i = 0; i < glowW; i++) {
    canvas.drawRoundRect(JUGAR_BTN_X - i, JUGAR_BTN_Y - i,
                          JUGAR_BTN_W + i * 2, JUGAR_BTN_H + i * 2,
                          10, glowColor);
  }

  // 3) Mejor puntaje, sobreimpreso abajo si existe
  if (bestScore > 0) {
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    canvas.setCursor(SCR_W / 2 - 30, SCR_H - 14);
    canvas.print("Mejor: "); canvas.print(bestScore);
  }

  // 4) Fade-in de entrada (de negro a la imagen) durante los primeros 450ms
  if (t < 450) {
    uint8_t fadeAlpha = (uint8_t)(255 * (1.0f - t / 450.0f));
    for (int y = 0; y < SCR_H; y++)
      for (int x = 0; x < SCR_W; x++) {
        uint16_t c = canvas.readPixel(x, y);
        uint8_t r  = ((c >> 11) & 0x1F) * (255 - fadeAlpha) / 255;
        uint8_t g  = ((c >>  5) & 0x3F) * (255 - fadeAlpha) / 255;
        uint8_t bv = ( c        & 0x1F) * (255 - fadeAlpha) / 255;
        canvas.drawPixel(x, y, (r << 11) | (g << 5) | bv);
      }
  }

  perfilPushSprite(0, 0);
}

// ════════════════════════════════════════════════════════════
//  GAME OVER
// ════════════════════════════════════════════════════════════
void drawGameOver() {
  canvasDrawBackgroundSelected();
  if (mapaJuego != 2) for (int i = 0; i < MAX_CLOUDS; i++)
    canvasDrawCloud((int)clouds[i].x, (int)clouds[i].y, clouds[i].w, clouds[i].h);
  // La decoración del bosque va detrás de las tuberías.
  canvasDrawMagicForestDecor();
  canvasDrawSnowDecor();
  for (int i = 0; i < MAX_PIPES; i++) {
    int topH = pipes[i].gapY, botY = pipes[i].gapY + PIPE_GAP;
    canvasDrawPipe((int)pipes[i].x, topH, botY, GROUND_Y - botY);
  }
  canvasDrawGround();
  canvasDrawNightFireflies();
  canvasDrawBirdSeleccionado((int)bird.x, min((int)bird.y, GROUND_Y-8), bird.frame, 80);

  // SE ELIMINÓ EL BUCLE FOR QUE DIBUJABA EL FONDO OSCURO DETRÁS DEL PANEL

  canvas.fillRoundRect(45, 40, SCR_W-90, 170, 12, COL_PANEL);
  canvas.drawRoundRect(45, 40, SCR_W-90, 170, 12, COL_PIPE_DK);
  canvas.drawRoundRect(46, 41, SCR_W-92, 168, 11, TFT_WHITE);

  canvas.setTextSize(2);
  canvas.setTextColor(COL_GAME_OVER, COL_PANEL);
  canvas.setCursor(75, 55); canvas.print("ESTADISTICAS");
  canvas.drawFastHLine(65, 85, SCR_W-130, COL_PIPE_DK);

  canvas.setTextSize(2);
  canvas.setTextColor(TFT_BLACK, COL_PANEL);
  canvas.setCursor(85, 100); canvas.print("Puntaje:  ");
  canvas.setTextColor(COL_PIPE_DK, COL_PANEL); canvas.print(score);
  canvas.setTextColor(TFT_BLACK, COL_PANEL);
  canvas.setCursor(85, 125); canvas.print("Mejor:    ");
  canvas.setTextColor(0xD400, COL_PANEL); canvas.print(bestScore);

  if (score >= 30) {
    canvas.fillCircle(SCR_W/2,165,22,0xFFE0); canvas.drawCircle(SCR_W/2,165,22,0xFCC0);
    canvas.setTextSize(1); canvas.setTextColor(TFT_RED,0xFFE0);
    canvas.setCursor(SCR_W/2-10,160); canvas.print("ORO!");
  } else if (score >= 10) {
    canvas.fillCircle(SCR_W/2,165,22,0xC618); canvas.drawCircle(SCR_W/2,165,22,0x8410);
    canvas.setTextSize(1); canvas.setTextColor(TFT_BLACK,0xC618);
    canvas.setCursor(SCR_W/2-14,160); canvas.print("PLATA");
  } else {
    canvas.fillCircle(SCR_W/2,165,22,0xCA93); canvas.drawCircle(SCR_W/2,165,22,0x9240);
    canvas.setTextSize(1); canvas.setTextColor(TFT_BLACK,0xCA93);
    canvas.setCursor(SCR_W/2-14,160); canvas.print("BRONCE");
  }

  canvas.setTextSize(1);
  canvas.setTextColor(TFT_DARKGREY, COL_PANEL);
  canvas.setCursor(112,195); canvas.print("Boton = reiniciar");
  perfilPushSprite(0, 0);
}

// ════════════════════════════════════════════════════════════
//  PRINCESA RESCATADA  ♥  se activa al llegar a RESCUE_SCORE
// ════════════════════════════════════════════════════════════
void runRescueScreen() {
  unsigned long t = millis() - rescueStart;

  // Jingle de fanfarria solo una vez (arranca a los 300ms, no bloqueante)
  if (!rescueJinglePlayed && t > 300) {
    rescueJinglePlayed = true;
    soundRescue();
  }
  updateRescueMusic(); // avanza el jingle nota a nota, sin congelar la pantalla

  // Fondo atardecer
  for (int y = 0; y < GROUND_Y; y++) {
    float frac = (float)y / GROUND_Y;
    uint8_t r = (uint8_t)(31 - 5  * frac);
    uint8_t g = (uint8_t)(10  + 8 * frac);
    uint8_t b = (uint8_t)(18  - 15 * frac);
    canvas.drawFastHLine(0, y, SCR_W, canvas.color565(r<<3, g<<2, b<<3));
  }

  // Sol de atardecer
  canvas.fillCircle(120, GROUND_Y - 30, 38, canvas.color565(255, 80, 0));
  canvas.fillCircle(120, GROUND_Y - 30, 28, canvas.color565(255, 140, 0));
  canvas.fillCircle(120, GROUND_Y - 30, 18, canvas.color565(255, 200, 50));
  canvas.fillRect(90, GROUND_Y-4, 60, 4, canvas.color565(255, 120, 0));

  // Suelo
  canvas.fillRect(0, GROUND_Y, SCR_W, GROUND_H, canvas.color565(80, 40, 10));
  canvas.fillRect(0, GROUND_Y, SCR_W, 8, canvas.color565(30, 80, 10));
  canvas.drawFastHLine(0, GROUND_Y, SCR_W, canvas.color565(60, 140, 20));

  // Nubes cálidas
  uint16_t warmCloud = canvas.color565(255, 160, 80);
  canvas.fillEllipse(55,  55, 24, 12, warmCloud);
  canvas.fillEllipse(35,  60, 16, 9,  warmCloud);
  canvas.fillEllipse(75,  60, 16, 9,  warmCloud);
  canvas.fillEllipse(170, 45, 20, 10, warmCloud);
  canvas.fillEllipse(152, 50, 14, 8,  warmCloud);
  canvas.fillEllipse(190, 50, 14, 8,  warmCloud);

  // ── Jaula colgando de la esquina superior derecha ──
  int cageX = SCR_W - CAGE_W - 6;
  int cageY = -18; // cortada por el borde de arriba, como si colgara del filo
  canvasDrawCage(cageX, cageY);

  // ── Dos pájaros: Coqui entra por la izquierda, Astro sale volando de la jaula ──
  float birdProg = (t > 400) ? min((float)(t - 400) / 1000.0f, 1.0f) : 0.0f;
  birdProg = 1.0f - (1.0f - birdProg) * (1.0f - birdProg); // ease-out

  int targetX1 = SCR_W / 2 - 22; // Coqui destino izquierda
  int targetX2 = SCR_W / 2 + 22; // Astro destino derecha
  int bx1 = (int)(-20 + (targetX1 + 20) * birdProg);      // Coqui desde izquierda
  int by     = 82 + (int)(sin(t * 0.004f) * 6);
  int bFrame = (t / 130) % 3;

  // Astro parte desde adentro de la jaula (como si escapara por abajo) y
  // vuela en línea curva hasta su posición final junto a Coqui
  int cageBirdStartX = cageX + CAGE_W / 2 + 4;
  int cageBirdStartY = cageY + CAGE_H - 14;
  int bx2 = (int)(cageBirdStartX + (targetX2 - cageBirdStartX) * birdProg);
  int by2 = (int)(cageBirdStartY + (by - cageBirdStartY) * birdProg);

  if (birdProg > 0.0f) {
    canvasDrawBird(bx1, by, bFrame, -10);                // Coqui (amarillo) ← izquierda
    canvasDrawBirdMagenta(bx2, by2, (bFrame+1)%3, -10);  // Astro (magenta)  ← sale de la jaula
  }

  // Corazón entre los dos pájaros
  if (birdProg > 0.85f) {
    float heartProg = (birdProg - 0.85f) / 0.15f;
    float beat = 1.0f + 0.35f * fabs(sin(t * 0.012f));
    int hs = (int)(20 * heartProg * beat);
    if (hs > 2) {
      canvasDrawHeart(SCR_W / 2, by - 22, hs, COL_GAME_OVER);
      canvasDrawHeart(SCR_W / 2 - 1, by - 23, hs / 2, 0xFBCF);
    }
  }

  // ── Banner "Princesa Rescatada" ──
  if (t > 1500) {
    float panProg = min((float)(t - 1500) / 700.0f, 1.0f);
    int panH = (int)(92 * panProg);
    int panY = 112;

    canvas.fillRoundRect(45, panY, SCR_W - 90, panH, 12,
                         canvas.color565(50, 10, 30));
    canvas.drawRoundRect(45, panY, SCR_W - 90, panH, 12,
                         canvas.color565(255, 80, 160));
    canvas.drawRoundRect(46, panY + 1, SCR_W - 92, panH - 2, 11,
                         canvas.color565(255, 180, 220));

    if (panProg > 0.45f) {
      // "Princesa" en magenta
      canvas.setTextSize(2);
      canvas.setTextColor(COL_BIRD_M);
      canvas.setCursor(82, panY + 14);
      canvas.print("Astro y coqui");

      // "Rescatada!" en dorado parpadeante
      float glow = fabs(sin(t * 0.008f));
      uint8_t gc = (uint8_t)(180 + 75 * glow);
      canvas.setTextColor(canvas.color565(255, gc, 0));
      canvas.setCursor(60, panY + 38);
      canvas.print("felices x siempre!");
    }

    if (t > 2800 && panProg >= 1.0f) {
      canvas.drawFastHLine(65, panY + 68, SCR_W - 130,
                           canvas.color565(255, 100, 180));
      canvas.setTextSize(1);
      canvas.setTextColor(canvas.color565(255, 220, 120));
      canvas.setCursor(85, panY + 78);
      canvas.print("Puntos: ");
      canvas.print(score);
      if (score > bestScore) {
        canvas.setTextColor(canvas.color565(255, 80, 0));
        canvas.print("  RECORD!");
      }
    }
  }

  // Corazones decorativos
  if (t > 2200) {
    int numDeco = min((int)((t - 2200) / 300), 5);
    for (int i = 0; i < numDeco; i++) {
      float pulse = 1.0f + 0.2f * fabs(sin(t * 0.009f + i * 0.6f));
      int hs2 = (int)(9 * pulse);
      canvasDrawHeart(68 + i * 46, 104, hs2, COL_GAME_OVER);
    }
  }

  // Texto inferior
  if (t > 3200) {
    canvas.setTextSize(1);
    canvas.setTextColor(canvas.color565(255, 200, 100));
    canvas.setCursor(100, 222);
    canvas.print("Boton = seguir jugando");
  }

  // Fade in al inicio
  uint8_t fadeAlpha = 0;
  if (t < 500) fadeAlpha = (uint8_t)(255 * (1.0f - t / 500.0f));
  if (fadeAlpha > 0) {
    for (int y = 0; y < SCR_H; y++)
      for (int x = 0; x < SCR_W; x++) {
        uint16_t c = canvas.readPixel(x, y);
        uint8_t r  = ((c >> 11) & 0x1F) * (255 - fadeAlpha) / 255;
        uint8_t g  = ((c >>  5) & 0x3F) * (255 - fadeAlpha) / 255;
        uint8_t bv = ( c        & 0x1F) * (255 - fadeAlpha) / 255;
        canvas.drawPixel(x, y, (r << 11) | (g << 5) | bv);
      }
  }

  perfilPushSprite(0, 0);
  // Salir: únicamente con el botón (ver STATE_RESCUED en loop())
}

void drawLogroPrincesitaDesbloqueado() {
  canvasDrawBackgroundSelected();
  if (mapaJuego != 2) for (int i = 0; i < MAX_CLOUDS; ++i) {
    canvasDrawCloud((int)clouds[i].x, (int)clouds[i].y, clouds[i].w, clouds[i].h);
  }
  canvasDrawMagicForestDecor();
  canvasDrawSnowDecor();
  canvasDrawGround();
  canvasDrawNightFireflies();

  canvas.fillRoundRect(28, 43, SCR_W - 56, 151, 12, 0x2104);
  canvas.drawRoundRect(28, 43, SCR_W - 56, 151, 12, TFT_YELLOW);

  // El juego cambia setTextSize() en varias escenas. Si no lo restauramos
  // aquí, el modal hereda ese escalado y los textos pueden salirse del cuadro.
  canvas.setTextSize(1);
  canvas.setTextFont(1);
  canvas.setTextDatum(MC_DATUM);

  canvas.setTextColor(TFT_YELLOW);
  canvas.drawString("LOGRO DESBLOQUEADO", SCR_W / 2, 68, 2);

  canvas.setTextColor(TFT_WHITE);
  canvas.drawString(LOGROS_PRINCESITA[ultimoLogroPrincesita].nombre,
                    SCR_W / 2, 108, 2);

  canvas.setTextColor(TFT_LIGHTGREY);
  canvas.drawString(LOGROS_PRINCESITA[ultimoLogroPrincesita].descripcion,
                    SCR_W / 2, 137, 1);

  canvas.setTextColor(TFT_WHITE);
  canvas.drawString("A: continuar", SCR_W / 2, 174, 1);

  // Dejar también un estado tipográfico neutro para la siguiente pantalla.
  canvas.setTextDatum(TL_DATUM);
  canvas.setTextFont(1);
  canvas.setTextSize(1);
  perfilPushSprite(0, 0);
}

void abrirSiguienteLogroPrincesitaPendiente() {
  ultimoLogroPrincesita = tomarSiguienteLogroPrincesitaPendiente();
  gameState = STATE_ACHIEVEMENT_UNLOCK;
  drawLogroPrincesitaDesbloqueado();
}

// ════════════════════════════════════════════════════════════
//  MENÚ DE PAUSA (MENU) — Salir / Volumen
// ════════════════════════════════════════════════════════════
#define PAUSE_PANEL_X 70
#define PAUSE_PANEL_Y 60
#define PAUSE_PANEL_W 180
#define PAUSE_PANEL_H 120

void drawPauseMenu() {
  canvas.fillRoundRect(PAUSE_PANEL_X, PAUSE_PANEL_Y, PAUSE_PANEL_W, PAUSE_PANEL_H, 12, COL_PANEL);
  canvas.drawRoundRect(PAUSE_PANEL_X, PAUSE_PANEL_Y, PAUSE_PANEL_W, PAUSE_PANEL_H, 12, COL_PIPE_DK);
  canvas.drawRoundRect(PAUSE_PANEL_X+1, PAUSE_PANEL_Y+1, PAUSE_PANEL_W-2, PAUSE_PANEL_H-2, 11, TFT_WHITE);

  canvas.setTextSize(2);
  canvas.setTextColor(COL_GAME_OVER, COL_PANEL);
  canvas.setCursor(PAUSE_PANEL_X + 48, PAUSE_PANEL_Y + 12);
  canvas.print("MENU");
  canvas.drawFastHLine(PAUSE_PANEL_X + 15, PAUSE_PANEL_Y + 38, PAUSE_PANEL_W - 30, COL_PIPE_DK);

  canvas.setTextSize(2);
  canvas.setTextColor(pausaSeleccion == PAUSA_SALIR ? 0xD400 : TFT_BLACK, COL_PANEL);
  canvas.setCursor(PAUSE_PANEL_X + 25, PAUSE_PANEL_Y + 52);
  canvas.print(pausaSeleccion == PAUSA_SALIR ? "> Salir" : "  Salir");

  canvas.setTextColor(pausaSeleccion == PAUSA_VOLUMEN_OPT ? 0xD400 : TFT_BLACK, COL_PANEL);
  canvas.setCursor(PAUSE_PANEL_X + 25, PAUSE_PANEL_Y + 80);
  canvas.print(pausaSeleccion == PAUSA_VOLUMEN_OPT ? "> Sonido" : "  Sonido");

  canvas.setTextSize(1);
  canvas.setTextColor(TFT_DARKGREY, COL_PANEL);
  canvas.setCursor(PAUSE_PANEL_X + 10, PAUSE_PANEL_Y + 105);
  canvas.print("Cruceta mueve, A elige");

  dibujarTemperaturaPausaCanvas();
  perfilPushSprite(0, 0);
}

void drawVolumeScreen() {
  canvas.fillRoundRect(PAUSE_PANEL_X, PAUSE_PANEL_Y, PAUSE_PANEL_W, PAUSE_PANEL_H, 12, COL_PANEL);
  canvas.drawRoundRect(PAUSE_PANEL_X, PAUSE_PANEL_Y, PAUSE_PANEL_W, PAUSE_PANEL_H, 12, COL_PIPE_DK);
  canvas.drawRoundRect(PAUSE_PANEL_X+1, PAUSE_PANEL_Y+1, PAUSE_PANEL_W-2, PAUSE_PANEL_H-2, 11, TFT_WHITE);

  canvas.setTextSize(2);
  canvas.setTextColor(COL_GAME_OVER, COL_PANEL);
  canvas.setCursor(PAUSE_PANEL_X + 30, PAUSE_PANEL_Y + 12);
  canvas.print("SONIDO");
  canvas.drawFastHLine(PAUSE_PANEL_X + 15, PAUSE_PANEL_Y + 38, PAUSE_PANEL_W - 30, COL_PIPE_DK);

  int barX = PAUSE_PANEL_X + 20, barY = PAUSE_PANEL_Y + 50, barW = PAUSE_PANEL_W - 40, barH = 20;
  canvas.drawRect(barX, barY, barW, barH, TFT_BLACK);
  int utilW = barW - 4;
  int rellenoW = (utilW * volumenPaso) / VOLUME_PASOS_MAX;
  canvas.fillRect(barX + 2, barY + 2, utilW, barH - 4, COL_PANEL);
  uint16_t colorBarra = TFT_GREEN;
  if (volumenPaso <= 3) colorBarra = TFT_RED;
  else if (volumenPaso <= 6) colorBarra = 0xFD20;
  if (rellenoW > 0) canvas.fillRect(barX + 2, barY + 2, rellenoW, barH - 4, colorBarra);

  canvas.setTextSize(2);
  canvas.setTextColor(TFT_BLACK, COL_PANEL);
  canvas.setCursor(barX + 40, barY + 28);
  canvas.print(arcadeNombreVolumen());

  canvas.setTextSize(1);
  canvas.setTextColor(TFT_DARKGREY, COL_PANEL);
  canvas.setCursor(PAUSE_PANEL_X + 6, PAUSE_PANEL_Y + 105);
  canvas.print("Izq/Der: nivel  A: volver");

  perfilPushSprite(0, 0);
}

// Se llama en cada loop() mientras gameState == STATE_PAUSED
void manejarMenuPausa() {
  DirCruceta dir = leerDireccionCruceta();

  bool okAhora = (digitalRead(BUTTON_PIN) == LOW);
  bool okPressed = (okAhora && !botonOkPausaPrev);
  botonOkPausaPrev = okAhora;

  if (pauseState == PAUSE_MENU) {
    if (dir == DIR_ARRIBA || dir == DIR_ABAJO) {
      pausaSeleccion = (pausaSeleccion == PAUSA_SALIR) ? PAUSA_VOLUMEN_OPT : PAUSA_SALIR;
      drawPauseMenu();
    }
    if (okPressed) {
      if (pausaSeleccion == PAUSA_VOLUMEN_OPT) {
        pauseState = PAUSE_VOLUMEN;
        drawVolumeScreen();
      } else {
        // SALIR → volver al menú principal del arcade
        startMusicPlaying = false;
        musicIndex = 0;
        buzzerOff();
        pauseState = PAUSE_MENU;
        gameState = STATE_START;   // queda listo por si se vuelve a elegir
        volverAlMenu();
        return;
      }
    }
  } else { // PAUSE_VOLUMEN
    if (dir == DIR_DERECHA) {
      if (volumenPaso < VOLUME_PASOS_MAX) {
        volumenPaso++;
        aplicarVolumenChopper();
        drawVolumeScreen();
      }
    } else if (dir == DIR_IZQUIERDA) {
      if (volumenPaso > 0) {
        volumenPaso--;
        aplicarVolumenChopper();
        drawVolumeScreen();
      }
    }
    if (okPressed) {
      pauseState = PAUSE_MENU;
      drawPauseMenu();
    }
  }
}

void drawParticles();
void canvasDrawScore();

void iniciarCuentaRegresivaReanudar() {
  resumeCountdownStart = millis();
  resumeCountdownShown = -1;
  gameState = STATE_RESUME_COUNTDOWN;
  buzzerOff();
}

void actualizarCuentaRegresivaReanudar() {
  const unsigned long transcurrido = millis() - resumeCountdownStart;

  // Dos segundos: muestra 2 durante el primero y 1 durante el segundo.
  int numero = (transcurrido < 1000UL) ? 2 : 1;

  if (numero != resumeCountdownShown) {
    resumeCountdownShown = numero;

    // Redibuja el gameplay congelado como fondo.
    canvasDrawBackgroundSelected();

    if (mapaJuego == 0) {
      for (int i = 0; i < MAX_CLOUDS; ++i) {
        canvasDrawCloud(
          static_cast<int>(clouds[i].x),
          static_cast<int>(clouds[i].y),
          clouds[i].w,
          clouds[i].h
        );
      }
    }

    // La decoración del bosque se pinta primero para que los pipes
    // siempre permanezcan visualmente al frente.
    canvasDrawMagicForestDecor();
    canvasDrawSnowDecor();
    for (int i = 0; i < MAX_PIPES; ++i) {
      const int topH = pipes[i].gapY;
      const int botY = pipes[i].gapY + PIPE_GAP;
      canvasDrawPipe(
        static_cast<int>(pipes[i].x),
        topH,
        botY,
        GROUND_Y - botY
      );
    }

    canvasDrawGround();
    canvasDrawNightFireflies();
    drawParticles();
    canvasDrawBirdSeleccionado(
      static_cast<int>(bird.x),
      static_cast<int>(bird.y),
      bird.frame,
      bird.angle
    );
    canvasDrawScore();

    // Panel central para la cuenta regresiva.
    canvas.fillRoundRect(SCR_W / 2 - 42, SCR_H / 2 - 48, 84, 96, 12, TFT_BLACK);
    canvas.drawRoundRect(SCR_W / 2 - 42, SCR_H / 2 - 48, 84, 96, 12, TFT_WHITE);
    canvas.setTextDatum(MC_DATUM);
    canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    canvas.drawString(String(numero), SCR_W / 2, SCR_H / 2, 7);
    canvas.setTextDatum(TL_DATUM);

    perfilPushSprite(0, 0);
  }

  if (transcurrido >= 2000UL) {
    resumeCountdownShown = -1;
    gameState = STATE_PLAYING;

    // Evita que el boton usado al salir del menu provoque un salto.
    btnCurrent = leerBoton();
    btnPrev = btnCurrent;
  }
}

// ════════════════════════════════════════════════════════════
//  LÓGICA
// ════════════════════════════════════════════════════════════
void initClouds() {
  for (int i = 0; i < MAX_CLOUDS; i++) {
    clouds[i].x = random(0, SCR_W); clouds[i].y = random(15, GROUND_Y/2);
    clouds[i].w = random(30, 60);   clouds[i].h = random(14, 28);
    clouds[i].speed = random(1, 3);
  }
}

void initPipes() {
  for (int i = 0; i < MAX_PIPES; i++) {
    pipes[i].x = SCR_W + i*(PIPE_WIDTH+PIPE_INTERVAL);
    pipes[i].gapY = random(50, GROUND_Y-PIPE_GAP-50);
    pipes[i].scored = false;
  }
}

void resetGame() {
  concederLogroPrincesita(LOGRO_PRIMER_VUELO);
  bird.x=SCR_W/4; bird.y=SCR_H/2; bird.vy=0; bird.angle=0;
  bird.frame=0; bird.animTick=0;
  score=0; groundScroll=0;
  rescueJinglePlayed = false;
  rescueMusicPlaying = false; buzzerOff();
  for (int i=0;i<MAX_PARTICLES;i++) particles[i].active=false;
  trailTick = 0;
  initPipes(); gameState=STATE_PLAYING;
}

// ══════════════════════════════════════════════
//  PARTÍCULAS
// ══════════════════════════════════════════════
void spawnParticles(float x, float y, uint8_t type, uint8_t count) {
  const bool tematico = modoJuego != PERSONAJE_AVENTURERO_SOLECITO;
  for (int n = 0; n < count; n++) {
    for (int i = 0; i < MAX_PARTICLES; i++) {
      if (!particles[i].active) {
        particles[i].active = true;
        particles[i].type   = tematico ? (uint8_t)(10 + modoJuego) : type;
        particles[i].x      = x + random(-4, 5);
        particles[i].y      = y + random(-4, 5);
        if (type == 0) {
          particles[i].vx = -PIPE_SPEED + random(-10, 11) / 20.0f;
          particles[i].vy = random(-5, 6) / 10.0f;
          particles[i].maxLife = tematico ? 18 : 12;
        } else {
          particles[i].vx = random(-30, 31) / 10.0f;
          particles[i].vy = random(-30, 10) / 10.0f;
          particles[i].maxLife = random(15, 30);
        }
        switch (modoJuego) {
          case PERSONAJE_AVENTURERO_CELESTE: particles[i].color = random(2) ? TFT_CYAN : TFT_WHITE; break;
          case PERSONAJE_AVENTURERO_BOSCOSO: {
            // Mezcla visible de tierra y hojas. El marrón domina para que la
            // estela se lea incluso sobre el fondo verde del Bosque Mágico.
            const uint8_t variante = random(4);
            if (variante < 2) particles[i].color = canvas.color565(145, 92, 45);
            else if (variante == 2) particles[i].color = canvas.color565(92, 58, 30);
            else particles[i].color = canvas.color565(105, 205, 75);
            break;
          }
          case PERSONAJE_AVENTURERO_RUBI: particles[i].color = random(2) ? canvas.color565(255,70,30) : TFT_YELLOW; break;
          case PERSONAJE_AVENTURERO_AMATISTA: particles[i].color = random(2) ? canvas.color565(190,80,255) : TFT_CYAN; break;
          case PERSONAJE_AVENTURERO_ECLIPSE: particles[i].color = random(2) ? canvas.color565(110,45,170) : canvas.color565(35,25,60); break;
          case PERSONAJE_AVENTURERO_COPITO: particles[i].color = random(2) ? TFT_WHITE : canvas.color565(150,225,255); break;
          case PERSONAJE_PRINCESITA_REAL: particles[i].color = random(3) ? canvas.color565(255,100,190) : TFT_YELLOW; break;
          case PERSONAJE_AVENTURERO_ESTELAR: particles[i].color = random(3)==0 ? canvas.color565(180,70,255) : (random(2) ? TFT_CYAN : TFT_WHITE); break;
          default: {
            uint16_t cols[3] = {C_EXPLODE1, C_EXPLODE2, C_EXPLODE3};
            particles[i].color = (type == 0) ? C_SMOKE : cols[random(0,3)];
          }
        }
        particles[i].life = particles[i].maxLife;
        break;
      }
    }
  }
}

void updateParticles() {
  for (int i = 0; i < MAX_PARTICLES; i++) {
    if (!particles[i].active) continue;
    particles[i].x += particles[i].vx;
    particles[i].y += particles[i].vy;
    if (particles[i].type != 10 + PERSONAJE_AVENTURERO_CELESTE) particles[i].vy += 0.08f;
    if (particles[i].life > 0) particles[i].life--;
    else particles[i].active = false;
  }
}

void drawParticles() {
  for (int i = 0; i < MAX_PARTICLES; i++) {
    if (!particles[i].active) continue;
    const int px = (int)particles[i].x, py = (int)particles[i].y;
    const uint8_t tipo = particles[i].type;
    if (tipo < 10) {
      int sz = (tipo == 0) ? map(particles[i].life,0,particles[i].maxLife,0,2)
                           : map(particles[i].life,0,particles[i].maxLife,0,3);
      if (sz < 1) sz = 1;
      canvas.fillCircle(px,py,sz,particles[i].color);
      continue;
    }
    const uint8_t personaje = tipo - 10;
    switch (personaje) {
      case PERSONAJE_AVENTURERO_CELESTE:
        canvas.drawCircle(px,py,1 + (particles[i].life % 2),particles[i].color); break;
      case PERSONAJE_AVENTURERO_BOSCOSO:
        if ((particles[i].life % 3) == 0) {
          // Hoja que gira.
          canvas.fillEllipse(px, py, 3, 1, particles[i].color);
          canvas.drawLine(px-2, py+1, px+2, py-1,
                          canvas.color565(35,110,40));
        } else {
          // Polvito de tierra: más grande y marrón para que sí sea visible.
          const int radio = 1 + ((particles[i].life / 3) & 1);
          canvas.fillCircle(px, py, radio, particles[i].color);
          canvas.drawPixel(px+radio, py, canvas.color565(190,130,65));
        }
        break;
      case PERSONAJE_AVENTURERO_RUBI:
        canvas.fillTriangle(px,py-2,px-2,py+2,px+2,py+2,particles[i].color); break;
      case PERSONAJE_AVENTURERO_AMATISTA:
        canvas.drawFastHLine(px-2,py,5,particles[i].color);
        canvas.drawFastVLine(px,py-2,5,particles[i].color); break;
      case PERSONAJE_AVENTURERO_ECLIPSE:
        canvas.fillCircle(px,py,2,particles[i].color);
        canvas.drawPixel(px+2,py-1,canvas.color565(190,75,255)); break;
      case PERSONAJE_AVENTURERO_COPITO:
        canvas.drawFastHLine(px-2,py,5,particles[i].color);
        canvas.drawFastVLine(px,py-2,5,particles[i].color);
        canvas.drawPixel(px-1,py-1,particles[i].color);
        canvas.drawPixel(px+1,py+1,particles[i].color); break;
      case PERSONAJE_PRINCESITA_REAL:
        if ((particles[i].life % 9) == 0) {
          canvas.fillCircle(px-1,py,1,particles[i].color);
          canvas.fillCircle(px+1,py,1,particles[i].color);
          canvas.fillTriangle(px-2,py,px+2,py,px,py+3,particles[i].color);
        } else {
          canvas.drawPixel(px,py,particles[i].color);
          canvas.drawPixel(px+1,py,particles[i].color);
          canvas.drawPixel(px,py+1,particles[i].color);
        }
        break;
      case PERSONAJE_AVENTURERO_ESTELAR:
        canvas.drawFastHLine(px-2,py,5,particles[i].color);
        canvas.drawFastVLine(px,py-2,5,particles[i].color);
        canvas.drawPixel(px-1,py-1,TFT_WHITE);
        canvas.drawPixel(px+1,py+1,TFT_WHITE); break;
      default: canvas.drawPixel(px,py,particles[i].color); break;
    }
  }
}

bool checkCollision() {
  // Hitbox rectangular ajustada al sprite real (elipse 9x7 + cola/pico),
  // achicada un poco extra para que el juego sea justo con el jugador.
  int bx=(int)bird.x, by=(int)bird.y;
  int bw=7, bh=6; // ancho/alto medios de la caja de colisión
  if (by+bh>=GROUND_Y || by-bh<=0) return true;
  for (int i=0;i<MAX_PIPES;i++) {
    int px=(int)pipes[i].x;
    if (bx+bw>px && bx-bw<px+PIPE_WIDTH)
      if (by-bh<pipes[i].gapY || by+bh>pipes[i].gapY+PIPE_GAP) return true;
  }
  return false;
}

void canvasDrawScore() {
  canvas.setTextSize(3);
  canvas.setTextColor(COL_SCORE_SHD);
  canvas.setCursor(SCR_W/2-15+2, 12); canvas.print(score);
  canvas.setTextColor(COL_SCORE);
  canvas.setCursor(SCR_W/2-15,   10); canvas.print(score);
}

// ════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════
// Llamado por el launcher cada vez que se entra a este juego desde el menú.
// El hardware (tft, canvas, pines, buzzer, volumen) ya lo inicializó
// ArcadeConsola.ino una sola vez; aquí solo se resetea el estado del juego.
void init() {
  aplicarVolumenChopper();

  randomSeed(esp_random() ^ micros());
  bird.x=SCR_W/4; bird.y=SCR_H/2; bird.vy=0; bird.angle=0;
  bird.frame=0; bird.animTick=0;

  initClouds(); initPipes(); precalcSky();
  cargarConfiguracionPrincesita();

  opcionMenuPrincipal = 0;
  opcionConfigPrincesita = 0;
  botonBPrincesitaPrev = (digitalRead(PIN_VOLUMEN_PANEL) == LOW);
  gameState = STATE_START;
  rescueJinglePlayed  = false;
  rescueMusicPlaying  = false; buzzerOff();

  // Evita el "fantasma" del botón OK: si al entrar desde el menú el
  // dedo todavía está apretando el botón (fue el mismo botón usado
  // para confirmar "Princesita Astro"), sin esto btnPrev arrancaba
  // en false y la primera lectura lo tomaba como pulsación nueva,
  // saltándose la pantalla de inicio e yendo directo al gameplay.
  btnCurrent = leerBoton();
  btnPrev    = btnCurrent;
}

// ════════════════════════════════════════════════════════════
//  UPDATE (una vuelta de "loop" por frame; llamado desde el launcher)
// ════════════════════════════════════════════════════════════
void update() {
  static unsigned long lastFrame = 0;
  unsigned long now = millis();
  updateJumpSound();
  if (now - lastFrame < 20) return;
  lastFrame = now;
  debugRegistrarFrame();

  btnCurrent = leerBoton();
  bool btnPressed = (btnCurrent && !btnPrev);
  btnPrev = btnCurrent;

  // ── BOTÓN DE PAUSA (MENU) ──
  bool pausaAhora = (digitalRead(PIN_PAUSA) == LOW);
  bool pausaPressed = (pausaAhora && !pausaPrev);
  pausaPrev = pausaAhora;

  if (pausaPressed) {
    if (gameState == STATE_START || gameState == STATE_MAIN_MENU) {
      startMusicPlaying = false;
      musicIndex = 0;
      buzzerOff();
      volverAlMenu();
      return;
    }

    if (gameState == STATE_PLAYING) {
      gameState = STATE_PAUSED;
      pauseState = PAUSE_MENU;
      pausaSeleccion = PAUSA_SALIR;
      buzzerOff();
      drawPauseMenu();
    } else if (gameState == STATE_PAUSED) {
      if (pauseState == PAUSE_VOLUMEN) {
        // Si estamos en el submenú de volumen, un toque de pausa
        // vuelve al menú principal en vez de reanudar de una
        pauseState = PAUSE_MENU;
        drawPauseMenu();
      } else {
        iniciarCuentaRegresivaReanudar();
      }
    }
  }

  // ── MENÚ DE PAUSA ──
  if (gameState == STATE_PAUSED) {
    manejarMenuPausa();
    return;
  }

  // ── CUENTA REGRESIVA AL REANUDAR ──
  if (gameState == STATE_RESUME_COUNTDOWN) {
    actualizarCuentaRegresivaReanudar();
    return;
  }

  // ── INICIO ──
if (gameState == STATE_START) {
  // La musica continua hasta entrar realmente al gameplay.
  playStartMusic();

  if (btnPressed) {
    opcionMenuPrincipal = 0;
    gameState = STATE_MAIN_MENU;
    drawPrincesitaMainMenu();
  } else {
    drawStartScreen();
  }
  return;
}

  if (gameState == STATE_MAIN_MENU) {
    // Mantiene la melodia avanzando; evita que una nota quede trabada.
    playStartMusic();

    static DirCruceta direccionAnteriorMenu = DIR_NINGUNO;
    const DirCruceta dir = leerDireccionCruceta();

    if (dir != DIR_NINGUNO && dir != direccionAnteriorMenu) {
      direccionAnteriorMenu = dir;

      if (dir == DIR_ARRIBA || dir == DIR_ABAJO) {
        opcionMenuPrincipal = 1 - opcionMenuPrincipal;
        drawPrincesitaMainMenu();
      }
    }

    if (dir == DIR_NINGUNO) direccionAnteriorMenu = DIR_NINGUNO;

    if (btnPressed) {
      if (opcionMenuPrincipal == 0) {
        startMusicPlaying = false;
        musicIndex = 0;
        buzzerOff();
        resetGame();
      } else {
        opcionConfigPrincesita = 0;
        gameState = STATE_CONFIG_MENU;
        drawPrincesitaConfigMenu();
      }
    }
    return;
  }

  if (gameState == STATE_CONFIG_MENU) {
    // La misma musica sigue durante Configuracion.
    playStartMusic();
    static unsigned long ultimoFramePreview = 0;
    if (opcionConfigPrincesita == 0 && millis() - ultimoFramePreview >= 140) {
      ultimoFramePreview = millis();
      drawPrincesitaConfigMenu();
    }

    static DirCruceta direccionAnteriorConfig = DIR_NINGUNO;
    const DirCruceta dir = leerDireccionCruceta();

    if (dir != DIR_NINGUNO && dir != direccionAnteriorConfig) {
      direccionAnteriorConfig = dir;

      if (dir == DIR_ARRIBA) {
        opcionConfigPrincesita =
            (opcionConfigPrincesita + 2) % 3;
        drawPrincesitaConfigMenu();
      } else if (dir == DIR_ABAJO) {
        opcionConfigPrincesita =
            (opcionConfigPrincesita + 1) % 3;
        drawPrincesitaConfigMenu();
      } else if (dir == DIR_IZQUIERDA || dir == DIR_DERECHA) {
        if (opcionConfigPrincesita == 0) {
          modoJuego = sistemaDesbloqueos.siguientePersonajeDisponible(modoJuego, dir == DIR_IZQUIERDA ? -1 : 1, sistemaLogros);
          guardarConfiguracionPrincesita();
            drawPrincesitaConfigMenu();
        } else if (opcionConfigPrincesita == 1) {
          mapaJuego = sistemaDesbloqueos.siguienteMapaDisponible(
              mapaJuego, dir == DIR_IZQUIERDA ? -1 : 1, sistemaLogros);
          guardarConfiguracionPrincesita();
            drawPrincesitaConfigMenu();
        }
      }
    }

    if (dir == DIR_NINGUNO) direccionAnteriorConfig = DIR_NINGUNO;

    const bool botonBActual =
        digitalRead(PIN_VOLUMEN_PANEL) == LOW;
    const bool botonBPressed =
        botonBActual && !botonBPrincesitaPrev;
    botonBPrincesitaPrev = botonBActual;

    if (btnPressed) {
      if (opcionConfigPrincesita == 0) {
        modoJuego = sistemaDesbloqueos.siguientePersonajeDisponible(modoJuego, 1, sistemaLogros);
        guardarConfiguracionPrincesita();
        drawPrincesitaConfigMenu();
      } else if (opcionConfigPrincesita == 1) {
        mapaJuego = sistemaDesbloqueos.siguienteMapaDisponible(mapaJuego, 1, sistemaLogros);
        guardarConfiguracionPrincesita();
        drawPrincesitaConfigMenu();
      } else {
        opcionMenuPrincipal = 0;
        gameState = STATE_MAIN_MENU;
        drawPrincesitaMainMenu();
      }
    }

    if (pausaPressed || botonBPressed) {
      opcionMenuPrincipal = 0;
      gameState = STATE_MAIN_MENU;
      drawPrincesitaMainMenu();
    }
    return;
  }

  // ── AVISO DE LOGRO ──
  if (gameState == STATE_ACHIEVEMENT_UNLOCK) {
    if (btnPressed) {
      if (hayLogrosPrincesitaPendientes()) {
        abrirSiguienteLogroPrincesitaPendiente();
      } else {
        if (score > bestScore) bestScore = score;
        gameState = STATE_OVER;
        drawGameOver();
      }
    }
    return;
  }

  // ── PRINCESA RESCATADA ──
  if (gameState == STATE_RESCUED) {
    if (btnPressed) {
      if (hayLogrosPrincesitaPendientes()) {
        abrirSiguienteLogroPrincesitaPendiente();
      } else {
        if (score > bestScore) bestScore = score;
        gameState = STATE_OVER;
        drawGameOver();
      }
    } else {
      runRescueScreen();
    }
    return;
  }

  // ── GAME OVER ──
  if (gameState == STATE_OVER) {
    if (btnPressed) {
      startMusicPlaying = false;
      musicIndex = 0;
      buzzerOff();
      resetGame();   // ← directo a jugar, sin pasar por la pantalla de inicio
    }
    return;
  }

  // ── MUERTO ──
  if (gameState == STATE_DEAD) {
    bird.vy += GRAVITY * 2; bird.y += bird.vy;
    canvasDrawBackgroundSelected();
    if (mapaJuego != 2) for (int i=0;i<MAX_CLOUDS;i++) canvasDrawCloud((int)clouds[i].x,(int)clouds[i].y,clouds[i].w,clouds[i].h);
    canvasDrawMagicForestDecor(); // detrás de los pipes
    canvasDrawSnowDecor();         // detrás de los pipes
    for (int i=0;i<MAX_PIPES;i++) { int topH=pipes[i].gapY,botY=pipes[i].gapY+PIPE_GAP; canvasDrawPipe((int)pipes[i].x,topH,botY,GROUND_Y-botY); }
    canvasDrawGround();
    canvasDrawNightFireflies();
    if (bird.y < GROUND_Y-8) canvasDrawBirdSeleccionado((int)bird.x,(int)bird.y,bird.frame,80);
    perfilPushSprite(0, 0);
    if (bird.y >= GROUND_Y+30) {
      if (hayLogrosPrincesitaPendientes()) {
        abrirSiguienteLogroPrincesitaPendiente();
      } else {
        if (score>bestScore) bestScore=score;
        gameState=STATE_OVER; drawGameOver();
      }
    }
    return;
  }

  // ── JUGANDO ──
  if (btnPressed) {
    bird.vy = JUMP_FORCE;
    soundJump();
    spawnParticles(bird.x-4, bird.y+5, 1, 6);   // ráfaga de partículas al saltar
  }

  // Estela de aire continua mientras vuela
  if (++trailTick >= 3) {
    trailTick = 0;
    spawnParticles(bird.x-9, bird.y+2, 0, 1);
  }
  updateParticles();

  bird.vy += GRAVITY; bird.y += bird.vy;
  bird.angle = bird.vy * 5.0f;
  if (bird.angle >  80) bird.angle =  80;
  if (bird.angle < -30) bird.angle = -30;

  bird.animTick++;
  if (bird.animTick >= 5) { bird.animTick=0; bird.frame=(bird.frame+1)%3; }

  for (int i=0;i<MAX_CLOUDS;i++) {
    clouds[i].x -= clouds[i].speed*0.5f;
    if (clouds[i].x+clouds[i].w<0) {
      clouds[i].x=SCR_W+10; clouds[i].y=random(15,GROUND_Y/2);
      clouds[i].w=random(30,60); clouds[i].h=random(14,28);
    }
  }

  for (int i=0;i<MAX_PIPES;i++) {
    pipes[i].x -= PIPE_SPEED;
    if (pipes[i].x+PIPE_WIDTH<0) {
      float maxX=pipes[0].x;
      for (int j=1;j<MAX_PIPES;j++) if(pipes[j].x>maxX) maxX=pipes[j].x;
      pipes[i].x=maxX+PIPE_WIDTH+PIPE_INTERVAL;
      pipes[i].gapY=random(50,GROUND_Y-PIPE_GAP-50);
      pipes[i].scored=false;
    }
    if (!pipes[i].scored && pipes[i].x+PIPE_WIDTH<bird.x) {
      pipes[i].scored=true;
      score++;
      evaluarLogrosPrincesita(mapaJuego, score, false);
      soundPoint();
      // ── ACTIVAR PANTALLA DE RESCATE al llegar a RESCUE_SCORE ──
      if (score >= RESCUE_SCORE) {
        rescueStart        = millis();
        rescueJinglePlayed = false;
        rescueMusicPlaying = false;
        evaluarLogrosPrincesita(mapaJuego, score, mapaJuego == 0);
        gameState          = STATE_RESCUED;
        return;
      }
    }
  }

  groundScroll += PIPE_SPEED;

  if (checkCollision()) {
    soundDie();
    gameState=STATE_DEAD;
    return;
  }

  canvasDrawBackgroundSelected();
  if (mapaJuego != 2) for (int i=0;i<MAX_CLOUDS;i++) canvasDrawCloud((int)clouds[i].x,(int)clouds[i].y,clouds[i].w,clouds[i].h);
  canvasDrawMagicForestDecor(); // detrás de los pipes
  canvasDrawSnowDecor();        // detrás de los pipes
  for (int i=0;i<MAX_PIPES;i++) { int topH=pipes[i].gapY,botY=pipes[i].gapY+PIPE_GAP; canvasDrawPipe((int)pipes[i].x,topH,botY,GROUND_Y-botY); }
  canvasDrawGround();
  canvasDrawNightFireflies();
  drawParticles();
  canvasDrawBirdSeleccionado((int)bird.x,(int)bird.y,bird.frame,bird.angle);
  canvasDrawScore();
  perfilPushSprite(0, 0);
}

} // namespace Princesita

// =============================================================
//  MEMORAMA ARCADE – ESP32-S3 Zero + TFT ST7789 (320x240 horizontal)
//  Hardware:
//    Pines TFT (MOSI/SCLK/CS/DC/RST) van en User_Setup.h de TFT_eSPI,
//    NO se definen en este sketch. Evita usar GPIO1 y GPIO2 para la
//    pantalla, ya están ocupados por la cruceta y el botón OK.
//    Buzzer  = GPIO 7   (pasivo, manejado por LEDC)
//    Sonido = GPIO 13  (transistor "chopper", PWM alta frec. + filtro RC)
//    Pausa   = GPIO 5   (abre/cierra el menú de pausa)
//    Cruceta (D-pad) = GPIO 1 (ADC, escalera de resistencias:
//                       ARRIBA / ABAJO / IZQUIERDA / DERECHA)
//    BTN_OK  = GPIO 2  (confirmar selección / iniciar / reiniciar)
//  Librería: TFT_eSPI  (configurar User_Setup.h con los pines de pantalla)
// =============================================================

// NOTA: TFT_eSPI y pgmspace.h ya se incluyen una sola vez en
// ArcadeConsola.ino, y "tft" es el objeto TFT_eSPI COMPARTIDO
// del launcher (se sigue dibujando directo a tft, sin sprite,
// igual que en el sketch original).

namespace Memorama {

// ── Pines ─────────────────────────────────────────────────────
#define PIN_OK           2
#define PIN_ADC_CRUCETA  1

// ── Buzzer (pasivo, usa LEDC del ESP32) ──────────────────────
#define BUZZER_PIN     7
#define BUZZER_RES     8   // resolución de duty (bits)

// ── Volumen: transistor T2 "chopper" que regula el voltaje que
//    llega al buzzer, vía PWM de alta frecuencia + filtro RC ────
// PIN_VOLUME_CHOPPER, VOLUME_PASOS_MAX y volumenPaso ahora son
// globales y compartidos (ver bloque "SONIDO" cerca del inicio
// del archivo) para que el volumen sea el mismo en todo el arcade.

// ── Botón de pausa ────────────────────────────────────────────
#define PIN_PAUSA  5
bool pausaPrev = false;

enum PauseState { PAUSE_MENU, PAUSE_VOLUMEN };
PauseState pauseState = PAUSE_MENU;

enum PauseOption { PAUSA_SALIR, PAUSA_VOLUMEN_OPT };
PauseOption pausaSeleccion = PAUSA_SALIR;

bool botonOkPausaPrev = false;

// ── Colores ───────────────────────────────────────────────────
#define C_BG        TFT_BLACK
#define C_CARD_BACK 0x2104   // gris oscuro
#define C_CARD_FACE TFT_BLACK
#define C_BORDER    TFT_WHITE
#define C_CURSOR    TFT_YELLOW
#define C_MATCH     0x07E0   // verde
#define C_P1        0x001F   // azul
#define C_P2        0xF800   // rojo
#define C_TEXT      TFT_WHITE

// ── Tablero ───────────────────────────────────────────────────
// 24 cartas en 4 filas × 6 columnas
#define COLS        6
#define ROWS        4
#define NUM_CARDS   (COLS * ROWS)   // 24

// Tamaño y posición de cada carta
#define CARD_W      48
#define CARD_H      44
#define CARD_GAP    4
// Ancho total tablero: 6*(48+4)-4 = 320-4 = 316 → margen 2px cada lado
#define BOARD_X     2
// HUD de 28px arriba; alto tablero: 4*(44+4)-4=192; total=28+192=220 ≤ 240
#define BOARD_Y     28

// Tamaño del bitmap de los iconos (48×44)
#define ICON_W      48
#define ICON_H      44

// ─────────────────────────────────────────────────────────────
//  BITMAPS MONOCROMOS (48×44 px, PROGMEM) — sin cambios
// ─────────────────────────────────────────────────────────────

// 'astro'
const unsigned char epd_bitmap_astro [] PROGMEM = {
  0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xf0,0x00,0x00,0x00,
  0x00,0x0f,0xf3,0x00,0x00,0x00,0x00,0x0f,0xc3,0x00,0x00,0x00,0x00,0xc3,0xcc,0x00,
  0x00,0x20,0x00,0xc3,0xcc,0x00,0x00,0x20,0x00,0x33,0xc0,0x00,0x00,0x20,0x00,0x33,
  0xc0,0x00,0x00,0x00,0x00,0x03,0xc0,0x00,0x00,0x20,0x00,0x03,0xc0,0x00,0x00,0x70,
  0x00,0x03,0xc2,0xe0,0x00,0xf8,0x00,0x03,0xc3,0xb0,0x1d,0xfd,0xc0,0x03,0xc3,0x1c,
  0x00,0xf8,0x00,0x03,0xc3,0x04,0x00,0x70,0x00,0x03,0xc3,0x06,0x00,0x20,0x00,0x03,
  0xc3,0x02,0x00,0x00,0x00,0x03,0xc3,0x06,0x00,0x00,0x00,0x03,0xc3,0x3c,0x00,0x00,
  0x00,0x03,0xc3,0xc1,0xe1,0xa4,0xe0,0x03,0xc3,0x83,0x33,0xa4,0xb0,0x03,0xc3,0x82,
  0x12,0xa4,0x90,0x03,0xc3,0x82,0x14,0xa4,0x90,0x03,0xc3,0x86,0x74,0xa4,0xb0,0x03,
  0xc3,0x86,0x04,0xa4,0x80,0x03,0xc3,0x86,0x04,0xa4,0x80,0x03,0xc3,0x02,0x04,0xa4,
  0x80,0x03,0xc3,0x03,0x04,0xa4,0x80,0x03,0xc3,0x03,0xf4,0xa4,0xc0,0x03,0xc3,0x00,
  0x77,0xbc,0x78,0x03,0xc0,0x00,0x00,0x80,0x00,0x03,0xc0,0x00,0x00,0x80,0x00,0x03,
  0xc0,0x00,0x00,0x80,0x00,0x03,0xc0,0x00,0x00,0x80,0x00,0x03,0xc0,0x00,0x00,0x80,
  0x00,0x03,0xc0,0x00,0x00,0x80,0x00,0x03,0xc0,0x00,0x00,0x00,0x00,0x03,0xcc,0x00,
  0x00,0x00,0x00,0x33,0xcc,0x00,0x00,0x00,0x00,0x33,0xc3,0x00,0x00,0x00,0x00,0xc3,
  0xf3,0x00,0x00,0x00,0x00,0xcf,0xf0,0x00,0x00,0x00,0x00,0x0f,0xff,0xff,0xff,0xff,
  0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff
};

// 'COQUI'
const unsigned char epd_bitmap_COQUI [] PROGMEM = {
  0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xf0,0x00,0x00,0x00,
  0x00,0x0f,0xf6,0x00,0x00,0x00,0x00,0x6f,0xc6,0x00,0x00,0x00,0x00,0x63,0xd8,0x00,
  0x00,0x00,0x00,0x1b,0xd8,0x00,0x00,0x00,0x00,0x1b,0xc0,0x00,0x00,0x00,0x00,0x03,
  0xc0,0x00,0x00,0x00,0x00,0x03,0xc0,0x00,0x00,0x00,0x00,0x03,0xc0,0x00,0x00,0x00,
  0x00,0x03,0xc0,0x00,0x00,0x00,0x00,0x03,0xc3,0xe1,0x86,0x44,0xc0,0x03,0xc2,0x23,
  0x8f,0x44,0xc0,0x03,0xc6,0x22,0xcd,0x44,0xc0,0x03,0xc4,0x06,0x49,0x44,0x00,0x03,
  0xc4,0x04,0x69,0x44,0x00,0x03,0xc4,0x04,0x29,0x44,0x00,0x03,0xc4,0x04,0x29,0x44,
  0xc0,0x03,0xc4,0x04,0x29,0x44,0xc0,0x03,0xc4,0x04,0x29,0x44,0xc0,0x03,0xc4,0x24,
  0x29,0x44,0xc0,0x03,0xc6,0x24,0x29,0x44,0xc0,0x03,0xc3,0x62,0x2f,0x6c,0xc0,0x03,
  0xc1,0xe3,0xe7,0x38,0xc0,0x03,0xc0,0x00,0x01,0x00,0x00,0x03,0xc0,0x00,0x01,0x00,
  0x00,0x03,0xc0,0x00,0x01,0x00,0x10,0x83,0xc0,0x00,0x01,0x00,0x39,0xc3,0xc0,0x00,
  0x01,0x00,0x7f,0xe3,0xc0,0x00,0x00,0x00,0x7f,0xe3,0xc0,0x00,0x00,0x00,0x7f,0xe3,
  0xc0,0x00,0x00,0x00,0x3f,0xc3,0xc0,0x00,0x00,0x00,0x1f,0x83,0xc0,0x00,0x00,0x00,
  0x0f,0x03,0xc0,0x00,0x00,0x00,0x06,0x03,0xc0,0x00,0x00,0x00,0x06,0x03,0xd8,0x00,
  0x00,0x00,0x00,0x1b,0xd8,0x00,0x00,0x00,0x00,0x1b,0xc6,0x00,0x00,0x00,0x00,0x63,
  0xf6,0x00,0x00,0x00,0x00,0x6f,0xf0,0x00,0x00,0x00,0x00,0x0f,0xff,0xff,0xff,0xff,
  0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff
};

// ─────────────────────────────────────────────────────────────
//  BITMAPS A COLOR (48×44 px, RGB565, PROGMEM)
//  Generados desde sprites LVGL indexados de 4 bits (paleta + índices)
// ─────────────────────────────────────────────────────────────

// 'gallinita'  48x44px, color RGB565 (R/B intercambiados p/ panel BGR), desde LVGL indexed4bit
const uint16_t icon_gallinita[2112] PROGMEM = {
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x4D3F, 0x4D3F, 0x4D3F, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x4D3F, 0x4D3F, 0x4D3F, 0xA0C2, 0xA0C2, 0xA0C2, 0xFF06, 0xFF06, 0xFF06, 0xA0C2, 0xA0C2, 0xA0C2, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x4D3F, 0x4D3F, 0xA0C2, 0xA0C2, 0xA0C2, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xA0C2, 0xA0C2, 0xA0C2, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x4D3F, 0xA0C2, 0xA0C2, 0xA0C2, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xA0C2, 0xA0C2, 0xA0C2, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xA0C2, 0xA0C2, 0xA0C2, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xA0C2, 0xA0C2, 0xA0C2, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xA0C2, 0xA0C2, 0xA0C2, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xA0C2, 0xA0C2, 0xA0C2, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0xFF06, 0xFF06, 0xFF06, 0xA0C2, 0xA0C2, 0xA0C2, 0xFF06, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFF06, 0xA0C2, 0xA0C2, 0xA0C2, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0xFF06, 0xFF06, 0xA0C2, 0xA0C2, 0xA0C2, 0xFF06, 0xFF06, 0x0000, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x0000, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x0000, 0xFF06, 0xFF06, 0xA0C2, 0xA0C2, 0xA0C2, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0xFF06, 0xA0C2, 0xA0C2, 0xA0C2, 0xFF06, 0xFF06, 0xFF06, 0x0000, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x0000, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x0000, 0xFF06, 0xFF06, 0xFF06, 0xA0C2, 0xA0C2, 0xA0C2, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0xA0C2, 0xA0C2, 0xA0C2, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x0000, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x0000, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x0000, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xA0C2, 0xA0C2, 0xA0C2, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0xA0C2, 0xA0C2, 0xA0C2, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xA0C2, 0xA0C2, 0xA0C2, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0xA0C2, 0xA0C2, 0xA0C2, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x0000, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x0000, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x0000, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xA0C2, 0xA0C2, 0xA0C2,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x0000, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x0000, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x0000, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x0000, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x0000, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x0000, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xA0C2, 0xA0C2, 0xA0C2, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0xA0C2, 0xA0C2, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xC618, 0xFFFF, 0xFFFF, 0xFFFF, 0xC618, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x0000, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x0000, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x4D3F, 0x4D3F,
  0x4D3F, 0x0000, 0xC618, 0xC618, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0xFFFF, 0xFF06, 0xFF06, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x0000, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x0000, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x4D3F, 0x4D3F,
  0x4D3F, 0x0000, 0xC618, 0xFFFF, 0xFFFF, 0xC618, 0x0000, 0x0000, 0x0000, 0xC618, 0xFFFF, 0xFFFF, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x0000, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x0000, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x4D3F, 0x4D3F,
  0x4D3F, 0x0000, 0xC618, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xC618, 0xC618, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x0000, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x0000, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x0000, 0xC618, 0xC618, 0xFFFF, 0xFFFF, 0xFFFF, 0xC618, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xC618, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x0000, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x0000, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x4D3F, 0x4D3F,
  0x4D3F, 0x0000, 0xC618, 0xC618, 0xC618, 0x0000, 0x0000, 0xC618, 0xC618, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x0000, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x0000, 0xA0C2, 0x0000, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x4D3F, 0x4D3F,
  0x4D3F, 0x0000, 0xC618, 0xC618, 0xC618, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x0000, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x0000, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x0000, 0xC618, 0xC618, 0xC618, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xC618, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x0000, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x0000, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xC618, 0xC618, 0xC618, 0xC618, 0xC618, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xC618, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x0000, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x0000, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x4D3F, 0x4D3F,
  0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x0000, 0x0000, 0xC618, 0x0000, 0xC618, 0xC618, 0xC618, 0xC618, 0xC618, 0x0000, 0x0000, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47,
  0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x0000, 0x6143, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47,
  0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x0000, 0x6143, 0xFF06, 0xFF06, 0x0000, 0xFF06, 0x0000, 0x9E47, 0x9E47, 0x8412, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47,
  0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x8412, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x8412, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47,
  0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47,
  0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x8412, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x8412, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47,
  0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x8412, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x8412, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47,
  0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47,
  0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47,
  0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47, 0x9E47,
};
// 'solaire'  48x44px, color RGB565 (R/B intercambiados p/ panel BGR), desde LVGL indexed4bit
const uint16_t icon_solaire[2112] PROGMEM = {
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xA0C2, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xA0C2, 0xA0C2, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xA0C2, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0xC618, 0xC618, 0xC618, 0x5ACD, 0xC618, 0xC618, 0xC618, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xC618, 0xC618, 0x5ACD, 0xC618, 0x5ACD, 0xC618, 0xC618, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xC618, 0xC618, 0x5ACD, 0xC618, 0x5ACD, 0xC618, 0xC618, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xC618, 0xC618, 0x5ACD, 0xC618, 0x5ACD, 0xC618, 0xC618, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xC618, 0x0000, 0x0000, 0xC618, 0x0000, 0x0000, 0xC618, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xC618, 0xC618, 0x5ACD, 0xC618, 0x5ACD, 0xC618, 0xC618, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x5ACD, 0x5ACD, 0xC618, 0xC618, 0xC618, 0x5ACD, 0x5ACD, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x0000, 0x0000, 0xC618, 0x0000, 0x0000, 0x5ACD, 0xC618, 0x5ACD, 0x0000, 0x0000, 0xC618, 0x0000, 0x0000, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x3C21, 0x3C21, 0x0000, 0x0000, 0x0000, 0xC618, 0xC618, 0x0000, 0x0000, 0x0000, 0xC618, 0xC618, 0x0000, 0x0000, 0x0000, 0x3C21, 0x3C21, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x0000, 0xA0C2, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xA0C2, 0x0000, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x3C21, 0x3C21, 0x3C21, 0x0000, 0x0000, 0xFFFF, 0xA0C2, 0xA0C2, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xA0C2, 0xA0C2, 0x0000, 0x0000, 0x3C21, 0x3C21, 0x3C21, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x3C21, 0x3C21, 0x3C21, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x3C21, 0x0000, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xA0C2, 0xFF06, 0xFF06, 0xFF06, 0xA0C2, 0xA0C2, 0xA0C2, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xA0C2, 0xA0C2, 0xFFFF, 0x0000, 0x0000, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xA0C2, 0xA0C2, 0xA0C2, 0x0000, 0xC618, 0xC618, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0xA0C2, 0xFFFF, 0xFFFF, 0xFFFF, 0xA0C2, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xA0C2, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0xC618, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xFFFF, 0xFFFF, 0xA0C2, 0xA0C2, 0xFFFF, 0xA0C2, 0xA0C2, 0xFFFF, 0xFFFF, 0xFFFF, 0xA0C2, 0xFFFF, 0x0000, 0xFF06, 0xFF06, 0xA0C2, 0xA0C2, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0xC618, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xFFFF, 0xFFFF, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xFFFF, 0xA0C2, 0xA0C2, 0xFFFF, 0x0000, 0xA0C2, 0xA0C2, 0xFFFF, 0xA0C2, 0xA0C2, 0x0000, 0x0000, 0x0000, 0x0000, 0xC618, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xA0C2, 0xA0C2, 0xFFFF, 0xFFFF, 0xA0C2, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xA0C2, 0xA0C2, 0xA0C2, 0xFFFF, 0xFFFF, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x6143, 0x0000, 0x0000, 0xC618, 0x0000, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xFFFF, 0xA0C2, 0xA0C2, 0xA0C2, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xA0C2, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x6143, 0x6143, 0x6143, 0x6143, 0x0000, 0x0000, 0x0000, 0x0000, 0x6143, 0x6143, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xA0C2, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xA0C2, 0xA0C2, 0xA0C2, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x4D3F, 0x0000, 0x6143, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xA0C2, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xA0C2, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x4D3F, 0x0000, 0x0000, 0xC618, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xFFFF, 0xFFFF, 0xA0C2, 0xA0C2, 0xA0C2, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xA0C2, 0xA0C2, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0xC618, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xA0C2, 0xA0C2, 0xFFFF, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xFFFF, 0xA0C2, 0xA0C2, 0xA0C2, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xC618, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xA0C2, 0xFFFF, 0xFFFF, 0xA0C2, 0xA0C2, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xC618, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0xA0C2, 0xA0C2, 0xFFFF, 0xFFFF, 0xFFFF, 0xA0C2, 0xA0C2, 0xFFFF, 0xFFFF, 0x0000, 0xFFFF, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0xFFFF, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xC618, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xC618, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xC618, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0xC618, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0xC618, 0xC618, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xC618, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xC618, 0xC618, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0xC618, 0xC618, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xC618, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xC618, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xC618, 0xC618, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xC618, 0xC618, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xC618, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xC618, 0xC618, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xC618, 0xC618, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
};
// 'astronauta'  48x44px, color RGB565 (R/B intercambiados p/ panel BGR), desde LVGL indexed4bit
const uint16_t icon_astronauta[2112] PROGMEM = {
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xFFFF, 0x0000, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x00B0, 0x00B0, 0xFFFF, 0xFFFF, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x00B0, 0xFFFF, 0xFFFF, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0xFFFF, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xFFFF, 0x0000, 0x00B0, 0xFFFF, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x0000, 0xFFFF, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xFFFF, 0x0000, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x0000, 0xFFFF, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xFFFF, 0x0000, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x0000, 0xFFFF, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x0000, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xFFFF, 0x0000, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x5ACD, 0x5ACD, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x5ACD, 0x5ACD, 0x8412, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x2126, 0x5ACD, 0x8412, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x8412, 0xFFFF, 0x8412, 0x0000, 0x0000, 0x5ACD, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x8412, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x5ACD, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xA0C2, 0xFFFF, 0x8412, 0xFFFF, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x2126, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xA0C2, 0xA0C2, 0x0000, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xFFFF, 0xFFFF, 0xA0C2, 0x5ACD, 0xFFFF, 0x8412, 0xA0C2, 0x8412, 0x00B0, 0x8412, 0x2126, 0xFFFF, 0xFFFF, 0x8412, 0xA0C2, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0x5ACD, 0xFFFF, 0x8412, 0x00B0, 0x8412, 0xA0C2, 0x8412, 0x2126, 0xFFFF, 0xFFFF, 0x5ACD, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0xFFFF, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x2126, 0xFFFF, 0xFFFF, 0x5ACD, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x8412, 0x8412, 0x2126, 0x0000, 0x8412, 0x2126, 0x2126, 0x2126, 0x2126, 0x2126, 0x8412, 0x8412, 0x8412, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x8412, 0x8412, 0x8412, 0x0000, 0xFFFF, 0x5ACD, 0x8412, 0x8412, 0x8412, 0x8412, 0x5ACD, 0xFFFF, 0xFFFF, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x5ACD, 0x5ACD, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0x8412, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x8412, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x4D3F, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x8412, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x2126, 0x2126, 0xFFFF, 0x8412, 0x0000, 0x4D3F, 0x0000, 0x2126, 0x2126, 0x2126, 0xFFFF, 0x8412, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x8412, 0x8412, 0x5ACD, 0x5ACD, 0x2126, 0x0000, 0x4D3F, 0x0000, 0x8412, 0x8412, 0x8412, 0x5ACD, 0x2126, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x5ACD, 0x5ACD, 0x5ACD, 0x5ACD, 0x2126, 0x0000, 0x4D3F, 0x0000, 0x5ACD, 0x5ACD, 0x5ACD, 0x5ACD, 0x2126, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x4D3F, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0,
  0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0,
  0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0,
  0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0,
  0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0,
  0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0,
  0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0,
  0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0,
};
// 'cactus'  48x44px, color RGB565 (R/B intercambiados p/ panel BGR), desde LVGL indexed4bit
const uint16_t icon_cactus[2112] PROGMEM = {
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x3C21, 0x02C0, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x02C0, 0x3C21, 0x5ACD, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x5ACD, 0x02C0, 0x3C21, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x3C21, 0x02C0, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x4D3F, 0x4D3F, 0x4D3F, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x02C0, 0x3C21, 0x5ACD, 0x3C21, 0x02C0, 0x3C21, 0x4D3F, 0x4D3F, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x5ACD, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x02C0, 0x3C21, 0x02C0, 0x5ACD, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x4D3F, 0x4D3F, 0x4D3F, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x4D3F, 0x4D3F, 0x02C0, 0x3C21, 0x02C0, 0x5ACD, 0x02C0, 0x3C21, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x5ACD, 0x3C21, 0x02C0, 0x3C21, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x5ACD, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x02C0, 0x3C21, 0x02C0, 0x5ACD, 0x02C0, 0x3C21, 0x02C0, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x3C21, 0x02C0, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0,
  0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0,
  0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0,
  0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0,
  0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0,
  0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0,
  0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0, 0xFCA0,
};
// 'caballero'  48x44px, color RGB565 (R/B intercambiados p/ panel BGR), desde LVGL indexed4bit
const uint16_t icon_caballero[2112] PROGMEM = {
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x5ACD, 0xC618, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x5ACD, 0x5ACD, 0x5ACD, 0xC618, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x4D3F, 0x5ACD, 0x5ACD, 0x5ACD, 0xC618, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x5ACD, 0x5ACD, 0x5ACD, 0xC618, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x5ACD, 0x5ACD, 0x5ACD, 0xC618, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0xA0C2, 0xA0C2, 0x5ACD, 0x5ACD, 0x5ACD, 0x5ACD, 0xA0C2, 0xA0C2, 0xA0C2, 0x5ACD, 0x5ACD, 0x5ACD, 0xC618, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0xA0C2, 0x5ACD, 0x5ACD, 0x5ACD, 0x5ACD, 0x5ACD, 0x8412, 0xA0C2, 0xA0C2, 0xC618, 0x5ACD, 0x5ACD, 0xC618, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x5ACD, 0x5ACD, 0x5ACD, 0x5ACD, 0x5ACD, 0x8412, 0x8412, 0x8412, 0xC618, 0x5ACD, 0x5ACD, 0xC618, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x5ACD, 0x5ACD, 0x5ACD, 0x5ACD, 0x5ACD, 0x8412, 0x8412, 0x8412, 0xC618, 0x8412, 0x5ACD, 0xC618, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x5ACD, 0x5ACD, 0x5ACD, 0x5ACD, 0x2126, 0x2126, 0x8412, 0x8412, 0x2126, 0x5ACD, 0x8412, 0xC618, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x5ACD, 0x5ACD, 0x5ACD, 0x5ACD, 0x2126, 0x2126, 0x2126, 0x2126, 0x2126, 0x8412, 0x5ACD, 0xC618, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x5ACD, 0x5ACD, 0x5ACD, 0x5ACD, 0x8412, 0x8412, 0x2126, 0x2126, 0xC618, 0x8412, 0x8412, 0xC618, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x6143, 0x6143, 0x6143, 0x6143, 0x6143, 0x6143, 0x8412, 0x8412, 0x8412, 0x2126, 0x2126, 0xC618, 0x5ACD, 0x8412, 0xC618, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x6143, 0x6143, 0x5ACD, 0x5ACD, 0x6143, 0x6143, 0x8412, 0x8412, 0x8412, 0x2126, 0x2126, 0xC618, 0x5ACD, 0x5ACD, 0xC618, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x6143, 0x5ACD, 0x5ACD, 0x5ACD, 0x5ACD, 0x6143, 0x8412, 0x5ACD, 0x5ACD, 0x5ACD, 0x5ACD, 0x5ACD, 0x5ACD, 0x5ACD, 0xC618, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x6143, 0x5ACD, 0x5ACD, 0x5ACD, 0x5ACD, 0x6143, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0xC618, 0x2126, 0x2126, 0x2126, 0x31A9, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x6143, 0x6143, 0x5ACD, 0x5ACD, 0x6143, 0x6143, 0x8412, 0x5ACD, 0x5ACD, 0x5ACD, 0x5ACD, 0x5ACD, 0x5ACD, 0x8412, 0x2126, 0x31A9, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x6143, 0x6143, 0x5ACD, 0x5ACD, 0x6143, 0x6143, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0xC618, 0x5ACD, 0x8412, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x6143, 0x6143, 0x6143, 0x6143, 0x8412, 0x5ACD, 0x8412, 0x8412, 0x8412, 0x8412, 0xC618, 0x2126, 0x31A9, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x6143, 0x6143, 0x8412, 0x5ACD, 0x5ACD, 0x4D3F, 0x4D3F, 0x5ACD, 0x5ACD, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x5ACD, 0x5ACD, 0x4D3F, 0x4D3F, 0x4D3F, 0x5ACD, 0x5ACD, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x8412, 0x8412, 0x4D3F, 0x4D3F, 0x4D3F, 0x5ACD, 0x5ACD, 0x8412, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
};
// 'corazon'  48x44px, color RGB565 (R/B intercambiados p/ panel BGR), desde LVGL indexed4bit
const uint16_t icon_corazon[2112] PROGMEM = {
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x0000, 0x0000, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xFFFF, 0xFFFF, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x0000, 0x0000, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xFFFF, 0xFFFF, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xFFFF, 0xFFFF, 0xA0C2, 0xA0C2, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xFFFF, 0xFFFF, 0xA0C2, 0xA0C2, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0xA0C2, 0xA0C2, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0xA0C2, 0xA0C2, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
};
// 'pinos'  48x44px, color RGB565 (R/B intercambiados p/ panel BGR), desde LVGL indexed4bit
const uint16_t icon_pinos[2112] PROGMEM = {
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xFFFF, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xFFFF, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xFFFF, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xFFFF, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xFFFF, 0xFFFF, 0xFFFF, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xFFFF, 0xFFFF, 0xFFFF, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xFFFF, 0xFFFF, 0xFFFF, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xFFFF, 0xFFFF, 0xFFFF, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xFFFF, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xFFFF, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0xFFFF, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x02C0, 0x02C0, 0xFFFF, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xFFFF, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0xFFFF, 0xFFFF, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xFFFF, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xFFFF, 0xFFFF, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0xFFFF, 0xFFFF, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xFFFF, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0xFFFF, 0x02C0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xFFFF, 0xFFFF, 0xFFFF, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0xFFFF, 0xFFFF, 0xFFFF, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xFFFF, 0xFFFF, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0xFFFF, 0xFFFF, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xFFFF, 0xFFFF, 0xFFFF, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0xFFFF, 0xFFFF, 0xFFFF, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xFFFF, 0xFFFF, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0xFFFF, 0xFFFF, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xFFFF, 0xFFFF, 0xFFFF, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0xFFFF, 0xFFFF, 0xFFFF, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x02C0, 0xFFFF, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x02C0, 0x02C0, 0x00B0, 0x7AA6, 0x7AA6, 0x7AA6, 0x7AA6, 0x00B0, 0x02C0, 0x02C0, 0x00B0, 0x00B0, 0xFFFF, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x02C0, 0x02C0, 0x00B0, 0x7AA6, 0x7AA6, 0x7AA6, 0x7AA6, 0x00B0, 0x02C0, 0x02C0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x7AA6, 0x7AA6, 0x7AA6, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x7AA6, 0x7AA6, 0x7AA6, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x7AA6, 0x7AA6, 0x7AA6, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x7AA6, 0x7AA6, 0x7AA6, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x7AA6, 0x7AA6, 0x7AA6, 0x7AA6, 0x7AA6, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x7AA6, 0x7AA6, 0x7AA6, 0x7AA6, 0x7AA6, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21,
  0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21,
  0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x3C21, 0x3C21, 0x3C21,
  0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x3C21, 0x3C21,
  0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x3C21, 0x3C21, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x3C21,
  0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
  0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
  0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
  0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
  0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x3C21,
  0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x3C21, 0x3C21,
  0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x3C21, 0x3C21, 0x3C21,
  0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21,
};
// 'luciernaga'  48x44px, color RGB565 (R/B intercambiados p/ panel BGR), desde LVGL indexed4bit
const uint16_t icon_luciernaga[2112] PROGMEM = {
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x5ACD, 0x5ACD, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x5ACD, 0x5ACD, 0x5ACD, 0x5ACD, 0x00B0, 0x00B0, 0x00B0, 0xFF37, 0xFF37, 0x5ACD, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x0000, 0x0000, 0x00B0, 0x0000, 0x0000, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x5ACD, 0xFF37, 0xFF37, 0xFF37, 0x5ACD, 0x5ACD, 0x00B0, 0xFF37, 0xFF37, 0xFF37, 0x5ACD, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x0000, 0x00B0, 0x00B0, 0x0000, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x5ACD, 0xFF37, 0xFF37, 0xFF37, 0xFF37, 0x5ACD, 0x5ACD, 0xFF37, 0xFF37, 0xFF37, 0x5ACD, 0x00B0, 0x00B0, 0x00B0, 0x0000, 0x00B0, 0x00B0, 0x0000, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x5ACD, 0xFF37, 0xFF37, 0xFF37, 0xFF37, 0xFF37, 0x5ACD, 0x5ACD, 0xFF37, 0xFF37, 0x5ACD, 0x0000, 0x7AA6, 0x0000, 0x7AA6, 0x7AA6, 0x0000, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xA0C2, 0xA0C2, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x5ACD, 0x5ACD, 0xFF37, 0xFF37, 0xFF37, 0xFF37, 0xFF37, 0x5ACD, 0x0000, 0x0000, 0x0000, 0x7AA6, 0x7AA6, 0x7AA6, 0x7AA6, 0x7AA6, 0x7AA6, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xA0C2, 0xA0C2, 0xA0C2, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x7AA6, 0x7AA6, 0x7AA6, 0x7AA6, 0x0000, 0x0000, 0x7AA6, 0x0000, 0x0000, 0x7AA6, 0x7AA6, 0x0000, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x7AA6, 0x7AA6, 0x7AA6, 0x7AA6, 0x7AA6, 0x7AA6, 0x7AA6, 0x0000, 0x7AA6, 0x0000, 0x0000, 0x7AA6, 0x7AA6, 0x0000, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xA0C2, 0xA0C2, 0xA0C2, 0x00B0, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x00B0, 0x00B0, 0xA0C2, 0xA0C2, 0xA0C2,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x7AA6, 0x7AA6, 0x7AA6, 0x7AA6, 0x7AA6, 0x7AA6, 0x7AA6, 0x7AA6, 0x0000, 0x7AA6, 0x7AA6, 0x7AA6, 0x7AA6, 0x7AA6, 0x7AA6, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xA0C2, 0xA0C2, 0xA0C2, 0x00B0, 0x00B0, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x7AA6, 0x7AA6, 0x7AA6, 0x7AA6, 0x7AA6, 0x7AA6, 0x7AA6, 0x7AA6, 0x7AA6, 0x0000, 0x7AA6, 0x7AA6, 0x7AA6, 0x7AA6, 0x7AA6, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xFF06, 0xFF06, 0xFF06, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x7AA6, 0x7AA6, 0x7AA6, 0x7AA6, 0x7AA6, 0x7AA6, 0x7AA6, 0x7AA6, 0x7AA6, 0x0000, 0x0000, 0x0000, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xA0C2, 0xA0C2, 0xA0C2, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xA0C2, 0xA0C2, 0xA0C2, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xFF06, 0xFF06, 0x7AA6, 0x7AA6, 0x7AA6, 0x7AA6, 0x7AA6, 0x7AA6, 0x7AA6, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xA0C2, 0xFF06, 0xFF06, 0xFF06, 0xA0C2, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xFF06, 0xFF06, 0xFF06, 0x0000, 0x7AA6, 0x7AA6, 0x7AA6, 0x7AA6, 0x7AA6, 0x0000, 0x0000, 0x0000, 0x0000, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x0000, 0x7AA6, 0x7AA6, 0x0000, 0x0000, 0x0000, 0x0000, 0x00B0, 0x0000, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x02C0, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x0000, 0x7AA6, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x00B0, 0x02C0, 0x00B0, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x0000, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x02C0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xA0C2, 0xA0C2, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xA0C2, 0xA0C2, 0xA0C2, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xA0C2, 0xA0C2, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x02C0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xA0C2, 0xA0C2, 0xA0C2, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x02C0, 0x02C0, 0x02C0, 0x00B0, 0x02C0, 0x00B0, 0x02C0, 0x02C0, 0x02C0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xFF06, 0xFF06, 0xFF06, 0xA0C2, 0xA0C2, 0xA0C2, 0x00B0, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x00B0, 0x00B0, 0xA0C2, 0xA0C2, 0xA0C2, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x02C0, 0x02C0, 0x00B0, 0x00B0, 0x02C0, 0x00B0, 0x00B0, 0x02C0, 0x02C0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xA0C2, 0xA0C2, 0xA0C2, 0x00B0, 0x00B0, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x00B0, 0xA0C2, 0xA0C2, 0xA0C2, 0x00B0, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x00B0, 0x00B0, 0xA0C2, 0xA0C2, 0xA0C2, 0x00B0, 0x00B0, 0x00B0, 0x02C0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xFF06, 0xFF06, 0xFF06, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x00B0, 0xA0C2, 0xA0C2, 0xA0C2, 0x00B0, 0x00B0, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x00B0, 0x00B0, 0x00B0, 0x02C0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xA0C2, 0xA0C2, 0xA0C2, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xA0C2, 0xA0C2, 0xA0C2, 0x00B0, 0x00B0, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xFF06, 0xFF06, 0xFF06, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x00B0, 0x00B0, 0x00B0, 0x02C0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xA0C2, 0xFF06, 0xFF06, 0xFF06, 0xA0C2, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xA0C2, 0xA0C2, 0xA0C2, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xA0C2, 0xA0C2, 0xA0C2, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x02C0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0xA0C2, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xA0C2, 0xFF06, 0xFF06, 0xFF06, 0xA0C2, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x02C0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0xA0C2, 0xA0C2, 0xA0C2, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x02C0, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x02C0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x00B0, 0x02C0, 0x00B0, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x00B0, 0x00B0, 0x00B0, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x02C0, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x00B0, 0x00B0, 0x00B0, 0x02C0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x02C0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x02C0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x00B0, 0x02C0, 0x00B0, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x00B0, 0x00B0, 0x02C0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x00B0, 0x02C0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x02C0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x02C0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x02C0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x00B0, 0x00B0, 0x02C0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x02C0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x02C0, 0x02C0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x02C0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0,
  0x3C21, 0x3C21, 0x02C0, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x02C0, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x02C0, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x02C0, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21,
  0x3C21, 0x3C21, 0x02C0, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x02C0, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0xC618, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x02C0, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x02C0, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21,
  0x3C21, 0x3C21, 0x02C0, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0xC618, 0x02C0, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21,
  0x3C21, 0x3C21, 0x02C0, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0xC618, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x02C0, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21,
  0x3C21, 0x02C0, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0xC618, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21,
  0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0xC618, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21,
  0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21,
  0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0xC618, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0xC618, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21,
  0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21,
  0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21, 0x3C21,
};
// 'bot'  48x44px, color RGB565 (R/B intercambiados p/ panel BGR), desde LVGL indexed4bit
const uint16_t icon_bot[2112] PROGMEM = {
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0xA0C2, 0xA0C2, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x8412, 0x8412, 0x8412, 0xA0C2, 0xA0C2, 0x8412, 0x8412, 0x8412, 0x8412, 0x00B0, 0x00B0, 0x8412, 0x8412, 0x8412, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x8412, 0x8412, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x8412, 0x8412, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x8412, 0x8412, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x8412, 0x8412, 0xA0C2, 0xA0C2, 0xA0C2, 0xA0C2, 0x8412, 0x8412, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x8412, 0x8412, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x8412, 0x8412, 0x8412, 0xA0C2, 0xA0C2, 0x8412, 0x8412, 0x8412, 0x8412, 0x00B0, 0x00B0, 0x8412, 0x8412, 0x8412, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x8412, 0x8412, 0x8412, 0x8412, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x8412, 0x8412, 0x8412, 0x8412, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x8412, 0x8412, 0x8412, 0x8412, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x0000, 0x4D3F, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x8412, 0x8412, 0x8412, 0x0000, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x0000, 0x8412, 0x8412, 0x8412, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x8412, 0x8412, 0x8412, 0x8412, 0x0000, 0x8412, 0x8412, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x8412, 0x8412, 0x0000, 0x8412, 0x8412, 0x8412, 0x8412, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x8412, 0x8412, 0x8412, 0x8412, 0x0000, 0x8412, 0x8412, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x8412, 0x8412, 0x0000, 0x8412, 0x8412, 0x8412, 0x8412, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x8412, 0x8412, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x8412, 0x8412, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x8412, 0x8412, 0x0000, 0x0000, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x0000, 0x0000, 0x8412, 0x8412, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x8412, 0x8412, 0x0000, 0x0000, 0x8412, 0x8412, 0x3C21, 0x8412, 0x3C21, 0x8412, 0x8412, 0x3C21, 0x8412, 0x3C21, 0x8412, 0x8412, 0x0000, 0x0000, 0x8412, 0x8412, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x8412, 0x8412, 0x0000, 0x0000, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x0000, 0x0000, 0x8412, 0x8412, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x8412, 0x8412, 0x0000, 0x4D3F, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x4D3F, 0x0000, 0x8412, 0x8412, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x0000, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x0000, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x8412, 0x8412, 0x0000, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x0000, 0x8412, 0x8412, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x8412, 0x8412, 0x0000, 0x4D3F, 0x0000, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x0000, 0x4D3F, 0x0000, 0x8412, 0x8412, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x8412, 0x8412, 0x0000, 0x4D3F, 0x0000, 0x0000, 0x0000, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x8412, 0x0000, 0x0000, 0x0000, 0x4D3F, 0x0000, 0x8412, 0x8412, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x8412, 0x0000, 0x8412, 0x8412, 0x8412, 0x8412, 0x0000, 0x8412, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x4D3F, 0x4D3F, 0x0000, 0x4D3F, 0x4D3F, 0x0000, 0x8412, 0x8412, 0x0000, 0x0000, 0x0000, 0x0000, 0x8412, 0x8412, 0x0000, 0x4D3F, 0x4D3F, 0x0000, 0x4D3F, 0x4D3F, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x4D3F, 0x4D3F, 0x0000, 0x4D3F, 0x4D3F, 0x0000, 0x8412, 0x8412, 0x0000, 0x4D3F, 0x4D3F, 0x0000, 0x8412, 0x8412, 0x0000, 0x4D3F, 0x4D3F, 0x0000, 0x4D3F, 0x4D3F, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x8412, 0x8412, 0x0000, 0x4D3F, 0x4D3F, 0x0000, 0x8412, 0x8412, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x8412, 0x8412, 0x0000, 0x4D3F, 0x4D3F, 0x0000, 0x8412, 0x8412, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x8412, 0x8412, 0x0000, 0x4D3F, 0x4D3F, 0x0000, 0x8412, 0x8412, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x8412, 0x8412, 0x0000, 0x4D3F, 0x4D3F, 0x0000, 0x8412, 0x8412, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x8412, 0x8412, 0x8412, 0x8412, 0x0000, 0x4D3F, 0x4D3F, 0x0000, 0x8412, 0x8412, 0x8412, 0x8412, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x8412, 0x8412, 0x8412, 0x8412, 0x0000, 0x4D3F, 0x4D3F, 0x0000, 0x8412, 0x8412, 0x8412, 0x8412, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
};
// 'coraline'  48x44px, color RGB565 (R/B intercambiados p/ panel BGR), desde LVGL indexed4bit
const uint16_t icon_coraline[2112] PROGMEM = {
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x0000, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x0000, 0x00B0, 0x00B0, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x0000, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x0000, 0xFF37, 0x0000, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x0000, 0x0000, 0x0000, 0x0000, 0xFF37, 0xFF37, 0xFF37, 0x0000, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x00B0, 0x00B0, 0x00B0, 0x0000, 0xFF37, 0xFF37, 0xFF37, 0xFF37, 0xFF37, 0xFF37, 0xFF37, 0xFF37, 0x0000, 0x00B0, 0x00B0, 0x00B0, 0x00B0, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x00B0, 0x00B0, 0x0000, 0xFF37, 0xFF37, 0xFF37, 0xFF37, 0xFF37, 0xFF37, 0xFF37, 0xFF37, 0xFF37, 0xFF18, 0x0000, 0x00B0, 0x00B0, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xFF37, 0x00B0, 0x00B0, 0x0000, 0xFF37, 0x6143, 0xFF37, 0xFF37, 0xFF37, 0xFF37, 0xFF37, 0xFF37, 0x6143, 0xFF37, 0x0000, 0x00B0, 0x00B0, 0xFF37, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xFF37, 0x00B0, 0x00B0, 0x0000, 0xFF37, 0x6143, 0xFF37, 0xFF37, 0xFF37, 0xFF37, 0xFF37, 0xFF37, 0x6143, 0xFF37, 0x0000, 0x00B0, 0x00B0, 0xFF37, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x00B0, 0x0000, 0xFF37, 0xFF37, 0xFF37, 0xFF37, 0xFF37, 0xFF37, 0xFF37, 0xFF37, 0xFF37, 0xFF37, 0x0000, 0x00B0, 0x00B0, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x00B0, 0x00B0, 0x0000, 0xFF37, 0xFF37, 0xFF37, 0xFF37, 0xFF37, 0xFF37, 0xFF37, 0xFF37, 0x0000, 0x00B0, 0x00B0, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x00B0, 0x00B0, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x00B0, 0x00B0, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x00B0, 0x0000, 0x0000, 0xFF06, 0xFF06, 0xFF37, 0xFF37, 0xFF06, 0xFF06, 0x0000, 0x0000, 0x00B0, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x00B0, 0x0000, 0x0000, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x0000, 0x0000, 0x00B0, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xFF06, 0xFF06, 0x6143, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x6143, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xFF06, 0x0000, 0xFF06, 0xFF06, 0xFF06, 0x6143, 0x6143, 0xFF06, 0x6143, 0xFF06, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xFF06, 0x0000, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x6143, 0x6143, 0xFF06, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xFF37, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF37, 0x0000, 0x6143, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x0000, 0x6143, 0x6143, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x0000, 0x6143, 0x6143, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0xFF06, 0x0000, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x00B0, 0x0000, 0x0000, 0x00B0, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x00B0, 0x0000, 0x0000, 0x00B0, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xFF06, 0x0000, 0x0000, 0xFF06, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0xFF06, 0x0000, 0x0000, 0xFF06, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x0000, 0x0000, 0x0000, 0x0000, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
  0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F, 0x4D3F,
};

// ─────────────────────────────────────────────────────────────
//  ARREGLO DE ICONOS  (12 únicos)
//  epd_bitmap_astro y epd_bitmap_COQUI se conservan sin cambios
//  (mono, 1bpp). El resto ahora son sprites a color RGB565.
// ─────────────────────────────────────────────────────────────
struct Icono {
  const void* data;   // puntero al bitmap (mono 1bpp o RGB565)
  bool        color;  // true = RGB565 (uint16_t*), false = mono (uint8_t*)
};

const Icono iconos[12] = {
  { icon_gallinita,   true  },
  { icon_solaire,     true  },
  { epd_bitmap_COQUI, false },
  { icon_astronauta,  true  },
  { icon_cactus,      true  },
  { icon_caballero,   true  },
  { icon_corazon,     true  },
  { epd_bitmap_astro, false },
  { icon_pinos,       true  },
  { icon_luciernaga,  true  },
  { icon_bot,         true  },
  { icon_coraline,    true  }
};

// Nombres de cada icono, en el mismo orden que el arreglo iconos[]
// (se usan para las notificaciones de "match")
const char* nombresIconos[12] = {
  "Gallinita",
  "Solaire",
  "Coqui",
  "Astronauta",
  "Cactus",
  "Caballero",
  "Corazon",
  "Astro",
  "Pinos",
  "Luciernaga",
  "Bot",
  "Coraline"
};

// ─────────────────────────────────────────────────────────────
//  ESTADO DEL JUEGO
// ─────────────────────────────────────────────────────────────

struct Card {
  uint8_t  tipo;      // índice del icono (0-11)
  bool     revelada;  // carta visible
  bool     emparejada;// ya encontró su par
};

Card     tablero[NUM_CARDS];   // las 24 cartas
int      cursorPos = 0;        // posición del cursor (0-23)
int      primeraSel = -1;      // índice de la primera carta seleccionada (-1 = ninguna)
int      segundaSel = -1;      // índice de la segunda carta seleccionada
int      puntosJ1   = 0;
int      puntosJ2   = 0;
int      turno      = 1;       // 1 = Astro (J1), 2 = Coqui (J2)
bool     juegoTerminado = false;
bool     esperandoSegunda = false; // ya se eligió la primera, esperando la segunda

// ── Estado general (para poder abrir el menú de pausa) ─────────
// STATE_INICIO: pantalla "pulsa A" mostrada al entrar desde el menú
// del arcade (reemplaza la espera bloqueante que tenía el setup() original)
enum GameState { STATE_INICIO, STATE_PLAYING, STATE_PAUSED };
GameState gameState = STATE_INICIO;

// ── Debounce del botón OK ──────────────────────────────────────
unsigned long ultimoPulsoOK = 0;
#define DEBOUNCE_MS 200

// ── Cruceta (D-pad) por escalera de resistencias en un solo pin ADC ──
#define MUESTRAS_ADC     8
#define DEBOUNCE_CRUCETA_MS 40

const int ADC_NINGUNO   = 3687;  // medido en este ESP32
const int ADC_DERECHA   = 2927;  // reescalado proporcional (33k)  // 33k
const int ADC_IZQUIERDA = 1891;  // reescalado proporcional (10k)  // 10k
const int ADC_ABAJO     = 828;   // reescalado proporcional (3.3k)   // 3.3k
const int ADC_ARRIBA    = 284;   // reescalado proporcional (1k)   // 1k

const int UMBRAL_DER_NINGUNO = (ADC_NINGUNO + ADC_DERECHA) / 2;
const int UMBRAL_IZQ_DER     = (ADC_DERECHA + ADC_IZQUIERDA) / 2;
const int UMBRAL_ABAJO_IZQ   = (ADC_IZQUIERDA + ADC_ABAJO) / 2;
const int UMBRAL_ARR_ABAJO   = (ADC_ABAJO + ADC_ARRIBA) / 2;

enum Direccion { DIR_NINGUNO, DIR_ARRIBA, DIR_ABAJO, DIR_IZQUIERDA, DIR_DERECHA };

Direccion direccionActual   = DIR_NINGUNO;
Direccion direccionAnterior = DIR_NINGUNO;
unsigned long ultimoCambioCruceta = 0;

// Repetición automática al mantener pulsada la cruceta.
#define REPETICION_INICIAL_MS  330
#define REPETICION_INTERVALO_MS 105
unsigned long proximaRepeticionCruceta = 0;

// Animación no bloqueante de la pantalla inicial.
bool textoInicioVisible = true;
unsigned long ultimoParpadeoInicio = 0;
#define PARPADEO_INICIO_MS 300

// Notificación de pareja encontrada: permanece visible sin bloquear controles.
bool notificacionMatchActiva = false;
unsigned long notificacionMatchHasta = 0;
#define DURACION_NOTIFICACION_MS 2000

// ─────────────────────────────────────────────────────────────
//  PROTOTIPO DE FUNCIONES
// ─────────────────────────────────────────────────────────────
void inicializarTablero();
void dibujarTablero();
void dibujarCarta(int idx, bool resaltar);
void dibujarIcono(int x, int y, const Icono &ic);
void dibujarHUD();
int  filaInferiorY();
void mostrarNotificacionMatch(int jugador, uint8_t tipoIcono);
void procesarOK();
void moverCursor(int delta_col, int delta_row);
void mostrarAnimacionMatch(int idx1, int idx2);
void mostrarAnimacionFallo(int idx1, int idx2);
void mostrarPantallaInicio();
void mostrarPantallaFin();
void beepOK();
void beepFail();
void beepMatch();
void beepStart();
void buzzerTono(int freq);
void buzzerOff();
void setVolumen(int paso);
void aplicarVolumen();
void drawPauseMenu();
void manejarMenuPausa();
void salirAlMenuInicio();
bool botonOKPresionado();
int  leerADCCrucetaPromediado();
Direccion clasificarCruceta(int valor);
Direccion leerPulsoCruceta();
void actualizarPantallaInicio();
void actualizarNotificacionMatch();
int  cardX(int idx);
int  cardY(int idx);

// ─────────────────────────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────────────────────────
// Llamado por el launcher cada vez que se entra a Memorama desde el
// menú. El hardware compartido (tft, pines, buzzer, volumen) ya lo
// inicializó ArcadeConsola.ino; aquí solo se resetea el juego y se
// muestra la pantalla de inicio, sin bloquear (antes había un
// while() esperando el botón OK, aquí se usa el mismo mecanismo de
// "juegoTerminado" que ya existía para el reinicio tras game-over).
void init() {
  aplicarVolumen();

  randomSeed(esp_random() ^ micros());

  turno = 1; puntosJ1 = 0; puntosJ2 = 0;
  cursorPos = 0; primeraSel = -1; segundaSel = -1; esperandoSegunda = false;
  gameState = STATE_INICIO;
  juegoTerminado = true;   // hace que update() espere el botón OK
  textoInicioVisible = true;
  ultimoParpadeoInicio = millis();
  notificacionMatchActiva = false;
  direccionActual = DIR_NINGUNO;
  direccionAnterior = DIR_NINGUNO;
  proximaRepeticionCruceta = 0;

  // Sincroniza MENU al entrar para que una pulsacion heredada no cuente
  // como una pulsacion nueva en la pantalla inicial.
  pausaPrev = (digitalRead(PIN_PAUSA) == LOW);

  mostrarPantallaInicio();
}

// ─────────────────────────────────────────────────────────────
//  UPDATE (antes "loop"; llamado desde el launcher cada frame)
// ─────────────────────────────────────────────────────────────
void update() {
  // MENU se atiende incluso antes de comenzar el gameplay.
  // Se usa flanco, no nivel, para evitar dobles pulsaciones fantasma.
  bool pausaAhora = (digitalRead(PIN_PAUSA) == LOW);
  bool pausaPressed = (pausaAhora && !pausaPrev);
  pausaPrev = pausaAhora;

  if (gameState == STATE_INICIO && pausaPressed) {
    buzzerOff();
    volverAlMenu();
    return;
  }

  if (juegoTerminado) {
    // La pantalla inicial sigue animándose, pero nunca bloquea la lectura de OK.
    if (gameState == STATE_INICIO) actualizarPantallaInicio();

    // Esperar OK para reiniciar / empezar.
    if (botonOKPresionado()) {
      turno = 1; puntosJ1 = 0; puntosJ2 = 0;
      juegoTerminado = false; cursorPos = 0;
      primeraSel = -1; segundaSel = -1; esperandoSegunda = false;
      notificacionMatchActiva = false;
      gameState = STATE_PLAYING;
      inicializarTablero();
      tft.fillScreen(C_BG);
      dibujarHUD();
      dibujarTablero();
    }
    return;
  }

  // ── Botón de pausa (MENU) ────────────────────────────────
  if (pausaPressed) {
    if (gameState == STATE_PLAYING) {
      gameState = STATE_PAUSED;
      pauseState = PAUSE_MENU;
      pausaSeleccion = PAUSA_SALIR;
      buzzerOff();
      drawPauseMenu();
    } else if (gameState == STATE_PAUSED) {
      if (pauseState == PAUSE_VOLUMEN) {
        // Si estamos en el submenú de volumen, un toque de pausa
        // vuelve al menú principal en vez de reanudar de una
        pauseState = PAUSE_MENU;
        drawPauseMenu();
      } else {
        gameState = STATE_PLAYING;   // reanuda el juego
        // Repintar el tablero, ya que el menú lo tapaba
        tft.fillScreen(C_BG);
        dibujarHUD();
        dibujarTablero();
      }
    }
  }

  // ── MENÚ DE PAUSA ──
  if (gameState == STATE_PAUSED) {
    manejarMenuPausa();
    return;
  }

  // La notificación se borra por tiempo, sin detener el juego.
  actualizarNotificacionMatch();

  // ── Movimiento del cursor con la cruceta (ADC) ────────────
  Direccion dir = leerPulsoCruceta();
  switch (dir) {
    case DIR_IZQUIERDA: moverCursor(-1, 0); break;
    case DIR_DERECHA:   moverCursor( 1, 0); break;
    case DIR_ARRIBA:    moverCursor( 0,-1); break;
    case DIR_ABAJO:     moverCursor( 0, 1); break;
    default: break;
  }

  if (botonOKPresionado()) procesarOK();
}

// ─────────────────────────────────────────────────────────────
//  DEBOUNCE SIMPLE DEL BOTÓN OK (GPIO2)
// ─────────────────────────────────────────────────────────────
bool botonOKPresionado() {
  if (digitalRead(PIN_OK) == LOW) {
    unsigned long ahora = millis();
    if (ahora - ultimoPulsoOK > DEBOUNCE_MS) {
      ultimoPulsoOK = ahora;
      return true;
    }
  }
  return false;
}

// ─────────────────────────────────────────────────────────────
//  LECTURA DE LA CRUCETA (ADC, escalera de resistencias)
// ─────────────────────────────────────────────────────────────
int leerADCCrucetaPromediado() {
  long suma = 0;
  for (int i = 0; i < MUESTRAS_ADC; i++) {
    suma += leerCrucetaADC();
    delayMicroseconds(200);
  }
  return suma / MUESTRAS_ADC;
}

Direccion clasificarCruceta(int valor) {
  if (valor > UMBRAL_DER_NINGUNO) return DIR_NINGUNO;
  if (valor > UMBRAL_IZQ_DER)     return DIR_DERECHA;
  if (valor > UMBRAL_ABAJO_IZQ)   return DIR_IZQUIERDA;
  if (valor > UMBRAL_ARR_ABAJO)   return DIR_ABAJO;
  return DIR_ARRIBA;
}

// Devuelve la dirección solo en el instante del flanco (ya estabilizado
// por debounce). DIR_NINGUNO si no hay pulso nuevo en este ciclo.
Direccion leerPulsoCruceta() {
  int valor = leerADCCrucetaPromediado();
  Direccion leido = clasificarCruceta(valor);
  unsigned long ahora = millis();

  // Registrar cualquier cambio físico y esperar a que se estabilice.
  if (leido != direccionActual) {
    direccionActual = leido;
    ultimoCambioCruceta = ahora;
    return DIR_NINGUNO;
  }

  if ((ahora - ultimoCambioCruceta) <= DEBOUNCE_CRUCETA_MS) {
    return DIR_NINGUNO;
  }

  // Flanco inicial: un movimiento inmediato y luego una pausa corta.
  if (direccionActual != direccionAnterior) {
    direccionAnterior = direccionActual;

    if (direccionActual == DIR_NINGUNO) {
      proximaRepeticionCruceta = 0;
      return DIR_NINGUNO;
    }

    proximaRepeticionCruceta = ahora + REPETICION_INICIAL_MS;
    return direccionActual;
  }

  // Botón mantenido: repetir a ritmo constante.
  if (direccionActual != DIR_NINGUNO &&
      proximaRepeticionCruceta != 0 &&
      (long)(ahora - proximaRepeticionCruceta) >= 0) {
    proximaRepeticionCruceta = ahora + REPETICION_INTERVALO_MS;
    return direccionActual;
  }

  return DIR_NINGUNO;
}

// ─────────────────────────────────────────────────────────────
//  INICIALIZAR TABLERO  (mezcla Fisher-Yates)
// ─────────────────────────────────────────────────────────────
void inicializarTablero() {
  // Re-sembrar en cada partida (no solo al encender) para máxima variedad
  randomSeed(esp_random() ^ micros());

  // Crear 2 copias de cada uno de los 12 tipos
  uint8_t tipos[NUM_CARDS];
  for (int i = 0; i < 12; i++) {
    tipos[i*2]     = i;
    tipos[i*2 + 1] = i;
  }
  // Barajar
  for (int i = NUM_CARDS - 1; i > 0; i--) {
    int j = random(0, i + 1);
    uint8_t tmp = tipos[i]; tipos[i] = tipos[j]; tipos[j] = tmp;
  }
  // Asignar
  for (int i = 0; i < NUM_CARDS; i++) {
    tablero[i].tipo       = tipos[i];
    tablero[i].revelada   = false;
    tablero[i].emparejada = false;
  }
}

// ─────────────────────────────────────────────────────────────
//  POSICIÓN PIXEL DE CADA CARTA
// ─────────────────────────────────────────────────────────────
int cardX(int idx) {
  int col = idx % COLS;
  return BOARD_X + col * (CARD_W + CARD_GAP);
}
int cardY(int idx) {
  int row = idx / COLS;
  return BOARD_Y + row * (CARD_H + CARD_GAP);
}

// ─────────────────────────────────────────────────────────────
//  DIBUJAR TODO EL TABLERO
// ─────────────────────────────────────────────────────────────
void dibujarTablero() {
  for (int i = 0; i < NUM_CARDS; i++) {
    dibujarCarta(i, (i == cursorPos));
  }
}

// ─────────────────────────────────────────────────────────────
//  DIBUJAR UNA CARTA
// ─────────────────────────────────────────────────────────────
void dibujarCarta(int idx, bool resaltar) {
  int x = cardX(idx);
  int y = cardY(idx);

  if (tablero[idx].emparejada) {
    // Fondo verde oscuro para las ya encontradas
    tft.fillRect(x, y, CARD_W, CARD_H, C_MATCH);
    // Dibujar el icono a tamaño completo 48×44 (ocupa toda la carta)
    dibujarIcono(x, y, iconos[tablero[idx].tipo]);
    // Borde: amarillo si el cursor está encima, tenue si no
    uint16_t bordeMatch = resaltar ? C_CURSOR : TFT_DARKGREY;
    tft.drawRect(x, y, CARD_W, CARD_H, bordeMatch);
    if (resaltar) tft.drawRect(x+1, y+1, CARD_W-2, CARD_H-2, bordeMatch);
  } else if (tablero[idx].revelada) {
    // Carta boca arriba: icono a tamaño completo 48×44
    tft.fillRect(x, y, CARD_W, CARD_H, C_CARD_FACE);
    dibujarIcono(x, y, iconos[tablero[idx].tipo]);
    uint16_t borde = resaltar ? C_CURSOR : C_BORDER;
    tft.drawRect(x, y, CARD_W, CARD_H, borde);
    if (resaltar) tft.drawRect(x+1, y+1, CARD_W-2, CARD_H-2, borde);
  } else {
    // Carta boca abajo: patrón de puntos
    tft.fillRect(x, y, CARD_W, CARD_H, C_CARD_BACK);
    // Pequeño patrón decorativo
    for (int py = y+4; py < y+CARD_H-4; py += 4)
      for (int px = x+4; px < x+CARD_W-4; px += 4)
        tft.drawPixel(px, py, TFT_DARKGREY);
    // Borde
    uint16_t borde = resaltar ? C_CURSOR : TFT_DARKGREY;
    tft.drawRect(x, y, CARD_W, CARD_H, borde);
    if (resaltar) tft.drawRect(x+1, y+1, CARD_W-2, CARD_H-2, borde);
  }
}

// ─────────────────────────────────────────────────────────────
//  DIBUJAR ICONO  (48×44 a tamaño real, 1 pixel = 1 pixel)
//  Soporta dos formatos:
//   - Mono 1bpp  (epd_bitmap_astro / epd_bitmap_COQUI, sin cambios)
//     cada fila ocupa ceil(48/8)=6 bytes
//   - Color RGB565 (todos los demás, generados desde sprites LVGL)
//     cada fila ocupa 48 uint16_t (un valor de color por pixel)
// ─────────────────────────────────────────────────────────────
void dibujarIcono(int x, int y, const Icono &ic) {
  if (ic.color) {
    const uint16_t* bmp = (const uint16_t*)ic.data;
    for (int row = 0; row < ICON_H; row++) {
      for (int col = 0; col < ICON_W; col++) {
        uint16_t color = pgm_read_word(&bmp[row * ICON_W + col]);
        tft.drawPixel(x + col, y + row, color);
      }
    }
  } else {
    const unsigned char* bmp = (const unsigned char*)ic.data;
    for (int row = 0; row < ICON_H; row++) {
      for (int col = 0; col < ICON_W; col++) {
        int byteIdx = row * 6 + col / 8;
        int bitIdx  = 7 - (col % 8);
        uint8_t b   = pgm_read_byte(&bmp[byteIdx]);
        bool pixel  = (b >> bitIdx) & 1;
        uint16_t color = pixel ? TFT_WHITE : TFT_BLACK;
        tft.drawPixel(x + col, y + row, color);
      }
    }
  }
}

// ─────────────────────────────────────────────────────────────
//  DIBUJAR HUD  (puntuación y turno) – pantalla 320×240
// ─────────────────────────────────────────────────────────────
void dibujarHUD() {
  tft.fillRect(0, 0, 320, BOARD_Y - 1, C_BG);

  // Astro (izquierda)
  tft.setTextColor(turno == 1 ? C_P1 : TFT_DARKGREY, C_BG);
  tft.setTextSize(1);
  tft.setCursor(4, 8);
  tft.print("Astro:");
  tft.setTextColor(C_P1, C_BG);
  tft.setCursor(46, 8);
  tft.print(puntosJ1);

  // Turno actual (centro)
  tft.setTextColor(C_TEXT, C_BG);
  tft.setCursor(140, 8);
  tft.print("T:");
  tft.setTextColor(turno == 1 ? C_P1 : C_P2, C_BG);
  tft.setCursor(152, 8);
  tft.print(turno);

  // Coqui: desplazado 40 px para reservar el area del medidor de bateria.
  tft.setTextColor(turno == 2 ? C_P2 : TFT_DARKGREY, C_BG);
  tft.setCursor(170, 8);
  tft.print("Coqui:");
  tft.setTextColor(C_P2, C_BG);
  tft.setCursor(212, 8);
  tft.print(puntosJ2);

  // Línea separadora
  tft.drawFastHLine(0, BOARD_Y - 2, 320, TFT_DARKGREY);

  // Pares restantes (debajo del tablero)
  int yAbajo = filaInferiorY();
  if (yAbajo < 236) {
    int paresFaltantes = 0;
    for (int i = 0; i < NUM_CARDS; i++)
      if (!tablero[i].emparejada) paresFaltantes++;
    paresFaltantes /= 2;
    tft.fillRect(0, yAbajo, 320, 12, C_BG);
    tft.setTextColor(TFT_LIGHTGREY, C_BG);
    tft.setCursor(4, yAbajo + 2);
    tft.print("Pares:"); tft.print(paresFaltantes);
  }
}

// Calcula la posición Y de la franja inferior (donde va "Pares:" y las
// notificaciones de match), debajo del tablero.
int filaInferiorY() {
  return BOARD_Y + ROWS * (CARD_H + CARD_GAP) + 2;
}

// ─────────────────────────────────────────────────────────────
//  NOTIFICACIÓN DE MATCH  (franja inferior, lado derecho)
//  Ej: "Astro (J1) ha encontrado Gallinita!"
// ─────────────────────────────────────────────────────────────
void mostrarNotificacionMatch(int jugador, uint8_t tipoIcono) {
  int yAbajo = filaInferiorY();
  if (yAbajo >= 236) return;   // no hay espacio en pantalla para la franja

  uint16_t color = (jugador == 1) ? C_P1 : C_P2;
  const char* nombreJugador = (jugador == 1) ? "Astro" : "Coqui";
  const char* nombreIcono   = nombresIconos[tipoIcono];

  tft.fillRect(0, yAbajo, 320, 12, C_BG);
  tft.setTextColor(color, C_BG);
  tft.setTextSize(1);
  tft.setCursor(4, yAbajo + 2);
  tft.print(nombreJugador);
  tft.print(" ha encontrado ");
  tft.print(nombreIcono);
  tft.print("!");

  notificacionMatchActiva = true;
  notificacionMatchHasta = millis() + DURACION_NOTIFICACION_MS;
}

void actualizarNotificacionMatch() {
  if (!notificacionMatchActiva) return;
  if ((long)(millis() - notificacionMatchHasta) < 0) return;

  notificacionMatchActiva = false;
  // Restaurar únicamente la franja inferior; no se toca el tablero ni el cursor.
  int yAbajo = filaInferiorY();
  if (yAbajo < 236) {
    tft.fillRect(0, yAbajo, 320, 12, C_BG);
    int paresFaltantes = 0;
    for (int i = 0; i < NUM_CARDS; i++)
      if (!tablero[i].emparejada) paresFaltantes++;
    paresFaltantes /= 2;
    tft.setTextColor(TFT_LIGHTGREY, C_BG);
    tft.setTextSize(1);
    tft.setCursor(4, yAbajo + 2);
    tft.print("Pares:");
    tft.print(paresFaltantes);
  }
}

// ─────────────────────────────────────────────────────────────
//  MOVER CURSOR
// ─────────────────────────────────────────────────────────────
void moverCursor(int dc, int dr) {
  int prevPos = cursorPos;
  int col = cursorPos % COLS + dc;
  int row = cursorPos / COLS + dr;
  // Wrapping
  col = (col + COLS) % COLS;
  row = (row + ROWS) % ROWS;
  cursorPos = row * COLS + col;

  dibujarCarta(prevPos, false);
  dibujarCarta(cursorPos, true);
}

// ─────────────────────────────────────────────────────────────
//  PROCESAR BOTÓN OK
// ─────────────────────────────────────────────────────────────
void procesarOK() {
  // No hacer nada si la carta ya está emparejada
  if (tablero[cursorPos].emparejada) return;
  // No revelar la misma carta dos veces
  if (tablero[cursorPos].revelada) return;

  if (primeraSel == -1) {
    // ── Primera selección ────────────────────────────────────
    primeraSel = cursorPos;
    tablero[primeraSel].revelada = true;
    dibujarCarta(primeraSel, (primeraSel == cursorPos));
    beepOK();

  } else {
    // ── Segunda selección ────────────────────────────────────
    segundaSel = cursorPos;
    tablero[segundaSel].revelada = true;
    dibujarCarta(segundaSel, true);
    beepOK();
    delay(50);

    if (tablero[primeraSel].tipo == tablero[segundaSel].tipo) {
      // ── ¡MATCH! ──────────────────────────────────────────
      tablero[primeraSel].emparejada = true;
      tablero[segundaSel].emparejada = true;
      mostrarAnimacionMatch(primeraSel, segundaSel);
      beepMatch();

      if (turno == 1) puntosJ1++;
      else            puntosJ2++;

      // Actualizar marcador primero y después mostrar la notificación.
      // La notificación desaparece sola y no bloquea el cursor.
      dibujarHUD();
      dibujarCarta(primeraSel, false);
      dibujarCarta(segundaSel, false);
      mostrarNotificacionMatch(turno, tablero[primeraSel].tipo);

      // Verificar fin de juego
      bool finJuego = true;
      for (int i = 0; i < NUM_CARDS; i++)
        if (!tablero[i].emparejada) { finJuego = false; break; }

      if (finJuego) {
        juegoTerminado = true;
        delay(500);
        mostrarPantallaFin();
        // Salir ya: no seguir dibujando cartas encima de la pantalla de fin
        primeraSel = -1;
        segundaSel = -1;
        return;
      }
      // Mismo turno: el jugador sigue jugando (no se cambia turno)

    } else {
      // ── FALLO ────────────────────────────────────────────
      mostrarAnimacionFallo(primeraSel, segundaSel);
      beepFail();
      delay(700);

      // Ocultar las dos cartas
      tablero[primeraSel].revelada = false;
      tablero[segundaSel].revelada = false;
      dibujarCarta(primeraSel, false);
      dibujarCarta(segundaSel, false);

      // Cambiar turno
      turno = (turno == 1) ? 2 : 1;
      dibujarHUD();
    }

    // Resetear selecciones
    primeraSel = -1;
    segundaSel = -1;
    // Redibujar cursor en posición actual
    dibujarCarta(cursorPos, true);
  }
}

// ─────────────────────────────────────────────────────────────
//  ANIMACIONES
// ─────────────────────────────────────────────────────────────

// Destello verde sobre las dos cartas emparejadas
void mostrarAnimacionMatch(int idx1, int idx2) {
  for (int i = 0; i < 3; i++) {
    tft.drawRect(cardX(idx1), cardY(idx1), CARD_W, CARD_H, C_MATCH);
    tft.drawRect(cardX(idx2), cardY(idx2), CARD_W, CARD_H, C_MATCH);
    delay(80);
    tft.drawRect(cardX(idx1), cardY(idx1), CARD_W, CARD_H, TFT_WHITE);
    tft.drawRect(cardX(idx2), cardY(idx2), CARD_W, CARD_H, TFT_WHITE);
    delay(80);
  }
}

// Destello rojo sobre las dos cartas que no coinciden
void mostrarAnimacionFallo(int idx1, int idx2) {
  for (int i = 0; i < 3; i++) {
    tft.drawRect(cardX(idx1), cardY(idx1), CARD_W, CARD_H, TFT_RED);
    tft.drawRect(cardX(idx2), cardY(idx2), CARD_W, CARD_H, TFT_RED);
    delay(80);
    tft.drawRect(cardX(idx1), cardY(idx1), CARD_W, CARD_H, TFT_WHITE);
    tft.drawRect(cardX(idx2), cardY(idx2), CARD_W, CARD_H, TFT_WHITE);
    delay(80);
  }
}

// ─────────────────────────────────────────────────────────────
//  PANTALLA DE INICIO  – 320×240
// ─────────────────────────────────────────────────────────────
void mostrarPantallaInicio() {
  tft.fillScreen(C_BG);
  beepStart();

  tft.setTextColor(TFT_YELLOW, C_BG);
  tft.setTextSize(4);
  tft.setCursor(70, 20);
  tft.print("MEMORAMA");

  tft.setTextColor(TFT_CYAN, C_BG);
  tft.setTextSize(1);
  tft.setCursor(110, 55);
  tft.print("para astro<3");

  // Dibujar algunos iconos de muestra (tamaño completo 48×44)
  dibujarIcono(40,  80, iconos[6]);   // corazón
  dibujarIcono(136, 80, iconos[10]);  // bot (robot)
  dibujarIcono(232, 80, iconos[11]);  // coraline


  tft.setTextColor(TFT_GREEN, C_BG);
  tft.setTextSize(1);
  tft.setCursor(80, 200);
  tft.print("Pulsa boton azul para jugar");

  // El parpadeo continúa desde update(), sin delays bloqueantes.
  textoInicioVisible = true;
  ultimoParpadeoInicio = millis();
}

void actualizarPantallaInicio() {
  unsigned long ahora = millis();
  if (ahora - ultimoParpadeoInicio < PARPADEO_INICIO_MS) return;
  ultimoParpadeoInicio = ahora;
  textoInicioVisible = !textoInicioVisible;

  tft.setTextSize(1);
  tft.setCursor(80, 200);
  tft.setTextColor(textoInicioVisible ? TFT_GREEN : C_BG, C_BG);
  tft.print("Pulsa boton azul para jugar");
}

// ─────────────────────────────────────────────────────────────
//  PANTALLA DE FIN DE JUEGO  – 320×240
// ─────────────────────────────────────────────────────────────
void mostrarPantallaFin() {
  tft.fillScreen(C_BG);
  beepMatch(); delay(200); beepMatch(); delay(200); beepMatch();

  tft.setTextColor(TFT_YELLOW, C_BG);
  tft.setTextSize(2);
  tft.setCursor(70, 40);
  tft.print("FIN DE JUEGO");

  tft.setTextColor(C_P1, C_BG);
  tft.setTextSize(1);
  tft.setCursor(70, 90);
  tft.print("Astro: "); tft.print(puntosJ1); tft.print(" pares");

  tft.setTextColor(C_P2, C_BG);
  tft.setCursor(70, 110);
  tft.print("Coqui: "); tft.print(puntosJ2); tft.print(" pares");

  tft.setTextSize(2);
  tft.setCursor(90, 150);
  if (puntosJ1 > puntosJ2) {
    tft.setTextColor(C_P1, C_BG);
    tft.print("GANA ASTRO!!");
  } else if (puntosJ2 > puntosJ1) {
    tft.setTextColor(C_P2, C_BG);
    tft.print("GANA COQUI!!");
  } else {
    tft.setTextColor(TFT_YELLOW, C_BG);
    tft.print("EMPATE!!");
  }

  tft.setTextColor(TFT_WHITE, C_BG);
  tft.setTextSize(1);
  tft.setCursor(80, 210);
  tft.print("boton azul para reiniciar");
}

// ─────────────────────────────────────────────────────────────
//  SONIDOS (buzzer pasivo por LEDC)
// ─────────────────────────────────────────────────────────────

// Enciende el buzzer a una frecuencia dada (onda cuadrada por LEDC)
void buzzerTono(int freq) {
  arcadeBuzzerWriteStable(BUZZER_PIN, freq);
}

// Apaga el buzzer
void buzzerOff() {
  arcadeBuzzerWriteStable(BUZZER_PIN, 0);
}

// Bip corto de confirmación
void beepOK() {
  buzzerTono(1200);
  delay(60);
  buzzerOff();
  delay(10);
}

// Sonido de fallo (dos tonos descendentes)
void beepFail() {
  buzzerTono(400);
  delay(100);
  buzzerOff();
  delay(20);
  buzzerTono(250);
  delay(150);
  buzzerOff();
  delay(10);
}

// Fanfarria de match (tres tonos ascendentes)
void beepMatch() {
  buzzerTono(800);  delay(80);  buzzerOff(); delay(10);
  buzzerTono(1000); delay(80);  buzzerOff(); delay(10);
  buzzerTono(1400); delay(120); buzzerOff(); delay(10);
}

// Melodía de inicio
void beepStart() {
  int notas[] = {523, 659, 784, 1047};
  int dur[]   = {100, 100, 100, 200};
  for (int i = 0; i < 4; i++) {
    buzzerTono(notas[i]);
    delay(dur[i]);
    buzzerOff();
    delay(20);
  }
}

// ─────────────────────────────────────────────────────────────
//  VOLUMEN (chopper por LEDC + filtro RC)
// ─────────────────────────────────────────────────────────────

// Fija el volumen (0-3) y lo aplica de inmediato al buzzer
void setVolumen(int paso) {
  if (paso < 0) paso = 0;
  if (paso > VOLUME_PASOS_MAX) paso = VOLUME_PASOS_MAX;
  volumenPaso = paso;
  aplicarVolumen();
}

// Actualiza el estado global. El duty se aplica al comenzar cada nota.
void aplicarVolumen() {
  sonidoActivo = (volumenPaso > 0);
  cocodriloAplicarVolumenMAX();
  if (!sonidoActivo) arcadeBuzzerWriteStable(7, 0);
}

// ─────────────────────────────────────────────────────────────
//  MENÚ DE PAUSA
// ─────────────────────────────────────────────────────────────

// Dibuja el cuadro de pausa (menú principal o submenú de volumen)
void drawPauseMenu() {
  const int x = 60, y = 50, w = 200, h = 140;

  tft.fillRect(x, y, w, h, C_BG);
  tft.drawRect(x, y, w, h, C_BORDER);
  tft.drawRect(x + 1, y + 1, w - 2, h - 2, TFT_DARKGREY);

  tft.setTextColor(TFT_YELLOW, C_BG);
  tft.setTextSize(2);
  tft.setCursor(x + 46, y + 12);
  tft.print("MENU");
  tft.setTextSize(1);

  if (pauseState == PAUSE_MENU) {
    tft.setTextColor(pausaSeleccion == PAUSA_SALIR ? TFT_YELLOW : TFT_WHITE, C_BG);
    tft.setCursor(x + 20, y + 50);
    tft.print(pausaSeleccion == PAUSA_SALIR ? "> Salir del juego" : "  Salir del juego");

    tft.setTextColor(pausaSeleccion == PAUSA_VOLUMEN_OPT ? TFT_YELLOW : TFT_WHITE, C_BG);
    tft.setCursor(x + 20, y + 70);
    tft.print(pausaSeleccion == PAUSA_VOLUMEN_OPT ? "> Sonido" : "  Sonido");

    tft.setTextColor(TFT_LIGHTGREY, C_BG);
    tft.setCursor(x + 14, y + 110);
    tft.print("Cruceta: elegir");
    tft.setCursor(x + 22, y + 124);
    tft.print("boton azul: confirmar");

  } else {   // PAUSE_VOLUMEN
    tft.setTextColor(TFT_CYAN, C_BG);
    tft.setCursor(x + 62, y + 46);
    tft.print("Sonido");

    int barX = x + 20, barY = y + 68, barW = w - 40, barH = 10;
    tft.drawRect(barX, barY, barW, barH, TFT_WHITE);
    int relleno = (barW - 2) * volumenPaso / VOLUME_PASOS_MAX;
    tft.fillRect(barX + 1, barY + 1, relleno, barH - 2, TFT_GREEN);

    tft.setTextColor(TFT_WHITE, C_BG);
    tft.setCursor(x + 86, y + 86);
    tft.print(arcadeNombreVolumen());

    tft.setTextColor(TFT_LIGHTGREY, C_BG);
    tft.setCursor(x + 8, y + 112);
    tft.print("Izq/Der: ajustar");
    tft.setCursor(x + 20, y + 126);
    tft.print("boton azul: escuchar");
  }

  dibujarTemperaturaPausaTFT();
}

// Atiende cruceta y botón OK mientras el menú de pausa está abierto
void manejarMenuPausa() {
  Direccion dir = leerPulsoCruceta();

  if (pauseState == PAUSE_MENU) {
    if (dir == DIR_ARRIBA || dir == DIR_ABAJO) {
      pausaSeleccion = (pausaSeleccion == PAUSA_SALIR) ? PAUSA_VOLUMEN_OPT : PAUSA_SALIR;
      drawPauseMenu();
    }
  } else {   // PAUSE_VOLUMEN
    if (dir == DIR_IZQUIERDA) {
      setVolumen(volumenPaso - 1);
      drawPauseMenu();
    } else if (dir == DIR_DERECHA) {
      setVolumen(volumenPaso + 1);
      drawPauseMenu();
    }
  }

  // Botón OK dentro del menú (flanco propio, sin el debounce por
  // tiempo que usa el botón OK durante la partida)
  bool okAhora = (digitalRead(PIN_OK) == LOW);
  bool okPressed = (okAhora && !botonOkPausaPrev);
  botonOkPausaPrev = okAhora;

  if (okPressed) {
    if (pauseState == PAUSE_MENU) {
      if (pausaSeleccion == PAUSA_VOLUMEN_OPT) {
        pauseState = PAUSE_VOLUMEN;
        drawPauseMenu();
      } else {
        salirAlMenuInicio();
      }
    } else {
      // En el submenú de volumen, OK reproduce un bip de prueba
      // para escuchar el nivel elegido
      beepOK();
    }
  }
}

// Sale de la partida actual y vuelve al menú principal del arcade
void salirAlMenuInicio() {
  volverAlMenu();
}

} // namespace Memorama

// ============================================================
//  ZONA VERDE  —  ESP32-S3 ZERO + ST7789 320x240  —  TFT_eSPI
//  Control: cruceta (D-pad de 1 solo pin ADC) + botón OK + botón pausa
//
//  PINES:
//    GPIO1  = Cruceta (escalera de resistencias, entrada ADC)
//    GPIO2  = Botón OK / acción / iniciar
//    MENU  = Botón de pausa
//    GPIO7  = Buzzer pasivo (tono, LEDC)
//    GPIO13 = Chopper de volumen (PWM 30kHz + filtro RC hacia el buzzer)
//    GPIO8/9/10/11/12 = TFT (RST/DC/CS/MOSI/SCLK)
//
//  IMPORTANTE: TFT_eSPI no recibe los pines aquí. Edita
//  User_Setup.h de la librería (Documentos/Arduino/libraries/TFT_eSPI/User_Setup.h):
//
//    #define ST7789_DRIVER
//    #define TFT_WIDTH  240
//    #define TFT_HEIGHT 320
//    #define TFT_MOSI 11
//    #define TFT_SCLK 12
//    #define TFT_CS   10
//    #define TFT_DC    9
//    #define TFT_RST   8
//    #define LOAD_GLCD
//    #define SPI_FREQUENCY 27000000
//
//  GPIO1 está reservado para la cruceta: no lo uses para la pantalla.
// ============================================================
// NOTA: TFT_eSPI y SPI ya se incluyen una sola vez en ArcadeConsola.ino;
// tft y canvas son los objetos COMPARTIDOS del launcher.

namespace ZonaVerde {

// ── Pines de control ──────────────────────────────────────────
#define BTN_OK      2   // OK / START / acción en juego
#define PIN_PAUSA   5   // Botón de pausa

// ── Cruceta (D-pad de 1 pin ADC, escalera de resistencias) ───
#define PIN_CRUCETA        1
#define MUESTRAS_ADC       8
#define CRUCETA_DEBOUNCE_MS 40

const int ADC_NINGUNO   = 3687;  // medido en este ESP32
const int ADC_DERECHA   = 2927;  // reescalado proporcional (33k)  // 33k
const int ADC_IZQUIERDA = 1891;  // reescalado proporcional (10k)  // 10k
const int ADC_ABAJO     =  920;  // 3.3k
const int ADC_ARRIBA    =  315;  // 1k

const int UMBRAL_DER_NINGUNO = (ADC_NINGUNO + ADC_DERECHA) / 2;
const int UMBRAL_IZQ_DER     = (ADC_DERECHA + ADC_IZQUIERDA) / 2;
const int UMBRAL_ABAJO_IZQ   = (ADC_IZQUIERDA + ADC_ABAJO) / 2;
const int UMBRAL_ARR_ABAJO   = (ADC_ABAJO + ADC_ARRIBA) / 2;

enum Boton : uint8_t { NINGUNO, ARRIBA, ABAJO, IZQUIERDA, DERECHA };

static Boton    crucetaActual       = NINGUNO;
static Boton    crucetaAnterior     = NINGUNO;
static uint32_t crucetaUltimoCambio = 0;

// ── Buzzer (pasivo, usa LEDC del ESP32) ──────────────────────
#define BUZZER_PIN     7
#define BUZZER_CH      0   // canal LEDC (0-15)
#define BUZZER_RES     8   // resolución de duty (bits)

// ── Volumen: transistor "chopper" que regula el voltaje que
//    llega al buzzer, vía PWM de alta frecuencia + filtro RC ────
// PIN_VOLUME_CHOPPER, VOLUME_PASOS_MAX y volumenPaso ahora son
// globales y compartidos (ver bloque "SONIDO" cerca del inicio
// del archivo) para que el volumen sea el mismo en todo el arcade.

// ── Pantalla ────────────────────────────────────────────────
#define SW 320
#define SH 240

// ── Colores frecuentes como constantes (no repite literales) ─
static const uint16_t C_RED    = TFT_RED;
static const uint16_t C_GREEN  = TFT_GREEN;
static const uint16_t C_WHITE  = TFT_WHITE;
static const uint16_t C_BLACK  = TFT_BLACK;
static const uint16_t C_YELLOW = TFT_YELLOW;
static const uint16_t C_DKRED  = 0x7800;

// ── Estados ─────────────────────────────────────────────────
enum State : uint8_t { ST_MENU, ST_PLAY, ST_PAUSE, ST_OVER };
static State state = ST_MENU;

// ── Pausa ───────────────────────────────────────────────────
static bool pausaPrev          = false;
static bool botonOkPausaPrev   = false;

enum PauseState  { PAUSE_MENU, PAUSE_VOLUMEN };
static PauseState pauseState = PAUSE_MENU;

enum PauseOption { PAUSA_SALIR, PAUSA_VOLUMEN_OPT };
static PauseOption pausaSeleccion = PAUSA_SALIR;

// ── Variables de juego (int16 donde alcanza → ahorra RAM) ──
static int16_t  score    = 0;
static int8_t   hitValue = 1;
static int16_t  timeLeft = 20;
static int16_t  chosenTime = 20;   // elegido en menú

static float    angleDot  = 0.0f;
static float    speedDot  = 14.0f;  // Rápido desde el inicio

static int16_t  targetStart = 0;
static int16_t  targetSize  = 45;

// ── Menú: opción seleccionada (0=20s, 1=30s, 2=40s) ────────
static uint8_t menuSel = 0;

// ── Timings ─────────────────────────────────────────────────
static uint32_t lastFrame  = 0;
static uint32_t lastSecond = 0;
static uint32_t lastButton = 0;
static bool     lastBtn    = HIGH;

// ── Menú: pulsación corta = cicla opciones, larga (>800ms) = inicia ──
static uint32_t btnDownAt  = 0;
static bool     btnHeld    = false;

// ════════════════════════════════════════════════════════════
//  CRUCETA — lectura digital con debounce y detección de flanco
// ════════════════════════════════════════════════════════════
static int leerADCCrucetaPromediado() {
  long suma = 0;
  for (int i = 0; i < MUESTRAS_ADC; i++) {
    suma += leerCrucetaADC();
    delayMicroseconds(200);
  }
  return suma / MUESTRAS_ADC;
}

static Boton clasificarCruceta(int valor) {
  if (valor > UMBRAL_DER_NINGUNO) return NINGUNO;
  if (valor > UMBRAL_IZQ_DER)     return DERECHA;
  if (valor > UMBRAL_ABAJO_IZQ)   return IZQUIERDA;
  if (valor > UMBRAL_ARR_ABAJO)   return ABAJO;
  return ARRIBA;
}

// Devuelve true UNA sola vez cuando aparece una nueva dirección estable
// (hay que soltar y volver a mover la cruceta para disparar de nuevo)
static bool crucetaPressed(Boton &out) {
  int valor = leerADCCrucetaPromediado();
  Boton leido = clasificarCruceta(valor);
  if (leido != crucetaActual) {
    crucetaUltimoCambio = millis();
    crucetaActual = leido;
  }
  if ((millis() - crucetaUltimoCambio) > CRUCETA_DEBOUNCE_MS && crucetaActual != crucetaAnterior) {
    crucetaAnterior = crucetaActual;
    if (crucetaActual != NINGUNO) {
      out = crucetaActual;
      return true;
    }
  }
  return false;
}

// ════════════════════════════════════════════════════════════
//  BUZZER + VOLUMEN  (compatible v2 y v3 de arduino-esp32)
// ════════════════════════════════════════════════════════════
static void aplicarVolumen() {
  sonidoActivo = (volumenPaso > 0);
  cocodriloAplicarVolumenMAX();
  if (!sonidoActivo) arcadeBuzzerWriteStable(7, 0);
}

static void buzzerInit() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
arcadeBuzzerWriteStable(BUZZER_PIN, 0);
#else
#endif
  aplicarVolumen();
}

static void buzzerTone(uint16_t freq, uint16_t ms) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  arcadeBuzzerWriteStable(BUZZER_PIN, freq);
  delay(ms);
  arcadeBuzzerWriteStable(BUZZER_PIN, 0);
#else
  arcadeBuzzerWriteStable(BUZZER_CH, freq);
  ledcWrite(BUZZER_CH, arcadeVolumenDuty());
  delay(ms);
#endif
}

static void buzzerOff() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  arcadeBuzzerWriteStable(BUZZER_PIN, 0);
#else
#endif
}

static inline void beepGood()     { buzzerTone(1200, 55); }
static inline void beepBad()      { buzzerTone(260,  110); }
static inline void beepGameOver() { buzzerTone(200,  240); }
static inline void beepSelect()   { buzzerTone(900,  40);  }

// ════════════════════════════════════════════════════════════
//  LÓGICA
// ════════════════════════════════════════════════════════════
static bool angleInTarget(int16_t a) {
  int16_t end = (targetStart + targetSize) % 360;
  if (targetStart < end) return a >= targetStart && a <= end;
  return a >= targetStart || a <= end;
}

static void newTarget() {
  targetStart = (int16_t)random(0, 360);
  uint8_t maxSz = 68;
  if (speedDot > 16.0f) maxSz = 44;
  else if (speedDot > 13.0f) maxSz = 55;
  targetSize = (int16_t)random(22, maxSz);
}

static void startGame() {
  score     = 0;
  hitValue  = 1;
  timeLeft  = chosenTime;
  angleDot  = 0.0f;
  speedDot  = 14.0f;          // rápido desde el inicio
  state     = ST_PLAY;
  lastSecond = millis();
  newTarget();
}

static void checkHit() {
  if (angleInTarget((int16_t)angleDot)) {
    score += hitValue;
    if (hitValue < 5) hitValue++;
    speedDot += 0.4f;
    if (speedDot > 20.0f) speedDot = 20.0f;
    beepGood();
  } else {
    hitValue = 1;
    if (score > 0) score--;
    beepBad();
  }
  newTarget();
}

// ════════════════════════════════════════════════════════════
//  DIBUJO — MENÚ
// ════════════════════════════════════════════════════════════
static const char TXT_TITLE[]  PROGMEM = "ZONA ASTRO";
static const char TXT_CHOOSE[] PROGMEM = "Elige el tiempo:";
static const char TXT_START[]  PROGMEM = "Pulsa para comenzar";
static const char TXT_OVER[]   PROGMEM = "GAME OVER";
static const char TXT_SCORE[]  PROGMEM = "Score:";
static const char TXT_RETRY[]  PROGMEM = "Pulsa para reiniciar";

void drawMenu() {
  canvas.fillSprite(C_BLACK);
  canvas.setTextDatum(MC_DATUM);

  canvas.setTextColor(C_GREEN, C_BLACK);
  canvas.drawString(F("ZONA ASTRO"), SW/2, 38, 4);

  canvas.setTextColor(C_WHITE, C_BLACK);
  canvas.drawString(F("Elige el tiempo:"), SW/2, 78, 2);

  static const uint8_t times[3] = {30, 40, 60};
  static const uint8_t bx[3] = {70, 160, 250};

  char buf[6];
  for (uint8_t i = 0; i < 3; i++) {
    bool sel = (i == menuSel);
    uint16_t boxCol = sel ? C_GREEN  : 0x2945;
    uint16_t txtCol = sel ? C_BLACK  : C_WHITE;

    canvas.fillRoundRect(bx[i]-34, 104, 68, 48, 10, boxCol);
    canvas.drawRoundRect(bx[i]-34, 104, 68, 48, 10, sel ? C_WHITE : 0x4228);

    canvas.setTextColor(txtCol, boxCol);
    itoa(times[i], buf, 10);
    canvas.drawString(buf, bx[i], 120, 4);
    canvas.setTextColor(txtCol, boxCol);
    canvas.drawString("s", bx[i]+22, 122, 4);
  }

  canvas.setTextColor(C_WHITE, C_BLACK);
  canvas.drawString(F("<"), 30, 128, 4);
  canvas.drawString(F(">"), 290, 128, 4);

  
  canvas.setTextColor(C_YELLOW, C_BLACK);
  canvas.drawString(F("boton azul 1s para iniciar"), SW/2, 170, 2);

  perfilPushSprite(0, 0);
}

// ════════════════════════════════════════════════════════════
//  DIBUJO — JUEGO
// ════════════════════════════════════════════════════════════
static void drawHUD() {
  // Fijar el datum explícitamente. Zona Astro heredaba el datum de otras
  // pantallas y el contador terminaba dibujándose hacia abajo, fuera del cuadro.
  canvas.setTextDatum(MC_DATUM);

  canvas.fillRoundRect(120, 6, 80, 34, 8, C_DKRED);
  canvas.drawRoundRect(120, 6, 80, 34, 8, C_WHITE);
  canvas.setTextColor(C_WHITE, C_DKRED);
  canvas.drawString(String(timeLeft), 160, 22, 4);

  canvas.setTextColor(C_WHITE, C_BLACK);
  canvas.drawString("Score", 58, 215, 2);
  canvas.setTextColor(C_YELLOW, C_BLACK);
  canvas.drawString(String(score), 118, 215, 4);

  canvas.setTextColor(C_WHITE, C_BLACK);
  canvas.drawString("x", 213, 215, 2);
  canvas.setTextColor(C_GREEN, C_BLACK);
  canvas.drawString(String(hitValue), 238, 215, 4);

  canvas.setTextDatum(TL_DATUM);
}

static void drawRing() {
  const int16_t cx = 160, cy = 116;
  const int16_t OR = 62, IR = 35;

  canvas.fillCircle(cx, cy, OR, C_RED);
  canvas.fillCircle(cx, cy, IR, C_BLACK);
  canvas.drawCircle(cx, cy, OR, C_WHITE);
  canvas.drawCircle(cx, cy, IR, C_WHITE);

  int16_t end = targetStart + targetSize;
  for (int16_t a = targetStart; a < end; a += 2) {
    float rad = radians(a % 360);
    float c = cosf(rad), s = sinf(rad);
    canvas.drawLine(
      cx + (int16_t)(c * IR), cy + (int16_t)(s * IR),
      cx + (int16_t)(c * OR), cy + (int16_t)(s * OR),
      C_GREEN
    );
  }

  const float r = (OR + IR) * 0.5f;
  float rad = radians(angleDot);
  int16_t px = cx + (int16_t)(cosf(rad) * r);
  int16_t py = cy + (int16_t)(sinf(rad) * r);
  canvas.fillCircle(px, py, 9, C_WHITE);
  canvas.drawCircle(px, py, 9, C_BLACK);
}

static void drawGame() {
  canvas.fillSprite(C_BLACK);
  drawHUD();
  drawRing();
  perfilPushSprite(0, 0);
}

static void drawGameOver() {
  canvas.fillSprite(C_BLACK);
  canvas.setTextDatum(MC_DATUM);
  canvas.setTextColor(C_RED, C_BLACK);
  canvas.drawString(F("GAME OVER"), SW/2, 75, 4);
  canvas.setTextColor(C_YELLOW, C_BLACK);
  canvas.drawString("Score: " + String(score), SW/2, 120, 4);
  canvas.setTextColor(C_WHITE, C_BLACK);
  canvas.drawString(F("Pulsa para reiniciar"), SW/2, 172, 2);
  perfilPushSprite(0, 0);
}

// ════════════════════════════════════════════════════════════
//  DIBUJO — MENÚ DE PAUSA
// ════════════════════════════════════════════════════════════
static void drawPauseMenu() {
  canvas.fillSprite(C_BLACK);
  canvas.setTextDatum(MC_DATUM);
  canvas.setTextColor(C_YELLOW, C_BLACK);
  canvas.drawString(F("MENU"), SW/2, 36, 4);

  if (pauseState == PAUSE_MENU) {
    const char* opts[2] = {"SALIR", "SONIDO"};
    for (uint8_t i = 0; i < 2; i++) {
      bool sel = (i == pausaSeleccion);
      uint16_t boxCol = sel ? C_GREEN : 0x2945;
      uint16_t txtCol = sel ? C_BLACK : C_WHITE;
      int16_t y = 84 + i * 56;

      canvas.fillRoundRect(90, y, 140, 42, 10, boxCol);
      canvas.drawRoundRect(90, y, 140, 42, 10, sel ? C_WHITE : 0x4228);
      canvas.setTextColor(txtCol, boxCol);
      canvas.drawString(opts[i], SW/2, y + 21, 2);
    }
    canvas.setTextColor(0xC618, C_BLACK);
    canvas.drawString(F("Arriba/Abajo: elegir"), SW/2, 205, 2);
    canvas.drawString(F("boton azul: confirmar"), SW/2, 205, 2);
  } else { // PAUSE_VOLUMEN
    canvas.setTextColor(C_WHITE, C_BLACK);
    canvas.drawString(F("Sonido"), SW/2, 84, 4);

    int16_t barX = 60, barY = 122, barW = 200, barH = 22;
    canvas.drawRoundRect(barX, barY, barW, barH, 6, C_WHITE);
    int16_t fillW = (barW - 4) * volumenPaso / VOLUME_PASOS_MAX;
    if (fillW > 0) canvas.fillRoundRect(barX + 2, barY + 2, fillW, barH - 4, 4, C_GREEN);

    canvas.setTextColor(C_YELLOW, C_BLACK);
    canvas.drawString(String(arcadeNombreVolumen()), SW/2, 168, 4);

    canvas.setTextColor(0xC618, C_BLACK);
    canvas.drawString(F("Izq/Der: ON/OFF"), SW/2, 205, 2);
    canvas.drawString(F("MENU: volver"), SW/2, 222, 2);
  }

  dibujarTemperaturaPausaCanvas();
  perfilPushSprite(0, 0);
}

static void manejarMenuPausa() {
  Boton dir;
  bool crucetaEvento = crucetaPressed(dir);

  bool okAhora   = (digitalRead(BTN_OK) == LOW);
  bool okPressed = (okAhora && !botonOkPausaPrev);
  botonOkPausaPrev = okAhora;

  if (pauseState == PAUSE_MENU) {
    if (crucetaEvento && (dir == ARRIBA || dir == ABAJO)) {
      pausaSeleccion = (pausaSeleccion == PAUSA_SALIR) ? PAUSA_VOLUMEN_OPT : PAUSA_SALIR;
      beepSelect();
      drawPauseMenu();
    }
    if (okPressed) {
      beepSelect();
      if (pausaSeleccion == PAUSA_SALIR) {
        state   = ST_MENU;   // listo por si se vuelve a elegir el juego
        menuSel = 0;
        volverAlMenu();
        return;
      } else {
        pauseState = PAUSE_VOLUMEN;
        drawPauseMenu();
      }
    }
  } else { // PAUSE_VOLUMEN
    if (crucetaEvento && dir == DERECHA) {
      if (volumenPaso < VOLUME_PASOS_MAX) volumenPaso++;
      aplicarVolumen();
      beepSelect();
      drawPauseMenu();
    }
    if (crucetaEvento && dir == IZQUIERDA) {
      if (volumenPaso > 0) volumenPaso--;
      aplicarVolumen();
      beepSelect();
      drawPauseMenu();
    }
  }
}

// ════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════
// Llamado por el launcher cada vez que se entra a Zona Verde desde el
// menú del arcade. El hardware compartido ya lo inicializó
// ArcadeConsola.ino una sola vez.
void init() {
  // Restablece la tipografia al entrar por primera vez desde el launcher.
  canvas.setTextSize(1);
  canvas.setTextFont(1);
  canvas.setTextDatum(TL_DATUM);
  aplicarVolumen();
  randomSeed(esp_random());

  state   = ST_MENU;
  menuSel = 0;

  // Sincroniza MENU al entrar: no heredar un LOW anterior.
  pausaPrev = (digitalRead(PIN_PAUSA) == LOW);

  drawMenu();
}

// ════════════════════════════════════════════════════════════
//  UPDATE (antes "loop"; llamado desde el launcher cada frame)
// ════════════════════════════════════════════════════════════
void update() {
  uint32_t now = millis();

  // ── BOTÓN DE PAUSA (MENU) ──
  bool pausaAhora   = (digitalRead(PIN_PAUSA) == LOW);
  bool pausaPressed = (pausaAhora && !pausaPrev);
  pausaPrev = pausaAhora;

  if (pausaPressed) {
    if (state == ST_MENU) {
      buzzerOff();
      volverAlMenu();
      return;
    }

    if (state == ST_PLAY) {
      state = ST_PAUSE;
      pauseState = PAUSE_MENU;
      pausaSeleccion = PAUSA_SALIR;
      buzzerOff();
      drawPauseMenu();
    } else if (state == ST_PAUSE) {
      if (pauseState == PAUSE_VOLUMEN) {
        // Si estamos en el submenú de volumen, un toque de pausa
        // vuelve al menú principal en vez de reanudar de una
        pauseState = PAUSE_MENU;
        drawPauseMenu();
      } else {
        state = ST_PLAY;   // reanuda el juego
        lastSecond = now;  // no descuenta de golpe el tiempo en pausa
      }
    }
  }

  // ── MENÚ DE PAUSA ──
  if (state == ST_PAUSE) {
    manejarMenuPausa();
    return;
  }

  // ── MENÚ ────────────────────────────────────────────────
  if (state == ST_MENU) {
    Boton dir;
    if (crucetaPressed(dir)) {
      if (dir == IZQUIERDA) {
        menuSel = (menuSel + 2) % 3;  // retrocede: 0←2←1←0
        beepSelect();
        drawMenu();
      } else if (dir == DERECHA) {
        menuSel = (menuSel + 1) % 3;
        beepSelect();
        drawMenu();
      }
    }

    // OK/START: mantener para iniciar
    bool cur = digitalRead(BTN_OK);
    if (lastBtn == HIGH && cur == LOW) {
      btnDownAt = now;
      btnHeld   = false;
    }
    if (cur == LOW && !btnHeld && (now - btnDownAt) > 800) {
      btnHeld = true;
      beepSelect();
      static const uint8_t times[3] = {30, 40, 60};
      chosenTime = times[menuSel];
      lastBtn = cur;
      startGame();
      return;
    }
    lastBtn = cur;
    return;
  }

  // ── GAME OVER ────────────────────────────────────────────
  if (state == ST_OVER) {
    bool cur = digitalRead(BTN_OK);
    if (lastBtn == HIGH && cur == LOW && (now - lastButton) > 250) {
      lastButton = now;
      state = ST_MENU;
      menuSel = 0;
      drawMenu();
    }
    lastBtn = cur;
    return;
  }

  // ── JUGANDO ─────────────────────────────────────────────
  if (now - lastSecond >= 1000) {
    lastSecond = now;
    if (--timeLeft <= 0) {
      state = ST_OVER;
      beepGameOver();
      drawGameOver();
      lastBtn = digitalRead(BTN_OK);
      return;
    }
  }

  bool cur = digitalRead(BTN_OK);
  if (lastBtn == HIGH && cur == LOW && (now - lastButton) > 160) {
    lastButton = now;
    checkHit();
  }
  lastBtn = cur;

  if (now - lastFrame >= 16) {
    lastFrame = now;
    angleDot += speedDot;
    if (angleDot >= 360.0f) angleDot -= 360.0f;
    drawGame();
  }
}

} // namespace ZonaVerde

// Funciones/estado del menú principal usados por la galería.
void beepMenuMove();
void beepDenegado();
void drawMenu();
void invalidarMenuBase();
extern bool volumenPanelAbierto;


 // ══════════════════ AFINADOR CROMATICO ══════════════════
 // Generador senoidal I2S. A suena mientras se mantiene pulsado.
 namespace AfinadorCocodrilo {
 static int nota=9, octava=4;
 static bool prevIz=false,prevDer=false,prevB=false;
 static bool prevArr=false,prevAb=false,prevPausa=false, sonando=false;
 static bool osdVisible=false;
 static uint32_t osdHasta=0;
 static const char* nombres[12]={"DO","DO#","RE","RE#","MI","FA","FA#","SOL","SOL#","LA","LA#","SI"};
 static float frecuenciaExacta(){
   return 440.0f*powf(2.0f,(float)((octava-4)*12+nota-9)/12.0f);
 }
 static uint32_t frecuencia(){return (uint32_t)lroundf(frecuenciaExacta());}
 static void dibujarTeclado(){
   // Una octava cromatica: 7 teclas blancas y 5 negras.
   static const uint8_t blancas[7]={0,2,4,5,7,9,11};
   static const int8_t negras[5]={1,3,6,8,10};
   static const uint8_t antes[5]={0,1,3,4,5};
   const int x0=20,y0=116,ancho=40,alto=88;
   for(int i=0;i<7;i++){
     const int x=x0+i*ancho;
     const bool elegido=nota==blancas[i];
     tft.fillRect(x+1,y0,ancho-2,alto,elegido?TFT_CYAN:TFT_WHITE);
     tft.drawRect(x,y0,ancho,alto,TFT_DARKGREY);
   }
   // Dibujar negras despues para respetar su superposicion fisica.
   for(int i=0;i<5;i++){
     const int x=x0+(antes[i]+1)*ancho-13;
     const bool elegido=nota==negras[i];
     tft.fillRoundRect(x,y0,26,53,3,elegido?TFT_YELLOW:0x1082);
     tft.drawRoundRect(x,y0,26,53,3,elegido?TFT_WHITE:TFT_DARKGREY);
   }
 }
 static void dibujarVolumenOSD(){
   // Mismo estilo y tiempo de despliegue que el reproductor Musica.
   const int x=80,y=146,w=160,h=26;
   tft.fillRoundRect(x,y,w,h,6,TFT_BLACK);
   tft.drawRoundRect(x,y,w,h,6,TFT_WHITE);
   const int bx=x+34,by=y+9,bw=w-47,bh=8;
   tft.drawRect(bx,by,bw,bh,TFT_DARKGREY);
   const int relleno=((bw-2)*volumenPaso)/VOLUME_PASOS_MAX;
   if(relleno>0)tft.fillRect(bx+1,by+1,relleno,bh-2,TFT_CYAN);
   tft.setTextDatum(ML_DATUM);
   tft.setTextColor(TFT_WHITE,TFT_BLACK);
   tft.drawString(String(volumenPaso),x+12,y+h/2,1);
   tft.setTextDatum(TL_DATUM);
 }
 static void dibujar(){
   tft.fillScreen(TFT_BLACK);
   tft.setTextDatum(MC_DATUM);
   tft.setTextColor(TFT_CYAN,TFT_BLACK);
   tft.drawString("AFINADOR",160,17,4);
   // Nombre de nota y frecuencia juntos, actualizados al cambiar tecla/octava.
   tft.setTextColor(TFT_YELLOW,TFT_BLACK);
   tft.drawString(String(nombres[nota])+String(octava)+"  "+String(frecuenciaExacta(),2)+" Hz",160,79,4);
   dibujarTeclado();
   tft.setTextColor(TFT_CYAN,TFT_BLACK);
   tft.drawString("IZQ/DER NOTA  B+IZQ/DER OCTAVA",160,217,1);
   tft.setTextColor(TFT_LIGHTGREY,TFT_BLACK);
   tft.drawString(sonando?"A: SONANDO":"A: TOCAR  |  MENU: SALIR",160,233,1);
   tft.setTextDatum(TL_DATUM);
   if((int32_t)(osdHasta-millis())>0){
     dibujarVolumenOSD();osdVisible=true;
   }else osdVisible=false;
 }
 static void entrar(){
   AudioMAX98357::stop();
   nota=9;octava=4;sonando=false;
   osdHasta=0;osdVisible=false;
   prevIz=prevDer=prevB=prevArr=prevAb=prevPausa=false;
   dibujar();
 }
 static void actualizar(){
   bool iz=digitalRead(PIN_LEFT)==LOW, der=digitalRead(PIN_RIGHT)==LOW;
   bool arr=digitalRead(PIN_UP)==LOW, ab=digitalRead(PIN_DOWN)==LOW;
   bool b=digitalRead(PIN_VOLUMEN_PANEL)==LOW, a=digitalRead(BUTTON_PIN)==LOW;
   bool pausa=digitalRead(PIN_PAUSA)==LOW;
   if(pausa&&!prevPausa){
     AudioMAX98357::stop();sonando=false;
     gameState=STATE_MENU;invalidarMenuBase();drawMenu();
     prevPausa=pausa;return;
   }
   prevPausa=pausa;
   bool refrescar=false;
   if((iz&&!prevIz)||(der&&!prevDer)){
     const int delta=(der&&!prevDer)?1:-1;
     if(b){
       // B + direccion: salto de octava conservando la nota.
       octava=constrain(octava+delta,1,7);
     }else{
       // Recorrido cromatico continuo: SI4 -> DO5 y DO5 -> SI4.
       const int total=(octava-1)*12+nota+delta;
       const int limitado=constrain(total,0,7*12-1);
       octava=1+limitado/12;
       nota=limitado%12;
     }
     refrescar=true;
   }
   if(arr&&!prevArr){volumenPaso=min(VOLUME_PASOS_MAX,volumenPaso+1);aplicarVolumenChopper();osdHasta=millis()+1400;refrescar=true;}
   if(ab&&!prevAb){volumenPaso=max(0,volumenPaso-1);aplicarVolumenChopper();osdHasta=millis()+1400;refrescar=true;}
   if(a){
     AudioMAX98357::tone(frecuencia());
     if(!sonando){sonando=true;refrescar=true;}
   }else if(sonando){
     AudioMAX98357::stop();sonando=false;refrescar=true;
   }
   if(osdVisible&&(int32_t)(osdHasta-millis())<=0)refrescar=true;
   prevIz=iz;prevDer=der;prevB=b;prevArr=arr;prevAb=ab;
   if(refrescar)dibujar();
 }
 } // namespace AfinadorCocodrilo

// ═══════════════════ METRONOMO COCODRILO ════════════════════
// Temporizador no bloqueante con acumulación de fase en microsegundos.
namespace MetronomoCocodrilo {
int bpm=120,compas=4,siguiente=0,ultimoTocado=-1;
int acentoExtra=-1,opcion=0;
bool activo=false,opciones=false,modoTap=false,prevB=false,prevIz=false,prevDer=false,prevArr=false,prevAb=false,prevPausa=false;
uint32_t proximoPulso=0,tonoHasta=0,bDesde=0;
uint32_t taps[4]={0,0,0,0};
uint8_t numTaps=0;
const uint32_t B_LARGO_MS=650;
uint32_t periodo(){return 60000000UL/(uint32_t)bpm/(compas==6?2UL:1UL);}
void silencio(){tonoHasta=0;AudioMAX98357::stop();}
void detener(){activo=false;silencio();}
void iniciar(){siguiente=0;ultimoTocado=-1;proximoPulso=micros();activo=true;}
void tap(){
  uint32_t ahora=millis();
  if(numTaps && ahora-taps[numTaps-1]>2000UL)numTaps=0;
  if(numTaps==4){for(int i=0;i<3;i++)taps[i]=taps[i+1];numTaps=3;}
  taps[numTaps++]=ahora;
  if(numTaps>=2){
    uint32_t total=0;
    for(int i=1;i<numTaps;i++)total+=taps[i]-taps[i-1];
    uint32_t promedio=total/(numTaps-1);
    if(promedio>=250 && promedio<=2000){
      bpm=constrain((int)(60000UL/promedio),30,240);
      if(activo)iniciar();
    }
  }
}
void dibujarIndicadores(){
  tft.fillRect(18,148,284,58,TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  for(int i=0;i<compas;i++){
    int x=160+i*35-(compas-1)*35/2;
    bool fuerte=i==0 || i==acentoExtra;
    bool actual=activo&&ultimoTocado==i;
    uint16_t color=actual?(fuerte?TFT_ORANGE:TFT_CYAN):(fuerte?0xA2A0:0x4208);
    tft.fillCircle(x,171,fuerte?12:10,color);
    tft.setTextColor(TFT_WHITE,color);
    tft.drawNumber(i+1,x,171,2);
  }
  tft.setTextColor(TFT_WHITE,TFT_BLACK);
  tft.drawString(String(compas)+"/"+(compas==6?"8":"4")+"  "+(activo?"PLAY":"STOP"),160,197,2);
  tft.setTextDatum(TL_DATUM);
}
void dibujar(){
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_CYAN,TFT_BLACK);
  tft.drawString("METRONOMO",160,17,4);
  if(opciones){
    tft.setTextColor(TFT_WHITE,TFT_BLACK);
    tft.drawString("OPCIONES",160,51,4);
    tft.setTextColor(opcion==0?TFT_YELLOW:TFT_WHITE,TFT_BLACK);
    tft.drawString(String(opcion==0?"> ":"  ")+"COMPAS: "+String(compas)+"/"+(compas==6?"8":"4"),160,94,2);
    tft.setTextColor(opcion==1?TFT_YELLOW:TFT_WHITE,TFT_BLACK);
    tft.drawString(String(opcion==1?"> ":"  ")+"ACENTO EXTRA: "+(acentoExtra<0?String("NO"):String(acentoExtra+1)),160,130,2);
    tft.setTextColor(0xBDF7,TFT_BLACK);
    tft.drawString("ARR/ABA ELEGIR  IZQ/DER CAMBIAR",160,199,1);
    tft.drawString("B VOLVER   MENU SALIR",160,224,1);
  }else{
    tft.setTextColor(TFT_WHITE,TFT_BLACK);
    tft.drawNumber(bpm,160,91,7);
    tft.drawString("BPM",160,127,2);
    dibujarIndicadores();
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(0xBDF7,TFT_BLACK);
    tft.drawString(modoTap?"TAP: PULSA B AL RITMO":"IZQ/DER BPM  ARR/ABA VOL",160,215,1);
    tft.drawString(modoTap?"MANTEN B PARA TERMINAR":"A PLAY  B OPCIONES  MANTEN B TAP",160,230,1);
  }
  tft.setTextDatum(TL_DATUM);
}
void entrar(){
  bpm=120;compas=4;siguiente=0;ultimoTocado=-1;acentoExtra=-1;
  activo=false;opciones=false;modoTap=false;numTaps=0;
  prevB=prevIz=prevDer=prevArr=prevAb=prevPausa=false;
  dibujar();
}
void actualizar(bool botonA){
  const uint32_t ahora=micros();
  if(tonoHasta && (int32_t)(ahora-tonoHasta)>=0)silencio();
  if(activo && (int32_t)(ahora-proximoPulso)>=0){
    uint32_t p=periodo();
    uint32_t perdidos=(uint32_t)(ahora-proximoPulso)/p;
    uint32_t avance=perdidos+1;
    int golpe=(siguiente+(int)(perdidos%compas))%compas;
    siguiente=(golpe+1)%compas;
    proximoPulso+=(uint32_t)((uint64_t)avance*p);
    // Acentos basados en el golpe real, no en el siguiente.
    bool fuerte=golpe==0||golpe==acentoExtra;
    AudioMAX98357::clickMetronomo(fuerte);
    tonoHasta=0; // Duracion contenida en la muestra PCM.
    ultimoTocado=golpe;
    if(!opciones)dibujarIndicadores();
  }
  bool iz=digitalRead(PIN_LEFT)==LOW,de=digitalRead(PIN_RIGHT)==LOW;
  bool ar=digitalRead(PIN_UP)==LOW,ab=digitalRead(PIN_DOWN)==LOW;
  bool b=digitalRead(PIN_VOLUMEN_PANEL)==LOW;
  bool pausa=digitalRead(PIN_PAUSA)==LOW;
  if(pausa&&!prevPausa){
    detener();gameState=STATE_MENU;invalidarMenuBase();drawMenu();prevPausa=pausa;return;
  }
  prevPausa=pausa;
  bool refrescar=false;
  if(b&&!prevB)bDesde=millis();
  if(!b&&prevB){
    const bool largo=millis()-bDesde>=B_LARGO_MS;
    if(largo){
      modoTap=!modoTap;opciones=false;numTaps=0;refrescar=true;
    }else if(modoTap){
      tap();refrescar=true;
    }else{
      opciones=!opciones;refrescar=true;
    }
  }
  if(!modoTap){
    if(opciones){
      if((ar&&!prevArr)||(ab&&!prevAb)){opcion=1-opcion;refrescar=true;}
      if((iz&&!prevIz)||(de&&!prevDer)){
        int direccion=de?1:-1;
        if(opcion==0){
          const int valores[]={2,3,4,6};
          int idx=0;while(idx<3&&valores[idx]!=compas)idx++;
          compas=valores[(idx+direccion+4)%4];
          if(acentoExtra>=compas)acentoExtra=-1;
          if(activo)iniciar();
        }else{
          acentoExtra+=direccion;
          if(acentoExtra>=compas)acentoExtra=-1;
          if(acentoExtra < -1)acentoExtra=compas-1;
          // El tiempo 1 siempre lleva el acento principal.
          if(acentoExtra==0)acentoExtra=direccion>0?1:-1;
        }
        refrescar=true;
      }
    }else{
      if(iz&&!prevIz){bpm=max(30,bpm-1);if(activo)iniciar();refrescar=true;}
      if(de&&!prevDer){bpm=min(240,bpm+1);if(activo)iniciar();refrescar=true;}
      if(ar&&!prevArr){volumenPaso=min(VOLUME_PASOS_MAX,volumenPaso+1);aplicarVolumenChopper();refrescar=true;}
      if(ab&&!prevAb){volumenPaso=max(0,volumenPaso-1);aplicarVolumenChopper();refrescar=true;}
      if(botonA){if(activo)detener();else iniciar();ultimoTocado=-1;refrescar=true;}
    }
  }
  prevB=b;prevIz=iz;prevDer=de;prevArr=ar;prevAb=ab;
  if(refrescar)dibujar();
}
} // namespace MetronomoCocodrilo



// ════════════════════════════════════════════════════════════
//  GALERÍA MJPEG — integrada al launcher
//
//  Archivos esperados: /video/*.mjpeg
//  Cruceta arriba/abajo: elegir video
//  OK: reproducir
//  PAUSA (MENU): detener video o volver al menú principal
//
//  La galería reutiliza la TFT, la SD y TJpg_Decoder ya inicializados.
//  El buffer de 48 KB solo existe mientras la galería está abierta.
// ════════════════════════════════════════════════════════════

// ════════════════════════════════════════════════════════════
//  CUATETE MAGIC — juego integrado en el launcher
// ════════════════════════════════════════════════════════════

// ════════════════════════════════════════════════════════════
namespace CuateteMagic {
// ---------- Cruceta (Jugador 1) ----------
#define PIN_ADC 1
#define MUESTRAS 8

const int ADC_NINGUNO   = 3687;  // medido en este ESP32
const int ADC_DERECHA   = 2927;  // reescalado proporcional (33k)
const int ADC_IZQUIERDA = 1891;  // reescalado proporcional (10k)
const int ADC_ABAJO     = 828;   // reescalado proporcional (3.3k)
const int ADC_ARRIBA    = 284;   // reescalado proporcional (1k)

const int UMBRAL_DER_NINGUNO = (ADC_NINGUNO + ADC_DERECHA) / 2;
const int UMBRAL_IZQ_DER     = (ADC_DERECHA + ADC_IZQUIERDA) / 2;
const int UMBRAL_ABAJO_IZQ   = (ADC_IZQUIERDA + ADC_ABAJO) / 2;
const int UMBRAL_ARR_ABAJO   = (ADC_ABAJO + ADC_ARRIBA) / 2;

enum Boton { NINGUNO, ARRIBA, ABAJO, IZQUIERDA, DERECHA };

// ---------- Botones Jugador 2 ----------
#define PIN_J2_ARRIBA 2
#define PIN_J2_ABAJO  4

// ---------- Buzzer ----------
// NOTA: usamos la API nueva de LEDC (arduino-esp32 core 3.x), que ya
// no usa canales numéricos: ledcAttach/ledcWriteTone reciben el PIN
// directamente. Si tu core es 2.x y esto te marca error al reves,
// avisame y lo regreso a ledcSetup/ledcAttachPin con canal.
#define PIN_BUZZER    7

// ---------- Pantalla ----------
// Con setRotation(1) en un panel de 240x320 -> ancho 320, alto 240
const int SCREEN_W = 320;
const int SCREEN_H = 240;

// Franja superior reservada para nombres y marcador: ni la bola ni
// las palas pueden entrar ahi, para que nunca borren ese texto.
const int CAMPO_TOP = 34;

// ---------- Palas ----------
const int PALA_ANCHO = 8;
const int PALA_ALTO  = 40;
const int PALA_VEL   = 4;          // píxeles por frame mientras se mantiene el botón
const int PALA_MARGEN = 12;        // separación del borde de la pantalla

float palaY1, palaY2;              // posición Y (esquina superior) de cada pala
int palaY1_prev, palaY2_prev;

// ---------- Bola ----------
float bolaX, bolaY;
float bolaVX, bolaVY;
const int BOLA_TAM = 8;
int bolaX_prev, bolaY_prev;

// ---------- Marcador ----------
int puntosJ1 = 0;
int puntosJ2 = 0;
const int PUNTOS_PARA_GANAR = 7;

const char* NOMBRE_J1 = "ASTRO";   // izquierda - cruceta - lado AZUL
const char* NOMBRE_J2 = "COQUI";   // derecha   - GPIO2/GPIO4 - lado ROJO

// ---------- Estado del juego ----------
enum EstadoJuego { PORTADA, JUGANDO, PAUSADO, FIN_PARTIDA };
EstadoJuego estado = PORTADA;

// Evita que el OK usado para abrir Cuatete (GPIO2) salte la portada.
// La portada no acepta una entrada hasta que TODOS los controles hayan sido soltados.
bool entradaPortadaArmada = false;
bool entradaPortadaPrev = false;

// Menu de pausa compartiendo el volumen global del launcher.
enum EstadoPausa { PAUSA_MENU, PAUSA_VOLUMEN };
EstadoPausa estadoPausa = PAUSA_MENU;
enum OpcionPausa { OPCION_SALIR, OPCION_VOLUMEN };
OpcionPausa opcionPausa = OPCION_SALIR;
bool pausaPrev = false;
bool okPausaPrev = false;
Boton direccionPausaPrev = NINGUNO;

// ---------- Jugadores (para saber quien golpeo la bola por ultima vez) ----------
enum Jugador { NINGUN_JUGADOR, JUGADOR1, JUGADOR2 };
Jugador ultimoGolpe = NINGUN_JUGADOR;

// Prototipos manuales: el preprocesador de Arduino no siempre genera
// correctamente las declaraciones adelantadas dentro de un namespace.
void dibujarPortada();
void actualizarPortada();
void dibujarBordesNeon();
void dibujarCancha();
void dibujarNombres();
void dibujarMarcador();
void reiniciarBola(bool haciaJ1);
bool hayEntradaCualquierJugador();
void iniciarJuego();
void reiniciarPartida();
void moverJugador1();
void moverJugador2();
void moverBola();
void terminarPunto();
void centrarPalas();
void anunciarGol(const char* nombreAnotador);
void anunciarEscudo(const char* nombreProtegido);
void mostrarGanador(const char* nombreGanador);
void render();
void reiniciarComodines();
void actualizarComodines();
void generarComodin();
void dibujarComodin();
void dibujarIconoCuatetazo(bool pulso);
void dibujarIconoEscudo(bool pulso);
void dibujarIconoElote(bool pulso);
void borrarComodin();
void recogerComodin();
void activarCuatetazo();
void activarEloteDoble(Jugador dueno);
void moverBolaFalsa();
void quitarBolaFalsa();
void dibujarMenuPausa();
void manejarMenuPausa();
void restaurarPantallaJuego();
bool controlesCuatetePresionados();

// ---------- Colores ----------
#define COLOR_FONDO     TFT_BLACK
#define COLOR_BOLA      TFT_WHITE      // pelota blanca
#define COLOR_LINEA     TFT_DARKGREY
#define COLOR_TEXTO     TFT_CYAN
#define COLOR_GANADOR   TFT_GOLD
#define COLOR_CUATETAZO TFT_ORANGE
#define COLOR_ESCUDO    TFT_SKYBLUE
#define COLOR_ELOTE     TFT_GOLD
#define COLOR_FLAMA1    TFT_ORANGE
#define COLOR_FLAMA2    TFT_RED

// Colores de lado: ASTRO = azul neon, COQUI = rojo neon
#define COLOR_J1        TFT_BLUE
#define COLOR_J1_GLOW   TFT_NAVY
#define COLOR_J2        TFT_RED
#define COLOR_J2_GLOW   TFT_MAROON

unsigned long ultimoFrame = 0;
const int FRAME_MS = 16;   // ~60 fps

// ============================================================
//  FONDO ESTRELLADO
// ============================================================
// Nota: este juego no usa sprite/framebuffer, dibuja directo al
// TFT borrando solo la posicion anterior de cada cosa que se
// mueve. Por eso las estrellas se generan UNA vez (quedan fijas
// durante toda la partida) y cada rutina que "borra" algo (pala,
// pelota, comodin, chispa) usa las variantes *ConEstrellas de
// abajo, que primero pintan negro y luego repintan cualquier
// estrella que haya quedado adentro de esa zona borrada.
const int MAX_ESTRELLAS = 45;
struct Estrella { int16_t x, y; uint8_t brillo; };   // brillo 1 (tenue) - 3 (brillante)
Estrella estrellas[MAX_ESTRELLAS];

void generarEstrellas() {
  for (int i = 0; i < MAX_ESTRELLAS; i++) {
    estrellas[i].x = random(3, SCREEN_W - 3);
    estrellas[i].y = random(CAMPO_TOP + 3, SCREEN_H - 3);
    estrellas[i].brillo = random(1, 4);
  }
}

uint16_t colorEstrella(uint8_t brillo) {
  if (brillo >= 3) return TFT_WHITE;
  if (brillo == 2) return 0xC618;   // gris claro
  return 0x8410;                    // gris tenue
}

void dibujarEstrellas() {
  for (int i = 0; i < MAX_ESTRELLAS; i++) {
    tft.drawPixel(estrellas[i].x, estrellas[i].y, colorEstrella(estrellas[i].brillo));
  }
}

// Borra un rectangulo (pala, comodin) sin perder las estrellas que caigan adentro
void borrarRectConEstrellas(int x, int y, int w, int h) {
  tft.fillRect(x, y, w, h, COLOR_FONDO);
  for (int i = 0; i < MAX_ESTRELLAS; i++) {
    if (estrellas[i].x >= x && estrellas[i].x < x + w &&
        estrellas[i].y >= y && estrellas[i].y < y + h) {
      tft.drawPixel(estrellas[i].x, estrellas[i].y, colorEstrella(estrellas[i].brillo));
    }
  }
}

// Borra un circulo (pelota real/falsa) sin perder las estrellas que caigan adentro
void borrarCirculoConEstrellas(int cx, int cy, int r) {
  tft.fillCircle(cx, cy, r, COLOR_FONDO);
  int r2 = r * r;
  for (int i = 0; i < MAX_ESTRELLAS; i++) {
    int dx = estrellas[i].x - cx;
    int dy = estrellas[i].y - cy;
    if (dx * dx + dy * dy <= r2) {
      tft.drawPixel(estrellas[i].x, estrellas[i].y, colorEstrella(estrellas[i].brillo));
    }
  }
}

// Borra un solo pixel (chispa) sin perder la estrella si estaba ahi
void borrarPixelConEstrella(int x, int y) {
  uint16_t color = COLOR_FONDO;
  for (int i = 0; i < MAX_ESTRELLAS; i++) {
    if (estrellas[i].x == x && estrellas[i].y == y) {
      color = colorEstrella(estrellas[i].brillo);
      break;
    }
  }
  tft.drawPixel(x, y, color);
}

// ============================================================
//  CHISPAS al golpear la pelota contra una pala
// ============================================================
const int MAX_CHISPAS = 14;
struct Chispa {
  bool activa;
  float x, y, vx, vy;
  int vida, vidaMax;
  uint16_t color;
  int xPrev, yPrev;
};
Chispa chispas[MAX_CHISPAS];

void reiniciarChispas() {
  for (int i = 0; i < MAX_CHISPAS; i++) chispas[i].activa = false;
}

// dir: -1 si la chispa debe salir disparada hacia la izquierda
//      (golpe de J2) o +1 hacia la derecha (golpe de J1)
void generarChispas(float x, float y, int dir, uint8_t cantidad) {
  uint16_t colores[3] = { TFT_YELLOW, TFT_WHITE, TFT_ORANGE };
  for (int n = 0; n < cantidad; n++) {
    for (int i = 0; i < MAX_CHISPAS; i++) {
      if (!chispas[i].activa) {
        chispas[i].activa  = true;
        chispas[i].x       = x;
        chispas[i].y       = y;
        chispas[i].vx      = dir * (random(8, 30) / 10.0f);
        chispas[i].vy      = random(-25, 26) / 10.0f;
        chispas[i].vidaMax = random(8, 18);
        chispas[i].vida    = chispas[i].vidaMax;
        chispas[i].color   = colores[random(0, 3)];
        chispas[i].xPrev   = (int)x;
        chispas[i].yPrev   = (int)y;
        break;
      }
    }
  }
}

void actualizarChispas() {
  for (int i = 0; i < MAX_CHISPAS; i++) {
    if (!chispas[i].activa) continue;

    borrarPixelConEstrella(chispas[i].xPrev, chispas[i].yPrev);

    chispas[i].x  += chispas[i].vx;
    chispas[i].y  += chispas[i].vy;
    chispas[i].vy += 0.18f;   // gravedad ligera, para que caigan un poco
    chispas[i].vida--;

    if (chispas[i].vida <= 0 ||
        chispas[i].x < 0 || chispas[i].x >= SCREEN_W ||
        chispas[i].y < CAMPO_TOP || chispas[i].y >= SCREEN_H) {
      chispas[i].activa = false;
      continue;
    }
    chispas[i].xPrev = (int)chispas[i].x;
    chispas[i].yPrev = (int)chispas[i].y;
  }
}

void dibujarChispas() {
  for (int i = 0; i < MAX_CHISPAS; i++) {
    if (!chispas[i].activa) continue;
    // Las chispas mas viejas se apagan a blanco-gris para dar sensacion de "enfriarse"
    uint16_t c = (chispas[i].vida > chispas[i].vidaMax / 3) ? chispas[i].color : TFT_DARKGREY;
    tft.drawPixel((int)chispas[i].x, (int)chispas[i].y, c);
  }
}

// ============================================================
//  COMODINES
// ============================================================
enum TipoComodin { COMODIN_NINGUNO, COMODIN_CUATETAZO, COMODIN_ESCUDO, COMODIN_ELOTE };

const int COMODIN_TAM = 20;
const unsigned long COMODIN_VIDA_MS = 12000;  // tiempo que dura visible si nadie lo toca (antes 8000)

bool comodinEnPantalla = false;
TipoComodin comodinTipo = COMODIN_NINGUNO;
int comodinX, comodinY;
unsigned long comodinApareceEn = 0;   // proximo momento en el que puede salir uno nuevo
unsigned long comodinExpiraEn  = 0;   // cuando desaparece solo si nadie lo agarra
unsigned long comodinUltimoParpadeo = 0;   // controla la animacion de brillo del icono

// ---- CUATETAZO: acelera la pelota 3 segundos ----
bool cuatetazoActivo = false;
unsigned long cuatetazoFin = 0;
const float CUATETAZO_FACTOR = 1.6;
const unsigned long CUATETAZO_DURACION_MS = 3000;

// ---- MANGO ESCUDO: bloquea un gol ----
bool escudoJ1 = false;
bool escudoJ2 = false;

// ---- ELOTE DOBLE: pelota falsa ----
bool eloteActivo = false;
Jugador eloteDueno = NINGUN_JUGADOR;
float falsaX, falsaY, falsaVX, falsaVY;
int falsaX_prev, falsaY_prev;

// ============================================================
//  BUZZER (efectos de sonido no bloqueantes + melodias cortas)
// ============================================================
bool buzzerSonando = false;
unsigned long buzzerApagarEn = 0;

void iniciarBuzzer() {
  // El launcher ya configuró LEDC en GPIO7.
  arcadeBuzzerWriteStable(PIN_BUZZER, 0);
}

// Sonido corto que NO detiene el juego (para golpes, rebotes, comodines)
void reproducirTono(int frecuencia, int duracionMs) {
  arcadeBuzzerWriteStable(PIN_BUZZER, frecuencia);
  buzzerSonando = true;
  buzzerApagarEn = millis() + duracionMs;
}

// Revisa si ya toca apagar el tono actual (llamar cada frame)
void actualizarBuzzer() {
  if (buzzerSonando && millis() >= buzzerApagarEn) {
    arcadeBuzzerWriteStable(PIN_BUZZER, 0);
    buzzerSonando = false;
  }
}

// Nota bloqueante, solo para usarla dentro de pantallas que ya
// pausan el juego (anuncio de gol, ganador, escudo, portada).
void notaBloqueante(int frecuencia, int duracionMs) {
  arcadeBuzzerWriteStable(PIN_BUZZER, frecuencia);
  delay(duracionMs);
  arcadeBuzzerWriteStable(PIN_BUZZER, 0);
  delay(15);
}

void melodiaGol() {
  notaBloqueante(880, 90);
  notaBloqueante(1108, 90);
  notaBloqueante(1320, 160);
}

void melodiaGanador() {
  notaBloqueante(659, 120);
  notaBloqueante(659, 120);
  notaBloqueante(784, 120);
  notaBloqueante(988, 160);
  notaBloqueante(1318, 320);
}

void melodiaEscudo() {
  notaBloqueante(320, 110);
  notaBloqueante(180, 160);
}

void melodiaInicio() {
  notaBloqueante(440, 70);
  notaBloqueante(660, 70);
  notaBloqueante(880, 120);
}

void init() {
  pinMode(PIN_J2_ARRIBA, INPUT_PULLUP);
  pinMode(PIN_J2_ABAJO,  INPUT_PULLUP);

  // La TFT, ADC, buzzer y control de volumen ya fueron inicializados
  // por el launcher. Aquí solo reiniciamos el estado propio del juego.
  arcadeBuzzerWriteStable(PIN_BUZZER, 0);
  buzzerSonando = false;
  randomSeed(esp_random());
  generarEstrellas();
  reiniciarChispas();

  estado = PORTADA;
  entradaPortadaArmada = false;
  entradaPortadaPrev = false;
  estadoPausa = PAUSA_MENU;
  opcionPausa = OPCION_SALIR;
  pausaPrev = (digitalRead(PIN_PAUSA) == LOW);
  okPausaPrev = (digitalRead(BUTTON_PIN) == LOW);
  direccionPausaPrev = NINGUNO;
  ultimoFrame = 0;
  tft.setRotation(1);
  tft.fillScreen(COLOR_FONDO);
  dibujarPortada();
}

// ---------------- Lectura cruceta ----------------
int leerADCPromediado() {
  long suma = 0;
  for (int i = 0; i < MUESTRAS; i++) {
    suma += leerCrucetaADC();
    delayMicroseconds(150);
  }
  return suma / MUESTRAS;
}

Boton clasificar(int valor) {
  if (valor > UMBRAL_DER_NINGUNO) return NINGUNO;
  if (valor > UMBRAL_IZQ_DER)     return DERECHA;
  if (valor > UMBRAL_ABAJO_IZQ)   return IZQUIERDA;
  if (valor > UMBRAL_ARR_ABAJO)   return ABAJO;
  return ARRIBA;
}

// ============================================================
//  PORTADA
// ============================================================
void dibujarPortada() {
  tft.fillScreen(COLOR_FONDO);
  dibujarEstrellas();
  dibujarBordesNeon();

  // Titulo partido en dos colores para anticipar el azul/rojo del juego
  tft.setTextSize(4);
  tft.setTextColor(COLOR_J1, COLOR_FONDO);
  tft.setCursor(SCREEN_W / 2 - 118, 55);
  tft.print("CUATETE");

  tft.setTextSize(4);
  tft.setTextColor(COLOR_J2, COLOR_FONDO);
  tft.setCursor(SCREEN_W / 2 - 62, 95);
  tft.print("MAGIC");

  tft.setTextSize(1);
  tft.setTextColor(COLOR_TEXTO, COLOR_FONDO);
  const char* l1 = "CUATETAZO * MANGO ESCUDO * ELOTE DOBLE";
  tft.setCursor(SCREEN_W / 2 - (int)(strlen(l1) * 3), 150);
  tft.print(l1);

  tft.setTextColor(COLOR_J1, COLOR_FONDO);
  tft.setCursor(PALA_MARGEN + 4, 175);
  tft.print(NOMBRE_J1);
  tft.setTextColor(COLOR_TEXTO, COLOR_FONDO);
  tft.print(" amarillo_magenta");

  tft.setTextColor(COLOR_J2, COLOR_FONDO);
  int anchoJ2 = (strlen(NOMBRE_J2) + strlen(" azul_naranja")) * 6;
  tft.setCursor(SCREEN_W - PALA_MARGEN - 4 - anchoJ2, 175);
  tft.print(NOMBRE_J2);
  tft.setTextColor(COLOR_TEXTO, COLOR_FONDO);
  tft.print(" azul_naranja");
}

void actualizarPortada() {
  // Parpadeo del texto de inicio, sin volver a redibujar todo lo demas
  bool visible = (millis() / 500) % 2 == 0;
  const char* msg = "PRESIONA CUALQUIER BOTON PARA JUGAR";
  int ancho = strlen(msg) * 6;
  int x = SCREEN_W / 2 - ancho / 2;
  int y = 205;
  tft.setTextSize(1);
  if (visible) {
    tft.setTextColor(COLOR_GANADOR, COLOR_FONDO);
    tft.setCursor(x, y);
    tft.print(msg);
  } else {
    tft.fillRect(x - 2, y - 2, ancho + 4, 10, COLOR_FONDO);
  }
}

// Bordes / lineas neon a cada lado de la cancha para distinguir a
// ASTRO (izquierda, azul) de COQUI (derecha, rojo).
void dibujarBordesNeon() {
  tft.fillRect(0, 0, 2, SCREEN_H, COLOR_J1);
  tft.fillRect(2, 0, 2, SCREEN_H, COLOR_J1_GLOW);

  tft.fillRect(SCREEN_W - 2, 0, 2, SCREEN_H, COLOR_J2);
  tft.fillRect(SCREEN_W - 4, 0, 2, SCREEN_H, COLOR_J2_GLOW);
}

// ---------------- Dibujo ----------------
void dibujarCancha() {
  tft.fillScreen(COLOR_FONDO);
  dibujarEstrellas();
  dibujarBordesNeon();
  // Línea punteada central
  for (int y = 0; y < SCREEN_H; y += 12) {
    tft.fillRect(SCREEN_W / 2 - 1, y, 2, 6, COLOR_LINEA);
  }
  dibujarNombres();
}

void dibujarNombres() {
  tft.setTextSize(1);

  tft.setTextColor(COLOR_J1, COLOR_FONDO);
  tft.setCursor(PALA_MARGEN, 8);
  tft.print(NOMBRE_J1);

  tft.setTextColor(COLOR_J2, COLOR_FONDO);
  int anchoTexto = strlen(NOMBRE_J2) * 6;   // tamaño de texto 1 -> 6px por caracter
  const int marcadorJ2X = SCREEN_W / 2 + 30;
  const int separacionNombreMarcador = 8;
  tft.setCursor(marcadorJ2X - anchoTexto - separacionNombreMarcador, 8);
  tft.print(NOMBRE_J2);
}

void dibujarMarcador() {
  tft.setTextSize(3);

  // Los marcadores conservan sus posiciones originales a ambos lados
  // del centro. Solo el nombre de Coqui permanece desplazado a la izquierda.
  tft.setTextColor(COLOR_J1, COLOR_FONDO);
  tft.fillRect(SCREEN_W / 2 - 60, 6, 40, 24, COLOR_FONDO);
  tft.setCursor(SCREEN_W / 2 - 50, 6);
  tft.print(puntosJ1);

  tft.setTextColor(COLOR_J2, COLOR_FONDO);
  tft.fillRect(SCREEN_W / 2 + 20, 6, 40, 24, COLOR_FONDO);
  tft.setCursor(SCREEN_W / 2 + 30, 6);
  tft.print(puntosJ2);
}

void reiniciarBola(bool haciaJ1) {
  bolaX = SCREEN_W / 2 - BOLA_TAM / 2;
  bolaY = SCREEN_H / 2 - BOLA_TAM / 2;
  bolaX_prev = (int)bolaX;
  bolaY_prev = (int)bolaY;

  float velY = random(-100, 100) / 100.0;   // ligera inclinación vertical inicial
  float velX = haciaJ1 ? -3.4f : 3.4f;
  bolaVX = velX;
  bolaVY = velY;

  ultimoGolpe = NINGUN_JUGADOR;
}

// ---------------- Menu de pausa ----------------
void dibujarMenuPausa() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextSize(1);

  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("MENU", SCREEN_W / 2, 34, 4);

  if (estadoPausa == PAUSA_MENU) {
    const char* opciones[2] = {"SALIR", "SONIDO"};
    for (int i = 0; i < 2; i++) {
      bool seleccionado = (i == (int)opcionPausa);
      uint16_t fondo = seleccionado ? TFT_BLUE : 0x2104;
      uint16_t texto = seleccionado ? TFT_WHITE : TFT_LIGHTGREY;
      int y = 82 + i * 58;
      tft.fillRoundRect(82, y, 156, 44, 9, fondo);
      tft.drawRoundRect(82, y, 156, 44, 9, seleccionado ? TFT_CYAN : TFT_DARKGREY);
      tft.setTextColor(texto, fondo);
      tft.drawString(opciones[i], SCREEN_W / 2, y + 22, 2);
    }
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString("Arr/Ab: elegir   A: aceptar", SCREEN_W / 2, 207, 1);
    tft.drawString("MENU: continuar", SCREEN_W / 2, 224, 1);
  } else {
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("SONIDO", SCREEN_W / 2, 83, 4);

    const int x = 48, y = 122, w = 224, h = 24;
    tft.drawRoundRect(x, y, w, h, 6, TFT_WHITE);
    int relleno = (w - 4) * volumenPaso / VOLUME_PASOS_MAX;
    if (relleno > 0) tft.fillRoundRect(x + 2, y + 2, relleno, h - 4, 4, TFT_GREEN);

    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    String porcentaje = String(arcadeNombreVolumen());
    tft.drawString(porcentaje, SCREEN_W / 2, 174, 4);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString("Izq/Der: ajustar", SCREEN_W / 2, 210, 1);
    tft.drawString("MENU: volver", SCREEN_W / 2, 226, 1);
  }
  dibujarTemperaturaPausaTFT();
  tft.setTextDatum(TL_DATUM);
}

void restaurarPantallaJuego() {
  dibujarCancha();
  dibujarMarcador();
  render();
  if (comodinEnPantalla) dibujarComodin();
}

void manejarMenuPausa() {
  Boton direccion = clasificar(leerADCPromediado());
  bool eventoDireccion = (direccion != NINGUNO && direccionPausaPrev == NINGUNO);
  direccionPausaPrev = direccion;

  bool okAhora = (digitalRead(BUTTON_PIN) == LOW);
  bool okPressed = okAhora && !okPausaPrev;
  okPausaPrev = okAhora;

  if (estadoPausa == PAUSA_MENU) {
    if (eventoDireccion && (direccion == ARRIBA || direccion == ABAJO)) {
      opcionPausa = (opcionPausa == OPCION_SALIR) ? OPCION_VOLUMEN : OPCION_SALIR;
      dibujarMenuPausa();
    }
    if (okPressed) {
      if (opcionPausa == OPCION_SALIR) {
        arcadeBuzzerWriteStable(PIN_BUZZER, 0);
        buzzerSonando = false;
        volverAlMenu();
        return;
      }
      estadoPausa = PAUSA_VOLUMEN;
      dibujarMenuPausa();
    }
  } else if (eventoDireccion) {
    if (direccion == DERECHA && volumenPaso < VOLUME_PASOS_MAX) {
      volumenPaso++;
      aplicarVolumenChopper();
      dibujarMenuPausa();
    } else if (direccion == IZQUIERDA && volumenPaso > 0) {
      volumenPaso--;
      aplicarVolumenChopper();
      dibujarMenuPausa();
    }
  }
}

bool controlesCuatetePresionados() {
  return clasificar(leerADCPromediado()) != NINGUNO ||
         digitalRead(PIN_J2_ARRIBA) == LOW ||
         digitalRead(PIN_J2_ABAJO) == LOW;
}

// ---------------- Loop principal ----------------
void update() {
  bool pausaAhora = (digitalRead(PIN_PAUSA) == LOW);
  bool pausaPressed = pausaAhora && !pausaPrev;
  pausaPrev = pausaAhora;

  if (pausaPressed) {
    if (estado == PORTADA) {
      arcadeBuzzerWriteStable(PIN_BUZZER, 0);
      buzzerSonando = false;
      volverAlMenu();
      return;
    }

    if (estado == JUGANDO) {
      estado = PAUSADO;
      estadoPausa = PAUSA_MENU;
      opcionPausa = OPCION_SALIR;
      direccionPausaPrev = NINGUNO;
      okPausaPrev = (digitalRead(BUTTON_PIN) == LOW);
      arcadeBuzzerWriteStable(PIN_BUZZER, 0);
      buzzerSonando = false;
      dibujarMenuPausa();
    } else if (estado == PAUSADO) {
      if (estadoPausa == PAUSA_VOLUMEN) {
        estadoPausa = PAUSA_MENU;
        direccionPausaPrev = NINGUNO;
        dibujarMenuPausa();
      } else {
        estado = JUGANDO;
        ultimoFrame = millis();
        restaurarPantallaJuego();
      }
    }
  }

  if (estado == PAUSADO) {
    manejarMenuPausa();
    return;
  }

  if (millis() - ultimoFrame < FRAME_MS) return;
  ultimoFrame = millis();

  actualizarBuzzer();

  if (estado == PORTADA) {
    actualizarPortada();

    // Primero exige una liberacion completa. Asi el GPIO2 usado para
    // abrir el juego no cuenta como "cualquier boton" en la portada.
    bool entradaAhora = controlesCuatetePresionados();
    if (!entradaPortadaArmada) {
      if (!entradaAhora) {
        entradaPortadaArmada = true;
        entradaPortadaPrev = false;
      }
      return;
    }

    if (entradaAhora && !entradaPortadaPrev) iniciarJuego();
    entradaPortadaPrev = entradaAhora;
    return;
  }

  if (estado == FIN_PARTIDA) {
    if (hayEntradaCualquierJugador()) reiniciarPartida();
    return;
  }

  moverJugador1();
  moverJugador2();
  actualizarComodines();
  moverBola();
  if (estado == FIN_PARTIDA) return;
  moverBolaFalsa();
  render();
}

bool hayEntradaCualquierJugador() {
  int valor = leerADCPromediado();
  Boton b = clasificar(valor);
  bool crucetaPresionada = (b != NINGUNO);
  bool j2Presionado = (digitalRead(PIN_J2_ARRIBA) == LOW) || (digitalRead(PIN_J2_ABAJO) == LOW);
  return crucetaPresionada || j2Presionado;
}

// Arranca la partida por primera vez desde la portada
void iniciarJuego() {
  melodiaInicio();
  puntosJ1 = 0;
  puntosJ2 = 0;
  estado = JUGANDO;
  dibujarCancha();
  dibujarMarcador();
  reiniciarBola(random(0, 2) == 0);
  palaY1 = palaY2 = (SCREEN_H - PALA_ALTO) / 2;
  palaY1_prev = palaY2_prev = (int)palaY1;
  reiniciarComodines();
}

void reiniciarPartida() {
  puntosJ1 = 0;
  puntosJ2 = 0;
  estado = JUGANDO;
  dibujarCancha();
  dibujarMarcador();
  reiniciarBola(random(0, 2) == 0);
  palaY1 = palaY2 = (SCREEN_H - PALA_ALTO) / 2;
  palaY1_prev = palaY2_prev = (int)palaY1;
  reiniciarComodines();
}

void moverJugador1() {
  int valor = leerADCPromediado();
  Boton b = clasificar(valor);

  if (b == ARRIBA) palaY1 -= PALA_VEL;
  else if (b == ABAJO) palaY1 += PALA_VEL;

  palaY1 = constrain(palaY1, CAMPO_TOP, SCREEN_H - PALA_ALTO);
}

void moverJugador2() {
  // Botones activos en LOW (INPUT_PULLUP)
  bool arriba = digitalRead(PIN_J2_ARRIBA) == LOW;
  bool abajo  = digitalRead(PIN_J2_ABAJO)  == LOW;

  if (arriba) palaY2 -= PALA_VEL;
  else if (abajo) palaY2 += PALA_VEL;

  palaY2 = constrain(palaY2, CAMPO_TOP, SCREEN_H - PALA_ALTO);
}

void moverBola() {
  // Si el CUATETAZO ya termino sus 3 segundos, regresa la velocidad normal
  if (cuatetazoActivo && millis() >= cuatetazoFin) {
    bolaVX /= CUATETAZO_FACTOR;
    bolaVY /= CUATETAZO_FACTOR;
    cuatetazoActivo = false;
  }

  bolaX += bolaVX;
  bolaY += bolaVY;

  // Rebote arriba/abajo (el techo real es CAMPO_TOP, no 0, para
  // que la pelota nunca invada la franja de nombres/marcador)
  if (bolaY <= CAMPO_TOP) {
    bolaY = CAMPO_TOP;
    bolaVY = -bolaVY;
    reproducirTono(500, 15);
  } else if (bolaY + BOLA_TAM >= SCREEN_H) {
    bolaY = SCREEN_H - BOLA_TAM;
    bolaVY = -bolaVY;
    reproducirTono(500, 15);
  }

  // ¿La pelota real toca un comodin en la cancha?
  if (comodinEnPantalla &&
      bolaX + BOLA_TAM >= comodinX && bolaX <= comodinX + COMODIN_TAM &&
      bolaY + BOLA_TAM >= comodinY && bolaY <= comodinY + COMODIN_TAM) {
    recogerComodin();
  }

  int palaX1 = PALA_MARGEN;
  int palaX2 = SCREEN_W - PALA_MARGEN - PALA_ANCHO;

  // Colisión con pala del jugador 1 (izquierda, ASTRO)
  if (bolaVX < 0 &&
      bolaX <= palaX1 + PALA_ANCHO &&
      bolaX + BOLA_TAM >= palaX1 &&
      bolaY + BOLA_TAM >= palaY1 &&
      bolaY <= palaY1 + PALA_ALTO) {
    bolaX = palaX1 + PALA_ANCHO;
    bolaVX = -bolaVX * 1.06f;               // un poco más rápido cada golpe
    bolaVY += ((bolaY + BOLA_TAM / 2) - (palaY1 + PALA_ALTO / 2)) * 0.06;
    ultimoGolpe = JUGADOR1;
    reproducirTono(900, 25);
    generarChispas(bolaX, bolaY + BOLA_TAM / 2.0f, 1, 7);   // salen hacia la derecha
    // Si el contrincante (J1) golpea la pelota real, se quita la falsa
    if (eloteActivo && eloteDueno != JUGADOR1) quitarBolaFalsa();
  }

  // Colisión con pala del jugador 2 (derecha, COQUI)
  if (bolaVX > 0 &&
      bolaX + BOLA_TAM >= palaX2 &&
      bolaX <= palaX2 + PALA_ANCHO &&
      bolaY + BOLA_TAM >= palaY2 &&
      bolaY <= palaY2 + PALA_ALTO) {
    bolaX = palaX2 - BOLA_TAM;
    bolaVX = -bolaVX * 1.06f;
    bolaVY += ((bolaY + BOLA_TAM / 2) - (palaY2 + PALA_ALTO / 2)) * 0.06;
    ultimoGolpe = JUGADOR2;
    reproducirTono(1200, 25);
    generarChispas(bolaX + BOLA_TAM, bolaY + BOLA_TAM / 2.0f, -1, 7);   // salen hacia la izquierda
    // Si el contrincante (J2) golpea la pelota real, se quita la falsa
    if (eloteActivo && eloteDueno != JUGADOR2) quitarBolaFalsa();
  }

  // Límite de velocidad para que no se descontrole (mas alto si hay CUATETAZO)
  float maxVX = cuatetazoActivo ? 10.0f : 7.5f;
  float maxVY = cuatetazoActivo ? 8.0f : 5.5f;
  bolaVX = constrain(bolaVX, -maxVX, maxVX);
  bolaVY = constrain(bolaVY, -maxVY, maxVY);

  // Punto anotado
  if (bolaX < -BOLA_TAM) {
    if (escudoJ1) {
      // El MANGO ESCUDO de J1 bloquea el gol: rebota la pelota y se consume
      escudoJ1 = false;
      bolaX = 0;
      if (bolaVX < 0) bolaVX = -bolaVX;
      anunciarEscudo(NOMBRE_J1);
      return;
    }
    puntosJ2++;
    terminarPunto();
    if (puntosJ2 >= PUNTOS_PARA_GANAR) {
      mostrarGanador(NOMBRE_J2);
      return;
    }
    anunciarGol(NOMBRE_J2);
    dibujarCancha();
    dibujarMarcador();
    reiniciarBola(false);
    centrarPalas();
  } else if (bolaX > SCREEN_W) {
    if (escudoJ2) {
      // El MANGO ESCUDO de J2 bloquea el gol: rebota la pelota y se consume
      escudoJ2 = false;
      bolaX = SCREEN_W - BOLA_TAM;
      if (bolaVX > 0) bolaVX = -bolaVX;
      anunciarEscudo(NOMBRE_J2);
      return;
    }
    puntosJ1++;
    terminarPunto();
    if (puntosJ1 >= PUNTOS_PARA_GANAR) {
      mostrarGanador(NOMBRE_J1);
      return;
    }
    anunciarGol(NOMBRE_J1);
    dibujarCancha();
    dibujarMarcador();
    reiniciarBola(true);
    centrarPalas();
  }
}

// Limpia efectos de comodines que no deben sobrevivir al fin del punto
void terminarPunto() {
  cuatetazoActivo = false;
  quitarBolaFalsa();
  reiniciarChispas();
  if (comodinEnPantalla) {
    comodinEnPantalla = false;   // la pantalla se va a limpiar completa de todos modos
  }
  comodinApareceEn = millis() + random(3000, 7000);
}

void centrarPalas() {
  palaY1 = palaY2 = (SCREEN_H - PALA_ALTO) / 2;
  palaY1_prev = palaY2_prev = -100;   // fuerza redibujo en la nueva posición
}

void anunciarGol(const char* nombreAnotador) {
  tft.fillScreen(COLOR_FONDO);
  dibujarBordesNeon();

  uint16_t colorAnotador = (strcmp(nombreAnotador, NOMBRE_J1) == 0) ? COLOR_J1 : COLOR_J2;

  tft.setTextSize(3);
  tft.setTextColor(COLOR_GANADOR, COLOR_FONDO);
  const char* titulo = "GOOOL";
  int anchoTitulo = strlen(titulo) * 18;
  tft.setCursor(SCREEN_W / 2 - anchoTitulo / 2, 90);
  tft.print(titulo);

  tft.setTextSize(2);
  tft.setTextColor(colorAnotador, COLOR_FONDO);
  String linea = String("de ") + nombreAnotador;
  int anchoLinea = linea.length() * 12;
  tft.setCursor(SCREEN_W / 2 - anchoLinea / 2, 130);
  tft.print(linea);

  melodiaGol();
  delay(500);   // pausa breve para leer el anuncio antes de sacar de nuevo
}

// Mensaje corto cuando el MANGO ESCUDO bloquea un gol (no reinicia la jugada)
void anunciarEscudo(const char* nombreProtegido) {
  String msg = String("ESCUDO DE ") + nombreProtegido + " BLOQUEO EL GOL!";
  int ancho = msg.length() * 6;
  int x = SCREEN_W / 2 - ancho / 2;
  int y = SCREEN_H / 2 - 4;

  tft.setTextSize(1);
  tft.fillRect(x - 4, y - 4, ancho + 8, 16, COLOR_FONDO);
  tft.setTextColor(COLOR_ESCUDO, COLOR_FONDO);
  tft.setCursor(x, y);
  tft.print(msg);
  melodiaEscudo();
  delay(300);
  tft.fillRect(x - 4, y - 4, ancho + 8, 16, COLOR_FONDO);
}

void mostrarGanador(const char* nombreGanador) {
  estado = FIN_PARTIDA;

  tft.fillScreen(COLOR_FONDO);
  dibujarBordesNeon();

  uint16_t colorGanador = (strcmp(nombreGanador, NOMBRE_J1) == 0) ? COLOR_J1 : COLOR_J2;

  tft.setTextSize(2);
  tft.setTextColor(COLOR_GANADOR, COLOR_FONDO);
  int anchoTitulo = strlen("GANA") * 12;
  tft.setCursor(SCREEN_W / 2 - anchoTitulo / 2, 70);
  tft.print("GANA");

  tft.setTextSize(3);
  tft.setTextColor(colorGanador, COLOR_FONDO);
  int anchoNombre = strlen(nombreGanador) * 18;
  tft.setCursor(SCREEN_W / 2 - anchoNombre / 2, 100);
  tft.print(nombreGanador);

  tft.setTextSize(1);
  tft.setTextColor(COLOR_TEXTO, COLOR_FONDO);
  const char* msg = "Presiona cualquier boton para jugar de nuevo";
  int anchoMsg = strlen(msg) * 6;
  tft.setCursor(SCREEN_W / 2 - anchoMsg / 2, 160);
  tft.print(msg);

  melodiaGanador();
}

void render() {
  int palaX1 = PALA_MARGEN;
  int palaX2 = SCREEN_W - PALA_MARGEN - PALA_ANCHO;

  // Chispas: se actualizan (borran su pixel viejo) antes de redibujar
  // todo lo demas encima, para que ninguna pala/pelota las tape mal
  actualizarChispas();

  // Borra posición previa de la pala 1 y dibuja la nueva (azul, ASTRO)
  if ((int)palaY1 != palaY1_prev) {
    borrarRectConEstrellas(palaX1, palaY1_prev, PALA_ANCHO, PALA_ALTO);
  }
  tft.fillRect(palaX1, (int)palaY1, PALA_ANCHO, PALA_ALTO, COLOR_J1);
  palaY1_prev = (int)palaY1;

  // Borra posición previa de la pala 2 y dibuja la nueva (roja, COQUI)
  if ((int)palaY2 != palaY2_prev) {
    borrarRectConEstrellas(palaX2, palaY2_prev, PALA_ANCHO, PALA_ALTO);
  }
  tft.fillRect(palaX2, (int)palaY2, PALA_ANCHO, PALA_ALTO, COLOR_J2);
  palaY2_prev = (int)palaY2;

  // Borra bola previa y dibuja la nueva (circulo blanco; llamas si hay CUATETAZO)
  uint16_t colorBola = COLOR_BOLA;
  if (cuatetazoActivo) {
    colorBola = ((millis() / 100) % 2 == 0) ? COLOR_FLAMA1 : COLOR_FLAMA2;
  }
  borrarCirculoConEstrellas(bolaX_prev + BOLA_TAM / 2, bolaY_prev + BOLA_TAM / 2, BOLA_TAM / 2);
  tft.fillCircle((int)bolaX + BOLA_TAM / 2, (int)bolaY + BOLA_TAM / 2, BOLA_TAM / 2, colorBola);
  bolaX_prev = (int)bolaX;
  bolaY_prev = (int)bolaY;

  // Pelota falsa del comodin ELOTE DOBLE, si esta activa
  if (eloteActivo) {
    borrarCirculoConEstrellas(falsaX_prev + BOLA_TAM / 2, falsaY_prev + BOLA_TAM / 2, BOLA_TAM / 2);
    tft.fillCircle((int)falsaX + BOLA_TAM / 2, (int)falsaY + BOLA_TAM / 2, BOLA_TAM / 2, COLOR_BOLA);
    falsaX_prev = (int)falsaX;
    falsaY_prev = (int)falsaY;
  }

  // Chispas encima de todo lo demas
  dibujarChispas();
}

// ============================================================
//  COMODINES: aparicion, recoleccion y efectos
// ============================================================

void reiniciarComodines() {
  comodinEnPantalla = false;
  comodinTipo = COMODIN_NINGUNO;
  comodinApareceEn = millis() + random(3000, 7000);
  comodinExpiraEn = 0;

  cuatetazoActivo = false;
  escudoJ1 = false;
  escudoJ2 = false;

  eloteActivo = false;
  eloteDueno = NINGUN_JUGADOR;

  reiniciarChispas();
}

void actualizarComodines() {
  unsigned long ahora = millis();

  if (!comodinEnPantalla) {
    if (ahora >= comodinApareceEn) {
      generarComodin();
    }
    return;
  }

  // Si nadie lo agarro a tiempo, desaparece solo
  if (ahora >= comodinExpiraEn) {
    borrarComodin();
    comodinEnPantalla = false;
    comodinApareceEn = ahora + random(3000, 7000);
    return;
  }

  // Brillo pulsante para darle vida al icono (se redibuja cada ~150ms)
  if (ahora - comodinUltimoParpadeo >= 150) {
    comodinUltimoParpadeo = ahora;
    dibujarComodin();
  }
}

void generarComodin() {
  comodinTipo = (TipoComodin)random(1, 4);   // 1=CUATETAZO, 2=ESCUDO, 3=ELOTE
  comodinX = random(SCREEN_W / 2 - 70, SCREEN_W / 2 + 70 - COMODIN_TAM);
  comodinY = random(CAMPO_TOP + 6, SCREEN_H - 20 - COMODIN_TAM);
  comodinEnPantalla = true;
  comodinExpiraEn = millis() + COMODIN_VIDA_MS;
  comodinUltimoParpadeo = millis();
  dibujarComodin();
}

void dibujarComodin() {
  bool pulso = ((millis() / 150) % 2 == 0);
  switch (comodinTipo) {
    case COMODIN_CUATETAZO: dibujarIconoCuatetazo(pulso); break;
    case COMODIN_ESCUDO:    dibujarIconoEscudo(pulso);    break;
    case COMODIN_ELOTE:     dibujarIconoElote(pulso);     break;
    default: return;
  }
}

// C = CUATETAZO: una bolita de fuego con llama de dos capas que
// alterna colores para simular que arde de verdad.
void dibujarIconoCuatetazo(bool pulso) {
  int x = comodinX, y = comodinY, t = COMODIN_TAM;
  int cx = x + t / 2, cy = y + t / 2;

  uint16_t fondo = pulso ? TFT_MAROON : TFT_RED;
  tft.fillCircle(cx, cy, t / 2, fondo);
  tft.drawCircle(cx, cy, t / 2, TFT_WHITE);

  uint16_t llamaExt = pulso ? COLOR_FLAMA1 : COLOR_FLAMA2;
  uint16_t llamaInt = pulso ? TFT_YELLOW : COLOR_FLAMA1;

  tft.fillTriangle(cx, y + 3, x + 4, y + t - 4, x + t - 4, y + t - 4, llamaExt);
  tft.fillTriangle(cx, y + 7, x + 7, y + t - 5, x + t - 7, y + t - 5, llamaInt);
  tft.fillCircle(cx, y + 6, 1, TFT_WHITE);   // chispita en la punta
}

// M = MANGO ESCUDO: un escudo plateado con mango de madera y una
// cruz al centro, sobre un fondo azul que respira (brilla y se apaga).
void dibujarIconoEscudo(bool pulso) {
  int x = comodinX, y = comodinY, t = COMODIN_TAM;
  int cx = x + t / 2;

  uint16_t fondo = pulso ? TFT_NAVY : COLOR_ESCUDO;
  tft.fillRoundRect(x, y, t, t, 4, fondo);
  tft.drawRoundRect(x, y, t, t, 4, TFT_WHITE);

  int sx = x + 4, sy = y + 2;
  int sw = t - 8;
  int midY = y + t - 8;
  tft.fillRect(sx, sy, sw, midY - sy, TFT_SILVER);
  tft.fillTriangle(sx, midY, sx + sw, midY, cx, y + t - 3, TFT_SILVER);
  tft.drawRect(sx, sy, sw, midY - sy, TFT_WHITE);

  tft.fillRect(cx - 1, y + t - 3, 2, 3, TFT_BROWN);          // mango de madera
  tft.drawFastVLine(cx, sy + 1, midY - sy - 2, TFT_SKYBLUE); // cruz
  tft.drawFastHLine(sx + 2, sy + (midY - sy) / 2, sw - 4, TFT_SKYBLUE);
}

// D = ELOTE DOBLE: una mazorca dorada con granitos y hojita verde,
// mas dos puntitos blancos arriba que recuerdan que son DOS pelotas.
void dibujarIconoElote(bool pulso) {
  int x = comodinX, y = comodinY, t = COMODIN_TAM;
  int cx = x + t / 2, cy = y + t / 2;

  uint16_t fondo = pulso ? TFT_DARKGREEN : TFT_OLIVE;
  tft.fillRoundRect(x, y, t, t, 4, fondo);
  tft.drawRoundRect(x, y, t, t, 4, TFT_WHITE);

  tft.fillEllipse(cx, cy + 1, t / 2 - 4, t / 2 - 2, COLOR_ELOTE);
  for (int gy = cy - 3; gy <= cy + 5; gy += 3) {
    for (int gx = cx - 3; gx <= cx + 3; gx += 3) {
      tft.drawPixel(gx, gy, TFT_BROWN);
    }
  }
  tft.fillTriangle(cx - 4, y + 3, cx + 4, y + 3, cx, y - 1 + 5, TFT_GREEN);   // hojita

  tft.fillCircle(x + 4, y + 4, 2, TFT_WHITE);         // pelota 1 (doble)
  tft.fillCircle(x + t - 4, y + 4, 2, TFT_WHITE);     // pelota 2 (doble)
}

void borrarComodin() {
  borrarRectConEstrellas(comodinX, comodinY, COMODIN_TAM, COMODIN_TAM);
}

void recogerComodin() {
  TipoComodin tipoRecogido = comodinTipo;
  borrarComodin();
  comodinEnPantalla = false;
  comodinApareceEn = millis() + random(3000, 7000);

  // Si nadie ha golpeado la bola todavia (recien salio), nadie lo reclama
  if (ultimoGolpe == NINGUN_JUGADOR) {
    reproducirTono(300, 60);   // sonido neutro: comodin desperdiciado
    return;
  }

  switch (tipoRecogido) {
    case COMODIN_CUATETAZO:
      activarCuatetazo();
      reproducirTono(2000, 80);
      break;
    case COMODIN_ESCUDO:
      if (ultimoGolpe == JUGADOR1) escudoJ1 = true;
      else escudoJ2 = true;
      reproducirTono(1200, 90);
      break;
    case COMODIN_ELOTE:
      activarEloteDoble(ultimoGolpe);
      reproducirTono(700, 90);
      break;
    default:
      break;
  }
}

// ---- CUATETAZO ----
void activarCuatetazo() {
  cuatetazoActivo = true;
  cuatetazoFin = millis() + CUATETAZO_DURACION_MS;
  bolaVX *= CUATETAZO_FACTOR;
  bolaVY *= CUATETAZO_FACTOR;
}

// ---- ELOTE DOBLE ----
void activarEloteDoble(Jugador dueno) {
  eloteActivo = true;
  eloteDueno = dueno;

  falsaX = bolaX;
  falsaY = bolaY;
  // Misma direccion horizontal que la bola real: las DOS pelotas
  // salen disparadas hacia el rival. Solo varia un poco la
  // velocidad y el angulo para que se separen y sea dificil
  // saber cual es la autentica.
  float factorVel = 0.85 + random(0, 40) / 100.0;   // 0.85x - 1.25x la velocidad real
  falsaVX = bolaVX * factorVel;
  falsaVY = bolaVY + (random(-180, 180) / 100.0);

  falsaX_prev = (int)falsaX;
  falsaY_prev = (int)falsaY;
}

void moverBolaFalsa() {
  if (!eloteActivo) return;

  falsaX += falsaVX;
  falsaY += falsaVY;

  if (falsaY <= CAMPO_TOP) {
    falsaY = CAMPO_TOP;
    falsaVY = -falsaVY;
  } else if (falsaY + BOLA_TAM >= SCREEN_H) {
    falsaY = SCREEN_H - BOLA_TAM;
    falsaVY = -falsaVY;
  }

  // Rebota en los costados (no anota goles, solo distrae)
  if (falsaX <= 0) {
    falsaX = 0;
    falsaVX = -falsaVX;
  } else if (falsaX + BOLA_TAM >= SCREEN_W) {
    falsaX = SCREEN_W - BOLA_TAM;
    falsaVX = -falsaVX;
  }
}

void quitarBolaFalsa() {
  if (!eloteActivo) return;
  borrarCirculoConEstrellas(falsaX_prev + BOLA_TAM / 2, falsaY_prev + BOLA_TAM / 2, BOLA_TAM / 2);
  eloteActivo = false;
  eloteDueno = NINGUN_JUGADOR;
}

/* ============================================================
   NOTAS
   ============================================================
   - Portada "CUATETE MAGIC": al encender se queda mostrando el
     titulo (ASTRO en azul, COQUI en rojo) y parpadea "presiona
     cualquier boton para jugar". Cualquier entrada (cruceta o
     GPIO2/GPIO4) arranca la primera partida.
   - Jugador 1 (ASTRO, lado AZUL): mueve la cruceta hacia
     ARRIBA/ABAJO (mismo circuito ADC de tu sketch original,
     pin GPIO1).
   - Jugador 2 (COQUI, lado ROJO): dos botones normales, cada
     uno conectado entre el pin y GND:
       GPIO2 -> botón arriba
       GPIO4 -> botón abajo
     Se usan con INPUT_PULLUP, así que NO necesitas resistencias
     externas; el botón simplemente conecta el pin a GND al
     presionarlo.
   - Cada lado de la cancha tiene una linea/borde neon (azul a
     la izquierda, rojo a la derecha) y las palas, nombres,
     marcador y textos de "GOOOL"/"GANO" se pintan del color de
     quien corresponda, para distinguir a los jugadores de un
     vistazo.
   - El marcador se muestra arriba, centrado (Astro izquierda /
     Coqui derecha), con sus nombres justo encima.
   - Cada vez que anotan gol aparece "GOOOL de <nombre>" con una
     melodia corta, luego la bola y AMBAS palas vuelven al
     centro para el siguiente saque.
   - Gana quien llegue primero a 7 puntos (PUNTOS_PARA_GANAR).
     Al ganar se muestra "GANO <nombre>" con una melodia de
     victoria y el marcador se congela; presiona cualquier boton
     (cruceta o GPIO2/GPIO4) para reiniciar la partida a 0-0
     (sin pasar de nuevo por la portada).
   - La pelota es un circulo BLANCO.

   COMODINES (aparecen solos cada 3-7 s cerca del centro de la
   cancha, como iconos redondeados de 20x20 con brillo pulsante,
   ya no cajitas con letra):
     CUATETAZO (bolita de fuego con llama de dos colores) : lo
         reclama quien golpeo la bola por ultimo antes de
         tocarlo. Acelera la pelota 3 segundos y la pinta de
         colores de llama (naranja/rojo parpadeando).
     MANGO ESCUDO (escudo plateado con mango de madera y cruz) :
         lo reclama quien golpeo la bola por ultimo. La proxima
         vez que le anoten gol a ESE jugador, el gol se bloquea
         (la pelota rebota) y el escudo se consume. Se puede
         tener guardado varios puntos hasta que haga falta.
     ELOTE DOBLE (mazorca dorada con hojita y dos puntitos
         blancos) : lo reclama quien golpeo la bola por ultimo.
         Lanza una segunda pelota (blanca, identica a la real)
         EN LA MISMA DIRECCION que la real, hacia el rival, con
         velocidad y angulo ligeramente distintos para separarse
         y confundirlo. Se quita sola en cuanto: (a) el
         contrincante golpea la pelota REAL con su pala, o
         (b) se anota un gol (termina el punto).
   Si nadie ha golpeado la bola todavia (justo despues de un
   saque) y esta toca un comodin, se pierde sin efecto (con un
   sonido distinto, mas apagado), para que no se pueda "robar"
   antes de jugar.

   BUGS CORREGIDOS EN ESTA VERSION:
   - La bola y las palas ya no pueden entrar en la franja
     superior donde estan los nombres y el marcador (constante
     CAMPO_TOP), asi que ya no se pintan de negro esos textos al
     pasar la pelota o la pala cerca de ahi.
   - ELOTE DOBLE ahora lanza las DOS pelotas hacia el rival (antes
     la pelota falsa salia disparada de regreso hacia quien
     recogio el comodin, por error).

   SONIDO (buzzer en GPIO7, via LEDC canal 0):
     - Golpe de pala: bip corto (mas grave para ASTRO, mas
       agudo para COQUI).
     - Rebote arriba/abajo: bip breve y grave.
     - Recoger comodin: tono distinto segun el tipo (agudo =
       CUATETAZO, medio = ESCUDO, grave = ELOTE).
     - Comodin desperdiciado (nadie lo reclama): bip apagado.
     - Gol anotado / victoria / escudo bloqueando un gol /
       arranque desde la portada: melodias cortas de 2-5 notas.
   Si el buzzer no suena, revisa que este conectado entre GPIO7
   y GND (o segun tu modulo), y que ninguna otra parte del
   circuito este usando el canal LEDC 0.
   ============================================================ */

} // namespace CuateteMagic

// Las macros del sketch original no deben filtrarse al resto del launcher.
#undef PIN_ADC
#undef MUESTRAS
#undef PIN_J2_ARRIBA
#undef PIN_J2_ABAJO
#undef PIN_BUZZER
#undef COLOR_FONDO
#undef COLOR_BOLA
#undef COLOR_LINEA
#undef COLOR_TEXTO
#undef COLOR_GANADOR
#undef COLOR_CUATETAZO
#undef COLOR_ESCUDO
#undef COLOR_ELOTE
#undef COLOR_FLAMA1
#undef COLOR_FLAMA2
#undef COLOR_J1
#undef COLOR_J1_GLOW
#undef COLOR_J2
#undef COLOR_J2_GLOW

namespace Galeria {

enum VistaGaleria {
  VISTA_SECCIONES,
  VISTA_VIDEOS,
  VISTA_FOTOS,
  VISTA_FOTO_ABIERTA
};

static VistaGaleria vista = VISTA_SECCIONES;
static int seccionSeleccionada = 0; // 0=fotos, 1=videos

static const char *CARPETA = "/video";
static const char *CARPETA_FOTOS = "/images";
static const int MAX_FOTOS = 40;
String fotos[MAX_FOTOS];
int cantidadFotos = 0;
int seleccionFoto = 0;
int primeraFotoVisible = 0;
static const int MAX_ARCHIVOS = 20;
static const int FILAS_VISIBLES = 8;
static const size_t TAM_BUFFER = 48 * 1024;

String archivos[MAX_ARCHIVOS];
uint32_t tamanos[MAX_ARCHIVOS] = {0};
int cantidad = 0;
int seleccion = 0;
int primeroVisible = 0;
uint8_t *buffer = nullptr;
size_t bytesBuffer = 0;
bool reproduciendo = false;
bool mostrandoTutorial = false;
bool pausaAnterior = false;

// Audio acompañante de galería: mismo nombre base que el MJPEG.
// Ejemplo: /video/demo.mjpeg + /video/demo.wav
// Los videos con WAV se reproducen a 24 FPS; los MJPEG antiguos sin WAV
// conservan el comportamiento previo (sin temporización forzada).
static constexpr uint32_t FPS_AV = 24;
static constexpr uint32_t FRAME_US_AV = 1000000UL / FPS_AV;

static File archivoWav;
static TaskHandle_t tareaWav = nullptr;
static volatile bool detenerWav = false;
static volatile bool wavTerminado = false;
static uint32_t wavBytesRestantes = 0;
static bool wavI2STomado = false;
static uint32_t wavDataOffset = 0;
static uint32_t wavDataSizeTotal = 0;
static volatile bool wavPausaSolicitada = false;
static volatile bool wavPausaConfirmada = false;

static uint16_t leerLE16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t leerLE32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static String rutaWavPareja(const String &rutaMjpeg) {
  String r = rutaMjpeg;
  String low = r;
  low.toLowerCase();
  if (low.endsWith(".mjpeg")) r.remove(r.length() - 6);
  return r + ".wav";
}

static bool prepararWav(const String &ruta) {
  archivoWav = SD_MMC.open(ruta, FILE_READ);
  if (!archivoWav || archivoWav.isDirectory()) {
    if (archivoWav) archivoWav.close();
    return false;
  }

  uint8_t cab[12];
  if (archivoWav.read(cab, sizeof(cab)) != (int)sizeof(cab) ||
      memcmp(cab, "RIFF", 4) != 0 || memcmp(cab + 8, "WAVE", 4) != 0) {
    Serial.printf("[GALERIA][WAV] cabecera RIFF invalida: %s\n", ruta.c_str());
    archivoWav.close();
    return false;
  }

  bool fmtOk = false;
  bool dataOk = false;
  uint16_t formato = 0, canales = 0, bits = 0;
  uint32_t sampleRate = 0, dataOffset = 0, dataSize = 0;

  while (archivoWav.available()) {
    uint8_t ch[8];
    if (archivoWav.read(ch, sizeof(ch)) != (int)sizeof(ch)) break;
    const uint32_t tam = leerLE32(ch + 4);
    const uint32_t inicioDatos = (uint32_t)archivoWav.position();

    if (memcmp(ch, "fmt ", 4) == 0) {
      uint8_t fmt[16];
      if (tam < 16 || archivoWav.read(fmt, sizeof(fmt)) != (int)sizeof(fmt)) break;
      formato = leerLE16(fmt + 0);
      canales = leerLE16(fmt + 2);
      sampleRate = leerLE32(fmt + 4);
      bits = leerLE16(fmt + 14);
      fmtOk = true;
    } else if (memcmp(ch, "data", 4) == 0) {
      dataOffset = inicioDatos;
      dataSize = tam;
      dataOk = true;
      break;
    }

    const uint32_t siguiente = inicioDatos + tam + (tam & 1U);
    if (!archivoWav.seek(siguiente)) break;
  }

  if (!fmtOk || !dataOk || formato != 1 || canales != 2 ||
      sampleRate != 44100 || bits != 16) {
    Serial.printf(
      "[GALERIA][WAV] formato no compatible: fmt=%u ch=%u hz=%u bits=%u\n",
      (unsigned)formato, (unsigned)canales, (unsigned)sampleRate, (unsigned)bits);
    archivoWav.close();
    return false;
  }

  if (!archivoWav.seek(dataOffset)) {
    archivoWav.close();
    return false;
  }

  wavDataOffset = dataOffset;
  wavDataSizeTotal = dataSize;
  wavBytesRestantes = dataSize;
  detenerWav = false;
  wavTerminado = false;
  wavPausaSolicitada = false;
  wavPausaConfirmada = false;

  // Reutiliza la cesión cooperativa ya probada para NES: la tarea de tonos
  // deja de escribir en I2S_NUM_1 mientras la galería manda PCM real.
  if (!AudioMAX98357::entrarModoGaleria()) {
    Serial.println("[GALERIA][WAV] no se pudo tomar I2S");
    archivoWav.close();
    return false;
  }
  wavI2STomado = true;

  Serial.printf("[GALERIA][WAV] listo: %s PCM16 44100 Hz stereo, %u bytes\n",
                ruta.c_str(), (unsigned)dataSize);
  return true;
}

static void tareaSalidaWav(void*) {
  static uint8_t pcm[2048]; // 512 frames stereo ~= 11.6 ms
  static const uint8_t escalaAudifonos[11] = {
    0, 7, 14, 22, 31, 41, 55, 68, 78, 86, 94
  };
  static const uint8_t escalaAltavoz[11] = {
    0, 7, 14, 22, 31, 41, 49, 54, 58, 62, 66
  };

  while (!detenerWav && wavBytesRestantes > 0) {
    if (wavPausaSolicitada) {
      wavPausaConfirmada = true;
      while (wavPausaSolicitada && !detenerWav) {
        vTaskDelay(pdMS_TO_TICKS(1));
      }
      wavPausaConfirmada = false;
      if (detenerWav) break;
    }

    const size_t pedir = min((uint32_t)sizeof(pcm), wavBytesRestantes);
    int n = archivoWav.read(pcm, pedir);
    if (n <= 0) break;
    wavBytesRestantes -= (uint32_t)n;

    uint8_t paso = AudioMAX98357::getVolumeStep();
    if (paso > 10) paso = 10;
    const bool audifonos = AudioMAX98357::hayAudifonos();
    const uint8_t *tabla = audifonos ? escalaAudifonos : escalaAltavoz;
    const int escala = AudioMAX98357::isEnabled() ? tabla[paso] : 0;

    int16_t *m = reinterpret_cast<int16_t*>(pcm);
    const int muestras = n / (int)sizeof(int16_t);
    for (int i = 0; i < muestras; ++i) {
      m[i] = (int16_t)(((int32_t)m[i] * escala) / 100);
    }

    size_t escritos = 0;
    esp_err_t err = i2s_write(
      AudioMAX98357::I2S_PORT, pcm, (size_t)n, &escritos, portMAX_DELAY);
    if (err != ESP_OK || escritos != (size_t)n) {
      Serial.printf("[GALERIA][WAV] I2S err=%d %u/%u\n",
                    (int)err, (unsigned)escritos, (unsigned)n);
      break;
    }
  }

  wavTerminado = true;
  tareaWav = nullptr;
  vTaskDelete(nullptr);
}

static bool arrancarWav() {
  if (!wavI2STomado || !archivoWav) return false;
  BaseType_t ok = xTaskCreatePinnedToCore(
      tareaSalidaWav, "GaleriaWAV", 4096, nullptr, 3, &tareaWav, 0);
  if (ok != pdPASS) {
    tareaWav = nullptr;
    Serial.println("[GALERIA][WAV] no se pudo crear tarea PCM");
    return false;
  }
  return true;
}

static void detenerYRestaurarWav() {
  detenerWav = true;
  const uint32_t t0 = millis();
  while (tareaWav != nullptr && (millis() - t0) < 250) {
    vTaskDelay(pdMS_TO_TICKS(1));
  }

  if (archivoWav) archivoWav.close();
  wavBytesRestantes = 0;
  wavTerminado = true;
  wavPausaSolicitada = false;
  wavPausaConfirmada = false;

  if (wavI2STomado) {
    AudioMAX98357::salirModoGaleria();
    wavI2STomado = false;
  }
}

static void pausarWav(bool pausa) {
  if (!wavI2STomado || tareaWav == nullptr) return;

  if (pausa) {
    wavPausaSolicitada = true;
    const uint32_t t0 = millis();
    while (!wavPausaConfirmada && tareaWav != nullptr && (millis() - t0) < 100) {
      vTaskDelay(pdMS_TO_TICKS(1));
    }
    // Quita el audio que ya estuviera esperando en DMA para que la pausa sea inmediata.
    i2s_zero_dma_buffer(AudioMAX98357::I2S_PORT);
  } else {
    wavPausaSolicitada = false;
  }
}

static uint32_t duracionWavMs() {
  // 44100 Hz * 2 canales * 16 bits = 176400 bytes/s.
  if (wavDataSizeTotal == 0) return 0;
  return (uint32_t)(((uint64_t)wavDataSizeTotal * 1000ULL) / 176400ULL);
}

static bool buscarWavMs(uint32_t destinoMs) {
  if (!wavI2STomado || !archivoWav || wavDataSizeTotal == 0) return false;

  const uint32_t dur = duracionWavMs();
  if (dur == 0) return false;
  if (destinoMs > dur) destinoMs = dur;

  // Cada frame PCM stereo ocupa 4 bytes. Alinear para no partir una muestra.
  uint64_t rel = ((uint64_t)wavDataSizeTotal * destinoMs) / dur;
  rel &= ~3ULL;
  if (rel > wavDataSizeTotal) rel = wavDataSizeTotal;

  wavPausaSolicitada = true;
  const uint32_t t0 = millis();
  while (!wavPausaConfirmada && tareaWav != nullptr && (millis() - t0) < 120) {
    vTaskDelay(pdMS_TO_TICKS(1));
  }

  i2s_zero_dma_buffer(AudioMAX98357::I2S_PORT);
  const bool ok = archivoWav.seek(wavDataOffset + (uint32_t)rel);
  if (ok) wavBytesRestantes = wavDataSizeTotal - (uint32_t)rel;
  wavPausaSolicitada = false;
  return ok;
}

static void mostrarVolumenVideo() {
  const int x = 45, y = SCR_H - 34, w = SCR_W - 90, h = 22;
  tft.fillRoundRect(x, y, w, h, 6, TFT_BLACK);
  tft.drawRoundRect(x, y, w, h, 6, TFT_WHITE);

  const int bx = x + 34, by = y + 7, bw = w - 45, bh = 8;
  tft.drawRect(bx, by, bw, bh, TFT_DARKGREY);
  const int fill = ((bw - 2) * volumenPaso) / VOLUME_PASOS_MAX;
  if (fill > 0) tft.fillRect(bx + 1, by + 1, fill, bh - 2, TFT_CYAN);

  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(String(volumenPaso), x + 12, y + h / 2, 1);
  tft.setTextDatum(TL_DATUM);
}

// -----------------------------------------------------------------------------
// GALERIA V2: tiempo, indice persistente y reanudacion
// -----------------------------------------------------------------------------
static constexpr uint32_t IDX_VERSION = 1;
static constexpr uint32_t RESUME_VERSION = 1;
static constexpr uint32_t RESUME_MIN_MS = 30000UL;   // no guardar < 30 s
// Guardado periodico desactivado: solo se guarda al pausar o salir.
static constexpr uint32_t BARRA_SEEK_MS = 2000UL;

struct CidxHeader {
  char magic[4];          // "CIDX"
  uint16_t version;       // 1
  uint16_t fps;           // FPS_AV
  uint32_t videoSize;     // valida que el MJPEG no haya cambiado
  uint32_t totalFrames;
  uint32_t offsetCount;   // un uint32_t por segundo
};

struct ResumeHeader {
  char magic[4];          // "CRES"
  uint16_t version;       // 1
  uint16_t reservado;
  uint32_t videoSize;
  uint32_t posicionMs;
  uint32_t duracionMs;
};

static String rutaConExtension(const String &rutaMjpeg, const char *ext) {
  String r = rutaMjpeg;
  String low = r;
  low.toLowerCase();
  if (low.endsWith(".mjpeg")) r.remove(r.length() - 6);
  return r + ext;
}

static String rutaIdxPareja(const String &rutaMjpeg) {
  return rutaConExtension(rutaMjpeg, ".idx");
}

static String rutaResumePareja(const String &rutaMjpeg) {
  return rutaConExtension(rutaMjpeg, ".resume");
}

static String formatoTiempoVideo(uint32_t ms) {
  uint32_t total = ms / 1000UL;
  uint32_t horas = total / 3600UL;
  uint32_t minutos = (total % 3600UL) / 60UL;
  uint32_t segundos = total % 60UL;
  char txt[20];

  if (horas > 0) {
    snprintf(txt, sizeof(txt), "%lu:%02lu:%02lu",
             (unsigned long)horas,
             (unsigned long)minutos,
             (unsigned long)segundos);
  } else {
    snprintf(txt, sizeof(txt), "%02lu:%02lu",
             (unsigned long)minutos,
             (unsigned long)segundos);
  }
  return String(txt);
}

static void mostrarBarraVideo(uint32_t actualMs, uint32_t totalMs,
                              const char *estado = nullptr) {
  if (totalMs == 0) return;
  if (actualMs > totalMs) actualMs = totalMs;

  const int panelX = 8;
  const int panelY = SCR_H - 55;
  const int panelW = SCR_W - 16;
  const int panelH = 47;
  const int barraX = panelX + 10;
  const int barraY = panelY + 30;
  const int barraW = panelW - 20;
  const int barraH = 8;

  tft.fillRoundRect(panelX, panelY, panelW, panelH, 7, TFT_BLACK);
  tft.drawRoundRect(panelX, panelY, panelW, panelH, 7, TFT_DARKGREY);

  const String actual = formatoTiempoVideo(actualMs);
  const String total = formatoTiempoVideo(totalMs);

  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(actual, panelX + 8, panelY + 14, 1);

  tft.setTextDatum(MR_DATUM);
  tft.drawString(total, panelX + panelW - 8, panelY + 14, 1);

  if (estado && estado[0]) {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString(estado, SCR_W / 2, panelY + 14, 1);
  }

  tft.drawRect(barraX, barraY, barraW, barraH, TFT_DARKGREY);
  int progreso = (int)(((uint64_t)(barraW - 2) * actualMs) / totalMs);
  if (progreso < 0) progreso = 0;
  if (progreso > barraW - 2) progreso = barraW - 2;
  if (progreso > 0) {
    tft.fillRect(barraX + 1, barraY + 1, progreso, barraH - 2, TFT_CYAN);
  }

  tft.setTextDatum(TL_DATUM);
}

static void mostrarPausaVideo(bool pausa, uint32_t actualMs = 0,
                              uint32_t totalMs = 0) {
  const int w = 104, h = 28;
  const int x = (SCR_W - w) / 2, y = (SCR_H - h) / 2;
  tft.fillRoundRect(x, y, w, h, 7, TFT_BLACK);
  tft.drawRoundRect(x, y, w, h, 7, pausa ? TFT_YELLOW : TFT_CYAN);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(pausa ? TFT_YELLOW : TFT_CYAN, TFT_BLACK);
  tft.drawString(pausa ? "PAUSA" : "REPRODUCIENDO", SCR_W / 2, SCR_H / 2, 2);
  tft.setTextDatum(TL_DATUM);

  if (pausa && totalMs > 0) {
    mostrarBarraVideo(actualMs, totalMs, "PAUSA");
  }
}

static void dibujarProgresoIndice(uint32_t hecho, uint32_t total) {
  if (total == 0) total = 1;
  uint32_t pct = (uint32_t)(((uint64_t)hecho * 100ULL) / total);
  if (pct > 100) pct = 100;

  const int x = 24, y = 126, w = SCR_W - 48, h = 12;
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("CREANDO INDICE", SCR_W / 2, 76, 2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(String((unsigned)pct) + "%", SCR_W / 2, 104, 1);

  tft.drawRect(x, y, w, h, TFT_DARKGREY);
  int fill = (int)(((uint64_t)(w - 2) * pct) / 100ULL);
  if (fill > 0) tft.fillRect(x + 1, y + 1, fill, h - 2, TFT_CYAN);

  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("Solo se hace la primera vez", SCR_W / 2, 157, 1);
  tft.drawString("MENU: cancelar", SCR_W / 2, 176, 1);
  tft.setTextDatum(TL_DATUM);
}

static bool validarIndice(File &idx, uint32_t tamVideo, CidxHeader &cab) {
  if (!idx || idx.isDirectory()) return false;
  if (!idx.seek(0)) return false;
  if (idx.read((uint8_t*)&cab, sizeof(cab)) != (int)sizeof(cab)) return false;
  if (memcmp(cab.magic, "CIDX", 4) != 0) return false;
  if (cab.version != IDX_VERSION) return false;
  if (cab.fps != FPS_AV) return false;
  if (cab.videoSize != tamVideo) return false;
  if (cab.totalFrames == 0 || cab.offsetCount == 0) return false;
  const uint64_t requerido =
      (uint64_t)sizeof(CidxHeader) + (uint64_t)cab.offsetCount * sizeof(uint32_t);
  return (uint64_t)idx.size() >= requerido;
}

static bool generarIndiceMJPEG(File &video, const String &rutaIdx,
                               uint32_t tamVideo, CidxHeader &cabSalida) {
  String rutaTmp = rutaIdx + ".tmp";
  if (SD_MMC.exists(rutaTmp)) SD_MMC.remove(rutaTmp);

  File out = SD_MMC.open(rutaTmp, FILE_WRITE);
  if (!out) {
    Serial.println("[GALERIA][IDX] no se pudo crear temporal");
    return false;
  }

  CidxHeader cab = {};
  memcpy(cab.magic, "CIDX", 4);
  cab.version = IDX_VERSION;
  cab.fps = FPS_AV;
  cab.videoSize = tamVideo;
  cab.totalFrames = 0;
  cab.offsetCount = 0;
  out.write((uint8_t*)&cab, sizeof(cab));

  if (!video.seek(0)) {
    out.close();
    SD_MMC.remove(rutaTmp);
    return false;
  }

  uint8_t scan[4096];
  uint8_t anterior = 0;
  uint32_t frame = 0;
  uint32_t offsets = 0;
  uint32_t ultimoUI = 0;
  bool cancelado = false;
  bool error = false;

  dibujarProgresoIndice(0, tamVideo);

  while (video.available()) {
    if (digitalRead(PIN_PAUSA) == LOW) {
      cancelado = true;
      break;
    }

    uint32_t base = (uint32_t)video.position();
    int n = video.read(scan, sizeof(scan));
    if (n <= 0) break;

    for (int i = 0; i < n; ++i) {
      const uint8_t b = scan[i];
      if (anterior == 0xFF && b == 0xD8) {
        uint32_t offset = base + (uint32_t)i - 1U;
        if ((frame % FPS_AV) == 0) {
          if (out.write((uint8_t*)&offset, sizeof(offset)) != sizeof(offset)) {
            error = true;
            break;
          }
          offsets++;
        }
        frame++;
      }
      anterior = b;
    }
    if (error) break;

    if (millis() - ultimoUI >= 250) {
      ultimoUI = millis();
      dibujarProgresoIndice((uint32_t)video.position(), tamVideo);
    }
    yield();
  }

  if (!cancelado && !error && frame > 0 && offsets > 0) {
    cab.totalFrames = frame;
    cab.offsetCount = offsets;
    if (!out.seek(0) ||
        out.write((uint8_t*)&cab, sizeof(cab)) != sizeof(cab)) {
      error = true;
    }
  }

  out.close();
  video.seek(0);
  bytesBuffer = 0;

  if (cancelado || error || frame == 0 || offsets == 0) {
    SD_MMC.remove(rutaTmp);
    Serial.printf("[GALERIA][IDX] %s\n", cancelado ? "cancelado" : "error");
    return false;
  }

  if (SD_MMC.exists(rutaIdx)) SD_MMC.remove(rutaIdx);
  if (!SD_MMC.rename(rutaTmp.c_str(), rutaIdx.c_str())) {
    SD_MMC.remove(rutaTmp);
    Serial.println("[GALERIA][IDX] no se pudo renombrar temporal");
    return false;
  }

  cabSalida = cab;
  dibujarProgresoIndice(tamVideo, tamVideo);
  delay(180);
  Serial.printf("[GALERIA][IDX] listo frames=%u entradas=%u\n",
                (unsigned)cab.totalFrames, (unsigned)cab.offsetCount);
  return true;
}

static bool leerOffsetIndice(File &idx, const CidxHeader &cab,
                             uint32_t segundo, uint32_t &offset) {
  if (!idx || cab.offsetCount == 0) return false;
  if (segundo >= cab.offsetCount) segundo = cab.offsetCount - 1;
  uint32_t pos = sizeof(CidxHeader) + segundo * sizeof(uint32_t);
  if (!idx.seek(pos)) return false;
  return idx.read((uint8_t*)&offset, sizeof(offset)) == (int)sizeof(offset);
}

static bool cargarResume(const String &rutaResume, uint32_t tamVideo,
                         uint32_t duracionMs, uint32_t &posicionMs) {
  File r = SD_MMC.open(rutaResume, FILE_READ);
  if (!r || r.isDirectory()) {
    if (r) r.close();
    return false;
  }

  ResumeHeader cab = {};
  bool ok = r.read((uint8_t*)&cab, sizeof(cab)) == (int)sizeof(cab);
  r.close();

  if (!ok || memcmp(cab.magic, "CRES", 4) != 0 ||
      cab.version != RESUME_VERSION || cab.videoSize != tamVideo) {
    return false;
  }

  if (cab.posicionMs < RESUME_MIN_MS) return false;
  if (duracionMs > 0 && cab.posicionMs >= (uint32_t)(((uint64_t)duracionMs * 95ULL) / 100ULL)) {
    SD_MMC.remove(rutaResume);
    return false;
  }

  posicionMs = cab.posicionMs;
  if (duracionMs > 0 && posicionMs > duracionMs) posicionMs = duracionMs;
  return true;
}

static void guardarResume(const String &rutaResume, uint32_t tamVideo,
                          uint32_t posicionMs, uint32_t duracionMs) {
  if (duracionMs == 0 || posicionMs < RESUME_MIN_MS ||
      posicionMs >= (uint32_t)(((uint64_t)duracionMs * 95ULL) / 100ULL)) {
    if (SD_MMC.exists(rutaResume)) SD_MMC.remove(rutaResume);
    return;
  }

  String tmp = rutaResume + ".tmp";
  if (SD_MMC.exists(tmp)) SD_MMC.remove(tmp);
  File r = SD_MMC.open(tmp, FILE_WRITE);
  if (!r) return;

  ResumeHeader cab = {};
  memcpy(cab.magic, "CRES", 4);
  cab.version = RESUME_VERSION;
  cab.videoSize = tamVideo;
  cab.posicionMs = posicionMs;
  cab.duracionMs = duracionMs;
  r.write((uint8_t*)&cab, sizeof(cab));
  r.close();

  if (SD_MMC.exists(rutaResume)) SD_MMC.remove(rutaResume);
  if (!SD_MMC.rename(tmp.c_str(), rutaResume.c_str())) SD_MMC.remove(tmp);
}

static int preguntarReanudar(uint32_t posicionMs, uint32_t duracionMs) {
  // 1 = reanudar, 0 = reiniciar, -1 = cancelar y volver a galeria.
  int opcion = 0; // 0 reanudar, 1 reiniciar
  while (digitalRead(PIN_OK) == LOW ||
         digitalRead(PIN_UP) == LOW ||
         digitalRead(PIN_DOWN) == LOW ||
         digitalRead(PIN_PAUSA) == LOW) delay(5);

  while (true) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString("CONTINUAR VIDEO?", SCR_W / 2, 52, 2);

    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(formatoTiempoVideo(posicionMs) + " / " +
                   formatoTiempoVideo(duracionMs), SCR_W / 2, 82, 1);

    const int x = 52, w = SCR_W - 104, h = 28;
    const int y1 = 112, y2 = 148;
    tft.fillRoundRect(x, y1, w, h, 6, opcion == 0 ? 0x2104 : TFT_BLACK);
    tft.drawRoundRect(x, y1, w, h, 6, opcion == 0 ? TFT_CYAN : TFT_DARKGREY);
    tft.fillRoundRect(x, y2, w, h, 6, opcion == 1 ? 0x2104 : TFT_BLACK);
    tft.drawRoundRect(x, y2, w, h, 6, opcion == 1 ? TFT_CYAN : TFT_DARKGREY);

    tft.setTextColor(opcion == 0 ? TFT_CYAN : TFT_WHITE,
                     opcion == 0 ? 0x2104 : TFT_BLACK);
    tft.drawString("Reanudar", SCR_W / 2, y1 + h / 2, 1);
    tft.setTextColor(opcion == 1 ? TFT_CYAN : TFT_WHITE,
                     opcion == 1 ? 0x2104 : TFT_BLACK);
    tft.drawString("Reiniciar", SCR_W / 2, y2 + h / 2, 1);

    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString("Arr/Ab: elegir  A: aceptar", SCR_W / 2, 201, 1);
    tft.drawString("MENU: volver", SCR_W / 2, 218, 1);
    tft.setTextDatum(TL_DATUM);

    while (true) {
      if (digitalRead(PIN_PAUSA) == LOW) {
        while (digitalRead(PIN_PAUSA) == LOW) delay(5);
        return -1;
      }
      if (digitalRead(PIN_UP) == LOW || digitalRead(PIN_DOWN) == LOW) {
        opcion = 1 - opcion;
        while (digitalRead(PIN_UP) == LOW || digitalRead(PIN_DOWN) == LOW) delay(5);
        break;
      }
      if (digitalRead(PIN_OK) == LOW) {
        while (digitalRead(PIN_OK) == LOW) delay(5);
        return opcion == 0 ? 1 : 0;
      }
      delay(5);
    }
  }
}

static bool salidaJpg(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  if (!bitmap) return false;

  // Un JPEG grande puede seguir excediendo 320x240 incluso con escala 1/8.
  // TJpg_Decoder entrega sus bloques MCU completos; los que ya quedaron fuera
  // de pantalla simplemente se ignoran. Devolver false aqui abortaria la
  // decodificacion con JDR_INTR aunque la imagen sea valida.
  if (y >= tft.height() || x >= tft.width()) return true;

  if (x + w > tft.width()) w = tft.width() - x;
  if (y + h > tft.height()) h = tft.height() - y;
  if (w == 0 || h == 0) return true;

  tft.pushImage(x, y, w, h, bitmap);
  return true;
}

static bool esJpegGaleria(const String &nombre) {
  int slash = nombre.lastIndexOf('/');
  String base = slash >= 0 ? nombre.substring(slash + 1) : nombre;
  base.toLowerCase();
  return !base.startsWith("._") &&
         (base.endsWith(".jpg") || base.endsWith(".jpeg"));
}

static String rutaFotoCompleta(const String &nombre) {
  if (nombre.startsWith("/")) return nombre;
  if (nombre.indexOf('/') >= 0) return "/" + nombre;
  return String(CARPETA_FOTOS) + "/" + nombre;
}

static void cargarFotos() {
  cantidadFotos = 0;
  seleccionFoto = 0;
  primeraFotoVisible = 0;

  File dir = SD_MMC.open(CARPETA_FOTOS);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    Serial.printf("Galeria: no se pudo abrir %s\n", CARPETA_FOTOS);
    return;
  }

  while (cantidadFotos < MAX_FOTOS) {
    File f = dir.openNextFile();
    if (!f) break;
    if (!f.isDirectory()) {
      String nombre = f.name();
      if (esJpegGaleria(nombre)) fotos[cantidadFotos++] = nombre;
    }
    f.close();
  }
  dir.close();

  Serial.printf("Galeria: %d fotos encontradas\n", cantidadFotos);
}

static String nombreFotoVisible(const String &ruta) {
  int slash = ruta.lastIndexOf('/');
  String n = slash >= 0 ? ruta.substring(slash + 1) : ruta;
  if (n.length() > 28) n = n.substring(0, 25) + "...";
  return n;
}

static void dibujarSelectorSecciones() {
  vista = VISTA_SECCIONES;
  tft.fillScreen(TFT_BLACK);
  tft.setTextFont(1);
  tft.setTextSize(1);

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("GALERIA", SCR_W / 2, 13, 2);
  tft.drawFastHLine(18, 39, SCR_W - 36, TFT_DARKGREY);

  const int w = 118, h = 92, gap = 18;
  const int x1 = (SCR_W - (w * 2 + gap)) / 2;
  const int x2 = x1 + w + gap;
  const int y = 67;

  for (int i = 0; i < 2; ++i) {
    const bool sel = seccionSeleccionada == i;
    const int x = i == 0 ? x1 : x2;
    tft.fillRoundRect(x, y, w, h, 10, sel ? 0x2104 : TFT_BLACK);
    tft.drawRoundRect(x, y, w, h, 10, sel ? colorCursor : TFT_DARKGREY);

    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(sel ? colorCursor : TFT_WHITE,
                     sel ? 0x2104 : TFT_BLACK);

    if (i == 0) {
      // Icono simple de fotografía.
      const int cx = x + w / 2;
      tft.drawRect(cx - 25, y + 20, 50, 34, sel ? colorCursor : TFT_LIGHTGREY);
      tft.fillCircle(cx + 12, y + 30, 5, sel ? colorCursor : TFT_LIGHTGREY);
      tft.drawLine(cx - 21, y + 49, cx - 7, y + 36, sel ? colorCursor : TFT_LIGHTGREY);
      tft.drawLine(cx - 7, y + 36, cx + 1, y + 44, sel ? colorCursor : TFT_LIGHTGREY);
      tft.drawLine(cx + 1, y + 44, cx + 10, y + 38, sel ? colorCursor : TFT_LIGHTGREY);
      tft.drawLine(cx + 10, y + 38, cx + 22, y + 49, sel ? colorCursor : TFT_LIGHTGREY);
      tft.drawString("FOTOS", cx, y + 72, 2);
    } else {
      // Icono simple de video.
      const int cx = x + w / 2;
      tft.drawRoundRect(cx - 26, y + 19, 52, 36, 5,
                        sel ? colorCursor : TFT_LIGHTGREY);
      tft.fillTriangle(cx - 7, y + 27, cx - 7, y + 47, cx + 12, y + 37,
                       sel ? colorCursor : TFT_LIGHTGREY);
      tft.drawString("VIDEOS", cx, y + 72, 2);
    }
  }

  tft.fillRect(0, SCR_H - 25, SCR_W, 25, 0x1082);
  tft.drawFastHLine(0, SCR_H - 25, SCR_W, TFT_DARKGREY);
  tft.setTextDatum(BC_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, 0x1082);
  tft.drawString("Izq/Der: elegir   A: abrir   MENU: volver",
                 SCR_W / 2, SCR_H - 6, 1);
  tft.setTextDatum(TL_DATUM);
}

static void ajustarScrollFotos() {
  if (cantidadFotos <= 0) {
    primeraFotoVisible = 0;
    return;
  }
  if (seleccionFoto < primeraFotoVisible) primeraFotoVisible = seleccionFoto;
  if (seleccionFoto >= primeraFotoVisible + FILAS_VISIBLES)
    primeraFotoVisible = seleccionFoto - FILAS_VISIBLES + 1;
}

static void dibujarMenuFotos() {
  vista = VISTA_FOTOS;
  tft.fillScreen(TFT_BLACK);
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("FOTOS", SCR_W / 2, 8, 2);
  tft.drawFastHLine(12, 31, SCR_W - 24, TFT_DARKGREY);

  const int y0 = 38;
  const int alto = 22;
  const int fin = min(cantidadFotos, primeraFotoVisible + FILAS_VISIBLES);

  for (int i = primeraFotoVisible; i < fin; ++i) {
    const int y = y0 + (i - primeraFotoVisible) * alto;
    const bool sel = i == seleccionFoto;

    if (sel) {
      tft.fillRoundRect(10, y, SCR_W - 20, alto - 2, 5, 0x2104);
      tft.drawRoundRect(10, y, SCR_W - 20, alto - 2, 5, colorCursor);
    }

    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(sel ? colorCursor : TFT_WHITE,
                     sel ? 0x2104 : TFT_BLACK);
    tft.drawString(String(sel ? "> " : "  ") + nombreFotoVisible(fotos[i]),
                   16, y + 9, 1);
  }

  if (cantidadFotos == 0) {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString("No hay fotos en /images", SCR_W / 2, 108, 2);
    tft.drawString("Formatos: JPG / JPEG", SCR_W / 2, 132, 1);
  }

  tft.fillRect(0, SCR_H - 25, SCR_W, 25, 0x1082);
  tft.drawFastHLine(0, SCR_H - 25, SCR_W, TFT_DARKGREY);
  tft.setTextDatum(BC_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, 0x1082);
  tft.drawString("Arr/Ab: elegir   A: abrir   MENU: secciones",
                 SCR_W / 2, SCR_H - 6, 1);
  tft.setTextDatum(TL_DATUM);
}

static bool abrirFotoSeleccionada() {
  if (cantidadFotos <= 0 || seleccionFoto < 0 || seleccionFoto >= cantidadFotos)
    return false;

  const String ruta = rutaFotoCompleta(fotos[seleccionFoto]);
  uint16_t iw = 0, ih = 0;
  if (TJpgDec.getFsJpgSize(&iw, &ih, ruta.c_str(), SD_MMC) != 0 ||
      iw == 0 || ih == 0) {
    beepDenegado();
    return false;
  }

  uint8_t escala = 1;
  while (escala < 8 &&
         ((iw + escala - 1) / escala > SCR_W ||
          (ih + escala - 1) / escala > SCR_H)) {
    escala *= 2;
  }

  const int dw = max(1, (int)iw / (int)escala);
  const int dh = max(1, (int)ih / (int)escala);
  const int dx = max(0, (SCR_W - dw) / 2);
  const int dy = max(0, (SCR_H - dh) / 2);

  tft.fillScreen(TFT_BLACK);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setJpgScale(escala);
  TJpgDec.setCallback(salidaJpg);

  const uint16_t rc = TJpgDec.drawFsJpg(dx, dy, ruta.c_str(), SD_MMC);

  TJpgDec.setSwapBytes(false);
  TJpgDec.setJpgScale(1);
  TJpgDec.setCallback(jpgDibujarBloque);

  if (rc != JDR_OK) {
    Serial.printf("[GALERIA][JPG] error al decodificar %s rc=%d\n",
                  ruta.c_str(), (int)rc);
    beepDenegado();
    dibujarMenuFotos();
    return false;
  }

  vista = VISTA_FOTO_ABIERTA;

  // Banda discreta inferior para conservar la foto casi a pantalla completa.
  tft.fillRect(0, SCR_H - 18, SCR_W, 18, TFT_BLACK);
  tft.setTextDatum(BC_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("A / MENU: volver", SCR_W / 2, SCR_H - 4, 1);
  tft.setTextDatum(TL_DATUM);
  return true;
}

static bool esMjpeg(const String &nombre) {
  int slash = nombre.lastIndexOf('/');
  String base = slash >= 0 ? nombre.substring(slash + 1) : nombre;
  base.toLowerCase();
  return base.endsWith(".mjpeg") && !base.startsWith("._");
}

static String rutaCompleta(const String &nombre) {
  if (nombre.startsWith("/")) return nombre;
  if (nombre.indexOf('/') >= 0) return "/" + nombre;
  return String(CARPETA) + "/" + nombre;
}

static void cargarLista() {
  cantidad = 0;
  seleccion = 0;
  primeroVisible = 0;

  File dir = SD_MMC.open(CARPETA);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    Serial.printf("Galeria: no se pudo abrir %s\n", CARPETA);
    return;
  }

  while (cantidad < MAX_ARCHIVOS) {
    File f = dir.openNextFile();
    if (!f) break;
    if (!f.isDirectory()) {
      String nombre = f.name();
      if (esMjpeg(nombre)) {
        archivos[cantidad] = nombre;
        tamanos[cantidad] = f.size();
        cantidad++;
      }
    }
    f.close();
  }
  dir.close();
  Serial.printf("Galeria: %d videos encontrados\n", cantidad);
}

static String nombreVisible(const String &ruta) {
  int slash = ruta.lastIndexOf('/');
  String n = slash >= 0 ? ruta.substring(slash + 1) : ruta;
  if (n.length() > 25) n = n.substring(0, 22) + "...";
  return n;
}

static int totalElementos() {
  // Dos herramientas fijas: tutorial y conversor. Los videos comienzan en 2.
  return cantidad + 2;
}

static String nombreElemento(int indice) {
  if (indice == 0) return "Tutorial: cargar JPG y MJPEG";
  if (indice == 1) return "Conversor MJPEG";
  return nombreVisible(archivos[indice - 2]);
}

static void dibujarTutorial() {
  mostrandoTutorial = true;
  tft.fillScreen(TFT_BLACK);
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(0xFFE0, TFT_BLACK);
  tft.drawString("TUTORIAL DE GALERIA", SCR_W / 2, 8, 2);
  tft.drawFastHLine(12, 31, SCR_W - 24, TFT_DARKGREY);

  QR::dibujarTutorialGaleria(tft, SCR_W / 2, 124, 164);

  tft.setTextDatum(BC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Escanea para aprender a cargar", SCR_W / 2, 216, 1);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("archivos JPG y videos MJPEG", SCR_W / 2, 229, 1);
  tft.setTextDatum(TL_DATUM);
}

static void dibujarConversorMJPEG() {
  mostrandoTutorial = true;
  tft.fillScreen(TFT_BLACK);
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("CONVERSOR MJPEG", SCR_W / 2, 8, 2);
  tft.drawFastHLine(12, 31, SCR_W - 24, TFT_DARKGREY);

  QR::dibujarConversorMJPEG(tft, SCR_W / 2, 124, 164);

  tft.setTextDatum(BC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Escanea para convertir videos", SCR_W / 2, 216, 1);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("al formato MJPEG de COCODRILO OS", SCR_W / 2, 229, 1);
  tft.setTextDatum(TL_DATUM);
}

static void ajustarScroll() {
  const int total = totalElementos();
  if (total <= 0) {
    primeroVisible = 0;
    return;
  }
  if (seleccion < primeroVisible) primeroVisible = seleccion;
  if (seleccion >= primeroVisible + FILAS_VISIBLES)
    primeroVisible = seleccion - FILAS_VISIBLES + 1;
}

static constexpr int MENU_ALTO_FILA = 22;
static constexpr int MENU_Y0 = 38;
static constexpr int MENU_MARGEN_X = 10;

void dibujarMenu();

static void dibujarFilaMenu(int indice) {
  const int total = totalElementos();
  if (indice < primeroVisible || indice >= primeroVisible + FILAS_VISIBLES ||
      indice < 0 || indice >= total) return;

  const int y = MENU_Y0 + (indice - primeroVisible) * MENU_ALTO_FILA;
  const bool sel = indice == seleccion;

  // Limpiar SOLO esta fila. Evita el flash de fillScreen() al mover el cursor.
  tft.fillRect(0, y, SCR_W, MENU_ALTO_FILA, TFT_BLACK);

  if (sel) {
    tft.fillRoundRect(MENU_MARGEN_X, y, SCR_W - MENU_MARGEN_X * 2,
                      MENU_ALTO_FILA - 2, 5, 0x2104);
    tft.drawRoundRect(MENU_MARGEN_X, y, SCR_W - MENU_MARGEN_X * 2,
                      MENU_ALTO_FILA - 2, 5, colorCursor);
  }

  const bool tutorial = indice == 0;
  const bool conversor = indice == 1;
  const uint16_t colorNormal =
      tutorial ? TFT_YELLOW : (conversor ? TFT_CYAN : TFT_WHITE);

  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(sel ? colorCursor : colorNormal,
                   sel ? 0x2104 : TFT_BLACK);
  tft.drawString(String(sel ? "> " : "  ") + nombreElemento(indice),
                 16, y + 9, 1);
  tft.setTextDatum(TL_DATUM);
}

static void actualizarCursorMenu(int seleccionAnterior, int primeroAnterior) {
  // Si cambió la ventana de scroll, sí hay filas distintas visibles y toca
  // reconstruir la lista. Mientras el scroll no cambie sólo se pintan 2 filas.
  if (primeroAnterior != primeroVisible) {
    dibujarMenu();
    return;
  }

  dibujarFilaMenu(seleccionAnterior);
  dibujarFilaMenu(seleccion);
}

void dibujarMenu() {
  vista = VISTA_VIDEOS;
  // Dibujo completo sólo al entrar, volver de un video/tutorial o cambiar scroll.
  tft.fillScreen(TFT_BLACK);
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(0xFFE0, TFT_BLACK);
  tft.drawString("GALERIA", SCR_W / 2, 8, 2);
  tft.drawFastHLine(12, 31, SCR_W - 24, TFT_DARKGREY);

  const int total = totalElementos();
  const int fin = min(total, primeroVisible + FILAS_VISIBLES);

  for (int i = primeroVisible; i < fin; ++i) {
    dibujarFilaMenu(i);
  }

  if (cantidad == 0) {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString("No hay videos en /video", SCR_W / 2, 145, 1);
    tft.drawString("Abre el tutorial para comenzar", SCR_W / 2, 160, 1);
  }

  tft.fillRect(0, SCR_H - 25, SCR_W, 25, 0x1082);
  tft.drawFastHLine(0, SCR_H - 25, SCR_W, TFT_DARKGREY);
  tft.setTextDatum(BC_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, 0x1082);
  tft.drawString("Arr/Ab: elegir   A: abrir   MENU: volver",
                 SCR_W / 2, SCR_H - 6, 1);
  tft.setTextDatum(TL_DATUM);
}

// Asegura que haya un JPEG completo en buffer. El frame NO se mueve
// hasta después de decodificarlo, evitando invalidar el puntero.
static bool buscarFrame(File &f, size_t &inicio, size_t &longitud) {
  while (true) {
    while (bytesBuffer < TAM_BUFFER && f.available()) {
      size_t libre = TAM_BUFFER - bytesBuffer;
      size_t pedir = libre < 2048 ? libre : 2048;
      int n = f.read(buffer + bytesBuffer, pedir);
      if (n <= 0) break;
      bytesBuffer += (size_t)n;
    }

    size_t soi = SIZE_MAX;
    for (size_t i = 0; i + 1 < bytesBuffer; ++i) {
      if (buffer[i] == 0xFF && buffer[i + 1] == 0xD8) { soi = i; break; }
    }

    if (soi == SIZE_MAX) {
      bytesBuffer = 0;
      if (!f.available()) return false;
      continue;
    }

    size_t eoi = SIZE_MAX;
    for (size_t i = soi + 2; i + 1 < bytesBuffer; ++i) {
      if (buffer[i] == 0xFF && buffer[i + 1] == 0xD9) { eoi = i + 1; break; }
    }

    if (eoi != SIZE_MAX) {
      inicio = soi;
      longitud = eoi - soi + 1;
      return true;
    }

    if (!f.available()) { bytesBuffer = 0; return false; }
    if (bytesBuffer >= TAM_BUFFER) {
      // Frame mayor que 48 KB: se descarta para continuar buscando.
      bytesBuffer = 0;
    }
  }
}

static void consumirFrame(size_t inicio, size_t longitud) {
  size_t consumidos = inicio + longitud;
  size_t restantes = bytesBuffer - consumidos;
  if (restantes) memmove(buffer, buffer + consumidos, restantes);
  bytesBuffer = restantes;
}

static void reproducirSeleccionado() {
  if (seleccion == 0) {
    dibujarTutorial();
    return;
  }

  if (seleccion == 1) {
    dibujarConversorMJPEG();
    return;
  }

  if (cantidad <= 0 || !buffer) return;

  const int indiceVideo = seleccion - 2;
  if (indiceVideo < 0 || indiceVideo >= cantidad) return;

  String ruta = rutaCompleta(archivos[indiceVideo]);
  File f = SD_MMC.open(ruta, FILE_READ);
  if (!f || f.isDirectory()) {
    if (f) f.close();
    beepDenegado();
    return;
  }

  reproduciendo = true;
  bytesBuffer = 0;
  tft.fillScreen(TFT_BLACK);
  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(salidaJpg);

  const uint32_t tamVideo = (uint32_t)f.size();
  const String rutaIdx = rutaIdxPareja(ruta);
  const String rutaResume = rutaResumePareja(ruta);

  // -----------------------------------------------------------------------
  // IDX persistente: se crea SOLO si falta o no corresponde al MJPEG actual.
  // Después se lee directamente desde SD: no ocupa una tabla grande en RAM.
  // -----------------------------------------------------------------------
  CidxHeader idxCab = {};
  File idx = SD_MMC.open(rutaIdx, FILE_READ);
  bool idxValido = validarIndice(idx, tamVideo, idxCab);
  if (!idxValido) {
    if (idx) idx.close();

    if (!generarIndiceMJPEG(f, rutaIdx, tamVideo, idxCab)) {
      // MENU durante la creación cancela la apertura del video.
      f.close();
      reproduciendo = false;
      TJpgDec.setSwapBytes(false);
      TJpgDec.setJpgScale(1);
      TJpgDec.setCallback(jpgDibujarBloque);
      while (digitalRead(PIN_PAUSA) == LOW) delay(5);
      dibujarMenu();
      return;
    }

    idx = SD_MMC.open(rutaIdx, FILE_READ);
    idxValido = validarIndice(idx, tamVideo, idxCab);
  }

  if (!idxValido) {
    if (idx) idx.close();
    f.close();
    reproduciendo = false;
    TJpgDec.setSwapBytes(false);
    TJpgDec.setJpgScale(1);
    TJpgDec.setCallback(jpgDibujarBloque);
    beepDenegado();
    dibujarMenu();
    return;
  }

  const uint32_t duracionVideoMs =
      (uint32_t)(((uint64_t)idxCab.totalFrames * 1000ULL) / FPS_AV);

  // Audio acompañante con el mismo nombre base.
  const String rutaWav = rutaWavPareja(ruta);
  bool conAudio = prepararWav(rutaWav);
  const uint32_t duracionAudioMs = conAudio ? duracionWavMs() : 0;
  const uint32_t duracionMs = duracionAudioMs ? duracionAudioMs : duracionVideoMs;

  // Flancos locales del reproductor. A = PIN_OK.
  while (digitalRead(PIN_PAUSA) == LOW || digitalRead(PIN_OK) == LOW) delay(5);
  bool aPrev = false;
  bool izqPrev = false, derPrev = false, arrPrev = false, abaPrev = false;
  bool pausado = false;
  uint32_t volumenOSDHasta = 0;
  uint32_t barraHasta = 0;
  uint32_t ultimoGuardadoResume = 0;

  // Estado para mantener IZQ/DER y acelerar el desplazamiento.
  int direccionHold = 0;
  uint32_t holdInicio = 0;
  uint32_t holdUltimoPaso = 0;

  size_t inicio = 0, longitud = 0;
  uint32_t frameIndice = 0;
  uint32_t frameBaseReloj = 0;
  uint32_t framesMostrados = 0;
  uint32_t framesSaltados = 0;
  uint32_t relojAV = 0;

  auto tiempoActualMs = [&]() -> uint32_t {
    uint64_t ms = ((uint64_t)frameIndice * 1000ULL) / FPS_AV;
    if (duracionMs > 0 && ms > duracionMs) ms = duracionMs;
    return (uint32_t)ms;
  };

  auto ajustarVolumenVideo = [&](int delta) {
    int nv = volumenPaso + delta;
    if (nv < 0) nv = 0;
    if (nv > VOLUME_PASOS_MAX) nv = VOLUME_PASOS_MAX;
    if (nv != volumenPaso) {
      volumenPaso = nv;
      sonidoActivo = (volumenPaso > 0);
      cocodriloAplicarVolumenMAX();
    }
    volumenOSDHasta = millis() + 1400;
    if (pausado) mostrarVolumenVideo();
  };

  auto buscarVideoMs = [&](uint32_t destinoMs) -> bool {
    if (!idxValido || idxCab.offsetCount == 0 || idxCab.totalFrames == 0)
      return false;

    uint32_t frameObjetivo =
        (uint32_t)(((uint64_t)destinoMs * FPS_AV) / 1000ULL);
    if (frameObjetivo >= idxCab.totalFrames)
      frameObjetivo = idxCab.totalFrames - 1;

    uint32_t segundoBase = frameObjetivo / FPS_AV;
    uint32_t frameBase = segundoBase * FPS_AV;
    uint32_t offset = 0;
    if (!leerOffsetIndice(idx, idxCab, segundoBase, offset)) return false;
    if (!f.seek(offset)) return false;
    bytesBuffer = 0;

    // Desde la entrada de ese segundo sólo recorremos los frames restantes
    // dentro del segundo (0..23 en 24 FPS).
    uint32_t saltar = frameObjetivo - frameBase;
    for (uint32_t i = 0; i < saltar; ++i) {
      size_t ini = 0, len = 0;
      if (!buscarFrame(f, ini, len)) return false;
      consumirFrame(ini, len);
    }
    return true;
  };

  auto irATiempo = [&](uint32_t destinoMs, bool mantenerPausa,
                        const char *estado) -> bool {
    if (duracionMs == 0) return false;
    if (destinoMs >= duracionMs) destinoMs = duracionMs > 250 ? duracionMs - 250 : 0;

    if (conAudio) pausarWav(true);

    if (!buscarVideoMs(destinoMs)) {
      if (conAudio) pausarWav(false);
      return false;
    }

    if (conAudio) {
      buscarWavMs(destinoMs);
      if (mantenerPausa) pausarWav(true);
    }

    frameIndice = (uint32_t)(((uint64_t)destinoMs * FPS_AV) / 1000ULL);
    if (frameIndice >= idxCab.totalFrames) frameIndice = idxCab.totalFrames - 1;

    // Reloj relativo al último seek: evita overflow en películas > 71 minutos.
    frameBaseReloj = frameIndice;
    relojAV = micros();

    pausado = mantenerPausa;
    tft.fillScreen(TFT_BLACK);
    mostrarBarraVideo(destinoMs, duracionMs, estado);
    barraHasta = mantenerPausa ? 0 : millis() + BARRA_SEEK_MS;
    return true;
  };

  auto buscarTiempo = [&](int deltaSeg, bool mantenerPausa) {
    int64_t destino = (int64_t)tiempoActualMs() + (int64_t)deltaSeg * 1000LL;
    if (destino < 0) destino = 0;
    if (destino > (int64_t)duracionMs - 250LL)
      destino = duracionMs > 250 ? (int64_t)duracionMs - 250LL : 0;

    char estado[16];
    snprintf(estado, sizeof(estado), "%+d s", deltaSeg);
    if (irATiempo((uint32_t)destino, mantenerPausa, estado)) {
      Serial.printf("[GALERIA][SEEK] %+d s -> %llu ms\n",
                    deltaSeg, (unsigned long long)destino);
    }
  };

  auto pasoHold = [&](bool izquierda, bool derecha, bool mantenerPausa) -> bool {
    int dir = izquierda ? -1 : (derecha ? +1 : 0);
    uint32_t ahora = millis();

    if (dir == 0) {
      direccionHold = 0;
      holdInicio = 0;
      holdUltimoPaso = 0;
      return false;
    }

    if (dir != direccionHold) {
      direccionHold = dir;
      holdInicio = ahora;
      holdUltimoPaso = ahora;
      buscarTiempo(dir * 10, mantenerPausa); // toque inicial: +/-10 s
      return true;
    }

    uint32_t sostenido = ahora - holdInicio;
    if (sostenido < 700) return false; // pequeña espera antes de repetir

    int salto = 10;
    uint32_t intervalo = 420;
    if (sostenido >= 4000) {
      salto = 60;
      intervalo = 260;
    } else if (sostenido >= 1500) {
      salto = 30;
      intervalo = 320;
    }

    if (ahora - holdUltimoPaso >= intervalo) {
      holdUltimoPaso = ahora;
      buscarTiempo(dir * salto, mantenerPausa);
      return true;
    }
    return false;
  };

  // -----------------------------------------------------------------------
  // Reanudar / reiniciar
  // -----------------------------------------------------------------------
  uint32_t resumeMs = 0;
  if (cargarResume(rutaResume, tamVideo, duracionMs, resumeMs)) {
    int eleccion = preguntarReanudar(resumeMs, duracionMs);
    if (eleccion < 0) {
      idx.close();
      f.close();
      if (conAudio || wavI2STomado) detenerYRestaurarWav();
      reproduciendo = false;
      TJpgDec.setSwapBytes(false);
      TJpgDec.setJpgScale(1);
      TJpgDec.setCallback(jpgDibujarBloque);
      dibujarMenu();
      return;
    } else if (eleccion == 1) {
      irATiempo(resumeMs, false, "REANUDAR");
      barraHasta = millis() + BARRA_SEEK_MS;
    } else {
      SD_MMC.remove(rutaResume);
      f.seek(0);
      bytesBuffer = 0;
      frameIndice = 0;
      if (conAudio) buscarWavMs(0);
    }
  }

  // -----------------------------------------------------------------------
  // Reproducción
  // -----------------------------------------------------------------------
  while (buscarFrame(f, inicio, longitud)) {
    const bool aAhora = digitalRead(PIN_OK) == LOW;
    const bool izqAhora = digitalRead(PIN_LEFT) == LOW;
    const bool derAhora = digitalRead(PIN_RIGHT) == LOW;
    const bool arrAhora = digitalRead(PIN_UP) == LOW;
    const bool abaAhora = digitalRead(PIN_DOWN) == LOW;

    const bool aPress = aAhora && !aPrev;
    const bool arrPress = arrAhora && !arrPrev;
    const bool abaPress = abaAhora && !abaPrev;

    aPrev = aAhora;
    arrPrev = arrAhora;
    abaPrev = abaAhora;
    izqPrev = izqAhora;
    derPrev = derAhora;

    if (digitalRead(PIN_PAUSA) == LOW) break;

    if (aPress) {
      pausado = true;

      // Guardar solo cuando el usuario pausa. Evita escrituras a la SD
      // durante la reproducción, que podían provocar tirones en el vídeo.
      guardarResume(rutaResume, tamVideo, tiempoActualMs(), duracionMs);

      if (conAudio) pausarWav(true);
      mostrarPausaVideo(true, tiempoActualMs(), duracionMs);

      // Al entrar a pausa, reiniciar hold para que un botón que quedó pulsado
      // no dispare inmediatamente.
      direccionHold = 0;
      holdInicio = 0;
      holdUltimoPaso = 0;

      const uint32_t pausaInicio = micros();
      while (pausado) {
        if (digitalRead(PIN_PAUSA) == LOW) {
          pausado = false;
          break;
        }

        const bool a2 = digitalRead(PIN_OK) == LOW;
        const bool up2 = digitalRead(PIN_UP) == LOW;
        const bool dn2 = digitalRead(PIN_DOWN) == LOW;
        const bool l2 = digitalRead(PIN_LEFT) == LOW;
        const bool r2 = digitalRead(PIN_RIGHT) == LOW;

        if (a2 && !aPrev) {
          pausado = false;
          if (conAudio) pausarWav(false);
          // Reiniciar el reloj desde el frame actual. Esto también funciona
          // si durante la pausa hubo uno o varios seeks.
          frameBaseReloj = frameIndice;
          relojAV = micros();
          tft.fillScreen(TFT_BLACK);
          barraHasta = millis() + BARRA_SEEK_MS;
        }

        if (up2 && !arrPrev) ajustarVolumenVideo(+1);
        if (dn2 && !abaPrev) ajustarVolumenVideo(-1);

        if (pasoHold(l2, r2, true)) {
          mostrarPausaVideo(true, tiempoActualMs(), duracionMs);
        }

        aPrev = a2;
        arrPrev = up2;
        abaPrev = dn2;
        izqPrev = l2;
        derPrev = r2;
        delay(10);
      }

      direccionHold = 0;
      holdInicio = 0;
      holdUltimoPaso = 0;

      if (digitalRead(PIN_PAUSA) == LOW) break;
      continue;
    }

    if (arrPress) ajustarVolumenVideo(+1);
    if (abaPress) ajustarVolumenVideo(-1);

    if (pasoHold(izqAhora, derAhora, false)) {
      continue;
    }

    bool mostrar = true;

    if (conAudio) {
      if (tareaWav == nullptr && !wavTerminado) {
        // Primer frame del segmento antes de arrancar el PCM.
        TJpgDec.drawJpg(0, 0, buffer + inicio, longitud);
        framesMostrados++;

        if ((int32_t)(volumenOSDHasta - millis()) > 0) mostrarVolumenVideo();
        if ((int32_t)(barraHasta - millis()) > 0)
          mostrarBarraVideo(tiempoActualMs(), duracionMs, nullptr);

        consumirFrame(inicio, longitud);

        relojAV = micros();
        frameBaseReloj = frameIndice;
        if (!arrancarWav()) {
          detenerYRestaurarWav();
          conAudio = false;
        } else {
          Serial.printf("[GALERIA][AV] inicio %u FPS frame=%u\n",
                        (unsigned)FPS_AV, (unsigned)frameIndice);
        }
        frameIndice++;
        yield();
        continue;
      }

      const uint32_t framesDesdeBase = frameIndice - frameBaseReloj;
      const uint32_t objetivo =
          (uint32_t)(((uint64_t)framesDesdeBase * 1000000ULL) / FPS_AV);
      uint32_t transcurrido = (uint32_t)(micros() - relojAV);

      if (transcurrido > objetivo + FRAME_US_AV) {
        mostrar = false;
        framesSaltados++;
      } else {
        while ((uint32_t)(micros() - relojAV) < objetivo) {
          if (digitalRead(PIN_PAUSA) == LOW) break;
          delay(1);
        }
      }
    }

    if (mostrar) {
      TJpgDec.drawJpg(0, 0, buffer + inicio, longitud);
      framesMostrados++;
      if ((int32_t)(volumenOSDHasta - millis()) > 0) mostrarVolumenVideo();
      if ((int32_t)(barraHasta - millis()) > 0)
        mostrarBarraVideo(tiempoActualMs(), duracionMs, nullptr);
    }

    consumirFrame(inicio, longitud);
    frameIndice++;

    if (digitalRead(PIN_PAUSA) == LOW) break;
    yield();
  }

  // Guardar la posición final si salimos antes del 95%; si se terminó,
  // guardarResume() borra automáticamente el marcador.
  guardarResume(rutaResume, tamVideo, tiempoActualMs(), duracionMs);

  idx.close();
  f.close();
  if (conAudio || wavI2STomado) detenerYRestaurarWav();

  if (frameIndice > 0 && (framesSaltados > 0 || conAudio)) {
    Serial.printf("[GALERIA][AV] frames=%u mostrados=%u saltados=%u\n",
                  (unsigned)frameIndice, (unsigned)framesMostrados,
                  (unsigned)framesSaltados);
  }

  reproduciendo = false;
  TJpgDec.setSwapBytes(false);
  TJpgDec.setJpgScale(1);
  TJpgDec.setCallback(jpgDibujarBloque);

  while (digitalRead(PIN_PAUSA) == LOW || digitalRead(PIN_OK) == LOW) delay(5);
  pausaAnterior = false;
  dibujarMenu();
}


void entrar() {
  bytesBuffer = 0;
  mostrandoTutorial = false;
  vista = VISTA_SECCIONES;
  seccionSeleccionada = 0;
  pausaAnterior = (digitalRead(PIN_PAUSA) == LOW);
  dibujarSelectorSecciones();
}

void salir() {
  if (tareaWav != nullptr || wavI2STomado) detenerYRestaurarWav();

  if (buffer) {
    free(buffer);
    buffer = nullptr;
  }
  bytesBuffer = 0;
  reproduciendo = false;
  mostrandoTutorial = false;
  vista = VISTA_SECCIONES;

  // TJpg_Decoder conserva callback y byte-swap entre llamadas. Dejarlos como
  // estaban durante el vídeo corrompe el wallpaper al reconstruir el menú.
  TJpgDec.setSwapBytes(false);
  TJpgDec.setJpgScale(1);
  TJpgDec.setCallback(jpgDibujarBloque);

  gameState = STATE_MENU;
  volumenPanelAbierto = false;
  invalidarMenuBase();

  // Limpia cualquier último fotograma antes de regenerar completamente el
  // launcher y su wallpaper desde la SD.
  tft.fillScreen(TFT_BLACK);
  canvas.fillScreen(TFT_BLACK);
  ::drawMenu();
}

void actualizar(bool okPresionado) {
  static Direccion actual = DIR_NINGUNO;
  static Direccion anterior = DIR_NINGUNO;
  static unsigned long ultimoCambio = 0;

  const bool pausa = digitalRead(PIN_PAUSA) == LOW;
  const bool pausaPulsada = pausa && !pausaAnterior;
  pausaAnterior = pausa;

  if (mostrandoTutorial) {
    if (pausaPulsada || okPresionado) {
      mostrandoTutorial = false;
      beepMenuMove();
      dibujarMenu();
    }
    return;
  }

  if (vista == VISTA_FOTO_ABIERTA) {
    if (pausaPulsada || okPresionado) {
      beepMenuMove();
      dibujarMenuFotos();
    }
    return;
  }

  Direccion leida = clasificarCruceta(leerCrucetaADC());
  if (leida != actual) {
    actual = leida;
    ultimoCambio = millis();
  }

  const bool eventoDireccion =
      (millis() - ultimoCambio) > DEBOUNCE_CRUCETA_MS && actual != anterior;

  if (eventoDireccion) {
    anterior = actual;

    if (vista == VISTA_SECCIONES &&
        (actual == DIR_IZQUIERDA || actual == DIR_DERECHA)) {
      seccionSeleccionada = 1 - seccionSeleccionada;
      beepMenuMove();
      dibujarSelectorSecciones();
    } else if (vista == VISTA_VIDEOS) {
      const int total = totalElementos();
      if (total > 0 && (actual == DIR_ARRIBA || actual == DIR_ABAJO)) {
        const int seleccionAnterior = seleccion;
        const int primeroAnterior = primeroVisible;

        if (actual == DIR_ARRIBA) seleccion = (seleccion + total - 1) % total;
        else seleccion = (seleccion + 1) % total;

        ajustarScroll();
        beepMenuMove();
        actualizarCursorMenu(seleccionAnterior, primeroAnterior);
      }
    } else if (vista == VISTA_FOTOS && cantidadFotos > 0 &&
               (actual == DIR_ARRIBA || actual == DIR_ABAJO)) {
      if (actual == DIR_ARRIBA)
        seleccionFoto = (seleccionFoto + cantidadFotos - 1) % cantidadFotos;
      else
        seleccionFoto = (seleccionFoto + 1) % cantidadFotos;

      ajustarScrollFotos();
      beepMenuMove();
      dibujarMenuFotos();
    }
  }

  if (pausaPulsada) {
    if (vista == VISTA_SECCIONES) {
      salir();
    } else {
      beepMenuMove();
      dibujarSelectorSecciones();
    }
    return;
  }

  if (!okPresionado) return;

  beepMenuMove();

  if (vista == VISTA_SECCIONES) {
    if (seccionSeleccionada == 0) {
      cargarFotos();
      dibujarMenuFotos();
    } else {
      if (!buffer) buffer = (uint8_t*)malloc(TAM_BUFFER);
      if (!buffer) {
        Serial.println("Galeria: RAM insuficiente para buffer MJPEG");
        beepDenegado();
        return;
      }
      bytesBuffer = 0;
      cargarLista();
      vista = VISTA_VIDEOS;
      dibujarMenu();
    }
  } else if (vista == VISTA_VIDEOS) {
    reproducirSeleccionado();
  } else if (vista == VISTA_FOTOS) {
    abrirFotoSeleccionada();
  }
}

} // namespace Galeria


// ════════════════════════════════════════════════════════════
//  ASTRONAUTA — módulo integrado al launcher
//  Usa el TFT, canvas, buzzer, cruceta y botón PAUSA compartidos.
// ════════════════════════════════════════════════════════════
namespace Astronauta {
// ─────────────────────────────────────────────
//  CONSTANTES DE PANTALLA
// ──────────────────────────────────────────────
#define PQ_SCREEN_W   320
#define PQ_SCREEN_H   240


  
// ──────────────────────────────────────────────
//  PINES (ESP32-S3-Zero)
//  La TFT ya usa MOSI 11 / SCLK 12 / CS 10 / DC 9 / RST 8,
//  así que estos tres pines libres y seguros del S3-Zero
//  reemplazan a los 27/35/25 del ESP32 DevKit V1. Se evitan
//  GPIO0 (BOOT), GPIO21 (LED RGB), GPIO33-37 (PSRAM octal),
//  GPIO43/44 (UART0) y GPIO45/46 (estrapeo).
// ──────────────────────────────────────────────
#define PQ_PIN_BTN_LEFT    2      // Botón físico izquierdo (antes GPIO4)
#define PQ_PIN_BUZZER      BUZZER_PIN  // usa el buzzer compartido del launcher
#define PQ_BUZZER_CH       0   // canal LEDC
#define PQ_BUZZER_RES      8   // resolución bits

// ──────────────────────────────────────────────
//  BOTÓN DERECHO = dirección DERECHA de la cruceta (D-pad)
//  La cruceta manda 4 botones por un solo pin ADC mediante una
//  escalera de resistencias (ver cruceta_esp32s3_tft.ino). Aquí
//  solo nos interesa detectar cuándo se presiona "DERECHA".
// ──────────────────────────────────────────────
#define PQ_PIN_ADC_CRUCETA     1   // mismo pin que ya se usaba para semilla aleatoria
#define PQ_MUESTRAS_CRUCETA    8

const int PQ_ADC_NINGUNO_CRZ   = 4095;
const int PQ_ADC_DERECHA_CRZ   = 3250;  // 33k
const int PQ_ADC_IZQUIERDA_CRZ = 2100;  // 10k
const int PQ_ADC_ABAJO_CRZ     = 920;   // 3.3k
const int PQ_ADC_ARRIBA_CRZ    = 315;   // 1k

const int PQ_UMBRAL_DER_TECHO  = (PQ_ADC_NINGUNO_CRZ + PQ_ADC_DERECHA_CRZ) / 2;
const int PQ_UMBRAL_DER_PISO   = (PQ_ADC_DERECHA_CRZ + PQ_ADC_IZQUIERDA_CRZ) / 2;

// ──────────────────────────────────────────────
//  BUZZER – usa LEDC del ESP32 (igual que el
//  juego de referencia, compatible v2 y v3)
// ──────────────────────────────────────────────
void buzzerOn(int freq) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  arcadeBuzzerWriteStable(PQ_PIN_BUZZER, freq);
#else
  arcadeBuzzerWriteStable(PQ_BUZZER_CH, freq);
  ledcWrite(PQ_BUZZER_CH, arcadeVolumenDuty());
#endif
}
void buzzerOff() {
  // IMPORTANTE con MAX98357A:
  // antes Astronauta apagaba el buzzer físico escribiendo duty=0 al LEDC.
  // Ahora el sonido sale por I2S, así que ledcWrite() ya NO controla nada.
  // Hay que detener explícitamente el backend de audio.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  arcadeBuzzerWriteStable(PQ_PIN_BUZZER, 0);
#else
  arcadeBuzzerWriteStable(PQ_BUZZER_CH, 0);
#endif
}

static constexpr const char *PQ_SFX_ATERRIZAJE = "/sfx/astronauta/aterrizaje.wav";
static constexpr const char *PQ_SFX_EXPLOSION  = "/sfx/astronauta/explosion.wav";
static constexpr const char *PQ_SFX_INICIO     = "/sfx/astronauta/inicio.wav";
static constexpr const char *PQ_SFX_PROPULSOR  = "/sfx/astronauta/propulsor.wav";

static bool sfxSDIniciado(SFXSD::Resultado r) {
  return r == SFXSD::INICIADO;
}

void soundLandFallback() {
  buzzerOn(523); delay(60);
  buzzerOn(659); delay(60);
  buzzerOn(784); delay(100);
  buzzerOff();
}
void soundCrashFallback() {
  for (int f = 600; f > 100; f -= 40) {
    buzzerOn(f); delay(15);
  }
  buzzerOff();
}
void soundThrustFallback() {
  buzzerOn(180); delay(30);
  buzzerOff();
}
void soundStartFallback() {
  int mel[] = {392, 523, 659, 784};
  int dur[] = {80, 80, 80, 160};
  for (int i = 0; i < 4; i++) {
    buzzerOn(mel[i]); delay(dur[i]);
    buzzerOff(); delay(20);
  }
}

void soundLand() {
  // Cada acción nueva reemplaza el SFX anterior para que nunca quede muteada.
  const SFXSD::Resultado r = SFXSD::reproducir(PQ_SFX_ATERRIZAJE, true);
  if (!sfxSDIniciado(r)) soundLandFallback();
}
void soundCrash() {
  const SFXSD::Resultado r = SFXSD::reproducir(PQ_SFX_EXPLOSION, true);
  if (!sfxSDIniciado(r)) soundCrashFallback();
}
void soundThrust() {
  const SFXSD::Resultado r = SFXSD::reproducir(PQ_SFX_PROPULSOR, true);
  if (!sfxSDIniciado(r)) soundThrustFallback();
}
void soundStart() {
  const SFXSD::Resultado r = SFXSD::reproducir(PQ_SFX_INICIO, true);
  if (!sfxSDIniciado(r)) soundStartFallback();
}

// ──────────────────────────────────────────────
//  MÚSICA DE LA PANTALLA DE TÍTULO (no bloqueante)
//  El loop principal corre a ~60fps sin usar delay(),
//  así que esta melodía avanza nota a nota comparando
//  millis() en cada frame, en vez de bloquear con delay().
// ──────────────────────────────────────────────
const int BPM_TITULO         = 111;
const int T_NEGRA             = 60000 / BPM_TITULO;
const int T_CORCHEA           = T_NEGRA / 2;
const int T_BLANCA            = T_NEGRA * 2;
const int T_REDONDA           = T_NEGRA * 4;
const int T_NEGRA_PUNTO       = T_NEGRA + T_CORCHEA;
const int T_REDONDA_PUNTO     = T_REDONDA + T_BLANCA;

#define SIL 0

#define NOTE_D4  294
#define NOTE_E4  330
#define NOTE_FS4 370
#define NOTE_G4  392
#define NOTE_A4  440
#define NOTE_B4  494
#define NOTE_CS5 554

#define NOTE_D5  587
#define NOTE_E5  659
#define NOTE_FS5 740
#define NOTE_G5  784
#define NOTE_A5  880
#define NOTE_B5  988
#define NOTE_CS6 1109

struct NotaTitulo { int frecuencia; int duracion; };

NotaTitulo melodiaTitulo[] = {
  // Anacrusa
  {NOTE_A4, T_CORCHEA},

  // Compás 1
  {NOTE_D5, T_CORCHEA}, {NOTE_A4, T_CORCHEA}, {NOTE_D5, T_CORCHEA}, {NOTE_E5, T_CORCHEA},
  {NOTE_CS5, T_CORCHEA}, {NOTE_A4, T_CORCHEA}, {NOTE_CS5, T_CORCHEA}, {NOTE_D5, T_CORCHEA},

  // Compás 2
  {NOTE_B4, T_CORCHEA}, {NOTE_A4, T_CORCHEA}, {NOTE_A4, T_CORCHEA}, {NOTE_G4, T_CORCHEA},
  {NOTE_G4, T_CORCHEA}, {NOTE_FS4, T_CORCHEA}, {NOTE_E4, T_CORCHEA}, {NOTE_A4, T_CORCHEA},

  // Compás 3
  {NOTE_D5, T_CORCHEA}, {NOTE_A4, T_CORCHEA}, {NOTE_D5, T_CORCHEA}, {NOTE_E5, T_CORCHEA},
  {NOTE_CS5, T_CORCHEA}, {NOTE_A4, T_CORCHEA}, {NOTE_CS5, T_CORCHEA}, {NOTE_D5, T_CORCHEA},

  // Compás 4 con ligadura al siguiente Re
  {NOTE_B4, T_CORCHEA}, {NOTE_A4, T_CORCHEA}, {NOTE_A4, T_CORCHEA}, {NOTE_G4, T_CORCHEA},
  {NOTE_G4, T_CORCHEA}, {NOTE_FS4, T_CORCHEA}, {NOTE_E4, T_CORCHEA},
  {NOTE_D4, T_NEGRA_PUNTO},

  // Parte aguda
  {NOTE_FS5, T_NEGRA},
  {NOTE_G5, T_BLANCA},

  {NOTE_E5, T_NEGRA},
  {NOTE_G5, T_NEGRA},
  {NOTE_FS5, T_BLANCA},

  {SIL, T_NEGRA},
  {NOTE_G5, T_BLANCA},

  {NOTE_E5, T_NEGRA},
  {NOTE_G5, T_NEGRA},
  {NOTE_FS5, T_BLANCA},

  // Subida aguda
  {SIL, T_NEGRA},
  {NOTE_B5, T_NEGRA},
  {NOTE_B5, T_NEGRA},
  {NOTE_CS6, T_NEGRA},

  {NOTE_CS6, T_NEGRA_PUNTO},
  {NOTE_A5, T_CORCHEA},
  {NOTE_FS5, T_BLANCA},

  {SIL, T_NEGRA},
  {NOTE_B5, T_NEGRA},
  {NOTE_B5, T_NEGRA},
  {NOTE_CS6, T_NEGRA},

  {NOTE_CS6, T_NEGRA},
  {NOTE_B5, T_CORCHEA},
  {NOTE_A5, T_CORCHEA},
  {NOTE_FS5, T_BLANCA},

  // Regreso a octava original
  {NOTE_D4, T_BLANCA},
  {NOTE_FS4, T_BLANCA},

  {NOTE_E4, T_NEGRA_PUNTO},
  {NOTE_FS4, T_CORCHEA},
  {NOTE_FS4, T_BLANCA},

  {SIL, T_NEGRA},
  {NOTE_D4, T_NEGRA},
  {NOTE_FS4, T_NEGRA},
  {NOTE_A4, T_NEGRA},

  {NOTE_E4, T_NEGRA_PUNTO},
  {NOTE_FS4, T_CORCHEA},
  {NOTE_FS4, T_BLANCA},

  {NOTE_B4, T_CORCHEA},
  {NOTE_FS4, T_CORCHEA},
  {NOTE_B4, T_CORCHEA},
  {NOTE_A4, T_CORCHEA},
  {NOTE_FS4, T_NEGRA_PUNTO},
  {NOTE_E4, T_CORCHEA},

  {NOTE_E4, T_NEGRA_PUNTO},
  {NOTE_FS4, T_CORCHEA},
  {NOTE_B4, T_NEGRA_PUNTO},
  {NOTE_CS5, T_CORCHEA},

  {NOTE_D5, T_NEGRA_PUNTO},
  {NOTE_E5, T_CORCHEA},
  {NOTE_D5, T_CORCHEA},
  {NOTE_CS5, T_CORCHEA},
  {NOTE_B4, T_CORCHEA},
  {NOTE_A4, T_CORCHEA},

  {NOTE_B4, T_REDONDA},

  // 2 compases de silencio antes de repetir
  {SIL, T_REDONDA},
  {SIL, T_REDONDA}
};

const int totalNotasTitulo = sizeof(melodiaTitulo) / sizeof(melodiaTitulo[0]);

int      tituloNotaIdx     = 0;
uint32_t tituloNotaInicio  = 0;
bool     tituloTonoActivo  = false;  // true durante el 90% "sonoro" de la nota

// Reinicia la melodía desde la primera nota y la empieza a sonar.
void resetTitleMusic() {
  tituloNotaIdx    = 0;
  tituloNotaInicio = millis();
  tituloTonoActivo = true;
  int f = melodiaTitulo[0].frecuencia;
  if (f != SIL) buzzerOn(f); else buzzerOff();
}

// Avanza la melodía de la pantalla de título; se llama una vez por frame
// mientras gameState == GS_TITLE. No usa delay(), así los botones y el
// dibujo de pantalla se siguen leyendo con normalidad.
void updateTitleMusic() {
  NotaTitulo &n = melodiaTitulo[tituloNotaIdx];
  uint32_t elapsed = millis() - tituloNotaInicio;
  int sonoro = (int)(n.duracion * 0.9f);   // duración "con sonido" (staccato)

  if (tituloTonoActivo) {
    if (elapsed >= (uint32_t)sonoro) {
      buzzerOff();
      tituloTonoActivo = false;
    }
  } else if (elapsed >= (uint32_t)n.duracion) {
    tituloNotaIdx    = (tituloNotaIdx + 1) % totalNotasTitulo;
    tituloNotaInicio = millis();
    tituloTonoActivo = true;
    int f = melodiaTitulo[tituloNotaIdx].frecuencia;
    if (f != SIL) buzzerOn(f); else buzzerOff();
  }
}

// ──────────────────────────────────────────────
//  PALETA DE COLORES (tema Marte)
// ──────────────────────────────────────────────
#define C_SKY       TFT_BLACK
#define C_GROUND    0xCA41
#define C_MOUNTAIN  0xC200
#define C_MTN_EDGE  0xFB60
#define C_PLATFORM  0x8C51
#define C_PLT_TOP   0xCE59
#define C_SUIT      0xFFFF
#define C_VISOR     0x04FF
#define C_THRUSTER  0xFD20
#define C_FLAME1    0xFFE0
#define C_FLAME2    0xF800
#define C_FUEL_BG   0x2104
#define C_FUEL_OK   0x07E0
#define C_FUEL_LOW  0xFD20
#define C_FUEL_CRIT 0xF800
#define C_HUD_TEXT  0xFFFF
#define C_STREAK    0xFD20
#define C_EXPLODE1  0xFFE0
#define C_EXPLODE2  0xFD20
#define C_EXPLODE3  0xF800
#define C_SMOKE     0x8410
#define C_BLACKHOLE  0x18C3
#define C_BH_RING1   0x781F
#define C_BH_RING2   0x07FF

// Paleta de montañas: cada entrada es {color base, color de realce/luz}
// para dar variedad de "roca marciana" en vez de un triángulo plano.
const uint16_t MTN_BASE[4] = {0xC200, 0x6B2B, 0x7965, 0xBD0F};
const uint16_t MTN_HI[4]   = {0xFB60, 0xAD12, 0xB2E9, 0xE695};
#define C_SNOW      0xE75E   // casquete de hielo para picos altos

// ──────────────────────────────────────────────
//  FÍSICA
// ──────────────────────────────────────────────
#define GRAVITY          0.12f
#define THRUST_UP        0.22f
// Propulsores laterales: ahora empujan en diagonal (más vertical que
// horizontal) para que no sea tan fácil "deslizarse" en línea recta.
#define THRUST_SIDE_H    0.10f   // componente horizontal
#define THRUST_SIDE_V    0.18f   // componente vertical (empuje hacia arriba)
#define FRICTION         0.92f
#define MAX_SAFE_LAND    2.8f
#define MAX_VX           3.5f
#define MAX_VY           6.0f
#define FUEL_MAX         100.0f
// El tanque se vacía en ~2s (120 frames a 60fps) si se mantiene UN solo
// propulsor lateral presionado sin soltar, igual que en el Mars Mars original.
#define FUEL_BURN_RATE   (FUEL_MAX / 120.0f)

// ──────────────────────────────────────────────
//  MUNDO
// ──────────────────────────────────────────────
#define WORLD_GROUND_Y   (PQ_SCREEN_H - 35)
#define PLATFORM_H       6
#define MAX_PLATFORMS    8
#define MAX_MOUNTAINS    6
#define MAX_PARTICLES    40
#define MAX_STARS        35
#define MAX_BLACK_HOLES  5
#define SCROLL_MARGIN    100

// ──────────────────────────────────────────────
//  ESTADOS
// ──────────────────────────────────────────────
enum GameState { GS_TITLE, GS_PLAYING, GS_EXPLODING, GS_REWARD, GS_RESPAWN };

// ──────────────────────────────────────────────
//  ESTRUCTURAS
// ──────────────────────────────────────────────
struct Star     { int16_t x, y; uint8_t brightness; };
struct Platform { float worldX; int16_t y; uint16_t w;
                  bool visited; bool isCurrent; };
struct Mountain { float worldX; int16_t baseY, h, w; uint8_t style;
                  uint8_t seed; uint8_t colorIdx; };
struct BlackHole { float worldX; int16_t y; uint8_t r; bool active; };
struct Astronaut {
  float x, y, worldX, vx, vy, fuel;
  bool  onGround, thrustLeft, thrustRight, alive;
  int   width = 10, height = 16;
  // Frames de gracia tras respawn: colisiones desactivadas temporalmente
  uint8_t graceTicks = 0;
};
struct Particle {
  float x, y, vx, vy;
  uint16_t color;
  uint8_t  life, maxLife, type;
  bool     active;
};

// ──────────────────────────────────────────────
//  GLOBALES
// ──────────────────────────────────────────────
TFT_eSprite &spr = ::canvas;

GameState gameState = GS_TITLE;
uint8_t recompensaActualPQ = RECOMPENSA_TOTAL;
bool botonesLiberadosRecompensaPQ = false;
Astronaut astro;
Platform  platforms[MAX_PLATFORMS];
Mountain  mountains[MAX_MOUNTAINS];
BlackHole blackHoles[MAX_BLACK_HOLES];
Particle  particles[MAX_PARTICLES];
Star      stars[MAX_STARS];

float    cameraX = 0;
int      streak  = 0;
int      lastPlatformIdx = 0;
float    worldGen = 0;
uint32_t lastFrame  = 0;
uint32_t stateTimer = 0;

bool btnLeft_prev = false, btnRight_prev = false;

// Impide que el botón usado para abrir el juego desde el launcher
// sea interpretado como una pulsación para saltar la pantalla de título.
bool controlesTituloLiberados = false;

// Menú de pausa.
enum EstadoPausaPQ { PQ_PAUSA_MENU, PQ_PAUSA_VOLUMEN };
EstadoPausaPQ estadoPausaPQ = PQ_PAUSA_MENU;
uint8_t opcionPausaPQ = 0;  // 0 = SALIR, 1 = VOLUMEN
bool pausaActivaPQ = false;
bool pausaAnteriorPQ = false;
bool okAnteriorPausaPQ = false;
int direccionAnteriorPausaPQ = 0;

// Temperatura mostrada únicamente en la pausa.
int temperaturaPausaPQ = 0;
uint32_t ultimaTemperaturaPausaPQ = 0;

// ──────────────────────────────────────────────
//  PROTOTIPOS
// ──────────────────────────────────────────────
void  initWorld();
void  generateNextChunk();
void  updatePhysics();
void  checkCollisions();
void  killAstronaut();
void  respawnAstronaut();
void  spawnParticles(float x, float y, uint8_t type, uint8_t count);
void  updateParticles();
void  drawScene(bool pushToScreen = true);
void  drawBackground();
void  drawMountains();
void  drawPlatforms();
void  drawBlackHoles();
void  drawAstronaut();
void  drawParticles();
void  drawHUD();
void  drawTitleScreen();
void  drawExplosionFrame();
float worldToScreen(float wx);
float mountainHeightAt(const Mountain &m, float cx);
int   findFreePlatformSlot();
int   findFreeMountainSlot();
int   findFreeBlackHoleSlot();
bool  mountainCoversPlatformTooMuch(float x, int w, int h, uint8_t style, uint8_t seed);
void  updateBlackHoleForces();
void  readButtons(bool &left, bool &right);
void  resetTitleMusic();
void  updateTitleMusic();
void  cerrar();
int   leerDireccionPausaPQ();
void  dibujarMenuPausaPQ();
void  manejarMenuPausaPQ();

// ══════════════════════════════════════════════
//  MENÚ DE PAUSA DE ASTRONAUTA
// ══════════════════════════════════════════════
int leerDireccionPausaPQ() {
  if (digitalRead(PIN_UP) == LOW)    return 1;
  if (digitalRead(PIN_DOWN) == LOW)  return 2;
  if (digitalRead(PIN_LEFT) == LOW)  return 3;
  if (digitalRead(PIN_RIGHT) == LOW) return 4;
  return 0;
}
void dibujarMenuPausaPQ() {
  spr.fillSprite(TFT_BLACK);
  spr.setTextSize(1);
  spr.setTextDatum(MC_DATUM);

  spr.setTextColor(TFT_YELLOW, TFT_BLACK);
  spr.drawString(estadoPausaPQ == PQ_PAUSA_MENU ? "MENU" : "SONIDO",
                 PQ_SCREEN_W / 2, 34, 4);

  if (estadoPausaPQ == PQ_PAUSA_MENU) {
    const char* opciones[2] = {"SALIR", "SONIDO"};

    for (int i = 0; i < 2; ++i) {
      const bool seleccionado = (i == opcionPausaPQ);
      const uint16_t fondo = seleccionado ? TFT_BLUE : 0x2104;
      const uint16_t texto = seleccionado ? TFT_WHITE : TFT_LIGHTGREY;
      const int y = 82 + i * 58;

      spr.fillRoundRect(82, y, 156, 44, 9, fondo);
      spr.drawRoundRect(82, y, 156, 44, 9,
                        seleccionado ? TFT_CYAN : TFT_DARKGREY);
      spr.setTextColor(texto, fondo);
      spr.drawString(opciones[i], PQ_SCREEN_W / 2, y + 22, 2);
    }

    spr.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    spr.drawString("Arr/Ab: elegir   A: aceptar",
                   PQ_SCREEN_W / 2, 207, 1);
    spr.drawString("MENU: continuar",
                   PQ_SCREEN_W / 2, 224, 1);
  } else {
    const int x = 48;
    const int y = 122;
    const int w = 224;
    const int h = 24;

    spr.drawRoundRect(x, y, w, h, 6, TFT_WHITE);

    const int relleno = (w - 4) * volumenPaso / VOLUME_PASOS_MAX;
    if (relleno > 0) {
      spr.fillRoundRect(x + 2, y + 2, relleno, h - 4, 4, TFT_GREEN);
    }

    spr.setTextColor(TFT_YELLOW, TFT_BLACK);
    spr.drawString(String(arcadeNombreVolumen()),
                   PQ_SCREEN_W / 2, 174, 4);

    spr.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    spr.drawString("Izq/Der: ajustar",
                   PQ_SCREEN_W / 2, 210, 1);
    spr.drawString("MENU: volver",
                   PQ_SCREEN_W / 2, 226, 1);
  }

  const uint32_t ahora = millis();
  if (ultimaTemperaturaPausaPQ == 0 ||
      ahora - ultimaTemperaturaPausaPQ >= 10000UL) {
    ultimaTemperaturaPausaPQ = ahora;
    temperaturaPausaPQ = (int)roundf(temperatureRead());
  }

  spr.setTextDatum(BL_DATUM);
  spr.setTextFont(1);
  spr.setTextSize(1);
  spr.setTextColor(TFT_WHITE, TFT_BLACK);
  spr.drawString(String("TEM. ") + String(temperaturaPausaPQ) + (char)247,
                 8, PQ_SCREEN_H - 5);

  perfilPushSprite(0, 0);

  spr.setTextDatum(TL_DATUM);
  spr.setTextFont(1);
  spr.setTextSize(1);
}

void manejarMenuPausaPQ() {
  // Menú rehecho con la misma estructura funcional que Cuatete:
  // esta función SOLO navega/acepta. El botón MENU se procesa en update().
  const int direccion = leerDireccionPausaPQ();
  const bool eventoDireccion =
      direccion != 0 && direccionAnteriorPausaPQ == 0;
  direccionAnteriorPausaPQ = direccion;

  const bool okAhora = (digitalRead(PQ_PIN_BTN_LEFT) == LOW); // GPIO2
  const bool okPulsado = okAhora && !okAnteriorPausaPQ;
  okAnteriorPausaPQ = okAhora;

  if (estadoPausaPQ == PQ_PAUSA_MENU) {
    if (eventoDireccion && (direccion == 1 || direccion == 2)) {
      opcionPausaPQ = (opcionPausaPQ == 0) ? 1 : 0;
      dibujarMenuPausaPQ();
    }

    if (okPulsado) {
      if (opcionPausaPQ == 0) {
        buzzerOff();
        astro.thrustLeft = false;
        astro.thrustRight = false;
        cerrar();
        pausaActivaPQ = false;
        volverAlMenu();
        return;
      }

      estadoPausaPQ = PQ_PAUSA_VOLUMEN;
      direccionAnteriorPausaPQ = 0;
      dibujarMenuPausaPQ();
    }
  } else if (eventoDireccion) {
    if (direccion == 4 && volumenPaso < VOLUME_PASOS_MAX) {
      ++volumenPaso;
      aplicarVolumenChopper();
      dibujarMenuPausaPQ();
    } else if (direccion == 3 && volumenPaso > 0) {
      --volumenPaso;
      aplicarVolumenChopper();
      dibujarMenuPausaPQ();
    }
  }
}
// ══════════════════════════════════════════════
//  SETUP
// ══════════════════════════════════════════════
void init() {
  // El launcher ya inicializó Serial, TFT, canvas, ADC y buzzer.
  // Reafirmar todos los controles como entradas digitales.
  // GPIO1 había quedado afectado por una lectura ADC heredada.
  pinMode(PIN_UP, INPUT_PULLUP);          // GPIO1
  pinMode(PIN_DOWN, INPUT_PULLUP);        // GPIO16
  pinMode(PIN_LEFT, INPUT_PULLUP);        // GPIO7
  pinMode(PIN_RIGHT, INPUT_PULLUP);       // GPIO15
  pinMode(PQ_PIN_BTN_LEFT, INPUT_PULLUP); // GPIO2
  pinMode(PIN_PAUSA, INPUT_PULLUP);       // GPIO5

  // El canvas compartido es 320x240. Restauramos sus ajustes porque
  // otros juegos pueden haber cambiado fuente, datum o tamaño de texto.
  spr.setTextSize(1);
  spr.setTextFont(1);
  spr.setTextDatum(TL_DATUM);

  randomSeed((uint32_t)esp_random() ^ micros() ^ leerCrucetaADC());
  for (int i = 0; i < MAX_STARS; i++) {
    stars[i].x          = random(0, PQ_SCREEN_W);
    stars[i].y          = random(0, PQ_SCREEN_H / 2);
    stars[i].brightness = random(0, 4);
  }

  gameState = GS_TITLE;

  bool leftInicial = false;
  bool rightInicial = false;
  readButtons(leftInicial, rightInicial);

  btnLeft_prev = leftInicial;
  btnRight_prev = rightInicial;
  controlesTituloLiberados = !(leftInicial || rightInicial);

  pausaActivaPQ = false;
  estadoPausaPQ = PQ_PAUSA_MENU;
  opcionPausaPQ = 0;
  pausaAnteriorPQ = (digitalRead(PIN_PAUSA) == LOW);
  okAnteriorPausaPQ = (digitalRead(PQ_PIN_BTN_LEFT) == LOW);
  direccionAnteriorPausaPQ = 0;

  lastFrame = millis();
  resetTitleMusic();
  drawTitleScreen();
}

void cerrar() {
  buzzerOff();
  astro.thrustLeft = false;
  astro.thrustRight = false;
  spr.setTextSize(1);
  spr.setTextFont(1);
  spr.setTextDatum(TL_DATUM);
}

// Registra el logro y, si entrega contenido, prepara el aviso para la proxima muerte.
bool desbloquearLogroAstronauta(uint8_t id) {
  const bool nuevo = sistemaLogros.desbloquear(JUEGO_ASTRONAUTA, id);
  if (nuevo) sistemaDesbloqueos.registrarLogroAstronauta(id);
  return nuevo;
}

void dibujarRecompensaAstronauta() {
  spr.fillSprite(C_BG);
  // Estrellas sencillas para conservar la identidad visual del juego.
  for (int i = 0; i < 34; ++i) {
    int x = (i * 47 + 13) % PQ_SCREEN_W;
    int y = (i * 29 + 7) % PQ_SCREEN_H;
    spr.drawPixel(x, y, (i % 3 == 0) ? TFT_CYAN : TFT_WHITE);
  }
  spr.fillRoundRect(24, 38, PQ_SCREEN_W - 48, 164, 12, 0x1082);
  spr.drawRoundRect(24, 38, PQ_SCREEN_W - 48, 164, 12, TFT_YELLOW);
  spr.setTextDatum(MC_DATUM);
  spr.setTextColor(TFT_YELLOW, 0x1082);
  spr.drawString("RECOMPENSA DESBLOQUEADA", PQ_SCREEN_W / 2, 66, 2);
  spr.setTextColor(TFT_WHITE, 0x1082);
  spr.drawString(sistemaDesbloqueos.tituloRecompensa(recompensaActualPQ),
                 PQ_SCREEN_W / 2, 112, 2);
  spr.setTextColor(TFT_LIGHTGREY, 0x1082);
  spr.drawString(sistemaDesbloqueos.detalleRecompensa(recompensaActualPQ),
                 PQ_SCREEN_W / 2, 145, 1);
  spr.drawString("Pulsa un propulsor", PQ_SCREEN_W / 2, 180, 1);
  spr.setTextDatum(TL_DATUM);
  perfilPushSprite(0, 0);
}

void abrirSiguienteRecompensaAstronauta() {
  recompensaActualPQ = sistemaDesbloqueos.tomarRecompensaPendiente();
  botonesLiberadosRecompensaPQ = false;
  gameState = GS_REWARD;
  dibujarRecompensaAstronauta();
}

// ══════════════════════════════════════════════
//  LOOP
// ══════════════════════════════════════════════
void update() {
  // Igual que en Cuatete: MENU se procesa una sola vez aquí.
  const bool pausaAhora = (digitalRead(PIN_PAUSA) == LOW);
  const bool pausaPulsada = pausaAhora && !pausaAnteriorPQ;
  pausaAnteriorPQ = pausaAhora;

  if (pausaPulsada) {
    if (pausaActivaPQ) {
      if (estadoPausaPQ == PQ_PAUSA_VOLUMEN) {
        // Desde SONIDO, MENU vuelve al menú principal de pausa.
        estadoPausaPQ = PQ_PAUSA_MENU;
        opcionPausaPQ = 0;
        direccionAnteriorPausaPQ = 0;
        okAnteriorPausaPQ =
            (digitalRead(PQ_PIN_BTN_LEFT) == LOW);
        dibujarMenuPausaPQ();
      } else {
        // Desde el menú principal de pausa, MENU continúa la partida.
        pausaActivaPQ = false;
        astro.thrustLeft = false;
        astro.thrustRight = false;
        lastFrame = millis();
        drawScene();
      }
    } else {
      if (gameState == GS_TITLE) {
        cerrar();
        volverAlMenu();
      } else {
        buzzerOff();
        astro.thrustLeft = false;
        astro.thrustRight = false;

        pausaActivaPQ = true;
        estadoPausaPQ = PQ_PAUSA_MENU;
        opcionPausaPQ = 0;
        direccionAnteriorPausaPQ = 0;
        okAnteriorPausaPQ =
            (digitalRead(PQ_PIN_BTN_LEFT) == LOW);
        dibujarMenuPausaPQ();
      }
    }
    return;
  }

  if (pausaActivaPQ) {
    manejarMenuPausaPQ();
    return;
  }

  const uint32_t now = millis();
  if (now - lastFrame < 16) return;
  lastFrame = now;

  bool left = false;
  bool right = false;
  readButtons(left, right);

  switch (gameState) {
    case GS_TITLE:
      drawTitleScreen();
      updateTitleMusic();

      // El juego no puede comenzar hasta que se haya soltado por completo
      // el botón heredado del launcher.
      if (!controlesTituloLiberados) {
        if (!left && !right) {
          controlesTituloLiberados = true;
          btnLeft_prev = false;
          btnRight_prev = false;
        }
        break;
      }

      if ((left || right) && !(btnLeft_prev || btnRight_prev)) {
        buzzerOff();
        initWorld();
        gameState = GS_PLAYING;
        soundStart();
        buzzerOff();
      }
      break;

    case GS_PLAYING: {
      const bool thrustAhora = left || right;
      const bool thrustAntes = btnLeft_prev || btnRight_prev;

      // El propulsor suena al comenzar una nueva pulsación. Si se suelta,
      // cortamos únicamente su WAV; otros SFX (aterrizaje/explosión) no se tocan.
      if (thrustAhora && !thrustAntes) {
        soundThrust();
      } else if (!thrustAhora && thrustAntes) {
        SFXSD::detenerSiRuta(PQ_SFX_PROPULSOR);
      }

      astro.thrustLeft = left;
      astro.thrustRight = right;
      updatePhysics();
      updateParticles();
      generateNextChunk();
      checkCollisions();
      drawScene();
      break;
    }

    case GS_EXPLODING:
      updateParticles();
      drawScene(false);
      drawExplosionFrame();
      if (millis() - stateTimer > 900) {
        buzzerOff();
        if (sistemaDesbloqueos.hayRecompensasPendientes()) {
          abrirSiguienteRecompensaAstronauta();
        } else {
          respawnAstronaut();
          gameState = GS_PLAYING;
        }
      }
      break;

    case GS_REWARD:
      // Exige soltar los dos propulsores para no cerrar el aviso con
      // la misma pulsacion que se estaba usando antes de morir.
      if (!botonesLiberadosRecompensaPQ) {
        if (!left && !right) botonesLiberadosRecompensaPQ = true;
      } else if ((left || right) && !(btnLeft_prev || btnRight_prev)) {
        if (sistemaDesbloqueos.hayRecompensasPendientes()) {
          abrirSiguienteRecompensaAstronauta();
        } else {
          respawnAstronaut();
          gameState = GS_PLAYING;
        }
      }
      break;

    case GS_RESPAWN:
      buzzerOff();
      respawnAstronaut();
      gameState = GS_PLAYING;
      break;
  }

  btnLeft_prev = left;
  btnRight_prev = right;
}

// ──────────────────────────────────────────────
// Lee el ADC de la cruceta (promediado) y dice si está en DERECHA
bool leerCrucetaDerecha() {
  long suma = 0;
  for (int i = 0; i < PQ_MUESTRAS_CRUCETA; i++) {
    suma += leerCrucetaADC();
    delayMicroseconds(200);
  }
  int valor = suma / PQ_MUESTRAS_CRUCETA;
  return (valor <= PQ_UMBRAL_DER_TECHO && valor > PQ_UMBRAL_DER_PISO);
}

void readButtons(bool &left, bool &right) {
  // IMPORTANTE: en updatePhysics(), thrustLeft empuja hacia la DERECHA
  // y thrustRight empuja hacia la IZQUIERDA.
  //
  // Manejo solicitado:
  // botón físico DERECHO  GPIO2  -> personaje hacia la IZQUIERDA
  // botón físico IZQUIERDO GPIO15 -> personaje hacia la DERECHA
  left  = (digitalRead(PIN_RIGHT) == LOW);       // GPIO15 -> thrustLeft -> derecha
  right = (digitalRead(PQ_PIN_BTN_LEFT) == LOW); // GPIO2  -> thrustRight -> izquierda
}

float worldToScreen(float wx) { return wx - cameraX; }

// ══════════════════════════════════════════════
//  INICIALIZAR MUNDO
// ══════════════════════════════════════════════
void initWorld() {
  // Re-sembrar cada partida para que las montañas/plataformas
  // no repitan el mismo patrón al empezar de nuevo.
  randomSeed((uint32_t)esp_random() ^ micros() ^ millis()); // GPIO1 ya no se usa como ADC

  cameraX = 0; worldGen = 0; streak = 0; lastPlatformIdx = 0;
  memset(platforms, 0, sizeof(platforms));
  memset(mountains, 0, sizeof(mountains));
  memset(blackHoles, 0, sizeof(blackHoles));
  memset(particles, 0, sizeof(particles));

  // Plataforma inicial
  platforms[0].worldX    = 30;
  platforms[0].y         = WORLD_GROUND_Y - 10;
  platforms[0].w         = 60;
  platforms[0].visited   = true;
  platforms[0].isCurrent = true;
  worldGen = 200;

  for (int i = 0; i < 4; i++) generateNextChunk();

  astro.worldX   = platforms[0].worldX + platforms[0].w / 2;
  astro.x        = worldToScreen(astro.worldX);
  astro.y        = platforms[0].y - astro.height;
  astro.vx = astro.vy = 0;
  astro.fuel       = FUEL_MAX;
  astro.onGround   = true;
  astro.alive      = true;
  astro.graceTicks = 30;
  astro.thrustLeft = astro.thrustRight = false;
  buzzerOff();
}

// ══════════════════════════════════════════════
//  GENERACIÓN PROCEDURAL
// ══════════════════════════════════════════════
// Devuelve true si el rango [x1,x2] se solapa con alguna plataforma
// (con margen de seguridad extra a cada lado)
bool overlapsAnyPlatform(float x1, float x2, float margin = 20.0f) {
  for (int i = 0; i < MAX_PLATFORMS; i++) {
    if (platforms[i].w == 0) continue;
    float px1 = platforms[i].worldX - margin;
    float px2 = platforms[i].worldX + platforms[i].w + margin;
    if (x2 > px1 && x1 < px2) return true;
  }
  return false;
}

// Una montaña SÍ puede quedar detrás de una plataforma, pero nunca
// debe tapar/bloquear más del 50% de su superficie aterrizable.
bool mountainCoversPlatformTooMuch(float x, int w, int h, uint8_t style, uint8_t seed) {
  Mountain temp;
  temp.worldX = x; temp.baseY = WORLD_GROUND_Y; temp.w = w; temp.h = h;
  temp.style = style; temp.seed = seed; temp.colorIdx = 0;
  for (int i = 0; i < MAX_PLATFORMS; i++) {
    if (platforms[i].w == 0) continue;
    int covered = 0;
    int limit = platforms[i].w / 2;
    for (int px = 0; px < platforms[i].w; px++) {
      float wx = platforms[i].worldX + px;
      if (wx < x || wx > x + w) continue;
      float cx = (wx - x) / w;
      float surfaceY = WORLD_GROUND_Y - mountainHeightAt(temp, cx);
      // Cuenta como "tapada" si la montaña llega hasta la altura
      // de la plataforma o queda por encima de ella.
      if (surfaceY <= platforms[i].y + PLATFORM_H) covered++;
      if (covered > limit) return true;
    }
  }
  return false;
}

void generateNextChunk() {
  // ── Nueva plataforma ───────────────────────────────
  int slot = findFreePlatformSlot();
  if (slot >= 0) {
    float lastX = 0;
    for (int i = 0; i < MAX_PLATFORMS; i++)
      if (platforms[i].w > 0 && platforms[i].worldX > lastX)
        lastX = platforms[i].worldX + platforms[i].w;
    platforms[slot].worldX    = lastX + random(60, 130);
    platforms[slot].y         = WORLD_GROUND_Y - random(20, 90);
    platforms[slot].w         = random(30, 65);
    platforms[slot].visited   = false;
    platforms[slot].isCurrent = false;
    worldGen = platforms[slot].worldX + platforms[slot].w + 60;
  }

  // ── Montañas: pueden pasar detrás, pero dejando mínimo 50% libre ──
  int mCount = random(1, 3);
  for (int m = 0; m < mCount; m++) {
    int ms = findFreeMountainSlot();
    if (ms < 0) return;

    // Buscar el extremo derecho de todas las montañas activas
    float lastMX = 0;
    for (int i = 0; i < MAX_MOUNTAINS; i++)
      if (mountains[i].w > 0 && mountains[i].worldX > lastMX)
        lastMX = mountains[i].worldX + mountains[i].w;

    int   mw    = random(45, 110);
    int   mh    = random(60, 170);
    uint8_t sty = random(0, 5);     // 5 estilos distintos de relieve
    uint8_t sd  = random(0, 256);   // semilla para variar la cresta dentada
    uint8_t col = random(0, 4);     // variante de color de roca
    float mStart = lastMX + random(10, 40);

    // Si tapa más de media plataforma, buscar otra posición.
    int attempts = 0;
    while (mountainCoversPlatformTooMuch(mStart, mw, mh, sty, sd) && attempts < 25) {
      mStart += 25;
      attempts++;
    }
    if (mountainCoversPlatformTooMuch(mStart, mw, mh, sty, sd)) continue;

    mountains[ms].worldX  = mStart;
    mountains[ms].baseY   = WORLD_GROUND_Y;
    mountains[ms].h       = mh;
    mountains[ms].w       = mw;
    mountains[ms].style   = sty;
    mountains[ms].seed    = sd;
    mountains[ms].colorIdx = col;
  }

  // ── Agujeros negros: obstáculos aéreos que succionan ──
  if (random(0, 100) < 45) {
    int bs = findFreeBlackHoleSlot();
    if (bs >= 0) {
      float newestPlatformX = 0;
      for (int i = 0; i < MAX_PLATFORMS; i++)
        if (platforms[i].w > 0 && platforms[i].worldX > newestPlatformX)
          newestPlatformX = platforms[i].worldX;
      blackHoles[bs].worldX = newestPlatformX + random(-40, 90);
      blackHoles[bs].y      = random(45, WORLD_GROUND_Y - 75);
      blackHoles[bs].r      = random(9, 15);
      blackHoles[bs].active = true;
    }
  }
}

int findFreePlatformSlot() {
  for (int i = 0; i < MAX_PLATFORMS; i++) {
    if (platforms[i].w == 0) return i;
    if (worldToScreen(platforms[i].worldX + platforms[i].w) < -40
        && !platforms[i].isCurrent) {
      platforms[i].w = 0; return i;
    }
  }
  return -1;
}

int findFreeMountainSlot() {
  for (int i = 0; i < MAX_MOUNTAINS; i++) {
    if (mountains[i].w == 0) return i;
    if (worldToScreen(mountains[i].worldX + mountains[i].w) < -40) {
      mountains[i].w = 0; return i;
    }
  }
  return -1;
}

int findFreeBlackHoleSlot() {
  for (int i = 0; i < MAX_BLACK_HOLES; i++) {
    if (!blackHoles[i].active) return i;
    if (worldToScreen(blackHoles[i].worldX) < -40) {
      blackHoles[i].active = false; return i;
    }
  }
  return -1;
}

// ══════════════════════════════════════════════
//  FÍSICA
// ══════════════════════════════════════════════
void updateBlackHoleForces() {
  if (astro.onGround || astro.graceTicks > 0) return;
  for (int i = 0; i < MAX_BLACK_HOLES; i++) {
    if (!blackHoles[i].active) continue;
    float bx = blackHoles[i].worldX;
    float by = blackHoles[i].y;
    float ax = astro.worldX;
    float ay = astro.y + astro.height / 2;
    float dx = bx - ax;
    float dy = by - ay;
    float d2 = dx * dx + dy * dy;
    float pullRadius = blackHoles[i].r * 7.0f;
    if (d2 < pullRadius * pullRadius && d2 > 1.0f) {
      float d = sqrt(d2);
      float force = 0.18f * (1.0f - d / pullRadius);
      astro.vx += (dx / d) * force;
      astro.vy += (dy / d) * force;
      if (d < blackHoles[i].r + 5) { killAstronaut(); return; }
    }
  }
}

void updatePhysics() {
  if (!astro.alive) return;

  bool bothThrust = astro.thrustLeft && astro.thrustRight;

  if (astro.fuel > 0) {
    if (bothThrust) {
      astro.vy   -= THRUST_UP;
      astro.fuel -= FUEL_BURN_RATE * 2.0f;
      if (random(0, 3) == 0)
        spawnParticles(astro.x + astro.width / 2,
                       astro.y + astro.height, 0, 1);
    } else {
      if (astro.thrustLeft) {
        astro.vx   += THRUST_SIDE_H;
        astro.vy   -= THRUST_SIDE_V;
        astro.fuel -= FUEL_BURN_RATE;
        if (random(0, 2) == 0)
          spawnParticles(astro.x, astro.y + astro.height - 4, 0, 1);
      }
      if (astro.thrustRight) {
        astro.vx   -= THRUST_SIDE_H;
        astro.vy   -= THRUST_SIDE_V;
        astro.fuel -= FUEL_BURN_RATE;
        if (random(0, 2) == 0)
          spawnParticles(astro.x + astro.width,
                         astro.y + astro.height - 4, 0, 1);
      }
    }
    astro.fuel = max(0.0f, astro.fuel);
  }

  // Freno de caída con doble propulsor
  if (bothThrust && astro.vy > 0) astro.vy *= 0.80f;

  bool wasOnPlatform = astro.onGround;

  if (!wasOnPlatform) {
    astro.vy += GRAVITY;
  } else if (!astro.thrustLeft && !astro.thrustRight) {
    // En plataforma: que el astronauta se quede plantado y no resbale.
    astro.vx = 0;
    astro.vy = 0;
  }

  updateBlackHoleForces();
  if (!astro.alive) return;

  astro.vx = constrain(astro.vx, -MAX_VX, MAX_VX);
  astro.vy = constrain(astro.vy, -MAX_VY, MAX_VY);

  astro.onGround = false;
  astro.y       += astro.vy;
  astro.worldX  += astro.vx;
  astro.x        = worldToScreen(astro.worldX);

  // Scroll de cámara
  if (astro.x > PQ_SCREEN_W - SCROLL_MARGIN) {
    cameraX += astro.x - (PQ_SCREEN_W - SCROLL_MARGIN);
    astro.x  = worldToScreen(astro.worldX);
  }
  if (astro.x < 30) {
    cameraX -= 30 - astro.x;
    astro.x  = worldToScreen(astro.worldX);
  }

  // Suelo base → siempre cuenta como caída/muerte.
  // Solo las plataformas son zonas válidas de aterrizaje.
  if (astro.y + astro.height >= WORLD_GROUND_Y) {
    astro.y = WORLD_GROUND_Y - astro.height;
    killAstronaut();
    return;
  }

  if (astro.worldX > worldGen - 200) generateNextChunk();
}

// ══════════════════════════════════════════════
//  COLISIONES
// ══════════════════════════════════════════════
void checkCollisions() {
  if (!astro.alive) return;
  // La gracia tras respawn NO debe desactivar plataformas,
  // porque si no el astronauta cae a través de la plataforma inicial.
  bool skipHazards = false;
  if (astro.graceTicks > 0) { astro.graceTicks--; skipHazards = true; }

  float ax1 = astro.worldX - astro.width / 2;
  float ax2 = astro.worldX + astro.width / 2;
  float ay2 = astro.y + astro.height;
  float ay1 = astro.y;

  // Plataformas
  for (int i = 0; i < MAX_PLATFORMS; i++) {
    if (platforms[i].w == 0) continue;
    float px1 = platforms[i].worldX;
    float px2 = platforms[i].worldX + platforms[i].w;
    float py  = platforms[i].y;

    if ((ax2 > px1 + 2) && (ax1 < px2 - 2) &&
        (ay2 >= py) && (ay2 <= py + 10) && (astro.vy >= 0)) {
      if (astro.vy > MAX_SAFE_LAND) { killAstronaut(); return; }
      const float velocidadAterrizaje = astro.vy;
      const float combustibleAterrizaje = astro.fuel;
      bool saltoPlataforma = false;
      if (!platforms[i].visited) {
        const float xAnterior = platforms[lastPlatformIdx].worldX;
        float siguienteX = 1.0e30f;
        for (int k = 0; k < MAX_PLATFORMS; ++k) {
          if (platforms[k].w == 0 || platforms[k].visited) continue;
          if (platforms[k].worldX > xAnterior + 1.0f && platforms[k].worldX < siguienteX)
            siguienteX = platforms[k].worldX;
        }
        saltoPlataforma = (siguienteX < 1.0e29f && platforms[i].worldX > siguienteX + 1.0f);
      }
      astro.y  = py - astro.height;
      astro.vy = 0;
      astro.vx = 0;  // aterrizaje estable: sin deslizamiento sobre la plataforma
      astro.onGround = true;
      if (!platforms[i].visited) {
        platforms[i].visited = true;
        for (int j = 0; j < MAX_PLATFORMS; j++) platforms[j].isCurrent = false;
        platforms[i].isCurrent = true;
        lastPlatformIdx = i;
        streak++;
        desbloquearLogroAstronauta(PQ_LOGRO_PRIMER_ALUNIZAJE);
        if (streak >= 15) desbloquearLogroAstronauta(PQ_LOGRO_PILOTO_NOVATO);
        if (streak >= 20) desbloquearLogroAstronauta(PQ_LOGRO_EXPLORADOR_ESPACIAL);
        if (streak >= 25) desbloquearLogroAstronauta(PQ_LOGRO_CORTO_GRAN_SALTO);
        if (streak >= 30) desbloquearLogroAstronauta(PQ_LOGRO_ASTRONAUTA_LEGENDARIO);
        if (velocidadAterrizaje <= 0.45f) desbloquearLogroAstronauta(PQ_LOGRO_ATERRIZAJE_PERFECTO);
        if (saltoPlataforma) desbloquearLogroAstronauta(PQ_LOGRO_SALTO_DE_FE);
        if (combustibleAterrizaje <= 0.001f) desbloquearLogroAstronauta(PQ_LOGRO_SIN_UNA_GOTA);
        if (combustibleAterrizaje <= FUEL_MAX * 0.20f) desbloquearLogroAstronauta(PQ_LOGRO_EN_RESERVA);
        soundLand();
        spawnParticles(astro.x + astro.width / 2,
                       astro.y + astro.height, 1, 8);
      }
      astro.fuel = min(FUEL_MAX, astro.fuel + 15.0f);
      return;
    }
  }

  // Montañas
  // Si el astronauta ya está posado (onGround), las montañas
  // no pueden matarlo — está sobre una plataforma, no volando.
  if (!astro.onGround && !skipHazards) {
    for (int i = 0; i < MAX_MOUNTAINS; i++) {
      if (mountains[i].w == 0) continue;
      float mx1 = mountains[i].worldX;
      float mx2 = mountains[i].worldX + mountains[i].w;
      // Usar el centro del astronauta en X para el cálculo de perfil
      if (astro.worldX < mx1 || astro.worldX > mx2) continue;

      float cx = (astro.worldX - mx1) / mountains[i].w;
      cx = constrain(cx, 0.0f, 1.0f);
      float mHeight = mountainHeightAt(mountains[i], cx);
      // mSurface = Y de la cima de la montaña en esta columna
      float mSurface = mountains[i].baseY - max(0.0f, mHeight);
      // Colisión real: los pies del astronauta tocan o entran en la montaña
      // Se añade un margen de +3px para no matar en el borde del suelo base
      if (ay2 >= mSurface + 3 && ay1 < (float)mountains[i].baseY - 3) {
        killAstronaut(); return;
      }
    }
  }
}

void killAstronaut() {
  if (!astro.alive) return;
  astro.alive = false;
  streak = 0;
  spawnParticles(astro.x + astro.width / 2,
                 astro.y + astro.height / 2, 1, 20);
  soundCrash();
  buzzerOff();
  gameState  = GS_EXPLODING;
  stateTimer = millis();
}

void respawnAstronaut() {
  Platform &p = platforms[lastPlatformIdx];
  astro.worldX     = p.worldX + p.w / 2;
  astro.y          = p.y - astro.height;
  astro.vx = astro.vy = 0;
  astro.fuel       = FUEL_MAX;
  astro.onGround   = true;
  astro.alive      = true;
  astro.graceTicks = 30;  // ~0.5s sin colisiones
  astro.thrustLeft = astro.thrustRight = false;
  cameraX  = astro.worldX - PQ_SCREEN_W / 3;
  astro.x  = worldToScreen(astro.worldX);
  buzzerOff();
}

// ══════════════════════════════════════════════
//  PARTÍCULAS
// ══════════════════════════════════════════════
void spawnParticles(float x, float y, uint8_t type, uint8_t count) {
  for (int n = 0; n < count; n++) {
    for (int i = 0; i < MAX_PARTICLES; i++) {
      if (!particles[i].active) {
        particles[i].active = true;
        particles[i].type   = type;
        particles[i].x      = x + random(-4, 5);
        particles[i].y      = y + random(-4, 5);
        if (type == 0) {
          particles[i].vx      = random(-20, 21) / 20.0f;
          particles[i].vy      = random(5, 20) / 10.0f;
          particles[i].color   = C_SMOKE;
          particles[i].maxLife = 12;
        } else {
          particles[i].vx      = random(-30, 31) / 10.0f;
          particles[i].vy      = random(-30, 10) / 10.0f;
          uint16_t cols[3]     = {C_EXPLODE1, C_EXPLODE2, C_EXPLODE3};
          particles[i].color   = cols[random(0, 3)];
          particles[i].maxLife = random(15, 30);
        }
        particles[i].life = particles[i].maxLife;
        break;
      }
    }
  }
}

void updateParticles() {
  for (int i = 0; i < MAX_PARTICLES; i++) {
    if (!particles[i].active) continue;
    particles[i].x  += particles[i].vx;
    particles[i].y  += particles[i].vy;
    particles[i].vy += 0.08f;
    if (particles[i].life > 0) particles[i].life--;
    else                       particles[i].active = false;
  }
}

// ══════════════════════════════════════════════
//  RENDER
// ══════════════════════════════════════════════
void drawScene(bool pushToScreen) {
  spr.fillSprite(C_SKY);
  drawBackground();
  drawMountains();
  drawPlatforms();
  drawBlackHoles();
  drawParticles();
  if (astro.alive) drawAstronaut();
  drawHUD();
  if (pushToScreen) bateriaPushSprite(spr, 0, 0);
}

void drawBackground() {
  // Estrellas con parallax leve
  for (int i = 0; i < MAX_STARS; i++) {
    int sx = ((int)(stars[i].x - cameraX * 0.05f) % PQ_SCREEN_W + PQ_SCREEN_W) % PQ_SCREEN_W;
    uint16_t col = (stars[i].brightness == 3) ? 0xFFFF :
                   (stars[i].brightness == 2) ? 0xC618 :
                   (stars[i].brightness == 1) ? 0x8410 : 0x4208;
    spr.drawPixel(sx, stars[i].y, col);
  }
  // Banda de horizonte naranja
  for (int y = PQ_SCREEN_H * 2 / 3; y < WORLD_GROUND_Y; y++) {
    float t  = (float)(y - PQ_SCREEN_H * 2 / 3) / (WORLD_GROUND_Y - PQ_SCREEN_H * 2 / 3);
    uint8_t r = (uint8_t)(10 + t * 130);
    uint8_t g = (uint8_t)(2  + t * 30);
    spr.drawFastHLine(0, y, PQ_SCREEN_W, spr.color565(r, g, 0));
  }
  // Suelo
  spr.fillRect(0, WORLD_GROUND_Y, PQ_SCREEN_W, PQ_SCREEN_H - WORLD_GROUND_Y, C_GROUND);
  spr.drawFastHLine(0, WORLD_GROUND_Y, PQ_SCREEN_W, C_MTN_EDGE);
}

// Devuelve la altura del relieve de la montaña en la columna relativa
// `cx` (0.0 = borde izquierdo, 1.0 = borde derecho). Se usa tanto para
// dibujar como para las colisiones, así ambos quedan sincronizados.
float mountainHeightAt(const Mountain &m, float cx) {
  cx = constrain(cx, 0.0f, 1.0f);
  float h = 0;
  switch (m.style) {
    case 0: // pico simple
      h = m.h * (1.0f - 2.0f * abs(cx - 0.5f));
      break;
    case 1: // doble pico
      h = m.h * max(1.0f - 3.0f * abs(cx - 0.25f),
                    1.0f - 3.0f * abs(cx - 0.75f));
      h = max(0.0f, h);
      break;
    case 2: // meseta con laterales en pendiente
      if      (cx < 0.2f) h = m.h * (cx / 0.2f);
      else if (cx > 0.8f) h = m.h * ((1.0f - cx) / 0.2f);
      else                h = m.h;
      break;
    case 3: { // cresta dentada: varios picos irregulares
      float p1 = 0.12f + (m.seed % 5)        * 0.02f;
      float p2 = 0.50f + ((m.seed / 5) % 5 - 2) * 0.03f;
      float p3 = 0.85f - (m.seed % 4)        * 0.02f;
      float h1 = m.h * max(0.0f, 1.0f - 4.5f * abs(cx - p1));
      float h2 = m.h * 0.75f * max(0.0f, 1.0f - 4.5f * abs(cx - p2));
      float h3 = m.h * 0.92f * max(0.0f, 1.0f - 4.5f * abs(cx - p3));
      h = max(h1, max(h2, h3));
      break;
    }
    case 4: { // mesa escalonada (terrazas tipo cañón)
      float tri = m.h * (1.0f - 2.0f * abs(cx - 0.5f));
      tri = max(0.0f, tri);
      float step = m.h / 5.0f;
      h = (float)((int)(tri / step)) * step;
      break;
    }
  }
  return h;
}

void drawMountains() {
  for (int i = 0; i < MAX_MOUNTAINS; i++) {
    if (mountains[i].w == 0) continue;
    float sx = worldToScreen(mountains[i].worldX);
    if (sx > PQ_SCREEN_W + 10 || sx + mountains[i].w < -10) continue;
    int mw = mountains[i].w, mh = mountains[i].h, by = mountains[i].baseY;
    uint16_t baseCol = MTN_BASE[mountains[i].colorIdx & 0x03];
    uint16_t hiCol   = MTN_HI[mountains[i].colorIdx & 0x03];
    bool snowCap = mh > 130; // los picos más altos amanecen nevados
    for (int col = 0; col < mw; col++) {
      float cx   = (float)col / mw;
      float colH = mountainHeightAt(mountains[i], cx);
      int px = (int)(sx + col);
      if (px < 0 || px >= PQ_SCREEN_W) continue;
      int topY = by - (int)colH;
      if (topY >= by) continue;
      int h = by - topY;
      // Sombreado: franja superior más clara (luz), cuerpo más oscuro
      int hiH = (int)(h * 0.35f);
      if (hiH > 0) spr.drawFastVLine(px, topY, hiH, hiCol);
      spr.drawFastVLine(px, topY + hiH, h - hiH, baseCol);
      // Casquete de nieve en los picos más altos
      if (snowCap && colH > mh * 0.80f) {
        int snowH = constrain((int)(colH - mh * 0.80f), 1, 6);
        spr.drawFastVLine(px, topY, snowH, C_SNOW);
      }
    }
  }
}

void drawPlatforms() {
  for (int i = 0; i < MAX_PLATFORMS; i++) {
    if (platforms[i].w == 0) continue;
    float sx = worldToScreen(platforms[i].worldX);
    if (sx > PQ_SCREEN_W || sx + platforms[i].w < 0) continue;
    int px = (int)sx, py = platforms[i].y, pw = platforms[i].w;
    uint16_t bodyColor = platforms[i].isCurrent ? 0x07E0 : C_PLATFORM;
    spr.fillRect(px, py, pw, PLATFORM_H, bodyColor);
    spr.drawFastHLine(px, py, pw, C_PLT_TOP);
    if (platforms[i].visited && !platforms[i].isCurrent)
      spr.fillTriangle(px + pw/2 - 3, py - 1, px + pw/2 + 3, py - 1,
                       px + pw/2, py - 5, 0x07E0);
    if (platforms[i].isCurrent)
      spr.drawRect(px - 1, py - 1, pw + 2, PLATFORM_H + 2, 0x07E0);
  }
}

void drawBlackHoles() {
  for (int i = 0; i < MAX_BLACK_HOLES; i++) {
    if (!blackHoles[i].active) continue;
    int bx = (int)worldToScreen(blackHoles[i].worldX);
    int by = blackHoles[i].y;
    int r  = blackHoles[i].r;
    if (bx < -25 || bx > PQ_SCREEN_W + 25) continue;
    int spin = (millis() / 80 + i * 3) % 12;
    spr.drawCircle(bx, by, r + 6, C_BH_RING1);
    spr.drawCircle(bx, by, r + 3, C_BH_RING2);
    spr.fillCircle(bx, by, r, C_BLACKHOLE);
    spr.fillCircle(bx, by, max(2, r - 4), TFT_BLACK);
    spr.drawLine(bx - r - 4, by + spin - 6, bx + r + 4, by - spin + 6, C_BH_RING1);
  }
}

static void dibujarSpriteAstronauta4bpp(const uint8_t *datos,
                                         uint8_t w, uint8_t h,
                                         uint8_t anchorX, uint8_t anchorY,
                                         int16_t centroX, int16_t pieY) {
  const int16_t origenX = centroX - anchorX;
  const int16_t origenY = pieY - anchorY;
  const uint16_t total = (uint16_t)w * h;

  for (uint16_t i = 0; i < total; ++i) {
    const uint8_t packed = pgm_read_byte(&datos[i >> 1]);
    const uint8_t indice = (i & 1) ? (packed & 0x0F) : (packed >> 4);
    if (indice == 0) continue;  // transparente

    const uint16_t color = pgm_read_word(&AstronautaSprites::PALETA[indice]);
    const int16_t px = origenX + (i % w);
    const int16_t py = origenY + (i / w);
    spr.drawPixel(px, py, color);
  }
}

void drawAstronaut() {
  const int16_t ax = (int16_t)astro.x;
  const int16_t ay = (int16_t)astro.y;
  const int16_t centroX = ax + astro.width / 2;
  const int16_t pieY = ay + astro.height - 1;

  const bool left  = astro.thrustLeft  && astro.fuel > 0;
  const bool right = astro.thrustRight && astro.fuel > 0;
  const bool both  = left && right;

  // Mapeo de los sprites originales:
  // 1 = botón A / propulsor izquierdo
  // 2 = cruceta derecha / propulsor derecho
  // 5 = ambos propulsores
  // 6 = reposo en plataforma (y fallback cuando no hay propulsión)
  if (both) {
    dibujarSpriteAstronauta4bpp(
        AstronautaSprites::astroner5,
        AstronautaSprites::astroner5_w,
        AstronautaSprites::astroner5_h,
        AstronautaSprites::astroner5_ax,
        AstronautaSprites::astroner5_ay,
        centroX, pieY);
  } else if (left) {
    dibujarSpriteAstronauta4bpp(
        AstronautaSprites::astroner2,
        AstronautaSprites::astroner2_w,
        AstronautaSprites::astroner2_h,
        AstronautaSprites::astroner2_ax,
        AstronautaSprites::astroner2_ay,
        centroX, pieY);
  } else if (right) {
    dibujarSpriteAstronauta4bpp(
        AstronautaSprites::astroner1,
        AstronautaSprites::astroner1_w,
        AstronautaSprites::astroner1_h,
        AstronautaSprites::astroner1_ax,
        AstronautaSprites::astroner1_ay,
        centroX, pieY);
  } else {
    dibujarSpriteAstronauta4bpp(
        AstronautaSprites::astroner6,
        AstronautaSprites::astroner6_w,
        AstronautaSprites::astroner6_h,
        AstronautaSprites::astroner6_ax,
        AstronautaSprites::astroner6_ay,
        centroX, pieY);
  }
}

void drawParticles() {
  for (int i = 0; i < MAX_PARTICLES; i++) {
    if (!particles[i].active) continue;
    float alpha = (float)particles[i].life / particles[i].maxLife;
    uint16_t col = (alpha < 0.3f) ? (uint16_t)0x2104 : particles[i].color;
    int px = (int)particles[i].x, py = (int)particles[i].y;
    if (px < 0 || px >= PQ_SCREEN_W || py < 0 || py >= PQ_SCREEN_H) continue;
    if (particles[i].type == 0) spr.fillCircle(px, py, 1, col);
    else {
      spr.drawPixel(px, py, col);
      if (alpha > 0.5f) spr.drawPixel(px+1, py, col);
    }
  }
}

void drawHUD() {
  int barX=5, barY=5, barW=60, barH=7;
  spr.fillRect(barX-1, barY-1, barW+2, barH+2, C_FUEL_BG);
  float pct   = astro.fuel / FUEL_MAX;
  int   fillW = (int)(pct * barW);
  uint16_t fc = (pct > 0.5f) ? C_FUEL_OK : (pct > 0.2f) ? C_FUEL_LOW : C_FUEL_CRIT;
  if (fillW > 0) spr.fillRect(barX, barY, fillW, barH, fc);
  spr.drawRect(barX-1, barY-1, barW+2, barH+2, TFT_WHITE);
  spr.setTextColor(C_HUD_TEXT, C_SKY);
  spr.setTextSize(1);
  spr.setCursor(barX + barW + 4, barY);
  spr.print("FUEL");

  spr.setTextColor(C_STREAK, C_SKY);
  // Zona superior derecha reservada para el medidor de bateria.
  spr.setCursor(PQ_SCREEN_W - 140, 5);
  spr.print("RACHA:");
  spr.setTextSize(2);
  spr.setCursor(PQ_SCREEN_W - 102, 3);
  spr.print(streak);

  if (astro.thrustLeft && astro.fuel > 0)
    spr.fillTriangle(3, PQ_SCREEN_H-20, 3, PQ_SCREEN_H-8, 12, PQ_SCREEN_H-14, C_THRUSTER);
  if (astro.thrustRight && astro.fuel > 0)
    spr.fillTriangle(PQ_SCREEN_W-3, PQ_SCREEN_H-20, PQ_SCREEN_W-3, PQ_SCREEN_H-8,
                     PQ_SCREEN_W-12, PQ_SCREEN_H-14, C_THRUSTER);
}

// ══════════════════════════════════════════════
//  PANTALLA DE TÍTULO
// ══════════════════════════════════════════════
void drawTitleScreen() {
  spr.fillSprite(TFT_BLACK);
  for (int i = 0; i < MAX_STARS; i++)
    spr.drawPixel(stars[i].x, stars[i].y, TFT_WHITE);

  // Planeta
  spr.fillCircle(PQ_SCREEN_W/2, PQ_SCREEN_H/2 + 30, 75, 0xCA00);
  spr.fillCircle(PQ_SCREEN_W/2-15, PQ_SCREEN_H/2+10, 13, 0xA800);
  spr.fillCircle(PQ_SCREEN_W/2+22, PQ_SCREEN_H/2+35, 9, 0xA800);

  // Título
  spr.setTextDatum(MC_DATUM);
  spr.setTextColor(0xFD20, TFT_BLACK);
  spr.setTextSize(4);
  spr.drawString("ASTRONAUTA", PQ_SCREEN_W/2, 60);
  
  // Astronauta decorativo
  spr.fillRoundRect(PQ_SCREEN_W/2-5, 114, 10, 12, 2, TFT_WHITE);
  spr.fillCircle(PQ_SCREEN_W/2, 112, 5, TFT_WHITE);
  spr.fillEllipse(PQ_SCREEN_W/2, 112, 3, 2, C_VISOR);

  if ((millis() / 500) % 2 == 0) {
    spr.setTextSize(1);
    spr.setTextColor(TFT_WHITE, TFT_BLACK);
    spr.drawString("Presiona START", PQ_SCREEN_W/2, PQ_SCREEN_H - 50);
  }
  spr.setTextSize(1);
  spr.setTextColor(0x8410, TFT_BLACK);
  spr.drawString("Izq/Der: propulsores", PQ_SCREEN_W/2, PQ_SCREEN_H - 30);
  spr.drawString("Ambos: subir / frenar", PQ_SCREEN_W/2, PQ_SCREEN_H - 18);

  bateriaPushSprite(spr, 0, 0);
}

// ══════════════════════════════════════════════
//  FRAME DE EXPLOSIÓN
// ══════════════════════════════════════════════
void drawExplosionFrame() {
  uint32_t elapsed = millis() - stateTimer;
  if (elapsed < 200)
    spr.fillCircle((int)astro.x + astro.width/2,
                   (int)astro.y + astro.height/2,
                   (int)(elapsed / 10), TFT_WHITE);
  spr.setTextDatum(MC_DATUM);
  spr.setTextColor(TFT_RED, TFT_BLACK);
  spr.setTextSize(2);
  spr.drawString("CHOCASTE ASTRO!", PQ_SCREEN_W/2, PQ_SCREEN_H/2);
  spr.setTextColor(TFT_WHITE, TFT_BLACK);
  spr.setTextSize(1);
  spr.drawString("Racha perdida", PQ_SCREEN_W/2, PQ_SCREEN_H/2 + 22);
  bateriaPushSprite(spr, 0, 0);
}

} // namespace Astronauta



// ════════════════════════════════════════════════════════════
//  REPRODUCTOR MP3 — /mp3/*.mp3
//  dr_mp3 se usa únicamente como decoder. PCM -> I2S legacy existente.
//  Controles: A play/pausa | <-/-> anterior/siguiente | Arr/Ab volumen
//             MENU salir
// ════════════════════════════════════════════════════════════
namespace ReproductorMP3 {

static const char *CARPETA = "/mp3";
static const int MAX_CANCIONES = 64;
static const int FILAS_VISIBLES = 8;

static String canciones[MAX_CANCIONES];
static uint32_t tamanos[MAX_CANCIONES] = {0};
static uint32_t duracionesMs[MAX_CANCIONES] = {0};
static int cantidad = 0;
static int seleccion = 0;
static int primeroVisible = 0;

static volatile bool reproduciendo = false;
static volatile bool pausado = false;
static volatile bool detener = false;
static volatile bool terminado = false;
static volatile uint32_t bytesLeidos = 0;
static volatile uint32_t bytesTotales = 0;
static volatile uint32_t duracionTotalMs = 0;
static volatile uint32_t bitrateActual = 0;
// Bitrate fijado al primer frame valido: evita que la duracion "baile" en MP3 VBR.
static volatile uint32_t bitrateBase = 0;
// Tiempo real reproducido, acumulado desde las muestras PCM decodificadas.
static volatile uint32_t tiempoReproMs = 0;
static volatile uint32_t sampleRateActual = 44100;
static volatile uint8_t canalesActual = 2;

// Anti-pop exclusivo del reproductor MP3.
static volatile uint32_t fadeInFramesLeft = 0;
static volatile uint32_t fadeInFramesTotal = 0;
static volatile int16_t ultimoL = 0;
static volatile int16_t ultimoR = 0;

static TaskHandle_t tarea = nullptr;
static File archivo;
static bool i2sTomado = false;
static bool menuPausaPrev = false;
static uint32_t volumenOSDHasta = 0;
static bool volumenOSDVisible = false;
static bool vistaPlayer = false;

// Bloqueo de controles de COCODRILO MUSIC.
// B bloquea con una pulsacion. Para desbloquear hay que soltar B y
// mantenerlo presionado de nuevo durante 2 segundos.
static bool controlesBloqueados = false;
static bool botonBMusicPrev = false;
static bool esperandoSoltarBTrasBloqueo = false;
static uint32_t inicioHoldBDesbloqueo = 0;
static constexpr uint32_t HOLD_B_DESBLOQUEO_MS = 2000UL;

// Modo de música de fondo para escenas especiales. Reutiliza exactamente
// el mismo decoder/I2S del reproductor, pero sin abrir su interfaz.
static volatile bool modoFondoLoop = false;
static String rutaFondoLoop;

#if COCODRILO_TIENE_MP3_DRMP3
static drmp3 *decoder = nullptr;
#endif

static String nombreLimpio(String n) {
  if (n.startsWith("/")) {
    int p = n.lastIndexOf('/');
    if (p >= 0) n = n.substring(p + 1);
  }
  if (n.length() > 4) {
    String low = n;
    low.toLowerCase();
    if (low.endsWith(".mp3")) n.remove(n.length() - 4);
  }
  return n;
}

// Convertidor autonomo UTF-8 a CP437 para titulos MP3.
// Se mantiene esta funcionalidad sin depender del modulo personal Cartas.
static String nombreVisible(String n) {
  String entrada = nombreLimpio(n);
  String salida;
  salida.reserve(entrada.length());
  for (size_t i = 0; i < entrada.length(); ++i) {
    uint8_t c1 = (uint8_t)entrada[i];
    if (c1 == 0xC3 && i + 1 < entrada.length()) {
      const uint8_t c2 = (uint8_t)entrada[i + 1];
      int cp437 = -1;
      switch (c2) {
        case 0xA1: cp437 = 160; break; // a con acento
        case 0xA9: cp437 = 130; break;
        case 0xAD: cp437 = 161; break;
        case 0xB3: cp437 = 162; break;
        case 0xBA: cp437 = 163; break;
        case 0xB1: cp437 = 164; break; // enye
        case 0x91: cp437 = 165; break;
        case 0x89: cp437 = 144; break;
        case 0xBC: cp437 = 129; break;
        case 0x9C: cp437 = 154; break;
        case 0x81: salida += 'A'; ++i; continue;
        case 0x8D: salida += 'I'; ++i; continue;
        case 0x93: salida += 'O'; ++i; continue;
        case 0x9A: salida += 'U'; ++i; continue;
      }
      if (cp437 >= 0) { salida += (char)cp437; ++i; continue; }
    }
    if (c1 == 0xC2 && i + 1 < entrada.length()) {
      const uint8_t c2 = (uint8_t)entrada[i + 1];
      if (c2 == 0xBF) { salida += (char)168; ++i; continue; }
      if (c2 == 0xA1) { salida += (char)173; ++i; continue; }
    }
    salida += (char)c1;
  }
  return salida;
}


static uint32_t leerSynchsafe32(const uint8_t *p) {
  return ((uint32_t)(p[0] & 0x7F) << 21) |
         ((uint32_t)(p[1] & 0x7F) << 14) |
         ((uint32_t)(p[2] & 0x7F) << 7) |
         ((uint32_t)(p[3] & 0x7F));
}

static uint32_t leerBE32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) |
         ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) |
         (uint32_t)p[3];
}

struct PortadaMP3 {
  uint32_t offset = 0;
  uint32_t bytes = 0;
  bool jpeg = false;
};

static bool localizarPortadaAPIC(const String &ruta, PortadaMP3 &out) {
  out = PortadaMP3();

  File f = SD_MMC.open(ruta, FILE_READ);
  if (!f || f.isDirectory()) {
    if (f) f.close();
    return false;
  }

  uint8_t cab[10];
  if (f.read(cab, sizeof(cab)) != (int)sizeof(cab) ||
      cab[0] != 'I' || cab[1] != 'D' || cab[2] != '3') {
    f.close();
    Serial.printf("[MP3][APIC] sin ID3v2: %s\n", ruta.c_str());
    return false;
  }

  const uint32_t tagBytes = leerSynchsafe32(&cab[6]);
  const uint32_t tamArchivo = (uint32_t)f.size();
  const uint32_t finTag = min(tamArchivo, 10U + tagBytes);

  // Estrategia deliberadamente robusta:
  // dentro del ID3 buscamos la firma JPEG real FF D8 FF y su EOI FF D9.
  // Así no dependemos de versión ID3, flags APIC, encoding de descripción,
  // BOM UTF-16, padding ni peculiaridades del programa que creó el tag.
  if (finTag <= 13) {
    f.close();
    return false;
  }

  if (!f.seek(10)) {
    f.close();
    return false;
  }

  uint32_t pos = 10;
  uint32_t jpgInicio = 0;
  int b0 = -1, b1 = -1;

  while (pos < finTag) {
    int b2 = f.read();
    if (b2 < 0) break;

    if (b0 == 0xFF && b1 == 0xD8 && b2 == 0xFF) {
      jpgInicio = pos - 2;
      break;
    }

    b0 = b1;
    b1 = b2;
    ++pos;
  }

  if (jpgInicio == 0) {
    f.close();
    Serial.printf("[MP3][APIC] ID3 sin firma JPEG: %s tag=%lu\n",
                  ruta.c_str(), (unsigned long)tagBytes);
    return false;
  }

  if (!f.seek(jpgInicio + 3)) {
    f.close();
    return false;
  }

  uint32_t scan = jpgInicio + 3;
  uint32_t jpgFin = 0;
  int prev = -1;

  while (scan < finTag) {
    int cur = f.read();
    if (cur < 0) break;

    if (prev == 0xFF && cur == 0xD9) {
      jpgFin = scan + 1; // posición inmediatamente posterior a FF D9
      break;
    }

    prev = cur;
    ++scan;
  }

  // Si no aparece EOI antes del fin del tag, usar hasta el fin del ID3.
  // TJpg_Decoder tolera bytes sobrantes tras el JPEG; aun así preferimos EOI.
  if (jpgFin == 0) jpgFin = finTag;

  if (jpgFin <= jpgInicio + 4) {
    f.close();
    return false;
  }

  out.offset = jpgInicio;
  out.bytes = jpgFin - jpgInicio;
  out.jpeg = true;

  Serial.printf("[MP3][APIC] JPEG bruto offset=%lu bytes=%lu tag=%lu\n",
                (unsigned long)out.offset,
                (unsigned long)out.bytes,
                (unsigned long)tagBytes);

  f.close();
  return true;
}

static constexpr int COVER_X = 10;
static constexpr int COVER_Y = 39;
static constexpr int COVER_W = 126;
static constexpr int COVER_H = 126;


// La portada ya no toca la TFT desde la tarea que lee/decodifica.
// Se decodifica a un buffer 126x126 en segundo plano y el loop principal
// hace un único push cuando el resultado está listo.
struct SolicitudPortadaMP3 {
  char ruta[256];
  uint32_t generacion;
};

static QueueHandle_t colaPortadaMP3 = nullptr;
static TaskHandle_t tareaPortadaMP3 = nullptr;
static volatile uint32_t generacionPortadaMP3 = 0;

static portMUX_TYPE muxPortadaMP3 = portMUX_INITIALIZER_UNLOCKED;
static uint16_t *resultadoPortadaMP3 = nullptr;
static uint32_t resultadoPortadaGeneracion = 0;
static bool resultadoPortadaListo = false;
static bool resultadoPortadaOK = false;

// Buffers exclusivos de la tarea de portada.
static uint16_t *bufferDecodePortada = nullptr;
static uint32_t generacionDecodePortada = 0;

static void dibujarPortadaVacia() {
  tft.fillRect(COVER_X, COVER_Y, COVER_W, COVER_H, TFT_BLACK);
  tft.drawRoundRect(COVER_X, COVER_Y, COVER_W, COVER_H, 8, TFT_DARKGREY);
}

static void dibujarCaratulaGenerica() {
  tft.fillRoundRect(COVER_X, COVER_Y, COVER_W, COVER_H, 8, 0x1082);
  tft.drawRoundRect(COVER_X, COVER_Y, COVER_W, COVER_H, 8, TFT_DARKGREY);

  const int cx = COVER_X + COVER_W / 2;
  const int cy = COVER_Y + COVER_H / 2;
  const uint16_t col = TFT_CYAN;
  tft.fillCircle(cx - 19, cy + 27, 12, col);
  tft.fillCircle(cx + 24, cy + 17, 12, col);
  tft.fillRect(cx - 9, cy - 38, 7, 66, col);
  tft.fillRect(cx + 34, cy - 49, 7, 67, col);
  tft.fillRect(cx - 5, cy - 49, 43, 9, col);
}

static int portadaOffsetX = 0;
static int portadaOffsetY = 0;
// Los JPEG baseline se procesan con TJpg_Decoder; los SOF2 con lowzag.
static bool copiarBloquePortadaBaseline(int16_t x, int16_t y,
                                       uint16_t w, uint16_t h, uint16_t *pixels) {
  if (!pixels || !bufferDecodePortada) return false;
  if (generacionDecodePortada != generacionPortadaMP3) return false;
  for (int dy = 0; dy < (int)h; ++dy) {
    int yy = portadaOffsetY + y + dy;
    if (yy < 0 || yy >= COVER_H) continue;
    for (int dx = 0; dx < (int)w; ++dx) {
      int xx = portadaOffsetX + x + dx;
      if (xx < 0 || xx >= COVER_W) continue;
      const uint16_t c = pixels[dy * w + dx];
      bufferDecodePortada[yy * COVER_W + xx] = (c >> 8) | (c << 8);
    }
  }
  return true;
}

static void publicarResultadoPortada(uint32_t generacion,
                                     uint16_t *buffer,
                                     bool ok) {
  uint16_t *viejo = nullptr;

  portENTER_CRITICAL(&muxPortadaMP3);
  viejo = resultadoPortadaMP3;
  resultadoPortadaMP3 = buffer;
  resultadoPortadaGeneracion = generacion;
  resultadoPortadaOK = ok;
  resultadoPortadaListo = true;
  portEXIT_CRITICAL(&muxPortadaMP3);

  if (viejo) heap_caps_free(viejo);
}

static void tareaCargaPortadaMP3(void *param) {
  (void)param;
  SolicitudPortadaMP3 req;

  for (;;) {
    if (xQueueReceive(colaPortadaMP3, &req, portMAX_DELAY) != pdTRUE) continue;

    // Si había varias pulsaciones rápidas, procesar siempre la solicitud
    // más reciente disponible antes de empezar trabajo costoso.
    SolicitudPortadaMP3 masReciente;
    while (xQueueReceive(colaPortadaMP3, &masReciente, 0) == pdTRUE) {
      req = masReciente;
    }

    const uint32_t gen = req.generacion;
    if (gen != generacionPortadaMP3) continue;

    PortadaMP3 p;
    if (!localizarPortadaAPIC(String(req.ruta), p) || !p.jpeg) {
      if (gen == generacionPortadaMP3) publicarResultadoPortada(gen, nullptr, false);
      continue;
    }

    if (gen != generacionPortadaMP3) continue;

    static constexpr uint32_t MAX_PORTADA = 2U * 1024U * 1024U;
    if (p.bytes == 0 || p.bytes > MAX_PORTADA) {
      Serial.printf("[MP3][APIC] portada omitida: %lu bytes\n",
                    (unsigned long)p.bytes);
      if (gen == generacionPortadaMP3) publicarResultadoPortada(gen, nullptr, false);
      continue;
    }

    uint8_t *jpg = (uint8_t*)heap_caps_malloc(
        p.bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!jpg) jpg = (uint8_t*)heap_caps_malloc(p.bytes, MALLOC_CAP_8BIT);
    if (!jpg) {
      Serial.printf("[MP3][APIC] sin RAM para %lu bytes\n",
                    (unsigned long)p.bytes);
      if (gen == generacionPortadaMP3) publicarResultadoPortada(gen, nullptr, false);
      continue;
    }

    File pf = SD_MMC.open(req.ruta, FILE_READ);
    const bool lecturaOK =
        pf && pf.seek(p.offset) && pf.read(jpg, p.bytes) == (int)p.bytes;
    if (pf) pf.close();

    if (!lecturaOK || gen != generacionPortadaMP3) {
      heap_caps_free(jpg);
      if (!lecturaOK && gen == generacionPortadaMP3)
        publicarResultadoPortada(gen, nullptr, false);
      continue;
    }

    uint16_t *cover = (uint16_t*)heap_caps_malloc(
        (size_t)COVER_W * COVER_H * sizeof(uint16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!cover) {
      cover = (uint16_t*)heap_caps_malloc(
          (size_t)COVER_W * COVER_H * sizeof(uint16_t),
          MALLOC_CAP_8BIT);
    }
    if (!cover) {
      heap_caps_free(jpg);
      if (gen == generacionPortadaMP3) publicarResultadoPortada(gen, nullptr, false);
      continue;
    }
    memset(cover, 0, (size_t)COVER_W * COVER_H * sizeof(uint16_t));

    bufferDecodePortada = cover;
    generacionDecodePortada = gen;

    bool ok = false;
    int jw = 0, jh = 0;
    const lowzag_err_t tipo = lowzag_probe(jpg, p.bytes, &jw, &jh);
    if (tipo == LOWZAG_OK) {
      ok = PortadaLowzag::decode(jpg, p.bytes, cover, COVER_W, COVER_H,
                                 &generacionPortadaMP3, gen, jw, jh);
    } else if (tipo == LOWZAG_ERR_NOT_PROGRESSIVE) {
      // Mantener las portadas baseline sin la dependencia Apache-2.0.
      uint16_t w = 0, h = 0;
      if (TJpgDec.getJpgSize(&w, &h, jpg, p.bytes) == 0 && w > 0 && h > 0) {
        jw = w; jh = h;
        portadaOffsetX = max(0, (COVER_W - (jw+7)/8)/2);
        portadaOffsetY = max(0, (COVER_H - (jh+7)/8)/2);
        TJpgDec.setSwapBytes(false);
        TJpgDec.setJpgScale(8);
        TJpgDec.setCallback(copiarBloquePortadaBaseline);
        ok = TJpgDec.drawJpg(0, 0, jpg, p.bytes) == 0;
        TJpgDec.setCallback(jpgDibujarBloque);
        TJpgDec.setJpgScale(1);
        TJpgDec.setSwapBytes(false);
      }
    }

    bufferDecodePortada = nullptr;
    heap_caps_free(jpg);

    if (gen != generacionPortadaMP3) {
      heap_caps_free(cover);
      continue;
    }

    if (!ok) {
      Serial.printf("[MP3][PORTADA BG] JPEG fallo %dx%d tipo=%d\n", jw, jh, (int)tipo);
      heap_caps_free(cover);
      publicarResultadoPortada(gen, nullptr, false);
      continue;
    }
    Serial.printf("[MP3][PORTADA BG] JPEG listo %dx%d progressive=%d\n",
                  jw, jh, tipo == LOWZAG_OK);
    publicarResultadoPortada(gen, cover, true);
  }
}

static bool asegurarTareaPortadaMP3() {
  if (!colaPortadaMP3) {
    colaPortadaMP3 = xQueueCreate(1, sizeof(SolicitudPortadaMP3));
    if (!colaPortadaMP3) return false;
  }

  if (!tareaPortadaMP3) {
    BaseType_t rc = xTaskCreatePinnedToCore(
        tareaCargaPortadaMP3,
        "MP3_Portada",
        6144,
        nullptr,
        1,              // debajo del audio MP3
        &tareaPortadaMP3,
        0);             // mismo core que audio, que tiene prioridad mayor
    if (rc != pdPASS) {
      tareaPortadaMP3 = nullptr;
      return false;
    }
  }
  return true;
}

static void cancelarPortadaMP3() {
  generacionPortadaMP3++;
  if (colaPortadaMP3) xQueueReset(colaPortadaMP3);

  uint16_t *viejo = nullptr;
  portENTER_CRITICAL(&muxPortadaMP3);
  viejo = resultadoPortadaMP3;
  resultadoPortadaMP3 = nullptr;
  resultadoPortadaListo = false;
  resultadoPortadaOK = false;
  portEXIT_CRITICAL(&muxPortadaMP3);

  if (viejo) heap_caps_free(viejo);
}

static void solicitarPortadaMP3(const String &ruta) {
  // El usuario pidió explícitamente no conservar la imagen de la canción
  // anterior mientras llega la nueva.
  dibujarPortadaVacia();

  if (!asegurarTareaPortadaMP3()) {
    Serial.println("[MP3][PORTADA BG] no se pudo crear tarea/cola");
    dibujarCaratulaGenerica();
    return;
  }

  SolicitudPortadaMP3 req{};
  req.generacion = ++generacionPortadaMP3;
  strncpy(req.ruta, ruta.c_str(), sizeof(req.ruta) - 1);
  req.ruta[sizeof(req.ruta) - 1] = 0;

  // Cola de una posición: una pulsación nueva sustituye a la anterior.
  xQueueOverwrite(colaPortadaMP3, &req);
}

static void procesarResultadoPortadaMP3() {
  uint16_t *buffer = nullptr;
  uint32_t gen = 0;
  bool listo = false;
  bool ok = false;

  portENTER_CRITICAL(&muxPortadaMP3);
  if (resultadoPortadaListo) {
    buffer = resultadoPortadaMP3;
    gen = resultadoPortadaGeneracion;
    ok = resultadoPortadaOK;
    listo = true;
    resultadoPortadaMP3 = nullptr;
    resultadoPortadaListo = false;
  }
  portEXIT_CRITICAL(&muxPortadaMP3);

  if (!listo) return;

  if (gen != generacionPortadaMP3 || !vistaPlayer) {
    if (buffer) heap_caps_free(buffer);
    return;
  }

  if (!ok || !buffer) {
    dibujarCaratulaGenerica();
    if (buffer) heap_caps_free(buffer);
    return;
  }

  // La portada ya está completamente decodificada: una sola escritura corta
  // en el loop principal. Los botones nunca quedan esperando al decoder JPEG.
  const bool swapAntes = tft.getSwapBytes();
  tft.setSwapBytes(false);
  tft.pushImage(COVER_X, COVER_Y, COVER_W, COVER_H, buffer);
  tft.setSwapBytes(swapAntes);
  tft.drawRoundRect(COVER_X, COVER_Y, COVER_W, COVER_H, 8, TFT_DARKGREY);

  heap_caps_free(buffer);
}

static String formatoTiempo(uint32_t ms) {
  const uint32_t seg = ms / 1000UL;
  const uint32_t m = seg / 60UL;
  const uint32_t s = seg % 60UL;
  char b[16];
  snprintf(b, sizeof(b), "%lu:%02lu", (unsigned long)m, (unsigned long)s);
  return String(b);
}

static void ordenarLista() {
  for (int i = 0; i < cantidad - 1; ++i) {
    for (int j = i + 1; j < cantidad; ++j) {
      if (canciones[j].compareTo(canciones[i]) < 0) {
        String ts = canciones[i]; canciones[i] = canciones[j]; canciones[j] = ts;
        uint32_t tz = tamanos[i]; tamanos[i] = tamanos[j]; tamanos[j] = tz;
        uint32_t td = duracionesMs[i]; duracionesMs[i] = duracionesMs[j]; duracionesMs[j] = td;
      }
    }
  }
}

static void cargarLista() {
  cantidad = 0;

  File dir = SD_MMC.open(CARPETA);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    Serial.println("[MP3] carpeta /mp3 no existe");
    seleccion = 0;
    primeroVisible = 0;
    return;
  }

  File f = dir.openNextFile();
  while (f && cantidad < MAX_CANCIONES) {
    if (!f.isDirectory()) {
      String nombre = String(f.name());
      String low = nombre;
      low.toLowerCase();
      if (low.endsWith(".mp3")) {
        String ruta = nombre;
        if (!ruta.startsWith("/")) ruta = String(CARPETA) + "/" + ruta;
        else if (!ruta.startsWith(String(CARPETA) + "/")) {
          int p = ruta.lastIndexOf('/');
          ruta = String(CARPETA) + "/" + (p >= 0 ? ruta.substring(p + 1) : ruta);
        }
        canciones[cantidad] = ruta;
        tamanos[cantidad] = (uint32_t)f.size();
        duracionesMs[cantidad] = 0;
        cantidad++;
      }
    }
    f.close();
    f = dir.openNextFile();
  }
  if (f) f.close();
  dir.close();

  ordenarLista();
  if (cantidad <= 0) seleccion = 0;
  else if (seleccion >= cantidad) seleccion = cantidad - 1;
  if (seleccion < 0) seleccion = 0;
  primeroVisible = max(0, seleccion - FILAS_VISIBLES + 1);

  Serial.printf("[MP3] %d canciones en /mp3\n", cantidad);
}

static void ajustarScroll() {
  if (seleccion < primeroVisible) primeroVisible = seleccion;
  if (seleccion >= primeroVisible + FILAS_VISIBLES)
    primeroVisible = seleccion - FILAS_VISIBLES + 1;
  if (primeroVisible < 0) primeroVisible = 0;
}

static void dibujarFilaListaMP3(int indice) {
#if COCODRILO_TIENE_MP3_DRMP3
  if (cantidad <= 0) return;
  if (indice < primeroVisible || indice >= primeroVisible + FILAS_VISIBLES) return;
  if (indice < 0 || indice >= cantidad) return;

  const int y0 = 38;
  const int alto = 22;
  const int fila = indice - primeroVisible;
  const int y = y0 + fila * alto;
  const bool sel = indice == seleccion;

  tft.fillRect(7, y - 1, SCR_W - 14, alto, TFT_BLACK);

  if (sel) {
    tft.fillRoundRect(8, y, SCR_W - 16, alto - 2, 5, 0x2104);
    tft.drawRoundRect(8, y, SCR_W - 16, alto - 2, 5, TFT_CYAN);
  }

  String nombre = nombreVisible(canciones[indice]);
  if (nombre.length() > 34) nombre = nombre.substring(0, 31) + "...";

  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(sel ? TFT_CYAN : TFT_WHITE, sel ? 0x2104 : TFT_BLACK);
  tft.drawString(String(sel ? "> " : "  ") + nombre, 14, y + 9, 1);
  tft.setTextDatum(TL_DATUM);
#endif
}

static void actualizarCursorListaMP3(int seleccionAnterior, int primeroAnterior) {
#if COCODRILO_TIENE_MP3_DRMP3
  if (primeroAnterior != primeroVisible) {
    const int y0 = 37;
    const int altoTotal = FILAS_VISIBLES * 22 + 2;
    tft.fillRect(0, y0, SCR_W, altoTotal, TFT_BLACK);

    const int fin = min(cantidad, primeroVisible + FILAS_VISIBLES);
    for (int i = primeroVisible; i < fin; ++i) dibujarFilaListaMP3(i);
    return;
  }

  dibujarFilaListaMP3(seleccionAnterior);
  dibujarFilaListaMP3(seleccion);
#endif
}

static void dibujarLista() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextFont(1);
  tft.setTextSize(1);

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("MUSICA", SCR_W / 2, 8, 2);
  tft.drawFastHLine(12, 31, SCR_W - 24, TFT_DARKGREY);

#if !COCODRILO_TIENE_MP3_DRMP3
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("Falta decodificador MP3", SCR_W / 2, 92, 2);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("Instala la libreria y recompila", SCR_W / 2, 118, 1);
  tft.drawString("dr_mp3.h", SCR_W / 2, 136, 1);
#else
  if (cantidad == 0) {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString("No hay archivos MP3", SCR_W / 2, 105, 2);
    tft.drawString("Crea /mp3 en la microSD", SCR_W / 2, 132, 1);
  } else {
    const int y0 = 38;
    const int alto = 22;
    const int fin = min(cantidad, primeroVisible + FILAS_VISIBLES);

    for (int i = primeroVisible; i < fin; ++i) {
      const int y = y0 + (i - primeroVisible) * alto;
      const bool sel = i == seleccion;
      if (sel) {
        tft.fillRoundRect(8, y, SCR_W - 16, alto - 2, 5, 0x2104);
        tft.drawRoundRect(8, y, SCR_W - 16, alto - 2, 5, TFT_CYAN);
      }

      String nombre = nombreVisible(canciones[i]);
      if (nombre.length() > 34) nombre = nombre.substring(0, 31) + "...";

      tft.setTextDatum(ML_DATUM);
      tft.setTextColor(sel ? TFT_CYAN : TFT_WHITE, sel ? 0x2104 : TFT_BLACK);
      tft.drawString(String(sel ? "> " : "  ") + nombre, 14, y + 9, 1);
    }
  }
#endif

  tft.fillRect(0, SCR_H - 25, SCR_W, 25, 0x1082);
  tft.drawFastHLine(0, SCR_H - 25, SCR_W, TFT_DARKGREY);
  tft.setTextDatum(BC_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, 0x1082);
#if COCODRILO_TIENE_MP3_DRMP3
  tft.drawString("Arr/Ab elegir   A reproducir   MENU volver", SCR_W/2, SCR_H-6, 1);
#else
  tft.drawString("MENU: volver", SCR_W/2, SCR_H-6, 1);
#endif
  tft.setTextDatum(TL_DATUM);
}

struct HeaderMP3Info {
  uint32_t sampleRate;
  uint16_t bitrateKbps;
  uint16_t frameBytes;
  uint16_t samplesPerFrame;
};

static bool leerHeaderMP3(File &f, uint32_t pos, HeaderMP3Info &out) {
  if (!f.seek(pos)) return false;

  uint8_t h[4];
  if (f.read(h, 4) != 4) return false;

  // Sync MPEG: 11 bits a 1.
  if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return false;

  const uint8_t versionBits = (h[1] >> 3) & 0x03;
  const uint8_t layerBits   = (h[1] >> 1) & 0x03;
  const uint8_t brIndex     = (h[2] >> 4) & 0x0F;
  const uint8_t srIndex     = (h[2] >> 2) & 0x03;
  const uint8_t padding     = (h[2] >> 1) & 0x01;

  // version 01 reservada; layer 01 = Layer III.
  if (versionBits == 1 || layerBits != 1) return false;
  if (brIndex == 0 || brIndex == 15 || srIndex == 3) return false;

  static const uint16_t brMpeg1L3[16] = {
    0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0
  };
  static const uint16_t brMpeg2L3[16] = {
    0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0
  };
  static const uint32_t srBase[3] = {44100, 48000, 32000};

  const bool mpeg1 = (versionBits == 3);
  const bool mpeg25 = (versionBits == 0);

  uint32_t sr = srBase[srIndex];
  if (!mpeg1) sr /= 2;
  if (mpeg25) sr /= 2;

  const uint16_t br = mpeg1 ? brMpeg1L3[brIndex] : brMpeg2L3[brIndex];
  if (sr == 0 || br == 0) return false;

  const uint16_t frameBytes = (uint16_t)(
      ((mpeg1 ? 144000UL : 72000UL) * (uint32_t)br) / sr + padding);

  if (frameBytes < 24) return false;

  out.sampleRate = sr;
  out.bitrateKbps = br;
  out.frameBytes = frameBytes;
  out.samplesPerFrame = mpeg1 ? 1152 : 576;
  return true;
}

// Busca el siguiente frame valido sin confundir basura/ID3 con audio.
// El limite pequeno solo se usa para resincronizar si hay metadata entre frames.
static bool buscarFrameMP3(File &f, uint32_t desde, uint32_t limite,
                           uint32_t &posEncontrada, HeaderMP3Info &info) {
  const uint32_t finArchivo = (uint32_t)f.size();
  uint32_t fin = desde + limite;
  if (fin < desde || fin > finArchivo) fin = finArchivo;

  for (uint32_t p = desde; p + 4 <= fin; ++p) {
    if (leerHeaderMP3(f, p, info)) {
      posEncontrada = p;
      return true;
    }
  }
  return false;
}

// Duracion RAPIDA: solo inspecciona el comienzo del MP3.
// 1) Xing/Info -> numero real de frames (ideal para VBR).
// 2) VBRI      -> numero real de frames.
// 3) CBR       -> tamaño de audio / bitrate del primer frame.
// No recorre toda la cancion, asi que entrar al player es practicamente inmediato.
static uint32_t calcularDuracionRapidaMP3(const String &ruta) {
  File f = SD_MMC.open(ruta, FILE_READ);
  if (!f || f.isDirectory()) {
    if (f) f.close();
    return 0;
  }

  const uint32_t tam = (uint32_t)f.size();
  if (tam < 4) {
    f.close();
    return 0;
  }

  HeaderMP3Info hi;
  uint32_t pos = 0;

  // Saltar ID3v2 si existe.
  uint8_t id3[10];
  if (f.seek(0) && f.read(id3, sizeof(id3)) == (int)sizeof(id3) &&
      id3[0] == 'I' && id3[1] == 'D' && id3[2] == '3') {
    const uint32_t tagSize =
      ((uint32_t)(id3[6] & 0x7F) << 21) |
      ((uint32_t)(id3[7] & 0x7F) << 14) |
      ((uint32_t)(id3[8] & 0x7F) << 7)  |
      ((uint32_t)(id3[9] & 0x7F));
    pos = 10 + tagSize;
    if (id3[3] == 4 && (id3[5] & 0x10)) pos += 10;
  }

  uint32_t framePos = 0;
  const uint32_t margen = (pos < tam) ? min((uint32_t)262144, tam - pos) : 0;
  if (!margen || !buscarFrameMP3(f, pos, margen, framePos, hi)) {
    f.close();
    return 0;
  }

  // Leer solo el primer frame y buscar cabeceras VBR conocidas.
  const uint32_t leerN = min((uint32_t)hi.frameBytes, (uint32_t)1024);
  uint8_t primerFrame[1024];
  uint32_t framesTotales = 0;

  if (leerN >= 24 && f.seek(framePos) &&
      f.read(primerFrame, leerN) == (int)leerN) {

    auto be32 = [](const uint8_t *p) -> uint32_t {
      return ((uint32_t)p[0] << 24) |
             ((uint32_t)p[1] << 16) |
             ((uint32_t)p[2] << 8)  |
             (uint32_t)p[3];
    };

    for (uint32_t i = 4; i + 18 <= leerN; ++i) {
      // Xing / Info
      if ((primerFrame[i] == 'X' && primerFrame[i+1] == 'i' &&
           primerFrame[i+2] == 'n' && primerFrame[i+3] == 'g') ||
          (primerFrame[i] == 'I' && primerFrame[i+1] == 'n' &&
           primerFrame[i+2] == 'f' && primerFrame[i+3] == 'o')) {
        const uint32_t flags = be32(&primerFrame[i + 4]);
        uint32_t p = i + 8;
        if ((flags & 0x0001) && p + 4 <= leerN) {
          framesTotales = be32(&primerFrame[p]);
        }
        break;
      }

      // VBRI: campo "frames" a +14 bytes desde la firma.
      if (primerFrame[i] == 'V' && primerFrame[i+1] == 'B' &&
          primerFrame[i+2] == 'R' && primerFrame[i+3] == 'I') {
        if (i + 18 <= leerN) framesTotales = be32(&primerFrame[i + 14]);
        break;
      }
    }
  }

  if (framesTotales > 0 && hi.sampleRate > 0) {
    const uint32_t ms = (uint32_t)(
      ((uint64_t)framesTotales * hi.samplesPerFrame * 1000ULL) / hi.sampleRate);
    f.close();
    Serial.printf("[MP3] duracion Xing/VBRI: %lu ms\n", (unsigned long)ms);
    return ms;
  }

  // Fallback CBR. Quitar ID3v1 si existe al final.
  uint32_t finAudio = tam;
  if (tam >= 128) {
    uint8_t tag[3];
    if (f.seek(tam - 128) && f.read(tag, 3) == 3 &&
        tag[0] == 'T' && tag[1] == 'A' && tag[2] == 'G') {
      finAudio -= 128;
    }
  }
  f.close();

  if (hi.bitrateKbps == 0 || finAudio <= framePos) return 0;

  const uint32_t bytesAudio = finAudio - framePos;
  const uint32_t ms = (uint32_t)(
      ((uint64_t)bytesAudio * 8ULL * 1000ULL) /
      ((uint64_t)hi.bitrateKbps * 1000ULL));

  Serial.printf("[MP3] duracion CBR rapida: %lu ms\n", (unsigned long)ms);
  return ms;
}

static uint32_t duracionAproxMs() {
  // Principal: duracion calculada desde los headers reales de TODOS los frames.
  if (duracionTotalMs > 0) return duracionTotalMs;

  // Fallback solamente si el archivo no pudo analizarse.
  uint32_t br = bitrateBase;
  if (br > 0 && br < 1000) br *= 1000;
  if (br == 0 || bytesTotales == 0) return 0;
  return (uint32_t)(((uint64_t)bytesTotales * 8ULL * 1000ULL) / br);
}

static uint32_t tiempoAproxMs() {
  // Ya no inferimos el tiempo por bytes/bitrate: el decoder puede leer por adelantado
  // y en VBR el bitrate cambia. Usamos el PCM que realmente se mando al I2S.
  uint32_t t = tiempoReproMs;
  const uint32_t dur = duracionAproxMs();
  if (dur && t > dur) t = dur;
  return t;
}

static void dibujarVolumenOSD() {
  const int x = 148, y = 132, w = 162, h = 24;
  tft.fillRoundRect(x, y, w, h, 6, TFT_BLACK);
  tft.drawRoundRect(x, y, w, h, 6, TFT_WHITE);
  const int bx = x + 34, by = y + 8, bw = w - 47, bh = 8;
  tft.drawRect(bx, by, bw, bh, TFT_DARKGREY);
  const int relleno = ((bw - 2) * volumenPaso) / VOLUME_PASOS_MAX;
  if (relleno > 0) tft.fillRect(bx + 1, by + 1, relleno, bh - 2, TFT_CYAN);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(String(volumenPaso), x + 12, y + h/2, 1);
  tft.setTextDatum(TL_DATUM);
}

static void dibujarProgresoPlayer() {
  const uint32_t dur = duracionAproxMs();
  const uint32_t act = tiempoAproxMs();

  const int bx = 24, by = 174, bw = SCR_W - 48, bh = 9;

  // Refresco parcial: barra, tiempos e icono central.
  tft.fillRect(bx + 1, by + 1, bw - 2, bh - 2, TFT_BLACK);
  tft.drawRect(bx, by, bw, bh, TFT_DARKGREY);

  int fill = 0;
  if (dur > 0) fill = (int)(((uint64_t)(bw - 2) * min(act, dur)) / dur);
  if (fill > 0) tft.fillRect(bx + 1, by + 1, fill, bh - 2, TFT_CYAN);

  // Línea inferior: tiempo actual | estado gráfico | duración.
  // El icono se dibuja con primitivas para no depender de glifos Unicode.
  tft.fillRect(bx, 186, bw, 15, TFT_BLACK);

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString(formatoTiempo(act), bx, 188, 1);

  tft.setTextDatum(TR_DATUM);
  tft.drawString(dur ? formatoTiempo(dur) : String("--:--"), bx + bw, 188, 1);

  const int cx = SCR_W / 2;
  const int cy = 193;
  const uint16_t iconColor = pausado ? TFT_YELLOW : TFT_WHITE;

  if (pausado) {
    // Equivalente visual a ⏯: play + pausa.
    tft.fillTriangle(cx - 10, cy - 6,
                     cx - 10, cy + 6,
                     cx - 2,  cy,
                     iconColor);
    tft.fillRect(cx + 3, cy - 6, 3, 12, iconColor);
    tft.fillRect(cx + 8, cy - 6, 3, 12, iconColor);
  } else {
    // Equivalente visual a ⏸.
    tft.fillRect(cx - 6, cy - 6, 4, 12, iconColor);
    tft.fillRect(cx + 2, cy - 6, 4, 12, iconColor);
  }

  tft.setTextDatum(TL_DATUM);
}

static void dibujarControlesPlayer() {
  tft.fillRect(0, SCR_H - 36, SCR_W, 36, 0x1082);
  tft.drawFastHLine(0, SCR_H - 36, SCR_W, TFT_DARKGREY);
  tft.setTextDatum(MC_DATUM);

  if (controlesBloqueados) {
    tft.setTextColor(TFT_CYAN, 0x1082);
    tft.drawString("CONTROLES BLOQUEADOS", SCR_W / 2, SCR_H - 23, 1);
    tft.setTextColor(TFT_LIGHTGREY, 0x1082);
    tft.drawString("Manten B 2 s para desbloquear", SCR_W / 2, SCR_H - 9, 1);
  } else {
    tft.setTextColor(TFT_LIGHTGREY, 0x1082);
    tft.drawString("A pausa   <- anterior   siguiente ->", SCR_W / 2, SCR_H - 23, 1);
    tft.drawString("Arr/Ab volumen   B bloquear   MENU volver", SCR_W / 2, SCR_H - 9, 1);
  }

  tft.setTextDatum(TL_DATUM);
}

static void dibujarPlayer(bool completo = true) {
  if (!completo) {
    dibujarProgresoPlayer();

    if ((int32_t)(volumenOSDHasta - millis()) > 0) {
      dibujarVolumenOSD();
      volumenOSDVisible = true;
    } else if (volumenOSDVisible) {
      // El OSD vive a la derecha de la portada. Se limpia una sola vez.
      tft.fillRect(144, 128, 170, 34, TFT_BLACK);
      volumenOSDVisible = false;
    }
    return;
  }

  tft.fillScreen(TFT_BLACK);

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("COCODRILO MUSIC", SCR_W / 2, 8, 2);

  String nombre = cantidad > 0 ? nombreVisible(canciones[seleccion]) : String("---");

  // Reservar visualmente el área; la portada llegará en segundo plano.
  dibujarPortadaVacia();

  // Nombre a la derecha de la portada. Dos líneas cortas para no invadirla.
  String l1 = nombre;
  String l2 = "";
  if (l1.length() > 20) {
    int corte = 20;
    for (int i = min(20, (int)l1.length() - 1); i >= 8; --i) {
      if (l1[i] == ' ') { corte = i; break; }
    }
    l2 = l1.substring(corte);
    l2.trim();
    l1 = l1.substring(0, corte);
  }
  if (l1.length() > 20) l1 = l1.substring(0, 17) + "...";
  if (l2.length() > 20) l2 = l2.substring(0, 17) + "...";

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(l1, 148, 53, 2);
  if (l2.length()) tft.drawString(l2, 148, 77, 2);

  dibujarProgresoPlayer();

  dibujarControlesPlayer();

  if ((int32_t)(volumenOSDHasta - millis()) > 0) {
    dibujarVolumenOSD();
    volumenOSDVisible = true;
  }

  // La lectura del APIC/JPEG ocurre fuera del loop principal.
  if (cantidad > 0) solicitarPortadaMP3(canciones[seleccion]);
}

// Cambio de canción mientras ya estamos dentro del player:
// no reconstruir pantalla, cabecera ni controles. La portada anterior permanece
// visible durante la búsqueda/lectura del nuevo APIC y solo cambia al decodificar.
static void actualizarCancionPlayerParcial() {
  if (cantidad <= 0) return;

  String nombre = nombreVisible(canciones[seleccion]);
  String l1 = nombre;
  String l2 = "";
  if (l1.length() > 20) {
    int corte = 20;
    for (int i = min(20, (int)l1.length() - 1); i >= 8; --i) {
      if (l1[i] == ' ') { corte = i; break; }
    }
    l2 = l1.substring(corte);
    l2.trim();
    l1 = l1.substring(0, corte);
  }
  if (l1.length() > 20) l1 = l1.substring(0, 17) + "...";
  if (l2.length() > 20) l2 = l2.substring(0, 17) + "...";

  // Solo las zonas que realmente cambian entre canciones.
  tft.fillRect(144, 39, SCR_W - 144, 89, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(l1, 148, 53, 2);
  if (l2.length()) tft.drawString(l2, 148, 77, 2);

  dibujarProgresoPlayer();

  // Borrar inmediatamente la carátula anterior. La nueva se carga aparte,
  // por lo que volumen/cambio de canción siguen respondiendo mientras llega.
  solicitarPortadaMP3(canciones[seleccion]);
}

#if COCODRILO_TIENE_MP3_DRMP3

static inline int16_t aplicarFadeInMP3(int16_t v) {
  const uint32_t total = fadeInFramesTotal;
  const uint32_t left = fadeInFramesLeft;
  if (total == 0 || left == 0) return v;

  const uint32_t hecho = total - left;
  return (int16_t)(((int32_t)v * (int32_t)hecho) / (int32_t)total);
}

static inline void avanzarFadeInMP3() {
  if (fadeInFramesLeft > 0) --fadeInFramesLeft;
}

static void armarFadeInMP3(uint32_t sr) {
  if (sr == 0) sr = 44100;
  // 10 ms: suficiente para quitar el escalon sin sentirse como un fade audible.
  fadeInFramesTotal = max((uint32_t)1, sr / 100U);
  fadeInFramesLeft = fadeInFramesTotal;
  ultimoL = 0;
  ultimoR = 0;
}

static void clickEntradaMP3() {
  if (!i2sTomado) return;

  // Click corto y controlado en el MISMO MAX98357A.
  // Sirve como feedback de "cancion abierta" y camufla el pequeño transitorio real.
  const uint32_t sr = sampleRateActual ? sampleRateActual : 44100;
  const uint32_t frames = max((uint32_t)32, sr / 250U); // ~4 ms
  int16_t buf[256 * 2];
  const uint32_t n = min(frames, (uint32_t)256);

  for (uint32_t i = 0; i < n; ++i) {
    // Pulso bipolar con decaimiento rápido, volumen deliberadamente bajo.
    const int32_t amp = (int32_t)(1100L * (int32_t)(n - i) / (int32_t)n);
    const int16_t v = (i & 1) ? (int16_t)-amp : (int16_t)amp;
    buf[i * 2] = v;
    buf[i * 2 + 1] = v;
  }

  size_t escritos = 0;
  i2s_write(AudioMAX98357::I2S_PORT, buf, n * 4, &escritos, portMAX_DELAY);
  i2s_zero_dma_buffer(AudioMAX98357::I2S_PORT);
  vTaskDelay(pdMS_TO_TICKS(2));
}

static void fadeOutMP3() {
  if (!i2sTomado) return;

  // Congelar al decoder entre bloques antes de escribir nuestra rampa.
  pausado = true;
  vTaskDelay(pdMS_TO_TICKS(3));

  const uint32_t sr = sampleRateActual ? sampleRateActual : 44100;
  const uint32_t framesTotal = max((uint32_t)1, sr / 125U); // ~8 ms
  int16_t inicioL = ultimoL;
  int16_t inicioR = ultimoR;

  int16_t buf[128 * 2];
  uint32_t hechos = 0;

  while (hechos < framesTotal) {
    const uint32_t n = min((uint32_t)128, framesTotal - hechos);
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t pos = hechos + i;
      const uint32_t quedan = framesTotal - pos;
      buf[i * 2]     = (int16_t)(((int32_t)inicioL * (int32_t)quedan) / (int32_t)framesTotal);
      buf[i * 2 + 1] = (int16_t)(((int32_t)inicioR * (int32_t)quedan) / (int32_t)framesTotal);
    }
    size_t escritos = 0;
    i2s_write(AudioMAX98357::I2S_PORT, buf, n * 4, &escritos, portMAX_DELAY);
    hechos += n;
  }

  ultimoL = 0;
  ultimoR = 0;
  i2s_zero_dma_buffer(AudioMAX98357::I2S_PORT);
  vTaskDelay(pdMS_TO_TICKS(2));
}

static inline int escalaVolumenMP3(uint8_t paso, bool audifonos) {
  if (paso > 10) paso = 10;

  // El PCM5102A tiene margen limpio y no necesita la protección de la bocina.
  static const uint8_t escalaAudifonos[11] = {
    0, 7, 14, 22, 31, 41, 52, 64, 72, 79, 86
  };

  // Refuerzo digital moderado del MP3 (+6 puntos al maximo).
  // Mantener margen en altavoz pequeno: no escalar al 100%.
  static const uint8_t escalaAltavoz[11] = {
    0, 7, 14, 22, 31, 41, 48, 52, 55, 58, 60
  };

  return audifonos ? escalaAudifonos[paso] : escalaAltavoz[paso];
}

static int32_t hpfPrevXL = 0;
static int32_t hpfPrevXR = 0;
static int32_t hpfPrevYL = 0;
static int32_t hpfPrevYR = 0;
static uint32_t hpfSampleRate = 0;

static inline void reiniciarFiltroGravesMP3() {
  hpfPrevXL = hpfPrevXR = 0;
  hpfPrevYL = hpfPrevYR = 0;
  hpfSampleRate = 0;
}

static inline int16_t filtrarGravesAltavozMP3(int32_t x, bool derecha, uint32_t sr) {
  if (sr == 0) sr = 44100;

  if (hpfSampleRate != sr) {
    hpfPrevXL = hpfPrevXR = 0;
    hpfPrevYL = hpfPrevYR = 0;
    hpfSampleRate = sr;
  }

  // Pasa-altos de 1er orden ~120 Hz:
  // y[n] = a * (y[n-1] + x[n] - x[n-1])
  // a = fs / (fs + 2*pi*120), representado en Q15.
  const int32_t alphaQ15 =
      (int32_t)(((uint64_t)sr * 32768ULL) / ((uint64_t)sr + 754ULL));

  int32_t &xp = derecha ? hpfPrevXR : hpfPrevXL;
  int32_t &yp = derecha ? hpfPrevYR : hpfPrevYL;

  const int32_t y = (int32_t)(((int64_t)alphaQ15 *
      ((int64_t)yp + (int64_t)x - (int64_t)xp)) >> 15);

  xp = x;
  yp = y;

  if (y > 32767) return 32767;
  if (y < -32768) return -32768;
  return (int16_t)y;
}

static void pcmCallback(PCMFrameInfoCommunity &info, short *pcm, size_t len, void *ref) {
  (void)ref;
  if (detener || pausado || len == 0 || pcm == nullptr) return;

  if (info.samprate > 0 && (uint32_t)info.samprate != sampleRateActual) {
    // Evita el escalon audible al cambiar el reloj del I2S.
    i2s_zero_dma_buffer(AudioMAX98357::I2S_PORT);
    vTaskDelay(pdMS_TO_TICKS(2));
    sampleRateActual = (uint32_t)info.samprate;
    i2s_set_clk(AudioMAX98357::I2S_PORT, sampleRateActual,
                I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
    i2s_zero_dma_buffer(AudioMAX98357::I2S_PORT);
    armarFadeInMP3(sampleRateActual);
  }
  canalesActual = (uint8_t)max(1, info.nChans);
  if (info.bitrate > 0) {
    bitrateActual = (uint32_t)info.bitrate;
    if (bitrateBase == 0) bitrateBase = bitrateActual;
  }

  uint8_t paso = AudioMAX98357::getVolumeStep();
  if (paso > 10) paso = 10;
  const bool audifonos = AudioMAX98357::hayAudifonos();
  const int escala = AudioMAX98357::isEnabled()
      ? escalaVolumenMP3(paso, audifonos)
      : 0;
  const uint32_t srFiltro =
      info.samprate > 0 ? (uint32_t)info.samprate : sampleRateActual;

  // El PCM5102A queda completamente sin filtro. Al usar audifonos limpiamos
  // el estado para que, al volver a bocina, el HPF arranque sin memoria vieja.
  if (audifonos) reiniciarFiltroGravesMP3();

  // dr_mp3 entrega len = cantidad total de muestras PCM (intercaladas).
  if (info.nChans == 1) {
    static int16_t stereo[2304]; // hasta 1152 frames mono duplicados.
    size_t frames = min((size_t)1152, len);
    for (size_t i = 0; i < frames; ++i) {
      int32_t v32 = ((int32_t)pcm[i] * escala) / 100;
      if (!audifonos) v32 = filtrarGravesAltavozMP3(v32, false, srFiltro);
      if (v32 > 32767) v32 = 32767;
      if (v32 < -32768) v32 = -32768;
      int16_t v = (int16_t)v32;
      v = aplicarFadeInMP3(v);
      stereo[i*2] = v;
      stereo[i*2+1] = v;
      ultimoL = v;
      ultimoR = v;
      avanzarFadeInMP3();
    }
    size_t escritos = 0;
    i2s_write(AudioMAX98357::I2S_PORT, stereo, frames * 4,
              &escritos, portMAX_DELAY);
  } else {
    // Modificar en el buffer temporal del decoder es válido hasta retornar.
    const size_t framesStereo = len / 2;
    for (size_t f = 0; f < framesStereo; ++f) {
      int32_t l32 = ((int32_t)pcm[f*2] * escala) / 100;
      int32_t r32 = ((int32_t)pcm[f*2+1] * escala) / 100;
      if (!audifonos) {
        l32 = filtrarGravesAltavozMP3(l32, false, srFiltro);
        r32 = filtrarGravesAltavozMP3(r32, true, srFiltro);
      }
      if (l32 > 32767) l32 = 32767;
      if (l32 < -32768) l32 = -32768;
      if (r32 > 32767) r32 = 32767;
      if (r32 < -32768) r32 = -32768;
      int16_t l = (int16_t)l32;
      int16_t r = (int16_t)r32;
      l = aplicarFadeInMP3(l);
      r = aplicarFadeInMP3(r);
      pcm[f*2] = l;
      pcm[f*2+1] = r;
      ultimoL = l;
      ultimoR = r;
      avanzarFadeInMP3();
    }
    size_t escritos = 0;
    i2s_write(AudioMAX98357::I2S_PORT, pcm, len * sizeof(short),
              &escritos, portMAX_DELAY);
  }

  // Avanzar el reloj con los frames PCM realmente enviados al I2S.
  const uint32_t sr = info.samprate > 0 ? (uint32_t)info.samprate : sampleRateActual;
  const uint32_t ch = info.nChans > 0 ? (uint32_t)info.nChans : 2U;
  if (sr > 0 && ch > 0) {
    const uint32_t frames = (uint32_t)(len / ch);
    tiempoReproMs += (uint32_t)(((uint64_t)frames * 1000ULL) / sr);
  }
}

// Adaptador de lectura SD_MMC para el decodificador MP3 permisivo.
static size_t leerMP3SD(void* ctx, void* dst, size_t n) {
  File* f = static_cast<File*>(ctx);
  return (f && *f) ? f->read(static_cast<uint8_t*>(dst), n) : 0;
}
static drmp3_bool32 seekMP3SD(void* ctx, int offset, drmp3_seek_origin origin) {
  File* f = static_cast<File*>(ctx);
  if (!f || !*f) return DRMP3_FALSE;
  int64_t base = origin == DRMP3_SEEK_CUR ? (int64_t)f->position() :
                 origin == DRMP3_SEEK_END ? (int64_t)f->size() : 0;
  int64_t pos = base + (int64_t)offset;
  if (pos < 0 || (uint64_t)pos > f->size()) return DRMP3_FALSE;
  return f->seek((uint32_t)pos) ? DRMP3_TRUE : DRMP3_FALSE;
}
static drmp3_bool32 tellMP3SD(void* ctx, drmp3_int64* pos) {
  File* f = static_cast<File*>(ctx);
  if (!f || !*f || !pos) return DRMP3_FALSE;
  *pos = f->position();
  return DRMP3_TRUE;
}
static void tareaMP3(void*) {
  decoder = new (std::nothrow) drmp3{};
  int16_t* pcm = static_cast<int16_t*>(
      heap_caps_malloc(1152U * 2U * sizeof(int16_t), MALLOC_CAP_8BIT));
  if (!decoder || !pcm) {
    Serial.println("[MP3] dr_mp3 sin memoria");
    delete decoder; decoder = nullptr;
    if (pcm) heap_caps_free(pcm);
    if (archivo) archivo.close();
    terminado = true; reproduciendo = false; tarea = nullptr;
    vTaskDelete(nullptr); return;
  }
  while (!detener && archivo) {
    if (!drmp3_init(decoder, leerMP3SD, seekMP3SD, tellMP3SD,
                    nullptr, &archivo, nullptr)) {
      Serial.println("[MP3] no se pudo iniciar dr_mp3"); break;
    }
    PCMFrameInfoCommunity info = {
      (int)decoder->sampleRate, (int)decoder->channels, (int)bitrateBase};
    if (info.samprate <= 0 || info.nChans < 1 || info.nChans > 2) {
      drmp3_uninit(decoder); break;
    }
    while (!detener) {
      if (pausado) { vTaskDelay(pdMS_TO_TICKS(4)); continue; }
      drmp3_uint64 frames = drmp3_read_pcm_frames_s16(decoder, 1152, pcm);
      bytesLeidos = archivo.position();
      if (!frames) break;
      info.samprate = (int)decoder->sampleRate;
      info.nChans = (int)decoder->channels;
      pcmCallback(info, pcm, (size_t)frames * (size_t)info.nChans, nullptr);
      taskYIELD();
    }
    drmp3_uninit(decoder);
    if (detener || !modoFondoLoop) break;
    if (archivo) archivo.close();
    archivo = SD_MMC.open(rutaFondoLoop, FILE_READ);
    if (!archivo || archivo.isDirectory()) break;
    bytesLeidos = 0; tiempoReproMs = 0;
    armarFadeInMP3(sampleRateActual);
  }
  heap_caps_free(pcm); delete decoder; decoder = nullptr;
  if (archivo) archivo.close();
  terminado = true; reproduciendo = false; tarea = nullptr;
  vTaskDelete(nullptr);
}

static void detenerActual() {
  // Anti-pop solo del MP3: bajar desde la ultima muestra real hasta cero.
  if (reproduciendo && i2sTomado) fadeOutMP3();

  detener = true;
  pausado = false;
  modoFondoLoop = false;
  rutaFondoLoop = "";

  const uint32_t t0 = millis();
  while (tarea != nullptr && (millis() - t0) < 600) {
    vTaskDelay(pdMS_TO_TICKS(2));
  }

  if (archivo) archivo.close();
  i2s_zero_dma_buffer(AudioMAX98357::I2S_PORT);

  // El audio general de COCODRILO siempre vuelve a 44.1 kHz.
  i2s_set_clk(AudioMAX98357::I2S_PORT, 44100,
              I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);

  if (i2sTomado) {
    AudioMAX98357::salirModoGaleria();
    i2sTomado = false;
  }

  reproduciendo = false;
  terminado = false;
  detener = false;
  bytesLeidos = 0;
  bytesTotales = 0;
  duracionTotalMs = 0;
  bitrateActual = 0;
  bitrateBase = 0;
  tiempoReproMs = 0;
  sampleRateActual = 44100;
  canalesActual = 2;
  fadeInFramesLeft = 0;
  fadeInFramesTotal = 0;
  ultimoL = 0;
  ultimoR = 0;
  reiniciarFiltroGravesMP3();
}

static bool iniciarSeleccion() {
  if (cantidad <= 0) return false;

  const bool playerYaVisible = vistaPlayer;
  detenerActual();

  archivo = SD_MMC.open(canciones[seleccion], FILE_READ);
  if (!archivo || archivo.isDirectory()) {
    if (archivo) archivo.close();
    Serial.printf("[MP3] no se pudo abrir %s\n", canciones[seleccion].c_str());
    return false;
  }

  bytesTotales = (uint32_t)archivo.size();
  bytesLeidos = 0;

  // Duracion rapida: Xing/Info/VBRI o fallback CBR; queda en cache.
  if (duracionesMs[seleccion] == 0) {
    duracionesMs[seleccion] = calcularDuracionRapidaMP3(canciones[seleccion]);
  }
  duracionTotalMs = duracionesMs[seleccion];

  // Bitrate promedio estimado para mostrarlo en el reproductor.
  bitrateBase = duracionTotalMs ? (uint32_t)(((uint64_t)bytesTotales * 8000ULL) / duracionTotalMs) : 0;
  bitrateActual = bitrateBase;
  tiempoReproMs = 0;
  sampleRateActual = 44100;
  canalesActual = 2;
  detener = false;
  pausado = false;
  terminado = false;

  // Cede cooperativamente I2S_NUM_1 igual que la galería WAV.
  if (!AudioMAX98357::entrarModoGaleria()) {
    archivo.close();
    Serial.println("[MP3] no se pudo tomar I2S");
    return false;
  }
  i2sTomado = true;

  // Arranque silencioso: limpiar DMA, fijar reloj y entrar con rampa de 10 ms.
  i2s_zero_dma_buffer(AudioMAX98357::I2S_PORT);
  vTaskDelay(pdMS_TO_TICKS(2));
  i2s_set_clk(AudioMAX98357::I2S_PORT, 44100,
              I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
  i2s_zero_dma_buffer(AudioMAX98357::I2S_PORT);

  clickEntradaMP3();
  armarFadeInMP3(44100);

  BaseType_t ok = xTaskCreatePinnedToCore(
      tareaMP3, "COCODRILO_MP3", 12288, nullptr, 3, &tarea, 0);

  if (ok != pdPASS) {
    tarea = nullptr;
    archivo.close();
    AudioMAX98357::salirModoGaleria();
    i2sTomado = false;
    Serial.println("[MP3] no se pudo crear tarea");
    return false;
  }

  reproduciendo = true;
  vistaPlayer = true;
  Serial.printf("[MP3] play: %s\n", canciones[seleccion].c_str());

  if (playerYaVisible) {
    actualizarCancionPlayerParcial();
  } else {
    dibujarPlayer(true);
  }
  return true;
}

// Busca "stars.mp3" dentro de /mp3 sin depender de mayúsculas/minúsculas.
static String buscarStarsMP3() {
  if (SD_MMC.cardType() == CARD_NONE) return String();

  File dir = SD_MMC.open(CARPETA);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return String();
  }

  String encontrada;
  File f = dir.openNextFile();
  while (f) {
    if (!f.isDirectory()) {
      String nombre = String(f.name());
      int slash = nombre.lastIndexOf('/');
      if (slash >= 0) nombre = nombre.substring(slash + 1);
      String low = nombre;
      low.toLowerCase();
      if (low == "stars.mp3") {
        encontrada = String(CARPETA) + "/" + nombre;
        f.close();
        break;
      }
    }
    f.close();
    f = dir.openNextFile();
  }
  if (f) f.close();
  dir.close();
  return encontrada;
}

// Arranca Stars como música de fondo en loop, sin dibujar el reproductor.
// Devuelve false y no altera el audio normal si SD/archivo no están.
bool iniciarFondoStars() {
  String ruta = buscarStarsMP3();
  if (ruta.length() == 0) {
    Serial.println("[MP3][FONDO] stars.mp3 no disponible; usar jingles");
    return false;
  }

  detenerActual();

  archivo = SD_MMC.open(ruta, FILE_READ);
  if (!archivo || archivo.isDirectory()) {
    if (archivo) archivo.close();
    Serial.println("[MP3][FONDO] no se pudo abrir stars.mp3");
    return false;
  }

  bytesTotales = (uint32_t)archivo.size();
  bytesLeidos = 0;
  duracionTotalMs = 0;
  bitrateActual = 0;
  bitrateBase = 0;
  tiempoReproMs = 0;
  sampleRateActual = 44100;
  canalesActual = 2;
  detener = false;
  pausado = false;
  terminado = false;
  modoFondoLoop = true;
  rutaFondoLoop = ruta;

  if (!AudioMAX98357::entrarModoGaleria()) {
    archivo.close();
    modoFondoLoop = false;
    rutaFondoLoop = "";
    Serial.println("[MP3][FONDO] no se pudo tomar I2S");
    return false;
  }
  i2sTomado = true;

  i2s_zero_dma_buffer(AudioMAX98357::I2S_PORT);
  vTaskDelay(pdMS_TO_TICKS(2));
  i2s_set_clk(AudioMAX98357::I2S_PORT, 44100,
              I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
  i2s_zero_dma_buffer(AudioMAX98357::I2S_PORT);
  armarFadeInMP3(44100);

  BaseType_t ok = xTaskCreatePinnedToCore(
      tareaMP3, "COCODRILO_STARS", 8192, nullptr, 3, &tarea, 0);

  if (ok != pdPASS) {
    tarea = nullptr;
    archivo.close();
    AudioMAX98357::salirModoGaleria();
    i2sTomado = false;
    modoFondoLoop = false;
    rutaFondoLoop = "";
    Serial.println("[MP3][FONDO] no se pudo crear tarea");
    return false;
  }

  reproduciendo = true;
  Serial.printf("[MP3][FONDO] loop: %s\n", ruta.c_str());
  return true;
}

bool fondoStarsActivo() {
  return modoFondoLoop && (reproduciendo || tarea != nullptr);
}

void detenerFondoStars() {
  if (modoFondoLoop || tarea != nullptr || i2sTomado) detenerActual();
}

static void cambiarCancion(int delta) {
  if (cantidad <= 0) return;
  seleccion = (seleccion + delta + cantidad) % cantidad;
  iniciarSeleccion();
}

#endif // COCODRILO_TIENE_MP3_DRMP3

void entrar() {
  vistaPlayer = false;
  controlesBloqueados = false;
  esperandoSoltarBTrasBloqueo = false;
  inicioHoldBDesbloqueo = 0;
  botonBMusicPrev = digitalRead(PIN_VOLUMEN_PANEL) == LOW;
  menuPausaPrev = digitalRead(PIN_PAUSA) == LOW;
  cargarLista();
  dibujarLista();
}

void salir() {
#if COCODRILO_TIENE_MP3_DRMP3
  cancelarPortadaMP3();
  if (tarea != nullptr || i2sTomado) detenerActual();
#endif
  vistaPlayer = false;
  controlesBloqueados = false;
  esperandoSoltarBTrasBloqueo = false;
  inicioHoldBDesbloqueo = 0;
  gameState = STATE_MENU;
  volumenPanelAbierto = false;
  invalidarMenuBase();
  tft.fillScreen(TFT_BLACK);
  canvas.fillScreen(TFT_BLACK);
  ::drawMenu();
}

void actualizar(bool okPresionado) {
  static Direccion actual = DIR_NINGUNO;
  static Direccion anterior = DIR_NINGUNO;
  static unsigned long ultimoCambio = 0;
  static uint32_t ultimoRefresh = 0;

  const bool menuAhora = digitalRead(PIN_PAUSA) == LOW;
  const bool menuPulsado = menuAhora && !menuPausaPrev;
  menuPausaPrev = menuAhora;

  const bool botonBAhora = digitalRead(PIN_VOLUMEN_PANEL) == LOW;
  const bool botonBPulsado = botonBAhora && !botonBMusicPrev;
  botonBMusicPrev = botonBAhora;

#if COCODRILO_TIENE_MP3_DRMP3
  // Publicar una portada terminada sin bloquear lectura de botones.
  procesarResultadoPortadaMP3();
#endif

  // B solo actua como bloqueo dentro de la vista de reproduccion.
  // Una pulsacion bloquea inmediatamente. Para desbloquear, primero hay que
  // soltar esa pulsacion y luego mantener B durante 2 segundos.
  if (vistaPlayer && reproduciendo) {
    if (!controlesBloqueados && botonBPulsado) {
      controlesBloqueados = true;
      esperandoSoltarBTrasBloqueo = true;
      inicioHoldBDesbloqueo = 0;
      beepMenuMove();
      dibujarControlesPlayer();
      return;
    }

    if (controlesBloqueados) {
      if (esperandoSoltarBTrasBloqueo) {
        if (!botonBAhora) {
          esperandoSoltarBTrasBloqueo = false;
          inicioHoldBDesbloqueo = 0;
        }
      } else if (botonBAhora) {
        if (inicioHoldBDesbloqueo == 0) inicioHoldBDesbloqueo = millis();

        if ((uint32_t)(millis() - inicioHoldBDesbloqueo) >= HOLD_B_DESBLOQUEO_MS) {
          controlesBloqueados = false;
          inicioHoldBDesbloqueo = 0;
          beepMenuMove();
          dibujarControlesPlayer();
          return;
        }
      } else {
        inicioHoldBDesbloqueo = 0;
      }

      // Mientras esta bloqueado ignoramos A, cruceta y MENU, pero el audio,
      // la portada y el progreso siguen actualizandose normalmente.
      if (millis() - ultimoRefresh >= 300) {
        ultimoRefresh = millis();
        dibujarPlayer(false);
      }
      return;
    }
  }

  if (menuPulsado) {
#if COCODRILO_TIENE_MP3_DRMP3
    if (vistaPlayer) {
      // MENU dentro del reproductor vuelve a la lista de canciones.
      cancelarPortadaMP3();
      if (tarea != nullptr || i2sTomado) detenerActual();
      vistaPlayer = false;
      volumenOSDVisible = false;
      beepMenuMove();
      dibujarLista();
      return;
    }
#endif
    // MENU desde la lista sí vuelve al launcher de COCODRILO OS.
    salir();
    return;
  }

#if !COCODRILO_TIENE_MP3_DRMP3
  return;
#else
  // Fin natural: pasar automáticamente a la siguiente canción.
  if (vistaPlayer && terminado && tarea == nullptr) {
    terminado = false;
    if (cantidad > 0) {
      seleccion = (seleccion + 1) % cantidad;
      iniciarSeleccion();
    }
    return;
  }

  Direccion leida = clasificarCruceta(leerCrucetaADC());
  if (leida != actual) {
    actual = leida;
    ultimoCambio = millis();
  }

  bool dirPulsada = false;
  if ((millis() - ultimoCambio) > DEBOUNCE_CRUCETA_MS && actual != anterior) {
    anterior = actual;
    dirPulsada = actual != DIR_NINGUNO;
  }

  if (!vistaPlayer) {
    if (dirPulsada && cantidad > 0 &&
        (actual == DIR_ARRIBA || actual == DIR_ABAJO)) {
      const int seleccionAnterior = seleccion;
      const int primeroAnterior = primeroVisible;

      if (actual == DIR_ARRIBA) seleccion = (seleccion + cantidad - 1) % cantidad;
      else seleccion = (seleccion + 1) % cantidad;

      ajustarScroll();
      beepMenuMove();
      actualizarCursorListaMP3(seleccionAnterior, primeroAnterior);
    }

    if (okPresionado && cantidad > 0) {
      beepMenuMove();
      iniciarSeleccion();
    }
    return;
  }

  if (okPresionado && reproduciendo) {
    pausado = !pausado;
    if (pausado) i2s_zero_dma_buffer(AudioMAX98357::I2S_PORT);
    beepMenuMove();
    dibujarProgresoPlayer();
  }

  if (dirPulsada) {
    if (actual == DIR_IZQUIERDA) {
      beepMenuMove();
      cambiarCancion(-1);
      return;
    }
    if (actual == DIR_DERECHA) {
      beepMenuMove();
      cambiarCancion(+1);
      return;
    }

    if (actual == DIR_ARRIBA || actual == DIR_ABAJO) {
      int nuevo = volumenPaso + (actual == DIR_ARRIBA ? 1 : -1);
      if (nuevo < 0) nuevo = 0;
      if (nuevo > VOLUME_PASOS_MAX) nuevo = VOLUME_PASOS_MAX;
      if (nuevo != volumenPaso) {
        volumenPaso = nuevo;
        sonidoActivo = volumenPaso > 0;
        cocodriloAplicarVolumenMAX();
        beepMenuMove();
      }
      volumenOSDHasta = millis() + 1400;
      dibujarVolumenOSD();
      volumenOSDVisible = true;
    }
  }

  if (vistaPlayer && millis() - ultimoRefresh >= 300) {
    ultimoRefresh = millis();
    dibujarPlayer(false);
  }
#endif
}

} // namespace ReproductorMP3


// ════════════════════════════════════════════════════════════
//  MENÚ DEL ARCADE — cuadrícula visible 3 x 3 con scroll vertical
//  La cruceta completa navega por la cuadrícula. GPIO4 abre el
//  panel de volumen; dentro de ese panel izquierda/derecha ajusta.
//  El panel se actualiza sin reconstruir el menú ni decodificar el JPG.
// ════════════════════════════════════════════════════════════
#define NUM_JUEGOS 11
#define MENU_COLUMNAS 3
#define MENU_FILAS_VISIBLES 3
#define MENU_FILAS_TOTALES ((NUM_JUEGOS + MENU_COLUMNAS - 1) / MENU_COLUMNAS)

const char* nombresJuegos[NUM_JUEGOS] = {
  "Princesita", "Astronauta", "Cuatete",
  "Memorama", "Zona Astro",
  "Emuladores", "Personal.",
  "Galeria", "Musica", "Metronomo", "Afinador"
};

// Emuladores usa por ahora el icono original icon_nes.
// Personalizacion usa icon_alt_config desde iconos_personalizados.h.
const uint16_t* iconosNormal[NUM_JUEGOS] = {
  icon_princesita, icon_astronauta, icon_cuatete,
  icon_memorama, icon_zona,
  icon_nes, icon_alt_config,
  icon_galeria, icon_musica, icon_metronomo, icon_afinador
};
const uint16_t* iconosDeshabilitados[NUM_JUEGOS] = {
  icon_princesita_disabled, icon_astronauta_disabled, icon_cuatete_disabled,
  icon_memorama_disabled, icon_zona_disabled,
  icon_nes_disabled, icon_alt_config,
  icon_galeria_disabled, icon_musica_disabled, icon_metronomo, icon_afinador
};

// 0 = icono original de fábrica. 1..N = iconos alternativos.
uint8_t iconoElegidoPorApp[NUM_JUEGOS] = {0};
int appIconoSeleccionada = 0;
int opcionIconoTemporal = 0;

void cargarIconosPersonalizados();

const uint16_t* obtenerIconoApp(int app, bool disponible = true) {
  if (app < 0 || app >= NUM_JUEGOS) return iconosNormal[0];

  const uint8_t eleccion = iconoElegidoPorApp[app];
  if (eleccion == 0) {
    return disponible ? iconosNormal[app] : iconosDeshabilitados[app];
  }

  const int alternativo = eleccion - 1;
  if (alternativo >= 0 && alternativo < CANT_ICONOS_ALTERNATIVOS) {
    return ICONOS_ALTERNATIVOS[alternativo];
  }

  return disponible ? iconosNormal[app] : iconosDeshabilitados[app];
}

void cargarIconosPersonalizados() {
  preferenciasArcade.begin("arcade", true);
  for (int i = 0; i < NUM_JUEGOS; ++i) {
    char clave[8];
    // Mantener las preferencias de iconos de instalaciones anteriores.
    const int indiceAnterior = (i >= 2) ? i + 1 : i;
    snprintf(clave, sizeof(clave), "icon%d", indiceAnterior);
    iconoElegidoPorApp[i] = preferenciasArcade.getUChar(clave, 0);
    if (iconoElegidoPorApp[i] > CANT_ICONOS_ALTERNATIVOS) {
      iconoElegidoPorApp[i] = 0;
    }
  }
  preferenciasArcade.end();
}

void guardarIconoApp(int app, uint8_t eleccion) {
  if (app < 0 || app >= NUM_JUEGOS) return;
  if (eleccion > CANT_ICONOS_ALTERNATIVOS) eleccion = 0;

  iconoElegidoPorApp[app] = eleccion;
  preferenciasArcade.begin("arcade", false);
  char clave[8];
  const int indiceAnterior = (app >= 2) ? app + 1 : app;
  snprintf(clave, sizeof(clave), "icon%d", indiceAnterior);
  preferenciasArcade.putUChar(clave, eleccion);
  preferenciasArcade.end();

  invalidarMenuBase();
}

// true = ya implementado y jugable desde el menu
bool juegoDisponible[NUM_JUEGOS] = {
  true, true, true,
  true, true, true,
  true, true, true,
  true, true  // Total: 11 aplicaciones
};

int menuSeleccion = 0;
int menuPrimeraFilaVisible = 0;
bool volumenPanelAbierto = false;


// Caché pequeño: guarda únicamente la celda actualmente seleccionada
// en su estado normal (sin cursor). Requiere unos 12 KB, no PSRAM.
uint16_t* celdaBaseBuffer = nullptr;
bool celdaBaseValida = false;
int celdaBaseIndice = -1;

static const int MENU_GRID_Y = 31;
static const int MENU_CELL_W = SCR_W / MENU_COLUMNAS;
static const int MENU_CELL_H = 62;
static const int MENU_CACHE_X_PAD = 6;
static const int MENU_CACHE_Y_PAD = 3;
static const int MENU_CACHE_W = MENU_CELL_W - 12;
static const int MENU_CACHE_H = MENU_CELL_H;

// Caché del wallpaper del launcher.
// 320x240 RGB565 = ~150 KB. En S3 N16R8 intentamos PSRAM primero.
// Si no hay memoria suficiente, el menú vuelve automáticamente al método anterior.
uint16_t* menuWallpaperCache = nullptr;
bool menuWallpaperCacheValido = false;
String menuWallpaperCacheRuta = "";

bool reservarCacheWallpaperMenu() {
  if (canvas.getColorDepth() != 16) return false;
  if (menuWallpaperCache != nullptr) return true;

  const size_t bytes = (size_t)SCR_W * SCR_H * sizeof(uint16_t);

  #if defined(BOARD_HAS_PSRAM)
    menuWallpaperCache = (uint16_t*)ps_malloc(bytes);
  #endif

  if (!menuWallpaperCache) {
    menuWallpaperCache = (uint16_t*)malloc(bytes);
  }

  if (!menuWallpaperCache) {
    Serial.println("Cache wallpaper menu desactivado: RAM insuficiente");
    return false;
  }

  Serial.printf("Cache wallpaper menu reservado: %u bytes\n", (unsigned)bytes);
  return true;
}

void invalidarCacheWallpaperMenu() {
  menuWallpaperCacheValido = false;
  menuWallpaperCacheRuta = "";
}

bool prepararWallpaperMenuCacheado() {
  const bool mismaRuta =
      menuWallpaperCacheValido &&
      menuWallpaperCacheRuta == rutaWallpaperActual;

  if (mismaRuta && menuWallpaperCache) {
    memcpy(
      canvas.getPointer(),
      menuWallpaperCache,
      (size_t)SCR_W * SCR_H * sizeof(uint16_t)
    );
    return true;
  }

  const bool ok = dibujarWallpaperActivo(rutaWallpaperActual.c_str(), canvas);

  if (ok && reservarCacheWallpaperMenu()) {
    memcpy(
      menuWallpaperCache,
      canvas.getPointer(),
      (size_t)SCR_W * SCR_H * sizeof(uint16_t)
    );
    menuWallpaperCacheRuta = rutaWallpaperActual;
    menuWallpaperCacheValido = true;
  }

  return ok;
}

void invalidarMenuBase() {
  celdaBaseValida = false;
  celdaBaseIndice = -1;
}

bool ajustarScrollMenuParaSeleccion(int indice) {
  const int fila = indice / MENU_COLUMNAS;
  int nuevaPrimera = menuPrimeraFilaVisible;

  if (fila < nuevaPrimera) {
    nuevaPrimera = fila;
  } else if (fila >= nuevaPrimera + MENU_FILAS_VISIBLES) {
    nuevaPrimera = fila - MENU_FILAS_VISIBLES + 1;
  }

  const int maxPrimera = max(0, MENU_FILAS_TOTALES - MENU_FILAS_VISIBLES);
  nuevaPrimera = constrain(nuevaPrimera, 0, maxPrimera);

  if (nuevaPrimera != menuPrimeraFilaVisible) {
    menuPrimeraFilaVisible = nuevaPrimera;
    invalidarMenuBase();
    return true;
  }
  return false;
}

void obtenerRegionCelda(int indice, int &x, int &y) {
  const int col = indice % MENU_COLUMNAS;
  const int filaGlobal = indice / MENU_COLUMNAS;
  const int filaVisual = filaGlobal - menuPrimeraFilaVisible;
  const int cy = MENU_GRID_Y + filaVisual * MENU_CELL_H;
  x = col * MENU_CELL_W + MENU_CACHE_X_PAD;
  y = cy - MENU_CACHE_Y_PAD;
}

bool reservarCacheCelda() {
  if (canvas.getColorDepth() != 16) return false;
  if (celdaBaseBuffer != nullptr) return true;
  const size_t bytes = (size_t)MENU_CACHE_W * MENU_CACHE_H * sizeof(uint16_t);
  celdaBaseBuffer = (uint16_t*)malloc(bytes);
  if (!celdaBaseBuffer) Serial.println("Cache de celda desactivado: RAM insuficiente");
  return celdaBaseBuffer != nullptr;
}

bool capturarCeldaBase(int indice) {
  if (!reservarCacheCelda()) return false;
  int x, y;
  obtenerRegionCelda(indice, x, y);
  uint16_t* origen = (uint16_t*)canvas.getPointer();
  for (int fila = 0; fila < MENU_CACHE_H; ++fila) {
    memcpy(celdaBaseBuffer + fila * MENU_CACHE_W,
           origen + (y + fila) * SCR_W + x,
           MENU_CACHE_W * sizeof(uint16_t));
  }
  celdaBaseIndice = indice;
  celdaBaseValida = true;
  return true;
}

void restaurarCeldaBase() {
  if (!celdaBaseValida || !celdaBaseBuffer) return;
  int x, y;
  obtenerRegionCelda(celdaBaseIndice, x, y);
  uint16_t* destino = (uint16_t*)canvas.getPointer();
  for (int fila = 0; fila < MENU_CACHE_H; ++fila) {
    memcpy(destino + (y + fila) * SCR_W + x,
           celdaBaseBuffer + fila * MENU_CACHE_W,
           MENU_CACHE_W * sizeof(uint16_t));
  }
}

// Envía únicamente una región del canvas a la TFT.
// Así mover el cursor no vuelve a empujar el encabezado "SIN CONEXION".
static void pushRegionMenuDesdeCanvas(int x, int y, int w, int h) {
  if (canvas.getColorDepth() != 16 || !canvas.getPointer()) {
    perfilPushSprite(0, 0);
    return;
  }

  x = constrain(x, 0, SCR_W - 1);
  y = constrain(y, 0, SCR_H - 1);
  w = min(w, SCR_W - x);
  h = min(h, SCR_H - y);
  if (w <= 0 || h <= 0) return;

  uint16_t* pix = (uint16_t*)canvas.getPointer();

  tft.startWrite();
  for (int fila = 0; fila < h; ++fila) {
    tft.pushImage(x, y + fila, w, 1,
                  pix + (y + fila) * SCR_W + x);
  }
  tft.endWrite();
}

// Recupera solo la franja del encabezado desde el wallpaper cacheado.
// No toca las celdas ni el cursor.
static bool restaurarEncabezadoDesdeWallpaper() {
  if (!menuWallpaperCacheValido || !menuWallpaperCache ||
      canvas.getColorDepth() != 16 || !canvas.getPointer()) {
    return false;
  }

  constexpr int HEADER_H = 30;
  uint16_t* destino = (uint16_t*)canvas.getPointer();
  for (int y = 0; y < HEADER_H; ++y) {
    memcpy(destino + y * SCR_W,
           menuWallpaperCache + y * SCR_W,
           SCR_W * sizeof(uint16_t));
  }
  return true;
}

void beepMenuMove() {
  buzzerOn(NOTE_A4); delay(20); buzzerOff();
}

void beepDenegado() {
  buzzerOn(NOTE_CS4); delay(90); buzzerOff();
  delay(40);
  buzzerOn(NOTE_CS4); delay(90); buzzerOff();
}

// Los iconos ya están escalados a 48x48. Se envían en una sola operación,
// usando magenta como color transparente. Esto evita corrupción por cientos
// de llamadas fillRect() por icono en el sprite.
void dibujarIconoJuego(int x, int y, const uint16_t* imagen) {
  // Agrupa píxeles consecutivos del mismo color en líneas horizontales.
  // Reduce mucho la cantidad de llamadas respecto a drawPixel().
  for (int iy = 0; iy < ICON_H; ++iy) {
    int ix = 0;
    while (ix < ICON_W) {
      const uint16_t color = pgm_read_word(&imagen[iy * ICON_W + ix]);

      if (color == ICON_TRANSPARENT_KEY) {
        ++ix;
        continue;
      }

      const int inicio = ix;
      ++ix;
      while (ix < ICON_W) {
        const uint16_t siguiente = pgm_read_word(&imagen[iy * ICON_W + ix]);
        if (siguiente != color) break;
        ++ix;
      }

      canvas.drawFastHLine(x + inicio, y + iy, ix - inicio, color);
    }
  }
}

void drawPanelVolumen() {
  const int x = 48, y = 66, w = 224, h = 108;
  canvas.fillRoundRect(x + 3, y + 3, w, h, 10, TFT_BLACK);
  canvas.fillRoundRect(x, y, w, h, 10, 0x18E3);
  canvas.drawRoundRect(x, y, w, h, 10, TFT_WHITE);
  canvas.setTextDatum(MC_DATUM);
  canvas.setTextColor(0xFFE0);
  canvas.drawString("SONIDO", SCR_W / 2, y + 20, 2);

  int barX = x + 24, barY = y + 47, barW = w - 48, barH = 18;
  canvas.drawRect(barX, barY, barW, barH, TFT_WHITE);
  int relleno = (barW - 4) * volumenPaso / VOLUME_PASOS_MAX;
  uint16_t color = volumenPaso == 0 ? TFT_RED : (volumenPaso <= 3 ? 0xFD20 : TFT_GREEN);
  if (relleno > 0) canvas.fillRect(barX + 2, barY + 2, relleno, barH - 4, color);
  canvas.setTextColor(TFT_WHITE);
  canvas.drawString(String(arcadeNombreVolumen()), SCR_W / 2, y + 78, 2);
  canvas.setTextColor(TFT_LIGHTGREY);
  canvas.drawString("Izq/Der ajusta   B u A cierra", SCR_W / 2, y + 96, 1);
  canvas.setTextDatum(TL_DATUM);
}

// Actualiza SOLO la parte dinámica del panel de volumen.
// Esto evita enviar otra vez los 320x240 píxeles a la TFT en cada toque,
// que era lo que producía el pequeño "flash" visual al mover la barra.
void actualizarPanelVolumenParcial() {
  const int panelX = 48, panelY = 66, panelW = 224;
  const int barX = panelX + 24;
  const int barY = panelY + 47;
  const int barW = panelW - 48;
  const int barH = 18;
  const uint16_t fondoPanel = 0x18E3;

  const int relleno =
      (barW - 4) * volumenPaso / VOLUME_PASOS_MAX;
  const uint16_t color =
      volumenPaso == 0 ? TFT_RED :
      (volumenPaso <= 3 ? 0xFD20 : TFT_GREEN);

  // ---------- framebuffer ----------
  // Limpiar solo el interior de la barra.
  canvas.fillRect(barX + 2, barY + 2,
                  barW - 4, barH - 4, fondoPanel);
  if (relleno > 0) {
    canvas.fillRect(barX + 2, barY + 2,
                    relleno, barH - 4, color);
  }

  // Limpiar únicamente la zona del texto numérico.
  canvas.fillRect(panelX + 50, panelY + 67,
                  panelW - 100, 24, fondoPanel);
  canvas.setTextDatum(MC_DATUM);
  canvas.setTextColor(TFT_WHITE, fondoPanel);
  canvas.drawString(String(arcadeNombreVolumen()),
                    SCR_W / 2, panelY + 78, 2);
  canvas.setTextDatum(TL_DATUM);

  // ---------- TFT ----------
  // Repetimos esas MISMAS operaciones directamente sobre la pantalla.
  // No hay fillScreen(), drawMenu() ni pushSprite() completo.
  tft.fillRect(barX + 2, barY + 2,
               barW - 4, barH - 4, fondoPanel);
  if (relleno > 0) {
    tft.fillRect(barX + 2, barY + 2,
                 relleno, barH - 4, color);
  }

  tft.fillRect(panelX + 50, panelY + 67,
               panelW - 100, 24, fondoPanel);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, fondoPanel);
  tft.drawString(String(arcadeNombreVolumen()),
                 SCR_W / 2, panelY + 78, 2);
  tft.setTextDatum(TL_DATUM);
}


void dibujarMenuLogros() {
  dibujarWallpaperActivo(rutaWallpaperActual.c_str(), canvas);
  canvas.fillRoundRect(16, 12, SCR_W - 32, SCR_H - 24, 12, 0x1082);
  canvas.drawRoundRect(16, 12, SCR_W - 32, SCR_H - 24, 12, TFT_WHITE);
  const uint8_t total = sistemaLogros.total(juegoLogrosSeleccionado);
  if (total == 0) { gameState = STATE_MENU; drawMenu(); return; }
  if (logroSeleccionado >= total) logroSeleccionado = total - 1;
  canvas.setTextDatum(MC_DATUM); canvas.setTextColor(TFT_YELLOW);
  canvas.drawString(String("LOGROS - ") + sistemaLogros.nombreJuego(juegoLogrosSeleccionado), SCR_W/2, 28, 2);
  canvas.setTextColor(TFT_WHITE);
  canvas.drawString(String(sistemaLogros.contar(juegoLogrosSeleccionado)) + " / " + String(total), SCR_W/2, 47, 1);
  const int visibles = 5;
  if (logroSeleccionado < logroPrimerVisible) logroPrimerVisible = logroSeleccionado;
  if (logroSeleccionado >= logroPrimerVisible + visibles) logroPrimerVisible = logroSeleccionado - visibles + 1;
  for (int fila=0; fila<visibles; ++fila) {
    int i=logroPrimerVisible+fila; if (i>=total) break;
    int y=70+fila*29; bool sel=i==logroSeleccionado;
    bool ok=sistemaLogros.obtenido(juegoLogrosSeleccionado,(uint8_t)i);
    if(sel){ canvas.fillRoundRect(29,y-11,SCR_W-58,25,6,TFT_BLACK); canvas.drawRoundRect(29,y-11,SCR_W-58,25,6,colorCursor); }
    canvas.setTextDatum(ML_DATUM); canvas.setTextColor(ok?TFT_GREEN:(sel?colorCursor:TFT_LIGHTGREY));
    canvas.drawString(String(ok?"[OK] ":"[--] ")+sistemaLogros.definicion(juegoLogrosSeleccionado,i).nombre,39,y,1);
  }
  const DefinicionLogro &l=sistemaLogros.definicion(juegoLogrosSeleccionado,logroSeleccionado);
  canvas.setTextDatum(MC_DATUM); canvas.setTextColor(TFT_WHITE); canvas.drawString(l.descripcion,SCR_W/2,219,1);
  canvas.setTextColor(TFT_LIGHTGREY); canvas.drawString("Arriba/Abajo: revisar   MENU: volver",SCR_W/2,232,1);
  canvas.setTextDatum(TL_DATUM); perfilPushSprite(0,0);
}

void actualizarMenuLogros(bool btnPressed) {
  static Direccion anterior=DIR_NINGUNO; static bool menuAnterior=false;
  const uint8_t total=sistemaLogros.total(juegoLogrosSeleccionado);
  const Direccion dir=clasificarCruceta(leerCrucetaADC());
  if(dir!=DIR_NINGUNO && dir!=anterior && total){
    if(dir==DIR_ARRIBA){ logroSeleccionado=(logroSeleccionado+total-1)%total; beepMenuMove(); dibujarMenuLogros(); }
    else if(dir==DIR_ABAJO){ logroSeleccionado=(logroSeleccionado+1)%total; beepMenuMove(); dibujarMenuLogros(); }
  }
  anterior=dir;
  bool menuAhora=digitalRead(PIN_PAUSA)==LOW; bool menuPulsado=menuAhora&&!menuAnterior; menuAnterior=menuAhora;
  if(menuPulsado||btnPressed){ gameState=STATE_MENU; invalidarMenuBase(); drawMenu(); }
}

void dibujarCeldaMenu(int i, bool seleccionada) {
  const int gridY = 31;
  const int cellW = SCR_W / MENU_COLUMNAS;
  const int cellH = 62;

  const int col = i % MENU_COLUMNAS;
  const int filaGlobal = i / MENU_COLUMNAS;
  const int fila = filaGlobal - menuPrimeraFilaVisible;
  if (fila < 0 || fila >= MENU_FILAS_VISIBLES) return;

  const int cx = col * cellW + cellW / 2;
  const int cy = gridY + fila * cellH;
  const bool disponible = juegoDisponible[i];

  if (seleccionada) {
    canvas.fillRoundRect(col * cellW + 7, cy - 2, cellW - 14, cellH - 3, 8, 0x2104);
    canvas.drawRoundRect(col * cellW + 7, cy - 2, cellW - 14, cellH - 3, 8, colorCursor);
  }

  dibujarIconoJuego(cx - 24, cy + 1, obtenerIconoApp(i, disponible));

  canvas.setTextDatum(TC_DATUM);
  canvas.setTextColor(TFT_BLACK);
  canvas.drawString(nombresJuegos[i], cx + 1, cy + 50, 1);
  canvas.setTextColor(disponible ? (seleccionada ? colorCursor : TFT_WHITE) : 0x9CF3);
  canvas.drawString(nombresJuegos[i], cx, cy + 49, 1);
}

void dibujarEncabezadoFechaHora() {
  // Texto directamente sobre el wallpaper, sin barra negra.
  // Se usa una sombra de 1 pixel para conservar la legibilidad.
  canvas.setTextFont(1);
  canvas.setTextSize(1);

  const bool conectado = WiFi.status() == WL_CONNECTED;

  if (!conectado) {
    canvas.setTextDatum(MC_DATUM);
    canvas.setTextColor(TFT_BLACK);
    canvas.drawString("SIN CONEXION", SCR_W / 2 + 1, 16, 2);
    canvas.setTextColor(TFT_ORANGE);
    canvas.drawString("SIN CONEXION", SCR_W / 2, 15, 2);
    canvas.setTextDatum(TL_DATUM);
    return;
  }

  // Wi-Fi ya está conectado, aunque NTP todavía pueda tardar unos segundos.
  if (!horaSincronizada || !horaSistemaValida()) {
    canvas.setTextDatum(MC_DATUM);
    canvas.setTextColor(TFT_BLACK);
    canvas.drawString("CONECTADO", SCR_W / 2 + 1, 16, 2);
    canvas.setTextColor(TFT_GREEN);
    canvas.drawString("CONECTADO", SCR_W / 2, 15, 2);
    canvas.setTextDatum(TL_DATUM);
    return;
  }

  struct tm tiempoLocal;
  if (!getLocalTime(&tiempoLocal, 20)) {
    canvas.setTextDatum(MC_DATUM);
    canvas.setTextColor(TFT_BLACK);
    canvas.drawString("CONECTADO", SCR_W / 2 + 1, 16, 2);
    canvas.setTextColor(TFT_GREEN);
    canvas.drawString("CONECTADO", SCR_W / 2, 15, 2);
    canvas.setTextDatum(TL_DATUM);
    return;
  }

  static const char* meses[] = {
    "ENE", "FEB", "MAR", "ABR", "MAY", "JUN",
    "JUL", "AGO", "SEP", "OCT", "NOV", "DIC"
  };

  char fecha[20];
  char hora[8];

  snprintf(
    fecha,
    sizeof(fecha),
    "%02d %s %04d",
    tiempoLocal.tm_mday,
    meses[tiempoLocal.tm_mon],
    tiempoLocal.tm_year + 1900
  );

  snprintf(
    hora,
    sizeof(hora),
    "%02d:%02d",
    tiempoLocal.tm_hour,
    tiempoLocal.tm_min
  );

  canvas.setTextDatum(ML_DATUM);

  canvas.setTextColor(TFT_BLACK);
  canvas.drawString(fecha, 13, 16, 2);
  canvas.drawString(hora, 133, 16, 2);

  canvas.setTextColor(0xFFE0);
  canvas.drawString(fecha, 12, 15, 2);
  canvas.drawString(hora, 132, 15, 2);

  canvas.setTextDatum(TL_DATUM);
}

void actualizarEncabezadoMenuSiNecesario() {
  if (gameState != STATE_MENU) return;

  const wl_status_t estadoActual = WiFi.status();
  int minutoActual = -1;

  if (estadoActual == WL_CONNECTED &&
      horaSincronizada &&
      horaSistemaValida()) {
    struct tm tiempoLocal;
    if (getLocalTime(&tiempoLocal, 10)) {
      minutoActual = tiempoLocal.tm_min;
    }
  }

  if (estadoActual == estadoWiFiEncabezadoAnterior &&
      minutoActual == minutoEncabezadoAnterior) {
    return;
  }

  estadoWiFiEncabezadoAnterior = estadoActual;
  minutoEncabezadoAnterior = minutoActual;

  // Actualizar únicamente la franja superior. El cursor y las celdas
  // permanecen intactos y no se vuelven a enviar a la TFT.
  if (!restaurarEncabezadoDesdeWallpaper()) {
    // Fallback seguro si todavía no existe caché del wallpaper.
    drawMenu();
    return;
  }

  dibujarEncabezadoFechaHora();

  // La restauracion del encabezado recupera el wallpaper y por tanto
  // tambien borra el medidor que bateriaPushSprite habia dibujado encima.
  // Volvemos a dibujarlo dentro del propio canvas antes de enviar la franja.
  // Asi mover el cursor / refrescar fecha-hora nunca hace desaparecer bateria.
  canvas.setTextFont(1);
  canvas.setTextSize(1);
  canvas.setTextDatum(TL_DATUM);
  Bateria::Dibujar(canvas, SCR_W - 32, 4);

  canvas.setTextDatum(TL_DATUM);
  pushRegionMenuDesdeCanvas(0, 0, SCR_W, 30);
}

void construirMenuCompleto() {
  canvas.setTextSize(1);
  canvas.setTextFont(1);
  canvas.setTextDatum(TL_DATUM);

  // Scroll rápido: restaurar el wallpaper desde RAM en vez de
  // decodificar el JPG de nuevo al cambiar de fila.
  prepararWallpaperMenuCacheado();

  dibujarEncabezadoFechaHora();

  for (int i = 0; i < NUM_JUEGOS; ++i) {
    dibujarCeldaMenu(i, false);
  }

  // Indicadores mínimos de que hay más filas sin tapar el wallpaper.
  canvas.setTextDatum(MC_DATUM);
  canvas.setTextColor(TFT_LIGHTGREY);
  if (menuPrimeraFilaVisible > 0) {
    canvas.drawString("^", SCR_W - 8, 38, 1);
  }
  if (menuPrimeraFilaVisible + MENU_FILAS_VISIBLES < MENU_FILAS_TOTALES) {
    canvas.drawString("v", SCR_W - 8, 205, 1);
  }

  canvas.setTextDatum(BC_DATUM);
  canvas.setTextColor(TFT_BLACK);
  canvas.drawString("Cruceta: elegir  MENU: logros  A: jugar", SCR_W / 2 + 1, SCR_H - 3, 1);
  canvas.setTextColor(TFT_LIGHTGREY);
  canvas.drawString("Cruceta: elegir  MENU: logros  A: jugar", SCR_W / 2, SCR_H - 4, 1);
  canvas.setTextDatum(TL_DATUM);
}

void drawMenu() {
  // Solo se usa al entrar/regresar al menú o al abrir/cerrar volumen.
  ajustarScrollMenuParaSeleccion(menuSeleccion);
  minutoEncabezadoAnterior = -1;
  estadoWiFiEncabezadoAnterior = WL_NO_SHIELD;
  construirMenuCompleto();
  capturarCeldaBase(menuSeleccion);
  dibujarCeldaMenu(menuSeleccion, true);
  if (volumenPanelAbierto) drawPanelVolumen();
  canvas.setTextDatum(TL_DATUM);
  perfilPushSprite(0, 0);
}

void moverCursorMenu(int nuevaSeleccion) {
  if (nuevaSeleccion == menuSeleccion) return;

  const int seleccionAnterior = menuSeleccion;
  menuSeleccion = nuevaSeleccion;

  // Si al movernos cruzamos el borde visible, desplazamos una fila completa.
  // Se reconstruyen iconos/textos, pero el wallpaper se restaura desde RAM:
  // ya no se vuelve a decodificar el JPG en cada scroll.
  if (ajustarScrollMenuParaSeleccion(menuSeleccion)) {
    drawMenu();
    return;
  }

  // Si no hay caché pequeño disponible, conserva el método seguro.
  if (!celdaBaseValida || celdaBaseIndice != seleccionAnterior) {
    drawMenu();
    return;
  }

  // 1) Quitar el cursor anterior restaurando solo esa celda.
  int xAnterior, yAnterior;
  obtenerRegionCelda(seleccionAnterior, xAnterior, yAnterior);
  restaurarCeldaBase();
  pushRegionMenuDesdeCanvas(xAnterior, yAnterior, MENU_CACHE_W, MENU_CACHE_H);

  // 2) Guardar la celda nueva antes de dibujarle el cursor.
  if (!capturarCeldaBase(menuSeleccion)) {
    drawMenu();
    return;
  }

  // 3) Dibujar y enviar únicamente la celda nueva.
  dibujarCeldaMenu(menuSeleccion, true);
  int xNueva, yNueva;
  obtenerRegionCelda(menuSeleccion, xNueva, yNueva);
  canvas.setTextDatum(TL_DATUM);
  pushRegionMenuDesdeCanvas(xNueva, yNueva, MENU_CACHE_W, MENU_CACHE_H);
}

// Abre el panel sin reconstruir el menú ni volver a leer el JPG.
// Al cerrarlo sí se reconstruye una vez para recuperar la zona tapada.
void manejarBotonVolumenMenu() {
  static bool anterior = false;
  bool ahora = (digitalRead(PIN_VOLUMEN_PANEL) == LOW);

  if (ahora && !anterior) {
    beepMenuMove();

    if (!volumenPanelAbierto) {
      volumenPanelAbierto = true;
      drawPanelVolumen();
      perfilPushSprite(0, 0);
    } else {
      volumenPanelAbierto = false;
      invalidarMenuBase();
      drawMenu();
    }
  }

  anterior = ahora;
}

void manejarCrucetaMenu() {
  static Direccion actual = DIR_NINGUNO;
  static Direccion anterior = DIR_NINGUNO;
  static unsigned long ultimoCambio = 0;

  Direccion leida = clasificarCruceta(leerCrucetaADC());
  if (leida != actual) { ultimoCambio = millis(); actual = leida; }

  if ((millis() - ultimoCambio) > DEBOUNCE_CRUCETA_MS && actual != anterior) {
    anterior = actual;
    if (actual == DIR_NINGUNO) return;

    if (volumenPanelAbierto) {
      if (actual == DIR_IZQUIERDA && volumenPaso > 0) volumenPaso--;
      else if (actual == DIR_DERECHA && volumenPaso < VOLUME_PASOS_MAX) volumenPaso++;
      else return;

      aplicarVolumenChopper();
      beepMenuMove();

      // Redibujado realmente parcial: solamente barra + número.
      // Ya no enviamos el framebuffer completo al TFT en cada paso.
      actualizarPanelVolumenParcial();
      return;
    }

    int fila = menuSeleccion / MENU_COLUMNAS;
    int col = menuSeleccion % MENU_COLUMNAS;

    if (actual == DIR_IZQUIERDA || actual == DIR_DERECHA) {
      const int inicioFila = fila * MENU_COLUMNAS;
      const int restantes = NUM_JUEGOS - inicioFila;
      const int columnasValidas = min(MENU_COLUMNAS, restantes);
      if (actual == DIR_IZQUIERDA) col = (col + columnasValidas - 1) % columnasValidas;
      else col = (col + 1) % columnasValidas;
    } else {
      const int paso = (actual == DIR_ARRIBA) ? -1 : 1;
      int filaCandidata = fila;

      for (int intento = 0; intento < MENU_FILAS_TOTALES; ++intento) {
        filaCandidata = (filaCandidata + paso + MENU_FILAS_TOTALES) % MENU_FILAS_TOTALES;

        const int inicioFila = filaCandidata * MENU_COLUMNAS;
        if (inicioFila >= NUM_JUEGOS) continue;

        const int restantes = NUM_JUEGOS - inicioFila;
        const int columnasValidas = min(MENU_COLUMNAS, restantes);

        // Si la última fila tiene menos columnas, cae en la última disponible.
        const int colCandidata = min(col, columnasValidas - 1);
        const int candidato = inicioFila + colCandidata;

        if (candidato < NUM_JUEGOS) {
          fila = filaCandidata;
          col = colCandidata;
          break;
        }
      }
    }

    const int nuevaSeleccion = fila * MENU_COLUMNAS + col;
    beepMenuMove();
    moverCursorMenu(nuevaSeleccion);
  }
}



#include "COCODRILO_NES_RetroGo.h"

// Usado por Modo NES para restaurar Wi-Fi al salir del emulador.
bool conectarWiFiGuardado(uint32_t esperaMs);

// ════════════════════════════════════════════════════════════
//  NINTENDO NES — navegador inicial de ROMs desde /NES
//  Por ahora prueba la entrada, SD y selección. El núcleo del emulador
//  se conectará después a iniciarRomNES().
// ════════════════════════════════════════════════════════════
namespace EmuladoresMenu { void dibujar(); }

// Ficha intermedia de juego.
// La portada existe solo mientras se dibuja esta pantalla: el JPEG se libera
// inmediatamente y nunca acompaña al core del emulador.
namespace FichaJuego {

static constexpr int COVER_X = 114;
static constexpr int COVER_Y = 20;
static constexpr int COVER_W = 92;
static constexpr int COVER_H = 116;
static constexpr uint32_t COVER_BYTES =
    (uint32_t)COVER_W * (uint32_t)COVER_H * 2U;

static String rutaPortada(const char *sistema, const String &nombre) {
  return String("/covers/") + sistema + "/" + nombre + ".rgb565";
}

static void portadaGenerica(const char *sistema) {
  tft.fillRoundRect(COVER_X, COVER_Y, COVER_W, COVER_H, 8, 0x1082);
  tft.drawRoundRect(COVER_X, COVER_Y, COVER_W, COVER_H, 8, TFT_DARKGREY);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(strcmp(sistema, "nes") == 0 ? TFT_RED : TFT_CYAN, 0x1082);
  tft.drawString(strcmp(sistema, "nes") == 0 ? "NES" : "GB", 160, 72, 2);
  tft.setTextColor(TFT_LIGHTGREY, 0x1082);
  tft.drawString("GAME", 160, 94, 1);
  tft.setTextDatum(TL_DATUM);
}

static bool dibujarPortada(const char *sistema, const String &nombre) {
  const String ruta = rutaPortada(sistema, nombre);

  File f = SD_MMC.open(ruta, FILE_READ);
  if (!f || f.isDirectory() || (uint32_t)f.size() != COVER_BYTES) {
    if (f) f.close();
    portadaGenerica(sistema);
    return false;
  }

  // Asset listo para ST7789: 92x116 RGB565, sin JPEGDEC en esta pantalla, sin escalado y
  // sin buffers grandes. Solo 8 filas (1472 bytes) viven en RAM por bloque.
  static uint16_t bloque[COVER_W * 8];

  const bool swapAnterior = tft.getSwapBytes();
  // Los .rgb565 se guardan como uint16_t nativo; TFT_eSPI debe intercambiar
  // los bytes al enviarlos por SPI para que la ST7789 reciba RGB565 correcto.
  tft.setSwapBytes(true);
  tft.fillRect(COVER_X, COVER_Y, COVER_W, COVER_H, TFT_BLACK);

  int y = 0;
  bool ok = true;
  while (y < COVER_H) {
    const int filas = min(8, COVER_H - y);
    const size_t bytes = (size_t)COVER_W * filas * sizeof(uint16_t);
    if (f.read((uint8_t*)bloque, bytes) != (int)bytes) {
      ok = false;
      break;
    }
    tft.pushImage(COVER_X, COVER_Y + y, COVER_W, filas, bloque);
    y += filas;
  }

  f.close();
  tft.setSwapBytes(swapAnterior);

  if (!ok) {
    portadaGenerica(sistema);
    return false;
  }

  tft.drawRoundRect(COVER_X, COVER_Y, COVER_W, COVER_H, 8, TFT_DARKGREY);
  return true;
}

static void botones(int seleccionado) {
  const int y = 199;
  const int h = 30;
  const int w = 105;
  const int xJugar = 38;
  const int xAtras = 177;

  tft.fillRoundRect(xJugar, y, w, h, 7,
                    seleccionado == 0 ? 0x2104 : TFT_BLACK);
  tft.drawRoundRect(xJugar, y, w, h, 7,
                    seleccionado == 0 ? colorCursor : TFT_DARKGREY);
  tft.fillRoundRect(xAtras, y, w, h, 7,
                    seleccionado == 1 ? 0x2104 : TFT_BLACK);
  tft.drawRoundRect(xAtras, y, w, h, 7,
                    seleccionado == 1 ? colorCursor : TFT_DARKGREY);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(seleccionado == 0 ? colorCursor : TFT_WHITE,
                   seleccionado == 0 ? 0x2104 : TFT_BLACK);
  tft.drawString("JUGAR", xJugar + w / 2, y + h / 2, 2);
  tft.setTextColor(seleccionado == 1 ? colorCursor : TFT_WHITE,
                   seleccionado == 1 ? 0x2104 : TFT_BLACK);
  tft.drawString("ATRAS", xAtras + w / 2, y + h / 2, 2);
  tft.setTextDatum(TL_DATUM);
}

static void dibujar(const char *sistema,
                    const String &nombre,
                    const char *consola,
                    int seleccionado) {
  tft.fillScreen(TFT_BLACK);

  dibujarPortada(sistema, nombre);

  String titulo = nombre;
  if (titulo.length() > 30) titulo = titulo.substring(0, 27) + "...";

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(titulo, SCR_W / 2, 151, 2);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString(consola, SCR_W / 2, 173, 1);

  tft.drawFastHLine(20, 188, SCR_W - 40, TFT_DARKGREY);
  tft.setTextDatum(TL_DATUM);

  botones(seleccionado);
}

} // namespace FichaJuego

namespace NESLauncher {

bool mostrarAdvertenciaModoNES() {
  // Evita que el mismo A usado para entrar a NES acepte el aviso.
  while (digitalRead(BUTTON_PIN) == LOW || digitalRead(PIN_PAUSA) == LOW) {
    delay(5);
  }
  delay(60);

  auto esperarAOMenu = []() -> int {
    bool aAnterior = false;
    bool menuAnterior = false;

    while (true) {
      const bool aAhora = digitalRead(BUTTON_PIN) == LOW;
      const bool menuAhora = digitalRead(PIN_PAUSA) == LOW;

      if (aAhora && !aAnterior) {
        while (digitalRead(BUTTON_PIN) == LOW) delay(5);
        delay(40);
        return 1; // A
      }

      if (menuAhora && !menuAnterior) {
        while (digitalRead(PIN_PAUSA) == LOW) delay(5);
        delay(40);
        return 0; // MENU
      }

      aAnterior = aAhora;
      menuAnterior = menuAhora;
      delay(5);
    }
  };

  // ─────────────────────────────────────────────────────────────
  // PAGINA 1 — aviso de servicios temporalmente pausados
  // ─────────────────────────────────────────────────────────────
  dibujarWallpaperActivo(rutaWallpaperActual.c_str(), canvas);
  canvas.fillRoundRect(18, 12, SCR_W - 36, SCR_H - 24, 12, TFT_BLACK);
  canvas.drawRoundRect(18, 12, SCR_W - 36, SCR_H - 24, 12, TFT_YELLOW);

  canvas.setTextDatum(MC_DATUM);
  canvas.setTextColor(TFT_YELLOW, TFT_BLACK);
  canvas.drawString("MODO NES", SCR_W / 2, 29, 2);

  canvas.setTextColor(TFT_WHITE, TFT_BLACK);
  canvas.drawString("Para priorizar el rendimiento,", SCR_W / 2, 58, 1);
  canvas.drawString("durante los juegos se pausan:", SCR_W / 2, 73, 1);

  canvas.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  canvas.drawString("Wi-Fi y servicios en linea", SCR_W / 2, 100, 1);
  canvas.drawString("Bateria y temperatura", SCR_W / 2, 116, 1);
  canvas.drawString("Actualizaciones de COCODRILO OS", SCR_W / 2, 132, 1);

  canvas.setTextColor(TFT_YELLOW, TFT_BLACK);
  canvas.drawString("IMPORTANTE: AL SALIR DEL NES", SCR_W / 2, 153, 1);
  canvas.setTextColor(TFT_WHITE, TFT_BLACK);
  canvas.drawString("COCODRILO OS SE REINICIARA", SCR_W / 2, 168, 1);

  canvas.setTextColor(TFT_WHITE, TFT_BLACK);
  canvas.drawString("A: VER CONTROLES", SCR_W / 2, 194, 2);
  canvas.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  canvas.drawString("MENU: VOLVER", SCR_W / 2, 218, 1);

  canvas.setTextDatum(TL_DATUM);
  perfilPushSprite(0, 0);

  if (esperarAOMenu() == 0) return false;

  // ─────────────────────────────────────────────────────────────
  // PAGINA 2 — controles NES de COCODRILO OS
  // ─────────────────────────────────────────────────────────────
  dibujarWallpaperActivo(rutaWallpaperActual.c_str(), canvas);
  canvas.fillRoundRect(18, 9, SCR_W - 36, SCR_H - 18, 12, TFT_BLACK);
  canvas.drawRoundRect(18, 9, SCR_W - 36, SCR_H - 18, 12, TFT_GREEN);

  canvas.setTextDatum(MC_DATUM);
  canvas.setTextColor(TFT_GREEN, TFT_BLACK);
  canvas.drawString("CONTROLES NES", SCR_W / 2, 26, 2);

  canvas.setTextColor(TFT_WHITE, TFT_BLACK);
  canvas.drawString("CRUCETA  =  MOVER", SCR_W / 2, 56, 1);
  canvas.drawString("A        =  BOTON A", SCR_W / 2, 76, 1);
  canvas.drawString("B        =  BOTON B", SCR_W / 2, 96, 1);
  canvas.drawString("MENU     =  START", SCR_W / 2, 116, 1);

  canvas.setTextColor(TFT_CYAN, TFT_BLACK);
  canvas.drawString("MENU + A  =  SELECT", SCR_W / 2, 142, 1);

  canvas.setTextColor(TFT_YELLOW, TFT_BLACK);
  canvas.drawString("MENU + B  =  SALIR", SCR_W / 2, 160, 1);
  canvas.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  canvas.drawString("mantener ~0.8 s y soltar", SCR_W / 2, 174, 1);
  canvas.setTextColor(TFT_ORANGE, TFT_BLACK);
  canvas.drawString("AL SALIR: LA CONSOLA SE REINICIA", SCR_W / 2, 190, 1);

  canvas.setTextColor(TFT_WHITE, TFT_BLACK);
  canvas.drawString("A: CONTINUAR", SCR_W / 2, 210, 2);
  canvas.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  canvas.drawString("MENU: VOLVER", SCR_W / 2, 229, 1);

  canvas.setTextDatum(TL_DATUM);
  perfilPushSprite(0, 0);

  return esperarAOMenu() == 1;
}

struct EstadoServiciosNES {
  bool wifiPreferido = false;
  bool wifiEstabaConectado = false;
};

EstadoServiciosNES pausarServiciosParaJuego() {
  EstadoServiciosNES e;
  e.wifiPreferido = wifiActivado;
  e.wifiEstabaConectado = (WiFi.status() == WL_CONNECTED);

  // El loop principal queda ocupado por el emulador NES, por lo que bateria,
  // temperatura y servicios periodicos dejan de actualizarse. Ademas,
  // apagamos explicitamente la radio Wi-Fi para liberar recursos.
  if (wifiActivado || e.wifiEstabaConectado) {
    WiFi.disconnect(false, false);   // conservar credenciales
    WiFi.mode(WIFI_OFF);
    delay(25);
  }

  // Retro-Go/Nofrendo toma el control del loop principal. Detenemos cualquier
  // tono activo de COCODRILO antes de entrar al core NES.
  AudioMAX98357::stop();

  Serial.println("[NES] Servicios COCODRILO pausados (core Retro-Go/Nofrendo)");
  return e;
}

void restaurarServiciosDespuesDelJuego(const EstadoServiciosNES &e) {
  // Solo reconectar si el usuario tenia Wi-Fi habilitado antes del juego.
  if (e.wifiPreferido && wifiConfigurado) {
    Serial.println("[NES] Restaurando Wi-Fi...");
    conectarWiFiGuardado(3000UL);
  }

  // Recuperar el estado global de sonido de COCODRILO OS.
  cocodriloAplicarVolumenMAX();
  AudioMAX98357::stop();

  Serial.println("[NES] Servicios COCODRILO restaurados (audio MAX restaurado)");
}


static const int MAX_ROMS = 40;
String nombres[MAX_ROMS];
String rutas[MAX_ROMS];
int cantidad = 0;
int seleccion = 0;
int primeraVisible = 0;
static const int VISIBLES = 7;
static bool vistaFicha = false;
static int opcionFicha = 0;

bool terminaEnNES(const String &nombre) {
  String n = nombre;
  n.toLowerCase();
  return n.endsWith(".nes");
}

String nombreBonito(const String &ruta) {
  int slash = ruta.lastIndexOf('/');
  String n = (slash >= 0) ? ruta.substring(slash + 1) : ruta;
  if (n.length() > 4 && terminaEnNES(n)) n.remove(n.length() - 4);
  return n;
}

void escanear() {
  cantidad = 0;
  seleccion = 0;
  primeraVisible = 0;

  File dir = SD_MMC.open("/NES");
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }

  File f = dir.openNextFile();
  while (f && cantidad < MAX_ROMS) {
    if (!f.isDirectory()) {
      String ruta = String(f.name());
      if (!ruta.startsWith("/")) ruta = String("/NES/") + ruta;
      if (terminaEnNES(ruta)) {
        rutas[cantidad] = ruta;
        nombres[cantidad] = nombreBonito(ruta);
        cantidad++;
      }
    }
    f.close();
    f = dir.openNextFile();
  }
  dir.close();

  // Orden alfabético simple para que la lista sea estable.
  for (int i = 0; i < cantidad - 1; ++i) {
    for (int j = i + 1; j < cantidad; ++j) {
      if (nombres[j].compareTo(nombres[i]) < 0) {
        String tn = nombres[i]; nombres[i] = nombres[j]; nombres[j] = tn;
        String tr = rutas[i];   rutas[i] = rutas[j];     rutas[j] = tr;
      }
    }
  }
}

static void ajustarScrollNES() {
  if (seleccion < primeraVisible) primeraVisible = seleccion;
  if (seleccion >= primeraVisible + VISIBLES)
    primeraVisible = seleccion - VISIBLES + 1;
  if (primeraVisible < 0) primeraVisible = 0;
}

static void dibujarFilaNESDirecta(int indice) {
  if (cantidad <= 0) return;
  if (indice < primeraVisible || indice >= primeraVisible + VISIBLES) return;
  if (indice < 0 || indice >= cantidad) return;

  const int fila = indice - primeraVisible;
  const int y = 55 + fila * 22;
  const bool sel = indice == seleccion;

  tft.fillRect(22, y - 10, SCR_W - 44, 21, 0x1082);

  if (sel) {
    tft.fillRoundRect(27, y - 9, SCR_W - 54, 19, 5, TFT_BLACK);
    tft.drawRoundRect(27, y - 9, SCR_W - 54, 19, 5, colorCursor);
  }

  String etiqueta = nombres[indice];
  if (etiqueta.length() > 32) etiqueta = etiqueta.substring(0, 29) + "...";

  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(sel ? colorCursor : TFT_WHITE, sel ? TFT_BLACK : 0x1082);
  tft.drawString(etiqueta, 36, y, 1);
  tft.setTextDatum(TL_DATUM);
}

static void dibujarPieNESDirecto() {
  tft.fillRect(24, 202, SCR_W - 48, 24, 0x1082);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, 0x1082);
  tft.drawString(
      String(seleccion + 1) + "/" + String(cantidad) +
      "   A: abrir   MENU: volver",
      SCR_W / 2, 214, 1
  );
  tft.setTextDatum(TL_DATUM);
}

static void actualizarCursorNES(int seleccionAnterior, int primeraAnterior) {
  ajustarScrollNES();

  if (primeraAnterior != primeraVisible) {
    tft.fillRect(22, 44, SCR_W - 44, VISIBLES * 22 + 3, 0x1082);
    const int fin = min(cantidad, primeraVisible + VISIBLES);
    for (int i = primeraVisible; i < fin; ++i) dibujarFilaNESDirecta(i);
  } else {
    dibujarFilaNESDirecta(seleccionAnterior);
    dibujarFilaNESDirecta(seleccion);
  }

  dibujarPieNESDirecto();
}

void dibujar() {
  dibujarWallpaperActivo(rutaWallpaperActual.c_str(), canvas);
  canvas.fillRoundRect(15, 10, SCR_W - 30, SCR_H - 20, 12, 0x1082);
  canvas.drawRoundRect(15, 10, SCR_W - 30, SCR_H - 20, 12, TFT_WHITE);

  canvas.setTextDatum(MC_DATUM);
  canvas.setTextColor(TFT_RED);
  canvas.drawString("NINTENDO NES", SCR_W / 2, 27, 2);

  if (SD_MMC.cardType() == CARD_NONE) {
    canvas.setTextColor(TFT_WHITE);
    canvas.drawString("Tarjeta SD no disponible", SCR_W / 2, 103, 2);
    canvas.setTextColor(TFT_LIGHTGREY);
    canvas.drawString("MENU: volver", SCR_W / 2, 210, 1);
    canvas.setTextDatum(TL_DATUM);
    perfilPushSprite(0, 0);
    return;
  }

  if (cantidad == 0) {
    canvas.setTextColor(TFT_WHITE);
    canvas.drawString("No hay ROMs en /NES/", SCR_W / 2, 95, 2);
    canvas.setTextColor(TFT_LIGHTGREY);
    canvas.drawString("Copia archivos .nes a /NES/", SCR_W / 2, 120, 1);
    canvas.drawString("MENU: volver", SCR_W / 2, 210, 1);
    canvas.setTextDatum(TL_DATUM);
    perfilPushSprite(0, 0);
    return;
  }

  ajustarScrollNES();

  canvas.setTextDatum(ML_DATUM);
  for (int fila = 0; fila < VISIBLES; ++fila) {
    const int i = primeraVisible + fila;
    if (i >= cantidad) break;

    const int y = 55 + fila * 22;
    const bool sel = i == seleccion;

    if (sel) {
      canvas.fillRoundRect(27, y - 9, SCR_W - 54, 19, 5, TFT_BLACK);
      canvas.drawRoundRect(27, y - 9, SCR_W - 54, 19, 5, colorCursor);
    }

    String etiqueta = nombres[i];
    if (etiqueta.length() > 32) etiqueta = etiqueta.substring(0, 29) + "...";
    canvas.setTextColor(sel ? colorCursor : TFT_WHITE);
    canvas.drawString(etiqueta, 36, y, 1);
  }

  canvas.setTextDatum(MC_DATUM);
  canvas.setTextColor(TFT_LIGHTGREY);
  canvas.drawString(
      String(seleccion + 1) + "/" + String(cantidad) +
      "   A: abrir   MENU: volver",
      SCR_W / 2, 214, 1
  );
  canvas.setTextDatum(TL_DATUM);
  perfilPushSprite(0, 0);
}

void entrar() {
  if (!mostrarAdvertenciaModoNES()) {
    gameState = STATE_EMULADORES;
    EmuladoresMenu::dibujar();
    return;
  }

  escanear();
  vistaFicha = false;
  opcionFicha = 0;
  gameState = STATE_NES_BROWSER;

  // Reportamos la entrada antes de apagar Wi-Fi durante el gameplay.
  dibujar();
}

void mostrarPendiente() {
  canvas.fillRoundRect(30, 72, SCR_W - 60, 96, 10, TFT_BLACK);
  canvas.drawRoundRect(30, 72, SCR_W - 60, 96, 10, TFT_RED);
  canvas.setTextDatum(MC_DATUM);
  canvas.setTextColor(TFT_WHITE);
  canvas.drawString("ROM DETECTADA", SCR_W / 2, 93, 2);
  canvas.setTextColor(TFT_YELLOW);
  String n = nombres[seleccion];
  if (n.length() > 27) n = n.substring(0, 24) + "...";
  canvas.drawString(n, SCR_W / 2, 119, 1);
  canvas.setTextColor(TFT_LIGHTGREY);
  canvas.drawString("Retro-Go listo para ejecutar", SCR_W / 2, 142, 1);
  canvas.setTextDatum(TL_DATUM);
  perfilPushSprite(0, 0);
  delay(700);
  dibujar();
}

void actualizar(bool btnPressed) {
  static Direccion anterior = DIR_NINGUNO;
  static bool menuAnterior = false;

  const Direccion dir = clasificarCruceta(leerCrucetaADC());

  if (vistaFicha) {
    if (dir != DIR_NINGUNO && dir != anterior &&
        (dir == DIR_IZQUIERDA || dir == DIR_DERECHA)) {
      opcionFicha = 1 - opcionFicha;
      beepMenuMove();
      FichaJuego::botones(opcionFicha);
    }
    anterior = dir;

    const bool menuAhora = digitalRead(PIN_PAUSA) == LOW;
    const bool menuPulsado = menuAhora && !menuAnterior;
    menuAnterior = menuAhora;

    if (menuPulsado) {
      esperarSoltarMenu();
      vistaFicha = false;
      dibujar();
      return;
    }

    if (btnPressed) {
      if (opcionFicha == 1) {
        vistaFicha = false;
        dibujar();
        return;
      }

      // Desde aquí comienza EXACTAMENTE el flujo del emulador.
      // La portada ya fue liberada al terminar FichaJuego::dibujar().
      const EstadoServiciosNES serviciosAntes = pausarServiciosParaJuego();
      const bool pudoArrancar =
          CocodriloNES::ejecutar(rutas[seleccion].c_str(), tft);

      if (pudoArrancar) {
        Serial.println("[NES] Salida -> reinicio controlado al menu");
        cocodriloRetornoNES = COCODRILO_RETORNO_NES_MAGIC;
        delay(40);
        ESP.restart();
        return;
      }

      restaurarServiciosDespuesDelJuego(serviciosAntes);
      invalidarMenuBase();
      vistaFicha = false;
      dibujar();

      if (!pudoArrancar) {
        canvas.fillRoundRect(24, 72, SCR_W - 48, 96, 10, TFT_BLACK);
        canvas.drawRoundRect(24, 72, SCR_W - 48, 96, 10, TFT_RED);
        canvas.setTextDatum(MC_DATUM);
        canvas.setTextColor(TFT_WHITE);
        canvas.drawString("RETRO-GO NO DISPONIBLE", SCR_W / 2, 95, 2);
        canvas.setTextColor(TFT_LIGHTGREY);
        canvas.drawString("Revisa COCODRILO_NES_RetroGo.h", SCR_W / 2, 124, 1);
        canvas.drawString("en la carpeta del sketch", SCR_W / 2, 142, 1);
        canvas.setTextDatum(TL_DATUM);
        perfilPushSprite(0, 0);
        delay(1300);
        dibujar();
      }
    }
    return;
  }

  if (cantidad > 0 && dir != DIR_NINGUNO && dir != anterior) {
    if (dir == DIR_ARRIBA || dir == DIR_ABAJO) {
      const int seleccionAnterior = seleccion;
      const int primeraAnterior = primeraVisible;

      if (dir == DIR_ARRIBA)
        seleccion = (seleccion + cantidad - 1) % cantidad;
      else
        seleccion = (seleccion + 1) % cantidad;

      beepMenuMove();
      actualizarCursorNES(seleccionAnterior, primeraAnterior);
    }
  }
  anterior = dir;

  const bool menuAhora = digitalRead(PIN_PAUSA) == LOW;
  const bool menuPulsado = menuAhora && !menuAnterior;
  menuAnterior = menuAhora;

  if (menuPulsado) {
    esperarSoltarMenu();
    gameState = STATE_EMULADORES;
    EmuladoresMenu::dibujar();
    return;
  }

  if (btnPressed && cantidad > 0) {
    vistaFicha = true;
    opcionFicha = 0;
    FichaJuego::dibujar("nes", nombres[seleccion], "Nintendo NES", opcionFicha);
  }
}

} // namespace NESLauncher

// ════════════════════════════════════════════════════════════
//  GAME BOY / GAME BOY COLOR — navegador de ROMs desde /GBC
// ════════════════════════════════════════════════════════════
namespace GBCLauncher {

static const int MAX_ROMS = 60;
static const int VISIBLES = 7;
String nombres[MAX_ROMS];
String rutas[MAX_ROMS];
int cantidad = 0;
int seleccion = 0;
int primeraVisible = 0;
static bool vistaFicha = false;
static int opcionFicha = 0;

bool terminaEnGB(const String &nombre) {
  String n = nombre;
  n.toLowerCase();
  return n.endsWith(".gb") || n.endsWith(".gbc");
}

String nombreBonito(const String &ruta) {
  int slash = ruta.lastIndexOf('/');
  String n = (slash >= 0) ? ruta.substring(slash + 1) : ruta;
  String low = n;
  low.toLowerCase();
  if (low.endsWith(".gbc") && n.length() > 4) n.remove(n.length() - 4);
  else if (low.endsWith(".gb") && n.length() > 3) n.remove(n.length() - 3);
  return n;
}

void escanear() {
  cantidad = 0;
  seleccion = 0;
  primeraVisible = 0;

  File dir = SD_MMC.open("/GBC");
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }

  File f = dir.openNextFile();
  while (f && cantidad < MAX_ROMS) {
    if (!f.isDirectory()) {
      String ruta = String(f.name());
      if (!ruta.startsWith("/")) ruta = String("/GBC/") + ruta;
      if (terminaEnGB(ruta)) {
        rutas[cantidad] = ruta;
        nombres[cantidad] = nombreBonito(ruta);
        cantidad++;
      }
    }
    f.close();
    f = dir.openNextFile();
  }
  dir.close();

  for (int i = 0; i < cantidad - 1; ++i) {
    for (int j = i + 1; j < cantidad; ++j) {
      if (nombres[j].compareTo(nombres[i]) < 0) {
        String tn = nombres[i]; nombres[i] = nombres[j]; nombres[j] = tn;
        String tr = rutas[i]; rutas[i] = rutas[j]; rutas[j] = tr;
      }
    }
  }
}

static void ajustarScroll() {
  if (seleccion < primeraVisible) primeraVisible = seleccion;
  if (seleccion >= primeraVisible + VISIBLES)
    primeraVisible = seleccion - VISIBLES + 1;
  if (primeraVisible < 0) primeraVisible = 0;
}

void dibujar() {
  dibujarWallpaperActivo(rutaWallpaperActual.c_str(), canvas);
  canvas.fillRoundRect(15, 10, SCR_W - 30, SCR_H - 20, 12, 0x1082);
  canvas.drawRoundRect(15, 10, SCR_W - 30, SCR_H - 20, 12, TFT_WHITE);
  canvas.setTextDatum(MC_DATUM);
  canvas.setTextColor(TFT_CYAN);
  canvas.drawString("GAME BOY / GBC", SCR_W / 2, 27, 2);

  if (SD_MMC.cardType() == CARD_NONE) {
    canvas.setTextColor(TFT_WHITE);
    canvas.drawString("Tarjeta SD no disponible", SCR_W / 2, 103, 2);
    canvas.setTextColor(TFT_LIGHTGREY);
    canvas.drawString("MENU: volver", SCR_W / 2, 210, 1);
    canvas.setTextDatum(TL_DATUM);
    perfilPushSprite(0, 0);
    return;
  }

  if (cantidad == 0) {
    canvas.setTextColor(TFT_WHITE);
    canvas.drawString("No hay ROMs en /GBC/", SCR_W / 2, 95, 2);
    canvas.setTextColor(TFT_LIGHTGREY);
    canvas.drawString("Copia archivos .gb o .gbc", SCR_W / 2, 120, 1);
    canvas.drawString("MENU: volver", SCR_W / 2, 210, 1);
    canvas.setTextDatum(TL_DATUM);
    perfilPushSprite(0, 0);
    return;
  }

  ajustarScroll();
  canvas.setTextDatum(ML_DATUM);

  for (int fila = 0; fila < VISIBLES; ++fila) {
    const int i = primeraVisible + fila;
    if (i >= cantidad) break;
    const int y = 55 + fila * 22;
    const bool sel = i == seleccion;

    if (sel) {
      canvas.fillRoundRect(27, y - 9, SCR_W - 54, 19, 5, TFT_BLACK);
      canvas.drawRoundRect(27, y - 9, SCR_W - 54, 19, 5, colorCursor);
    }

    String etiqueta = nombres[i];
    if (etiqueta.length() > 32) etiqueta = etiqueta.substring(0, 29) + "...";
    canvas.setTextColor(sel ? colorCursor : TFT_WHITE);
    canvas.drawString(etiqueta, 36, y, 1);
  }

  canvas.setTextDatum(MC_DATUM);
  canvas.setTextColor(TFT_LIGHTGREY);
  canvas.drawString(String(seleccion + 1) + "/" + String(cantidad) +
                    "   A: abrir   MENU: volver",
                    SCR_W / 2, 214, 1);
  canvas.setTextDatum(TL_DATUM);
  perfilPushSprite(0, 0);
}

void entrar() {
  escanear();
  vistaFicha = false;
  opcionFicha = 0;
  gameState = STATE_GBC_BROWSER;
  dibujar();
}

void actualizar(bool btnPressed) {
  static Direccion anterior = DIR_NINGUNO;
  static bool menuAnterior = false;

  const Direccion dir = clasificarCruceta(leerCrucetaADC());

  if (vistaFicha) {
    if (dir != DIR_NINGUNO && dir != anterior &&
        (dir == DIR_IZQUIERDA || dir == DIR_DERECHA)) {
      opcionFicha = 1 - opcionFicha;
      beepMenuMove();
      FichaJuego::botones(opcionFicha);
    }
    anterior = dir;

    const bool menuAhora = digitalRead(PIN_PAUSA) == LOW;
    const bool menuPulsado = menuAhora && !menuAnterior;
    menuAnterior = menuAhora;

    if (menuPulsado) {
      esperarSoltarMenu();
      vistaFicha = false;
      dibujar();
      return;
    }

    if (btnPressed) {
      if (opcionFicha == 1) {
        vistaFicha = false;
        dibujar();
        return;
      }

      // La portada ya no ocupa memoria. Desde aquí corre el launcher original.
      const NESLauncher::EstadoServiciosNES serviciosAntes =
          NESLauncher::pausarServiciosParaJuego();

      const bool pudoArrancar =
          CocodriloGBC::ejecutar(rutas[seleccion].c_str(), tft);

      if (pudoArrancar) {
        // Igual que NES: después de salir del core, reiniciar por completo
        // garantiza que TFT/SPI/DMA/I2S vuelvan a un estado limpio.
        Serial.println("[GBC] Salida -> reinicio controlado al menu");
        cocodriloRetornoNES = COCODRILO_RETORNO_NES_MAGIC;
        delay(40);
        ESP.restart();
        return;
      }

      NESLauncher::restaurarServiciosDespuesDelJuego(serviciosAntes);

      tft.setRotation(1);
      tft.setSwapBytes(false);
      invalidarMenuBase();
      vistaFicha = false;
      dibujar();

      if (!pudoArrancar) {
        canvas.fillRoundRect(24, 72, SCR_W - 48, 96, 10, TFT_BLACK);
        canvas.drawRoundRect(24, 72, SCR_W - 48, 96, 10, TFT_RED);
        canvas.setTextDatum(MC_DATUM);
        canvas.setTextColor(TFT_WHITE);
        canvas.drawString("GBC NO PUDO ARRANCAR", SCR_W / 2, 95, 2);
        canvas.setTextColor(TFT_LIGHTGREY);
        canvas.drawString("Revisa COCODRILO_GBC_RetroGo.h", SCR_W / 2, 124, 1);
        canvas.setTextDatum(TL_DATUM);
        perfilPushSprite(0, 0);
        delay(1300);
        dibujar();
      }
    }
    return;
  }

  if (cantidad > 0 && dir != DIR_NINGUNO && dir != anterior) {
    if (dir == DIR_ARRIBA) {
      seleccion = (seleccion + cantidad - 1) % cantidad;
      beepMenuMove();
      dibujar();
    } else if (dir == DIR_ABAJO) {
      seleccion = (seleccion + 1) % cantidad;
      beepMenuMove();
      dibujar();
    }
  }
  anterior = dir;

  const bool menuAhora = digitalRead(PIN_PAUSA) == LOW;
  const bool menuPulsado = menuAhora && !menuAnterior;
  menuAnterior = menuAhora;

  if (menuPulsado) {
    esperarSoltarMenu();
    gameState = STATE_EMULADORES;
    EmuladoresMenu::dibujar();
    return;
  }

  if (btnPressed && cantidad > 0) {
    vistaFicha = true;
    opcionFicha = 0;
    FichaJuego::dibujar("gbc", nombres[seleccion],
                        "Game Boy / Game Boy Color", opcionFicha);
  }
}

} // namespace GBCLauncher

// ════════════════════════════════════════════════════════════
//  MENÚ DE EMULADORES — NES + GAME BOY / GAME BOY COLOR
// ════════════════════════════════════════════════════════════
namespace EmuladoresMenu {

int seleccion = 0;
static bool menuAnterior = false;
static Direccion direccionAnterior = DIR_NINGUNO;

void dibujar() {
  dibujarWallpaperActivo(rutaWallpaperActual.c_str(), canvas);
  canvas.fillRoundRect(36, 25, SCR_W - 72, SCR_H - 50, 14, TFT_BLACK);
  canvas.drawRoundRect(36, 25, SCR_W - 72, SCR_H - 50, 14, colorCursor);
  canvas.setTextDatum(MC_DATUM);
  canvas.setTextColor(TFT_WHITE, TFT_BLACK);
  canvas.drawString("EMULADORES", SCR_W / 2, 48, 2);

  static const char* opciones[] = {
    "Nintendo NES",
    "Game Boy / Game Boy Color"
  };

  for (int i = 0; i < 2; ++i) {
    const int y = 96 + i * 48;
    const bool sel = (i == seleccion);
    if (sel) {
      canvas.fillRoundRect(58, y - 16, SCR_W - 116, 32, 7, 0x2104);
      canvas.drawRoundRect(58, y - 16, SCR_W - 116, 32, 7, colorCursor);
    }
    canvas.setTextColor(sel ? colorCursor : TFT_WHITE, sel ? 0x2104 : TFT_BLACK);
    canvas.drawString(opciones[i], SCR_W / 2, y, 1);
  }

  canvas.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  canvas.drawString("A: abrir   MENU: volver", SCR_W / 2, 207, 1);
  canvas.setTextDatum(TL_DATUM);
  perfilPushSprite(0, 0);
}

void entrar() {
  seleccion = 0;
  menuAnterior = digitalRead(PIN_PAUSA) == LOW;
  direccionAnterior = DIR_NINGUNO;
  gameState = STATE_EMULADORES;
  dibujar();
}

void actualizar(bool btnPressed) {
  const Direccion dir = clasificarCruceta(leerCrucetaADC());
  if (dir != DIR_NINGUNO && dir != direccionAnterior) {
    if (dir == DIR_ARRIBA || dir == DIR_ABAJO) {
      seleccion = 1 - seleccion;
      beepMenuMove();
      dibujar();
    }
  }
  direccionAnterior = dir;

  const bool menuAhora = digitalRead(PIN_PAUSA) == LOW;
  const bool menuPulsado = menuAhora && !menuAnterior;
  menuAnterior = menuAhora;

  if (menuPulsado) {
    esperarSoltarMenu();
    gameState = STATE_MENU;
    volumenPanelAbierto = false;
    invalidarMenuBase();
    drawMenu();
    return;
  }

  if (btnPressed) {
    if (seleccion == 0) NESLauncher::entrar();
    else GBCLauncher::entrar();
  }
}

} // namespace EmuladoresMenu

void prepararPantallaPersonalizacion(const char *titulo);
void drawPersonalizacion();
void mostrarAvisoActualizacionWiFi();

void drawMenuSD();
void drawConfirmacionActualizacion();
void drawProgresoActualizacion(size_t escritos, size_t total);

void esperarSoltarMenu() {
  while (digitalRead(PIN_PAUSA) == LOW) delay(5);
  delay(25);
}

// Hora de COCODRILO OS.
// Por ahora usa UTC-6 fijo. Más adelante puede convertirse en una opción
// configurable dentro del menú Wi-Fi.
bool horaSistemaValida() {
  time_t ahora = time(nullptr);
  return ahora > 1700000000;
}

bool sincronizarHoraNTP(uint32_t esperaMs = 5000UL) {
  if (WiFi.status() != WL_CONNECTED) {
    horaSincronizada = false;
    return false;
  }

  configTime(
    -6 * 3600,  // UTC-6
    0,
    "pool.ntp.org",
    "time.google.com",
    "time.cloudflare.com"
  );

  const uint32_t inicio = millis();
  while (!horaSistemaValida() && millis() - inicio < esperaMs) {
    delay(100);
  }

  horaSincronizada = horaSistemaValida();

  if (horaSincronizada) {
    struct tm tiempoLocal;
    if (getLocalTime(&tiempoLocal, 20)) {
      Serial.printf(
        "Hora NTP sincronizada: %02d/%02d/%04d %02d:%02d:%02d\n",
        tiempoLocal.tm_mday,
        tiempoLocal.tm_mon + 1,
        tiempoLocal.tm_year + 1900,
        tiempoLocal.tm_hour,
        tiempoLocal.tm_min,
        tiempoLocal.tm_sec
      );
    }
  } else {
    Serial.println("No se pudo sincronizar la hora NTP");
  }

  minutoEncabezadoAnterior = -1;
  return horaSincronizada;
}

// ════════════════════════════════════════════════════════════
//  WIFI DE COCODRILO OS
// ════════════════════════════════════════════════════════════
void cargarConfiguracionWiFi() {
  preferenciasArcade.begin("arcade", true);
  wifiActivado = preferenciasArcade.getBool("wifiOn", false);
  wifiSSIDGuardado = preferenciasArcade.getString("wifiSSID", "");
  wifiClaveGuardada = preferenciasArcade.getString("wifiPass", "");
  preferenciasArcade.end();

  wifiConfigurado = wifiSSIDGuardado.length() > 0;
}

void guardarEstadoWiFi() {
  preferenciasArcade.begin("arcade", false);
  preferenciasArcade.putBool("wifiOn", wifiActivado);
  preferenciasArcade.end();
}

void guardarCredencialesWiFi(const String &ssid, const String &clave) {
  wifiSSIDGuardado = ssid;
  wifiClaveGuardada = clave;
  wifiConfigurado = wifiSSIDGuardado.length() > 0;

  preferenciasArcade.begin("arcade", false);
  preferenciasArcade.putString("wifiSSID", wifiSSIDGuardado);
  preferenciasArcade.putString("wifiPass", wifiClaveGuardada);
  preferenciasArcade.putBool("wifiOn", true);
  preferenciasArcade.end();

  wifiActivado = true;
}

void olvidarRedWiFi() {
  WiFi.disconnect(true, true);
  wifiSSIDGuardado = "";
  wifiClaveGuardada = "";
  wifiConfigurado = false;
  horaSincronizada = false;
  minutoEncabezadoAnterior = -1;

  preferenciasArcade.begin("arcade", false);
  preferenciasArcade.remove("wifiSSID");
  preferenciasArcade.remove("wifiPass");
  preferenciasArcade.end();
}

bool conectarWiFiGuardado(uint32_t esperaMs = 4500UL) {
  if (!wifiActivado) {
    WiFi.mode(WIFI_OFF);
    estadoWiFiArranque = "Desactivado";
    horaSincronizada = false;
    minutoEncabezadoAnterior = -1;
    return false;
  }

  if (!wifiConfigurado) {
    estadoWiFiArranque = "Configurar";
    horaSincronizada = false;
    minutoEncabezadoAnterior = -1;
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);
  WiFi.begin(wifiSSIDGuardado.c_str(), wifiClaveGuardada.c_str());

  const uint32_t inicio = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - inicio < esperaMs) {
    delay(80);
  }

  if (WiFi.status() == WL_CONNECTED) {
    estadoWiFiArranque = "Conectado";
    Serial.printf("WiFi conectado: %s | IP: %s\n",
                  WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    sincronizarHoraNTP();
    return true;
  }

  estadoWiFiArranque = "Sin conexion";
  horaSincronizada = false;
  minutoEncabezadoAnterior = -1;
  WiFi.disconnect(false, false);
  return false;
}

// Arranque rápido: inicia la asociación y espera solo un tiempo corto.
// Si no termina, NO cancela Wi-Fi; el intento sigue vivo en segundo plano.






// Se llama frecuentemente desde loop(). No espera por Wi-Fi ni por NTP.


String paginaConfiguracionWiFi() {
  String pagina;
  pagina.reserve(5000);
  pagina += F("<!doctype html><html><head><meta charset='utf-8'>");
  pagina += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  pagina += F("<title>COCODRILO OS WiFi</title><style>");
  pagina += F("body{font-family:Arial;background:#071611;color:#eafff6;max-width:560px;margin:35px auto;padding:18px}");
  pagina += F(".c{background:#102a22;border:1px solid #39d98a;border-radius:14px;padding:20px}");
  pagina += F("h1{color:#39d98a}label{display:block;margin-top:14px}select,input,button{width:100%;box-sizing:border-box;padding:12px;margin-top:6px;border-radius:9px;border:1px solid #50766a}");
  pagina += F("button{background:#39d98a;color:#071611;font-weight:bold}.p{color:#b7d5cb;font-size:14px}</style></head><body>");
  pagina += F("<div class='c'><h1>COCODRILO OS</h1><p>Configuracion Wi-Fi</p>");
  pagina += F("<form action='/guardar' method='post'><label>Red</label><select name='ssid'>");

  const int redes = WiFi.scanNetworks();
  if (redes <= 0) {
    pagina += F("<option value=''>No se encontraron redes</option>");
  } else {
    for (int i = 0; i < redes; ++i) {
      String ssid = WiFi.SSID(i);
      ssid.replace("&", "&amp;");
      ssid.replace("<", "&lt;");
      ssid.replace(">", "&gt;");
      ssid.replace("\"", "&quot;");
      pagina += "<option value=\"" + ssid + "\">" + ssid +
                " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
    }
  }

  pagina += F("</select><label>Contrasena</label>");
  pagina += F("<input name='clave' type='password' maxlength='63' autocomplete='current-password'>");
  pagina += F("<button type='submit'>Guardar y conectar</button></form>");
  pagina += F("<p class='p'>La consola seguira funcionando sin Internet.</p></div></body></html>");
  WiFi.scanDelete();
  return pagina;
}

void iniciarPortalWiFi() {
  // El portal necesita conservar AP+STA hasta que detenerPortalWiFi() lo cierre.
  // Primero detenemos cualquier AP anterior sin apagar por completo el radio.
  servidorWiFi.stop();
  dnsWiFi.stop();
  WiFi.softAPdisconnect(true);
  delay(100);

  // AP+STA permite mostrar COCODRILO-SETUP y a la vez escanear redes.
  WiFi.mode(WIFI_AP_STA);

  // Durante el portal desactivamos power-save: mejora descubrimiento/estabilidad
  // del AP en telefonos y evita que el radio duerma entre beacons.
  WiFi.setSleep(false);
  delay(50);

  // Canal 1, visible, hasta 4 clientes.
  const bool apOK = WiFi.softAP("COCODRILO-SETUP", "", 1, false, 4);
  if (!apOK) {
    portalWiFiActivo = false;
    Serial.println("Portal WiFi: ERROR al iniciar COCODRILO-SETUP");
    return;
  }

  portalWiFiActivo = true;
  credencialesWiFiRecibidas = false;

  const IPAddress ip = WiFi.softAPIP();
  dnsWiFi.start(53, "*", ip);

  servidorWiFi.on("/", HTTP_GET, []() {
    servidorWiFi.send(200, "text/html", paginaConfiguracionWiFi());
  });

  servidorWiFi.on("/guardar", HTTP_POST, []() {
    const String ssid = servidorWiFi.arg("ssid");
    const String clave = servidorWiFi.arg("clave");

    if (ssid.length() == 0) {
      servidorWiFi.send(400, "text/plain", "Selecciona una red.");
      return;
    }

    guardarCredencialesWiFi(ssid, clave);
    credencialesWiFiRecibidas = true;
    servidorWiFi.send(
      200, "text/html",
      "<html><body style='font-family:Arial;background:#071611;color:#eafff6'>"
      "<h2>Configuracion guardada</h2>"
      "<p>COCODRILO OS intentara conectarse. Ya puedes cerrar esta pagina.</p>"
      "</body></html>"
    );
  });

  servidorWiFi.onNotFound([]() {
    servidorWiFi.sendHeader("Location", "http://192.168.4.1/", true);
    servidorWiFi.send(302, "text/plain", "");
  });

  servidorWiFi.begin();

  Serial.printf("Portal WiFi: COCODRILO-SETUP | IP: %s | canal: %d | clientes: %d\n",
                ip.toString().c_str(),
                WiFi.channel(),
                WiFi.softAPgetStationNum());
}

void detenerPortalWiFi() {
  if (!portalWiFiActivo) return;

  servidorWiFi.stop();
  dnsWiFi.stop();
  WiFi.softAPdisconnect(true);
  portalWiFiActivo = false;

  // Volver al comportamiento Wi-Fi estable original.
  // No activar auto-reconnect ni lanzar begin() desde aquí.
  if (wifiActivado) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(true);
  } else {
    WiFi.mode(WIFI_OFF);
  }
}

int opcionWiFi = 0;

void drawMenuWiFi() {
  prepararPantallaPersonalizacion("WIFI");

  canvas.fillRoundRect(28, 57, SCR_W - 56, 139, 9, 0x2104);
  canvas.drawRoundRect(28, 57, SCR_W - 56, 139, 9, TFT_DARKGREY);

  String estado;
  if (!wifiActivado) estado = "Desactivado";
  else if (!wifiConfigurado) estado = "Sin configurar";
  else if (WiFi.status() == WL_CONNECTED) estado = "Conectado";
  else estado = "Sin conexion";

  canvas.setTextDatum(MC_DATUM);
  canvas.setTextColor(TFT_LIGHTGREY);
  canvas.drawString("Estado: " + estado, SCR_W / 2, 70, 1);

  static const char *opciones[] = {
    "Activar / Desactivar",
    "Configurar red",
    "Olvidar red"
  };

  for (int i = 0; i < 3; ++i) {
    const int y = 100 + i * 38;
    const bool seleccionado = opcionWiFi == i;

    if (seleccionado) {
      canvas.fillRoundRect(43, y - 14, SCR_W - 86, 29, 6, TFT_BLACK);
      canvas.drawRoundRect(43, y - 14, SCR_W - 86, 29, 6, colorCursor);
    }

    canvas.setTextColor(seleccionado ? colorCursor : TFT_WHITE);
    canvas.drawString(String(seleccionado ? "> " : "  ") + opciones[i],
                      SCR_W / 2, y, 2);
  }

  canvas.fillRect(0, SCR_H - 28, SCR_W, 28, TFT_BLACK);
  canvas.setTextColor(TFT_LIGHTGREY);
  canvas.drawString("Arriba/Abajo: elegir   A: aceptar   MENU: volver",
                    SCR_W / 2, SCR_H - 8, 1);
  canvas.setTextDatum(TL_DATUM);
  perfilPushSprite(0, 0);
}

void drawPortalWiFi() {
  canvas.fillSprite(TFT_BLACK);
  canvas.setTextWrap(false);
  canvas.setTextDatum(MC_DATUM);

  canvas.drawRoundRect(12, 10, SCR_W - 24, SCR_H - 20, 10, TFT_DARKGREY);
  canvas.drawFastHLine(28, 43, SCR_W - 56, TFT_DARKGREY);

  canvas.setTextColor(TFT_GREEN, TFT_BLACK);
  canvas.drawString("CONFIGURAR WIFI", SCR_W / 2, 28, 2);

  // QR de red abierta: WIFI:T:nopass;S:COCODRILO-SETUP;;
  QR::dibujarWiFi(canvas, 239, 119, 120);

  canvas.setTextDatum(MC_DATUM);
  canvas.setTextColor(TFT_WHITE, TFT_BLACK);
  canvas.drawString("Escanea el QR", 86, 67, 2);
  canvas.drawString("para conectarte", 86, 87, 1);

  canvas.setTextColor(colorCursor, TFT_BLACK);
  canvas.drawString("COCODRILO-", 86, 119, 2);
  canvas.drawString("SETUP", 86, 139, 2);

  canvas.setTextColor(TFT_CYAN, TFT_BLACK);
  canvas.drawString("Luego abre:", 86, 170, 1);
  canvas.drawString("192.168.4.1", 86, 188, 1);

  canvas.setTextDatum(BC_DATUM);
  canvas.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  canvas.drawString("Esperando configuracion...   MENU: cancelar",
                    SCR_W / 2, 228, 1);

  canvas.setTextDatum(TL_DATUM);
  perfilPushSprite(0, 0);
}

void entrarMenuWiFi() {
  cargarConfiguracionWiFi();
  opcionWiFi = 0;
  gameState = STATE_CONFIG_WIFI;
  drawMenuWiFi();
}

void actualizarMenuWiFi(bool btnPressed) {
  static Direccion anterior = DIR_NINGUNO;
  static bool pausaAnterior = false;

  const Direccion dir = clasificarCruceta(leerCrucetaADC());
  if (dir != anterior) {
    anterior = dir;
    if (dir == DIR_ARRIBA) {
      opcionWiFi = (opcionWiFi + 2) % 3;
      beepMenuMove();
      drawMenuWiFi();
    } else if (dir == DIR_ABAJO) {
      opcionWiFi = (opcionWiFi + 1) % 3;
      beepMenuMove();
      drawMenuWiFi();
    }
  }

  const bool pausa = digitalRead(PIN_PAUSA) == LOW;
  if (pausa && !pausaAnterior) {
    esperarSoltarMenu();
    gameState = STATE_CONFIG_MAIN;
    drawPersonalizacion();
  }
  pausaAnterior = pausa;
  if (gameState != STATE_CONFIG_WIFI) return;

  if (!btnPressed) return;
  beepMenuMove();

  if (opcionWiFi == 0) {
    wifiActivado = !wifiActivado;
    guardarEstadoWiFi();

    if (!wifiActivado) {
      WiFi.disconnect(true, false);
      WiFi.mode(WIFI_OFF);
      horaSincronizada = false;
      minutoEncabezadoAnterior = -1;
    } else if (wifiConfigurado) {
      conectarWiFiGuardado(4500);
    }

    drawMenuWiFi();
  } else if (opcionWiFi == 1) {
    wifiActivado = true;
    guardarEstadoWiFi();
    iniciarPortalWiFi();
    gameState = STATE_CONFIG_WIFI_PORTAL;
    drawPortalWiFi();
  } else {
    if (wifiConfigurado) {
      olvidarRedWiFi();
      beepMenuMove();
    } else {
      beepDenegado();
    }
    drawMenuWiFi();
  }
}

void actualizarPortalWiFi() {
  static bool pausaAnterior = false;

  dnsWiFi.processNextRequest();
  servidorWiFi.handleClient();

  if (credencialesWiFiRecibidas) {
    credencialesWiFiRecibidas = false;
    detenerPortalWiFi();

    canvas.fillScreen(TFT_BLACK);
    canvas.setTextDatum(MC_DATUM);
    canvas.setTextColor(TFT_WHITE);
    canvas.drawString("Conectando...", SCR_W / 2, SCR_H / 2 - 10, 2);
    perfilPushSprite(0, 0);

    conectarWiFiGuardado(8000);
    delay(500);
    gameState = STATE_CONFIG_WIFI;
    drawMenuWiFi();
    return;
  }

  const bool pausa = digitalRead(PIN_PAUSA) == LOW;
  if (pausa && !pausaAnterior) {
    esperarSoltarMenu();
    detenerPortalWiFi();
    gameState = STATE_CONFIG_WIFI;
    drawMenuWiFi();
  }
  pausaAnterior = pausa;
}

// ════════════════════════════════════════════════════════════
//  PERSONALIZACIÓN — Wallpaper, color, Wi-Fi y accesos futuros
//
//  Rendimiento:
//  - El JPG de fondo se decodifica una sola vez al entrar en cada pantalla.
//  - Al mover el cursor solo se repinta un panel opaco pequeño.
//  - No se conserva una copia adicional del wallpaper ni se usa una caché.
// ════════════════════════════════════════════════════════════
int opcionPersonalizacion = 0;
int primeraOpcionPersonalizacionVisible = 0;
static constexpr int TOTAL_OPCIONES_PERSONALIZACION = 10;
static constexpr int OPCIONES_PERSONALIZACION_VISIBLES = 7;

int opcionMenuSD = 0;
bool pantallaResultadoSD = false;
String tituloResultadoSD;
String detalleResultadoSD;
ActualizacionSD::Paquete paqueteActualizacion;
size_t progresoActualizacionEscrito = 0;
size_t progresoActualizacionTotal = 0;

void drawSelectorAppIcono();
void drawSelectorIcono();
void drawAcercaDe();
void drawMenuBrillo();
void actualizarMenuBrillo();
void drawActualizaciones();
void entrarActualizaciones();
void actualizarActualizaciones(bool btnPressed);

void prepararPantallaPersonalizacion(const char *titulo) {
  if (!dibujarWallpaperActivo(rutaWallpaperActual.c_str(), canvas)) {
    canvas.fillScreen(TFT_BLACK);
  }

  canvas.fillRoundRect(12, 9, SCR_W - 24, 40, 8, 0x2104);
  canvas.drawRoundRect(12, 9, SCR_W - 24, 40, 8, colorCursor);
  canvas.setTextDatum(MC_DATUM);
  canvas.setTextColor(colorCursor);
  canvas.drawString(titulo, SCR_W / 2, 29, 2);
}


void cargarConfiguracionPin() {
  preferenciasArcade.begin("arcade", true);
  bloqueoPinActivo = preferenciasArcade.getBool("pinOn", false);
  pinGuardado = preferenciasArcade.getUShort("pinCode", 0);
  preferenciasArcade.end();
}

void guardarConfiguracionPin() {
  preferenciasArcade.begin("arcade", false);
  preferenciasArcade.putBool("pinOn", bloqueoPinActivo);
  preferenciasArcade.putUShort("pinCode", pinGuardado);
  preferenciasArcade.end();
}

uint16_t digitosAPin(const uint8_t digitos[4]) {
  return uint16_t(digitos[0]) * 1000U +
         uint16_t(digitos[1]) * 100U +
         uint16_t(digitos[2]) * 10U +
         uint16_t(digitos[3]);
}

void pinADigitos(uint16_t pin, uint8_t digitos[4]) {
  digitos[0] = (pin / 1000U) % 10U;
  digitos[1] = (pin / 100U) % 10U;
  digitos[2] = (pin / 10U) % 10U;
  digitos[3] = pin % 10U;
}

void dibujarDigitosPin(TFT_eSprite &spr, int centroY, bool ocultar = false) {
  const int inicioX = 82;
  const int separacion = 52;

  spr.setTextDatum(MC_DATUM);
  spr.setTextFont(4);

  for (int i = 0; i < 4; ++i) {
    const int x = inicioX + i * separacion;
    const bool activo = (i == posicionPin);

    if (activo) {
      spr.fillRoundRect(x - 20, centroY - 25, 40, 50, 6, TFT_BLACK);
      spr.drawRoundRect(x - 20, centroY - 25, 40, 50, 6, colorCursor);
    } else {
      spr.fillRoundRect(x - 20, centroY - 25, 40, 50, 6, 0x2104);
      spr.drawRoundRect(x - 20, centroY - 25, 40, 50, 6, TFT_DARKGREY);
    }

    spr.setTextColor(activo ? colorCursor : TFT_WHITE);
    if (ocultar) {
      spr.drawString("*", x, centroY);
    } else {
      spr.drawString(String(pinEdicion[i]), x, centroY);
    }
  }

  spr.setTextDatum(TL_DATUM);
  spr.setTextFont(1);
}

void drawMenuPin() {
  prepararPantallaPersonalizacion("CONTRASENA");

  canvas.fillRoundRect(28, 58, SCR_W - 56, 139, 9, 0x2104);
  canvas.drawRoundRect(28, 58, SCR_W - 56, 139, 9, TFT_DARKGREY);

  canvas.setTextDatum(MC_DATUM);
  canvas.setTextColor(TFT_LIGHTGREY);
  canvas.drawString(
    String("Bloqueo: ") + (bloqueoPinActivo ? "Activado" : "Desactivado"),
    SCR_W / 2, 77, 1
  );

  static const char* opciones[] = {
    "Activar / Desactivar",
    "Cambiar PIN"
  };

  for (int i = 0; i < 2; ++i) {
    const int y = 118 + i * 43;
    const bool seleccionado = opcionMenuPin == i;

    if (seleccionado) {
      canvas.fillRoundRect(43, y - 15, SCR_W - 86, 31, 6, TFT_BLACK);
      canvas.drawRoundRect(43, y - 15, SCR_W - 86, 31, 6, colorCursor);
    }

    canvas.setTextColor(seleccionado ? colorCursor : TFT_WHITE);
    canvas.drawString(
      String(seleccionado ? "> " : "  ") + opciones[i],
      SCR_W / 2, y, 2
    );
  }

  canvas.fillRect(0, SCR_H - 27, SCR_W, 27, TFT_BLACK);
  canvas.setTextColor(TFT_LIGHTGREY);
  canvas.drawString("Arriba/Abajo: elegir  A: aceptar  MENU: volver",
                    SCR_W / 2, SCR_H - 8, 1);
  canvas.setTextDatum(TL_DATUM);
  perfilPushSprite(0, 0);
}

void comenzarEdicionPin(bool nuevoPin) {
  editandoPinNuevo = nuevoPin;
  posicionPin = 0;
  if (nuevoPin) {
    pinEdicion[0] = pinEdicion[1] = pinEdicion[2] = pinEdicion[3] = 0;
  } else {
    pinADigitos(pinGuardado, pinEdicion);
  }
  gameState = STATE_CONFIG_PIN_EDIT;
}

void drawEditorPin() {
  prepararPantallaPersonalizacion(editandoPinNuevo ? "NUEVO PIN" : "PIN");

  canvas.fillRoundRect(23, 58, SCR_W - 46, 139, 9, 0x2104);
  canvas.drawRoundRect(23, 58, SCR_W - 46, 139, 9, TFT_DARKGREY);

  canvas.setTextDatum(MC_DATUM);
  canvas.setTextColor(TFT_LIGHTGREY);
  canvas.drawString("Elige cuatro digitos", SCR_W / 2, 75, 1);

  dibujarDigitosPin(canvas, 127, false);

  canvas.setTextColor(TFT_LIGHTGREY);
  canvas.drawString("Arriba/Abajo: numero", SCR_W / 2, 169, 1);
  canvas.drawString("Izq/Der: mover  A: guardar", SCR_W / 2, 185, 1);

  canvas.fillRect(0, SCR_H - 25, SCR_W, 25, TFT_BLACK);
  canvas.drawString("MENU: cancelar", SCR_W / 2, SCR_H - 7, 1);
  canvas.setTextDatum(TL_DATUM);
  perfilPushSprite(0, 0);
}

void actualizarMenuPin(bool btnPressed) {
  static Direccion anterior = DIR_NINGUNO;
  static bool pausaAnterior = false;
  const Direccion dir = clasificarCruceta(leerCrucetaADC());

  if (dir != anterior) {
    anterior = dir;
    if (dir == DIR_ARRIBA || dir == DIR_ABAJO) {
      opcionMenuPin = 1 - opcionMenuPin;
      beepMenuMove();
      drawMenuPin();
    }
  }

  const bool pausa = digitalRead(PIN_PAUSA) == LOW;
  if (pausa && !pausaAnterior) {
    esperarSoltarMenu();
    gameState = STATE_CONFIG_MAIN;
    drawPersonalizacion();
  }
  pausaAnterior = pausa;
  if (gameState != STATE_CONFIG_PIN) return;

  if (!btnPressed) return;

  if (opcionMenuPin == 0) {
    if (!bloqueoPinActivo && pinGuardado == 0) {
      comenzarEdicionPin(true);
      drawEditorPin();
    } else {
      bloqueoPinActivo = !bloqueoPinActivo;
      guardarConfiguracionPin();
      beepMenuMove();
      drawMenuPin();
    }
  } else {
    comenzarEdicionPin(true);
    drawEditorPin();
  }
}

void actualizarEditorPin(bool btnPressed) {
  static Direccion anterior = DIR_NINGUNO;
  static bool pausaAnterior = false;
  const Direccion dir = clasificarCruceta(leerCrucetaADC());

  if (dir != anterior) {
    anterior = dir;
    bool cambio = false;

    if (dir == DIR_ARRIBA) {
      pinEdicion[posicionPin] = (pinEdicion[posicionPin] + 1) % 10;
      cambio = true;
    } else if (dir == DIR_ABAJO) {
      pinEdicion[posicionPin] = (pinEdicion[posicionPin] + 9) % 10;
      cambio = true;
    } else if (dir == DIR_IZQUIERDA) {
      posicionPin = (posicionPin + 3) % 4;
      cambio = true;
    } else if (dir == DIR_DERECHA) {
      posicionPin = (posicionPin + 1) % 4;
      cambio = true;
    }

    if (cambio) {
      beepMenuMove();
      drawEditorPin();
    }
  }

  const bool pausa = digitalRead(PIN_PAUSA) == LOW;
  if (pausa && !pausaAnterior) {
    esperarSoltarMenu();
    gameState = STATE_CONFIG_PIN;
    drawMenuPin();
  }
  pausaAnterior = pausa;
  if (gameState != STATE_CONFIG_PIN_EDIT) return;

  if (btnPressed) {
    pinGuardado = digitosAPin(pinEdicion);
    bloqueoPinActivo = true;
    guardarConfiguracionPin();
    beepMenuMove();
    gameState = STATE_CONFIG_PIN;
    drawMenuPin();
  }
}

void drawAcercaDe() {
  canvas.fillSprite(TFT_BLACK);
  canvas.setTextWrap(false);
  canvas.drawRoundRect(12, 10, SCR_W - 24, SCR_H - 20, 10, TFT_DARKGREY);
  canvas.setTextDatum(MC_DATUM);

  if (paginaAcerca == 0) {
    canvas.setTextColor(TFT_GREEN);
    canvas.drawString("COCODRILO OS", 160, 45, 2);
    canvas.setTextColor(TFT_WHITE);
    canvas.drawString("Version 1.0", 160, 82, 2);
    canvas.drawString("Creado por", 160, 118, 1);
    canvas.drawString("Isaac \"Coqui\"", 160, 145, 2);
    canvas.drawString("2025-2026", 160, 178, 1);
  } else if (paginaAcerca == 1) {
    canvas.setTextColor(TFT_GREEN);
    canvas.drawString("AGRADECIMIENTOS", 160, 24, 2);
    canvas.setTextColor(TFT_WHITE);

    const char* lineas[] = {
      "Gracias a Coqui,",
      "creador de Cocodrilo OS.",
      "",
      "Por su creatividad,",
      "dedicacion y esfuerzo",
      "al desarrollar esta consola.",
      "",
      "Disenado y creado",
      "por Coqui.",
      "",
      "COCODRILO OS"
    };

    int y = 50;
    for (int i = 0; i < 11; ++i) {
      canvas.drawString(lineas[i], 160, y, 1);
      y += 15;
    }
  } else {
    canvas.setTextColor(TFT_GREEN);
    canvas.drawString("INFORMACION", 160, 40, 2);
    canvas.setTextColor(TFT_WHITE);
    canvas.drawString("ESP32-S3", 160, 83, 2);
    canvas.drawString("Flash 16 MB", 160, 116, 1);
    canvas.drawString("PSRAM 8 MB", 160, 139, 1);
    canvas.drawString("COCODRILO OS", 160, 174, 2);
    canvas.drawString("Build 1.0", 160, 201, 1);
  }

  canvas.setTextDatum(BC_DATUM);
  canvas.setTextColor(TFT_LIGHTGREY);
  canvas.drawString(String("<  ") + String(paginaAcerca + 1) + "/" +
                    String(TOTAL_PAGINAS_ACERCA) + "  >",
                    SCR_W / 2, SCR_H - 7, 1);
  canvas.setTextDatum(TL_DATUM);
  perfilPushSprite(0, 0);
}

void actualizarAcercaDe() {
  static Direccion anterior = DIR_NINGUNO;
  static bool pausaAnterior = false;

  const Direccion dir = clasificarCruceta(leerCrucetaADC());

  if (dir != anterior) {
    anterior = dir;
    if (dir == DIR_IZQUIERDA) {
      paginaAcerca = (paginaAcerca + TOTAL_PAGINAS_ACERCA - 1) %
                     TOTAL_PAGINAS_ACERCA;
      beepMenuMove();
      drawAcercaDe();
    } else if (dir == DIR_DERECHA) {
      paginaAcerca = (paginaAcerca + 1) % TOTAL_PAGINAS_ACERCA;
      beepMenuMove();
      drawAcercaDe();
    }
  }

  const bool pausa = digitalRead(PIN_PAUSA) == LOW;
  if (pausa && !pausaAnterior) {
    esperarSoltarMenu();
    gameState = STATE_CONFIG_MAIN;
    drawPersonalizacion();
  }
  pausaAnterior = pausa;
}

void drawConfirmacionRestablecer() {
  canvas.fillSprite(TFT_BLACK);
  canvas.setTextWrap(false);
  canvas.setTextDatum(MC_DATUM);

  canvas.drawRoundRect(16, 14, SCR_W - 32, SCR_H - 28, 10, TFT_RED);
  canvas.setTextColor(TFT_RED, TFT_BLACK);
  canvas.drawString("RESTABLECER DE FABRICA", SCR_W / 2, 42, 2);

  canvas.setTextColor(TFT_WHITE, TFT_BLACK);
  canvas.drawString("Se borraran:", SCR_W / 2, 78, 1);
  canvas.drawString("Wallpaper, color e iconos", SCR_W / 2, 104, 1);
  canvas.drawString("WiFi, PIN, logros y progreso", SCR_W / 2, 124, 1);

  canvas.setTextColor(TFT_ORANGE, TFT_BLACK);
  canvas.drawString("No se puede deshacer", SCR_W / 2, 153, 2);

  canvas.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  canvas.drawString("Manten A + MENU por 2 segundos", SCR_W / 2, 190, 1);
  canvas.drawString("MENU: cancelar", SCR_W / 2, 220, 1);

  canvas.setTextDatum(TL_DATUM);
  perfilPushSprite(0, 0);
}

void ejecutarRestablecimientoFabrica() {
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);

  preferenciasArcade.begin("arcade", false);
  preferenciasArcade.clear();
  preferenciasArcade.end();

  // El restablecimiento de fabrica tambien borra logros y avisos pendientes.
  sistemaLogros.restablecer();
  sistemaDesbloqueos.restablecer();

  canvas.fillSprite(TFT_BLACK);
  canvas.setTextDatum(MC_DATUM);
  canvas.setTextColor(TFT_GREEN, TFT_BLACK);
  canvas.drawString("COCODRILO OS", SCR_W / 2, 84, 2);
  canvas.setTextColor(TFT_WHITE, TFT_BLACK);
  canvas.drawString("Restablecimiento completado", SCR_W / 2, 122, 2);
  canvas.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  canvas.drawString("Reiniciando...", SCR_W / 2, 158, 1);
  canvas.setTextDatum(TL_DATUM);
  perfilPushSprite(0, 0);

  delay(1200);
  ESP.restart();
}

void actualizarRestablecer() {
  static bool pausaAnterior = false;
  static uint32_t inicioCombinacion = 0;

  const bool pausa = digitalRead(PIN_PAUSA) == LOW;
  const bool botonA = digitalRead(BUTTON_PIN) == LOW;

  if (pausa && botonA) {
    if (inicioCombinacion == 0) inicioCombinacion = millis();

    if (millis() - inicioCombinacion >= 2000UL) {
      ejecutarRestablecimientoFabrica();
    }
  } else {
    inicioCombinacion = 0;
  }

  if (pausa && !pausaAnterior && !botonA) {
    esperarSoltarMenu();
    gameState = STATE_CONFIG_MAIN;
    drawPersonalizacion();
  }
  pausaAnterior = pausa;
}

void esperarCrucetaNeutralPin() {
  const uint32_t inicio = millis();

  while (clasificarCruceta(leerCrucetaADC()) != DIR_NINGUNO) {
    delay(8);

    // Evita quedar atrapado si existe una entrada averiada.
    if (millis() - inicio > 1500UL) break;
  }

  delay(35);
}

bool solicitarPinAlArrancar() {
  if (!bloqueoPinActivo) return true;

  pinEdicion[0] = pinEdicion[1] = pinEdicion[2] = pinEdicion[3] = 0;
  posicionPin = 0;

  // PIN_VOLUMEN_PANEL (GPIO4) funciona como botón B en el launcher.
  // Mantener B + MENU durante 2 segundos permite recuperar el acceso.
  uint32_t inicioRecuperacion = 0;

  while (true) {
    canvas.fillSprite(TFT_BLACK);
    canvas.setTextWrap(false);
    canvas.setTextDatum(MC_DATUM);

    canvas.setTextColor(TFT_GREEN, TFT_BLACK);
    canvas.drawString("COCODRILO OS", SCR_W / 2, 27, 2);

    canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    canvas.drawString("INGRESA EL PIN", SCR_W / 2, 57, 2);

    // Se muestran los números para que sea fácil editarlos.
    dibujarDigitosPin(canvas, 120, false);

    canvas.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    canvas.drawString("Arriba/Abajo: numero", SCR_W / 2, 166, 1);
    canvas.drawString("Izq/Der: mover  A: desbloquear", SCR_W / 2, 186, 1);

    canvas.setTextColor(TFT_DARKGREY, TFT_BLACK);
    canvas.drawString("B + MENU 2s: omitir bloqueo", SCR_W / 2, 216, 1);

    canvas.setTextDatum(TL_DATUM);
    perfilPushSprite(0, 0);

    bool aAnterior = digitalRead(BUTTON_PIN) == LOW;

    while (true) {
      // Recuperación deliberadamente sencilla: es una consola, no una caja fuerte.
      const bool botonB = digitalRead(PIN_VOLUMEN_PANEL) == LOW;
      const bool menu = digitalRead(PIN_PAUSA) == LOW;

      if (botonB && menu) {
        if (inicioRecuperacion == 0) inicioRecuperacion = millis();

        if (millis() - inicioRecuperacion >= 2000UL) {
          canvas.fillSprite(TFT_BLACK);
          canvas.setTextDatum(MC_DATUM);
          canvas.setTextColor(TFT_ORANGE, TFT_BLACK);
          canvas.drawString("BLOQUEO OMITIDO", SCR_W / 2, SCR_H / 2 - 8, 2);
          canvas.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
          canvas.drawString("Acceso de recuperacion", SCR_W / 2, SCR_H / 2 + 20, 1);
          canvas.setTextDatum(TL_DATUM);
          perfilPushSprite(0, 0);

          while (digitalRead(PIN_VOLUMEN_PANEL) == LOW ||
                 digitalRead(PIN_PAUSA) == LOW) {
            delay(5);
          }

          delay(450);
          return true;
        }
      } else {
        inicioRecuperacion = 0;
      }

      const Direccion dir = clasificarCruceta(leerCrucetaADC());
      bool cambio = false;

      if (dir == DIR_ARRIBA) {
        pinEdicion[posicionPin] = (pinEdicion[posicionPin] + 1) % 10;
        cambio = true;
      } else if (dir == DIR_ABAJO) {
        pinEdicion[posicionPin] = (pinEdicion[posicionPin] + 9) % 10;
        cambio = true;
      } else if (dir == DIR_IZQUIERDA) {
        posicionPin = (posicionPin + 3) % 4;
        cambio = true;
      } else if (dir == DIR_DERECHA) {
        posicionPin = (posicionPin + 1) % 4;
        cambio = true;
      }

      if (cambio) {
        beepMenuMove();
        esperarCrucetaNeutralPin();
        break; // redibuja una sola vez por pulsación física
      }

      const bool a = digitalRead(BUTTON_PIN) == LOW;
      if (a && !aAnterior) {
        while (digitalRead(BUTTON_PIN) == LOW) delay(5);

        if (digitosAPin(pinEdicion) == pinGuardado) {
          canvas.fillSprite(TFT_BLACK);
          canvas.setTextDatum(MC_DATUM);
          canvas.setTextColor(TFT_GREEN, TFT_BLACK);
          canvas.drawString("PIN CORRECTO", SCR_W / 2, SCR_H / 2, 2);
          canvas.setTextDatum(TL_DATUM);
          perfilPushSprite(0, 0);
          delay(500);
          return true;
        }

        canvas.fillSprite(TFT_BLACK);
        canvas.setTextDatum(MC_DATUM);
        canvas.setTextColor(TFT_RED, TFT_BLACK);
        canvas.drawString("PIN INCORRECTO", SCR_W / 2, SCR_H / 2, 2);
        canvas.setTextDatum(TL_DATUM);
        perfilPushSprite(0, 0);
        delay(700);

        pinEdicion[0] = pinEdicion[1] = pinEdicion[2] = pinEdicion[3] = 0;
        posicionPin = 0;
        break;
      }

      aAnterior = a;
      delay(8);
    }
  }
}

void dibujarIconoPersonalizacion(int indice, int x, int y, uint16_t color) {
  switch (indice) {
    case 0:
      canvas.drawRect(x - 7, y - 6, 14, 12, color);
      canvas.fillCircle(x + 3, y - 2, 2, color);
      canvas.drawLine(x - 5, y + 4, x - 1, y, color);
      canvas.drawLine(x - 1, y, x + 4, y + 4, color);
      break;
    case 1:
      canvas.fillCircle(x - 3, y, 3, color);
      canvas.drawCircle(x + 4, y, 4, color);
      break;
    case 2:
      canvas.drawRoundRect(x - 7, y - 7, 14, 14, 2, color);
      canvas.fillRect(x - 4, y - 4, 3, 3, color);
      canvas.fillRect(x + 2, y - 4, 3, 3, color);
      canvas.fillRect(x - 4, y + 2, 3, 3, color);
      canvas.fillRect(x + 2, y + 2, 3, 3, color);
      break;
    case 3:
      canvas.drawArc(x, y + 4, 10, 8, 220, 320, color, TFT_BLACK);
      canvas.drawArc(x, y + 4, 6, 4, 220, 320, color, TFT_BLACK);
      canvas.fillCircle(x, y + 4, 2, color);
      break;
    case 4:
      canvas.drawRoundRect(x - 6, y - 1, 12, 9, 2, color);
      canvas.drawArc(x, y, 5, 4, 180, 360, color, TFT_BLACK);
      break;
    case 5:
      canvas.drawRect(x - 7, y - 6, 14, 12, color);
      canvas.fillTriangle(x - 2, y - 4, x - 2, y + 4, x + 5, y, color);
      break;
    case 6:
      canvas.drawCircle(x, y, 7, color);
      canvas.fillCircle(x, y - 3, 1, color);
      canvas.drawFastVLine(x, y, 5, color);
      break;
    case 7:
      // Tarjeta SD estilizada.
      canvas.drawRoundRect(x - 7, y - 8, 14, 16, 2, color);
      canvas.fillTriangle(x + 2, y - 8, x + 7, y - 8, x + 7, y - 3, color);
      canvas.drawFastHLine(x - 4, y + 3, 8, color);
      canvas.drawFastHLine(x - 4, y + 6, 8, color);
      break;
    case 8:
      // Actualizaciones: flecha de descarga dentro de un cuadro.
      canvas.drawRoundRect(x - 8, y - 7, 16, 14, 3, color);
      canvas.drawFastVLine(x, y - 5, 7, color);
      canvas.fillTriangle(x - 4, y, x + 4, y, x, y + 5, color);
      canvas.drawFastHLine(x - 5, y + 5, 10, color);
      break;
    case 9:
      // Brillo: sol.
      canvas.drawCircle(x, y, 4, color);
      canvas.drawFastVLine(x, y - 9, 4, color);
      canvas.drawFastVLine(x, y + 6, 4, color);
      canvas.drawFastHLine(x - 9, y, 4, color);
      canvas.drawFastHLine(x + 6, y, 4, color);
      canvas.drawLine(x - 7, y - 7, x - 4, y - 4, color);
      canvas.drawLine(x + 4, y + 4, x + 7, y + 7, color);
      canvas.drawLine(x + 4, y - 4, x + 7, y - 7, color);
      canvas.drawLine(x - 7, y + 7, x - 4, y + 4, color);
      break;
  }
}
void ajustarScrollPersonalizacion() {
  if (opcionPersonalizacion < primeraOpcionPersonalizacionVisible) {
    primeraOpcionPersonalizacionVisible = opcionPersonalizacion;
  }

  const int ultimaVisible =
      primeraOpcionPersonalizacionVisible + OPCIONES_PERSONALIZACION_VISIBLES - 1;

  if (opcionPersonalizacion > ultimaVisible) {
    primeraOpcionPersonalizacionVisible =
        opcionPersonalizacion - OPCIONES_PERSONALIZACION_VISIBLES + 1;
  }

  const int maxPrimera =
      TOTAL_OPCIONES_PERSONALIZACION - OPCIONES_PERSONALIZACION_VISIBLES;

  primeraOpcionPersonalizacionVisible =
      constrain(primeraOpcionPersonalizacionVisible, 0, maxPrimera);
}

void redibujarOpcionesPersonalizacion() {
  static const char* opciones[] = {
    "Wallpaper",
    "Color del cursor",
    "Iconos",
    "WiFi",
    "Tarjeta SD",
    "Contrasena",
    "Restablecer",
    "Acerca de",
    "Actualizaciones",
    "Brillo"
  };

  ajustarScrollPersonalizacion();

  // Ventana desplazable; encabezado y barra inferior permanecen fijos.
  canvas.fillRoundRect(24, 43, SCR_W - 48, 178, 9, 0x2104);
  canvas.drawRoundRect(24, 43, SCR_W - 48, 178, 9, TFT_DARKGREY);

  const int inicioY = 62;
  const int pasoY = 22;

  for (int fila = 0; fila < OPCIONES_PERSONALIZACION_VISIBLES; ++fila) {
    const int i = primeraOpcionPersonalizacionVisible + fila;
    if (i >= TOTAL_OPCIONES_PERSONALIZACION) break;

    const int y = inicioY + fila * pasoY;
    const bool seleccionado = i == opcionPersonalizacion;
    const uint16_t color = seleccionado ? colorCursor : TFT_WHITE;

    if (seleccionado) {
      canvas.fillRoundRect(36, y - 9, SCR_W - 72, 19, 6, TFT_BLACK);
      canvas.drawRoundRect(36, y - 9, SCR_W - 72, 19, 6, colorCursor);
    }

    dibujarIconoPersonalizacion(i, 56, y, color);
    canvas.setTextDatum(ML_DATUM);
    canvas.setTextColor(color, seleccionado ? TFT_BLACK : 0x2104);
    canvas.drawString(opciones[i], 76, y, 1);

  }

  // Indicadores de que hay más contenido fuera de la ventana.
  canvas.setTextDatum(MC_DATUM);
  canvas.setTextColor(colorCursor, 0x2104);

  if (primeraOpcionPersonalizacionVisible > 0) {
    canvas.fillTriangle(SCR_W - 42, 51,
                        SCR_W - 48, 58,
                        SCR_W - 36, 58,
                        colorCursor);
  }

  if (primeraOpcionPersonalizacionVisible +
          OPCIONES_PERSONALIZACION_VISIBLES <
      TOTAL_OPCIONES_PERSONALIZACION) {
    canvas.fillTriangle(SCR_W - 42, 211,
                        SCR_W - 48, 204,
                        SCR_W - 36, 204,
                        colorCursor);
  }

  canvas.fillRect(0, SCR_H - 18, SCR_W, 18, TFT_BLACK);
  canvas.setTextDatum(BC_DATUM);
  canvas.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  canvas.drawString("Arriba/Abajo  A: entrar  MENU: salir",
                    SCR_W / 2, SCR_H - 3, 1);
  canvas.setTextDatum(TL_DATUM);
}

void drawPersonalizacion() {
  prepararPantallaPersonalizacion("PERSONALIZACION");
  redibujarOpcionesPersonalizacion();
  perfilPushSprite(0, 0);
}

void entrarPersonalizacion() {
  opcionPersonalizacion = 0;
  primeraOpcionPersonalizacionVisible = 0;
  gameState = STATE_CONFIG_MAIN;
  drawPersonalizacion();
}

void actualizarPersonalizacion(bool btnPressed) {
  static Direccion anterior = DIR_NINGUNO;
  static bool pausaAnterior = false;
  Direccion dir = clasificarCruceta(leerCrucetaADC());

  if (dir != anterior) {
    anterior = dir;
    if (dir == DIR_ARRIBA || dir == DIR_ABAJO) {
      if (dir == DIR_ARRIBA) {
        opcionPersonalizacion =
            (opcionPersonalizacion + TOTAL_OPCIONES_PERSONALIZACION - 1) %
            TOTAL_OPCIONES_PERSONALIZACION;
      } else {
        opcionPersonalizacion =
            (opcionPersonalizacion + 1) %
            TOTAL_OPCIONES_PERSONALIZACION;
      }
      beepMenuMove();
      redibujarOpcionesPersonalizacion();
      perfilPushSprite(0, 0);
    }
  }

  bool pausa = (digitalRead(PIN_PAUSA) == LOW);
  if (pausa && !pausaAnterior) {
    gameState = STATE_MENU;
    invalidarMenuBase();
    drawMenu();
  }
  pausaAnterior = pausa;
  if (gameState != STATE_CONFIG_MAIN) return;

  if (btnPressed) {
    beepMenuMove();
    if (opcionPersonalizacion == 0) {
      entrarSelectorWallpaper();
    } else if (opcionPersonalizacion == 1) {
      gameState = STATE_CONFIG_CURSOR;
      drawSelectorCursor();
    } else if (opcionPersonalizacion == 2) {
      appIconoSeleccionada = 0;
      gameState = STATE_CONFIG_ICON_APP;
      drawSelectorAppIcono();
    } else if (opcionPersonalizacion == 3) {
      entrarMenuWiFi();
    } else if (opcionPersonalizacion == 4) {
      opcionMenuSD = 0;
      pantallaResultadoSD = false;
      esperarSoltarMenu();
      gameState = STATE_CONFIG_SD;
      drawMenuSD();
    } else if (opcionPersonalizacion == 5) {
      cargarConfiguracionPin();
      opcionMenuPin = 0;
      gameState = STATE_CONFIG_PIN;
      drawMenuPin();
    } else if (opcionPersonalizacion == 6) {
      gameState = STATE_CONFIG_RESET;
      drawConfirmacionRestablecer();
    } else if (opcionPersonalizacion == 7) {
      paginaAcerca = 0;
      gameState = STATE_CONFIG_ACERCA;
      drawAcercaDe();
    } else if (opcionPersonalizacion == 8) {
      entrarActualizaciones();
    } else if (opcionPersonalizacion == 9) {
      gameState = STATE_CONFIG_BRILLO;
      drawMenuBrillo();
    }
  }
}


void drawMenuBrillo() {
  prepararPantallaPersonalizacion("BRILLO");

  canvas.fillRoundRect(30, 60, SCR_W - 60, 137, 10, 0x2104);
  canvas.drawRoundRect(30, 60, SCR_W - 60, 137, 10, TFT_DARKGREY);

  canvas.setTextDatum(MC_DATUM);
  canvas.setTextColor(TFT_WHITE, 0x2104);
  canvas.drawString("Brillo de pantalla", SCR_W / 2, 82, 2);

  canvas.setTextColor(colorCursor, 0x2104);
  canvas.drawString(String(BacklightControl::actual()) + " / 10", SCR_W / 2, 113, 4);

  const int bx = 58;
  const int by = 146;
  const int bw = 204;
  const int bh = 18;
  canvas.drawRoundRect(bx, by, bw, bh, 5, TFT_LIGHTGREY);
  const int relleno = map(BacklightControl::actual(), 1, BacklightControl::NIVEL_MAX, 16, bw - 4);
  canvas.fillRoundRect(bx + 2, by + 2, relleno, bh - 4, 4, colorCursor);

  canvas.setTextColor(TFT_LIGHTGREY, 0x2104);
  canvas.drawString("Izquierda / Derecha: ajustar", SCR_W / 2, 180, 1);

  canvas.fillRect(0, SCR_H - 25, SCR_W, 25, TFT_BLACK);
  canvas.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  canvas.drawString("MENU: volver", SCR_W / 2, SCR_H - 7, 1);
  canvas.setTextDatum(TL_DATUM);
  perfilPushSprite(0, 0);
}

void actualizarMenuBrillo() {
  static Direccion anterior = DIR_NINGUNO;
  static bool pausaAnterior = false;

  const Direccion dir = clasificarCruceta(leerCrucetaADC());
  if (dir != anterior) {
    anterior = dir;
    int nuevo = BacklightControl::actual();

    if (dir == DIR_IZQUIERDA || dir == DIR_ABAJO) nuevo--;
    if (dir == DIR_DERECHA || dir == DIR_ARRIBA) nuevo++;

    nuevo = constrain(nuevo, 1, BacklightControl::NIVEL_MAX);
    if (nuevo != BacklightControl::actual()) {
      BacklightControl::set(nuevo);
      beepMenuMove();
      drawMenuBrillo();
    }
  }

  const bool pausa = digitalRead(PIN_PAUSA) == LOW;
  if (pausa && !pausaAnterior) {
    esperarSoltarMenu();
    gameState = STATE_CONFIG_MAIN;
    drawPersonalizacion();
  }
  pausaAnterior = pausa;
}

static constexpr const char* URL_PAGINA_ACTUALIZACIONES =
    "https://backmeteor4090quintana.github.io/arcade-console-updates/";

void drawActualizaciones() {
  prepararPantallaPersonalizacion("ACTUALIZACIONES");

  const AvisoActualizacionWiFi::Aviso& estado =
      AvisoActualizacionWiFi::obtener();

  // Panel izquierdo: estado de la consola.
  canvas.fillRoundRect(18, 52, 145, 151, 9, 0x2104);
  canvas.drawRoundRect(18, 52, 145, 151, 9, TFT_DARKGREY);

  canvas.setTextDatum(MC_DATUM);

  if (WiFi.status() != WL_CONNECTED) {
    canvas.setTextColor(TFT_ORANGE, 0x2104);
    canvas.drawString("SIN WIFI", 90, 72, 2);

    canvas.setTextColor(TFT_WHITE, 0x2104);
    canvas.drawString("Version instalada", 90, 101, 1);
    canvas.setTextColor(colorCursor, 0x2104);
    canvas.drawString(versionFirmwareActual(), 90, 119, 2);

    canvas.setTextColor(TFT_LIGHTGREY, 0x2104);
    canvas.drawString("Conecta WiFi para", 90, 149, 1);
    canvas.drawString("buscar versiones", 90, 164, 1);
  } else if (!estado.consultaRealizada) {
    canvas.setTextColor(TFT_ORANGE, 0x2104);
    canvas.drawString("SIN RESPUESTA", 90, 72, 2);

    canvas.setTextColor(TFT_WHITE, 0x2104);
    canvas.drawString("Version instalada", 90, 101, 1);
    canvas.setTextColor(colorCursor, 0x2104);
    canvas.drawString(versionFirmwareActual(), 90, 119, 2);

    canvas.setTextColor(TFT_LIGHTGREY, 0x2104);
    String err = estado.error;
    if (err.length() > 20) err = err.substring(0, 20);
    canvas.drawString(err.length() ? err : "Intenta otra vez", 90, 154, 1);
  } else if (estado.disponible && estado.versionNueva) {
    canvas.setTextColor(TFT_YELLOW, 0x2104);
    canvas.drawString("NUEVA VERSION", 90, 72, 2);

    canvas.setTextColor(TFT_WHITE, 0x2104);
    canvas.drawString("Instalada", 90, 100, 1);
    canvas.setTextColor(TFT_LIGHTGREY, 0x2104);
    canvas.drawString(versionFirmwareActual(), 90, 116, 1);

    canvas.setTextColor(TFT_WHITE, 0x2104);
    canvas.drawString("Disponible", 90, 139, 1);
    canvas.setTextColor(colorCursor, 0x2104);
    canvas.drawString(estado.version, 90, 158, 2);

    canvas.setTextColor(TFT_LIGHTGREY, 0x2104);
    canvas.drawString("Instala desde SD", 90, 184, 1);
  } else {
    canvas.setTextColor(TFT_GREEN, 0x2104);
    canvas.drawString("ACTUALIZADA", 90, 72, 2);

    canvas.setTextColor(TFT_WHITE, 0x2104);
    canvas.drawString("Version instalada", 90, 103, 1);
    canvas.setTextColor(colorCursor, 0x2104);
    canvas.drawString(versionFirmwareActual(), 90, 123, 2);

    if (estado.version.length() > 0) {
      canvas.setTextColor(TFT_LIGHTGREY, 0x2104);
      canvas.drawString(String("Publicada: ") + estado.version, 90, 153, 1);
    }

    canvas.setTextColor(TFT_GREEN, 0x2104);
    canvas.drawString("Estas al dia", 90, 181, 1);
  }

  // Panel derecho: QR permanente. Aparece SIEMPRE.
  canvas.fillRoundRect(169, 52, 133, 151, 9, 0x2104);
  canvas.drawRoundRect(169, 52, 133, 151, 9, colorCursor);

  QR::dibujarActualizaciones(canvas, 235, 119, 118);

  canvas.setTextColor(TFT_WHITE, 0x2104);
  canvas.drawString("Escanea para", 235, 183, 1);
  canvas.drawString("descargas", 235, 195, 1);

  canvas.fillRect(0, SCR_H - 27, SCR_W, 27, TFT_BLACK);
  canvas.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  canvas.drawString("A: buscar otra vez   MENU: volver",
                    SCR_W / 2, SCR_H - 9, 1);

  canvas.setTextDatum(TL_DATUM);
  perfilPushSprite(0, 0);
}

void entrarActualizaciones() {
  // El QR no depende de Internet; la consulta sí.
  if (WiFi.status() == WL_CONNECTED) {
    AvisoActualizacionWiFi::consultar(ActualizacionSD::VERSION_ACTUAL);
  }

  gameState = STATE_CONFIG_UPDATES;
  drawActualizaciones();
}

void actualizarActualizaciones(bool btnPressed) {
  static bool menuAnterior = false;
  const bool menuAhora = digitalRead(PIN_PAUSA) == LOW;

  if (menuAhora && !menuAnterior) {
    esperarSoltarMenu();
    gameState = STATE_CONFIG_MAIN;
    drawPersonalizacion();
    menuAnterior = false;
    return;
  }
  menuAnterior = menuAhora;

  if (!btnPressed) return;

  beepMenuMove();

  canvas.fillRoundRect(18, 52, 145, 151, 9, 0x2104);
  canvas.drawRoundRect(18, 52, 145, 151, 9, colorCursor);
  canvas.setTextDatum(MC_DATUM);
  canvas.setTextColor(colorCursor, 0x2104);
  canvas.drawString("BUSCANDO...", 90, 117, 2);
  canvas.setTextDatum(TL_DATUM);
  perfilPushSprite(0, 0);

  if (WiFi.status() == WL_CONNECTED) {
    AvisoActualizacionWiFi::consultar(ActualizacionSD::VERSION_ACTUAL);
  }

  drawActualizaciones();
}

String formatearBytesSD(uint64_t bytes) {
  if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
    return String(bytes / (1024.0 * 1024.0 * 1024.0), 2) + " GB";
  }
  if (bytes >= 1024ULL * 1024ULL) {
    return String(bytes / (1024.0 * 1024.0), 1) + " MB";
  }
  return String(bytes / 1024.0, 1) + " KB";
}

void drawResultadoSD() {
  prepararPantallaPersonalizacion("TARJETA SD");
  canvas.fillRoundRect(22, 55, SCR_W - 44, 132, 10, 0x2104);
  canvas.drawRoundRect(22, 55, SCR_W - 44, 132, 10, colorCursor);
  canvas.setTextDatum(MC_DATUM);
  canvas.setTextColor(colorCursor, 0x2104);
  canvas.drawString(tituloResultadoSD, SCR_W / 2, 88, 2);
  canvas.setTextColor(TFT_WHITE, 0x2104);
  canvas.drawString(detalleResultadoSD, SCR_W / 2, 124, 1);
  canvas.setTextColor(TFT_LIGHTGREY, 0x2104);
  canvas.drawString("A o MENU: volver", SCR_W / 2, 166, 1);
  canvas.setTextDatum(TL_DATUM);
  perfilPushSprite(0, 0);
}

void drawMenuSD() {
  prepararPantallaPersonalizacion("TARJETA SD");

  const GestionSD::Informacion info = GestionSD::obtenerInformacion();
  canvas.fillRoundRect(22, 43, SCR_W - 44, 72, 9, 0x2104);
  canvas.drawRoundRect(22, 43, SCR_W - 44, 72, 9, TFT_DARKGREY);
  canvas.setTextDatum(ML_DATUM);
  canvas.setTextColor(info.montada ? TFT_GREEN : TFT_ORANGE, 0x2104);
  canvas.drawString(info.montada ? "Estado: conectada" : "Estado: no disponible", 36, 59, 1);

  if (info.montada) {
    canvas.setTextColor(TFT_WHITE, 0x2104);
    canvas.drawString(String("Tipo: ") + GestionSD::nombreTipo(info.tipo), 36, 78, 1);
    canvas.drawString(String("Libre: ") + formatearBytesSD(info.libre) +
                      " / " + formatearBytesSD(info.capacidad), 36, 97, 1);
  } else {
    canvas.setTextColor(TFT_LIGHTGREY, 0x2104);
    canvas.drawString("Reinicia tras volver a insertarla", 36, 84, 1);
  }

  static const char* opciones[] = {
    "Informacion",
    "Buscar actualizacion",
    "Expulsar de forma segura",
    "Preparar carpetas"
  };

  for (int i = 0; i < 4; ++i) {
    const int y = 127 + i * 22;
    const bool seleccionada = i == opcionMenuSD;
    const uint16_t color = seleccionada ? colorCursor : TFT_WHITE;
    if (seleccionada) {
      canvas.fillRoundRect(30, y - 9, SCR_W - 60, 19, 6, TFT_BLACK);
      canvas.drawRoundRect(30, y - 9, SCR_W - 60, 19, 6, colorCursor);
    }
    canvas.setTextDatum(MC_DATUM);
    canvas.setTextColor(color, seleccionada ? TFT_BLACK : 0x2104);
    canvas.drawString(opciones[i], SCR_W / 2, y, 1);
  }

  canvas.fillRect(0, SCR_H - 18, SCR_W, 18, TFT_BLACK);
  canvas.setTextDatum(BC_DATUM);
  canvas.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  canvas.drawString("Arriba/Abajo  A: elegir  MENU: volver", SCR_W / 2, SCR_H - 3, 1);
  canvas.setTextDatum(TL_DATUM);
  perfilPushSprite(0, 0);
}


String versionFirmwareActual() {
  return String(ActualizacionSD::VERSION_ACTUAL);
}

void drawConfirmacionActualizacion() {
  prepararPantallaPersonalizacion("ACTUALIZACION");

  canvas.fillRoundRect(18, 40, SCR_W - 36, 164, 10, 0x2104);
  canvas.drawRoundRect(18, 40, SCR_W - 36, 164, 10, colorCursor);

  canvas.setTextDatum(MC_DATUM);
  canvas.setTextColor(colorCursor, 0x2104);
  canvas.drawString(paqueteActualizacion.nombre, SCR_W / 2, 60, 2);

  canvas.setTextColor(TFT_WHITE, 0x2104);
  canvas.drawString(String("Instalada: ") + versionFirmwareActual(),
                    SCR_W / 2, 88, 1);
  canvas.drawString(String("Nueva: ") + paqueteActualizacion.version,
                    SCR_W / 2, 106, 1);
  canvas.drawString(String("Tamano: ") +
                    formatearBytesSD(paqueteActualizacion.tamano),
                    SCR_W / 2, 124, 1);

  if (paqueteActualizacion.notas.length() > 0) {
    String notas = paqueteActualizacion.notas;
    if (notas.length() > 42) notas = notas.substring(0, 39) + "...";
    canvas.setTextColor(TFT_LIGHTGREY, 0x2104);
    canvas.drawString(notas, SCR_W / 2, 148, 1);
  }

  canvas.setTextColor(TFT_YELLOW, 0x2104);
  canvas.drawString("No apagues ni retires la SD", SCR_W / 2, 172, 1);

  canvas.setTextColor(TFT_GREEN, 0x2104);
  canvas.drawString("A: instalar", 100, 191, 1);
  canvas.setTextColor(TFT_LIGHTGREY, 0x2104);
  canvas.drawString("MENU: cancelar", 226, 191, 1);

  canvas.setTextDatum(TL_DATUM);
  perfilPushSprite(0, 0);
}

void drawProgresoActualizacion(size_t escritos, size_t total) {
  prepararPantallaPersonalizacion("ACTUALIZANDO");

  const int porcentaje = total > 0
      ? constrain((int)((escritos * 100ULL) / total), 0, 100)
      : 0;

  canvas.setTextDatum(MC_DATUM);
  canvas.setTextColor(TFT_WHITE, TFT_BLACK);
  canvas.drawString("Instalando firmware", SCR_W / 2, 78, 2);

  const int barraX = 28;
  const int barraY = 112;
  const int barraW = SCR_W - 56;
  const int barraH = 24;

  canvas.drawRoundRect(barraX, barraY, barraW, barraH, 6, TFT_WHITE);
  const int relleno = map(porcentaje, 0, 100, 0, barraW - 4);
  if (relleno > 0) {
    canvas.fillRoundRect(barraX + 2, barraY + 2,
                         relleno, barraH - 4, 4, colorCursor);
  }

  canvas.setTextColor(colorCursor, TFT_BLACK);
  canvas.drawString(String(porcentaje) + "%", SCR_W / 2, 157, 2);
  canvas.setTextColor(TFT_YELLOW, TFT_BLACK);
  canvas.drawString("NO APAGAR LA CONSOLA", SCR_W / 2, 190, 1);
  canvas.setTextDatum(TL_DATUM);
  perfilPushSprite(0, 0);
}

void callbackProgresoActualizacion(size_t escritos, size_t total) {
  progresoActualizacionEscrito = escritos;
  progresoActualizacionTotal = total;

  static int ultimoPorcentaje = -1;
  const int porcentaje = total > 0
      ? constrain((int)((escritos * 100ULL) / total), 0, 100)
      : 0;

  if (porcentaje != ultimoPorcentaje) {
    ultimoPorcentaje = porcentaje;
    drawProgresoActualizacion(escritos, total);
  }
}

bool confirmarInstalacionActualizacion() {
  drawConfirmacionActualizacion();

  while (digitalRead(BUTTON_PIN) == LOW || digitalRead(PIN_PAUSA) == LOW) {
    delay(5);
  }
  delay(30);

  bool aPrevio = false;
  bool menuPrevio = false;

  while (true) {
    const bool aAhora = digitalRead(BUTTON_PIN) == LOW;
    const bool menuAhora = digitalRead(PIN_PAUSA) == LOW;

    if (aAhora && !aPrevio) {
      while (digitalRead(BUTTON_PIN) == LOW) delay(5);
      delay(25);
      return true;
    }

    if (menuAhora && !menuPrevio) {
      esperarSoltarMenu();
      return false;
    }

    aPrevio = aAhora;
    menuPrevio = menuAhora;
    delay(8);
  }
}

void ejecutarBusquedaActualizacion() {
  paqueteActualizacion = ActualizacionSD::buscar();

  if (!paqueteActualizacion.sdDisponible) {
    pantallaResultadoSD = true;
    tituloResultadoSD = "SD NO DISPONIBLE";
    detalleResultadoSD = "Inserta la tarjeta y reinicia";
    drawResultadoSD();
    return;
  }

  if (!paqueteActualizacion.firmwareEncontrado ||
      !paqueteActualizacion.firmwareValido) {
    pantallaResultadoSD = true;
    tituloResultadoSD = "SIN ACTUALIZACION";
    detalleResultadoSD = paqueteActualizacion.error;
    drawResultadoSD();
    return;
  }

  if (ActualizacionSD::mismaVersion(paqueteActualizacion)) {
    pantallaResultadoSD = true;
    tituloResultadoSD = "VERSION INSTALADA";
    detalleResultadoSD = String("Ya tienes la version ") +
                         ActualizacionSD::VERSION_ACTUAL;
    drawResultadoSD();
    return;
  }

  // Sin detector de cargador: se exige al menos 30% para actualizar.
  if (Bateria::medicionLista() &&
      Bateria::getPorcentaje() < 30) {
    pantallaResultadoSD = true;
    tituloResultadoSD = "BATERIA INSUFICIENTE";
    detalleResultadoSD = "Carga la consola antes de actualizar";
    drawResultadoSD();
    return;
  }

  if (!confirmarInstalacionActualizacion()) {
    drawMenuSD();
    return;
  }

  progresoActualizacionEscrito = 0;
  progresoActualizacionTotal = paqueteActualizacion.tamano;
  drawProgresoActualizacion(0, paqueteActualizacion.tamano);

  const ActualizacionSD::ResultadoInstalacion resultado =
      ActualizacionSD::instalar(
          paqueteActualizacion,
          callbackProgresoActualizacion
      );

  if (resultado != ActualizacionSD::INSTALACION_OK) {
    pantallaResultadoSD = true;
    tituloResultadoSD = "ERROR DE ACTUALIZACION";
    detalleResultadoSD = ActualizacionSD::textoError(resultado);
    drawResultadoSD();
    return;
  }

  prepararPantallaPersonalizacion("ACTUALIZACION");
  canvas.setTextDatum(MC_DATUM);
  canvas.setTextColor(TFT_GREEN, TFT_BLACK);
  canvas.drawString("ACTUALIZACION COMPLETA", SCR_W / 2, 92, 2);
  canvas.setTextColor(TFT_WHITE, TFT_BLACK);
  canvas.drawString("La consola se reiniciara", SCR_W / 2, 126, 1);
  canvas.drawString("automaticamente...", SCR_W / 2, 146, 1);
  canvas.setTextDatum(TL_DATUM);
  perfilPushSprite(0, 0);

  delay(1800);
  ESP.restart();
}

void actualizarMenuSD(bool btnPressed) {
  static Direccion anterior = DIR_NINGUNO;
  static bool menuAnterior = false;
  static bool menuArmado = false;

  const bool menuAhora = digitalRead(PIN_PAUSA) == LOW;
  if (!menuArmado) {
    if (!menuAhora) {
      menuArmado = true;
      menuAnterior = false;
    }
  }
  const bool menuPulsado = menuArmado && menuAhora && !menuAnterior;
  menuAnterior = menuAhora;

  if (pantallaResultadoSD) {
    if (menuPulsado || btnPressed) {
      esperarSoltarMenu();
      pantallaResultadoSD = false;
      drawMenuSD();
    }
    return;
  }

  const Direccion dir = clasificarCruceta(leerCrucetaADC());
  if (dir != anterior) {
    anterior = dir;
    if (dir == DIR_ARRIBA) {
      opcionMenuSD = (opcionMenuSD + 3) % 4;
      beepMenuMove();
      drawMenuSD();
    } else if (dir == DIR_ABAJO) {
      opcionMenuSD = (opcionMenuSD + 1) % 4;
      beepMenuMove();
      drawMenuSD();
    }
  }

  if (menuPulsado) {
    esperarSoltarMenu();
    menuArmado = false;
    gameState = STATE_CONFIG_MAIN;
    drawPersonalizacion();
    return;
  }

  if (!btnPressed) return;
  beepMenuMove();

  const GestionSD::Informacion info = GestionSD::obtenerInformacion();
  pantallaResultadoSD = true;

  if (opcionMenuSD == 0) {
    if (info.montada) {
      tituloResultadoSD = "INFORMACION";
      detalleResultadoSD = String("Usado ") + formatearBytesSD(info.usado) +
                           "   Libre " + formatearBytesSD(info.libre);
    } else {
      tituloResultadoSD = "SD NO DISPONIBLE";
      detalleResultadoSD = "Inserta la tarjeta y reinicia";
    }
  } else if (opcionMenuSD == 1) {
    pantallaResultadoSD = false;
    ejecutarBusquedaActualizacion();
    return;
  } else if (opcionMenuSD == 2) {
    if (!info.montada) {
      tituloResultadoSD = "SD NO DISPONIBLE";
      detalleResultadoSD = "La tarjeta ya esta desconectada";
    } else {
      GestionSD::expulsar();
      sdDetectadaArranque = false;
      tituloResultadoSD = "SD DESCONECTADA";
      detalleResultadoSD = "Ya puedes retirarla con seguridad";
    }
  } else {
    if (GestionSD::prepararCarpetas()) {
      tituloResultadoSD = "CARPETAS LISTAS";
      detalleResultadoSD = "video, images, wallpapers y update";
    } else {
      tituloResultadoSD = "NO SE PUDO PREPARAR";
      detalleResultadoSD = "Comprueba que la SD este conectada";
    }
  }

  drawResultadoSD();
}


const char* nombreIconoElegido(int eleccion) {
  if (eleccion <= 0) return "Original";
  const int idx = eleccion - 1;
  if (idx >= 0 && idx < CANT_ICONOS_ALTERNATIVOS) {
    return NOMBRES_ICONOS_ALTERNATIVOS[idx];
  }
  return "Original";
}

void drawSelectorAppIcono() {
  prepararPantallaPersonalizacion("ICONOS");

  canvas.fillRoundRect(29, 58, SCR_W - 58, 139, 9, 0x2104);
  canvas.drawRoundRect(29, 58, SCR_W - 58, 139, 9, TFT_DARKGREY);

  canvas.setTextDatum(MC_DATUM);
  canvas.setTextColor(TFT_LIGHTGREY);
  canvas.drawString("Elige la aplicacion", SCR_W / 2, 74, 1);

  dibujarIconoJuego(SCR_W / 2 - 24, 91,
                    obtenerIconoApp(appIconoSeleccionada, true));

  canvas.setTextColor(colorCursor);
  canvas.drawString(String("<  ") + nombresJuegos[appIconoSeleccionada] + "  >",
                    SCR_W / 2, 151, 2);

  canvas.setTextColor(TFT_WHITE);
  canvas.drawString(
    String("Actual: ") + nombreIconoElegido(iconoElegidoPorApp[appIconoSeleccionada]),
    SCR_W / 2, 176, 1
  );

  canvas.fillRect(0, SCR_H - 28, SCR_W, 28, TFT_BLACK);
  canvas.setTextColor(TFT_LIGHTGREY);
  canvas.drawString("Izq/Der: app  A: cambiar  MENU: volver",
                    SCR_W / 2, SCR_H - 14, 1);
  canvas.setTextDatum(TL_DATUM);
  perfilPushSprite(0, 0);
}

void actualizarSelectorAppIcono(bool btnPressed) {
  static Direccion anterior = DIR_NINGUNO;
  static bool pausaAnterior = false;
  const Direccion dir = clasificarCruceta(leerCrucetaADC());

  if (dir != anterior) {
    anterior = dir;
    if (dir == DIR_IZQUIERDA) {
      appIconoSeleccionada =
          (appIconoSeleccionada + NUM_JUEGOS - 1) % NUM_JUEGOS;
      beepMenuMove();
      drawSelectorAppIcono();
    } else if (dir == DIR_DERECHA) {
      appIconoSeleccionada =
          (appIconoSeleccionada + 1) % NUM_JUEGOS;
      beepMenuMove();
      drawSelectorAppIcono();
    }
  }

  const bool pausa = digitalRead(PIN_PAUSA) == LOW;
  if (pausa && !pausaAnterior) {
    esperarSoltarMenu();
    gameState = STATE_CONFIG_MAIN;
    drawPersonalizacion();
  }
  pausaAnterior = pausa;
  if (gameState != STATE_CONFIG_ICON_APP) return;

  if (btnPressed) {
    opcionIconoTemporal = iconoElegidoPorApp[appIconoSeleccionada];
    gameState = STATE_CONFIG_ICON_PICK;
    drawSelectorIcono();
  }
}

void drawSelectorIcono() {
  prepararPantallaPersonalizacion(nombresJuegos[appIconoSeleccionada]);

  canvas.fillRoundRect(29, 58, SCR_W - 58, 139, 9, 0x2104);
  canvas.drawRoundRect(29, 58, SCR_W - 58, 139, 9, TFT_DARKGREY);

  const uint16_t* vista;
  if (opcionIconoTemporal == 0) {
    vista = iconosNormal[appIconoSeleccionada];
  } else {
    vista = ICONOS_ALTERNATIVOS[opcionIconoTemporal - 1];
  }

  dibujarIconoJuego(SCR_W / 2 - 24, 78, vista);

  canvas.setTextDatum(MC_DATUM);
  canvas.setTextColor(colorCursor);
  canvas.drawString(
    String("<  ") + nombreIconoElegido(opcionIconoTemporal) + "  >",
    SCR_W / 2, 142, 2
  );

  canvas.setTextColor(TFT_LIGHTGREY);
  canvas.drawString(
    String(opcionIconoTemporal + 1) + "/" +
    String(CANT_ICONOS_ALTERNATIVOS + 1),
    SCR_W / 2, 171, 1
  );

  canvas.fillRect(0, SCR_H - 28, SCR_W, 28, TFT_BLACK);
  canvas.setTextColor(TFT_LIGHTGREY);
  canvas.drawString("Izq/Der: cambiar  A: guardar  MENU: volver",
                    SCR_W / 2, SCR_H - 14, 1);
  canvas.setTextDatum(TL_DATUM);
  perfilPushSprite(0, 0);
}

void actualizarSelectorIcono(bool btnPressed) {
  static Direccion anterior = DIR_NINGUNO;
  static bool pausaAnterior = false;
  const Direccion dir = clasificarCruceta(leerCrucetaADC());
  const int total = CANT_ICONOS_ALTERNATIVOS + 1;

  if (dir != anterior) {
    anterior = dir;
    if (dir == DIR_IZQUIERDA) {
      opcionIconoTemporal = (opcionIconoTemporal + total - 1) % total;
      beepMenuMove();
      drawSelectorIcono();
    } else if (dir == DIR_DERECHA) {
      opcionIconoTemporal = (opcionIconoTemporal + 1) % total;
      beepMenuMove();
      drawSelectorIcono();
    }
  }

  const bool pausa = digitalRead(PIN_PAUSA) == LOW;
  if (pausa && !pausaAnterior) {
    esperarSoltarMenu();
    gameState = STATE_CONFIG_ICON_APP;
    drawSelectorAppIcono();
  }
  pausaAnterior = pausa;
  if (gameState != STATE_CONFIG_ICON_PICK) return;

  if (btnPressed) {
    guardarIconoApp(appIconoSeleccionada, opcionIconoTemporal);
    beepMenuMove();
    gameState = STATE_CONFIG_ICON_APP;
    drawSelectorAppIcono();
  }
}

void redibujarSelectorCursor() {
  canvas.fillRoundRect(38, 69, SCR_W - 76, 111, 9, 0x2104);
  canvas.drawRoundRect(38, 69, SCR_W - 76, 111, 9, TFT_DARKGREY);

  canvas.setTextDatum(MC_DATUM);
  canvas.setTextColor(TFT_WHITE);
  canvas.drawString(String("<   ") + NOMBRES_COLORES_CURSOR[indiceColorCursor] + "   >",
                    SCR_W / 2, 99, 2);

  canvas.fillRoundRect(65, 126, SCR_W - 130, 36, 8, colorCursor);
  canvas.drawRoundRect(65, 126, SCR_W - 130, 36, 8, TFT_WHITE);
  canvas.setTextColor(TFT_BLACK);
  canvas.drawString("CURSOR", SCR_W / 2, 144, 2);

  canvas.fillRect(0, SCR_H - 28, SCR_W, 28, TFT_BLACK);
  canvas.setTextColor(TFT_LIGHTGREY);
  canvas.drawString("Izq/Der: cambiar   A: guardar   MENU: volver",
                    SCR_W / 2, SCR_H - 14, 1);
  canvas.setTextDatum(TL_DATUM);
}

void drawSelectorCursor() {
  prepararPantallaPersonalizacion("COLOR DEL CURSOR");
  redibujarSelectorCursor();
  perfilPushSprite(0, 0);
}

void actualizarSelectorCursor(bool btnPressed) {
  static Direccion anterior = DIR_NINGUNO;
  static bool pausaAnterior = false;
  Direccion dir = clasificarCruceta(leerCrucetaADC());

  if (dir != anterior) {
    anterior = dir;
    if (dir == DIR_IZQUIERDA) {
      indiceColorCursor = (indiceColorCursor + CANT_COLORES_CURSOR - 1) % CANT_COLORES_CURSOR;
      colorCursor = COLORES_CURSOR[indiceColorCursor];
      beepMenuMove();
      redibujarSelectorCursor();
      perfilPushSprite(0, 0);
    } else if (dir == DIR_DERECHA) {
      indiceColorCursor = (indiceColorCursor + 1) % CANT_COLORES_CURSOR;
      colorCursor = COLORES_CURSOR[indiceColorCursor];
      beepMenuMove();
      redibujarSelectorCursor();
      perfilPushSprite(0, 0);
    }
  }

  bool pausa = (digitalRead(PIN_PAUSA) == LOW);
  if (pausa && !pausaAnterior) {
    cargarPersonalizacion(); // descarta el cambio no guardado
    gameState = STATE_CONFIG_MAIN;
    drawPersonalizacion();
  }
  pausaAnterior = pausa;
  if (gameState != STATE_CONFIG_CURSOR) return;

  if (btnPressed) {
    guardarColorCursor();
    beepMenuMove();
    gameState = STATE_CONFIG_MAIN;
    drawPersonalizacion();
  }
}

void drawSelectorWallpaper() {
  canvas.setTextSize(1);
  canvas.setTextFont(1);
  canvas.setTextDatum(TL_DATUM);

  if (cantidadWallpapers <= 0) {
    canvas.fillSprite(TFT_BLACK);
    canvas.setTextWrap(false);
    canvas.setTextDatum(MC_DATUM);

    canvas.drawRoundRect(18, 14, SCR_W - 36, SCR_H - 28, 10, TFT_DARKGREY);
    canvas.drawFastHLine(38, 50, SCR_W - 76, TFT_DARKGREY);

    canvas.setTextColor(TFT_ORANGE, TFT_BLACK);
    canvas.drawString("NO HAY WALLPAPERS JPG", SCR_W / 2, 72, 2);

    canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    canvas.drawString("Copia imagenes JPG en:", SCR_W / 2, 116, 1);

    canvas.setTextColor(TFT_CYAN, TFT_BLACK);
    canvas.drawString("/wallpapers", SCR_W / 2, 142, 2);

    canvas.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    canvas.drawString("de la tarjeta SD", SCR_W / 2, 169, 1);
    canvas.drawString("MENU: volver", SCR_W / 2, 222, 1);
  } else {
    const char *rutaPreview = rutasWallpapers[wallpaperSeleccionado];

    if (!dibujarWallpaperActivo(rutaPreview, canvas)) {
      canvas.fillSprite(TFT_BLACK);
    }

    canvas.fillRoundRect(12, 9, SCR_W - 24, 43, 8, 0x2104);
    canvas.drawRoundRect(12, 9, SCR_W - 24, 43, 8, TFT_WHITE);

    canvas.setTextDatum(MC_DATUM);
    canvas.setTextColor(colorCursor);
    canvas.drawString("WALLPAPER", SCR_W / 2, 23, 2);

    canvas.setTextColor(TFT_WHITE);
    canvas.drawString(
      String(wallpaperSeleccionado + 1) + " / " +
      String(cantidadWallpapers),
      SCR_W / 2, 42, 1
    );

    String nombre = String(rutaPreview);
    int slash = nombre.lastIndexOf('/');
    if (slash >= 0) nombre = nombre.substring(slash + 1);

    canvas.fillRoundRect(18, SCR_H - 53, SCR_W - 36, 39, 7, 0x2104);

    canvas.setTextColor(TFT_WHITE);
    canvas.drawString(nombre, SCR_W / 2, SCR_H - 40, 1);

    canvas.setTextColor(TFT_LIGHTGREY);
    canvas.drawString(
      "Izq/Der: cambiar   A: guardar   MENU: volver",
      SCR_W / 2, SCR_H - 24, 1
    );
  }

  canvas.setTextDatum(TL_DATUM);
  perfilPushSprite(0, 0);
}
void entrarSelectorWallpaper() {
  escanearWallpapers();
  gameState = STATE_CONFIG_WALLPAPER;
  drawSelectorWallpaper();
}

void actualizarSelectorWallpaper(bool btnPressed) {
  static Direccion direccionAnterior = DIR_NINGUNO;
  static bool pausaAnterior = false;

  Direccion direccion = clasificarCruceta(leerCrucetaADC());
  if (direccion != direccionAnterior) {
    direccionAnterior = direccion;
    if (cantidadWallpapers > 0) {
      if (direccion == DIR_IZQUIERDA) {
        wallpaperSeleccionado = (wallpaperSeleccionado + cantidadWallpapers - 1) % cantidadWallpapers;
        beepMenuMove();
        drawSelectorWallpaper();
      } else if (direccion == DIR_DERECHA) {
        wallpaperSeleccionado = (wallpaperSeleccionado + 1) % cantidadWallpapers;
        beepMenuMove();
        drawSelectorWallpaper();
      }
    }
  }

  bool pausaAhora = (digitalRead(PIN_PAUSA) == LOW);
  if (pausaAhora && !pausaAnterior) {
    esperarSoltarMenu();
    gameState = STATE_CONFIG_MAIN;
    drawPersonalizacion();
  }
  pausaAnterior = pausaAhora;
  if (gameState != STATE_CONFIG_WALLPAPER) return;

  if (btnPressed && cantidadWallpapers > 0) {
    if (guardarWallpaperActual(rutasWallpapers[wallpaperSeleccionado])) {
      beepMenuMove();
      gameState = STATE_CONFIG_MAIN;
      drawPersonalizacion();
    } else {
      beepDenegado();
    }
  }
}

void iniciarJuegoSeleccionado() {
  if (!juegoDisponible[menuSeleccion]) { beepDenegado(); return; }

  // Evita que el juego herede el textSize/textFont usado por la intro o el menu.
  canvas.setTextSize(1);
  canvas.setTextFont(1);
  canvas.setTextDatum(TL_DATUM);

  switch (menuSeleccion) {
    case 0:
      Princesita::init();
      gameState = STATE_GAME_PRINCESITA;
      break;

    case 1:
      Astronauta::init();
      gameState = STATE_GAME_ASTRONAUTA;
      break;

    case 2:
      CuateteMagic::init();
      gameState = STATE_GAME_CUATETE;
      break;

    case 3:
      Memorama::init();
      gameState = STATE_GAME_MEMORAMA;
      break;

    case 4:
      ZonaVerde::init();
      gameState = STATE_GAME_ZONA;
      break;


    case 5:
      EmuladoresMenu::entrar();
      break;

    case 6:
      entrarPersonalizacion();
      break;

    case 7:
      gameState = STATE_GALERIA;
      Galeria::entrar();
      break;

    case 8:
      gameState = STATE_MP3;
      ReproductorMP3::entrar();
      break;

    case 9:
      gameState = STATE_METRONOMO;
      MetronomoCocodrilo::entrar();
      break;

    case 10:
      gameState = STATE_AFINADOR;
      AfinadorCocodrilo::entrar();
      break;

    default:
      beepDenegado();
      break;
  }
}




// Detecta MENU mantenido durante el encendido.
// No modifica Preferences: el silencio dura solo esta sesión.
bool detectarArranqueSilencioso() {
  if (digitalRead(PIN_PAUSA) != LOW) return false;

  const uint32_t inicio = millis();
  while (digitalRead(PIN_PAUSA) == LOW) {
    if (millis() - inicio >= 500UL) return true;
    delay(5);
  }

  return false;
}

void mostrarDiagnosticoArranque() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextWrap(false);

  tft.setTextDatum(TC_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString("COCODRILO OS", 160, 18);

  tft.drawFastHLine(30, 46, 260, TFT_DARKGREY);

  tft.setTextFont(2);
  tft.setTextDatum(TL_DATUM);

  const int xEtiqueta = 42;
  const int xValor = 142;
  int y = 68;

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Modo:", xEtiqueta, y);
  tft.setTextColor(arranqueSilencioso ? TFT_YELLOW : TFT_GREEN, TFT_BLACK);
  tft.drawString(arranqueSilencioso ? "Silencioso" : "Normal", xValor, y);

  y += 34;
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("SD:", xEtiqueta, y);
  tft.setTextColor(sdDetectadaArranque ? TFT_GREEN : TFT_ORANGE, TFT_BLACK);
  tft.drawString(sdDetectadaArranque ? "Detectada" : "No detectada", xValor, y);

  y += 34;
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("WiFi:", xEtiqueta, y);
  if (estadoWiFiArranque == "Conectado") {
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
  } else if (estadoWiFiArranque == "Desactivado") {
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
  } else {
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  }
  tft.drawString(estadoWiFiArranque, xValor, y);


  tft.setTextDatum(BC_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("Iniciando...", 160, 226);

  delay(1200);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(1);
  tft.setTextSize(1);
}

// ════════════════════════════════════════════════════════════
//  SETUP — inicializa el hardware UNA sola vez para todo el arcade
// ════════════════════════════════════════════════════════════

static void dibujarTextoCentradoEnvuelto(const String& texto,
                                         int centroX,
                                         int yInicial,
                                         int maxCaracteres,
                                         uint16_t color,
                                         uint16_t fondo) {
  String restante = texto;
  restante.trim();
  int y = yInicial;

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(color, fondo);

  while (restante.length() > 0 && y < 188) {
    int corte = min(maxCaracteres, (int)restante.length());
    if (corte < (int)restante.length()) {
      const int espacio = restante.lastIndexOf(' ', corte);
      if (espacio > 0) corte = espacio;
    }

    String linea = restante.substring(0, corte);
    linea.trim();
    tft.drawString(linea, centroX, y, 2);

    restante.remove(0, corte);
    restante.trim();
    y += 20;
  }
}

void mostrarAvisoActualizacionWiFi() {
  if (!AvisoActualizacionWiFi::hayAvisoPendiente()) return;

  const AvisoActualizacionWiFi::Aviso& aviso =
      AvisoActualizacionWiFi::obtener();

  while (digitalRead(BUTTON_PIN) == LOW ||
         digitalRead(PIN_PAUSA) == LOW) {
    delay(5);
  }
  delay(30);

  tft.fillScreen(TFT_BLACK);
  tft.fillRoundRect(14, 12, 292, 204, 10, 0x2104);
  tft.drawRoundRect(14, 12, 292, 204, 10, TFT_CYAN);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_CYAN, 0x2104);
  tft.drawString(aviso.titulo, 160, 34, 2);

  tft.setTextColor(TFT_WHITE, 0x2104);
  tft.drawString(String("Version ") + aviso.version, 160, 61, 2);

  dibujarTextoCentradoEnvuelto(
      aviso.mensaje,
      160,
      91,
      34,
      TFT_WHITE,
      0x2104
  );

  tft.setTextColor(TFT_YELLOW, 0x2104);
  tft.drawString("Instala desde Tarjeta SD", 160, 174, 1);
  tft.setTextColor(TFT_LIGHTGREY, 0x2104);
  tft.drawString("A o MENU: continuar", 160, 198, 1);

  bool aAnterior = false;
  bool menuAnterior = false;
  while (true) {
    const bool aAhora = digitalRead(BUTTON_PIN) == LOW;
    const bool menuAhora = digitalRead(PIN_PAUSA) == LOW;

    if ((aAhora && !aAnterior) || (menuAhora && !menuAnterior)) {
      break;
    }

    aAnterior = aAhora;
    menuAnterior = menuAhora;
    delay(8);
  }

  while (digitalRead(BUTTON_PIN) == LOW ||
         digitalRead(PIN_PAUSA) == LOW) {
    delay(5);
  }

  AvisoActualizacionWiFi::consumirAviso();
  avisoActualizacionMostrado = true;
}


void setup() {
  Serial.begin(115200);

  // Botones
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(PIN_PAUSA,  INPUT_PULLUP);
  pinMode(PIN_VOLUMEN_PANEL, INPUT_PULLUP);

  // Cruceta digital
  pinMode(PIN_UP, INPUT_PULLUP);
  pinMode(PIN_DOWN, INPUT_PULLUP);
  pinMode(PIN_LEFT, INPUT_PULLUP);
  pinMode(PIN_RIGHT, INPUT_PULLUP);

  // MENU + A mantenidos al arrancar abren el Modo Tecnico.
  // Se comprueba antes del arranque silencioso para evitar conflictos.
  modoTecnicoSolicitado =
      ModoTecnico::detectarSolicitud(PIN_PAUSA, BUTTON_PIN, 650UL);

  // MENU solo, mantenido durante 500 ms, activa el arranque silencioso.
  arranqueSilencioso =
      !modoTecnicoSolicitado && detectarArranqueSilencioso();

  // Audio digital general de COCODRILO OS por MAX98357A.
  // NES todavía NO usa este audio; durante gameplay NES se silencia.
  const bool maxAudioOK = AudioMAX98357::begin();
  // GPIO7 ya no controla SD/ENABLE: queda reservado exclusivamente a LEFT.
  pinMode(PIN_LEFT, INPUT_PULLUP);
  cocodriloAplicarVolumenMAX();
  AudioMAX98357::stop();
  Serial.println(maxAudioOK
      ? "[MAX98357] Audio de COCODRILO OS listo"
      : "[MAX98357] No se pudo iniciar; la consola seguira sin audio");
  cargarPersonalizacion();
  sistemaLogros.begin();
  sistemaDesbloqueos.begin();
  cargarIconosPersonalizados();
  cargarConfiguracionPin();

  if (arranqueSilencioso) {
    volumenPaso = 0;
    sonidoActivo = false;
  }

  cocodriloAplicarVolumenMAX();
  aplicarVolumenChopper();   // sincroniza volumen digital 0..10
  if (arranqueSilencioso) buzzerOff();

  // El modulo aislado mantiene el P-MOSFET apagado mientras inicia la TFT.
  BacklightControl::prepararAntesTFT();

  // Pantalla — un solo tft + un solo canvas (sprite) compartidos
  // por el menu y los juegos.
  // La TFT continua por SPI. La SD integrada usa SD_MMC independiente
  // en modo de 1 bit, por lo que NO comparte el bus SPI de la pantalla.
  pinMode(TFT_CS, OUTPUT);
  digitalWrite(TFT_CS, HIGH);

  tft.init();
  tft.setRotation(1);   // Horizontal / landscape
  tft.fillScreen(TFT_BLACK);

  // PWM del backlight después de dejar el panel negro: evita el flash blanco
  // de arranque cuando el MOSFET ya esté instalado.
  BacklightControl::iniciar();

  // SD integrada: SD_MMC en modo de 1 bit.
  // CLK=39, CMD=38, D0=40.
  if (!SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0)) {
    Serial.println("WARN: no se pudieron asignar los pines SD_MMC");
  }

  if (!SD_MMC.begin("/sdcard", true)) {
    sdDetectadaArranque = false;
    Serial.println("WARN: no se pudo montar la SD integrada por SD_MMC");
  } else {
    sdDetectadaArranque = (SD_MMC.cardType() != CARD_NONE);

    if (sdDetectadaArranque) {
      Serial.printf("SD_MMC montada: %.2f GB\n",
                    SD_MMC.cardSize() /
                        (1024.0 * 1024.0 * 1024.0));
      cargarWallpaperGuardado();
      escanearWallpapers();
    } else {
      Serial.println("WARN: SD_MMC montada pero CARD_NONE");
    }
  }

  GestionSD::iniciar(sdDetectadaArranque);

  canvas.setColorDepth(16);
  if (!canvas.createSprite(SCR_W, SCR_H)) {
    Serial.println("WARN: No hay RAM para 16bpp, usando 8bpp");
    canvas.setColorDepth(8);
    if (!canvas.createSprite(SCR_W, SCR_H)) {
      Serial.println("ERROR: tampoco se pudo crear el canvas de 8bpp");
      while (true) delay(1000);
    }
  }
  Serial.printf("Profundidad real del canvas: %d bits\n", canvas.getColorDepth());

  // Medidor LiPo.
  Bateria::Init();

  // Wi-Fi: rollback a la mecánica estable del 03/09/2026.
  // Un solo intento controlado; sin reconector de fondo ni auto-reconnect extra.
  cargarConfiguracionWiFi();
  conectarWiFiGuardado(4500);

  // Modo oculto de diagnostico: MENU + A durante el arranque.
  if (modoTecnicoSolicitado) {
    ModoTecnico::ejecutar(
        tft,
        PIN_PAUSA,
        BUTTON_PIN,
        PIN_VOLUMEN_PANEL,
        PIN_UP,
        PIN_DOWN,
        PIN_LEFT,
        PIN_RIGHT
    );
  }

  // Diagnostico de arranque previo al menu.
  mostrarDiagnosticoArranque();

  // Si el bloqueo esta activado, se solicita antes de abrir el menu.
  solicitarPinAlArrancar();

  bateriaDirecta.setColorDepth(16);
  bateriaDirecta.createSprite(76, 18);
  bateriaDirecta.setTextWrap(false);
  bateriaDirecta.setTextFont(1);
  bateriaDirecta.setTextSize(1);
  bateriaDirecta.setTextDatum(TL_DATUM);

  precalcSky();

  // Si venimos de salir de un juego NES, el reinicio fue intencional.
  // Al volver de NES reconstruimos el menu desde cero.
  if (cocodriloRetornoNES == COCODRILO_RETORNO_NES_MAGIC) {
    cocodriloRetornoNES = 0;
    gameState = STATE_MENU;
    menuSeleccion = 0;
    volumenPanelAbierto = false;
    Bateria::ComenzarMedicion();
    invalidarMenuBase();
    drawMenu();
    Serial.println("[NES] Retorno limpio al menu completado");
  } else {
    // Community arranca directamente en el menu, sin intro ni pantalla
    // intermedia de "Pulsa el boton para jugar".
    gameState = STATE_MENU;
    menuSeleccion = 0;
    volumenPanelAbierto = false;
    Bateria::ComenzarMedicion();
    invalidarMenuBase();
    drawMenu();
  }
}

// ════════════════════════════════════════════════════════════
//  LOOP — despacha la intro, el menu, o el juego activo
// ════════════════════════════════════════════════════════════

// ════════════════════════════════════════════════════════════
// TEMPERATURA SOLO EN EL MENÚ DE PAUSA
// La lectura se renueva como máximo una vez cada 10 segundos.
// ════════════════════════════════════════════════════════════
static int temperaturaPausaC = 0;
static uint32_t ultimaLecturaTemperaturaPausaMs = 0;

static int obtenerTemperaturaPausa() {
  const uint32_t ahora = millis();

  if (ultimaLecturaTemperaturaPausaMs == 0 ||
      ahora - ultimaLecturaTemperaturaPausaMs >= 10000UL) {
    ultimaLecturaTemperaturaPausaMs = ahora;
    temperaturaPausaC = (int)roundf(temperatureRead());
  }

  return temperaturaPausaC;
}

static void dibujarTemperaturaPausaTFT() {
  const String texto = String("TEM. ") +
                       String(obtenerTemperaturaPausa()) +
                       (char)247;

  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextDatum(BL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(texto, 8, tft.height() - 5);
}

static void dibujarTemperaturaPausaCanvas() {
  const String texto = String("TEM. ") +
                       String(obtenerTemperaturaPausa()) +
                       (char)247;

  canvas.setTextFont(1);
  canvas.setTextSize(1);
  canvas.setTextDatum(BL_DATUM);
  canvas.setTextColor(TFT_WHITE, TFT_BLACK);
  canvas.drawString(texto, 8, canvas.height() - 5);
}

// ════════════════════════════════════════════════════════════
//  BATERÍA EN MENÚ Y JUEGOS NATIVOS
//  El HUD del sistema también se superpone a los juegos nativos.
//  Los emuladores mantienen su propio indicador de batería.
// ════════════════════════════════════════════════════════════
static bool debeMostrarBateria() {
  return gameState == STATE_MENU ||
         esEstadoJuego(gameState) ||
         false;
}

static void dibujarBateriaDirectaTransparente() {
  bateriaDirecta.fillSprite(TFT_MAGENTA);
  Bateria::Tick(bateriaDirecta, tft, 46, 3);
  bateriaDirecta.pushSprite(242, 2, TFT_MAGENTA);
}

// ════════════════════════════════════════════════════════════
void loop() {
  bateriaVisibleFrame = debeMostrarBateria();

  // El MAX17048 solo se consulta mientras el usuario está en el menú principal.
  // Solo el porcentaje puede invalidar el HUD; ya no existe estado de carga.
  if (gameState == STATE_MENU) {
    static int porcentajeBateriaMenuAnterior = -1;

    Bateria::Actualizar();

    const int porcentajeActual = Bateria::getPorcentaje();
    if (porcentajeActual != porcentajeBateriaMenuAnterior) {
      porcentajeBateriaMenuAnterior = porcentajeActual;
      invalidarMenuBase();
      drawMenu();
    }
  }

  static uint32_t siguienteFrameUs = 0;
  static int estadoRelojFrame = -1;
  static int estadoAnteriorLoop = -1;
  static bool pantallaInicioDibujada = false;

  // El diagnóstico se atiende incluso mientras esperamos el siguiente frame.
  debugActualizar();
  reportarPerfilSiCorresponde();

  // Wi-Fi usa la mecánica estable sin reconector de fondo.


  // Si acabamos de entrar en la pantalla de inicio, permitimos
  // dibujarla una sola vez. Mientras sigamos en STATE_START no
  // se vuelve a decodificar el JPG desde la SD.
  if ((int)gameState != estadoAnteriorLoop) {
    if (gameState == STATE_START) {
      pantallaInicioDibujada = false;
    }
    estadoAnteriorLoop = (int)gameState;
  }

  unsigned long now = millis();

  // ============================================================
  // RELOJ DE FRAME INDEPENDIENTE DEL SPI/TFT
  // ============================================================
  // Launcher/UI siguen a 50 Hz.
  // Juegos nativos usan su propio reloj (~45 Hz inicialmente).
  // Así 80 MHz mejora únicamente el tiempo disponible para dibujar y NO
  // acelera automáticamente la lógica del juego.
  const uint32_t periodoFrameUs =
      esEstadoJuego(gameState) ? JUEGO_NATIVO_FRAME_US : OS_FRAME_US;

  const uint32_t ahoraUs = micros();

  // Al cambiar de estado reiniciamos fase para que una pausa/menu no provoque
  // catch-up ni varios updates juntos al volver al gameplay.
  if (estadoRelojFrame != (int)gameState || siguienteFrameUs == 0) {
    estadoRelojFrame = (int)gameState;
    siguienteFrameUs = ahoraUs + periodoFrameUs;
  }

  const int32_t faltaUs = (int32_t)(siguienteFrameUs - ahoraUs);
  if (faltaUs > 0) {
    // No bloqueamos con delays largos: WiFi/RTOS siguen respirando.
    if (faltaUs > 1500) delay(1);
    else taskYIELD();
    return;
  }

  // Si algo bloqueó varios frames (SD, modal, etc.), no tratamos de
  // recuperarlos de golpe. Eso evita una ráfaga de gameplay acelerado.
  const uint32_t atrasoUs = (uint32_t)(ahoraUs - siguienteFrameUs);
  if (atrasoUs > periodoFrameUs * 3UL) {
    siguienteFrameUs = ahoraUs + periodoFrameUs;
  } else {
    siguienteFrameUs += periodoFrameUs;
  }

  btnCurrent = (digitalRead(BUTTON_PIN) == LOW);
  bool btnPressed = (btnCurrent && !btnPrev);
  btnPrev = btnCurrent;

  // ── "PULSA EL BOTON PARA JUGAR" ──
  if (gameState == STATE_START) {
    if (!pantallaInicioDibujada) {
      drawStartScreen();
      pantallaInicioDibujada = true;
    }

    // Las consultas HTTP se hacen aquí, nunca durante la intro.
    // Se ejecuta una por frame para evitar encadenar dos esperas largas.
    if (WiFi.status() == WL_CONNECTED) {
      if (!avisoActualizacionConsultado) {
        AvisoActualizacionWiFi::consultar(ActualizacionSD::VERSION_ACTUAL);
        avisoActualizacionConsultado = true;
      }
    }

    if (btnPressed) {
      gameState = STATE_MENU;
      menuSeleccion = 0;
      volumenPanelAbierto = false;

      // La batería comienza a medirse aquí, no durante BIOS ni intros.
      Bateria::ComenzarMedicion();

      invalidarMenuBase();
      drawMenu();

      if (!avisoActualizacionMostrado &&
          AvisoActualizacionWiFi::hayAvisoPendiente()) {
        mostrarAvisoActualizacionWiFi();
        invalidarMenuBase();
        drawMenu();
      }


      return;
    }

    return;
  }

  // ── PERSONALIZACION ──
  if (gameState == STATE_CONFIG_MAIN) {
    actualizarPersonalizacion(btnPressed);
    return;
  }
  if (gameState == STATE_CONFIG_WALLPAPER) {
    actualizarSelectorWallpaper(btnPressed);
    return;
  }
  if (gameState == STATE_CONFIG_CURSOR) {
    actualizarSelectorCursor(btnPressed);
    return;
  }
  if (gameState == STATE_CONFIG_ICON_APP) {
    actualizarSelectorAppIcono(btnPressed);
    return;
  }
  if (gameState == STATE_CONFIG_ICON_PICK) {
    actualizarSelectorIcono(btnPressed);
    return;
  }
  if (gameState == STATE_CONFIG_WIFI) {
    actualizarMenuWiFi(btnPressed);
    return;
  }
  if (gameState == STATE_CONFIG_WIFI_PORTAL) {
    actualizarPortalWiFi();
    return;
  }
  if (gameState == STATE_CONFIG_SD) {
    actualizarMenuSD(btnPressed);
    return;
  }
  if (gameState == STATE_CONFIG_UPDATES) {
    actualizarActualizaciones(btnPressed);
    return;
  }
  if (gameState == STATE_CONFIG_PIN) {
    actualizarMenuPin(btnPressed);
    return;
  }
  if (gameState == STATE_CONFIG_PIN_EDIT) {
    actualizarEditorPin(btnPressed);
    return;
  }
  if (gameState == STATE_CONFIG_RESET) {
    actualizarRestablecer();
    return;
  }
  if (gameState == STATE_CONFIG_ACERCA) {
    actualizarAcercaDe();
    return;
  }
  if (gameState == STATE_CONFIG_BRILLO) {
    actualizarMenuBrillo();
    return;
  }

  // ── LOGROS DE PRINCESITA ──
  if (gameState == STATE_ACHIEVEMENTS) {
    actualizarMenuLogros(btnPressed);
    return;
  }

  // ── GALERÍA ──
  if (gameState == STATE_GALERIA) {
    Galeria::actualizar(btnPressed);
    return;
  }

  // ── REPRODUCTOR MP3 ──
  if (gameState == STATE_MP3) {
    ReproductorMP3::actualizar(btnPressed);
    return;
  }

  if (gameState == STATE_METRONOMO) {
    MetronomoCocodrilo::actualizar(btnPressed);
    return;
  }
  if (gameState == STATE_AFINADOR) {
    AfinadorCocodrilo::actualizar();
    return;
  }

  // ── MENU DE JUEGOS ──
  if (gameState == STATE_MENU) {
    actualizarEncabezadoMenuSiNecesario();
    static bool menuLogrosAnterior = false;
    const bool menuLogrosAhora = digitalRead(PIN_PAUSA) == LOW;
    const bool menuLogrosPulsado = menuLogrosAhora && !menuLogrosAnterior;
    menuLogrosAnterior = menuLogrosAhora;

    // Solo Princesita (0) y Astronauta (1) tienen logros en Community.
    if (menuLogrosPulsado && !volumenPanelAbierto && (menuSeleccion == 0 || menuSeleccion == 1)) {
      juegoLogrosSeleccionado = (uint8_t)menuSeleccion;
      logroSeleccionado = 0;
      logroPrimerVisible = 0;
      gameState = STATE_ACHIEVEMENTS;
      dibujarMenuLogros();
      return;
    }

    manejarBotonVolumenMenu();
    manejarCrucetaMenu();
    if (btnPressed) {
      if (volumenPanelAbierto) {
        volumenPanelAbierto = false;
        invalidarMenuBase();
        drawMenu();
      } else {
        iniciarJuegoSeleccionado();
      }
    }
    return;
  }

  if (gameState == STATE_EMULADORES) {
    EmuladoresMenu::actualizar(btnPressed);
    delay(1);
    return;
  }

  if (gameState == STATE_NES_BROWSER) {
    NESLauncher::actualizar(btnPressed);
    delay(1);
    return;
  }

  if (gameState == STATE_GBC_BROWSER) {
    GBCLauncher::actualizar(btnPressed);
    delay(1);
    return;
  }

  // ── JUEGO ACTIVO ──
  // Medimos la llamada completa. perfilPushSprite() mide por separado
  // el envío del framebuffer a la TFT, así podemos estimar el resto.
  const uint32_t perfilUpdateInicioUs = micros();
  switch (gameState) {
    case STATE_GAME_PRINCESITA: Princesita::update();     break;
    case STATE_GAME_ASTRONAUTA: Astronauta::update();     break;
    case STATE_GAME_CUATETE:    CuateteMagic::update();   break;
    case STATE_GAME_MEMORAMA:   Memorama::update();       break;
    case STATE_GAME_ZONA:       ZonaVerde::update();      break;

    default: break;
  }

  // Estos dos juegos no usan el framebuffer global. Su zona superior no se
  // repinta durante el movimiento normal, por lo que el icono permanece
  // estable y se copia usando transparencia, sin fondo negro.
  if (bateriaVisibleFrame &&
      (gameState == STATE_GAME_CUATETE || gameState == STATE_GAME_MEMORAMA)) {
    dibujarBateriaDirectaTransparente();
  }

  // Se dibuja después del juego para que el corazón quede por encima del frame.
  perfilRegistrarUpdate(micros() - perfilUpdateInicioUs);

  // El juego activo pidió volver al menú (Salir, en su pausa MENU)
  if (exitRequested) {
    exitRequested = false;

    // Antidoble-click global: la pulsacion de MENU que pidio salir
    // debe terminar antes de que el launcher vuelva a aceptar entradas.
    // Asi un mismo toque nunca atraviesa juego -> menu del arcade.
    esperarSoltarMenu();

    if (gameState == STATE_GAME_ASTRONAUTA) Astronauta::cerrar();
    gameState = STATE_MENU;
    menuSeleccion = 0;
    volumenPanelAbierto = false;
    invalidarMenuBase();
    drawMenu();
  }

  // Pausa cooperativa mínima: no altera perceptiblemente los controles,
  // pero permite al sistema ejecutar tareas internas y entrar en reposo.
  delay(1);
}
