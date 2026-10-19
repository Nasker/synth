#include "fx.h"
#include "synth.h"

#include <math.h>
#include <string.h>
#include "esp_heap_caps.h"

// Ping-pong stereo delay in PSRAM: each channel feeds back into the other,
// with a one-pole damper inside the feedback path.

#define MAX_DELAY_MS 1500

static int    s_sr;
static float *s_buf;          // stereo frames, PSRAM
static size_t s_buf_frames;   // ring size in frames
static size_t s_wpos;
static float  s_damp_l, s_damp_r;

void fx_init(int sample_rate)
{
    s_sr = sample_rate;
    s_buf_frames = (size_t)sample_rate * MAX_DELAY_MS / 1000;
    s_buf = heap_caps_calloc(s_buf_frames * 2, sizeof(float), MALLOC_CAP_SPIRAM);
}

void fx_process(float *buf, size_t frames)
{
    if (!s_buf) return;

    const size_t delay = (size_t)(synth_params()->delay_ms * s_sr / 1000.f) < s_buf_frames
                       ? (size_t)(synth_params()->delay_ms * s_sr / 1000.f)
                       : s_buf_frames - 1;
    if (delay == 0) return;

    const float fb  = synth_params()->delay_fb;
    const float wet = synth_params()->delay_mix;
    const float dry = 1.f - wet;
    // damp: 0 -> dc 0.85 (bright repeats), 1 -> dc 0.05 (dark repeats)
    const float dc  = 0.85f - 0.8f * synth_params()->delay_damp;

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
