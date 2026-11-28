#!/usr/bin/env bash
set -euo pipefail

root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
scratch=$(mktemp -d)
trap 'rm -rf -- "$scratch"' EXIT

if pkg-config --exists libcjson; then
  read -r -a includes <<<"$(pkg-config --cflags libcjson)"
  read -r -a libraries <<<"$(pkg-config --libs libcjson)"
else
  sdk="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/packages/framework-arduinoespressif32-libs"
  includes=("-I$sdk/esp32s3/include/json/cJSON")
  libraries=("-l:libcjson.so.1")
fi

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -I"$root/src" "${includes[@]}" \
  "$root/test/host/check.cpp" "$root/src/protocol.cpp" "$root/src/wire.cpp" \
  "${libraries[@]}" -o "$scratch/check"
"$scratch/check"
printf 'ESP32 protocol, watchdog, position, and Hall tests passed.\n'
