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
}

#include "wavetable_synth.h"

// Minimal Soundpipe state for a single-voice wavetable synth
namespace {
    sp_data* g_sp = nullptr;
    sp_ftbl* g_ft = nullptr;
    int g_table_size = 2048;
    float g_master_amp = 0.4f;
    float g_env_atk = 0.01f, g_env_dec = 0.1f, g_env_sus = 0.8f, g_env_rel = 0.2f;

    // Filter parameters (applied per-voice; each voice has its own filter + env)
    float g_fcut = 1200.0f;  // Hz
    float g_fres = 0.3f;     // 0..1
    float g_fenv_atk = 0.005f, g_fenv_dec = 0.15f, g_fenv_sus = 0.0f, g_fenv_rel = 0.25f;
    float g_fenv_amt = 2000.0f; // Hz added to cutoff when filter env=1

    struct Voice {
        sp_osc* osc = nullptr;
        sp_adsr* env = nullptr;
        sp_moogladder* vcf = nullptr;
        sp_adsr* fenv = nullptr;
        int midi = -1;
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

    void rebuild_table_sine() {
        if (!g_sp) return;
        if (!g_ft) sp_ftbl_create(g_sp, &g_ft, g_table_size);
        sp_gen_sine(g_sp, g_ft);
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

    void rebuild_table_saw(unsigned int partials = 32) {
        if (!g_sp) return;
        if (!g_ft) sp_ftbl_create(g_sp, &g_ft, g_table_size);
        auto coeffs = harmonic_series(partials, /*odd_only*/false, /*square*/false);
        sp_gen_sinesum(g_sp, g_ft, coeffs.c_str());
    }

    void rebuild_table_square(unsigned int partials = 32) {
        if (!g_sp) return;
        if (!g_ft) sp_ftbl_create(g_sp, &g_ft, g_table_size);
        auto coeffs = harmonic_series(partials, /*odd_only*/true, /*square*/true);
        sp_gen_sinesum(g_sp, g_ft, coeffs.c_str());
    }

    void rebuild_table_triangle() {
        if (!g_sp) return;
        if (!g_ft) sp_ftbl_create(g_sp, &g_ft, g_table_size);
        sp_gen_triangle(g_sp, g_ft);
    }

    void init_voices_if_needed() {
        if (!g_sp || !g_ft) return;
        for (int i = 0; i < g_poly_n; ++i) {
            if (!g_voices[i].osc) {
                sp_osc_create(&g_voices[i].osc);
                sp_osc_init(g_sp, g_voices[i].osc, g_ft, 0);
                g_voices[i].osc->amp = 1.0f;
                g_voices[i].osc->freq = 440.0f;
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
    }

    void free_all_voices() {
        for (int i = 0; i < MAX_VOICES; ++i) {
            if (g_voices[i].osc) { sp_osc_destroy(&g_voices[i].osc); }
            if (g_voices[i].env) { sp_adsr_destroy(&g_voices[i].env); }
            if (g_voices[i].vcf) { sp_moogladder_destroy(&g_voices[i].vcf); }
            if (g_voices[i].fenv) { sp_adsr_destroy(&g_voices[i].fenv); }
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
    rebuild_table_sine();
    g_master_amp = 0.4f;
    g_env_atk = 0.01f; g_env_dec = 0.1f; g_env_sus = 0.8f; g_env_rel = 0.2f;
    g_poly_n = g_poly_n < 1 ? 1 : (g_poly_n > MAX_VOICES ? MAX_VOICES : g_poly_n);
    init_voices_if_needed();
}

void synth_set_freq(float freq) {
    // Set all active voices to the same freq (legacy support)
    for (int i = 0; i < g_poly_n; ++i) if (g_voices[i].osc) g_voices[i].osc->freq = freq;
}

void synth_set_amp(float amp) {
    g_master_amp = amp;
}

void synth_set_wave(int type) {
    if (!g_sp) return;
    // Recreate/overwrite the function table
    if (g_ft) {
        sp_ftbl_destroy(&g_ft);
        g_ft = nullptr;
    }
    sp_ftbl_create(g_sp, &g_ft, g_table_size);
    switch (type) {
        case 1: // saw
            rebuild_table_saw();
            break;
        case 2: // square
            rebuild_table_square();
            break;
        case 3: // triangle
            rebuild_table_triangle();
            break;
        case 0: // sine
        default:
            rebuild_table_sine();
            break;
    }
    // Re-init voice oscillators with new table
    for (int i = 0; i < g_poly_n; ++i) {
        if (g_voices[i].osc) {
            float amp = g_voices[i].osc->amp;
            float freq = g_voices[i].osc->freq;
            sp_osc_destroy(&g_voices[i].osc);
            sp_osc_create(&g_voices[i].osc);
            sp_osc_init(g_sp, g_voices[i].osc, g_ft, 0);
            g_voices[i].osc->amp = amp;
            g_voices[i].osc->freq = freq;
        }
    }
}

void synth_render(float* out_ptr, int frames) {
    if (!out_ptr || !g_sp) return;
    float zero = 0.0f, one = 1.0f;
    for (int i = 0; i < frames; ++i) {
        float mix = 0.0f;
        for (int v = 0; v < g_poly_n; ++v) {
            Voice &vc = g_voices[v];
            if (!vc.active && vc.gate <= 0.f) continue;
            float s = 0.0f;
            if (vc.osc) sp_osc_compute(g_sp, vc.osc, nullptr, &s);
            float gate_in = vc.gate > 0.f ? one : zero;
            // Filter envelope
            float fenv = 0.0f;
            if (vc.fenv) sp_adsr_compute(g_sp, vc.fenv, &gate_in, &fenv);
            // Modulate per-voice filter cutoff
            if (vc.vcf) {
                float cutoff = g_fcut + g_fenv_amt * fenv;
                if (cutoff < 20.0f) cutoff = 20.0f;
                float nyq = 0.5f * (float)g_sp->sr;
                if (cutoff > nyq - 100.0f) cutoff = nyq - 100.0f;
                vc.vcf->freq = cutoff;
                float r = (g_fres < 0.f ? 0.f : (g_fres > 1.f ? 1.f : g_fres));
                vc.vcf->res = r;
                float fs = 0.0f;
                sp_moogladder_compute(g_sp, vc.vcf, &s, &fs);
                s = fs;
            }
            // Amplitude envelope
            float env = 0.0f;
            if (vc.env) sp_adsr_compute(g_sp, vc.env, &gate_in, &env);
            mix += s * env * (vc.vel * g_master_amp);
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
    if (vc.osc) vc.osc->freq = freq;
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
    if (g_ft) {
        sp_ftbl_destroy(&g_ft);
        g_ft = nullptr;
    }
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
