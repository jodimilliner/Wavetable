import { createWasmDSPNode } from './player.js';

const template = document.createElement('template');
template.innerHTML = `
  <style>
    :host { display: inline-flex; gap: .75rem; align-items: center; font: 14px/1.2 system-ui, -apple-system, Segoe UI, Roboto, Helvetica, Arial, sans-serif; }
    button, input[type="range"] { cursor: pointer; }
    .meter { min-width: 3ch; text-align: right; }
  </style>
  <button part="start">Start</button>
  <label part="label">Gain
    <input part="gain" type="range" min="0" max="4" step="0.01" value="1">
  </label>
  <span class="meter" part="value">1.00</span>
`;

export class WasmDSPElement extends HTMLElement {
  static get observedAttributes() { return ['autostart', 'gain']; }

  constructor() {
    super();
    this.attachShadow({ mode: 'open' }).appendChild(template.content.cloneNode(true));
    this.$start = this.shadowRoot.querySelector('button');
    this.$gain  = this.shadowRoot.querySelector('input[type="range"]');
    this.$val   = this.shadowRoot.querySelector('.meter');

    this.ctx = null;
    this.node = null;

    this.handleStart = this.handleStart.bind(this);
    this.handleGain  = this.handleGain.bind(this);
  }

  connectedCallback() {
    this.$start.addEventListener('click', this.handleStart);
    this.$gain.addEventListener('input', this.handleGain);

    if (this.hasAttribute('gain')) this.$gain.value = this.getAttribute('gain');
    if (this.$gain) this.$val.textContent = (+this.$gain.value).toFixed(2);

    if (this.hasAttribute('autostart')) {
      this.addEventListener('click', this.handleStart, { once: true });
    }
  }

  disconnectedCallback() {
    this.$start.removeEventListener('click', this.handleStart);
    this.$gain.removeEventListener('input', this.handleGain);
    this.stop();
  }

  attributeChangedCallback(name, _old, val) {
    if (!this.node) return;
    if (name === 'gain' && val != null) {
      this.$gain.value = val;
      this.handleGain();
    }
  }

  async handleStart() {
    if (this.ctx) return;
    this.ctx = new (window.AudioContext || window.webkitAudioContext)();
    await this.ctx.resume();
    this.node = await createWasmDSPNode(this.ctx);
    this.handleGain();
  }

  handleGain() {
    if (!this.node || !this.$gain) return;
    const g = parseFloat(this.$gain.value);
    this.$val.textContent = g.toFixed(2);
    this.node.parameters.get('gain').setValueAtTime(g, this.ctx.currentTime);
    this.setAttribute('gain', String(g));
  }

  stop() {
    try { this.node?.disconnect(); } catch {}
    try { this.ctx?.close(); } catch {}
    this.node = null; this.ctx = null;
  }
}

customElements.define('wasm-dsp', WasmDSPElement);

