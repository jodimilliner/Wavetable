#include <cmath>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "../deps/soundpipe/h/base.h"
#include "../deps/soundpipe/h/ftbl.h"
#include "../deps/soundpipe/h/osc.h"
#include "../deps/soundpipe/h/adsr.h"
#include "../deps/soundpipe/h/moogladder.h"
#include "../deps/soundpipe/h/fosc.h"
}

#include "wavetable_synth.h"

// Minimal Soundpipe state for a single-voice wavetable synth
namespace {
    sp_data* g_sp = nullptr;
    sp_ftbl* g_ft1 = nullptr;
    sp_ftbl* g_ft2 = nullptr;
    int g_table_size = 2048;
    float g_master_amp = 0.4f;
    float g_env_atk = 0.01f, g_env_dec = 0.1f, g_env_sus = 0.8f, g_env_rel = 0.2f;

    // Filter parameters (applied per-voice; each voice has its own filter + env)
    float g_fcut = 1200.0f;  // Hz
    float g_fres = 0.3f;     // 0..1
    float g_fenv_atk = 0.005f, g_fenv_dec = 0.15f, g_fenv_sus = 0.0f, g_fenv_rel = 0.25f;
    float g_fenv_amt = 2000.0f; // Hz added to cutoff when filter env=1

    // Master pitch LFO (sine)
    sp_ftbl* g_lfo_ft = nullptr;
    sp_osc*  g_lfo = nullptr;
    float g_lfo_rate = 5.0f;      // Hz
    float g_lfo_amt_semi = 0.0f;  // semitones peak (±)
    // Flexible LFO routing
    int   g_lfo_dest = 0;         // 0=pitch,1=cutoff,2=masterAmp,3=res,4=osc1Gain,5=osc2Gain,6=fm1Index,7=fm2Index
    float g_lfo_amt = 0.0f;       // generic amount; units depend on destination

    int g_wave1 = 0;
    int g_wave2 = 0;
    float g_detune1 = 0.0f;
    float g_detune2 = 0.0f;
    float g_gain1 = 0.5f;
    float g_gain2 = 0.5f;
    // FM defaults per oscillator
    float g_fm1_car = 1.0f, g_fm1_mod = 1.0f, g_fm1_indx = 2.0f;
    float g_fm2_car = 1.0f, g_fm2_mod = 1.0f, g_fm2_indx = 2.0f;

    struct Voice {
        sp_osc* osc1 = nullptr;
        sp_osc* osc2 = nullptr;
        sp_fosc* fosc1 = nullptr;
        sp_fosc* fosc2 = nullptr;
        sp_adsr* env = nullptr;
        sp_moogladder* vcf = nullptr;
        sp_adsr* fenv = nullptr;
        int midi = -1;
        float base_hz = 440.0f;
        float vel = 0.0f;
        float gate = 0.0f; // 1 = on, 0 = off
        bool active = false;
    };

    constexpr int MAX_VOICES = 16;
    int g_poly_n = 8;
    Voice g_voices[MAX_VOICES];
    int g_voice_rr = 0; // round-robin index for stealing

    void ensure_sp() {
        if (!g_sp) {
            sp_create(&g_sp);
        }
    }

    void rebuild_table_sine(sp_ftbl** pft) {
        if (!g_sp) return;
        if (!*pft) sp_ftbl_create(g_sp, pft, g_table_size);
        sp_gen_sine(g_sp, *pft);
    }

    // Build a harmonic series string like: "1 0.5 0.3333 ..." up to N partials
    std::string harmonic_series(unsigned int partials, bool odd_only, bool square) {
        std::string s;
        s.reserve(partials * 6);
        for (unsigned int n = 1, added = 0; added < partials; ++n) {
            if (odd_only && (n % 2 == 0)) continue; // skip even harmonics for square
            float amp = square ? (1.0f / static_cast<float>(n)) : (1.0f / static_cast<float>(n));
            // For saw: 1/n; for square (odd only): 1/n as well; sign handled by table phase
            // Note: Soundpipe sinesum accepts positive amplitudes.
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%g ", amp);
            s += buf;
            ++added;
        }
        if (!s.empty()) s.pop_back(); // remove trailing space
        return s;
    }

