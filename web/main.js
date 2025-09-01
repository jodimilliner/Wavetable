let audioCtx;
let node;

async function init() {
  audioCtx = new (window.AudioContext || window.webkitAudioContext)();
  const status = document.getElementById('status');
  const logEl = document.getElementById('log');
  function log(msg) {
    console.log('[Synth]', msg);
    if (!logEl) return;
    const time = new Date().toLocaleTimeString();
    const line = document.createElement('div');
    line.textContent = `${time} ${msg}`;
    logEl.appendChild(line);
    logEl.scrollTop = logEl.scrollHeight;
  }
  function setStatus(extra = '') {
    if (!status) return;
    status.textContent = `AudioContext: ${audioCtx.state} @ ${audioCtx.sampleRate} Hz ${extra}`;
  }
  setStatus();
  audioCtx.addEventListener('statechange', () => setStatus());

  try {
    await audioCtx.audioWorklet.addModule('worklet/synth-processor.js');
    node = new AudioWorkletNode(audioCtx, 'synth-processor');
    node.connect(audioCtx.destination);
  } catch (e) {
    log('Failed to load worklet: ' + (e && e.message ? e.message : e));
    setStatus(' | worklet load failed');
    return;
  }
  node.connect(audioCtx.destination);

  // Messages from worklet (logs/metrics)
  node.port.onmessage = (ev) => {
    const m = ev.data || {};
    if (m.type === 'ready') {
      log(`Worklet ready, sr=${m.sr}`);
    } else if (m.type === 'log') {
      log(m.msg);
    } else if (m.type === 'error') {
      log(`ERROR: ${m.msg}`);
    } else if (m.type === 'metrics') {
      const { rms, frames } = m;
      setStatus(`| frames=${frames} rms=${rms.toFixed(4)}`);
    }
  };

  // UI controls - Oscillators
  const wave1 = document.getElementById('wave1');
  const wave2 = document.getElementById('wave2');
  const det1 = document.getElementById('det1');
  const det2 = document.getElementById('det2');
  const gain1 = document.getElementById('gain1');
  const gain2 = document.getElementById('gain2');
  const fm1car = document.getElementById('fm1car');
  const fm1mod = document.getElementById('fm1mod');
  const fm1idx = document.getElementById('fm1idx');
  const fm2car = document.getElementById('fm2car');
  const fm2mod = document.getElementById('fm2mod');
  const fm2idx = document.getElementById('fm2idx');
  const presetSelect = document.getElementById('presetSelect');
  const savePresetBtn = document.getElementById('savePreset');
  function sendOsc1() {
    const w = wave1 ? (parseInt(wave1.value,10)|0) : 0;
    const d = det1 ? (+det1.value) : 0;
    const g = gain1 ? (+gain1.value) : undefined;
    const fm_car = fm1car ? (+fm1car.value) : undefined;
    const fm_mod = fm1mod ? (+fm1mod.value) : undefined;
    const fm_indx = fm1idx ? (+fm1idx.value) : undefined;
    node.port.postMessage({ type: 'osc1', wave: w, detune: d, gain: g, fm_car, fm_mod, fm_indx });
    const dv = document.getElementById('det1Val'); if (dv) dv.textContent = `${d.toFixed(2)} st`;
    const gv = document.getElementById('gain1Val'); if (gv && gain1) gv.textContent = `${(+gain1.value).toFixed(2)}`;
    const set = (id, val, suf='') => { const el = document.getElementById(id); if (el) el.textContent = `${val.toFixed(2)}${suf}`; };
    if (fm1car) set('fm1carVal', +fm1car.value);
    if (fm1mod) set('fm1modVal', +fm1mod.value);
    if (fm1idx) set('fm1idxVal', +fm1idx.value);
  }
  function sendOsc2() {
    const w = wave2 ? (parseInt(wave2.value,10)|0) : 0;
    const d = det2 ? (+det2.value) : 0;
    const g = gain2 ? (+gain2.value) : undefined;
    const fm_car = fm2car ? (+fm2car.value) : undefined;
    const fm_mod = fm2mod ? (+fm2mod.value) : undefined;
    const fm_indx = fm2idx ? (+fm2idx.value) : undefined;
    node.port.postMessage({ type: 'osc2', wave: w, detune: d, gain: g, fm_car, fm_mod, fm_indx });
    const dv = document.getElementById('det2Val'); if (dv) dv.textContent = `${d.toFixed(2)} st`;
    const gv = document.getElementById('gain2Val'); if (gv && gain2) gv.textContent = `${(+gain2.value).toFixed(2)}`;
    const set = (id, val, suf='') => { const el = document.getElementById(id); if (el) el.textContent = `${val.toFixed(2)}${suf}`; };
    if (fm2car) set('fm2carVal', +fm2car.value);
    if (fm2mod) set('fm2modVal', +fm2mod.value);
    if (fm2idx) set('fm2idxVal', +fm2idx.value);
  }
  if (wave1) wave1.addEventListener('change', sendOsc1);
  if (wave2) wave2.addEventListener('change', sendOsc2);
  if (det1) det1.addEventListener('input', sendOsc1);
  if (det2) det2.addEventListener('input', sendOsc2);

  // Toggle FM controls visibility based on wave selections
  function updateFmVisibility() {
    const w1 = wave1 ? (parseInt(wave1.value,10)|0) : 0;
    const w2 = wave2 ? (parseInt(wave2.value,10)|0) : 0;
    document.querySelectorAll('.fm1').forEach(el => { el.style.display = (w1 === 4 ? 'flex' : 'none');});
    document.querySelectorAll('.fm2').forEach(el => { el.style.display = (w2 === 4 ? 'flex' : 'none');});
  }
  if (wave1) wave1.addEventListener('change', updateFmVisibility);
  if (wave2) wave2.addEventListener('change', updateFmVisibility);
  updateFmVisibility();
  if (gain1) gain1.addEventListener('input', sendOsc1);
  if (gain2) gain2.addEventListener('input', sendOsc2);
  if (fm1car) fm1car.addEventListener('input', sendOsc1);
  if (fm1mod) fm1mod.addEventListener('input', sendOsc1);
  if (fm1idx) fm1idx.addEventListener('input', sendOsc1);
  if (fm2car) fm2car.addEventListener('input', sendOsc2);
  if (fm2mod) fm2mod.addEventListener('input', sendOsc2);
  if (fm2idx) fm2idx.addEventListener('input', sendOsc2);

  // Piano keyboard mapping starting at 'A' for C4: A W S E D F T G Y H U J K
  const KEY_TO_MIDI = { 'a':60,'w':61,'s':62,'e':63,'d':64,'f':65,'t':66,'g':67,'y':68,'h':69,'u':70,'j':71,'k':72 };

  const pressed = new Set();

  function noteOn(midi, velocity = 1.0) {
    node.port.postMessage({ type: 'note_on', midi, velocity });
  }
  function noteOff(midi) {
    node.port.postMessage({ type: 'note_off', midi });
  }

  async function ensureRunning() {
    if (audioCtx.state === 'suspended') await audioCtx.resume();
  }

  document.addEventListener('keydown', async (ev) => {
    const key = (ev.key || '').toLowerCase();
    if (!(key in KEY_TO_MIDI)) return;
    ev.preventDefault();
    if (pressed.has(key)) return; // ignore repeats
    pressed.add(key);
    const midi = KEY_TO_MIDI[key];
    await ensureRunning();
    noteOn(midi, 1.0);
    // highlight visual key
    const el = document.querySelector(`.key[data-key="${CSS.escape(key)}"]`);
    if (el) el.classList.add('down');
  });

  document.addEventListener('keyup', (ev) => {
    const key = (ev.key || '').toLowerCase();
    if (!(key in KEY_TO_MIDI)) return;
    ev.preventDefault();
    if (!pressed.has(key)) return;
    pressed.delete(key);
    const midi = KEY_TO_MIDI[key];
    noteOff(midi);
    // unhighlight visual key
    const el = document.querySelector(`.key[data-key="${CSS.escape(key)}"]`);
    if (el) el.classList.remove('down');
  });

  // Mouse interaction with on-screen keys (Prophet-style keyboard)
  document.addEventListener('mousedown', async (ev) => {
    const el = ev.target.closest('.white.key, .black.key');
    if (!el) return;
    const midi = parseInt(el.getAttribute('data-midi'), 10) | 0;
    el.classList.add('down');
    await ensureRunning();
    noteOn(midi, 1.0);
    const up = () => {
      el.classList.remove('down');
      noteOff(midi);
      window.removeEventListener('mouseup', up);
    };
    window.addEventListener('mouseup', up);
  });

  // Envelope controls
  const atk = document.getElementById('atk');
  const dec = document.getElementById('dec');
  const sus = document.getElementById('sus');
  const rel = document.getElementById('rel');
  function sendEnv() {
    if (!atk || !dec || !sus || !rel) return;
    node.port.postMessage({ type: 'env', attack: +atk.value, decay: +dec.value, sustain: +sus.value, release: +rel.value });
  }
  [atk, dec, sus, rel].forEach(el => el && el.addEventListener('input', sendEnv));

  // Polyphony control
  const poly = document.getElementById('poly');
  if (poly) poly.addEventListener('input', () => {
    const n = parseInt(poly.value,10)|0;
    node.port.postMessage({ type: 'poly', value: n });
    const pv = document.getElementById('polyVal');
    if (pv) pv.textContent = `${n} voices`;
  });

  // Master gain control
  const master = document.getElementById('master');
  if (master) master.addEventListener('input', () => {
    const val = +master.value;
    node.port.postMessage({ type: 'amp', value: val });
    const mv = document.getElementById('masterVal'); if (mv) mv.textContent = `${val.toFixed(2)}`;
  });

  // Filter controls
  const fc = document.getElementById('fc');
  const res = document.getElementById('res');
  const famt = document.getElementById('famt');
  function sendFilter() {
    if (!fc || !res) return;
    node.port.postMessage({ type: 'filter', cutoff: +fc.value, resonance: +res.value });
    const fcVal = document.getElementById('fcVal');
    if (fcVal) fcVal.textContent = `${(+fc.value/1000).toFixed(2)} kHz`;
    const resVal = document.getElementById('resVal');
    if (resVal) resVal.textContent = (+res.value).toFixed(2);
  }
  function sendFamt() { if (famt) node.port.postMessage({ type: 'famt', amount: +famt.value }); }
  if (famt) famt.addEventListener('input', () => {
    sendFamt();
    const famtVal = document.getElementById('famtVal');
    if (famtVal) famtVal.textContent = `${+famt.value >= 0 ? '+' : ''}${(+famt.value).toFixed(0)} Hz`;
  });
  [fc, res].forEach(el => el && el.addEventListener('input', sendFilter));

  // Filter ADSR controls
  const fatk = document.getElementById('fatk');
  const fdec = document.getElementById('fdec');
  const fsus = document.getElementById('fsus');
  const frel = document.getElementById('frel');
  function sendFenv() {
    if (!fatk || !fdec || !fsus || !frel) return;
    node.port.postMessage({ type: 'fenv', attack: +fatk.value, decay: +fdec.value, sustain: +fsus.value, release: +frel.value });
    const set = (id, text) => { const el = document.getElementById(id); if (el) el.textContent = text; };
    set('fatkVal', `${(+fatk.value).toFixed(3)} s`);
    set('fdecVal', `${(+fdec.value).toFixed(3)} s`);
    set('fsusVal', `${(+fsus.value).toFixed(2)}`);
    set('frelVal', `${(+frel.value).toFixed(3)} s`);
  }
  [fatk, fdec, fsus, frel].forEach(el => el && el.addEventListener('input', sendFenv));

  // LFO (pitch) controls
  const lfor = document.getElementById('lforate');
  const lfoa = document.getElementById('lfoamnt');
  const lfod = document.getElementById('lfodest');
  function updateLfoRange() {
    if (!lfoa || !lfod) return;
    const dest = parseInt(lfod.value, 10) | 0;
    // Adjust amount slider range/step by destination for usable resolution
    switch (dest) {
      case 0: // pitch (semitones)
        lfoa.min = '-24'; lfoa.max = '24'; lfoa.step = '0.1';
        break;
      case 1: // cutoff (Hz)
        lfoa.min = '-8000'; lfoa.max = '8000'; lfoa.step = '1';
        break;
      case 2: // master gain
      case 4: // osc1 gain
      case 5: // osc2 gain
        lfoa.min = '-1'; lfoa.max = '1'; lfoa.step = '0.01';
        break;
      case 3: // resonance
        lfoa.min = '-1'; lfoa.max = '1'; lfoa.step = '0.01';
        break;
      case 6: // fm1 index
      case 7: // fm2 index
        lfoa.min = '-10'; lfoa.max = '10'; lfoa.step = '0.05';
        break;
      default:
        lfoa.min = '-12'; lfoa.max = '12'; lfoa.step = '0.1';
    }
    // Ensure current value is within new range
    let v = parseFloat(lfoa.value);
    const min = parseFloat(lfoa.min), max = parseFloat(lfoa.max);
    if (v < min) v = min; if (v > max) v = max;
    lfoa.value = String(v);
  }
  function sendLfo() {
    const dest = lfod ? (parseInt(lfod.value,10)|0) : 0;
    node.port.postMessage({ type: 'lfo', rate: +lfor.value, amount: +lfoa.value, dest });
    const rv = document.getElementById('lforVal'); if (rv) rv.textContent = `${(+lfor.value).toFixed(2)} Hz`;
    const av = document.getElementById('lfoaVal');
    if (av) {
      let unit = 'st';
      if (dest === 1) unit = 'Hz';
      else if (dest === 2 || dest === 4 || dest === 5) unit = '';
      else if (dest === 3) unit = '';
      else if (dest === 6 || dest === 7) unit = ' idx';
      av.textContent = `${(+lfoa.value).toFixed(2)} ${unit}`.trim();
    }
  }
  [lfor, lfoa].forEach(el => el && el.addEventListener('input', sendLfo));
  if (lfod) lfod.addEventListener('change', () => { updateLfoRange(); sendLfo(); });
  // Initialize range based on default destination
  updateLfoRange();

  // Amp ADSR readouts
  function sendEnvAndUpdate() {
    sendEnv();
    const set = (id, text) => { const el = document.getElementById(id); if (el) el.textContent = text; };
    set('atkVal', `${(+atk.value).toFixed(3)} s`);
    set('decVal', `${(+dec.value).toFixed(3)} s`);
    set('susVal', `${(+sus.value).toFixed(2)}`);
    set('relVal', `${(+rel.value).toFixed(3)} s`);
  }
  [atk, dec, sus, rel].forEach(el => el && el.addEventListener('input', sendEnvAndUpdate));

  // Explicit start button for autoplay policies
  const startBtn = document.getElementById('start');
  if (startBtn) {
    startBtn.addEventListener('click', async () => {
      await audioCtx.resume();
      setStatus();
      log('AudioContext resumed via Start button');
    });
  }

  const testBtn = document.getElementById('test');
  if (testBtn) {
    testBtn.addEventListener('click', async () => {
      await audioCtx.resume();
      setStatus();
      log('Test A4');
      node.port.postMessage({ type: 'wave', value: parseInt(document.getElementById('wave').value, 10) | 0 });
      node.port.postMessage({ type: 'note_on', midi: 69, velocity: 1.0 });
      setTimeout(() => node.port.postMessage({ type: 'note_off', midi: 69 }), 500);
    });
  }
}

window.addEventListener('load', init);
