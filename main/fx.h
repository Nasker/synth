#pragma once

#include <stddef.h>

void fx_init(int sample_rate);
void fx_process(float *stereo_buf, size_t frames);   // in-place interleaved L/R
