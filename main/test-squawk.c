#include "math.h"
#include "stdio.h"
#include <stdio.h>

#include "audio_codec_test.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_dec_reg.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <string.h>

typedef union {
    esp_m4a_dec_cfg_t m4a_cfg;
    esp_ts_dec_cfg_t ts_cfg;
    esp_aac_dec_cfg_t aac_cfg;
} simp_dec_all_t;

typedef struct {
    uint8_t *data;
    int read_size;
    int size;
} read_ctx_t;

typedef struct {
    uint8_t *data;
    int write_size;
    int read_size;
    int size;
    int decode_size;
} write_ctx_t;

static write_ctx_t write_ctx;
static read_ctx_t read_ctx;

static const char *TAG = "SQUAWK_PLAYER";

#define SAMPLE_RATE 48000 // Try higher sample rate
#define I2S_BUFFER_SIZE 4096

#define I2S_BCK_PIN GPIO_NUM_6 // BCLK
#define I2S_WS_PIN GPIO_NUM_5  // LRC/LRCLK
#define I2S_DO_PIN GPIO_NUM_7  // DIN
#define I2S_SD_PIN GPIO_NUM_10 // SD (Shutdown, active high)
#define GAIN_PIN GPIO_NUM_9    // GAIN (Low=12dB, High=15dB gain)

static bool play_audio = false;
static float volume = 1.0f; // Volume control (0.0 to 1.0)

static i2s_chan_handle_t tx_handle; // I2S transmit channel

extern const uint8_t squawk_m4a_start[] asm("_binary_squawk_m4v_start");
extern const uint8_t squawk_m4a_end[] asm("_binary_squawk_m4v_end");

void configure_i2s() {
    // Configure I2S standard slot settings
    i2s_std_slot_config_t slot_cfg = {
        .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT, // Try 32-bit width
        .slot_mode = I2S_SLOT_MODE_STEREO,
        .slot_mask = I2S_STD_SLOT_LEFT, // Just use left channel since MAX98357A mixes internally
        .ws_width = 32,                 // Match the bit width
        .ws_pol = false,                // Standard polarity
        .bit_shift = false,             // Disable bit shift to maintain signal integrity
    };

    // Configure I2S standard configuration
    i2s_std_config_t i2s_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = slot_cfg,
        .gpio_cfg =
            {
                .mclk = I2S_GPIO_UNUSED, // MAX98357 doesn't need MCLK
                .bclk = I2S_BCK_PIN,
                .ws = I2S_WS_PIN,
                .dout = I2S_DO_PIN,
                .din = I2S_GPIO_UNUSED,
            },
    };

    // Create a new I2S channel
    i2s_chan_config_t chan_config = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 32,   // More descriptors
        .dma_frame_num = 128, // Smaller frames for better control
        .auto_clear = true,
    };

    // Initialize I2S
    ESP_ERROR_CHECK(i2s_new_channel(&chan_config, &tx_handle, NULL));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &i2s_config));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));

    // Configure SD pin - must be HIGH for audio output
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << I2S_SD_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(I2S_SD_PIN, 1); // Enable the amplifier

    // Configure GAIN pin - LOW for 12dB, HIGH for 15dB
    io_conf.pin_bit_mask = (1ULL << GAIN_PIN);
    gpio_config(&io_conf);
    gpio_set_level(GAIN_PIN, 1); // Start with lower gain

    ESP_LOGI(TAG, "I2S configured successfully");
}

static esp_audio_simple_dec_type_t get_simple_decoder_type(const uint8_t *data, size_t size) {
    // For embedded files, assume M4A format
    return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
}

