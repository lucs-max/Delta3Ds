#!/bin/sh
# Runs the Delta3DS interpreter test suite on the desktop build.
# Usage: tests/run_tests.sh <path to delta3ds_host>
set -u
HOST=${1:-build_host/delta3ds_host}
case "$HOST" in /*) ;; *) HOST="$(pwd)/$HOST" ;; esac
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

python3 "$ROOT/tools/mkdatawin.py" "$WORK" >/dev/null || { echo "FAIL: could not build test data"; exit 1; }

fails=0
expect() { # file, text
    if grep -qF -- "$2" "$1"; then
        echo "  ok   $2"
    else
        echo "  FAIL expected: $2"
        fails=$((fails + 1))
    fi
}

echo "== interpreter test game"
(cd "$WORK" && "$HOST" "$WORK/data.win" 8) >"$WORK/out1.txt" 2>&1
expect "$WORK/out1.txt" "[game] total=6"
expect "$WORK/out1.txt" "[game] arrlen=3"
expect "$WORK/out1.txt" "[game] arr2=7"
expect "$WORK/out1.txt" "[game] sum=10"
expect "$WORK/out1.txt" "[game] upper=HELLO"
expect "$WORK/out1.txt" "[game] len=12"
expect "$WORK/out1.txt" "[game] div=3.50"
expect "$WORK/out1.txt" "[game] children=2"
expect "$WORK/out1.txt" "[game] struct.a=9"
expect "$WORK/out1.txt" "[game] child v=42"
expect "$WORK/out1.txt" "Room 1: room_b"
expect "$WORK/out1.txt" "[game] room_b reached, counter=3"
expect "$WORK/out1.txt" "frames=8 room=1"

echo "== sprite game"
(cd "$WORK" && D3_TRACE_DRAW=1 "$HOST" "$WORK/sprite_game/data.win" 2) >"$WORK/out2.txt" 2>&1
expect "$WORK/out2.txt" "[game] frames=2"
expect "$WORK/out2.txt" "image tpag=1 src=(0,0 16x16) at=(100.0,100.0) size=(32.0,16.0) pivot=(16.0,8.0)"

echo "== texture converter"
if python3 -c "import PIL, numpy" 2>/dev/null; then
    python3 "$ROOT/tools/d3ds_convert.py" "$WORK/sprite_game" >"$WORK/out3.txt" 2>&1
    expect "$WORK/out3.txt" "wrote 1 atlases"
    python3 "$ROOT/tests/check_atlas.py" "$WORK/sprite_game/d3ds" >"$WORK/out4.txt" 2>&1
    expect "$WORK/out4.txt" "atlas ok"
else
    echo "  skip (Pillow/numpy not installed)"
fi

if [ "$fails" -ne 0 ]; then
    echo "$fails test(s) failed"
    for f in "$WORK"/out*.txt; do echo "--- $f"; cat "$f"; done
    exit 1
fi
echo "all tests passed"
