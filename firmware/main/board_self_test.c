#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_chip_info.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_psram.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "driver/gpio.h"
#if defined(CONFIG_FIGURE_ENABLE_SPEAKER) || defined(CONFIG_FIGURE_ENABLE_MICROPHONE)
#include "driver/i2s_std.h"
#endif
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"
#include "protocomm_security.h"
#include "rc522.h"
#include "sdkconfig.h"

#define WIFI_CONNECTED_BIT BIT0
#define HTTP_RESPONSE_CAPACITY 4096
#define ACCESS_TOKEN_CAPACITY 80
#define AUDIO_DOWNLOAD_MAX_BYTES (8U * 1024U * 1024U)
#define MICROPHONE_SAMPLE_RATE 16000U
#define MICROPHONE_MAX_RECORD_MS 10000U
#define WAV_HEADER_BYTES 44U
#define VOLUME_STEP 5U
#define API_BASE_URL_CAPACITY 192U
#define PROVISIONING_CONFIG_ENDPOINT "yuzhou-config"

typedef enum {
    DEVICE_STATE_BOOTING = 0,
    DEVICE_STATE_CONNECTING,
    DEVICE_STATE_IDLE,
    DEVICE_STATE_LISTENING,
    DEVICE_STATE_UPLOADING,
    DEVICE_STATE_THINKING,
    DEVICE_STATE_SPEAKING,
    DEVICE_STATE_REMINDER,
    DEVICE_STATE_ALARM,
    DEVICE_STATE_ERROR,
} device_state_t;

static const char *TAG = "figure_device";
static char api_base_url[API_BASE_URL_CAPACITY] = CONFIG_FIGURE_API_BASE_URL;
static led_strip_handle_t rgb_led;
static EventGroupHandle_t wifi_events;
static char access_token[ACCESS_TOKEN_CAPACITY];
static uint8_t current_volume = 60;
static uint8_t volume_before_mute = 55;
static int pending_command_count = 0;
static bool device_bound = false;
static volatile bool playback_stop_requested = false;
static char active_alarm_id[40];
static volatile bool sleep_mode_enabled = false;
static volatile device_state_t device_state = DEVICE_STATE_BOOTING;
static volatile TickType_t device_state_since = 0;
static volatile bool talk_button_requested = false;
static volatile bool talk_button_down = false;
static TaskHandle_t figure_network_task_handle;
static bool wifi_handler_registered = false;

#ifdef CONFIG_FIGURE_ENABLE_CONTROL_BUTTONS
typedef enum {
    DEVICE_BUTTON_EVENT_NONE = 0,
    DEVICE_BUTTON_EVENT_VOLUME_UP = BIT0,
    DEVICE_BUTTON_EVENT_VOLUME_DOWN = BIT1,
    DEVICE_BUTTON_EVENT_MUTE_TOGGLE = BIT2,
    DEVICE_BUTTON_EVENT_STOP_PLAYBACK = BIT3,
    DEVICE_BUTTON_EVENT_SNOOZE = BIT4,
    DEVICE_BUTTON_EVENT_SLEEP_TOGGLE = BIT5,
} device_button_event_t;

static volatile uint32_t pending_button_events = 0;
#endif

#ifdef CONFIG_FIGURE_ENABLE_NFC
typedef struct {
    char type[24];
    char uid[2 * RC522_UID_MAX_LENGTH + 1];
    uint8_t uid_length;
} nfc_event_t;

static QueueHandle_t nfc_event_queue;
#endif

static void device_set_state(device_state_t next_state);
static bool save_u8(const char *key, uint8_t value);
static bool save_string(const char *key, const char *value);

#ifdef CONFIG_FIGURE_ENABLE_DISPLAY
static bool display_ready = false;
static SemaphoreHandle_t display_mutex;
static SemaphoreHandle_t display_transfer_done;
static uint16_t *display_framebuffer;
static uint16_t display_transfer_buffer[CONFIG_FIGURE_DISPLAY_WIDTH * 16];
static uint16_t rgb565(uint8_t red, uint8_t green, uint8_t blue);
static void display_fill_rect(int x, int y, int width, int height, uint16_t color);
static void display_flush_rect(int x, int y, int width, int height);
static void display_draw_text(int x, int y, const char *text, uint16_t color,
                              uint16_t background, int scale);
static void display_render_status(const char *line1);
static void display_render_nfc(const char *uid, bool present);
static void display_render_state_animation(device_state_t state, unsigned frame);
#endif

#ifdef CONFIG_FIGURE_ENABLE_SPEAKER
#define AUDIO_BASE_PATH "/audio"
#define PROMPT_SETUP_PATH AUDIO_BASE_PATH "/setup_prompt.wav"
#define PROMPT_VOLUME_UP_PATH AUDIO_BASE_PATH "/prompts/volume_up.wav"
#define PROMPT_VOLUME_DOWN_PATH AUDIO_BASE_PATH "/prompts/volume_down.wav"
#define PROMPT_MUTED_PATH AUDIO_BASE_PATH "/prompts/muted.wav"
#define PROMPT_UNMUTED_PATH AUDIO_BASE_PATH "/prompts/unmuted.wav"
#define PROMPT_STOP_PLAYBACK_PATH AUDIO_BASE_PATH "/prompts/stop_playback.wav"
#define PROMPT_SNOOZE_PATH AUDIO_BASE_PATH "/prompts/snooze.wav"
#define PROMPT_SLEEP_ON_PATH AUDIO_BASE_PATH "/prompts/sleep_on.wav"
#define PROMPT_SLEEP_OFF_PATH AUDIO_BASE_PATH "/prompts/sleep_off.wav"

static i2s_chan_handle_t speaker_tx_channel;
static SemaphoreHandle_t speaker_mutex;
static uint32_t speaker_sample_rate = 16000;
static bool audio_storage_mounted = false;

static const int16_t sine32[] = {
    0, 195, 383, 556, 707, 831, 924, 981,
    1000, 981, 924, 831, 707, 556, 383, 195,
    0, -195, -383, -556, -707, -831, -924, -981,
    -1000, -981, -924, -831, -707, -556, -383, -195,
};

static void speaker_write_silence(uint32_t duration_ms)
{
    int16_t samples[128 * 2] = {0};
    const size_t frames = (16000U * duration_ms) / 1000U;
    size_t remaining = frames;
    while (remaining > 0) {
        const size_t block_frames = remaining > 128 ? 128 : remaining;
        size_t bytes_written = 0;
        ESP_ERROR_CHECK(i2s_channel_write(speaker_tx_channel, samples,
                                          block_frames * 2 * sizeof(int16_t),
                                          &bytes_written, portMAX_DELAY));
        remaining -= block_frames;
    }
}

static void speaker_play_tone(uint32_t frequency_hz, uint32_t duration_ms)
{
    int16_t samples[128 * 2];
    uint32_t phase = 0;
    const uint32_t phase_step = (uint32_t)(((uint64_t)frequency_hz << 32) / 16000U);
    const size_t frames = (16000U * duration_ms) / 1000U;
    size_t remaining = frames;

    while (remaining > 0) {
        const size_t block_frames = remaining > 128 ? 128 : remaining;
        for (size_t i = 0; i < block_frames; ++i) {
            const int16_t sample = sine32[phase >> 27];
            samples[i * 2] = sample;
            samples[i * 2 + 1] = sample;
            phase += phase_step;
        }
        size_t bytes_written = 0;
        ESP_ERROR_CHECK(i2s_channel_write(speaker_tx_channel, samples,
                                          block_frames * 2 * sizeof(int16_t),
                                          &bytes_written, portMAX_DELAY));
        remaining -= block_frames;
    }
}

static void configure_speaker(void)
{
    ESP_LOGI(TAG, "Initializing HT517 I2S speaker: BCLK=%d LRCK=%d DATA=%d",
             CONFIG_FIGURE_SPEAKER_PIN_BCLK,
             CONFIG_FIGURE_SPEAKER_PIN_LRCK,
             CONFIG_FIGURE_SPEAKER_PIN_DATA);

    const i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&channel_config, &speaker_tx_channel, NULL));
    speaker_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(speaker_mutex == NULL ? ESP_ERR_NO_MEM : ESP_OK);

    const i2s_std_config_t standard_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_FIGURE_SPEAKER_PIN_BCLK,
            .ws = CONFIG_FIGURE_SPEAKER_PIN_LRCK,
            .dout = CONFIG_FIGURE_SPEAKER_PIN_DATA,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(speaker_tx_channel, &standard_config));
    ESP_ERROR_CHECK(i2s_channel_enable(speaker_tx_channel));

    speaker_write_silence(100);
    speaker_play_tone(523, 180);
    speaker_write_silence(70);
    speaker_play_tone(659, 180);
    speaker_write_silence(70);
    speaker_play_tone(784, 220);
    speaker_write_silence(120);
    ESP_LOGI(TAG, "HT517 speaker self-test finished");
}

static void configure_audio_storage(void)
{
    const esp_vfs_spiffs_conf_t config = {
        .base_path = AUDIO_BASE_PATH,
        .partition_label = "audio",
        .max_files = 8,
        .format_if_mount_failed = false,
    };
    const esp_err_t error = esp_vfs_spiffs_register(&config);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Audio SPIFFS mount failed: %s", esp_err_to_name(error));
        audio_storage_mounted = false;
        return;
    }

    size_t total = 0;
    size_t used = 0;
    if (esp_spiffs_info("audio", &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "Audio SPIFFS mounted: used=%u total=%u",
                 (unsigned)used, (unsigned)total);
    } else {
        ESP_LOGI(TAG, "Audio SPIFFS mounted");
    }
    audio_storage_mounted = true;
}

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static bool speaker_set_sample_rate(uint32_t sample_rate)
{
    if (sample_rate == speaker_sample_rate) return true;

    esp_err_t error = i2s_channel_disable(speaker_tx_channel);
    if (error != ESP_OK) return false;
    const i2s_std_clk_config_t clock_config = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    error = i2s_channel_reconfig_std_clock(speaker_tx_channel, &clock_config);
    if (error == ESP_OK) error = i2s_channel_enable(speaker_tx_channel);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Speaker sample-rate change failed: %s", esp_err_to_name(error));
        return false;
    }
    speaker_sample_rate = sample_rate;
    return true;
}

static bool speaker_play_wav(const uint8_t *wav, size_t wav_length)
{
    if (wav_length < 44 || memcmp(wav, "RIFF", 4) != 0 || memcmp(wav + 8, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "Downloaded audio is not a RIFF/WAVE file");
        return false;
    }

    uint16_t audio_format = 0;
    uint16_t channels = 0;
    uint16_t bits_per_sample = 0;
    uint32_t sample_rate = 0;
    const uint8_t *pcm = NULL;
    size_t pcm_length = 0;
    size_t offset = 12;
    while (offset + 8 <= wav_length) {
        const uint32_t chunk_length = read_le32(wav + offset + 4);
        const size_t data_offset = offset + 8;
        if (memcmp(wav + offset, "fmt ", 4) == 0 && chunk_length >= 16) {
            if (chunk_length > wav_length - data_offset) break;
            audio_format = read_le16(wav + data_offset);
            channels = read_le16(wav + data_offset + 2);
            sample_rate = read_le32(wav + data_offset + 4);
            bits_per_sample = read_le16(wav + data_offset + 14);
        } else if (memcmp(wav + offset, "data", 4) == 0) {
            pcm = wav + data_offset;
            // DashScope returns a streaming-style WAV whose RIFF/data lengths
            // use a large sentinel value. The downloaded HTTP body is complete,
            // so in that case the remaining bytes are the actual PCM payload.
            const size_t available = wav_length - data_offset;
            pcm_length = chunk_length <= available ? chunk_length : available;
            break;
        } else if (chunk_length > wav_length - data_offset) {
            break;
        }
        offset = data_offset + chunk_length + (chunk_length & 1U);
    }

    if (audio_format != 1 || (channels != 1 && channels != 2) || bits_per_sample != 16 ||
        sample_rate < 8000 || sample_rate > 48000 || pcm == NULL || pcm_length == 0) {
        ESP_LOGE(TAG, "Unsupported WAV: format=%u channels=%u rate=%" PRIu32 " bits=%u bytes=%u",
                 audio_format, channels, sample_rate, bits_per_sample, (unsigned)pcm_length);
        return false;
    }
    if (!speaker_set_sample_rate(sample_rate)) return false;

    int16_t output[256 * 2];
    const size_t input_frame_bytes = channels * sizeof(int16_t);
    size_t position = 0;
    while (position + input_frame_bytes <= pcm_length) {
        if (playback_stop_requested) {
            ESP_LOGI(TAG, "Playback stopped by function button");
            speaker_write_silence(60);
            return true;
        }
        const size_t remaining_frames = (pcm_length - position) / input_frame_bytes;
        const size_t frame_count = remaining_frames > 256 ? 256 : remaining_frames;
        for (size_t frame = 0; frame < frame_count; ++frame) {
            const uint8_t *input = pcm + position + frame * input_frame_bytes;
            int16_t left = (int16_t)read_le16(input);
            int16_t right = channels == 2 ? (int16_t)read_le16(input + 2) : left;
            output[frame * 2] = (int16_t)(((int32_t)left * current_volume) / 100);
            output[frame * 2 + 1] = (int16_t)(((int32_t)right * current_volume) / 100);
        }
        size_t bytes_written = 0;
        const esp_err_t error = i2s_channel_write(speaker_tx_channel, output,
                                                   frame_count * 2 * sizeof(int16_t),
                                                   &bytes_written, portMAX_DELAY);
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "Speaker write failed: %s", esp_err_to_name(error));
            return false;
        }
        position += frame_count * input_frame_bytes;
    }
    return true;
}

