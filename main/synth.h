#pragma once

#include <stddef.h>
#include <stdint.h>

#define SYNTH_NUM_VOICES 8
#define SYNTH_NUM_PARTS  2

typedef enum {
    SYNTH_WAVE_SINE = 0,
    SYNTH_WAVE_SAW,
    SYNTH_WAVE_SQUARE,
    SYNTH_WAVE_TRI,
    SYNTH_WAVE_PWM,
    SYNTH_WAVE_NOISE,
    SYNTH_WAVE_COUNT
} synth_wave_t;

// Per-part patch — safe to poke from another task (web UI, MIDI).
// Each part owns its oscillator/filter/envelope settings and a MIDI
// channel; voices are drawn from the shared pool.
typedef struct {
    volatile int   wave1;          // synth_wave_t, osc1
    volatile int   wave2;          // synth_wave_t, osc2
    volatile float osc_mix;        // 0..1: 0=osc1 only, 0.5=equal, 1=osc2 only
    volatile float detune;         // 0..1 -> 0..50 cents on osc2
    volatile float pwm;            // 0.05..0.95 duty cycle for SYNTH_WAVE_PWM
    volatile float spread;         // 0..1 stereo voice spread
    volatile float cutoff;         // 0..1 -> ~40Hz..20kHz (exp)
    volatile float reso;           // 0..1
    volatile float lfo_rate;       // Hz
    volatile float lfo_depth;      // 0..1 -> +-2 octaves of cutoff mod
    volatile float attack_s;
    volatile float decay_s;
    volatile float sustain;        // 0..1 level
    volatile float release_s;
    volatile float glide_s;        // portamento time
    volatile int   midi_ch;        // 0..15
} synth_patch_t;

// Global FX-bus parameters shared by all parts
typedef struct {
    volatile float chorus_rate;    // Hz
    volatile float chorus_depth;   // 0..1 -> up to ~10ms of modulation
    volatile float chorus_mix;     // 0..0.5 wet
    volatile float delay_ms;       // 0 disables
    volatile float delay_fb;       // 0..0.95
    volatile float delay_mix;      // 0..0.5 wet
    volatile float delay_damp;     // 0..1 darkening in feedback
    volatile float rev_room;       // 0..1 room size
    volatile float rev_damp;       // 0..1 high-frequency damping
    volatile float rev_wet;        // 0..0.5 wet
    volatile float comp_thresh;    // 0.3..1 onset level
    volatile float comp_ratio;     // 1..10
    volatile float master;         // 0..1
} synth_fx_t;

void            synth_init(int sample_rate);
float           synth_midi_to_freq(int note);
void            synth_note_on(int midi_note, float velocity, int part);
void            synth_note_off(int midi_note, int part);
void            synth_pitch_bend(float semitones, int part);      // ±N semitones
synth_patch_t  *synth_patch(int part);                            // part clamps to range
synth_fx_t     *synth_fx_params(void);
int             synth_part_for_channel(int midi_ch);              // channel -> part map
void            synth_render(float *stereo_buf, size_t frames);   // float L/R, pre-FX
int             synth_active_voices(void);
