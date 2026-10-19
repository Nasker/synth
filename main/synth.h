#pragma once

#include <stddef.h>
#include <stdint.h>

#define SYNTH_NUM_VOICES 8

typedef enum {
    SYNTH_WAVE_SINE = 0,
    SYNTH_WAVE_SAW,
    SYNTH_WAVE_SQUARE,
    SYNTH_WAVE_TRI,
    SYNTH_WAVE_COUNT
} synth_wave_t;

// Live-tweakable parameters — safe to poke from another task (web UI, MIDI)
typedef struct {
    // oscillator
    volatile int   waveform;       // synth_wave_t
    volatile float detune;         // 0..1 -> 0..50 cents on osc2
    volatile float spread;         // 0..1 stereo voice spread
    // filter (per-voice SVF)
    volatile float cutoff;         // 0..1 -> ~40Hz..20kHz (exp)
    volatile float reso;           // 0..1
    volatile float lfo_rate;       // Hz
    volatile float lfo_depth;      // 0..1 -> +-2 octaves of cutoff mod
    // envelope
    volatile float attack_s;
    volatile float decay_s;
    volatile float sustain;        // 0..1 level
    volatile float release_s;
    volatile float glide_s;        // portamento time
    // delay fx
    volatile float delay_ms;       // 0 disables
    volatile float delay_fb;       // 0..0.95
    volatile float delay_mix;      // 0..0.5 wet
    volatile float delay_damp;     // 0..1 darkening in feedback
    // bus compressor
    volatile float comp_thresh;    // 0.3..1 onset level
    volatile float comp_ratio;     // 1..10
    volatile float master;         // 0..1
} synth_params_t;

void            synth_init(int sample_rate);
float           synth_midi_to_freq(int note);
void            synth_note_on(int midi_note, float velocity);   // velocity 0..1
void            synth_note_off(int midi_note);
void            synth_pitch_bend(float semitones);              // ±N semitones
synth_params_t *synth_params(void);
void            synth_render(float *stereo_buf, size_t frames); // float L/R, pre-FX
int             synth_active_voices(void);
