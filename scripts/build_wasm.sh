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
  "$SP_DIR/modules/adsr.c" \
  "$SP_DIR/modules/moogladder.c" \
  -DNO_LIBSNDFILE=1 \
  -I"$ROOT_DIR/include/sp_compat" \
  -I"$SP_DIR/h" \
  -s WASM=1 \
  -s MODULARIZE=1 \
  -s EXPORT_ES6=1 \
  -s EXPORT_NAME=createSynthModule \
  -s USE_ES6_IMPORT_META=1 \
  -s ENVIRONMENT=web,worker \
  -s SINGLE_FILE=1 \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s NO_EXIT_RUNTIME=1 \
  -s EXPORTED_FUNCTIONS='["_synth_init","_synth_set_freq","_synth_set_amp","_synth_set_wave","_synth_set_wave1","_synth_set_wave2","_synth_set_detune1","_synth_set_detune2","_synth_set_gain1","_synth_set_gain2","_synth_render","_synth_note_on","_synth_note_off","_synth_note_off_midi","_synth_set_env","_synth_set_poly","_synth_filter_set","_synth_filter_env","_synth_filter_env_amount","_synth_lfo_set","_synth_lfo_amount_semi","_synth_shutdown","_malloc","_free"]' \
  -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap","HEAPF32"]' \
  -o "$OUT_DIR/synth.js"
echo "[2/2] Done. Outputs in web/dist/"