static bool speaker_play_wav_file_unlocked(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        ESP_LOGW(TAG, "Prompt file not found: %s", path);
        return false;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    const long file_size = ftell(file);
    if (file_size <= 0 || file_size > (long)AUDIO_DOWNLOAD_MAX_BYTES ||
        fseek(file, 0, SEEK_SET) != 0) {
        ESP_LOGW(TAG, "Invalid prompt file size: %s size=%ld", path, file_size);
        fclose(file);
        return false;
    }

    uint8_t *audio = heap_caps_malloc((size_t)file_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (audio == NULL) audio = heap_caps_malloc((size_t)file_size, MALLOC_CAP_8BIT);
    if (audio == NULL) {
        ESP_LOGE(TAG, "Cannot allocate %ld bytes for prompt", file_size);
        fclose(file);
        return false;
    }

    const size_t read_bytes = fread(audio, 1, (size_t)file_size, file);
    fclose(file);
    if (read_bytes != (size_t)file_size) {
        ESP_LOGW(TAG, "Incomplete prompt read: %s expected=%ld actual=%u",
                 path, file_size, (unsigned)read_bytes);
        free(audio);
        return false;
    }

    const bool played = speaker_play_wav(audio, read_bytes);
    free(audio);
    return played;
}

static bool speaker_play_prompt_file(const char *path, TickType_t wait_ticks)
{
    if (!audio_storage_mounted || speaker_mutex == NULL) return false;
    if (xSemaphoreTake(speaker_mutex, wait_ticks) != pdTRUE) return false;

    playback_stop_requested = false;
    const bool played = speaker_play_wav_file_unlocked(path);
    speaker_write_silence(80);
    xSemaphoreGive(speaker_mutex);
    return played;
}

static void speaker_play_setup_prompt(void)
{
    ESP_LOGI(TAG, "Playing local setup voice prompt");
    if (!speaker_play_prompt_file(PROMPT_SETUP_PATH, pdMS_TO_TICKS(300))) {
        ESP_LOGW(TAG, "Setup voice prompt failed; falling back to chime");
        if (speaker_mutex != NULL &&
            xSemaphoreTake(speaker_mutex, pdMS_TO_TICKS(300)) != pdTRUE) {
            return;
        }
        playback_stop_requested = false;
        speaker_set_sample_rate(16000);
        speaker_play_tone(659, 120);
        speaker_write_silence(50);
        speaker_play_tone(880, 160);
        speaker_write_silence(50);
        speaker_play_tone(1047, 220);
        speaker_write_silence(80);
        if (speaker_mutex != NULL) xSemaphoreGive(speaker_mutex);
    }
}

static bool download_and_play_audio(const char *path, device_state_t playback_state)
{
    extern const uint8_t globalsign_root_r3_pem_start[]
        asm("_binary_globalsign_root_r3_pem_start");
    char url[1024];
    if (path == NULL) {
        ESP_LOGE(TAG, "Invalid audio path");
        return false;
    }
    const bool is_absolute = strncmp(path, "http://", 7) == 0 ||
                             strncmp(path, "https://", 8) == 0;
    const int url_length = is_absolute
        ? snprintf(url, sizeof(url), "%s", path)
        : snprintf(url, sizeof(url), "%s%s", api_base_url, path);
    if ((!is_absolute && path[0] != '/') || url_length < 0 || url_length >= (int)sizeof(url)) {
        ESP_LOGE(TAG, "Invalid or oversized audio URL");
        return false;
    }
    const bool is_https = strncmp(url, "https://", 8) == 0;
    const bool is_cos_https = is_https && strstr(url, ".cos.") != NULL &&
                              strstr(url, ".myqcloud.com") != NULL;

    playback_stop_requested = false;
    device_set_state(playback_state);
    uint8_t *audio = NULL;
    int received = 0;
    int64_t content_length = -1;
    for (unsigned attempt = 1; attempt <= 3; ++attempt) {
        esp_http_client_config_t config = {
            .url = url,
            .timeout_ms = 12000,
            .buffer_size = 4096,
            .cert_pem = is_cos_https
                ? (const char *)globalsign_root_r3_pem_start
                : NULL,
            .crt_bundle_attach = is_https && !is_cos_https
                ? esp_crt_bundle_attach
                : NULL,
        };
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client == NULL) break;
        if (!is_absolute) {
            char authorization[112];
            snprintf(authorization, sizeof(authorization), "Bearer %s", access_token);
            esp_http_client_set_header(client, "Authorization", authorization);
        }

        const esp_err_t error = esp_http_client_open(client, 0);
        content_length = error == ESP_OK ? esp_http_client_fetch_headers(client) : -1;
        const int status = error == ESP_OK ? esp_http_client_get_status_code(client) : -1;
        if (error != ESP_OK || status != 200 || content_length <= 0 ||
            content_length > AUDIO_DOWNLOAD_MAX_BYTES) {
            ESP_LOGW(TAG, "Audio request %u/3 failed: error=%s status=%d bytes=%lld",
                     attempt, esp_err_to_name(error), status, (long long)content_length);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        if (audio == NULL) {
            audio = heap_caps_malloc((size_t)content_length,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (audio == NULL) {
                audio = heap_caps_malloc((size_t)content_length, MALLOC_CAP_8BIT);
            }
        }
        if (audio == NULL) {
            ESP_LOGE(TAG, "Cannot allocate %lld bytes for audio", (long long)content_length);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return false;
        }

        received = 0;
        while (received < content_length) {
            const int chunk = esp_http_client_read(
                client,
                (char *)audio + received,
                (int)(content_length - received));
            if (chunk <= 0) break;
            received += chunk;
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        if (received == content_length) break;
        ESP_LOGW(TAG, "Audio request %u/3 incomplete: expected=%lld received=%d",
                 attempt, (long long)content_length, received);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    if (audio == NULL || received != content_length) {
        ESP_LOGE(TAG, "Audio download failed after retries: expected=%lld received=%d",
                 (long long)content_length, received);
        free(audio);
        return false;
    }

    bool played = false;
    if (speaker_mutex == NULL || xSemaphoreTake(speaker_mutex, portMAX_DELAY) == pdTRUE) {
        played = speaker_play_wav(audio, (size_t)received);
        if (speaker_mutex != NULL) xSemaphoreGive(speaker_mutex);
    }
    free(audio);
    return played;
}
#endif

#ifdef CONFIG_FIGURE_ENABLE_MICROPHONE
static i2s_chan_handle_t microphone_rx_channel;

#ifdef CONFIG_FIGURE_ENABLE_DISPLAY
static void display_render_microphone_level(unsigned level)
{
    if (!display_ready) return;
    if (xSemaphoreTake(display_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return;

    const uint16_t background = rgb565(20, 16, 32);
    const uint16_t track = rgb565(58, 52, 72);
    const uint16_t green = rgb565(40, 190, 112);
    const uint16_t ink = rgb565(242, 238, 255);
    char line[20];

    if (level > 100) level = 100;
    display_fill_rect(16, 104, 208, 32, track);
    if (level > 0) display_fill_rect(16, 104, (208 * level) / 100, 32, green);
    display_fill_rect(16, 148, 208, 24, background);
    snprintf(line, sizeof(line), "MIC:%3u", level);
    display_draw_text(16, 148, line, ink, background, 2);
    display_flush_rect(16, 104, 208, 68);
    xSemaphoreGive(display_mutex);
}
#endif

static uint32_t microphone_abs_sample(int32_t sample)
{
    if (sample == INT32_MIN) return INT32_MAX;
    return sample < 0 ? (uint32_t)(-sample) : (uint32_t)sample;
}

static void configure_microphone(void)
{
    ESP_LOGI(TAG, "Initializing INMP441 I2S microphone: SCK=%d WS=%d SD=%d",
             CONFIG_FIGURE_MICROPHONE_PIN_SCK,
             CONFIG_FIGURE_MICROPHONE_PIN_WS,
             CONFIG_FIGURE_MICROPHONE_PIN_SD);

    const i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&channel_config, NULL, &microphone_rx_channel));

    const i2s_std_config_t standard_config = {
        // 16 kHz * 32 bits * 2 slots = 1.024 MHz BCLK. INMP441 sends
        // 24-bit samples inside 32-bit I2S slots.
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MICROPHONE_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_FIGURE_MICROPHONE_PIN_SCK,
            .ws = CONFIG_FIGURE_MICROPHONE_PIN_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = CONFIG_FIGURE_MICROPHONE_PIN_SD,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(microphone_rx_channel, &standard_config));
    ESP_ERROR_CHECK(i2s_channel_enable(microphone_rx_channel));
}

static void run_microphone_self_test(void)
{
    int32_t samples[128 * 2];
    ESP_LOGI(TAG, "Microphone level self-test started (5 seconds, 16 kHz)");
#ifdef CONFIG_FIGURE_ENABLE_DISPLAY
    display_render_status("MIC TEST");
#endif

    for (int iteration = 0; iteration < 12; ++iteration) {
        size_t bytes_read = 0;
        const esp_err_t error = i2s_channel_read(microphone_rx_channel, samples,
                                                 sizeof(samples), &bytes_read,
                                                 pdMS_TO_TICKS(1000));
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "Microphone read failed: %s", esp_err_to_name(error));
            continue;
        }

        uint32_t left_peak = 0;
        uint32_t right_peak = 0;
        const size_t frame_count = bytes_read / (2 * sizeof(int32_t));
        for (size_t frame = 0; frame < frame_count; ++frame) {
            const uint32_t left = microphone_abs_sample(samples[frame * 2]);
            const uint32_t right = microphone_abs_sample(samples[frame * 2 + 1]);
            if (left > left_peak) left_peak = left;
            if (right > right_peak) right_peak = right;
        }

        const uint32_t peak = left_peak > right_peak ? left_peak : right_peak;
        uint32_t level = (peak >> 16) * 100U / 12000U;
        if (level > 100U) level = 100U;
        ESP_LOGI(TAG,
                 "MIC bytes=%u frames=%u level=%" PRIu32
                 " left=0x%08" PRIx32 " right=0x%08" PRIx32,
                 (unsigned)bytes_read, (unsigned)frame_count, level,
                 left_peak, right_peak);
#ifdef CONFIG_FIGURE_ENABLE_DISPLAY
        display_render_microphone_level(level);
#endif
        vTaskDelay(pdMS_TO_TICKS(240));
    }

    ESP_LOGI(TAG, "Microphone level self-test finished");
#ifdef CONFIG_FIGURE_ENABLE_DISPLAY
    display_render_status("MIC OK");
#endif
}

static void write_le16(uint8_t *target, uint16_t value)
{
    target[0] = (uint8_t)value;
    target[1] = (uint8_t)(value >> 8);
}

static void write_le32(uint8_t *target, uint32_t value)
{
    target[0] = (uint8_t)value;
    target[1] = (uint8_t)(value >> 8);
    target[2] = (uint8_t)(value >> 16);
    target[3] = (uint8_t)(value >> 24);
}

static int16_t microphone_sample_to_pcm16(int32_t sample)
{
    // The INMP441 places its signed 24-bit value in the high bits of the
    // 32-bit I2S slot. Apply a conservative 2x digital gain for near-field
    // speech, then saturate to signed 16-bit PCM.
    int32_t value = sample >> 15;
    if (value > INT16_MAX) value = INT16_MAX;
    if (value < INT16_MIN) value = INT16_MIN;
    return (int16_t)value;
}

static bool record_microphone_wav(uint32_t duration_ms,
                                  bool stop_on_button_release,
                                  uint8_t **wav_out, size_t *wav_size_out)
{
    if (duration_ms < 1000U || duration_ms > MICROPHONE_MAX_RECORD_MS ||
        wav_out == NULL || wav_size_out == NULL) {
        return false;
    }

    const uint32_t max_frame_count =
        (MICROPHONE_SAMPLE_RATE * duration_ms) / 1000U;
    const size_t max_pcm_bytes =
        (size_t)max_frame_count * sizeof(int16_t);
    const size_t allocation_size = WAV_HEADER_BYTES + max_pcm_bytes;
    uint8_t *wav = heap_caps_malloc(allocation_size,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (wav == NULL) wav = heap_caps_malloc(allocation_size, MALLOC_CAP_8BIT);
    if (wav == NULL) {
        ESP_LOGE(TAG, "Unable to allocate %u bytes for microphone recording",
                 (unsigned)allocation_size);
        return false;
    }

    memcpy(wav, "RIFF", 4);
    memcpy(wav + 8, "WAVEfmt ", 8);
    write_le32(wav + 16, 16U);
    write_le16(wav + 20, 1U);
    write_le16(wav + 22, 1U);
    write_le32(wav + 24, MICROPHONE_SAMPLE_RATE);
    write_le32(wav + 28, MICROPHONE_SAMPLE_RATE * sizeof(int16_t));
    write_le16(wav + 32, sizeof(int16_t));
    write_le16(wav + 34, 16U);
    memcpy(wav + 36, "data", 4);

    int32_t input[128 * 2];
    size_t discarded = 0;
    (void)i2s_channel_read(microphone_rx_channel, input, sizeof(input),
                           &discarded, pdMS_TO_TICKS(100));

    int16_t *pcm = (int16_t *)(wav + WAV_HEADER_BYTES);
    uint32_t written_frames = 0;
    uint32_t active_run_frames = 0;
    uint32_t silence_frames = 0;
    bool speech_detected = false;
    unsigned meter_divider = 0;
    uint64_t calibration_level_sum = 0;
    uint32_t calibration_blocks = 0;
    uint32_t noise_floor = 0;
    uint32_t vad_threshold = CONFIG_FIGURE_VAD_THRESHOLD;
    const uint32_t calibration_frames = MICROPHONE_SAMPLE_RATE / 3U;
    const uint32_t start_required_frames = MICROPHONE_SAMPLE_RATE / 25U;
    const uint32_t silence_limit_frames =
        (MICROPHONE_SAMPLE_RATE * CONFIG_FIGURE_VAD_SILENCE_MS) / 1000U;
    const uint32_t start_timeout_frames =
        (MICROPHONE_SAMPLE_RATE * CONFIG_FIGURE_VAD_START_TIMEOUT_MS) / 1000U;
    const uint32_t minimum_frames =
        (MICROPHONE_SAMPLE_RATE * CONFIG_FIGURE_VAD_MIN_RECORD_MS) / 1000U;

    while (written_frames < max_frame_count) {
        size_t bytes_read = 0;
        const esp_err_t error = i2s_channel_read(
            microphone_rx_channel, input, sizeof(input), &bytes_read,
            pdMS_TO_TICKS(1000));
        if (error != ESP_OK || bytes_read == 0) {
            ESP_LOGE(TAG, "Microphone recording read failed: %s",
                     esp_err_to_name(error));
            free(wav);
            return false;
        }

        const size_t available_frames = bytes_read / (2U * sizeof(int32_t));
        const uint32_t remaining_frames = max_frame_count - written_frames;
        const size_t copy_frames = available_frames < remaining_frames
                                       ? available_frames
                                       : remaining_frames;
        uint64_t absolute_sum = 0;
        uint32_t peak = 0;
        for (size_t frame = 0; frame < copy_frames; ++frame) {
            const int16_t sample = microphone_sample_to_pcm16(input[frame * 2]);
            pcm[written_frames++] = sample;
            const uint32_t absolute = sample == INT16_MIN
                                          ? INT16_MAX
                                          : (uint32_t)(sample < 0 ? -sample : sample);
            absolute_sum += absolute;
            if (absolute > peak) peak = absolute;
        }

        const uint32_t average = copy_frames > 0
                                     ? (uint32_t)(absolute_sum / copy_frames)
                                     : 0;
        if (!speech_detected && written_frames <= calibration_frames) {
            calibration_level_sum += average;
            calibration_blocks++;
            noise_floor = calibration_blocks > 0
                              ? (uint32_t)(calibration_level_sum /
                                           calibration_blocks)
                              : 0;
            const uint32_t adaptive_threshold = noise_floor * 2U + 150U;
            vad_threshold = adaptive_threshold > CONFIG_FIGURE_VAD_THRESHOLD
                                ? adaptive_threshold
                                : CONFIG_FIGURE_VAD_THRESHOLD;
        } else if (!speech_detected && average < vad_threshold) {
            // Follow slow changes in fan/room noise without letting a short
            // speech burst immediately raise the threshold.
            noise_floor = (noise_floor * 31U + average) / 32U;
            const uint32_t adaptive_threshold = noise_floor * 2U + 150U;
            vad_threshold = adaptive_threshold > CONFIG_FIGURE_VAD_THRESHOLD
                                ? adaptive_threshold
                                : CONFIG_FIGURE_VAD_THRESHOLD;
        }

        const bool calibration_done = written_frames > calibration_frames;
        const bool active = calibration_done && average >= vad_threshold;
        if (!speech_detected) {
            active_run_frames = active ? active_run_frames + copy_frames : 0;
            if (active_run_frames >= start_required_frames) {
                speech_detected = true;
                silence_frames = 0;
                ESP_LOGI(TAG, "VAD speech started at %" PRIu32
                              "ms, avg=%" PRIu32 ", threshold=%" PRIu32,
                         (written_frames * 1000U) / MICROPHONE_SAMPLE_RATE,
                         average, vad_threshold);
            }
        } else if (active) {
            silence_frames = 0;
        } else {
            silence_frames += copy_frames;
        }

#ifdef CONFIG_FIGURE_ENABLE_DISPLAY
        if (++meter_divider >= 12U) {
            meter_divider = 0;
            unsigned level = (unsigned)((peak * 100U) / 12000U);
            if (level > 100U) level = 100U;
            display_render_microphone_level(level);
        }
#endif

        if (stop_on_button_release && written_frames >= minimum_frames) {
#ifdef CONFIG_FIGURE_ENABLE_TALK_BUTTON
            if (!talk_button_down) {
                ESP_LOGI(TAG, "Push-to-talk stopped on button release");
                break;
            }
#endif
        } else if (written_frames >= minimum_frames) {
            if (speech_detected && silence_frames >= silence_limit_frames) {
                ESP_LOGI(TAG, "VAD stopped after %dms silence",
                         CONFIG_FIGURE_VAD_SILENCE_MS);
                break;
            }
            if (!speech_detected && written_frames >= start_timeout_frames) {
                ESP_LOGI(TAG, "VAD stopped: no speech detected (noise=%" PRIu32
                              ", threshold=%" PRIu32 ")",
                         noise_floor, vad_threshold);
                break;
            }
        }
    }

    const size_t pcm_bytes = (size_t)written_frames * sizeof(int16_t);
    const size_t wav_size = WAV_HEADER_BYTES + pcm_bytes;
    write_le32(wav + 4, (uint32_t)wav_size - 8U);
    write_le32(wav + 40, (uint32_t)pcm_bytes);
    *wav_out = wav;
    *wav_size_out = wav_size;
    ESP_LOGI(TAG, "Microphone recording ready: duration=%" PRIu32
                  "ms bytes=%u frames=%" PRIu32 " speech=%s",
             (written_frames * 1000U) / MICROPHONE_SAMPLE_RATE,
             (unsigned)wav_size, written_frames,
             speech_detected ? "yes" : "no");
    return true;
}
#endif

#ifdef CONFIG_FIGURE_ENABLE_DISPLAY
static esp_lcd_panel_handle_t lcd_panel;

static const uint8_t font5x7[][5] = {
    [' '] = {0x00, 0x00, 0x00, 0x00, 0x00},
    ['-'] = {0x08, 0x08, 0x08, 0x08, 0x08},
    ['.'] = {0x00, 0x60, 0x60, 0x00, 0x00},
    ['0'] = {0x3e, 0x51, 0x49, 0x45, 0x3e},
    ['1'] = {0x00, 0x42, 0x7f, 0x40, 0x00},
    ['2'] = {0x42, 0x61, 0x51, 0x49, 0x46},
    ['3'] = {0x21, 0x41, 0x45, 0x4b, 0x31},
    ['4'] = {0x18, 0x14, 0x12, 0x7f, 0x10},
    ['5'] = {0x27, 0x45, 0x45, 0x45, 0x39},
    ['6'] = {0x3c, 0x4a, 0x49, 0x49, 0x30},
    ['7'] = {0x01, 0x71, 0x09, 0x05, 0x03},
    ['8'] = {0x36, 0x49, 0x49, 0x49, 0x36},
    ['9'] = {0x06, 0x49, 0x49, 0x29, 0x1e},
    [':'] = {0x00, 0x36, 0x36, 0x00, 0x00},
    ['A'] = {0x7e, 0x11, 0x11, 0x11, 0x7e},
    ['B'] = {0x7f, 0x49, 0x49, 0x49, 0x36},
    ['C'] = {0x3e, 0x41, 0x41, 0x41, 0x22},
    ['D'] = {0x7f, 0x41, 0x41, 0x22, 0x1c},
    ['E'] = {0x7f, 0x49, 0x49, 0x49, 0x41},
    ['F'] = {0x7f, 0x09, 0x09, 0x09, 0x01},
    ['G'] = {0x3e, 0x41, 0x49, 0x49, 0x7a},
    ['H'] = {0x7f, 0x08, 0x08, 0x08, 0x7f},
    ['I'] = {0x00, 0x41, 0x7f, 0x41, 0x00},
    ['J'] = {0x20, 0x40, 0x41, 0x3f, 0x01},
    ['K'] = {0x7f, 0x08, 0x14, 0x22, 0x41},
    ['L'] = {0x7f, 0x40, 0x40, 0x40, 0x40},
    ['M'] = {0x7f, 0x02, 0x0c, 0x02, 0x7f},
    ['N'] = {0x7f, 0x04, 0x08, 0x10, 0x7f},
    ['O'] = {0x3e, 0x41, 0x41, 0x41, 0x3e},
    ['P'] = {0x7f, 0x09, 0x09, 0x09, 0x06},
    ['Q'] = {0x3e, 0x41, 0x51, 0x21, 0x5e},
    ['R'] = {0x7f, 0x09, 0x19, 0x29, 0x46},
    ['S'] = {0x46, 0x49, 0x49, 0x49, 0x31},
    ['T'] = {0x01, 0x01, 0x7f, 0x01, 0x01},
    ['U'] = {0x3f, 0x40, 0x40, 0x40, 0x3f},
    ['V'] = {0x1f, 0x20, 0x40, 0x20, 0x1f},
    ['W'] = {0x3f, 0x40, 0x38, 0x40, 0x3f},
    ['X'] = {0x63, 0x14, 0x08, 0x14, 0x63},
    ['Y'] = {0x07, 0x08, 0x70, 0x08, 0x07},
    ['Z'] = {0x61, 0x51, 0x49, 0x45, 0x43},
};
#endif

typedef struct {
    char data[HTTP_RESPONSE_CAPACITY];
    size_t length;
    bool overflow;
} http_response_t;

static http_response_t http_response;

#ifdef CONFIG_FIGURE_ENABLE_DISPLAY
static uint16_t rgb565(uint8_t red, uint8_t green, uint8_t blue)
{
    return ((red & 0xf8) << 8) | ((green & 0xfc) << 3) | (blue >> 3);
}

static bool display_color_transfer_done(esp_lcd_panel_io_handle_t panel_io,
                                        esp_lcd_panel_io_event_data_t *event_data,
                                        void *user_context)
{
    (void)panel_io;
    (void)event_data;
    (void)user_context;
    BaseType_t higher_priority_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(display_transfer_done, &higher_priority_task_woken);
    return higher_priority_task_woken == pdTRUE;
}

static void display_fill_rect(int x, int y, int width, int height, uint16_t color)
{
    if (display_framebuffer == NULL || width <= 0 || height <= 0) return;
    if (x < 0) {
        width += x;
        x = 0;
    }
    if (y < 0) {
        height += y;
        y = 0;
    }
    if (x + width > CONFIG_FIGURE_DISPLAY_WIDTH) {
        width = CONFIG_FIGURE_DISPLAY_WIDTH - x;
    }
    if (y + height > CONFIG_FIGURE_DISPLAY_HEIGHT) {
        height = CONFIG_FIGURE_DISPLAY_HEIGHT - y;
    }
    if (width <= 0 || height <= 0) return;

    for (int row = 0; row < height; ++row) {
        uint16_t *target = display_framebuffer +
                           (y + row) * CONFIG_FIGURE_DISPLAY_WIDTH + x;
        for (int col = 0; col < width; ++col) target[col] = color;
    }
}

static void display_flush_rect(int x, int y, int width, int height)
{
    if (!display_ready || display_framebuffer == NULL ||
        width <= 0 || height <= 0) return;

    for (int offset = 0; offset < height; offset += 16) {
        const int rows = height - offset < 16 ? height - offset : 16;
        for (int row = 0; row < rows; ++row) {
            memcpy(display_transfer_buffer + row * width,
                   display_framebuffer +
                       (y + offset + row) * CONFIG_FIGURE_DISPLAY_WIDTH + x,
                   width * sizeof(uint16_t));
        }
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(
            lcd_panel, x, y + offset, x + width, y + offset + rows,
            display_transfer_buffer));
        ESP_ERROR_CHECK(xSemaphoreTake(display_transfer_done,
                                       pdMS_TO_TICKS(500)) == pdTRUE
                            ? ESP_OK
                            : ESP_ERR_TIMEOUT);
    }
}

static void display_draw_char(int x, int y, char value, uint16_t color, uint16_t background, int scale)
{
    if (value >= 'a' && value <= 'z') value = (char)(value - 'a' + 'A');
    if ((unsigned char)value >= sizeof(font5x7) / sizeof(font5x7[0])) value = ' ';

    for (int col = 0; col < 5; ++col) {
        const uint8_t bits = font5x7[(unsigned char)value][col];
        for (int row = 0; row < 7; ++row) {
            display_fill_rect(x + col * scale, y + row * scale, scale, scale,
                              (bits & (1 << row)) ? color : background);
        }
    }
    display_fill_rect(x + 5 * scale, y, scale, 7 * scale, background);
}

static void display_draw_text(int x, int y, const char *text, uint16_t color, uint16_t background, int scale)
{
    while (*text != '\0') {
        display_draw_char(x, y, *text, color, background, scale);
        x += 6 * scale;
        ++text;
    }
}

static void display_render_status(const char *line1)
{
    if (!display_ready) return;
    if (xSemaphoreTake(display_mutex, pdMS_TO_TICKS(300)) != pdTRUE) return;

    const uint16_t background = rgb565(20, 16, 32);
    const uint16_t ink = rgb565(242, 238, 255);
    const uint16_t muted = rgb565(174, 166, 194);
    const uint16_t green = rgb565(40, 190, 112);
    const uint16_t purple = rgb565(118, 86, 232);

    display_fill_rect(0, 0, CONFIG_FIGURE_DISPLAY_WIDTH, CONFIG_FIGURE_DISPLAY_HEIGHT, background);
    display_fill_rect(0, 0, CONFIG_FIGURE_DISPLAY_WIDTH, 44, purple);
    display_draw_text(16, 14, "FIGURE", ink, purple, 2);
    display_draw_text(16, 64, line1, ink, background, 2);

    char line[32];
    snprintf(line, sizeof(line), "VOL:%u", current_volume);
    display_draw_text(16, 108, line, muted, background, 2);
    snprintf(line, sizeof(line), "PENDING:%d", pending_command_count);
    display_draw_text(16, 138, line, muted, background, 2);
    display_draw_text(16, 178, device_bound ? "BOUND:YES" : "BOUND:NO", green, background, 2);
    display_flush_rect(0, 0, CONFIG_FIGURE_DISPLAY_WIDTH,
                       CONFIG_FIGURE_DISPLAY_HEIGHT);
    xSemaphoreGive(display_mutex);
}

static void display_render_nfc(const char *uid, bool present)
{
    if (!display_ready) return;
    if (xSemaphoreTake(display_mutex, pdMS_TO_TICKS(300)) != pdTRUE) return;

    const uint16_t background = rgb565(20, 16, 32);
    const uint16_t ink = rgb565(242, 238, 255);
    const uint16_t muted = rgb565(174, 166, 194);
    const uint16_t green = rgb565(40, 190, 112);
    const uint16_t purple = rgb565(118, 86, 232);

    display_fill_rect(0, 0, CONFIG_FIGURE_DISPLAY_WIDTH,
                      CONFIG_FIGURE_DISPLAY_HEIGHT, background);
    display_fill_rect(0, 0, CONFIG_FIGURE_DISPLAY_WIDTH, 44, purple);
    display_draw_text(16, 14, "NFC READER", ink, purple, 2);
    if (present) {
        display_draw_text(16, 64, "TAG PRESENT", green, background, 2);
        display_draw_text(16, 104, "UID:", muted, background, 2);
        display_draw_text(16, 136, uid == NULL ? "" : uid, ink, background, 2);
        display_draw_text(16, 178, "REMOVE TO CLEAR", muted, background, 1);
    } else {
        display_draw_text(16, 68, "NO FIGURE", green, background, 2);
        display_draw_text(16, 112, "TAG REMOVED", ink, background, 2);
        display_draw_text(16, 156, "WAITING FOR TAG", muted, background, 1);
    }
    display_flush_rect(0, 0, CONFIG_FIGURE_DISPLAY_WIDTH,
                       CONFIG_FIGURE_DISPLAY_HEIGHT);
    xSemaphoreGive(display_mutex);
}

static void display_render_nfc_match(const char *uid, const char *character_name,
                                     bool matched)
{
    if (!display_ready) return;
    if (xSemaphoreTake(display_mutex, pdMS_TO_TICKS(300)) != pdTRUE) return;

    const uint16_t background = rgb565(20, 16, 32);
    const uint16_t ink = rgb565(242, 238, 255);
    const uint16_t muted = rgb565(174, 166, 194);
    const uint16_t green = rgb565(40, 190, 112);
    const uint16_t orange = rgb565(245, 156, 72);
    const uint16_t purple = rgb565(118, 86, 232);

    display_fill_rect(0, 0, CONFIG_FIGURE_DISPLAY_WIDTH,
                      CONFIG_FIGURE_DISPLAY_HEIGHT, background);
    display_fill_rect(0, 0, CONFIG_FIGURE_DISPLAY_WIDTH, 44, purple);
    display_draw_text(16, 14, "AI FIGURE", ink, purple, 2);
    if (matched) {
        display_draw_text(16, 64, "ROLE:", green, background, 2);
        display_draw_text(16, 100,
                          character_name == NULL || character_name[0] == '\0'
                              ? "CHARACTER"
                              : character_name,
                          ink, background, 2);
        display_draw_text(16, 146, "UID:", muted, background, 1);
        display_draw_text(16, 166, uid == NULL ? "" : uid, muted, background, 1);
    } else {
        display_draw_text(16, 64, "UNBOUND TAG", orange, background, 2);
        display_draw_text(16, 108, "BIND IN APP", ink, background, 2);
        display_draw_text(16, 154, uid == NULL ? "" : uid, muted, background, 1);
    }
    display_flush_rect(0, 0, CONFIG_FIGURE_DISPLAY_WIDTH,
                       CONFIG_FIGURE_DISPLAY_HEIGHT);
    xSemaphoreGive(display_mutex);
}

static void display_render_state_animation(device_state_t state, unsigned frame)
{
    if (!display_ready) return;
    if (sleep_mode_enabled && state == DEVICE_STATE_IDLE) return;
    if (xSemaphoreTake(display_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;

    const uint16_t background = rgb565(20, 16, 32);
    const uint16_t purple = rgb565(118, 86, 232);
    const uint16_t cyan = rgb565(55, 196, 224);
    const uint16_t orange = rgb565(245, 158, 11);
    const uint16_t red = rgb565(224, 70, 70);
    const uint16_t green = rgb565(40, 190, 112);
    const int y = CONFIG_FIGURE_DISPLAY_HEIGHT - 25;
    const int width = CONFIG_FIGURE_DISPLAY_WIDTH - 32;
    display_fill_rect(16, y, width, 14, background);

    if (state == DEVICE_STATE_THINKING) {
        for (int dot = 0; dot < 3; ++dot) {
            const bool active = dot == (int)(frame % 3U);
            display_fill_rect(84 + dot * 28, y + (active ? 0 : 5), 14,
                              active ? 14 : 9, purple);
        }
    } else if (state == DEVICE_STATE_SPEAKING) {
        for (int bar = 0; bar < 9; ++bar) {
            const int height = 4 + (int)((frame + (unsigned)(bar * 2)) % 4U) * 3;
            display_fill_rect(38 + bar * 18, y + 14 - height, 10, height, cyan);
        }
    } else if (state == DEVICE_STATE_REMINDER) {
        const int position = (int)((frame * 14U) % (unsigned)(width - 30));
        display_fill_rect(16 + position, y + 3, 30, 8, orange);
    } else if (state == DEVICE_STATE_ALARM) {
        display_fill_rect(16, y + 2, width, 10,
                          (frame % 2U) == 0U ? red : orange);
    } else if (state == DEVICE_STATE_IDLE) {
        const bool blink = (frame % 18U) == 0U;
        display_fill_rect(82, y + (blink ? 7 : 3), 22, blink ? 3 : 8, green);
        display_fill_rect(136, y + (blink ? 7 : 3), 22, blink ? 3 : 8, green);
    } else if (state == DEVICE_STATE_ERROR) {
        display_fill_rect(16, y + 2, width, 10, red);
    }
    display_flush_rect(16, y, width, 14);
    xSemaphoreGive(display_mutex);
}

static void configure_display(void)
{
    ESP_LOGI(TAG, "Initializing ST7789 display");
    display_mutex = xSemaphoreCreateMutex();
    display_transfer_done = xSemaphoreCreateBinary();
    display_framebuffer = heap_caps_calloc(
        CONFIG_FIGURE_DISPLAY_WIDTH * CONFIG_FIGURE_DISPLAY_HEIGHT,
        sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(display_mutex == NULL || display_transfer_done == NULL ||
                            display_framebuffer == NULL
                        ? ESP_ERR_NO_MEM
                        : ESP_OK);

#if CONFIG_FIGURE_DISPLAY_PIN_BL >= 0
    gpio_config_t backlight_config = {
        .pin_bit_mask = 1ULL << CONFIG_FIGURE_DISPLAY_PIN_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&backlight_config));
    ESP_ERROR_CHECK(gpio_set_level(CONFIG_FIGURE_DISPLAY_PIN_BL, 1));
#endif

    spi_bus_config_t bus_config = {
        .sclk_io_num = CONFIG_FIGURE_DISPLAY_PIN_SCLK,
        .mosi_io_num = CONFIG_FIGURE_DISPLAY_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = CONFIG_FIGURE_DISPLAY_WIDTH * 80 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize((spi_host_device_t)CONFIG_FIGURE_DISPLAY_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = CONFIG_FIGURE_DISPLAY_PIN_DC,
        .cs_gpio_num = CONFIG_FIGURE_DISPLAY_PIN_CS,
        .pclk_hz = 20 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 1,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)CONFIG_FIGURE_DISPLAY_SPI_HOST, &io_config, &io_handle));
    const esp_lcd_panel_io_callbacks_t io_callbacks = {
        .on_color_trans_done = display_color_transfer_done,
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(
        io_handle, &io_callbacks, NULL));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = CONFIG_FIGURE_DISPLAY_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &lcd_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(lcd_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(lcd_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(lcd_panel, true));
    display_ready = true;
    display_render_status("BOOTING");
}
#endif

static void set_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    ESP_ERROR_CHECK(led_strip_set_pixel(rgb_led, 0, red, green, blue));
    ESP_ERROR_CHECK(led_strip_refresh(rgb_led));
}

static void pulse_rgb(uint8_t red, uint8_t green, uint8_t blue, uint32_t duration_ms)
{
    set_rgb(red, green, blue);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    ESP_ERROR_CHECK(led_strip_clear(rgb_led));
}

static void configure_rgb_led(void)
{
    const led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_BLINK_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };
    const led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 0,
        .flags.with_dma = false,
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &rgb_led));
    ESP_ERROR_CHECK(led_strip_clear(rgb_led));
}

