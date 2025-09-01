#include <cmath>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "../deps/soundpipe/h/base.h"
#include "../deps/soundpipe/h/ftbl.h"
#include "../deps/soundpipe/h/osc.h"
}

#include "wavetable_synth.h"

// Minimal Soundpipe state for a single-voice wavetable synth
namespace {
    sp_data* g_sp = nullptr;
    sp_ftbl* g_ft = nullptr;
    sp_osc*  g_osc = nullptr;
    int g_table_size = 2048;

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

    void init_osc_if_needed() {
        if (!g_osc && g_sp && g_ft) {
            sp_osc_create(&g_osc);
            sp_osc_init(g_sp, g_osc, g_ft, 0);
            g_osc->amp = 0.2f;
            g_osc->freq = 440.0f;
        }
    }
}

extern "C" {

void synth_init(int sample_rate, int table_size) {
    synth_shutdown();
    ensure_sp();
    g_sp->sr = sample_rate;
    g_table_size = table_size > 64 ? table_size : 2048;
    rebuild_table_sine();
    init_osc_if_needed();
}

void synth_set_freq(float freq) {
    if (g_osc) g_osc->freq = freq;
}

void synth_set_amp(float amp) {
    if (g_osc) g_osc->amp = amp;
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
    // Re-init osc with new table to be safe
    if (g_osc) {
        float amp = g_osc->amp;
        float freq = g_osc->freq;
        sp_osc_destroy(&g_osc);
        sp_osc_create(&g_osc);
        sp_osc_init(g_sp, g_osc, g_ft, 0);
        g_osc->amp = amp;
        g_osc->freq = freq;
    } else {
        init_osc_if_needed();
    }
}

void synth_render(float* out_ptr, int frames) {
    if (!out_ptr || !g_sp || !g_osc) return;
    for (int i = 0; i < frames; ++i) {
        float out = 0.0f;
        sp_osc_compute(g_sp, g_osc, nullptr, &out);
        out_ptr[i] = out;
    }
}

void synth_note_on(int midi_note, float velocity) {
    ensure_sp();
    init_osc_if_needed();
    float freq = sp_midi2cps(static_cast<float>(midi_note));
    if (g_osc) {
        g_osc->freq = freq;
        g_osc->amp = velocity <= 0.f ? 0.f : (velocity > 1.f ? 1.f : velocity) * 0.5f;
    }
}

void synth_note_off() {
    if (g_osc) g_osc->amp = 0.0f;
}

void synth_shutdown() {
    if (g_osc) {
        sp_osc_destroy(&g_osc);
        g_osc = nullptr;
    }
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