    void rebuild_table_saw(sp_ftbl** pft, unsigned int partials = 32) {
        if (!g_sp) return;
        if (!*pft) sp_ftbl_create(g_sp, pft, g_table_size);
        auto coeffs = harmonic_series(partials, /*odd_only*/false, /*square*/false);
        sp_gen_sinesum(g_sp, *pft, coeffs.c_str());
    }

    void rebuild_table_square(sp_ftbl** pft, unsigned int partials = 32) {
        if (!g_sp) return;
        if (!*pft) sp_ftbl_create(g_sp, pft, g_table_size);
        auto coeffs = harmonic_series(partials, /*odd_only*/true, /*square*/true);
        sp_gen_sinesum(g_sp, *pft, coeffs.c_str());
    }

    void rebuild_table_triangle(sp_ftbl** pft) {
        if (!g_sp) return;
        if (!*pft) sp_ftbl_create(g_sp, pft, g_table_size);
        sp_gen_triangle(g_sp, *pft);
    }

    void init_voices_if_needed() {
        if (!g_sp) return;
        if (!g_ft1) rebuild_table_sine(&g_ft1);
        if (!g_ft2) rebuild_table_sine(&g_ft2);
        for (int i = 0; i < g_poly_n; ++i) {
            if (!g_voices[i].osc1) {
                sp_osc_create(&g_voices[i].osc1);
                sp_osc_init(g_sp, g_voices[i].osc1, g_ft1, 0);
                g_voices[i].osc1->amp = 1.0f;
                g_voices[i].osc1->freq = 440.0f;
            }
            if (!g_voices[i].osc2) {
                sp_osc_create(&g_voices[i].osc2);
                sp_osc_init(g_sp, g_voices[i].osc2, g_ft2, 0);
                g_voices[i].osc2->amp = 1.0f;
                g_voices[i].osc2->freq = 440.0f;
            }
            if (!g_voices[i].env) {
                sp_adsr_create(&g_voices[i].env);
                sp_adsr_init(g_sp, g_voices[i].env);
            }
            if (!g_voices[i].vcf) {
                sp_moogladder_create(&g_voices[i].vcf);
                sp_moogladder_init(g_sp, g_voices[i].vcf);
            }
            if (!g_voices[i].fenv) {
                sp_adsr_create(&g_voices[i].fenv);
                sp_adsr_init(g_sp, g_voices[i].fenv);
        }
        // FM oscillators
        if (!g_voices[i].fosc1) {
            sp_fosc_create(&g_voices[i].fosc1);
            // ensure sine ft exists for FM
            if (!g_lfo_ft) { sp_ftbl_create(g_sp, &g_lfo_ft, 2048); sp_gen_sine(g_sp, g_lfo_ft); }
            sp_fosc_init(g_sp, g_voices[i].fosc1, g_lfo_ft);
            g_voices[i].fosc1->amp = 1.0f; g_voices[i].fosc1->freq = 440.0f;
            g_voices[i].fosc1->car = g_fm1_car; g_voices[i].fosc1->mod = g_fm1_mod; g_voices[i].fosc1->indx = g_fm1_indx;
        }
        if (!g_voices[i].fosc2) {
            sp_fosc_create(&g_voices[i].fosc2);
            if (!g_lfo_ft) { sp_ftbl_create(g_sp, &g_lfo_ft, 2048); sp_gen_sine(g_sp, g_lfo_ft); }
            sp_fosc_init(g_sp, g_voices[i].fosc2, g_lfo_ft);
            g_voices[i].fosc2->amp = 1.0f; g_voices[i].fosc2->freq = 440.0f;
            g_voices[i].fosc2->car = g_fm2_car; g_voices[i].fosc2->mod = g_fm2_mod; g_voices[i].fosc2->indx = g_fm2_indx;
        }
        // Sync env params
        g_voices[i].env->atk = g_env_atk;
        g_voices[i].env->dec = g_env_dec;
        g_voices[i].env->sus = g_env_sus;
        g_voices[i].env->rel = g_env_rel;
        // Sync filter params
        g_voices[i].vcf->freq = g_fcut;
        g_voices[i].vcf->res = g_fres;
        g_voices[i].fenv->atk = g_fenv_atk;
        g_voices[i].fenv->dec = g_fenv_dec;
        g_voices[i].fenv->sus = g_fenv_sus;
        g_voices[i].fenv->rel = g_fenv_rel;
    }
    // LFO init
    if (!g_lfo_ft) sp_ftbl_create(g_sp, &g_lfo_ft, 2048), sp_gen_sine(g_sp, g_lfo_ft);
    if (!g_lfo) { sp_osc_create(&g_lfo); sp_osc_init(g_sp, g_lfo, g_lfo_ft, 0); g_lfo->amp = 1.0f; }
    g_lfo->freq = g_lfo_rate;
    }