#ifdef CONFIG_FIGURE_ENABLE_NFC
static bool nfc_uid_equal(const rc522_uid_t *left, const rc522_uid_t *right)
{
    return left->length == right->length &&
           memcmp(left->bytes, right->bytes, left->length) == 0;
}

static void queue_nfc_event(const char *type, const char *uid,
                            uint8_t uid_length)
{
    if (nfc_event_queue == NULL) return;
    nfc_event_t event = { .uid_length = uid_length };
    strlcpy(event.type, type, sizeof(event.type));
    strlcpy(event.uid, uid, sizeof(event.uid));
    if (xQueueSend(nfc_event_queue, &event, 0) != pdPASS) {
        ESP_LOGW(TAG, "NFC event queue full; dropped type=%s UID=%s", type,
                 uid);
    }
}

static void nfc_reader_task(void *arg)
{
    (void)arg;
    rc522_uid_t last_uid = {0};
    bool tag_present = false;
    unsigned missed_reads = 0;

    while (true) {
        rc522_uid_t uid = {0};
        const esp_err_t error = rc522_read_uid(&uid);
        if (error == ESP_OK) {
            missed_reads = 0;
            if (!tag_present || !nfc_uid_equal(&uid, &last_uid)) {
                char uid_log[3 * RC522_UID_MAX_LENGTH] = {0};
                char uid_screen[2 * RC522_UID_MAX_LENGTH + 1] = {0};
                rc522_format_uid(&uid, uid_log, sizeof(uid_log), true);
                rc522_format_uid(&uid, uid_screen, sizeof(uid_screen), false);
                ESP_LOGI(TAG, "NFC tag present: UID=%s (%u bytes)", uid_log,
                         (unsigned)uid.length);
                last_uid = uid;
                tag_present = true;
                queue_nfc_event("nfc_tag_present", uid_screen, uid.length);
#ifdef CONFIG_FIGURE_ENABLE_DISPLAY
                display_render_nfc(uid_screen, true);
#endif
            }
        } else if (error == ESP_ERR_NOT_FOUND || error == ESP_ERR_TIMEOUT) {
            if (tag_present && ++missed_reads >= 4) {
                char uid_log[3 * RC522_UID_MAX_LENGTH] = {0};
                char uid_canonical[2 * RC522_UID_MAX_LENGTH + 1] = {0};
                rc522_format_uid(&last_uid, uid_log, sizeof(uid_log), true);
                rc522_format_uid(&last_uid, uid_canonical,
                                 sizeof(uid_canonical), false);
                ESP_LOGI(TAG, "NFC tag removed: UID=%s", uid_log);
                queue_nfc_event("nfc_tag_removed", uid_canonical,
                                last_uid.length);
                memset(&last_uid, 0, sizeof(last_uid));
                tag_present = false;
                missed_reads = 0;
#ifdef CONFIG_FIGURE_ENABLE_DISPLAY
                display_render_nfc(NULL, false);
#endif
            }
        } else {
            ESP_LOGW(TAG, "NFC read failed: %s", esp_err_to_name(error));
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_FIGURE_NFC_POLL_INTERVAL_MS));
    }
}

