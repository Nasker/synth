#include "audio.h"
#include "synth.h"
#include "fx.h"

#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define FRAMES_PER_CHUNK 256

static const char *TAG = "audio";
static i2s_chan_handle_t s_tx_chan;
static volatile uint32_t s_blocks_rendered;

static void audio_task(void *arg)
{
    static float fbuf[FRAMES_PER_CHUNK * 2];
    int16_t buf[FRAMES_PER_CHUNK * 2];
    while (1) {
        synth_render(fbuf, FRAMES_PER_CHUNK);
        fx_process(fbuf, FRAMES_PER_CHUNK);
        for (int i = 0; i < FRAMES_PER_CHUNK * 2; i++) {
            float x = fbuf[i];
            if (x > -1e-6f && x < 1e-6f) x = 0.f;  // kill denormal float->int path
            if (x > 1.f)  x = 1.f;
            if (x < -1.f) x = -1.f;
            buf[i] = (int16_t)(x * 32767.f);
        }
        size_t written = 0;
        i2s_channel_write(s_tx_chan, buf, sizeof(buf), &written, portMAX_DELAY);
        s_blocks_rendered++;
    }
}

uint32_t audio_blocks_rendered(void)
{
    return s_blocks_rendered;
}

void audio_start(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 16;   // ~93ms of slack — absorbs jitter/underruns
    chan_cfg.dma_frame_num = FRAMES_PER_CHUNK;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCLK,
            .ws = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));

    synth_init(AUDIO_SAMPLE_RATE);
    fx_init(AUDIO_SAMPLE_RATE);
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, 23, NULL, 1);

    ESP_LOGI(TAG, "I2S TX running @ %d Hz, MCLK=GPIO%d BCK=GPIO%d LRCK=GPIO%d DIN=GPIO%d",
             AUDIO_SAMPLE_RATE, PIN_I2S_MCLK, PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DOUT);
}