static void get_simple_decoder_config(esp_audio_simple_dec_cfg_t *cfg) {
    simp_dec_all_t *all_cfg = (simp_dec_all_t *)cfg->dec_cfg;

    switch (cfg->dec_type) {
    case ESP_AUDIO_SIMPLE_DEC_TYPE_M4A: {
        esp_m4a_dec_cfg_t *m4a_cfg = &all_cfg->m4a_cfg;
        m4a_cfg->aac_plus_enable = true;
        cfg->cfg_size = sizeof(esp_m4a_dec_cfg_t);
        break;
    }
    default:
        break;
    }
}

static int encoder_read_pcm(uint8_t *data, int size) {
    if (read_ctx.read_size + size <= read_ctx.size) {
        memcpy(data, read_ctx.data + read_ctx.read_size, size);
        read_ctx.read_size += size;
        return size;
    }
    return 0;
}

static int adjust_volume(uint8_t *data, int size, float volume) {
    for (int i = 0; i < size / 2; i++) {
        int16_t sample = ((int16_t *)data)[i];
        sample = (int16_t)(sample * volume);
        ((int16_t *)data)[i] = sample;
    }
    return size;
}

static int simple_decoder_write_pcm(uint8_t *data, int size) {
    int32_t *buffer = malloc(size * 2);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate conversion buffer");
        return 0;
    }

    int samples = size / 2;
    int16_t *input = (int16_t *)data;

    // Add digital gain (1.5x boost) while checking for clipping
    const float digital_gain = 1.5f; // Increase this carefully
    for (int i = 0; i < samples; i++) {
        int32_t sample = (int32_t)input[i];
        // Apply volume and digital gain with clipping protection
        sample = (int32_t)(sample * volume * digital_gain);
        // Clip to 16-bit bounds
        if (sample > 32767)
            sample = 32767;
        if (sample < -32768)
            sample = -32768;
        // Shift to 32-bit position
        buffer[i] = sample << 16;
    }

    size_t bytes_written = 0;
    esp_err_t ret = i2s_channel_write(tx_handle, buffer, size * 2, &bytes_written, portMAX_DELAY);
    free(buffer);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write I2S data: %s", esp_err_to_name(ret));
        return 0;
    }

    return bytes_written / 2;
}

