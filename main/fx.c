#include "fx.h"
#include "synth.h"

#include <math.h>
#include <string.h>
#include "esp_heap_caps.h"

// FX chain: chorus -> ping-pong delay -> freeverb -> bus compressor.
// Chorus lives in internal RAM (short, latency-critical); the delay
// ring and reverb tanks live in PSRAM — sequential access suits the cache.

#define MAX_DELAY_MS   1500
#define CHORUS_MAX_MS  32
#define TWO_PI         6.28318530718f

static int    s_sr;

// ---- ping-pong delay ----
static float *s_buf;          // stereo frames, PSRAM
static size_t s_buf_frames;
static size_t s_wpos;
static float  s_damp_l, s_damp_r;

// ---- chorus: two quadrature-LFO modulated delay taps ----
static float *s_chbuf;        // stereo, internal RAM
static size_t s_ch_frames;
static size_t s_ch_w;
static float  s_ch_lfo;
static int16_t s_sin_lut[256];  // chorus LFO table — replaces per-sample sinf

// ---- freeverb: 8 combs + 4 allpasses per channel, PSRAM ----
typedef struct { float *buf; int len, idx; float lp; } comb_t;
typedef struct { float *buf; int len, idx; } allp_t;
static comb_t  s_combs[2][8];
static allp_t  s_allps[2][4];
static const int s_comb_len[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
static const int s_allp_len[4] = {556, 441, 341, 225};
#define REV_SPREAD 23           // stereo decorrelation (samples)

// ---- bus compressor ----
static float s_det_env;
static float s_atk_c, s_rel_c;

void fx_init(int sample_rate)
{
    s_sr = sample_rate;

    s_buf_frames = (size_t)sample_rate * MAX_DELAY_MS / 1000;
    s_buf = heap_caps_calloc(s_buf_frames * 2, sizeof(float), MALLOC_CAP_SPIRAM);

    s_ch_frames = (size_t)sample_rate * CHORUS_MAX_MS / 1000;
    s_chbuf = heap_caps_calloc(s_ch_frames * 2, sizeof(float),
                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    // carve one PSRAM pool into all reverb tanks
    int total = 0;
    for (int c = 0; c < 2; c++) {
        for (int i = 0; i < 8; i++) total += s_comb_len[i] + (c ? REV_SPREAD : 0);
        for (int i = 0; i < 4; i++) total += s_allp_len[i] + (c ? REV_SPREAD : 0);
    }
    float *pool = heap_caps_calloc(total, sizeof(float), MALLOC_CAP_SPIRAM);
    float *p = pool;
    for (int c = 0; c < 2; c++) {
        for (int i = 0; i < 8; i++) {
            // tanks run at half rate — halving the length preserves the
            // exact delay time and feedback decay of the 44.1k tuning
            s_combs[c][i].len = (s_comb_len[i] + (c ? REV_SPREAD : 0)) / 2;
            s_combs[c][i].buf = p;  p += s_combs[c][i].len;
        }
        for (int i = 0; i < 4; i++) {
            s_allps[c][i].len = (s_allp_len[i] + (c ? REV_SPREAD : 0)) / 2;
            s_allps[c][i].buf = p;  p += s_allps[c][i].len;
        }
    }

    for (int i = 0; i < 256; i++) {
        s_sin_lut[i] = (int16_t)(sinf(2.f * (float)M_PI * i / 256.f) * 32767.f);
    }

    s_atk_c = expf(-1.f / (0.002f * sample_rate));   // ~2ms attack
    s_rel_c = expf(-1.f / (0.2f   * sample_rate));   // ~200ms release
}

static inline float soft_clip(float x)
{
    return x * (27.f + x * x) / (27.f + 9.f * x * x);
}

// linear-interpolated read of the chorus ring, `tap` frames behind wpos
static inline float ch_read(const float *buf, size_t ch, float tap)
{
    float rf = (float)s_ch_w - tap;
    while (rf < 0.f) rf += s_ch_frames;
    size_t r0 = (size_t)rf;
    size_t r1 = (r0 + 1) % s_ch_frames;
    float f = rf - r0;
    return buf[2 * r0 + ch] + f * (buf[2 * r1 + ch] - buf[2 * r0 + ch]);
}

static inline float comb_step(comb_t *c, float in, float fb, float damp)
{
    float out = c->buf[c->idx];
    c->lp = out * (1.f - damp) + c->lp * damp;   // lowpass inside feedback
    if (!(c->lp > -1e9f && c->lp < 1e9f) || fabsf(c->lp) < 1e-15f) c->lp = 0.f;
    c->buf[c->idx] = in + c->lp * fb;
    if (++c->idx >= c->len) c->idx = 0;
    return out;
}

static inline float allp_step(allp_t *a, float in)
{
    float bv = a->buf[a->idx];
    float out = bv - in;
    a->buf[a->idx] = in + bv * 0.5f;
    if (++a->idx >= a->len) a->idx = 0;
    return out;
}

void fx_process(float *buf, size_t frames)
{
    if (!s_buf || !s_chbuf) return;

    const synth_fx_t *fx = synth_fx_params();

    // ---- chorus ----
    const float cmix = fx->chorus_mix;
    if (cmix > 0.001f) {
        const float cdry  = 1.f - cmix;
        const float lfo_i = fx->chorus_rate / s_sr;
        const float base  = 0.012f * s_sr;                    // 12 ms
        const float mod   = fx->chorus_depth * 0.010f * s_sr; // up to +10 ms
        for (size_t i = 0; i < frames; i++) {
            s_ch_lfo += lfo_i;
            if (s_ch_lfo >= 1.f) s_ch_lfo -= 1.f;
            uint8_t ph = (uint8_t)(s_ch_lfo * 256.f);
            float lf_l = s_sin_lut[ph] * (1.f / 32768.f);
            float lf_r = s_sin_lut[(uint8_t)(ph + 64)] * (1.f / 32768.f);  // quadrature
            float tap_l = base + mod * (0.5f + 0.5f * lf_l);
            float tap_r = base + mod * (0.5f + 0.5f * lf_r);

            float in_l = buf[2 * i], in_r = buf[2 * i + 1];
            s_chbuf[2 * s_ch_w]     = in_l;
            s_chbuf[2 * s_ch_w + 1] = in_r;

            float wl = ch_read(s_chbuf, 0, tap_l);
            float wr = ch_read(s_chbuf, 1, tap_r);
            if (!(wl > -1e9f && wl < 1e9f) || fabsf(wl) < 1e-15f) wl = 0.f;
            if (!(wr > -1e9f && wr < 1e9f) || fabsf(wr) < 1e-15f) wr = 0.f;

            buf[2 * i]     = in_l * cdry + wl * cmix;
            buf[2 * i + 1] = in_r * cdry + wr * cmix;

            s_ch_w = (s_ch_w + 1) % s_ch_frames;
        }
    }

    // ---- ping-pong delay ----
    size_t delay = (size_t)(fx->delay_ms * s_sr / 1000.f);
    if (delay >= s_buf_frames) delay = s_buf_frames - 1;

    if (delay > 0) {
        const float fb  = fx->delay_fb;
        const float wet = fx->delay_mix;
        const float dry = 1.f - wet;
        // damp: 0 -> dc 0.85 (bright repeats), 1 -> dc 0.05 (dark repeats)
        const float dc  = 0.85f - 0.8f * fx->delay_damp;

        size_t rpos = (s_wpos + s_buf_frames - delay) % s_buf_frames;

        for (size_t i = 0; i < frames; i++) {
            float dl = s_buf[2 * rpos];
            float dr = s_buf[2 * rpos + 1];

            s_damp_l += dc * (dl - s_damp_l);
            s_damp_r += dc * (dr - s_damp_r);

            // flush denormals (slow on Xtensa) + kill any NaN before it recirculates
            if (!(s_damp_l > -1e9f && s_damp_l < 1e9f) || fabsf(s_damp_l) < 1e-15f) s_damp_l = 0.f;
            if (!(s_damp_r > -1e9f && s_damp_r < 1e9f) || fabsf(s_damp_r) < 1e-15f) s_damp_r = 0.f;
            if (!(dl > -1e9f && dl < 1e9f) || fabsf(dl) < 1e-15f) dl = 0.f;
            if (!(dr > -1e9f && dr < 1e9f) || fabsf(dr) < 1e-15f) dr = 0.f;

            float in_l = buf[2 * i];
            float in_r = buf[2 * i + 1];

            s_buf[2 * s_wpos]     = in_l + fb * s_damp_r;  // right echo returns on left
            s_buf[2 * s_wpos + 1] = in_r + fb * s_damp_l;

            buf[2 * i]     = in_l * dry + dl * wet;
            buf[2 * i + 1] = in_r * dry + dr * wet;

            rpos  = (rpos + 1) % s_buf_frames;
            s_wpos = (s_wpos + 1) % s_buf_frames;
        }
    }

    // ---- freeverb, half rate ----
    // Tanks run on every other sample and the wet output is held —
    // halves both the ALU cost and the PSRAM traffic for an inaudible
    // change in a dark, diffuse path.
    const float rwet = fx->rev_wet;
    if (rwet > 0.001f) {
        const float rdry  = 1.f - rwet;
        const float fb    = 0.7f + 0.28f * fx->rev_room;  // classic freeverb room curve
        const float damp  = fx->rev_damp * 0.4f;
        const float r_in_g = 0.6f;                        // input is hotter than freeverb expects
        for (size_t i = 0; i < frames; i += 2) {
            float in_l = (buf[2 * i]     + buf[2 * i + 2]) * 0.5f;
            float in_r = (buf[2 * i + 1] + buf[2 * i + 3]) * 0.5f;
            float rin = (in_l + in_r) * 0.5f * r_in_g;

            float acc[2] = {0.f, 0.f};
            for (int c = 0; c < 2; c++) {
                for (int k = 0; k < 8; k++) acc[c] += comb_step(&s_combs[c][k], rin, fb, damp);
                for (int k = 0; k < 4; k++) acc[c] = allp_step(&s_allps[c][k], acc[c]);
            }
            float out_l = acc[0] * 0.09f, out_r = acc[1] * 0.09f;  // freeverb wet scale
            if (!(out_l > -1e9f && out_l < 1e9f)) out_l = 0.f;
            if (!(out_r > -1e9f && out_r < 1e9f)) out_r = 0.f;

            buf[2 * i]     = buf[2 * i]     * rdry + out_l * rwet;
            buf[2 * i + 1] = buf[2 * i + 1] * rdry + out_r * rwet;
            buf[2 * i + 2] = buf[2 * i + 2] * rdry + out_l * rwet;
            buf[2 * i + 3] = buf[2 * i + 3] * rdry + out_r * rwet;
        }
    }

    // ---- bus compressor (stereo-linked detector) ----
    const float T = fx->comp_thresh;
    const float R = fx->comp_ratio > 1.f ? fx->comp_ratio : 1.f;
    for (size_t i = 0; i < frames; i++) {
        float l = buf[2 * i], r = buf[2 * i + 1];
        float pk = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
        float c = pk > s_det_env ? s_atk_c : s_rel_c;
        s_det_env = c * s_det_env + (1.f - c) * pk;
        if (s_det_env < 1e-15f) s_det_env = 0.f;

        float g = 1.f;
        if (s_det_env > T) {
            float out = T + (s_det_env - T) / R;
            g = out / s_det_env;
        }
        buf[2 * i]     = soft_clip(l * g);
        buf[2 * i + 1] = soft_clip(r * g);
    }
}
