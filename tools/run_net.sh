#!/bin/sh
# Lokaler Lockstep-Test mit zwei Instanzen: tools/run_net.sh <name> <maxframes> [verzoegerung]
# Host bekommt nur die Pad-0-Eingaben, Client nur die Pad-1-Eingaben; der Rest kommt per UDP.
cd "$(dirname "$0")/.." || exit 1
for ROLE in host join; do
  RUN="runs/$1_$ROLE"; rm -rf "$RUN"; mkdir -p "$RUN"
  cp "tools/scripts/net_$ROLE.txt" "$RUN/script.txt"
done
start_one() {
  WRUN=$(cygpath -w "$PWD/runs/$1_$2")
  env LBO_DIR="$WRUN" LBO_MODE=script LBO_SCRIPT="$WRUN/script.txt" LBO_FIXEDDT=1 LBO_NOMOVIES=1 \
      LBO_FPSCAP=60 LBO_SYNCBG=2 LBO_MULTI=1 LBO_MAXFRAMES="$3" LBO_SHOT_EVERY=${LBO_SHOT_EVERY:-3000} \
      LBO_NET="$2" LBO_DELAY="${4:-3}" LBO_NETLOSS="${LBO_NETLOSS:-0}" ./build/lbo-launch.exe --wait -- -Windowed > "runs/$1_$2.out" 2>&1
}
start_one "$1" host "$2" "$3" &
sleep 2
start_one "$1" join "$2" "$3"
wait
