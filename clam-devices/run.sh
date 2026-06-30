#!/usr/bin/env bash
# Gon: nap moi truong ESP-IDF + flash/monitor con ESP32-C3.
# Dung:
#   ./run.sh                 -> build + flash + monitor (mac dinh)
#   ./run.sh build           -> chi build
#   ./run.sh monitor         -> chi mo monitor
#   ./run.sh flash           -> chi flash
#   ./run.sh -p /dev/ttyACM1 flash monitor   -> chi dinh cong khac
set -e

. ~/esp-idf/export.sh
cd "$(dirname "$0")"

if [ $# -eq 0 ]; then
  exec idf.py -p /dev/ttyACM0 flash monitor
else
  exec idf.py "$@"
fi
