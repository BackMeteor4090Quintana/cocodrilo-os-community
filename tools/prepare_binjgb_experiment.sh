#!/usr/bin/env bash
# Inspección aislada de binjgb, no altera el sketch ni los BIN de COCODRILO OS.
set -euo pipefail

if [[ $# -ne 1 || ! "$1" =~ ^[0-9a-fA-F]{40}$ ]]; then
  echo "Uso: bash tools/prepare_binjgb_experiment.sh <SHA_COMMIT_40_CARACTERES>" >&2
  echo "Consulta: git ls-remote https://github.com/binji/binjgb.git HEAD" >&2
  exit 2
fi

ref="$1"
root="$(mktemp -d "${TMPDIR:-/tmp}/cocodrilo-binjgb.XXXXXX")"
dest="$root/binjgb"

echo "[binjgb] Clonando en directorio temporal: $dest"
git clone --quiet --no-checkout --filter=blob:none https://github.com/binji/binjgb.git "$dest"
git -C "$dest" fetch --quiet --depth 1 origin "$ref"
git -C "$dest" checkout --quiet --detach FETCH_HEAD
actual="$(git -C "$dest" rev-parse HEAD)"

if [[ "${actual,,}" != "${ref,,}" ]]; then
  echo "[binjgb] ERROR: commit distinto al solicitado: $actual" >&2
  exit 1
fi

if [[ ! -f "$dest/LICENSE" ]]; then
  echo "[binjgb] ERROR: no se encontró licencia upstream; inspeccionar manualmente." >&2
  exit 1
fi

echo "[binjgb] Commit exacto: $actual"
echo "[binjgb] Licencia: $dest/LICENSE"
echo "[binjgb] Inventario de archivos fuente principales:"
find "$dest/src" -maxdepth 2 -type f \( -name '*.c' -o -name '*.h' \) -print | sort
echo
echo "[binjgb] Código disponible en: $dest"
echo "[binjgb] No se ha modificado ningún archivo del firmware."
echo "[binjgb] No se han creado ramas ni ejecutado compilaciones."
