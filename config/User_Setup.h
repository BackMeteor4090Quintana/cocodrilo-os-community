 // ============================================================
//  User_Setup.h - ST7789 240x320 en ESP32-S3 N16R8 (TFT_eSPI)
//  Reemplazá el archivo:
//  Arduino/libraries/TFT_eSPI/User_Setup.h
//  por este (hacé backup del original si querés conservarlo).
// ============================================================

#define USER_SETUP_INFO "User_Setup - S3 N16R8 ST7789 240x320"

// ---------- Driver ----------
#define ST7789_DRIVER

// FIX CRITICO para ESP32-S3: sin esto, tft.init() crashea con
// "Guru Meditation Error: StoreProhibited" (bug conocido de TFT_eSPI
// en ESP32-S3, ver github.com/Bodmer/TFT_eSPI/issues/3289)
#define USE_HSPI_PORT

// ---------- Resolución ----------
#define TFT_WIDTH  240
#define TFT_HEIGHT 320

// Si los colores salen invertidos (piel morada/azulada), descomentá esta
// línea. DEJALA COMENTADA por ahora: la estamos probando por separado
// para no mezclarla con otros problemas.
 #define TFT_RGB_ORDER TFT_BGR

// Mismo ajuste que COCODRILO OS original: inversion de display OFF.
// Corrige blanco/negro intercambiados en este panel ST7789.
//#define TFT_INVERSION_ON
#define TFT_INVERSION_OFF

// ---------- Pines (GPIO seguros en el S3 N16R8) ----------
// Evitados: GPIO0 (BOOT), GPIO26-32 (flash QSPI), GPIO33-37 (PSRAM
// octal), GPIO19-20 (USB nativo), GPIO43/44 (UART0), GPIO45/46 (estrapeo)
#define TFT_MOSI 11
#define TFT_MISO 13   // <- faltaba: sin esto algunos comandos/lecturas fallan
#define TFT_SCLK 12
#define TFT_CS   10   // Chip select
#define TFT_DC    9   // <- faltaba por completo: SIN ESTO NO HAY IMAGEN
#define TFT_RST   8   // <- faltaba por completo

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

// ---------- Velocidad SPI ----------
// Igual que COCODRILO OS original: SPI a 80 MHz (no 80 FPS).
// Si el cableado produce artefactos, revisar integridad de senal.
#define SPI_FREQUENCY       80000000
#define SPI_READ_FREQUENCY  10000000
