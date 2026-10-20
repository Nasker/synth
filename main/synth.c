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
    int      part;
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
    uint32_t rng;                 // noise generator state
} voice_t;

// Per-block snapshot of each part's patch — volatile patch fields are
// read once per render block instead of once per sample
typedef struct {
    int   wave1, wave2;
    float mix1, mix2;
    float pwm;
    float gk;                     // glide coefficient
    float arate, drate, rrate;
    float sustain;
    float lfo_phase;
    bool  dual;
} part_rt_t;

static int16_t       s_sine_lut[LUT_SIZE];
static int           s_sample_rate;
static voice_t       s_voices[SYNTH_NUM_VOICES];
static uint32_t      s_age_counter;
static volatile float s_bend[SYNTH_NUM_PARTS] = {1.f, 1.f};
static part_rt_t      s_rt[SYNTH_NUM_PARTS];

static synth_patch_t s_patches[SYNTH_NUM_PARTS] = {
    {   // part 0 — MIDI ch 1: lead voice
        .wave1     = SYNTH_WAVE_SAW,
        .wave2     = SYNTH_WAVE_SAW,
        .osc_mix   = 0.35f,
        .pwm       = 0.35f,
        .detune    = 0.25f,
        .spread    = 0.7f,
        .cutoff    = 0.7f,
        .reso      = 0.2f,
        .lfo_rate  = 0.4f,
        .lfo_depth = 0.0f,
        .attack_s  = 0.01f,
        .decay_s   = 0.15f,
        .sustain   = 0.7f,
        .release_s = 0.25f,
        .glide_s   = 0.0f,
        .midi_ch   = 0,
    },
    {   // part 1 — MIDI ch 2: softer pad voice
        .wave1     = SYNTH_WAVE_PWM,
        .wave2     = SYNTH_WAVE_SAW,
        .osc_mix   = 0.45f,
        .pwm       = 0.3f,
        .detune    = 0.4f,
        .spread    = 0.85f,
        .cutoff    = 0.5f,
        .reso      = 0.15f,
        .lfo_rate  = 0.25f,
        .lfo_depth = 0.15f,
        .attack_s  = 0.35f,
        .decay_s   = 0.4f,
        .sustain   = 0.8f,
        .release_s = 0.7f,
        .glide_s   = 0.0f,
        .midi_ch   = 1,
    },
};

static synth_fx_t s_fx = {
    .chorus_rate  = 0.8f,
    .chorus_depth = 0.35f,
    .chorus_mix   = 0.12f,
    .rev_room     = 0.55f,
    .rev_damp     = 0.4f,
    .rev_wet      = 0.15f,
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
        s_voices[v].rng     = 0x9E3779B9u + v * 2654435761u;
    }
}

synth_patch_t *synth_patch(int part)
{
    if (part < 0) part = 0;
    if (part >= SYNTH_NUM_PARTS) part = SYNTH_NUM_PARTS - 1;
    return &s_patches[part];
}

synth_fx_t *synth_fx_params(void)
{
    return &s_fx;
}

int synth_part_for_channel(int midi_ch)
{
    for (int i = 0; i < SYNTH_NUM_PARTS; i++) {
        if (s_patches[i].midi_ch == midi_ch) return i;
    }
    return 0;   // unmatched channels fall back to part 0
}

float synth_midi_to_freq(int note)
{
    return 440.f * powf(2.f, (note - 69) / 12.f);
}

