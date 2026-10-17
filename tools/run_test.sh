#!/bin/sh
# Fuehrt einen Skriptlauf ohne Live-Steuerung aus: tools/run_test.sh <runname> <skript> <maxframes>
# Feste Einstellungen fuer Determinismus-Tests: festes dt, keine Videos, 60 fps.
cd "$(dirname "$0")/.." || exit 1
RUN="runs/$1"
rm -rf "$RUN"; mkdir -p "$RUN"
cp "$2" "$RUN/script.txt"
WRUN=$(cygpath -w "$PWD/$RUN")
export LBO_DIR="$WRUN" LBO_MODE=script LBO_SCRIPT="$WRUN/script.txt" LBO_FIXEDDT=1 LBO_NOMOVIES=1 LBO_FPSCAP=60
export LBO_SYNCBG=${LBO_SYNCBG:-0} LBO_MULTI=${LBO_MULTI:-0} LBO_MAXFRAMES="$3" LBO_SHOT_EVERY=${LBO_SHOT_EVERY:-1500}
unset LBO_LIVE
./build/lbo-launch.exe --wait -- -Windowed
