#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SP_DIR="$ROOT_DIR/deps/soundpipe"
OUT_DIR="$ROOT_DIR/web/dist"

mkdir -p "$OUT_DIR"

echo "[1/2] Building WASM (synth.js/wasm)"
emcc \
  -O3 \
  "$ROOT_DIR/src/wavetable_synth.cpp" \
  "$SP_DIR/modules/base.c" \
  "$SP_DIR/modules/ftbl.c" \
  "$SP_DIR/modules/randmt.c" \
  "$SP_DIR/modules/osc.c" \
  -DNO_LIBSNDFILE=1 \
  -I"$ROOT_DIR/include/sp_compat" \
  -I"$SP_DIR/h" \
  -s WASM=1 \
  -s MODULARIZE=1 \
  -s EXPORT_NAME=createSynthModule \
  -s ENVIRONMENT=web \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s NO_EXIT_RUNTIME=1 \
  -s EXPORTED_FUNCTIONS='["_synth_init","_synth_set_freq","_synth_set_amp","_synth_set_wave","_synth_render","_synth_note_on","_synth_note_off","_synth_shutdown","_malloc","_free"]' \
  -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap","HEAPF32"]' \
  -o "$OUT_DIR/synth.js"
echo "[2/2] Done. Outputs in web/dist/"
