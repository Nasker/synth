#pragma once

#include <stdint.h>

#define AUDIO_SAMPLE_RATE 44100

// Wiring: ESP32-S3 -> PCM5102 module
#define PIN_I2S_MCLK GPIO_NUM_7   // SCK on the module
#define PIN_I2S_BCLK GPIO_NUM_6   // BCK
#define PIN_I2S_WS   GPIO_NUM_4   // LRCK
#define PIN_I2S_DOUT GPIO_NUM_5   // DIN

void audio_start(void);
uint32_t audio_blocks_rendered(void);   // for stall diagnostics
uint32_t audio_max_cycles(void);        // max cycles/block since last call (reads+clears)
