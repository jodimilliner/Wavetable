let audioCtx;
let ModuleFactory; // Emscripten module factory function
let Module;        // Emscripten module instance

async function init() {
  audioCtx = new (window.AudioContext || window.webkitAudioContext)();

  // The Emscripten build uses MODULARIZE=1, so dist/synth.js exposes a factory
  ModuleFactory = window.Module || window.createSynthModule || window.synthModule;
  if (!ModuleFactory && typeof window.createModule === 'function') {
    ModuleFactory = window.createModule;
  }
  if (!ModuleFactory && typeof window.Module === 'function') {
    ModuleFactory = window.Module; // Sometimes exported as Module()
  }

  if (!ModuleFactory) {
    // When MODULARIZE=1, synth.js exports as a function assigned to global 'Module'
    // Emscripten places it in 'window' as a function.
    ModuleFactory = window.Module;
  }

  if (typeof ModuleFactory !== 'function') {
    console.error('Unable to find Emscripten module factory from dist/synth.js');
    return;
  }

  Module = await ModuleFactory();

  const sr = audioCtx.sampleRate | 0;
  Module.ccall('synth_init', 'void', ['number', 'number'], [sr, 2048]);

  const waveSel = document.getElementById('wave');
  const freq = document.getElementById('freq');
  const freqVal = document.getElementById('freqVal');
  const dur = document.getElementById('dur');
  const durVal = document.getElementById('durVal');
  const playBtn = document.getElementById('play');
  const a4Btn = document.getElementById('a4');
  const c5Btn = document.getElementById('c5');

  waveSel.addEventListener('change', () => {
    const type = parseInt(waveSel.value, 10) | 0;
    Module.ccall('synth_set_wave', 'void', ['number'], [type]);
  });

  freq.addEventListener('input', () => {
    const f = parseFloat(freq.value);
    freqVal.textContent = `${f.toFixed(0)} Hz`;
  });

  dur.addEventListener('input', () => {
    const d = parseFloat(dur.value);
    durVal.textContent = `${d.toFixed(1)} s`;
  });

  function renderTone(frequency, seconds) {
    // Set freq/amp, render N samples in one go, and play via AudioBuffer
    Module.ccall('synth_set_freq', 'void', ['number'], [frequency]);
    Module.ccall('synth_set_amp', 'void', ['number'], [0.4]);

    const frames = Math.max(1, Math.floor(seconds * audioCtx.sampleRate));
    const bytes = frames * 4; // float32
    const ptr = Module._malloc(bytes);
    try {
      // Render into WASM heap
      Module.ccall('synth_render', 'void', ['number', 'number'], [ptr, frames]);
      const heap = Module.HEAPF32.subarray(ptr >> 2, (ptr >> 2) + frames);
      // Copy out to a JS Float32Array
      const out = new Float32Array(frames);
      out.set(heap);

      // Create and play an AudioBuffer
      const buffer = audioCtx.createBuffer(1, frames, audioCtx.sampleRate);
      buffer.copyToChannel(out, 0, 0);
      const src = audioCtx.createBufferSource();
      src.buffer = buffer;
      src.connect(audioCtx.destination);
      src.start();
    } finally {
      Module._free(ptr);
    }
  }

  playBtn.addEventListener('click', async () => {
    if (audioCtx.state === 'suspended') await audioCtx.resume();
    const f = parseFloat(freq.value);
    const seconds = parseFloat(dur.value);
    renderTone(f, seconds);
  });

  a4Btn.addEventListener('click', async () => {
    if (audioCtx.state === 'suspended') await audioCtx.resume();
    const seconds = parseFloat(dur.value);
    renderTone(440.0, seconds);
  });

  c5Btn.addEventListener('click', async () => {
    if (audioCtx.state === 'suspended') await audioCtx.resume();
    const seconds = parseFloat(dur.value);
    // C5 = ~523.251
    renderTone(523.251, seconds);
  });
}

window.addEventListener('load', init);

