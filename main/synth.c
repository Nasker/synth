#include "synth.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

#define LUT_BITS 10
#define LUT_SIZE (1 << LUT_BITS)
#define TWO_PI   6.28318530718f

enum { ENV_IDLE, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };

typedef struct {
    int      env_stage;
    int      note;
    uint32_t age;
    float    env;
    float    velocity;
    // dual oscillator
    uint32_t phase,  phase2;
    float    inc,    inc2;
    float    inc_t,  inc2_t;      // glide targets
    // per-voice TPT SVF state + per-block cached coeffs
    float    svf_ic1, svf_ic2;
    float    svf_a1, svf_a2, svf_a3;
    float    pan_l, pan_r;
    float    lfo_off;             // per-voice LFO phase offset
} voice_t;

static int16_t       s_sine_lut[LUT_SIZE];
static int           s_sample_rate;
static voice_t       s_voices[SYNTH_NUM_VOICES];
static uint32_t      s_age_counter;
static volatile float s_bend = 1.f;
static float          s_lfo_phase;

static synth_params_t s_params = {
    .waveform   = SYNTH_WAVE_SAW,
    .detune     = 0.25f,
    .spread     = 0.7f,
    .cutoff     = 0.7f,
    .reso       = 0.2f,
    .lfo_rate   = 0.4f,
    .lfo_depth  = 0.0f,
    .attack_s   = 0.01f,
    .decay_s    = 0.15f,
    .sustain    = 0.7f,
    .release_s  = 0.25f,
    .glide_s    = 0.0f,
    .delay_ms   = 380.f,
    .delay_fb   = 0.35f,
    .delay_mix  = 0.18f,
    .delay_damp = 0.4f,
    .comp_thresh = 0.6f,
    .comp_ratio  = 3.0f,
    .master     = 0.6f,
};

void synth_init(int sample_rate)
{
    s_sample_rate = sample_rate;
    for (int i = 0; i < LUT_SIZE; i++) {
        s_sine_lut[i] = (int16_t)(sinf(2.f * (float)M_PI * i / LUT_SIZE) * 32767.f);
    }
    memset(s_voices, 0, sizeof(s_voices));
    for (int v = 0; v < SYNTH_NUM_VOICES; v++) {
        // distribute filter-LFO phase + stereo position across voices
        s_voices[v].lfo_off = (float)v / SYNTH_NUM_VOICES;
        s_voices[v].pan_l   = s_voices[v].pan_r = 0.7071f;
    }
}

synth_params_t *synth_params(void)
{
    return &s_params;
}

float synth_midi_to_freq(int note)
{
    return 440.f * powf(2.f, (note - 69) / 12.f);
}

void synth_pitch_bend(float semitones)
{
    s_bend = powf(2.f, semitones / 12.f);
}

static inline float poly_blep(float t, float dt)
{
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.f;
    }
    if (t > 1.f - dt) {
        t = (t - 1.f) / dt;
        return t * t + t + t + 1.f;
    }
    return 0.f;
}

static inline float osc_sample(uint32_t phase, uint32_t inc, int wave)
{
    const float t  = phase / 4294967296.f;
    const float dt = inc / 4294967296.f;

    switch (wave) {
    case SYNTH_WAVE_SAW:
        return 2.f * t - 1.f - poly_blep(t, dt);
    case SYNTH_WAVE_SQUARE: {
        float s = t < 0.5f ? 1.f : -1.f;
        s += poly_blep(t, dt);
        float t2 = t + 0.5f;
        if (t2 >= 1.f) t2 -= 1.f;
        s -= poly_blep(t2, dt);
        return s;
    }
    case SYNTH_WAVE_TRI:
        return t < 0.5f ? 4.f * t - 1.f : 3.f - 4.f * t;
    case SYNTH_WAVE_SINE:
    default:
        return s_sine_lut[phase >> (32 - LUT_BITS)] / 32768.f;
    }
}

