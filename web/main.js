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

  // UI controls
  const waveSel = document.getElementById('wave');
  waveSel.addEventListener('change', () => {
    node.port.postMessage({ type: 'wave', value: parseInt(waveSel.value, 10) | 0 });
  });

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
  function sendLfo() {
    node.port.postMessage({ type: 'lfo', rate: +lfor.value, amount: +lfoa.value });
    const rv = document.getElementById('lforVal'); if (rv) rv.textContent = `${(+lfor.value).toFixed(2)} Hz`;
    const av = document.getElementById('lfoaVal'); if (av) av.textContent = `${(+lfoa.value).toFixed(1)} st`;
  }
  [lfor, lfoa].forEach(el => el && el.addEventListener('input', sendLfo));

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