static void configure_nfc_reader(void)
{
    nfc_event_queue = xQueueCreate(8, sizeof(nfc_event_t));
    if (nfc_event_queue == NULL) {
        ESP_LOGE(TAG, "Unable to create NFC event queue");
        return;
    }
    uint8_t version = 0;
    const esp_err_t error = rc522_init(&version);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "MFRC522 self-test failed: %s", esp_err_to_name(error));
#ifdef CONFIG_FIGURE_ENABLE_DISPLAY
        display_render_status("NFC ERROR");
#endif
        return;
    }
    ESP_LOGI(TAG, "MFRC522 self-test passed, version=0x%02X", version);
#ifdef CONFIG_FIGURE_ENABLE_DISPLAY
    display_render_nfc(NULL, false);
#endif
    if (xTaskCreate(nfc_reader_task, "nfc_reader", 4096, NULL, 4, NULL) !=
        pdPASS) {
        ESP_LOGE(TAG, "Unable to start NFC reader task");
    }
}
#endif

static const char *device_state_label(device_state_t state)
{
    switch (state) {
        case DEVICE_STATE_BOOTING: return "BOOTING";
        case DEVICE_STATE_CONNECTING: return "CONNECTING";
        case DEVICE_STATE_IDLE: return "ONLINE";
        case DEVICE_STATE_LISTENING: return "LISTENING";
        case DEVICE_STATE_UPLOADING: return "UPLOADING";
        case DEVICE_STATE_THINKING: return "THINKING";
        case DEVICE_STATE_SPEAKING: return "SPEAKING";
        case DEVICE_STATE_REMINDER: return "REMINDER";
        case DEVICE_STATE_ALARM: return "ALARM";
        case DEVICE_STATE_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

static void device_set_state(device_state_t next_state)
{
    if (device_state == next_state) return;
    device_state = next_state;
    device_state_since = xTaskGetTickCount();
    ESP_LOGI(TAG, "State -> %s", device_state_label(next_state));
#ifdef CONFIG_FIGURE_ENABLE_DISPLAY
    display_render_status(sleep_mode_enabled && next_state == DEVICE_STATE_IDLE
                              ? "SLEEP"
                              : device_state_label(next_state));
#endif
    if (sleep_mode_enabled && next_state == DEVICE_STATE_IDLE) {
        set_rgb(0, 0, 0);
        return;
    }
    switch (next_state) {
        case DEVICE_STATE_LISTENING: set_rgb(24, 0, 0); break;
        case DEVICE_STATE_UPLOADING: set_rgb(12, 0, 24); break;
        case DEVICE_STATE_THINKING: set_rgb(0, 4, 24); break;
        case DEVICE_STATE_SPEAKING: set_rgb(0, 18, 24); break;
        case DEVICE_STATE_REMINDER: set_rgb(24, 10, 0); break;
        case DEVICE_STATE_ALARM: set_rgb(24, 0, 0); break;
        case DEVICE_STATE_ERROR: set_rgb(24, 0, 0); break;
        case DEVICE_STATE_IDLE: set_rgb(0, 5, 1); break;
        default: set_rgb(8, 6, 0); break;
    }
}

#ifdef CONFIG_FIGURE_ENABLE_DISPLAY
static void device_animation_task(void *arg)
{
    (void)arg;
    unsigned frame = 0;
    while (true) {
        display_render_state_animation(device_state, frame++);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}
#endif

#ifdef CONFIG_FIGURE_ENABLE_TALK_BUTTON
static void talk_button_task(void *arg)
{
    (void)arg;
    while (true) {
        if (gpio_get_level(CONFIG_FIGURE_TALK_BUTTON_GPIO) == 0) {
            vTaskDelay(pdMS_TO_TICKS(CONFIG_FIGURE_TALK_BUTTON_DEBOUNCE_MS));
            if (gpio_get_level(CONFIG_FIGURE_TALK_BUTTON_GPIO) == 0) {
                if (!sleep_mode_enabled &&
                    device_state == DEVICE_STATE_IDLE &&
                    !talk_button_requested) {
                    talk_button_down = true;
                    talk_button_requested = true;
                    ESP_LOGI(TAG, "Talk button pressed; push-to-talk starting");
                    if (figure_network_task_handle != NULL) {
                        xTaskNotifyGive(figure_network_task_handle);
                    }
                } else {
                    ESP_LOGW(TAG, "Talk button ignored while state=%s",
                             device_state_label(device_state));
                }
                while (gpio_get_level(CONFIG_FIGURE_TALK_BUTTON_GPIO) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
                talk_button_down = false;
                ESP_LOGI(TAG, "Talk button released");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void configure_talk_button(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << CONFIG_FIGURE_TALK_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
    ESP_LOGI(TAG, "Talk button ready: GPIO=%d active=LOW",
             CONFIG_FIGURE_TALK_BUTTON_GPIO);
    xTaskCreate(talk_button_task, "talk_button", 3072, NULL, 5, NULL);
}
#endif

#ifdef CONFIG_FIGURE_ENABLE_CONTROL_BUTTONS
static void notify_network_task(void)
{
    if (figure_network_task_handle != NULL) {
        xTaskNotifyGive(figure_network_task_handle);
    }
}

static void queue_button_event(device_button_event_t event)
{
    pending_button_events |= (uint32_t)event;
    notify_network_task();
}

static void save_volume_and_render(const char *status)
{
    (void)save_u8("volume", current_volume);
    ESP_LOGI(TAG, "Local volume=%u", current_volume);
#ifdef CONFIG_FIGURE_ENABLE_DISPLAY
    display_render_status(status);
#endif
}

static void handle_volume_up_button(void)
{
    ESP_LOGI(TAG, "Button: volume up");
    if (current_volume == 0 && volume_before_mute > 0) {
        current_volume = volume_before_mute;
    } else if (current_volume <= 100U - VOLUME_STEP) {
        current_volume += VOLUME_STEP;
    } else {
        current_volume = 100U;
    }
    save_volume_and_render("VOL UP");
    queue_button_event(DEVICE_BUTTON_EVENT_VOLUME_UP);
#ifdef CONFIG_FIGURE_ENABLE_SPEAKER
    (void)speaker_play_prompt_file(PROMPT_VOLUME_UP_PATH, pdMS_TO_TICKS(200));
#endif
}

static void handle_volume_down_button(bool long_press)
{
    ESP_LOGI(TAG, "Button: volume down%s", long_press ? " long-press" : "");
    if (long_press) {
        if (current_volume == 0) {
            current_volume = volume_before_mute > 0 ? volume_before_mute : 55U;
            save_volume_and_render("UNMUTE");
#ifdef CONFIG_FIGURE_ENABLE_SPEAKER
            (void)speaker_play_prompt_file(PROMPT_UNMUTED_PATH, pdMS_TO_TICKS(200));
#endif
        } else {
            volume_before_mute = current_volume;
#ifdef CONFIG_FIGURE_ENABLE_SPEAKER
            (void)speaker_play_prompt_file(PROMPT_MUTED_PATH, pdMS_TO_TICKS(200));
#endif
            current_volume = 0;
            save_volume_and_render("MUTED");
        }
        queue_button_event(DEVICE_BUTTON_EVENT_MUTE_TOGGLE);
        return;
    }

    current_volume = current_volume > VOLUME_STEP ? current_volume - VOLUME_STEP : 0U;
    save_volume_and_render("VOL DOWN");
    queue_button_event(DEVICE_BUTTON_EVENT_VOLUME_DOWN);
#ifdef CONFIG_FIGURE_ENABLE_SPEAKER
    (void)speaker_play_prompt_file(PROMPT_VOLUME_DOWN_PATH, pdMS_TO_TICKS(200));
#endif
}

static void handle_function_button(void)
{
    ESP_LOGI(TAG, "Button: function");
    if (device_state == DEVICE_STATE_ALARM || device_state == DEVICE_STATE_REMINDER) {
        playback_stop_requested = true;
        ESP_LOGI(TAG, "Function button requested snooze");
#ifdef CONFIG_FIGURE_ENABLE_DISPLAY
        display_render_status("SNOOZE");
#endif
        queue_button_event(DEVICE_BUTTON_EVENT_SNOOZE);
#ifdef CONFIG_FIGURE_ENABLE_SPEAKER
        (void)speaker_play_prompt_file(PROMPT_SNOOZE_PATH, pdMS_TO_TICKS(2500));
#endif
        return;
    }

    playback_stop_requested = true;
    ESP_LOGI(TAG, "Function button requested playback stop");
#ifdef CONFIG_FIGURE_ENABLE_DISPLAY
    display_render_status("STOP");
#endif
    queue_button_event(DEVICE_BUTTON_EVENT_STOP_PLAYBACK);
#ifdef CONFIG_FIGURE_ENABLE_SPEAKER
    (void)speaker_play_prompt_file(PROMPT_STOP_PLAYBACK_PATH, pdMS_TO_TICKS(2500));
#endif
}

static void handle_sleep_button(void)
{
    ESP_LOGI(TAG, "Button: sleep");
    sleep_mode_enabled = !sleep_mode_enabled;
    if (sleep_mode_enabled) {
        playback_stop_requested = true;
        ESP_LOGI(TAG, "Sleep mode enabled");
#ifdef CONFIG_FIGURE_ENABLE_DISPLAY
        display_render_status("SLEEP");
#endif
        set_rgb(0, 0, 0);
#ifdef CONFIG_FIGURE_ENABLE_SPEAKER
        (void)speaker_play_prompt_file(PROMPT_SLEEP_ON_PATH, pdMS_TO_TICKS(2500));
#endif
    } else {
        ESP_LOGI(TAG, "Sleep mode disabled");
#ifdef CONFIG_FIGURE_ENABLE_DISPLAY
        display_render_status(device_state_label(device_state));
#endif
        if (device_state == DEVICE_STATE_IDLE) set_rgb(0, 5, 1);
#ifdef CONFIG_FIGURE_ENABLE_SPEAKER
        (void)speaker_play_prompt_file(PROMPT_SLEEP_OFF_PATH, pdMS_TO_TICKS(2500));
#endif
    }
    queue_button_event(DEVICE_BUTTON_EVENT_SLEEP_TOGGLE);
}

static bool read_control_button_press(uint32_t gpio, uint32_t *duration_ms)
{
    if (gpio_get_level(gpio) != 0) return false;
    vTaskDelay(pdMS_TO_TICKS(CONFIG_FIGURE_CONTROL_BUTTON_DEBOUNCE_MS));
    if (gpio_get_level(gpio) != 0) return false;

    const TickType_t started_at = xTaskGetTickCount();
    while (gpio_get_level(gpio) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (duration_ms != NULL) {
        *duration_ms =
            (uint32_t)((xTaskGetTickCount() - started_at) * portTICK_PERIOD_MS);
    }
    return true;
}

static void control_buttons_task(void *arg)
{
    (void)arg;
    while (true) {
        uint32_t duration_ms = 0;
        if (read_control_button_press(CONFIG_FIGURE_VOLUME_UP_BUTTON_GPIO,
                                      &duration_ms)
#if CONFIG_FIGURE_VOLUME_UP_ALT_BUTTON_GPIO >= 0
            || read_control_button_press(CONFIG_FIGURE_VOLUME_UP_ALT_BUTTON_GPIO,
                                         &duration_ms)
#endif
        ) {
            handle_volume_up_button();
        } else if (read_control_button_press(CONFIG_FIGURE_VOLUME_DOWN_BUTTON_GPIO,
                                             &duration_ms)) {
            handle_volume_down_button(
                duration_ms >= CONFIG_FIGURE_CONTROL_BUTTON_LONG_PRESS_MS);
        } else if (read_control_button_press(CONFIG_FIGURE_FUNCTION_BUTTON_GPIO,
                                             &duration_ms)) {
            handle_function_button();
        } else if (read_control_button_press(CONFIG_FIGURE_SLEEP_BUTTON_GPIO,
                                             &duration_ms)) {
            handle_sleep_button();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void configure_control_buttons(void)
{
    uint64_t button_mask =
        (1ULL << CONFIG_FIGURE_VOLUME_UP_BUTTON_GPIO) |
        (1ULL << CONFIG_FIGURE_VOLUME_DOWN_BUTTON_GPIO) |
        (1ULL << CONFIG_FIGURE_FUNCTION_BUTTON_GPIO) |
        (1ULL << CONFIG_FIGURE_SLEEP_BUTTON_GPIO);
#if CONFIG_FIGURE_VOLUME_UP_ALT_BUTTON_GPIO >= 0
    button_mask |= (1ULL << CONFIG_FIGURE_VOLUME_UP_ALT_BUTTON_GPIO);
#endif
    const gpio_config_t config = {
        .pin_bit_mask = button_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
    ESP_LOGI(TAG,
             "Control buttons ready: vol+=GPIO%d alt=GPIO%d vol-=GPIO%d function=GPIO%d sleep=GPIO%d active=LOW",
             CONFIG_FIGURE_VOLUME_UP_BUTTON_GPIO,
             CONFIG_FIGURE_VOLUME_UP_ALT_BUTTON_GPIO,
             CONFIG_FIGURE_VOLUME_DOWN_BUTTON_GPIO,
             CONFIG_FIGURE_FUNCTION_BUTTON_GPIO,
             CONFIG_FIGURE_SLEEP_BUTTON_GPIO);
    xTaskCreate(control_buttons_task, "control_buttons", 4096, NULL, 5, NULL);
}
#endif

static void print_board_info(void)
{
    esp_chip_info_t chip_info;
    uint32_t flash_size = 0;

    esp_chip_info(&chip_info);
    ESP_ERROR_CHECK(esp_flash_get_size(NULL, &flash_size));

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Figure companion firmware started");
    ESP_LOGI(TAG, "Chip: ESP32-S3, revision: %d.%d", chip_info.revision / 100, chip_info.revision % 100);
    ESP_LOGI(TAG, "CPU cores: %d", chip_info.cores);
    ESP_LOGI(TAG, "Flash: %" PRIu32 " MB", flash_size / (1024 * 1024));
#ifdef CONFIG_SPIRAM
    ESP_LOGI(TAG, "PSRAM initialized: %s", esp_psram_is_initialized() ? "yes" : "no");
    ESP_LOGI(TAG, "PSRAM size: %u bytes", (unsigned)esp_psram_get_size());
#else
    ESP_LOGW(TAG, "PSRAM support is disabled in sdkconfig");
#endif
    ESP_LOGI(TAG, "Free internal heap: %u bytes", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "RGB data GPIO: %d", CONFIG_BLINK_GPIO);
    ESP_LOGI(TAG, "Firmware: 0.7.4-dev");
    ESP_LOGI(TAG, "API base URL: %s", api_base_url);
    ESP_LOGI(TAG, "========================================");
}

static bool normalize_api_base_url(const char *input, size_t input_length,
                                   char *output, size_t output_capacity)
{
    if (input == NULL || input_length == 0 || output == NULL || output_capacity < 16) {
        return false;
    }
    while (input_length > 0 && (input[input_length - 1] == '\0' ||
                                input[input_length - 1] == ' ' ||
                                input[input_length - 1] == '\r' ||
                                input[input_length - 1] == '\n' ||
                                input[input_length - 1] == '\t')) {
        input_length--;
    }
    while (input_length > 0 && (*input == ' ' || *input == '\r' ||
                                *input == '\n' || *input == '\t')) {
        input++;
        input_length--;
    }
    if (input_length == 0 || input_length >= output_capacity) return false;
    if (!((input_length > 7 && strncmp(input, "http://", 7) == 0) ||
          (input_length > 8 && strncmp(input, "https://", 8) == 0))) {
        return false;
    }
    for (size_t index = 0; index < input_length; ++index) {
        const unsigned char value = (unsigned char)input[index];
        if (value <= 0x20 || value == 0x7f) return false;
    }

    while (input_length > 0 && input[input_length - 1] == '/') input_length--;
    const bool has_api_path = input_length >= 3 &&
                              memcmp(input + input_length - 3, "/v1", 3) == 0;
    const size_t suffix_length = has_api_path ? 0 : 3;
    if (input_length + suffix_length >= output_capacity) return false;
    memcpy(output, input, input_length);
    if (!has_api_path) memcpy(output + input_length, "/v1", 3);
    output[input_length + suffix_length] = '\0';
    return true;
}

static void init_nvs(void)
{
    esp_err_t error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES || error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        error = nvs_flash_init();
    }
    ESP_ERROR_CHECK(error);

    nvs_handle_t handle;
    if (nvs_open("figure", NVS_READONLY, &handle) == ESP_OK) {
        (void)nvs_get_u8(handle, "volume", &current_volume);
        if (current_volume > 0) volume_before_mute = current_volume;
        char saved_url[API_BASE_URL_CAPACITY];
        size_t saved_url_length = sizeof(saved_url);
        if (nvs_get_str(handle, "api_base_url", saved_url, &saved_url_length) == ESP_OK) {
            char normalized[API_BASE_URL_CAPACITY];
            if (normalize_api_base_url(saved_url, strlen(saved_url), normalized,
                                       sizeof(normalized))) {
                snprintf(api_base_url, sizeof(api_base_url), "%s", normalized);
            } else {
                ESP_LOGW(TAG, "Ignoring invalid saved API base URL");
            }
        }
        nvs_close(handle);
    }
    ESP_LOGI(TAG, "Runtime API base URL: %s", api_base_url);
}

static bool save_u8(const char *key, uint8_t value)
{
    nvs_handle_t handle;
    if (nvs_open("figure", NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t error = nvs_set_u8(handle, key, value);
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    return error == ESP_OK;
}

static bool save_string(const char *key, const char *value)
{
    nvs_handle_t handle;
    if (nvs_open("figure", NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t error = nvs_set_str(handle, key, value);
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    return error == ESP_OK;
}

static esp_err_t provisioning_config_handler(uint32_t session_id,
                                             const uint8_t *input,
                                             ssize_t input_length,
                                             uint8_t **output,
                                             ssize_t *output_length,
                                             void *private_data)
{
    (void)session_id;
    (void)private_data;
    if (input == NULL || input_length <= 0 || output == NULL || output_length == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char normalized[API_BASE_URL_CAPACITY];
    if (!normalize_api_base_url((const char *)input, (size_t)input_length,
                                normalized, sizeof(normalized))) {
        ESP_LOGW(TAG, "Rejected invalid API base URL from provisioning");
        return ESP_ERR_INVALID_ARG;
    }
    if (!save_string("api_base_url", normalized)) {
        ESP_LOGE(TAG, "Unable to persist API base URL");
        return ESP_FAIL;
    }
    snprintf(api_base_url, sizeof(api_base_url), "%s", normalized);
    ESP_LOGI(TAG, "Provisioning saved API base URL: %s", api_base_url);

    static const char response[] = "SUCCESS";
    *output = (uint8_t *)strdup(response);
    if (*output == NULL) return ESP_ERR_NO_MEM;
    *output_length = sizeof(response);
    return ESP_OK;
}

static bool consume_provisioning_request(void)
{
    nvs_handle_t handle;
    uint8_t requested = 0;
    if (nvs_open("figure", NVS_READWRITE, &handle) != ESP_OK) return false;
    (void)nvs_get_u8(handle, "wifi_setup", &requested);
    if (requested != 0) {
        (void)nvs_set_u8(handle, "wifi_setup", 0);
        (void)nvs_commit(handle);
    }
    nvs_close(handle);
    return requested != 0;
}

#ifdef CONFIG_FIGURE_ENABLE_PROVISIONING_BUTTON
static void provisioning_button_task(void *arg)
{
    (void)arg;
    while (true) {
        if (gpio_get_level(CONFIG_FIGURE_PROVISIONING_BUTTON_GPIO) == 0) {
            const TickType_t pressed_at = xTaskGetTickCount();
            while (gpio_get_level(CONFIG_FIGURE_PROVISIONING_BUTTON_GPIO) == 0) {
                if ((xTaskGetTickCount() - pressed_at) * portTICK_PERIOD_MS >=
                    CONFIG_FIGURE_PROVISIONING_HOLD_MS) {
                    ESP_LOGW(TAG, "Wi-Fi setup requested; restarting into BLE provisioning mode");
                    if (!save_u8("wifi_setup", 1)) {
                        ESP_LOGE(TAG, "Could not persist Wi-Fi setup request");
                        break;
                    }
#ifdef CONFIG_FIGURE_ENABLE_DISPLAY
                    display_render_status("SETUP");
#endif
                    set_rgb(18, 0, 24);
                    vTaskDelay(pdMS_TO_TICKS(500));
                    esp_restart();
                }
                vTaskDelay(pdMS_TO_TICKS(25));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}

static void configure_provisioning_button(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << CONFIG_FIGURE_PROVISIONING_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
    ESP_LOGI(TAG, "Wi-Fi setup button ready: GPIO=%d hold=%dms",
             CONFIG_FIGURE_PROVISIONING_BUTTON_GPIO,
             CONFIG_FIGURE_PROVISIONING_HOLD_MS);
    xTaskCreate(provisioning_button_task, "wifi_setup_button", 3072, NULL, 5, NULL);
}
#endif

static void register_wifi_handler(void);

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == NETWORK_PROV_EVENT) {
        switch (event_id) {
            case NETWORK_PROV_START:
                ESP_LOGI(TAG, "BLE Wi-Fi provisioning started");
                break;
            case NETWORK_PROV_WIFI_CRED_RECV: {
                const wifi_sta_config_t *config = event_data;
                ESP_LOGI(TAG, "Received Wi-Fi credentials for SSID: %s", config->ssid);
                break;
            }
            case NETWORK_PROV_WIFI_CRED_FAIL: {
                const network_prov_wifi_sta_fail_reason_t *reason = event_data;
                ESP_LOGW(TAG, "Provisioning connection failed: %s",
                         *reason == NETWORK_PROV_WIFI_STA_AUTH_ERROR
                             ? "authentication failed" : "network not found");
                break;
            }
            case NETWORK_PROV_WIFI_CRED_SUCCESS:
                ESP_LOGI(TAG, "Wi-Fi provisioning succeeded");
                break;
            case NETWORK_PROV_END:
                ESP_LOGI(TAG, "BLE provisioning service stopped");
                ESP_ERROR_CHECK(network_prov_mgr_deinit());
                register_wifi_handler();
                break;
            default:
                break;
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_ERROR_CHECK(esp_wifi_connect());
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "Wi-Fi disconnected; reconnecting");
        (void)esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        ESP_LOGI(TAG, "Wi-Fi connected, IP=" IPSTR, IP2STR(&event->ip_info.ip));
#ifdef CONFIG_FIGURE_ENABLE_DISPLAY
        display_render_status("WIFI OK");
#endif
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void register_wifi_handler(void)
{
    if (wifi_handler_registered) return;
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                &wifi_event_handler, NULL));
    wifi_handler_registered = true;
}

static void get_provisioning_service_name(char *service_name, size_t capacity)
{
    uint8_t mac[6] = {0};
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, mac));
    snprintf(service_name, capacity, "YZAI_%02X%02X%02X", mac[3], mac[4], mac[5]);
}

static bool connect_wifi(void)
{
    wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_sta() == NULL ? ESP_FAIL : ESP_OK);

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID,
                                                &wifi_event_handler, NULL));

    const network_prov_mgr_config_t manager_config = {
        .scheme = network_prov_scheme_ble,
        .scheme_event_handler = NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
        .app_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
        .network_prov_wifi_conn_cfg = {
            .wifi_conn_attempts = 5,
        },
    };
    ESP_ERROR_CHECK(network_prov_mgr_init(manager_config));

    if (consume_provisioning_request()) {
        ESP_LOGI(TAG, "Clearing saved Wi-Fi credentials for requested network change");
        ESP_ERROR_CHECK(network_prov_mgr_reset_wifi_provisioning());
    }

    bool provisioned = false;
    ESP_ERROR_CHECK(network_prov_mgr_is_wifi_provisioned(&provisioned));
    device_set_state(DEVICE_STATE_CONNECTING);

    if (!provisioned) {
        char service_name[20];
        get_provisioning_service_name(service_name, sizeof(service_name));
        network_prov_security1_params_t *security_params = CONFIG_FIGURE_PROVISIONING_POP;
        ESP_ERROR_CHECK(network_prov_mgr_endpoint_create(PROVISIONING_CONFIG_ENDPOINT));
        ESP_ERROR_CHECK(network_prov_mgr_start_provisioning(
            NETWORK_PROV_SECURITY_1,
            (const void *)security_params,
            service_name,
            NULL));
        ESP_ERROR_CHECK(network_prov_mgr_endpoint_register(
            PROVISIONING_CONFIG_ENDPOINT, provisioning_config_handler, NULL));
#ifdef CONFIG_FIGURE_ENABLE_DISPLAY
        display_render_status("SETUP");
#endif
        set_rgb(18, 0, 24);
#ifdef CONFIG_FIGURE_ENABLE_SPEAKER
        speaker_play_setup_prompt();
#endif
        ESP_LOGI(TAG, "Provisioning BLE name: %s", service_name);
        ESP_LOGI(TAG,
                 "Provisioning QR payload: {\"ver\":\"v1\",\"name\":\"%s\","
                 "\"pop\":\"%s\",\"transport\":\"ble\",\"security\":1,"
                 "\"hardwareId\":\"%s\",\"pairingCode\":\"%s\"}",
                 service_name, CONFIG_FIGURE_PROVISIONING_POP,
                 CONFIG_FIGURE_DEVICE_HARDWARE_ID,
                 CONFIG_FIGURE_DEVICE_PAIRING_CODE);
    } else {
        ESP_LOGI(TAG, "Saved Wi-Fi found; starting station mode");
        ESP_ERROR_CHECK(network_prov_mgr_deinit());
        register_wifi_handler();
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
    }

    xEventGroupWaitBits(wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    pulse_rgb(0, 20, 8, 500);
    return true;
}

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    http_response_t *response = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
        const size_t remaining = sizeof(response->data) - response->length - 1;
        const size_t copy_length = (size_t)event->data_len < remaining ? (size_t)event->data_len : remaining;
        if (copy_length > 0) {
            memcpy(response->data + response->length, event->data, copy_length);
            response->length += copy_length;
            response->data[response->length] = '\0';
        }
        if (copy_length < (size_t)event->data_len) response->overflow = true;
    }
    return ESP_OK;
}

static int http_request(esp_http_client_method_t method, const char *path, const char *body,
                        bool authenticated, http_response_t *response)
{
    char url[320];
    if (snprintf(url, sizeof(url), "%s%s", api_base_url, path) >= (int)sizeof(url)) {
        ESP_LOGE(TAG, "API URL is too long");
        return -1;
    }

    memset(response, 0, sizeof(*response));
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = response,
        .timeout_ms = 6000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) return -1;

    esp_http_client_set_method(client, method);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (authenticated && access_token[0] != '\0') {
        char authorization[112];
        snprintf(authorization, sizeof(authorization), "Bearer %s", access_token);
        esp_http_client_set_header(client, "Authorization", authorization);
    }
    if (body != NULL) esp_http_client_set_post_field(client, body, strlen(body));

    const esp_err_t error = esp_http_client_perform(client);
    const int status = error == ESP_OK ? esp_http_client_get_status_code(client) : -1;
    if (error != ESP_OK) ESP_LOGE(TAG, "HTTP %s failed: %s", url, esp_err_to_name(error));
    if (response->overflow) ESP_LOGW(TAG, "HTTP response exceeded %u bytes", (unsigned)HTTP_RESPONSE_CAPACITY);
    esp_http_client_cleanup(client);
    return status;
}

static int http_binary_post(const char *path, const uint8_t *data,
                            size_t data_length, const char *content_type,
                            http_response_t *response)
{
    char url[384];
    if (snprintf(url, sizeof(url), "%s%s", api_base_url, path) >=
        (int)sizeof(url)) {
        ESP_LOGE(TAG, "Binary API URL is too long");
        return -1;
    }

    memset(response, 0, sizeof(*response));
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = response,
        .timeout_ms = 30000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) return -1;

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", content_type);
    if (access_token[0] != '\0') {
        char authorization[112];
        snprintf(authorization, sizeof(authorization), "Bearer %s", access_token);
        esp_http_client_set_header(client, "Authorization", authorization);
    }
    esp_http_client_set_post_field(client, (const char *)data, data_length);

    const esp_err_t error = esp_http_client_perform(client);
    const int status =
        error == ESP_OK ? esp_http_client_get_status_code(client) : -1;
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "HTTP binary POST %s failed: %s", url,
                 esp_err_to_name(error));
    }
    if (response->overflow) {
        ESP_LOGW(TAG, "HTTP response exceeded %u bytes",
                 (unsigned)HTTP_RESPONSE_CAPACITY);
    }
    esp_http_client_cleanup(client);
    return status;
}

static bool create_device_session(void)
{
    cJSON *request_json = cJSON_CreateObject();
    cJSON_AddStringToObject(request_json, "hardwareId", CONFIG_FIGURE_DEVICE_HARDWARE_ID);
    cJSON_AddStringToObject(request_json, "deviceSecret", CONFIG_FIGURE_DEVICE_SECRET);
    char *body = cJSON_PrintUnformatted(request_json);
    const int status = http_request(HTTP_METHOD_POST, "/device/session", body, false, &http_response);
    cJSON_free(body);
    cJSON_Delete(request_json);

    if (status != 200 && status != 201) {
        ESP_LOGE(TAG, "Device session failed, HTTP status=%d, response=%s", status, http_response.data);
        return false;
    }

    cJSON *root = cJSON_Parse(http_response.data);
    cJSON *token = root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "accessToken");
    cJSON *device = root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "device");
    cJSON *pairing_code = device == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(device, "pairingCode");
    if (!cJSON_IsString(token) || token->valuestring == NULL) {
        ESP_LOGE(TAG, "Session response does not contain an access token");
        cJSON_Delete(root);
        return false;
    }

    strlcpy(access_token, token->valuestring, sizeof(access_token));
    ESP_LOGI(TAG, "Device session ready; pairing code=%s",
             cJSON_IsString(pairing_code) ? pairing_code->valuestring : "unknown");
    device_set_state(DEVICE_STATE_IDLE);
    cJSON_Delete(root);
    return true;
}

static int send_heartbeat(void)
{
    cJSON *request_json = cJSON_CreateObject();
    cJSON_AddStringToObject(request_json, "firmwareVersion", "0.7.3-dev");
    cJSON_AddNumberToObject(request_json, "volume", current_volume);
    char *body = cJSON_PrintUnformatted(request_json);
    const int status = http_request(HTTP_METHOD_POST, "/device/heartbeat", body, true, &http_response);
    cJSON_free(body);
    cJSON_Delete(request_json);

    if (status == 200 || status == 201) {
        cJSON *root = cJSON_Parse(http_response.data);
        cJSON *pending = root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "pendingCommandCount");
        cJSON *bound = root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "bound");
        pending_command_count = cJSON_IsNumber(pending) ? pending->valueint : 0;
        device_bound = cJSON_IsTrue(bound);
        ESP_LOGI(TAG, "Heartbeat OK; bound=%s, pending=%d",
                 device_bound ? "yes" : "no",
                 pending_command_count);
        if (device_state == DEVICE_STATE_CONNECTING ||
            device_state == DEVICE_STATE_BOOTING) {
            device_set_state(DEVICE_STATE_IDLE);
        }
        cJSON_Delete(root);
    } else {
        ESP_LOGE(TAG, "Heartbeat failed, HTTP status=%d, response=%s", status, http_response.data);
    }
    return status;
}

static bool acknowledge_command(const char *command_id)
{
    char path[112];
    snprintf(path, sizeof(path), "/device/commands/%s/ack", command_id);
    const int status = http_request(HTTP_METHOD_POST, path, "{}", true, &http_response);
    return status == 200 || status == 201;
}

static void post_device_event(const char *type, const char *message, const char *command_type)
{
    cJSON *request_json = cJSON_CreateObject();
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(request_json, "type", type);
    cJSON_AddStringToObject(payload, "message", message);
    if (command_type != NULL) cJSON_AddStringToObject(payload, "commandType", command_type);
    if (active_alarm_id[0] != '\0' &&
        ((command_type != NULL && strcmp(command_type, "snooze_reminder") == 0) ||
         strcmp(type, "alarm_playback_started") == 0 ||
         strcmp(type, "alarm_playback_completed") == 0)) {
        cJSON_AddStringToObject(payload, "alarmId", active_alarm_id);
    }
    cJSON_AddItemToObject(request_json, "payload", payload);

    char *body = cJSON_PrintUnformatted(request_json);
    const int status = http_request(HTTP_METHOD_POST, "/device/events", body, true, &http_response);
    if (status != 200 && status != 201) {
        ESP_LOGW(TAG, "Device event post failed, HTTP status=%d", status);
    }
    cJSON_free(body);
    cJSON_Delete(request_json);
}

#ifdef CONFIG_FIGURE_ENABLE_CONTROL_BUTTONS
static void post_pending_button_events(void)
{
    const uint32_t events = pending_button_events;
    if (events == 0) return;
    pending_button_events &= ~events;

    if ((events & DEVICE_BUTTON_EVENT_VOLUME_UP) != 0) {
        post_device_event("button_pressed", "音量加按键已按下", "volume_up");
    }
    if ((events & DEVICE_BUTTON_EVENT_VOLUME_DOWN) != 0) {
        post_device_event("button_pressed", "音量减按键已按下", "volume_down");
    }
    if ((events & DEVICE_BUTTON_EVENT_MUTE_TOGGLE) != 0) {
        post_device_event("button_pressed", "静音按键已切换", "mute_toggle");
    }
    if ((events & DEVICE_BUTTON_EVENT_STOP_PLAYBACK) != 0) {
        post_device_event("button_pressed", "功能键已请求停止播报", "stop_playback");
    }
    if ((events & DEVICE_BUTTON_EVENT_SNOOZE) != 0) {
        post_device_event("button_pressed", "功能键已请求稍后提醒", "snooze_reminder");
        active_alarm_id[0] = '\0';
    }
    if ((events & DEVICE_BUTTON_EVENT_SLEEP_TOGGLE) != 0) {
        post_device_event("button_pressed", sleep_mode_enabled
                                                ? "设备已进入睡眠模式"
                                                : "设备已退出睡眠模式",
                          sleep_mode_enabled ? "sleep_on" : "sleep_off");
    }
}
#endif

#ifdef CONFIG_FIGURE_ENABLE_NFC
static int post_nfc_device_event(const nfc_event_t *event)
{
    cJSON *request_json = cJSON_CreateObject();
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(request_json, "type", event->type);
    cJSON_AddStringToObject(payload, "uid", event->uid);
    cJSON_AddNumberToObject(payload, "uidLength", event->uid_length);
    cJSON_AddStringToObject(payload, "reader", "MFRC522");
    cJSON_AddItemToObject(request_json, "payload", payload);

    char *body = cJSON_PrintUnformatted(request_json);
    http_response_t *response = calloc(1, sizeof(*response));
    if (response == NULL) {
        ESP_LOGE(TAG, "Unable to allocate NFC HTTP response buffer");
        cJSON_free(body);
        cJSON_Delete(request_json);
        return -1;
    }
    const int status = http_request(HTTP_METHOD_POST, "/device/events", body,
                                    true, response);
    if (status != 200 && status != 201) {
        ESP_LOGW(TAG, "NFC event upload failed, type=%s UID=%s status=%d",
                 event->type, event->uid, status);
    } else if (strcmp(event->type, "nfc_tag_present") == 0) {
        cJSON *root = cJSON_Parse(response->data);
        const cJSON *match = root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "nfcMatch");
        const cJSON *character_name = cJSON_IsObject(match)
                                           ? cJSON_GetObjectItemCaseSensitive(match, "characterName")
                                           : NULL;
        const bool matched = cJSON_IsString(character_name) &&
                             character_name->valuestring != NULL &&
                             character_name->valuestring[0] != '\0';
#ifdef CONFIG_FIGURE_ENABLE_DISPLAY
        display_render_nfc_match(event->uid,
                                 matched ? character_name->valuestring : NULL,
                                 matched);
#endif
        cJSON_Delete(root);
    }
    cJSON_free(body);
    cJSON_Delete(request_json);
    free(response);
    return status;
}

static void nfc_event_uploader_task(void *arg)
{
    (void)arg;
    nfc_event_t event;
    while (true) {
        if (xQueueReceive(nfc_event_queue, &event, portMAX_DELAY) != pdPASS) {
            continue;
        }
        while (true) {
            const EventBits_t wifi_bits = wifi_events == NULL
                                                ? 0
                                                : xEventGroupGetBits(wifi_events);
            if (access_token[0] == '\0' ||
                (wifi_bits & WIFI_CONNECTED_BIT) == 0) {
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
            const int status = post_nfc_device_event(&event);
            if (status == 200 || status == 201) {
                ESP_LOGI(TAG, "NFC event uploaded: type=%s UID=%s",
                         event.type, event.uid);
                break;
            }
            if (status == 401) access_token[0] = '\0';
            vTaskDelay(pdMS_TO_TICKS(1500));
        }
    }
}
#endif

#ifdef CONFIG_FIGURE_ENABLE_MICROPHONE
static bool record_and_upload(const char *command_id, uint32_t duration_ms,
                              bool push_to_talk)
{
    device_set_state(DEVICE_STATE_LISTENING);
    uint8_t *wav = NULL;
    size_t wav_size = 0;
    if (!record_microphone_wav(duration_ms, push_to_talk, &wav, &wav_size)) {
        device_set_state(DEVICE_STATE_ERROR);
        return false;
    }

    device_set_state(DEVICE_STATE_UPLOADING);
    char path[160];
    if (command_id != NULL && command_id[0] != '\0') {
        snprintf(path, sizeof(path),
                 "/device/conversation/audio?commandId=%s", command_id);
    } else {
        strlcpy(path, "/device/conversation/audio", sizeof(path));
    }
    const int status = http_binary_post(path, wav, wav_size, "audio/wav",
                                        &http_response);
    free(wav);
    if (status != 200 && status != 201) {
        ESP_LOGE(TAG, "Recording upload failed, HTTP status=%d, response=%s",
                 status, http_response.data);
        device_set_state(DEVICE_STATE_ERROR);
        return false;
    }

    cJSON *root = cJSON_Parse(http_response.data);
    const cJSON *accepted =
        root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "accepted");
    const cJSON *text =
        root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "text");
    const bool success = cJSON_IsTrue(accepted) && cJSON_IsString(text);
    if (success) {
        ESP_LOGI(TAG, "ASR result: %s", text->valuestring);
        device_set_state(text->valuestring[0] == '\0'
                             ? DEVICE_STATE_IDLE
                             : DEVICE_STATE_THINKING);
    } else {
        ESP_LOGE(TAG, "Invalid ASR response: %s", http_response.data);
        device_set_state(DEVICE_STATE_ERROR);
    }
    cJSON_Delete(root);
    return success;
}
#endif

static bool apply_config_command(const cJSON *command)
{
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(command, "type");
    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(command, "payload");
    if (!cJSON_IsString(type) || !cJSON_IsObject(payload)) return false;

    if (strcmp(type->valuestring, "start_listening") == 0) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(command, "id");
        const cJSON *duration =
            cJSON_GetObjectItemCaseSensitive(payload, "durationMs");
        if (!cJSON_IsString(id) || id->valuestring == NULL ||
            !cJSON_IsNumber(duration) || duration->valueint < 1000 ||
            duration->valueint > (int)MICROPHONE_MAX_RECORD_MS) {
            return false;
        }
#ifdef CONFIG_FIGURE_ENABLE_MICROPHONE
        if (device_state != DEVICE_STATE_IDLE) return false;
        return record_and_upload(id->valuestring,
                                 (uint32_t)duration->valueint, false);
#else
        return false;
#endif
    }

    if (strcmp(type->valuestring, "set_volume") == 0) {
        const cJSON *volume = cJSON_GetObjectItemCaseSensitive(payload, "volume");
        if (!cJSON_IsNumber(volume) || volume->valueint < 0 || volume->valueint > 100) return false;
        current_volume = (uint8_t)volume->valueint;
        if (current_volume > 0) volume_before_mute = current_volume;
        ESP_LOGI(TAG, "Saved logical volume=%u", current_volume);
#ifdef CONFIG_FIGURE_ENABLE_DISPLAY
        display_render_status("VOL SET");
#endif
        return save_u8("volume", current_volume);
    }

    if (strcmp(type->valuestring, "sync_character") == 0) {
        const cJSON *character = cJSON_GetObjectItemCaseSensitive(payload, "character");
        const cJSON *character_id = cJSON_IsObject(character)
                                         ? cJSON_GetObjectItemCaseSensitive(character, "id")
                                         : NULL;
        const cJSON *character_name = cJSON_IsObject(character)
                                           ? cJSON_GetObjectItemCaseSensitive(character, "name")
                                           : NULL;
        if (!cJSON_IsString(character_id) || character_id->valuestring == NULL) return false;
        ESP_LOGI(TAG, "Saved character=%s", character_id->valuestring);
#ifdef CONFIG_FIGURE_ENABLE_DISPLAY
        display_render_nfc_match(NULL,
                                 cJSON_IsString(character_name) &&
                                         character_name->valuestring != NULL
                                     ? character_name->valuestring
                                     : character_id->valuestring,
                                 true);
#endif
        return save_string("character", character_id->valuestring);
    }

    if (strcmp(type->valuestring, "speak_text") == 0) {
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(payload, "text");
        const cJSON *audio_path = cJSON_GetObjectItemCaseSensitive(payload, "audioPath");
        ESP_LOGI(TAG, "Speak text: %s",
                 cJSON_IsString(text) && text->valuestring != NULL ? text->valuestring : "");
        if (sleep_mode_enabled) {
            ESP_LOGI(TAG, "Speak text skipped because sleep mode is enabled");
#ifdef CONFIG_FIGURE_ENABLE_DISPLAY
            display_render_status("SLEEP");
#endif
            return true;
        }
#ifdef CONFIG_FIGURE_ENABLE_SPEAKER
        if (!cJSON_IsString(audio_path) || audio_path->valuestring == NULL ||
            !download_and_play_audio(audio_path->valuestring,
                                     DEVICE_STATE_SPEAKING)) {
            device_set_state(DEVICE_STATE_ERROR);
            return false;
        }
#else
        return false;
#endif
        device_set_state(DEVICE_STATE_IDLE);
        return true;
    }

    if (strcmp(type->valuestring, "play_reminder") == 0) {
        const cJSON *title = cJSON_GetObjectItemCaseSensitive(payload, "title");
        const cJSON *audio_path = cJSON_GetObjectItemCaseSensitive(payload, "audioPath");
        const cJSON *kind = cJSON_GetObjectItemCaseSensitive(payload, "kind");
        const cJSON *alarm_id = cJSON_GetObjectItemCaseSensitive(payload, "alarmId");
        const device_state_t reminder_state =
            cJSON_IsString(kind) && strcmp(kind->valuestring, "alarm") == 0
                ? DEVICE_STATE_ALARM
                : DEVICE_STATE_REMINDER;
        ESP_LOGI(TAG, "Play reminder: %s",
                 cJSON_IsString(title) && title->valuestring != NULL ? title->valuestring : "");
        if (reminder_state == DEVICE_STATE_ALARM && cJSON_IsString(alarm_id) &&
            alarm_id->valuestring != NULL) {
            strlcpy(active_alarm_id, alarm_id->valuestring, sizeof(active_alarm_id));
        }
        if (sleep_mode_enabled && reminder_state != DEVICE_STATE_ALARM) {
            ESP_LOGI(TAG, "Reminder skipped because sleep mode is enabled");
#ifdef CONFIG_FIGURE_ENABLE_DISPLAY
            display_render_status("SLEEP");
#endif
            return true;
        }
#ifdef CONFIG_FIGURE_ENABLE_SPEAKER
        if (!cJSON_IsString(audio_path) || audio_path->valuestring == NULL ||
            !download_and_play_audio(audio_path->valuestring, reminder_state)) {
            if (reminder_state == DEVICE_STATE_ALARM) {
                post_device_event("alarm_playback_completed", "闹钟播放失败", "failed");
                active_alarm_id[0] = '\0';
            }
            device_set_state(DEVICE_STATE_ERROR);
            return false;
        }
#else
        return false;
#endif
        if (reminder_state == DEVICE_STATE_ALARM) {
            const bool stopped = playback_stop_requested;
            post_device_event("alarm_playback_completed",
                              stopped ? "闹钟已停止" : "闹钟播放完成",
                              stopped ? "stopped" : "completed");
            bool pending_snooze = false;
#ifdef CONFIG_FIGURE_ENABLE_CONTROL_BUTTONS
            pending_snooze = (pending_button_events & DEVICE_BUTTON_EVENT_SNOOZE) != 0;
#endif
            if (!pending_snooze) active_alarm_id[0] = '\0';
        }
        device_set_state(DEVICE_STATE_IDLE);
        return true;
    }

    return false;
}

static int fetch_commands(void)
{
    const int status = http_request(HTTP_METHOD_GET, "/device/commands", NULL, true, &http_response);
    if (status != 200) {
        ESP_LOGE(TAG, "Command fetch failed, HTTP status=%d, response=%s", status, http_response.data);
        return status;
    }

    cJSON *commands = cJSON_Parse(http_response.data);
    if (!cJSON_IsArray(commands)) {
        ESP_LOGE(TAG, "Command response is not an array");
        cJSON_Delete(commands);
        return status;
    }

    cJSON *command;
    cJSON_ArrayForEach(command, commands) {
        cJSON *id = cJSON_GetObjectItemCaseSensitive(command, "id");
        cJSON *type = cJSON_GetObjectItemCaseSensitive(command, "type");
        if (!cJSON_IsString(id) || !cJSON_IsString(type)) continue;
        ESP_LOGI(TAG, "Command received: %s (%s)", type->valuestring, id->valuestring);
        post_device_event("command_received", "设备已收到后端指令", type->valuestring);

        if (apply_config_command(command)) {
            if (acknowledge_command(id->valuestring)) {
                ESP_LOGI(TAG, "Command acknowledged: %s", id->valuestring);
                post_device_event("command_acknowledged", "设备已完成执行并确认", type->valuestring);
            }
        } else {
            ESP_LOGW(TAG, "Command deferred until its hardware is available: %s", type->valuestring);
            post_device_event("command_deferred", "该指令等待外设接入后执行", type->valuestring);
        }
    }
    cJSON_Delete(commands);
    return status;
}

static void figure_network_task(void *arg)
{
    (void)arg;

    while (true) {
        const TickType_t state_age = xTaskGetTickCount() - device_state_since;
        if ((device_state == DEVICE_STATE_ERROR &&
             state_age > pdMS_TO_TICKS(5000)) ||
            (device_state == DEVICE_STATE_THINKING &&
             state_age > pdMS_TO_TICKS(90000))) {
            ESP_LOGW(TAG, "Recovering stale state=%s",
                     device_state_label(device_state));
            device_set_state(DEVICE_STATE_IDLE);
        }

        if (access_token[0] == '\0' && !create_device_session()) {
            pulse_rgb(24, 0, 0, 500);
            vTaskDelay(pdMS_TO_TICKS(4500));
            continue;
        }

#ifdef CONFIG_FIGURE_ENABLE_CONTROL_BUTTONS
        post_pending_button_events();
#endif

#if defined(CONFIG_FIGURE_ENABLE_TALK_BUTTON) && defined(CONFIG_FIGURE_ENABLE_MICROPHONE)
        if (talk_button_requested) {
            talk_button_requested = false;
            if (device_bound && device_state == DEVICE_STATE_IDLE) {
                post_device_event("button_pressed", "对话按键已按下", "start_listening");
                (void)record_and_upload(NULL, MICROPHONE_MAX_RECORD_MS, true);
            } else {
                ESP_LOGW(TAG, "Talk request ignored: bound=%s state=%s",
                         device_bound ? "yes" : "no",
                         device_state_label(device_state));
            }
        }
#endif

        const int heartbeat_status = send_heartbeat();
        if (heartbeat_status == 401) {
            access_token[0] = '\0';
        } else if (heartbeat_status == 200 || heartbeat_status == 201) {
            const int command_status = fetch_commands();
            if (command_status == 401) access_token[0] = '\0';
            if (device_state == DEVICE_STATE_IDLE) {
                if (sleep_mode_enabled) {
                    set_rgb(0, 0, 0);
                } else {
                    pulse_rgb(0, 20, 4, 250);
                    set_rgb(0, 5, 1);
                }
            }
        } else {
            if (device_state == DEVICE_STATE_IDLE) {
                pulse_rgb(24, 0, 0, 350);
            }
        }

        (void)ulTaskNotifyTake(pdTRUE,
                               pdMS_TO_TICKS(CONFIG_FIGURE_HEARTBEAT_INTERVAL_MS));
    }
}

void app_main(void)
{
    configure_rgb_led();
#ifdef CONFIG_FIGURE_ENABLE_DISPLAY
    configure_display();
    xTaskCreate(device_animation_task, "screen_animation", 4096, NULL, 3, NULL);
#endif
#ifdef CONFIG_FIGURE_ENABLE_SPEAKER
    configure_speaker();
    configure_audio_storage();
#endif
#ifdef CONFIG_FIGURE_ENABLE_MICROPHONE
    configure_microphone();
    run_microphone_self_test();
#endif
#ifdef CONFIG_FIGURE_ENABLE_NFC
    configure_nfc_reader();
#endif
    print_board_info();
    init_nvs();
#ifdef CONFIG_FIGURE_ENABLE_PROVISIONING_BUTTON
    configure_provisioning_button();
#endif
#ifdef CONFIG_FIGURE_ENABLE_TALK_BUTTON
    configure_talk_button();
#endif
#ifdef CONFIG_FIGURE_ENABLE_CONTROL_BUTTONS
    configure_control_buttons();
#endif

    pulse_rgb(24, 0, 0, 450);
    pulse_rgb(0, 24, 0, 450);
    pulse_rgb(0, 0, 24, 450);

    if (!connect_wifi()) {
        while (true) {
            pulse_rgb(0, 0, 20, 300);
            vTaskDelay(pdMS_TO_TICKS(1700));
        }
    }

    xTaskCreate(figure_network_task, "figure_network", 24576, NULL, 5,
                &figure_network_task_handle);
#ifdef CONFIG_FIGURE_ENABLE_NFC
    if (nfc_event_queue != NULL &&
        xTaskCreate(nfc_event_uploader_task, "nfc_uploader", 10240, NULL, 4,
                    NULL) != pdPASS) {
        ESP_LOGE(TAG, "Unable to start NFC event uploader task");
    }
#endif
}