static inline void env_step(voice_t *v)
{
    const float sr = (float)s_sample_rate;
    switch (v->env_stage) {
    case ENV_ATTACK: {
        float rate = 1.f / (s_params.attack_s * sr + 1.f);
        v->env += rate;
        if (v->env >= 1.f) { v->env = 1.f; v->env_stage = ENV_DECAY; }
        break;
    }
    case ENV_DECAY: {
        float rate = 1.f / (s_params.decay_s * sr + 1.f);
        v->env -= rate;
        if (v->env <= s_params.sustain) { v->env = s_params.sustain; v->env_stage = ENV_SUSTAIN; }
        break;
    }
    case ENV_SUSTAIN:
        break;
    case ENV_RELEASE: {
        float rate = 1.f / (s_params.release_s * sr + 1.f);
        v->env -= rate;
        if (v->env <= 0.f) { v->env = 0.f; v->env_stage = ENV_IDLE; }
        break;
    }
    default:
        break;
    }
}

void synth_note_on(int midi_note, float velocity)
{
    int slot = -1;
    uint32_t oldest = UINT32_MAX;
    for (int i = 0; i < SYNTH_NUM_VOICES; i++) {
        if (s_voices[i].note == midi_note && s_voices[i].env_stage != ENV_IDLE) {
            slot = i;
            break;
        }
        if (s_voices[i].env_stage == ENV_IDLE && slot < 0) slot = i;
        if (s_voices[i].age < oldest) oldest = s_voices[i].age;
    }
    if (slot < 0) {
        for (int i = 0; i < SYNTH_NUM_VOICES; i++) {
            if (s_voices[i].age == oldest) { slot = i; break; }
        }
    }

    voice_t *v = &s_voices[slot];
    float f = synth_midi_to_freq(midi_note);
    float inc  = f * 4294967296.0 / s_sample_rate;
    float det  = powf(2.f, s_params.detune * 50.f / 1200.f); // 0..50 cents on osc2

    v->note     = midi_note;
    v->velocity = velocity;
    v->inc_t    = inc;
    v->inc2_t   = inc * det;
    if (s_params.glide_s > 0.0005f && v->inc != 0.f) {
        // keep current inc -> glides toward target
    } else {
        v->inc = inc; v->inc2 = inc * det;
        v->phase = v->phase2 = 0;
    }
    v->svf_ic1 = v->svf_ic2 = 0.f;
    v->env_stage = ENV_ATTACK;
    v->age       = s_age_counter++;
}

void synth_note_off(int midi_note)
{
    for (int i = 0; i < SYNTH_NUM_VOICES; i++) {
        if (s_voices[i].note == midi_note &&
            s_voices[i].env_stage != ENV_IDLE &&
            s_voices[i].env_stage != ENV_RELEASE) {
            s_voices[i].env_stage = ENV_RELEASE;
        }
    }
}

static inline float soft_clip(float x)
{
    return x * (27.f + x * x) / (27.f + 9.f * x * x);
}

// update per-block coefficients for one voice (TPT SVF, pan)
static void update_voice_coeffs(voice_t *v)
{
    float lf = sinf(TWO_PI * (s_lfo_phase + v->lfo_off));

    float fc = 40.f * powf(500.f, s_params.cutoff);        // 40Hz..20kHz
    fc *= powf(2.f, lf * s_params.lfo_depth * 2.f);        // LFO ±2 oct
    fc *= 0.6f + 0.8f * v->velocity;                       // velocity opens filter
    if (fc < 30.f)     fc = 30.f;
    if (fc > 16000.f)  fc = 16000.f;

    // Zavalishin TPT coefficients — unconditionally stable below Nyquist
    float g = tanf((float)M_PI * fc / s_sample_rate);
    float k = 2.f - 1.9f * s_params.reso;                  // 1/Q: res1 -> ~0.1
    if (g > 10.f) g = 10.f;
    float kk = g + k;
    v->svf_a1 = 1.f / (1.f + g * kk);
    v->svf_a2 = g * v->svf_a1;
    v->svf_a3 = g * v->svf_a2;

    float pan = ((float)(v - s_voices) / (SYNTH_NUM_VOICES - 1)) * 2.f - 1.f;
    pan *= s_params.spread;
    float a = (pan + 1.f) * (float)M_PI_4;                 // equal-power
    v->pan_l = cosf(a);
    v->pan_r = sinf(a);
}

