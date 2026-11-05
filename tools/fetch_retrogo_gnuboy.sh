#!/usr/bin/env bash
set -euo pipefail

echo "[RETROGO] Preparando Gnuboy + Nofrendo para COCODRILO OS"

rm -rf /tmp/retro-go
rm -rf "$GITHUB_WORKSPACE/libraries/RetroGoGnuboy"
rm -rf "$GITHUB_WORKSPACE/libraries/RetroGoNofrendo"

mkdir -p "$GITHUB_WORKSPACE/libraries/RetroGoGnuboy/src"
mkdir -p "$GITHUB_WORKSPACE/libraries/RetroGoNofrendo/src"

git clone --depth 1 --filter=blob:none --sparse \
  https://github.com/ducalex/retro-go.git \
  /tmp/retro-go

cd /tmp/retro-go
git sparse-checkout set \
  retro-core/components/gnuboy \
  retro-core/components/nofrendo

# ============================================================
# GNUBOY
# ============================================================
cp retro-core/components/gnuboy/*.c \
  "$GITHUB_WORKSPACE/libraries/RetroGoGnuboy/src/"

cp retro-core/components/gnuboy/*.h \
  "$GITHUB_WORKSPACE/libraries/RetroGoGnuboy/src/"

cp retro-core/components/gnuboy/COPYING \
  "$GITHUB_WORKSPACE/libraries/RetroGoGnuboy/" || true

python3 - <<'PY'
from pathlib import Path
import os

workspace = Path(os.environ["GITHUB_WORKSPACE"])
srcdir = workspace / "libraries" / "RetroGoGnuboy" / "src"
pragma = '#pragma GCC optimize ("O3")\n'

for p in srcdir.glob("*.c"):
    s = p.read_text(encoding="utf-8")
    if '#pragma GCC optimize ("O3")' not in s:
        p.write_text(pragma + s, encoding="utf-8")

p = srcdir / "gnuboy.h"
s = p.read_text(encoding="utf-8")
if "#define IRAM_ATTR" in s and "#ifndef IRAM_ATTR" not in s:
    s = s.replace(
        "#define IRAM_ATTR",
        "#ifndef IRAM_ATTR\n#define IRAM_ATTR\n#endif",
        1
    )
p.write_text(s, encoding="utf-8")
print("[PATCH] Gnuboy -> O3 por archivo")
PY

cat > "$GITHUB_WORKSPACE/libraries/RetroGoGnuboy/library.properties" <<'EOF'
name=RetroGoGnuboy
version=1.46-cocodrilo
author=ducalex / Retro-Go
sentence=Retro-Go Gnuboy core for COCODRILO OS
paragraph=Fetched from upstream Retro-Go during CI
category=Other
architectures=esp32
EOF

# ============================================================
# NOFRENDO
# Arduino compila recursivamente todo lo que vive bajo src/.
# Conservamos la estructura nes/ y mappers/.
# ============================================================
cp -R retro-core/components/nofrendo/. \
  "$GITHUB_WORKSPACE/libraries/RetroGoNofrendo/src/"

rm -rf "$GITHUB_WORKSPACE/libraries/RetroGoNofrendo/src/docs"
rm -f  "$GITHUB_WORKSPACE/libraries/RetroGoNofrendo/src/CMakeLists.txt"

# COPYING fuera de src para no ensuciar compilación.
if [ -f "$GITHUB_WORKSPACE/libraries/RetroGoNofrendo/src/COPYING" ]; then
  mv "$GITHUB_WORKSPACE/libraries/RetroGoNofrendo/src/COPYING" \
     "$GITHUB_WORKSPACE/libraries/RetroGoNofrendo/COPYING"
fi
rm -f "$GITHUB_WORKSPACE/libraries/RetroGoNofrendo/src/CREDITS"

python3 - <<'PY'
from pathlib import Path
import os

workspace = Path(os.environ["GITHUB_WORKSPACE"])
srcdir = workspace / "libraries" / "RetroGoNofrendo" / "src"

# Retro-Go compila Nofrendo con -O3. Lo aplicamos SOLO al core.
pragma = '#pragma GCC optimize ("O3")\n'
for p in srcdir.rglob("*.c"):
    s = p.read_text(encoding="utf-8", errors="ignore")
    if '#pragma GCC optimize ("O3")' not in s:
        p.write_text(pragma + s, encoding="utf-8")

# El CPU 6502 de Nofrendo incluye un dispatch por computed-goto ya mantenido
# por el propio core, pero upstream lo deja desactivado. En ESP32-S3/GCC
# evita el switch gigante por opcode y conserva exactamente los mismos handlers.
cpu = srcdir / "nes" / "cpu.c"
s = cpu.read_text(encoding="utf-8", errors="ignore")
s = s.replace("// #define NES6502_JUMPTABLE", "#define NES6502_JUMPTABLE", 1)
cpu.write_text(s, encoding="utf-8")

# Hot path PPU: ambos bitplanes de una fila de tile (addr y addr+8)
# siempre viven en la misma página CHR. Resolvemos esa página una sola vez.
ppu = srcdir / "nes" / "ppu.c"
s = ppu.read_text(encoding="utf-8", errors="ignore")
old = """INLINE uint32 get_patpix(uint32 tile_addr)
{
   uint32 pat1 = PPU_MEM_READ(tile_addr);
   uint32 pat2 = PPU_MEM_READ(tile_addr + 8);
   return ((pat2 & 0xAA) << 8) | ((pat2 & 0x55) << 1)
        | ((pat1 & 0xAA) << 7) | ((pat1 & 0x55) << 0);
}"""
new = """INLINE uint32 get_patpix(uint32 tile_addr)
{
   uint8 *page = ppu.page[tile_addr >> PPU_PAGESHIFT];
   uint32 pat1 = page[tile_addr];
   uint32 pat2 = page[tile_addr + 8];
   return ((pat2 & 0xAA) << 8) | ((pat2 & 0x55) << 1)
        | ((pat1 & 0xAA) << 7) | ((pat1 & 0x55) << 0);
}"""
if old not in s:
    raise SystemExit("No se encontró get_patpix esperado en Nofrendo")
s = s.replace(old, new, 1)
ppu.write_text(s, encoding="utf-8")

# Hot path sprites: Nofrendo recorre OAM en cada scanline visible.
# Conservamos exactamente sprite-0 hit, límite de 8 sprites, prioridades,
# flips y callbacks de mapper, pero evitamos recomputar el puntero OAM y
# reducimos la prueba vertical a una sola comparación unsigned.
ppu = srcdir / "nes" / "ppu.c"
s = ppu.read_text(encoding="utf-8", errors="ignore")
old = r'''INLINE void ppu_renderoam(uint8 *vidbuf, int scanline, bool draw)
{
   if (!ppu.obj_on)
      return;

   int sprite_height = ppu.obj_height;
   int sprite_offset = ppu.obj_base;
   uint8 savecol[8];

   /* Left column masking enabled, save first 8 pixels to reapply at the end */
   if (draw && !ppu.left_obj_on)
      memcpy(&savecol, vidbuf, 8);

   for (int sprite_num = 0, count = 0; sprite_num < 64; sprite_num++)
   {
      ppu_obj_t *sprite = (ppu_obj_t *)ppu.oam + sprite_num;

      int sprite_y = sprite->y_loc + 1;
      int tile_index = sprite->tile;

      /* Check to see if sprite is out of range */
      if ((sprite_y > scanline) || (sprite_y <= (scanline - sprite_height))
          || (0 == sprite_y) || (sprite_y >= 240))
         continue;

      /* Handle $FD/$FE magic tile CHR-ROM switching (MMC2/MMC4) */
      if (ppu.latchfunc && (tile_index == 0xFD || tile_index == 0xFE))
         ppu.latchfunc(sprite_offset, tile_index);

      int tile_addr, y_offset;

      /* 8x16 even sprites use $0000, odd use $1000 */
      if (16 == sprite_height)
         tile_addr = ((tile_index & 1) << 12) | ((tile_index & 0xFE) << 4);
      else
         tile_addr = sprite_offset + (tile_index << 4);

      /* Calculate offset (line within the sprite) */
      y_offset = scanline - sprite_y;
      if (y_offset > 7)
         y_offset += 8;

      /* Account for vertical flippage */
      if (sprite->attr & OAMF_VFLIP)
      {
         if (16 == sprite_height)
            y_offset -= 23;
         else
            y_offset -= 7;

         tile_addr -= y_offset;
      }
      else
      {
         tile_addr += y_offset;
      }

      /* Check for a strike on sprite 0 if strike flag isn't set */
      if (sprite_num == 0 && !ppu.strikeflag)
      {
         check_strike(draw ? vidbuf + sprite->x_loc : NULL, sprite->attr, get_patpix(tile_addr));
      }

      /* If we don't draw to buffer then we're done after sprite 0 */
      if (!draw)
         return;

      /* Fetch tile and draw it */
      draw_oamtile(
         vidbuf + sprite->x_loc,
         sprite->attr,
         get_patpix(tile_addr),
         ppu.palette + 16 + ((sprite->attr & 3) << 2));

      /* maximum of 8 sprites per scanline */
      if (OPT(PPU_LIMIT_SPRITES) && ++count == PPU_MAXSPRITE)
      {
         ppu.stat |= PPU_STATF_MAXSPRITE;
         break;
      }
   }

   /* Mask left columns if necessary */
   if (draw && !ppu.left_obj_on)
      memcpy(vidbuf, &savecol, 8);
}'''
new = r'''INLINE void ppu_renderoam(uint8 *vidbuf, int scanline, bool draw)
{
   if (!ppu.obj_on)
      return;

   const int sprite_height = ppu.obj_height;
   const int sprite_offset = ppu.obj_base;
   uint8 savecol[8];

   /* Left column masking enabled, save first 8 pixels to reapply at the end */
   if (draw && !ppu.left_obj_on)
      memcpy(&savecol, vidbuf, 8);

   ppu_obj_t *sprite = (ppu_obj_t *)ppu.oam;

   for (int sprite_num = 0, count = 0;
        sprite_num < 64;
        sprite_num++, sprite++)
   {
      const int sprite_y = sprite->y_loc + 1;

      /* Rango visible: equivalente a las comparaciones originales,
         pero y_offset permite resolverlo con una sola prueba unsigned. */
      int y_offset = scanline - sprite_y;
      if ((unsigned)y_offset >= (unsigned)sprite_height || sprite_y >= 240)
         continue;

      const int tile_index = sprite->tile;
      const uint8 attr = sprite->attr;

      /* Handle $FD/$FE magic tile CHR-ROM switching (MMC2/MMC4) */
      if (ppu.latchfunc && (tile_index == 0xFD || tile_index == 0xFE))
         ppu.latchfunc(sprite_offset, tile_index);

      int tile_addr;

      /* 8x16 even sprites use $0000, odd use $1000 */
      if (16 == sprite_height)
         tile_addr = ((tile_index & 1) << 12) | ((tile_index & 0xFE) << 4);
      else
         tile_addr = sprite_offset + (tile_index << 4);

      /* Calculate offset (line within the sprite) */
      if (y_offset > 7)
         y_offset += 8;

      /* Account for vertical flippage */
      if (attr & OAMF_VFLIP)
      {
         if (16 == sprite_height)
            y_offset -= 23;
         else
            y_offset -= 7;

         tile_addr -= y_offset;
      }
      else
      {
         tile_addr += y_offset;
      }

      /* Leer CHR una sola vez. Sprite 0 reutiliza el mismo patrón para
         strike y dibujo en lugar de hacer dos lecturas consecutivas. */
      const uint32 pattern = get_patpix(tile_addr);

      if (sprite_num == 0 && !ppu.strikeflag)
         check_strike(draw ? vidbuf + sprite->x_loc : NULL, attr, pattern);

      /* If we don't draw to buffer then we're done after sprite 0 */
      if (!draw)
         return;

      draw_oamtile(
         vidbuf + sprite->x_loc,
         attr,
         pattern,
         ppu.palette + 16 + ((attr & 3) << 2));

      /* maximum of 8 sprites per scanline */
      if (OPT(PPU_LIMIT_SPRITES) && ++count == PPU_MAXSPRITE)
      {
         ppu.stat |= PPU_STATF_MAXSPRITE;
         break;
      }
   }

   /* Mask left columns if necessary */
   if (draw && !ppu.left_obj_on)
      memcpy(vidbuf, &savecol, 8);
}'''
if old not in s:
    raise SystemExit("No se encontró ppu_renderoam esperado en Nofrendo")
s = s.replace(old, new, 1)
ppu.write_text(s, encoding="utf-8")

# En ESP32 Arduino IRAM_ATTR viene de esp_attr.h.
# Solo tocamos archivos que realmente usan el atributo.
for p in list(srcdir.rglob("*.c")) + list(srcdir.rglob("*.h")):
    s = p.read_text(encoding="utf-8", errors="ignore")
    if "IRAM_ATTR" in s and "esp_attr.h" not in s:
        p.write_text('#include "esp_attr.h"\n' + s, encoding="utf-8")

print("[PATCH] Nofrendo -> O3 + CPU jump table + PPU CHR fast path + OAM fast path + compat IRAM_ATTR")
PY

cat > "$GITHUB_WORKSPACE/libraries/RetroGoNofrendo/library.properties" <<'EOF'
name=RetroGoNofrendo
version=1.46-cocodrilo
author=ducalex / Retro-Go
sentence=Retro-Go Nofrendo NES core for COCODRILO OS
paragraph=Fetched from upstream Retro-Go during CI
category=Other
architectures=esp32
EOF

echo "[RETROGO] Gnuboy + Nofrendo preparados correctamente"
