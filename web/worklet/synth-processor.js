/* AudioWorkletProcessor that pulls audio from the WASM synth. */

// Load the Emscripten JS glue in the worklet global scope
// dist/ is one level up from worklet/
importScripts('../dist/synth.js');

// With EXPORT_NAME=createSynthModule we get a global factory function
// named createSynthModule in this scope after importScripts.

class SynthProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.ready = false;
    this.mod = null;
    this.ptr = 0;
    this.ptrCapacity = 0;
    this.processCount = 0;

    // Handle messages from main thread (UI)
    this.port.onmessage = (ev) => {
      const m = ev.data || {};
      if (!this.mod || !this.ready) return;
      try {
        switch (m.type) {
          case 'wave':
            this.mod.ccall('synth_set_wave', 'void', ['number'], [m.value|0]);
            this.port.postMessage({ type: 'log', msg: `wave -> ${m.value|0}` });
            break;
          case 'note_on':
            this.mod.ccall('synth_note_on', 'void', ['number', 'number'], [m.midi|0, m.velocity ?? 1.0]);
            this.port.postMessage({ type: 'log', msg: `note_on -> midi:${m.midi|0} vel:${m.velocity??1.0}` });
            break;
          case 'note_off':
            this.mod.ccall('synth_note_off', 'void', [], []);
            this.port.postMessage({ type: 'log', msg: 'note_off' });
            break;
          case 'amp':
            this.mod.ccall('synth_set_amp', 'void', ['number'], [m.value || 0]);
            this.port.postMessage({ type: 'log', msg: `amp -> ${m.value||0}` });
            break;
        }
      } catch (e) {
        // Avoid throwing in the audio thread
      }
    };

    // Instantiate the WASM module inside the worklet
    // Ensure the .wasm path is resolved relative to dist/
    const opts = {
      locateFile: (path) => path.endsWith('.wasm') ? `../dist/${path}` : path
    };
    // eslint-disable-next-line no-undef
    const ready = (typeof createSynthModule === 'function')
      ? createSynthModule(opts)
      : (typeof Module === 'function' ? Module(opts) : Promise.reject(new Error('No module factory')));

    ready.then((mod) => {
      this.mod = mod;
      const sr = sampleRate | 0; // global in AW scope
      this.mod.ccall('synth_init', 'void', ['number', 'number'], [sr, 2048]);
      this.ptrCapacity = 2048; // frames
      this.ptr = this.mod._malloc(this.ptrCapacity * 4);
      this.ready = true;
      this.port.postMessage({ type: 'ready', sr });
    }).catch(() => {
      // stay silent on failure
      this.ready = false;
      this.port.postMessage({ type: 'error', msg: 'Failed to init WASM in worklet' });
    });
  }

  process(inputs, outputs) {
    const output = outputs[0];
    const frames = output[0].length;

    if (!this.ready || !this.mod) {
      for (let ch = 0; ch < output.length; ch++) output[ch].fill(0);
      return true;
    }

    if (frames > this.ptrCapacity) {
      if (this.ptr) this.mod._free(this.ptr);
      this.ptrCapacity = frames;
      this.ptr = this.mod._malloc(this.ptrCapacity * 4);
    }

    // Render mono block into WASM heap
    this.mod.ccall('synth_render', 'void', ['number', 'number'], [this.ptr, frames]);
    const start = this.ptr >> 2;
    const heap = this.mod.HEAPF32.subarray(start, start + frames);

    // Copy mono to all output channels
    for (let i = 0; i < frames; i++) {
      const s = heap[i];
      for (let ch = 0; ch < output.length; ch++) {
        output[ch][i] = s;
      }
    }

    // lightweight diagnostics (rms) every ~8 callbacks
    if ((this.processCount++ & 7) === 0) {
      let sum = 0;
      for (let i = 0; i < frames; i++) { const v = heap[i]; sum += v*v; }
      const rms = Math.sqrt(sum / frames);
      this.port.postMessage({ type: 'metrics', frames, rms });
    }
    return true;
  }
}

registerProcessor('synth-processor', SynthProcessor);
