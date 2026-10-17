#!/bin/sh
# Startet das Spiel im Skriptmodus mit Live-Steuerung: tools/start_live.sh <runname> [skriptdatei]
# Weitere LBO_*-Variablen (z. B. LBO_FIXEDDT=1) koennen vorher exportiert werden.
cd "$(dirname "$0")/.." || exit 1
RUN="runs/$1"
mkdir -p "$RUN"
if [ -n "$2" ]; then cp "$2" "$RUN/script.txt"; else printf 'connect 0\n' > "$RUN/script.txt"; fi
: > runs/live.txt
WRUN=$(cygpath -w "$PWD/$RUN")
WLIVE=$(cygpath -w "$PWD/runs/live.txt")
export LBO_DIR="$WRUN" LBO_MODE=script LBO_SCRIPT="$WRUN/script.txt" LBO_LIVE="$WLIVE"
export LBO_MAXFRAMES=${LBO_MAXFRAMES:-0}
exec ./build/lbo-launch.exe --wait -- -Windowed