int audio_simple_decoder_test(esp_audio_simple_dec_type_t type, audio_codec_test_cfg_t *cfg, audio_info_t *info) {
    esp_audio_dec_register_default();
    int ret = esp_audio_simple_dec_register_default();
    int max_out_size = 4096;
    int read_size = 512;
    uint8_t *in_buf = malloc(read_size);
    uint8_t *out_buf = malloc(max_out_size);
    esp_audio_simple_dec_handle_t decoder = NULL;
    do {
        if (in_buf == NULL || out_buf == NULL) {
            ESP_LOGI(TAG, "No memory for decoder");
            ret = ESP_AUDIO_ERR_MEM_LACK;
            break;
        }
        simp_dec_all_t all_cfg = {};
        esp_audio_simple_dec_cfg_t dec_cfg = {
            .dec_type = type,
            .dec_cfg = &all_cfg,
        };
        get_simple_decoder_config(&dec_cfg);
        ret = esp_audio_simple_dec_open(&dec_cfg, &decoder);
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGI(TAG, "Fail to open simple decoder ret %d", ret);
            break;
        }
        int total_decoded = 0;
        uint64_t decode_time = 0;
        esp_audio_simple_dec_raw_t raw = {
            .buffer = in_buf,
        };
        uint64_t read_start = esp_timer_get_time();
        while (ret == ESP_AUDIO_ERR_OK) {
            ret = cfg->read(in_buf, read_size);
            if (ret < 0) {
                break;
            }
            raw.buffer = in_buf;
            raw.len = ret;
            raw.eos = (ret < read_size);
            esp_audio_simple_dec_out_t out_frame = {
                .buffer = out_buf,
                .len = max_out_size,
            };
            // ATTENTION: when input raw data unconsumed (`raw.len > 0`) do not overwrite its content
            // Or-else unexpected error may happen for data corrupt.
            while (raw.len) {
                uint64_t start = esp_timer_get_time();
                if (start > read_start + 30000000) {
                    raw.eos = true;
                    break;
                }
                ret = esp_audio_simple_dec_process(decoder, &raw, &out_frame);
                decode_time += esp_timer_get_time() - start;
                if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                    // Handle output buffer not enough case
                    uint8_t *new_buf = realloc(out_buf, out_frame.needed_size);
                    if (new_buf == NULL) {
                        break;
                    }
                    out_buf = new_buf;
                    out_frame.buffer = new_buf;
                    max_out_size = out_frame.needed_size;
                    out_frame.len = max_out_size;
                    continue;
                }
                if (ret != ESP_AUDIO_ERR_OK) {
                    ESP_LOGE(TAG, "Fail to decode data ret %d", ret);
                    break;
                }
                if (out_frame.decoded_size) {
                    if (total_decoded == 0) {
                        // Update audio information
                        esp_audio_simple_dec_info_t dec_info = {};
                        esp_audio_simple_dec_get_info(decoder, &dec_info);
                        info->sample_rate = dec_info.sample_rate;
                        info->bits_per_sample = dec_info.bits_per_sample;
                        info->channel = dec_info.channel;
                    }
                    total_decoded += out_frame.decoded_size;
                    if (cfg->write) {
                        cfg->write(out_frame.buffer, out_frame.decoded_size);
                    }
                }
                // In case that input data contain multiple frames
                raw.len -= raw.consumed;
                raw.buffer += raw.consumed;
            }
            if (raw.eos) {
                break;
            }
        }
        if (total_decoded) {
            int sample_size = info->channel * info->bits_per_sample >> 3;
            float cpu_usage = (float)decode_time * sample_size * info->sample_rate / total_decoded / 10000;
            ESP_LOGI(TAG, "Decode for %d cpu: %.2f%%", type, cpu_usage);
        }
    } while (0);
    esp_audio_simple_dec_close(decoder);
    esp_audio_simple_dec_unregister_default();
    esp_audio_dec_unregister_default();
    if (in_buf) {
        free(in_buf);
    }
    if (out_buf) {
        free(out_buf);
    }
    return ret;
}

int audio_simple_decoder_test_embedded(codec_write_cb writer, audio_info_t *info) {
    size_t file_size = squawk_m4a_end - squawk_m4a_start;
    read_ctx.data = (uint8_t *)squawk_m4a_start;
    read_ctx.size = file_size;
    read_ctx.read_size = 0;

    audio_codec_test_cfg_t dec_cfg = {
        .read = encoder_read_pcm,
        .write = writer,
    };

    return audio_simple_decoder_test(ESP_AUDIO_SIMPLE_DEC_TYPE_M4A, &dec_cfg, info);
}

// First, uncomment and update the cleanup function
void cleanup_i2s() {
    if (tx_handle) {
        i2s_channel_disable(tx_handle);
        i2s_del_channel(tx_handle); // Updated to use correct deletion function
        tx_handle = NULL;           // Clear the handle after deletion
    }
    gpio_set_level(I2S_SD_PIN, 0); // Disable amplifier
    gpio_set_level(GAIN_PIN, 0);   // Reset gain pin
    ESP_LOGI(TAG, "I2S resources cleaned up");
}

// Then update the squawk function to use cleanup
void squawk() {
    ESP_LOGI(TAG, "Free heap size before playback: %lu", esp_get_free_heap_size());

    audio_info_t info = {0};
    configure_i2s();
    if (audio_simple_decoder_test_embedded(simple_decoder_write_pcm, &info) != 0) {
        ESP_LOGW(TAG, "Playback failed for embedded file");
    } else {
        ESP_LOGI(TAG, "Playback succeeded for embedded file");
    }

    cleanup_i2s(); // Call cleanup after playback

    ESP_LOGI(TAG, "Free heap size after playback: %lu", esp_get_free_heap_size());
}

void app_main(void) { squawk(); }
