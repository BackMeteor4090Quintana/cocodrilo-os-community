#!/usr/bin/env python3
"""Auditoría preventiva del sketch original. No copia código ni secretos al repositorio.

Uso:
  python3 tools/auditar_ino.py /ruta/privada/cocodrilo-os.ino

Termina con código 1 mientras existan dependencias o referencias privadas.
"""
from pathlib import Path
import re
import sys

BLOQUEADOS = {
    "modulos_privados": r"Cartas(?:Contenido)?\.h|PropuestaNovia\.h|EscenaSecreta(?:_AB)?\.h|CorazonesWiFi\.h|TelegramNotificaciones\.h",
    "referencias_personales": r"\b(?:peque|coqui|vanessa|vane|novia)\b|pequenauta|princesita_peque",
    "llamadas_privadas": r"\b(?:Cartas|PropuestaNovia|EscenaSecreta|CorazonesWiFi|TelegramNotificaciones)::",
    "estados_privados": r"\bSTATE_(?:CARTAS|PROPUESTA_SECRETA)\b",
    "notificaciones": r"\b(?:encolarTelegramJuego|atenderTelegramPendiente|telegramJuegoPendiente)\b",
    "animacion_remota": r"\b(?:actualizarCorazonesRemotos|amorAnimando)\b",
    "credenciales": r"(?i)(?:bot[_-]?token|chat[_-]?id|api[_-]?key|password|secret)\s*[:=]",
}

def main() -> int:
    if len(sys.argv) != 2:
        print("Uso: auditar_ino.py <archivo-ino>")
        return 2
    ruta = Path(sys.argv[1])
    texto = ruta.read_text(encoding="utf-8")
    lineas = texto.splitlines()
    pendientes = 0
    for categoria, patron in BLOQUEADOS.items():
        hallazgos = [i for i, linea in enumerate(lineas, 1) if re.search(patron, linea, re.IGNORECASE)]
        if hallazgos:
            pendientes += len(hallazgos)
            print(f"{categoria}: {len(hallazgos)} línea(s) por revisar: {', '.join(map(str, hallazgos[:30]))}")
    if pendientes:
        print(f"NO APTO PARA PUBLICACIÓN: {pendientes} coincidencias; no se muestra el contenido privado.")
        return 1
    print("Sin coincidencias en este filtro. Requiere además revisión visual, licencias y compilación.")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