void synth_pitch_bend(float semitones, int part)
{
    if (part < 0 || part >= SYNTH_NUM_PARTS) return;
    s_bend[part] = powf(2.f, semitones / 12.f);
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

#define PHASE_SCALE (1.f / 4294967296.f)   // exact: divisor is a power of two

static inline float pulse_wave(float t, float dt, float w)
{
    float s = t < w ? 1.f : -1.f;
    s += poly_blep(t, dt);
    float t2 = t - w;
    if (t2 < 0.f) t2 += 1.f;
    s -= poly_blep(t2, dt);
    return s;
}

static inline float osc_sample(voice_t *v, uint32_t phase, uint32_t inc,
                               int wave, float pwm_w)
{
    const float t  = phase * PHASE_SCALE;
    const float dt = inc * PHASE_SCALE;

    switch (wave) {
    case SYNTH_WAVE_SAW:
        return 2.f * t - 1.f - poly_blep(t, dt);
    case SYNTH_WAVE_SQUARE:
        return pulse_wave(t, dt, 0.5f);
    case SYNTH_WAVE_PWM: {
        float w = pwm_w;
        if (w < 0.05f) w = 0.05f;
        if (w > 0.95f) w = 0.95f;
        return pulse_wave(t, dt, w);
    }
    case SYNTH_WAVE_NOISE:
        v->rng ^= v->rng << 13;
        v->rng ^= v->rng >> 17;
        v->rng ^= v->rng << 5;
        return (float)(v->rng >> 9) * (1.f / 8388608.f) - 1.f;
    case SYNTH_WAVE_TRI:
        return t < 0.5f ? 4.f * t - 1.f : 3.f - 4.f * t;
    case SYNTH_WAVE_SINE:
    default:
        return s_sine_lut[phase >> (32 - LUT_BITS)] / 32768.f;
    }
}

static inline void env_step(voice_t *v)
{
    const part_rt_t *rt = &s_rt[v->part];
    switch (v->env_stage) {
    case ENV_ATTACK:
        v->env += rt->arate;
        if (v->env >= 1.f) { v->env = 1.f; v->env_stage = ENV_DECAY; }
        break;
    case ENV_DECAY:
        v->env -= rt->drate;
        if (v->env <= rt->sustain) { v->env = rt->sustain; v->env_stage = ENV_SUSTAIN; }
        break;
    case ENV_SUSTAIN:
        break;
    case ENV_RELEASE:
        v->env -= rt->rrate;
        if (v->env <= 0.f) { v->env = 0.f; v->env_stage = ENV_IDLE; }
        break;
    default:
        break;
    }
}

void synth_note_on(int midi_note, float velocity, int part)
{
    if (part < 0 || part >= SYNTH_NUM_PARTS) return;
    const synth_patch_t *pp = &s_patches[part];

    int slot = -1;
    uint32_t oldest = UINT32_MAX;
    for (int i = 0; i < SYNTH_NUM_VOICES; i++) {
        if (s_voices[i].note == midi_note && s_voices[i].part == part &&
            s_voices[i].env_stage != ENV_IDLE) {
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
    float det  = powf(2.f, pp->detune * 50.f / 1200.f); // 0..50 cents on osc2

    v->note     = midi_note;
    v->part     = part;
    v->velocity = velocity;
    v->inc_t    = inc;
    v->inc2_t   = inc * det;
    if (pp->glide_s > 0.0005f && v->inc != 0.f) {
        // keep current inc -> glides toward target
    } else {
        v->inc = inc; v->inc2 = inc * det;
        v->phase = v->phase2 = 0;
    }
    v->svf_ic1 = v->svf_ic2 = 0.f;
    v->env_stage = ENV_ATTACK;
    v->age       = s_age_counter++;
}

void synth_note_off(int midi_note, int part)
{
    for (int i = 0; i < SYNTH_NUM_VOICES; i++) {
        if (s_voices[i].note == midi_note && s_voices[i].part == part &&
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
    const synth_patch_t *pp = &s_patches[v->part];
    float lf = sinf(TWO_PI * (s_rt[v->part].lfo_phase + v->lfo_off));

    float fc = 40.f * powf(500.f, pp->cutoff);             // 40Hz..20kHz
    fc *= powf(2.f, lf * pp->lfo_depth * 2.f);             // LFO ±2 oct
    fc *= 0.6f + 0.8f * v->velocity;                       // velocity opens filter
    if (fc < 30.f)     fc = 30.f;
    if (fc > 16000.f)  fc = 16000.f;

    // Zavalishin TPT coefficients — unconditionally stable below Nyquist
    float g = tanf((float)M_PI * fc / s_sample_rate);
    float k = 2.f - 1.9f * pp->reso;                       // 1/Q: res1 -> ~0.1
    if (g > 10.f) g = 10.f;
    float kk = g + k;
    v->svf_a1 = 1.f / (1.f + g * kk);
    v->svf_a2 = g * v->svf_a1;
    v->svf_a3 = g * v->svf_a2;

    float pan = ((float)(v - s_voices) / (SYNTH_NUM_VOICES - 1)) * 2.f - 1.f;
    pan *= pp->spread;
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
    const float master = s_fx.master;
    const float sr_f   = (float)s_sample_rate;

    // snapshot each part's volatile patch fields once per block
    for (int p = 0; p < SYNTH_NUM_PARTS; p++) {
        const synth_patch_t *pp = &s_patches[p];
        part_rt_t *rt = &s_rt[p];
        rt->wave1  = pp->wave1;
        rt->wave2  = pp->wave2;
        rt->mix1   = 1.f - pp->osc_mix;
        rt->mix2   = pp->osc_mix;
        rt->pwm    = pp->pwm;
        rt->dual   = rt->mix2 > 0.001f;
        rt->gk     = pp->glide_s > 0.0005f
                     ? 1.f / (pp->glide_s * sr_f + 1.f) : 1.f;
        rt->arate   = 1.f / (pp->attack_s  * sr_f + 1.f);
        rt->drate   = 1.f / (pp->decay_s   * sr_f + 1.f);
        rt->rrate   = 1.f / (pp->release_s * sr_f + 1.f);
        rt->sustain = pp->sustain;
        rt->lfo_phase += pp->lfo_rate * frames / sr_f;
        if (rt->lfo_phase >= 1.f) rt->lfo_phase -= 1.f;
    }

    int active = 0;
    for (int v = 0; v < SYNTH_NUM_VOICES; v++) {
        if (s_voices[v].env_stage != ENV_IDLE) { active++; update_voice_coeffs(&s_voices[v]); }
    }

    const float norm = active > 0 ? 1.f / sqrtf((float)active) : 1.f;

    for (size_t i = 0; i < frames; i++) {
        float ml = 0.f, mr = 0.f;
        for (int v = 0; v < SYNTH_NUM_VOICES; v++) {
            voice_t *vc = &s_voices[v];
            if (vc->env_stage == ENV_IDLE) continue;

            const part_rt_t *rt    = &s_rt[vc->part];
            const float      bend  = s_bend[vc->part];
            const float      pwm_w = rt->pwm;

            float o = osc_sample(vc, vc->phase, (uint32_t)vc->inc,
                                 rt->wave1, pwm_w) * rt->mix1;
            if (rt->dual) {
                o += osc_sample(vc, vc->phase2, (uint32_t)vc->inc2,
                                rt->wave2, pwm_w) * rt->mix2;
            }

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

            if (bend != 1.f) {
                vc->phase  += (uint32_t)(vc->inc  * bend);
                vc->phase2 += (uint32_t)(vc->inc2 * bend);
            } else {
                vc->phase  += (uint32_t)vc->inc;
                vc->phase2 += (uint32_t)vc->inc2;
            }
            if (rt->gk < 1.f) {
                vc->inc  += (vc->inc_t  - vc->inc)  * rt->gk;
                vc->inc2 += (vc->inc2_t - vc->inc2) * rt->gk;
            } else {
                vc->inc = vc->inc_t;
                vc->inc2 = vc->inc2_t;
            }
            env_step(vc);
        }
        if (fabsf(ml) < 1e-15f) ml = 0.f;
        if (fabsf(mr) < 1e-15f) mr = 0.f;
        stereo_buf[2 * i]     = soft_clip(ml * norm) * master;
        stereo_buf[2 * i + 1] = soft_clip(mr * norm) * master;
    }
}
