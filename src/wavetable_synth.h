#pragma once

#include <cstdint>

extern "C" {
// Initialize the synth. Creates Soundpipe context, table, and oscillator.
// sample_rate: e.g., 44100 or 48000
// table_size: size of wavetable (power of two recommended, e.g., 2048)
void synth_init(int sample_rate, int table_size);

// Set basic parameters
void synth_set_freq(float freq);
void synth_set_amp(float amp);

// Set waveform type
// 0 = sine, 1 = saw, 2 = square, 3 = triangle
void synth_set_wave(int type);

// Render 'frames' mono samples into memory pointed by 'out_ptr' (float*)
void synth_render(float* out_ptr, int frames);

// Simple MIDI helpers
void synth_note_on(int midi_note, float velocity);
void synth_note_off();

// Cleanup resources
void synth_shutdown();
}

