#include "math.h"
#include "stdio.h"
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "SQUAWK_PLAYER";

#define SAMPLE_RATE 48000
#define I2S_BUFFER_SIZE 4096

#define I2S_BCK_PIN GPIO_NUM_6 // BCLK
#define I2S_WS_PIN GPIO_NUM_5  // LRC/LRCLK
#define I2S_DO_PIN GPIO_NUM_7  // DIN
#define I2S_SD_PIN GPIO_NUM_10 // SD (Shutdown, active high)
#define GAIN_PIN GPIO_NUM_9    // GAIN (Low=12dB, High=15dB gain)

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

static i2s_chan_handle_t tx_handle;
static float volume = 1.0f; // Volume control (0.0 to 1.0)

extern const uint8_t squawk_pcm_start[] asm("_binary_squawk_pcm_start");
extern const uint8_t squawk_pcm_end[] asm("_binary_squawk_pcm_end");

void configure_i2s() {
    i2s_std_slot_config_t slot_cfg = {
        .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
        .slot_mode = I2S_SLOT_MODE_STEREO,
        .slot_mask = I2S_STD_SLOT_LEFT,
        .ws_width = 16,
        .ws_pol = false,
        .bit_shift = false,
    };

    i2s_std_config_t i2s_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = slot_cfg,
        .gpio_cfg =
            {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = I2S_BCK_PIN,
                .ws = I2S_WS_PIN,
                .dout = I2S_DO_PIN,
                .din = I2S_GPIO_UNUSED,
            },
    };

    i2s_chan_config_t chan_config = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 32,
        .dma_frame_num = 128,
        .auto_clear = true,
    };

    ESP_ERROR_CHECK(i2s_new_channel(&chan_config, &tx_handle, NULL));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &i2s_config));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << I2S_SD_PIN) | (1ULL << GAIN_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(I2S_SD_PIN, 1);
    gpio_set_level(GAIN_PIN, 1);

    ESP_LOGI(TAG, "I2S configured successfully");
}

void cleanup_i2s() {
    if (tx_handle) {
        i2s_channel_disable(tx_handle);
        i2s_del_channel(tx_handle);
        tx_handle = NULL;
    }
    gpio_set_level(I2S_SD_PIN, 0);
    gpio_set_level(GAIN_PIN, 0);
    ESP_LOGI(TAG, "I2S resources cleaned up");
}

// Use for testing purposes. Will verify wiring and I2S configuration.
void generate_sine_wave() {
    const int freq = 440;   // 440 Hz (A4)
    const int duration = 2; // 2 seconds
    int sample_rate = SAMPLE_RATE;
    int samples = sample_rate * duration;

    configure_i2s();

    int16_t *sine_wave = malloc(samples * sizeof(int16_t));
    if (!sine_wave) {
        ESP_LOGE(TAG, "Failed to allocate sine wave buffer");
        return;
    }

    for (int i = 0; i < samples; i++) {
        sine_wave[i] = (int16_t)(INT16_MAX * sinf(2 * M_PI * freq * i / sample_rate));
    }

    for (int i = 0; i < samples; i += I2S_BUFFER_SIZE / sizeof(int16_t)) {
        size_t chunk_size = MIN(I2S_BUFFER_SIZE, (samples - i) * sizeof(int16_t));
        i2s_channel_write(tx_handle, &sine_wave[i], chunk_size, NULL, portMAX_DELAY);
    }
    cleanup_i2s();
    free(sine_wave);
}

void squawk() {
    ESP_LOGI(TAG, "Starting playback...");

    configure_i2s();

    size_t file_size = squawk_pcm_end - squawk_pcm_start;
    size_t read_size = 0;

    while (read_size < file_size) {
        size_t chunk_size = MIN(I2S_BUFFER_SIZE, file_size - read_size);
        size_t bytes_written = 0;
        i2s_channel_write(tx_handle, squawk_pcm_start + read_size, chunk_size, &bytes_written, portMAX_DELAY);
        read_size += chunk_size;
    }

    cleanup_i2s();
    ESP_LOGI(TAG, "Playback complete.");
}
void app_main(void) { squawk(); }
