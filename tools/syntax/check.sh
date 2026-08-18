#!/usr/bin/env bash
# Typecheck the firmware's C++ without ESP-IDF. See README.md.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MAIN="$(cd "$HERE/../../main" && pwd)"
CXX="${CXX:-clang++}"
command -v "$CXX" >/dev/null || { echo "no $CXX on PATH"; exit 2; }

INC="-I$HERE/stub -I$MAIN -I$MAIN/sdk -I$MAIN/robot -I$MAIN/wasm -I$MAIN/skills -I$MAIN/util -I$MAIN/fs"

# Files whose dependencies the stubs cover. Others (robot.cc, http_server.cc,
# chat_ws.cc) reach too far into ESP-IDF to be worth stubbing; idf.py covers them.
FILES=(
  sdk/wasm_host_functions.cc
  wasm/wasm_sandbox.cc
  skills/registry.cc
  skills/runner.cc
  skills/movement.cc
  skills/events.cc
  skills/autorun.cc
  util/trace_ring.cc
  robot/imu.cc
)

# C sources — same stubs, C compiler.
C_FILES=(
  robot/driver_board.c
)

fail=0
for f in "${FILES[@]}"; do
  # -Wall only: this codebase leaves exec_env unused everywhere by design, so
  # -Wextra would be all noise and no signal.
  out=$("$CXX" -std=gnu++17 -fsyntax-only -Wall $INC "$MAIN/$f" 2>&1 | grep -E "error:")
  if [ -z "$out" ]; then printf "  ok    %s\n" "$f"
  else printf "  FAIL  %s\n" "$f"; echo "$out" | sed 's|^|        |'; fail=1; fi
done

for f in "${C_FILES[@]}"; do
  out=$("${CC:-clang}" -std=gnu11 -fsyntax-only -Wall $INC "$MAIN/$f" 2>&1 | grep -E "error:")
  if [ -z "$out" ]; then printf "  ok    %s\n" "$f"
  else printf "  FAIL  %s\n" "$f"; echo "$out" | sed 's|^|        |'; fail=1; fi
done

echo
if [ $fail -eq 0 ]; then
  echo "Typechecks. Now run idf.py build — this proves nothing about behaviour."
else
  echo "Errors above. Fix them before spending time on idf.py build."
fi
exit $fail
