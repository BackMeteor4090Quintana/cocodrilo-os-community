#!/usr/bin/env python3
"""Pre-publication source checks. This is NOT a substitute for a history/assets review."""
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
TEXT_SUFFIXES = {".ino", ".h", ".hpp", ".c", ".cpp", ".py", ".sh", ".yml", ".yaml", ".json", ".csv", ".txt"}
PRIVATE_FILES = re.compile(
    r"(?i)(?:^|/)(?:Cartas(?:Contenido)?|PropuestaNovia|EscenaSecreta(?:_AB)?|"
    r"CorazonesWiFi|TelegramNotificaciones)\.(?:h|cpp|ino)$"
)
PRIVATE_NAMES = re.compile(r"(?i)\b(?:vanessa|vane|pequenauta|princesita_peque)\b")
PRIVATE_PHRASES = re.compile(
    r"(?i)\b(?:te amo|mi amor|con todo mi ser|la mejor amiga del mundo)\b"
)
PRIVATE_MODULES = re.compile(
    r"\b(?:CartasContenido|PropuestaNovia|EscenaSecreta_AB|"
    r"CorazonesWiFi|TelegramNotificaciones)\b"
)
CREDENTIALS = re.compile(
    r"(?i)\b(?:bot[_-]?token|telegram[_-]?token|chat[_-]?id|"
    r"api[_-]?key|private[_-]?key|wifi[_-]?password)\b\s*[:=]\s*"
    r"[\"'][^\"']{4,}[\"']"
)

def main() -> int:
    tracked = subprocess.check_output(
        ["git", "-C", str(ROOT), "ls-files", "-z"], text=True
    ).split("\0")
    problems = []
    examined = 0
    for name in filter(None, tracked):
        path = ROOT / name
        if not path.is_file():
            continue
        if PRIVATE_FILES.search(name):
            problems.append(f"{name}: forbidden private module")
            continue
        # Documentation of removed modules is fine; inspect code and scripts.
        if path.suffix.lower() not in TEXT_SUFFIXES:
            continue
        if not (name.startswith(("staging/", "src/", "config/", "tools/")) or name.startswith(".github/")):
            continue
        # Only inspect source/code, not license wording or other documentation.
        if path.suffix.lower() == ".txt":
            continue
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except UnicodeError:
            problems.append(f"{name}: unreadable text (review manually)")
            continue
        examined += 1
        for i, line in enumerate(lines, 1):
            # Strip ordinary C++ and shell comments only for name/phrase matches.
            code = line.split("//", 1)[0] if path.suffix.lower() in {".ino", ".h", ".hpp", ".cpp", ".c"} else line
            if (name.startswith("staging/") or name.startswith("src/")) and (PRIVATE_NAMES.search(code) or PRIVATE_PHRASES.search(code) or PRIVATE_MODULES.search(code)):
                problems.append(f"{name}:{i}: private reference")
            if CREDENTIALS.search(line):
                problems.append(f"{name}:{i}: possible embedded credential")
    print(f"Audited {examined} tracked text source/config files.")
    for p in problems[:80]:
        print("BLOCK:", p)
    if len(problems) > 80:
        print(f"... plus {len(problems) - 80} additional findings")
    if problems:
        print("BLOCKED: review before publishing. No sensitive content printed.")
        return 1
    print("Source checks passed. STILL REQUIRED: all binary/graphic assets, git history, license review, and hardware build validation.")
    return 0

if __name__ == "__main__":
    sys.exit(main())