int synth_active_voices(void)
{
    int n = 0;
    for (int v = 0; v < SYNTH_NUM_VOICES; v++) {
        if (s_voices[v].env_stage != ENV_IDLE) n++;
    }
    return n;
}

void synth_render(float *stereo_buf, size_t frames)
{
    const int   wave   = s_params.waveform;
    const float master = s_params.master;
    const bool  dual   = s_params.detune > 0.005f;
    const float gk     = s_params.glide_s > 0.0005f
                         ? 1.f / (s_params.glide_s * s_sample_rate + 1.f) : 1.f;

    int active = 0;
    for (int v = 0; v < SYNTH_NUM_VOICES; v++) {
        if (s_voices[v].env_stage != ENV_IDLE) { active++; update_voice_coeffs(&s_voices[v]); }
    }
    s_lfo_phase += s_params.lfo_rate * frames / s_sample_rate;
    if (s_lfo_phase >= 1.f) s_lfo_phase -= 1.f;

    const float norm = active > 0 ? 1.f / sqrtf((float)active) : 1.f;

    for (size_t i = 0; i < frames; i++) {
        float ml = 0.f, mr = 0.f;
        for (int v = 0; v < SYNTH_NUM_VOICES; v++) {
            voice_t *vc = &s_voices[v];
            if (vc->env_stage == ENV_IDLE) continue;

            float o = osc_sample(vc->phase, (uint32_t)vc->inc, wave);
            if (dual) o = (o + osc_sample(vc->phase2, (uint32_t)vc->inc2, wave)) * 0.7f;

            // TPT SVF -> lowpass out
            float v3  = o - vc->svf_ic2;
            float v1  = vc->svf_a1 * vc->svf_ic1 + vc->svf_a2 * v3;
            float v2  = vc->svf_ic2 + vc->svf_a2 * vc->svf_ic1 + vc->svf_a3 * v3;
            vc->svf_ic1 = 2.f * v1 - vc->svf_ic1;
            vc->svf_ic2 = 2.f * v2 - vc->svf_ic2;
            // finiteness guard + denormal flush — a poisoned or stalled voice
            // must never contaminate the mix or stall the render loop
            if (!(vc->svf_ic1 > -1e9f && vc->svf_ic1 < 1e9f &&
                  vc->svf_ic2 > -1e9f && vc->svf_ic2 < 1e9f) ||
                (fabsf(vc->svf_ic1) < 1e-15f && fabsf(vc->svf_ic2) < 1e-15f)) {
                vc->svf_ic1 = vc->svf_ic2 = 0.f;
                v2 = 0.f;
            }

            float s = v2 * vc->env * vc->velocity;
            ml += s * vc->pan_l;
            mr += s * vc->pan_r;

            vc->phase  += (uint32_t)(vc->inc  * s_bend);
            vc->phase2 += (uint32_t)(vc->inc2 * s_bend);
            vc->inc  += (vc->inc_t  - vc->inc)  * gk;
            vc->inc2 += (vc->inc2_t - vc->inc2) * gk;
            env_step(vc);
        }
        if (fabsf(ml) < 1e-15f) ml = 0.f;
        if (fabsf(mr) < 1e-15f) mr = 0.f;
        stereo_buf[2 * i]     = soft_clip(ml * norm) * master;
        stereo_buf[2 * i + 1] = soft_clip(mr * norm) * master;
    }
}