    void free_all_voices() {
        for (int i = 0; i < MAX_VOICES; ++i) {
            if (g_voices[i].osc1) { sp_osc_destroy(&g_voices[i].osc1); }
            if (g_voices[i].osc2) { sp_osc_destroy(&g_voices[i].osc2); }
            if (g_voices[i].env) { sp_adsr_destroy(&g_voices[i].env); }
            if (g_voices[i].vcf) { sp_moogladder_destroy(&g_voices[i].vcf); }
            if (g_voices[i].fenv) { sp_adsr_destroy(&g_voices[i].fenv); }
            if (g_voices[i].fosc1) { sp_fosc_destroy(&g_voices[i].fosc1); }
            if (g_voices[i].fosc2) { sp_fosc_destroy(&g_voices[i].fosc2); }
            g_voices[i] = Voice{};
        }
    }

    int find_free_voice() {
        for (int i = 0; i < g_poly_n; ++i) {
            if (!g_voices[i].active && g_voices[i].gate <= 0.0f) return i;
        }
        // steal round robin
        int idx = g_voice_rr++ % g_poly_n;
        return idx;
    }
}

extern "C" {

void synth_init(int sample_rate, int table_size) {
    synth_shutdown();
    ensure_sp();
    g_sp->sr = sample_rate;
    g_table_size = table_size > 64 ? table_size : 2048;
    rebuild_table_sine(&g_ft1);
    rebuild_table_sine(&g_ft2);
    g_master_amp = 0.4f;
    g_env_atk = 0.01f; g_env_dec = 0.1f; g_env_sus = 0.8f; g_env_rel = 0.2f;
    g_poly_n = g_poly_n < 1 ? 1 : (g_poly_n > MAX_VOICES ? MAX_VOICES : g_poly_n);
    init_voices_if_needed();
}

void synth_set_freq(float freq) {
    // Set all active voices to the same freq (legacy support)
    for (int i = 0; i < g_poly_n; ++i) {
        if (g_voices[i].osc1) g_voices[i].osc1->freq = freq;
        if (g_voices[i].osc2) g_voices[i].osc2->freq = freq;
    }
}

void synth_set_amp(float amp) {
    g_master_amp = amp;
}

void synth_set_wave(int type) {
    if (!g_sp) return;
    // Backwards compatibility: set both oscillators
    synth_set_wave1(type);
    synth_set_wave2(type);
}

void synth_render(float* out_ptr, int frames) {
    if (!out_ptr || !g_sp) return;
    float zero = 0.0f, one = 1.0f;
    for (int i = 0; i < frames; ++i) {
        // Compute master pitch LFO value and factor
        float lfo_val = 0.0f;
        if (g_lfo) sp_osc_compute(g_sp, g_lfo, nullptr, &lfo_val);
        // LFO routing
        float pitch_mul = 1.0f;
        if (g_lfo_dest == 0) { // pitch (semitones)
            float semi = lfo_val * (g_lfo_amt != 0.0f ? g_lfo_amt : g_lfo_amt_semi);
            pitch_mul = powf(2.0f, semi / 12.0f);
        }
        float cutoff_lfo = (g_lfo_dest == 1) ? (g_lfo_amt * lfo_val) : 0.0f; // Hz
        float master_amp_mod = (g_lfo_dest == 2) ? (g_lfo_amt * lfo_val) : 0.0f; // linear
        float res_mod = (g_lfo_dest == 3) ? (g_lfo_amt * lfo_val) : 0.0f; // linear
        float osc1_gain_mod = (g_lfo_dest == 4) ? (g_lfo_amt * lfo_val) : 0.0f;
        float osc2_gain_mod = (g_lfo_dest == 5) ? (g_lfo_amt * lfo_val) : 0.0f;
        float fm1_idx_mod = (g_lfo_dest == 6) ? (g_lfo_amt * lfo_val) : 0.0f;
        float fm2_idx_mod = (g_lfo_dest == 7) ? (g_lfo_amt * lfo_val) : 0.0f;
        float mix = 0.0f;
        for (int v = 0; v < g_poly_n; ++v) {
            Voice &vc = g_voices[v];
            if (!vc.active && vc.gate <= 0.f) continue;
            float s1 = 0.0f, s2 = 0.0f;
            // Apply LFO and detune per oscillator
            float base = vc.base_hz > 0.f ? vc.base_hz : (vc.midi >= 0 ? sp_midi2cps((float)vc.midi) : 440.0f);
            if (vc.osc1 && g_wave1 != 4) {
                float det1 = powf(2.0f, g_detune1 / 12.0f);
                vc.osc1->freq = base * pitch_mul * det1;
                sp_osc_compute(g_sp, vc.osc1, nullptr, &s1);
            } else if (vc.fosc1 && g_wave1 == 4) {
                float det1 = powf(2.0f, g_detune1 / 12.0f);
                vc.fosc1->freq = base * pitch_mul * det1;
                sp_fosc_compute(g_sp, vc.fosc1, nullptr, &s1);
            }
            if (vc.osc2 && g_wave2 != 4) {
                float det2 = powf(2.0f, g_detune2 / 12.0f);
                vc.osc2->freq = base * pitch_mul * det2;
                sp_osc_compute(g_sp, vc.osc2, nullptr, &s2);
            } else if (vc.fosc2 && g_wave2 == 4) {
                float det2 = powf(2.0f, g_detune2 / 12.0f);
                vc.fosc2->freq = base * pitch_mul * det2;
                sp_fosc_compute(g_sp, vc.fosc2, nullptr, &s2);
            }
            float g1 = g_gain1 + osc1_gain_mod; if (g1 < 0.f) g1 = 0.f; if (g1 > 2.f) g1 = 2.f;
            float g2 = g_gain2 + osc2_gain_mod; if (g2 < 0.f) g2 = 0.f; if (g2 > 2.f) g2 = 2.f;
            // FM index modulation
            if (vc.fosc1) { float idx = g_fm1_indx + fm1_idx_mod; if (idx < 0.f) idx = 0.f; vc.fosc1->indx = idx; }
            if (vc.fosc2) { float idx = g_fm2_indx + fm2_idx_mod; if (idx < 0.f) idx = 0.f; vc.fosc2->indx = idx; }
            float s = s1 * g1 + s2 * g2;
            float gate_in = vc.gate > 0.f ? one : zero;
            // Filter envelope
            float fenv = 0.0f;
            if (vc.fenv) sp_adsr_compute(g_sp, vc.fenv, &gate_in, &fenv);
            // Modulate per-voice filter cutoff
            if (vc.vcf) {
                float cutoff = g_fcut + g_fenv_amt * fenv + cutoff_lfo;
                if (cutoff < 20.0f) cutoff = 20.0f;
                float nyq = 0.5f * (float)g_sp->sr;
                if (cutoff > nyq - 100.0f) cutoff = nyq - 100.0f;
                vc.vcf->freq = cutoff;
                float rbase = g_fres + res_mod; if (rbase < 0.f) rbase = 0.f; if (rbase > 1.f) rbase = 1.f;
                float r = rbase;
                vc.vcf->res = r;
                float fs = 0.0f;
                sp_moogladder_compute(g_sp, vc.vcf, &s, &fs);
                s = fs;
            }
            // Amplitude envelope
            float env = 0.0f;
            if (vc.env) sp_adsr_compute(g_sp, vc.env, &gate_in, &env);
            float mg = g_master_amp + master_amp_mod; if (mg < 0.f) mg = 0.f; if (mg > 2.f) mg = 2.f;
            mix += s * env * (vc.vel * mg);
            // Auto-deactivate if gate is off and env is near zero
            if (vc.gate <= 0.f && env < 1e-4f) {
                vc.active = false;
                vc.midi = -1;
                vc.vel = 0.f;
            }
        }
        out_ptr[i] = mix;
    }
}

void synth_note_on(int midi_note, float velocity) {
    ensure_sp();
    init_voices_if_needed();
    float freq = sp_midi2cps(static_cast<float>(midi_note));
    int idx = find_free_voice();
    Voice &vc = g_voices[idx];
    if (vc.osc1) vc.osc1->freq = freq;
    if (vc.osc2) vc.osc2->freq = freq;
    if (vc.fosc1) vc.fosc1->freq = freq;
    if (vc.fosc2) vc.fosc2->freq = freq;
    vc.base_hz = freq;
    // Update envelope params in case globals changed
    if (vc.env) { vc.env->atk = g_env_atk; vc.env->dec = g_env_dec; vc.env->sus = g_env_sus; vc.env->rel = g_env_rel; }
    if (vc.fenv) { vc.fenv->atk = g_fenv_atk; vc.fenv->dec = g_fenv_dec; vc.fenv->sus = g_fenv_sus; vc.fenv->rel = g_fenv_rel; }
    vc.midi = midi_note;
    vc.vel = (velocity <= 0.f ? 0.f : (velocity > 1.f ? 1.f : velocity));
    vc.gate = 1.0f;
    vc.active = true;
}

void synth_note_off_midi(int midi_note) {
    for (int i = 0; i < g_poly_n; ++i) {
        if (g_voices[i].active && g_voices[i].midi == midi_note) {
            g_voices[i].gate = 0.0f;
            // remain active until envelope releases
        }
    }
}

void synth_note_off() {
    for (int i = 0; i < g_poly_n; ++i) {
        if (g_voices[i].active) g_voices[i].gate = 0.0f;
    }
}

void synth_shutdown() {
    free_all_voices();
    if (g_ft1) { sp_ftbl_destroy(&g_ft1); g_ft1 = nullptr; }
    if (g_ft2) { sp_ftbl_destroy(&g_ft2); g_ft2 = nullptr; }
    if (g_lfo) { sp_osc_destroy(&g_lfo); g_lfo = nullptr; }
    if (g_lfo_ft) { sp_ftbl_destroy(&g_lfo_ft); g_lfo_ft = nullptr; }
    if (g_sp) {
        sp_destroy(&g_sp);
        g_sp = nullptr;
    }
}

} // extern "C"

