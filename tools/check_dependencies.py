#!/usr/bin/env python3
"""Comprueba dependencias locales del sketch Community, sin copiar archivos privados."""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SKETCH = ROOT / "staging" / "cocodrilo-os.ino"
EXCLUDED = {"Cartas.h", "CartasContenido.h", "PropuestaNovia.h",
            "EscenaSecreta_AB.h", "EscenaSecreta.h",
            "CorazonesWiFi.h", "TelegramNotificaciones.h"}
CORE_HEADERS = {"FS.h", "SD_MMC.h", "freertos/queue.h"}
THIRD_PARTY = {"TFT_eSPI.h": "TFT_eSPI 2.5.43",
               "TJpg_Decoder.h": "TJpg_Decoder"}
CORE_PREFIXES = ("SPI.h", "math.h", "pgmspace.h", "Preferences.h",
                 "WiFi.h", "WebServer.h", "DNSServer.h", "time.h",
                 "esp_heap_caps.h", "new")

def main():
    if not SKETCH.exists():
        print("FALTA el sketch de trabajo")
        return 2
    content = SKETCH.read_text(encoding="utf-8")
    includes = sorted(set(re.findall(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', content, re.M)))
    files = {p.name for p in (ROOT / "src" / "community").rglob("*") if p.is_file()}
    missing = []
    excluded = []
    for name in includes:
        if name in EXCLUDED:
            excluded.append(name)
        elif name in CORE_HEADERS or name in CORE_PREFIXES or name in THIRD_PARTY:
            continue
        elif name not in files:
            missing.append(name)
    print("Dependencias externas:", ", ".join(THIRD_PARTY.values()))
    print("Módulos prohibidos aún incluidos:", ", ".join(excluded) or "ninguno")
    print("Cabeceras locales ausentes:", ", ".join(missing) or "ninguna")
    print("NOTA: cabeceras presentes no implican que sus símbolos estén enlazados.")
    if missing or excluded:
        print("BLOQUEADO: no está preparado para compilar.")
        return 1
    print("Chequeo inicial superado: falta compilación real y auditoría.")
    return 0

if __name__ == "__main__":
    sys.exit(main())
