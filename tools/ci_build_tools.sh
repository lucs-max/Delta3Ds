#!/bin/sh
# CI helper: builds makerom and bannertool (needed to pack .cia files) from
# source into the given folder. Prebuilt Linux downloads are no longer
# reliably available, so we compile them.
#   tools/ci_build_tools.sh <out dir>
set -eu
OUT=$(mkdir -p "${1:-ci-tools}" && cd "${1:-ci-tools}" && pwd)
SRC=$(mktemp -d)

echo "== makerom (3DSGuy/Project_CTR)"
git clone -q --depth 1 https://github.com/3DSGuy/Project_CTR "$SRC/Project_CTR"
make -C "$SRC/Project_CTR/makerom" deps -j"$(nproc)"
make -C "$SRC/Project_CTR/makerom" -j"$(nproc)"
cp "$(find "$SRC/Project_CTR/makerom/bin" -type f -name makerom | head -n1)" "$OUT/makerom"

echo "== bannertool (carstene1ns/3ds-bannertool)"
git clone -q --depth 1 https://github.com/carstene1ns/3ds-bannertool "$SRC/bannertool"
cmake -S "$SRC/bannertool" -B "$SRC/bannertool/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$SRC/bannertool/build" -j"$(nproc)"
cp "$(find "$SRC/bannertool/build" -type f -name bannertool | head -n1)" "$OUT/bannertool"

chmod +x "$OUT/makerom" "$OUT/bannertool"
"$OUT/makerom" 2>&1 | head -n 3 || true
"$OUT/bannertool" 2>&1 | head -n 3 || true
echo "tools ready in $OUT"