extern "C" {
// Additional controls
void synth_set_env(float atk, float dec, float sus, float rel) {
    g_env_atk = atk; g_env_dec = dec; g_env_sus = sus; g_env_rel = rel;
    for (int i = 0; i < g_poly_n; ++i) {
        if (g_voices[i].env) {
            g_voices[i].env->atk = g_env_atk;
            g_voices[i].env->dec = g_env_dec;
            g_voices[i].env->sus = g_env_sus;
            g_voices[i].env->rel = g_env_rel;
        }
    }
}

void synth_set_poly(int n) {
    if (n < 1) n = 1; if (n > MAX_VOICES) n = MAX_VOICES;
    g_poly_n = n;
    init_voices_if_needed();
}

// LFO controls
void synth_lfo_set(float rate_hz) {
    g_lfo_rate = rate_hz;
    if (g_lfo) g_lfo->freq = g_lfo_rate;
}

void synth_lfo_amount_semi(float amt_semi) {
    g_lfo_amt_semi = amt_semi;
}

void synth_lfo_dest(int dest) { g_lfo_dest = dest; }
void synth_lfo_amount(float amount) { g_lfo_amt = amount; }

// Filter controls
void synth_filter_set(float cutoff_hz, float resonance) {
    g_fcut = cutoff_hz;
    g_fres = resonance;
    for (int i = 0; i < g_poly_n; ++i) {
        if (g_voices[i].vcf) { g_voices[i].vcf->freq = g_fcut; g_voices[i].vcf->res = g_fres; }
    }
}

void synth_filter_env(float atk, float dec, float sus, float rel) {
    g_fenv_atk = atk; g_fenv_dec = dec; g_fenv_sus = sus; g_fenv_rel = rel;
    for (int i = 0; i < g_poly_n; ++i) {
        if (g_voices[i].fenv) { g_voices[i].fenv->atk = atk; g_voices[i].fenv->dec = dec; g_voices[i].fenv->sus = sus; g_voices[i].fenv->rel = rel; }
    }
}

void synth_filter_env_amount(float amt_hz) { g_fenv_amt = amt_hz; }
}

