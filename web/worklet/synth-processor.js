/* AudioWorkletProcessor that pulls audio from the WASM synth. */
// ESM import of the Emscripten factory (built with EXPORT_ES6=1)
import createSynthModule from '../dist/synth.js';

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
            // Backwards-compat: set both oscillators' wave
            this.mod.ccall('synth_set_wave', 'void', ['number'], [m.value|0]);
            this.port.postMessage({ type: 'log', msg: `wave -> ${m.value|0}` });
            break;
          case 'osc1': {
            if (typeof m.wave === 'number') this.mod.ccall('synth_set_wave1', 'void', ['number'], [m.wave|0]);
            if (typeof m.detune === 'number') this.mod.ccall('synth_set_detune1', 'void', ['number'], [m.detune]);
            this.port.postMessage({ type: 'log', msg: `osc1 -> wave:${m.wave} det:${m.detune}` });
            break;
          }
          case 'osc2': {
            if (typeof m.wave === 'number') this.mod.ccall('synth_set_wave2', 'void', ['number'], [m.wave|0]);
            if (typeof m.detune === 'number') this.mod.ccall('synth_set_detune2', 'void', ['number'], [m.detune]);
            this.port.postMessage({ type: 'log', msg: `osc2 -> wave:${m.wave} det:${m.detune}` });
            break;
          }
          case 'note_on':
            this.mod.ccall('synth_note_on', 'void', ['number', 'number'], [m.midi|0, m.velocity ?? 1.0]);
            this.port.postMessage({ type: 'log', msg: `note_on -> midi:${m.midi|0} vel:${m.velocity??1.0}` });
            break;
          case 'note_off':
            if (typeof m.midi === 'number') {
              this.mod.ccall('synth_note_off_midi', 'void', ['number'], [m.midi|0]);
              this.port.postMessage({ type: 'log', msg: `note_off -> midi:${m.midi|0}` });
            } else {
              this.mod.ccall('synth_note_off', 'void', [], []);
              this.port.postMessage({ type: 'log', msg: 'note_off (all)' });
            }
            break;
          case 'amp':
            this.mod.ccall('synth_set_amp', 'void', ['number'], [m.value || 0]);
            this.port.postMessage({ type: 'log', msg: `amp -> ${m.value||0}` });
            break;
          case 'filter': {
            const cutoff = +m.cutoff || 0;
            let res = +m.resonance; if (!Number.isFinite(res)) res = 0;
            this.mod.ccall('synth_filter_set', 'void', ['number','number'], [cutoff, res]);
            this.port.postMessage({ type: 'log', msg: `filter -> fc:${cutoff}Hz res:${res}` });
            break;
          }
          case 'fenv': {
            const a = +m.attack||0, d = +m.decay||0, s = +m.sustain||0, r = +m.release||0;
            this.mod.ccall('synth_filter_env', 'void', ['number','number','number','number'], [a,d,s,r]);
            this.port.postMessage({ type: 'log', msg: `fenv -> a:${a} d:${d} s:${s} r:${r}` });
            break;
          }
          case 'famt': {
            const amt = +m.amount||0;
            this.mod.ccall('synth_filter_env_amount', 'void', ['number'], [amt]);
            this.port.postMessage({ type: 'log', msg: `famt -> ${amt} Hz` });
            break;
          }
          case 'lfo': {
            if (typeof m.rate === 'number') {
              this.mod.ccall('synth_lfo_set', 'void', ['number'], [m.rate]);
            }
            if (typeof m.amount === 'number') {
              this.mod.ccall('synth_lfo_amount_semi', 'void', ['number'], [m.amount]);
            }
            this.port.postMessage({ type: 'log', msg: `lfo -> rate:${m.rate}Hz amt:${m.amount} semi` });
            break;
          }
          case 'env': {
            const a = +m.attack||0, d = +m.decay||0, s = +m.sustain||0, r = +m.release||0;
            this.mod.ccall('synth_set_env', 'void', ['number','number','number','number'], [a,d,s,r]);
            this.port.postMessage({ type: 'log', msg: `env -> a:${a} d:${d} s:${s} r:${r}` });
            break;
          }
          case 'poly':
            this.mod.ccall('synth_set_poly', 'void', ['number'], [m.value|0]);
            this.port.postMessage({ type: 'log', msg: `poly -> ${m.value|0}` });
            break;
        }
      } catch (e) {
        // Avoid throwing in the audio thread
      }
    };

    // Instantiate the WASM module inside the worklet
    // Ensure the .wasm path is resolved relative to dist/
    const opts = {};
    createSynthModule(opts).then((mod) => {
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
