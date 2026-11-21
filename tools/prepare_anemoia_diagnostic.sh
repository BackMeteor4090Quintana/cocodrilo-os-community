#!/usr/bin/env bash
# Construye un sketch de DIAGNÓSTICO a partir del núcleo Anemoia ya adaptado.
# No modifica el firmware estable ni publica BIN; no clona ni crea ramas.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ANEMOIA="$ROOT/staging/anemoia"
OUT="${1:-${RUNNER_TEMP:-/tmp}/cocodrilo-anemoia-diagnostic}"
SKETCH="$OUT/AnemoiaDiagnostic"

if [[ ! -f "$ANEMOIA/ANEMOIA_LICENSE_GPLv3.txt" || ! -f "$ANEMOIA/src/core/bus.cpp" ]]; then
  echo "[Anemoia] Faltan fuentes o licencia en staging/anemoia" >&2
  exit 1
fi

if [[ "$SKETCH" == "$ROOT"/* ]]; then
  echo "[Anemoia] La salida de diagnóstico debe estar FUERA del repositorio." >&2
  exit 1
fi
mkdir -p "$SKETCH"
cp -R "$ANEMOIA/src" "$SKETCH/src"
cp "$ANEMOIA/ANEMOIA_LICENSE_GPLv3.txt" "$OUT/ANEMOIA_LICENSE_GPLv3.txt"
cp "$ROOT/config/partitions.csv" "$SKETCH/partitions.csv"

cat > "$SKETCH/AnemoiaDiagnostic.ino" <<'EOF'
#include <Arduino.h>
#include <TFT_eSPI.h>
#include "src/core/bus.h"
#include "src/core/cartridge.h"

// Prueba de compilación y enlazado del núcleo, NO se ejecuta ninguna ROM.
// El firmware de COCODRILO OS y su emulador NES estable no se tocan.
void setup() {
  Serial.begin(115200);
  Serial.println("[Anemoia] Núcleo de diagnóstico compilado");
}
void loop() {
  delay(1000);
}
EOF

echo "[Anemoia] Sketch de diagnóstico: $SKETCH"
echo "[Anemoia] Incluye fuentes locales y GPLv3; no modifica firmware-builds."
