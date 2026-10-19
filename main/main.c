#include "audio.h"
#include "synth.h"
#include "usb_midi.h"
#include "web.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";
static const char *wave_names[] = {"sine", "saw", "square", "tri"};

// health monitor: logs heap + audio throughput every 10 s so we can see leaks/stalls
static void stats_task(void *arg)
{
    uint32_t last_blocks = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        uint32_t blocks = audio_blocks_rendered();
        ESP_LOGI("stats", "heap int:%u(min:%u) psram:%u | voices:%d | blocks/10s:%lu",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 synth_active_voices(),
                 (unsigned long)(blocks - last_blocks));
        last_blocks = blocks;
    }
}

static void init_task(void *arg)
{
    // WiFi/USB init is stack-hungry — run it off the small main task
    usb_midi_start();
    web_start();
    xTaskCreatePinnedToCore(stats_task, "stats", 3072, NULL, 2, NULL, 0);
    ESP_LOGI("init", "free heap — internal: %u, psram: %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    vTaskDelete(NULL);
}

void app_main(void)
{
    audio_start();
    xTaskCreatePinnedToCore(init_task, "init", 8192, NULL, 5, NULL, 0);

    // Demo: each pass picks a different waveform and plays a little phrase
    const int arp[]    = {57, 61, 64, 69, 76, 69, 64, 61};  // Amaj7 up-down
    const int chord[]  = {57, 61, 64, 69};
    const int n_arp = sizeof(arp) / sizeof(arp[0]);
    const int n_chord = sizeof(chord) / sizeof(chord[0]);

    // Demo plays until a MIDI device or the web UI sends its first note
    int wave = SYNTH_WAVE_SAW;
    while (!usb_midi_seen_activity() && !web_user_played()) {
        synth_params()->waveform = wave;
        ESP_LOGI(TAG, "waveform: %s", wave_names[wave]);

        for (int i = 0; i < n_arp; i++) {
            synth_note_on(arp[i], 0.9f);
            vTaskDelay(pdMS_TO_TICKS(250));
            synth_note_off(arp[i]);
            vTaskDelay(pdMS_TO_TICKS(60));
        }

        for (int i = 0; i < n_chord; i++) synth_note_on(chord[i], 0.5f);
        vTaskDelay(pdMS_TO_TICKS(1200));
        for (int i = 0; i < n_chord; i++) synth_note_off(chord[i]);
        vTaskDelay(pdMS_TO_TICKS(700));

        wave = (wave + 1) % SYNTH_WAVE_COUNT;
    }

    ESP_LOGI(TAG, "MIDI active — demo done, playing live");
    vTaskDelete(NULL);
}