// New oscillator controls
extern "C" {
void synth_set_wave1(int type) {
    if (!g_sp) return;
    if (type == 4) { // FM
        // ensure fm uses sine table; nothing to rebuild
    } else {
        if (g_ft1) { sp_ftbl_destroy(&g_ft1); g_ft1 = nullptr; }
        switch (type) {
            case 1: rebuild_table_saw(&g_ft1); break;
            case 2: rebuild_table_square(&g_ft1); break;
            case 3: rebuild_table_triangle(&g_ft1); break;
            case 0: default: rebuild_table_sine(&g_ft1); break;
        }
    }
    g_wave1 = type;
    for (int i = 0; i < g_poly_n; ++i) {
        if (type == 4) {
            // switch to FM: nothing to reinit here (already created), ensure sine ft exists
            if (!g_lfo_ft) { sp_ftbl_create(g_sp, &g_lfo_ft, 2048); sp_gen_sine(g_sp, g_lfo_ft); }
            if (g_voices[i].fosc1 && g_voices[i].fosc1->ft != g_lfo_ft) {
                // no direct setter; re-init
                sp_fosc_init(g_sp, g_voices[i].fosc1, g_lfo_ft);
            }
        } else if (g_voices[i].osc1) {
            float a = g_voices[i].osc1->amp, f = g_voices[i].osc1->freq;
            sp_osc_destroy(&g_voices[i].osc1);
            sp_osc_create(&g_voices[i].osc1);
            sp_osc_init(g_sp, g_voices[i].osc1, g_ft1, 0);
            g_voices[i].osc1->amp = a; g_voices[i].osc1->freq = f;
        }
    }
}
void synth_set_wave2(int type) {
    if (!g_sp) return;
    if (type == 4) {
        // FM
    } else {
        if (g_ft2) { sp_ftbl_destroy(&g_ft2); g_ft2 = nullptr; }
        switch (type) {
            case 1: rebuild_table_saw(&g_ft2); break;
            case 2: rebuild_table_square(&g_ft2); break;
            case 3: rebuild_table_triangle(&g_ft2); break;
            case 0: default: rebuild_table_sine(&g_ft2); break;
        }
    }
    g_wave2 = type;
    for (int i = 0; i < g_poly_n; ++i) {
        if (type == 4) {
            if (!g_lfo_ft) { sp_ftbl_create(g_sp, &g_lfo_ft, 2048); sp_gen_sine(g_sp, g_lfo_ft); }
            if (g_voices[i].fosc2 && g_voices[i].fosc2->ft != g_lfo_ft) {
                sp_fosc_init(g_sp, g_voices[i].fosc2, g_lfo_ft);
            }
        } else if (g_voices[i].osc2) {
            float a = g_voices[i].osc2->amp, f = g_voices[i].osc2->freq;
            sp_osc_destroy(&g_voices[i].osc2);
            sp_osc_create(&g_voices[i].osc2);
            sp_osc_init(g_sp, g_voices[i].osc2, g_ft2, 0);
            g_voices[i].osc2->amp = a; g_voices[i].osc2->freq = f;
        }
    }
}
void synth_set_detune1(float semi) { g_detune1 = semi; }
void synth_set_detune2(float semi) { g_detune2 = semi; }
void synth_set_gain1(float g) { g_gain1 = g; }
void synth_set_gain2(float g) { g_gain2 = g; }

// FM parameter setters (per-oscillator)
void synth_fm1(float car, float mod, float indx) {
    g_fm1_car = car; g_fm1_mod = mod; g_fm1_indx = indx;
    for (int i = 0; i < g_poly_n; ++i) if (g_voices[i].fosc1) {
        g_voices[i].fosc1->car = g_fm1_car;
        g_voices[i].fosc1->mod = g_fm1_mod;
        g_voices[i].fosc1->indx = g_fm1_indx;
    }
}
void synth_fm2(float car, float mod, float indx) {
    g_fm2_car = car; g_fm2_mod = mod; g_fm2_indx = indx;
    for (int i = 0; i < g_poly_n; ++i) if (g_voices[i].fosc2) {
        g_voices[i].fosc2->car = g_fm2_car;
        g_voices[i].fosc2->mod = g_fm2_mod;
        g_voices[i].fosc2->indx = g_fm2_indx;
    }
}
}
