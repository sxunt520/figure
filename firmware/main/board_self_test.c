#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_chip_info.h"
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
#include "esp_system.h"
#include "esp_wifi.h"
#include "driver/gpio.h"
#if defined(CONFIG_FIGURE_ENABLE_SPEAKER) || defined(CONFIG_FIGURE_ENABLE_MICROPHONE)
#include "driver/i2s_std.h"
#endif
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#define WIFI_CONNECTED_BIT BIT0
#define HTTP_RESPONSE_CAPACITY 4096
#define ACCESS_TOKEN_CAPACITY 80

static const char *TAG = "figure_device";
static led_strip_handle_t rgb_led;
static EventGroupHandle_t wifi_events;
static char access_token[ACCESS_TOKEN_CAPACITY];
static uint8_t current_volume = 60;
static int pending_command_count = 0;
static bool device_bound = false;

#ifdef CONFIG_FIGURE_ENABLE_DISPLAY
static bool display_ready = false;
static uint16_t rgb565(uint8_t red, uint8_t green, uint8_t blue);
static void display_fill_rect(int x, int y, int width, int height, uint16_t color);
static void display_draw_text(int x, int y, const char *text, uint16_t color,
                              uint16_t background, int scale);
static void display_render_status(const char *line1);
#endif

#ifdef CONFIG_FIGURE_ENABLE_SPEAKER
static i2s_chan_handle_t speaker_tx_channel;

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
#endif

#ifdef CONFIG_FIGURE_ENABLE_MICROPHONE
static i2s_chan_handle_t microphone_rx_channel;

#ifdef CONFIG_FIGURE_ENABLE_DISPLAY
static void display_render_microphone_level(unsigned level)
{
    if (!display_ready) return;

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
}
#endif

static uint32_t microphone_abs_sample(int32_t sample)
{
    if (sample == INT32_MIN) return INT32_MAX;
    return sample < 0 ? (uint32_t)(-sample) : (uint32_t)sample;
}

static void configure_microphone(void)
{
    ESP_LOGI(TAG, "Initializing ZTS6672 I2S microphone: SCK=%d WS=%d SD=%d",
             CONFIG_FIGURE_MICROPHONE_PIN_SCK,
             CONFIG_FIGURE_MICROPHONE_PIN_WS,
             CONFIG_FIGURE_MICROPHONE_PIN_SD);

    const i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&channel_config, NULL, &microphone_rx_channel));

    const i2s_std_config_t standard_config = {
        // 48 kHz * 32 bits * 2 slots = 3.072 MHz BCLK.
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(48000),
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
    ESP_LOGI(TAG, "Microphone level self-test started (20 seconds, 48 kHz)");
#ifdef CONFIG_FIGURE_ENABLE_DISPLAY
    display_render_status("MIC TEST");
#endif

    // LCD drawing adds measurable latency; 48 iterations is about 20 seconds
    // on this board while still providing enough time for a speaking test.
    for (int iteration = 0; iteration < 48; ++iteration) {
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

static void display_fill_rect(int x, int y, int width, int height, uint16_t color)
{
    if (!display_ready || width <= 0 || height <= 0) return;

    uint16_t pixels[64];
    const int chunk = sizeof(pixels) / sizeof(pixels[0]);
    for (int i = 0; i < chunk; ++i) pixels[i] = color;

    for (int row = 0; row < height; ++row) {
        int remaining = width;
        int draw_x = x;
        while (remaining > 0) {
            const int count = remaining < chunk ? remaining : chunk;
            esp_lcd_panel_draw_bitmap(lcd_panel, draw_x, y + row, draw_x + count, y + row + 1, pixels);
            draw_x += count;
            remaining -= count;
        }
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
}

static void configure_display(void)
{
    ESP_LOGI(TAG, "Initializing ST7789 display");

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
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)CONFIG_FIGURE_DISPLAY_SPI_HOST, &io_config, &io_handle));

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
    ESP_LOGI(TAG, "Firmware: 0.2.0-dev");
    ESP_LOGI(TAG, "========================================");
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
        nvs_close(handle);
    }
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

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
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

static bool connect_wifi(void)
{
    if (strlen(CONFIG_FIGURE_WIFI_SSID) == 0) {
        ESP_LOGW(TAG, "Wi-Fi is not configured. Run idf.py menuconfig and set Figure Board Self-Test -> Wi-Fi SSID/password.");
        return false;
    }

    wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_sta() == NULL ? ESP_FAIL : ESP_OK);

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, CONFIG_FIGURE_WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, CONFIG_FIGURE_WIFI_PASSWORD, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = strlen(CONFIG_FIGURE_WIFI_PASSWORD) == 0
                                            ? WIFI_AUTH_OPEN
                                            : WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to Wi-Fi SSID: %s", CONFIG_FIGURE_WIFI_SSID);
#ifdef CONFIG_FIGURE_ENABLE_DISPLAY
    display_render_status("WIFI...");
#endif
    set_rgb(20, 12, 0);
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
    if (snprintf(url, sizeof(url), "%s%s", CONFIG_FIGURE_API_BASE_URL, path) >= (int)sizeof(url)) {
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
#ifdef CONFIG_FIGURE_ENABLE_DISPLAY
    display_render_status("ONLINE");
#endif
    cJSON_Delete(root);
    return true;
}

static int send_heartbeat(void)
{
    cJSON *request_json = cJSON_CreateObject();
    cJSON_AddStringToObject(request_json, "firmwareVersion", "0.2.0-dev");
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
#ifdef CONFIG_FIGURE_ENABLE_DISPLAY
        display_render_status("ONLINE");
#endif
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
    cJSON_AddItemToObject(request_json, "payload", payload);

    char *body = cJSON_PrintUnformatted(request_json);
    const int status = http_request(HTTP_METHOD_POST, "/device/events", body, true, &http_response);
    if (status != 200 && status != 201) {
        ESP_LOGW(TAG, "Device event post failed, HTTP status=%d", status);
    }
    cJSON_free(body);
    cJSON_Delete(request_json);
}

static bool apply_config_command(const cJSON *command)
{
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(command, "type");
    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(command, "payload");
    if (!cJSON_IsString(type) || !cJSON_IsObject(payload)) return false;

    if (strcmp(type->valuestring, "set_volume") == 0) {
        const cJSON *volume = cJSON_GetObjectItemCaseSensitive(payload, "volume");
        if (!cJSON_IsNumber(volume) || volume->valueint < 0 || volume->valueint > 100) return false;
        current_volume = (uint8_t)volume->valueint;
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
        if (!cJSON_IsString(character_id) || character_id->valuestring == NULL) return false;
        ESP_LOGI(TAG, "Saved character=%s", character_id->valuestring);
#ifdef CONFIG_FIGURE_ENABLE_DISPLAY
        display_render_status("CHAR OK");
#endif
        return save_string("character", character_id->valuestring);
    }

    if (strcmp(type->valuestring, "speak_text") == 0) {
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(payload, "text");
        ESP_LOGI(TAG, "Mock speak_text: %s",
                 cJSON_IsString(text) && text->valuestring != NULL ? text->valuestring : "");
#ifdef CONFIG_FIGURE_ENABLE_DISPLAY
        display_render_status("SPEAK OK");
#endif
        return true;
    }

    if (strcmp(type->valuestring, "play_reminder") == 0) {
        const cJSON *title = cJSON_GetObjectItemCaseSensitive(payload, "title");
        ESP_LOGI(TAG, "Mock play_reminder: %s",
                 cJSON_IsString(title) && title->valuestring != NULL ? title->valuestring : "");
#ifdef CONFIG_FIGURE_ENABLE_DISPLAY
        display_render_status("REMIND OK");
#endif
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
                post_device_event("command_acknowledged", "设备已完成模拟执行并确认", type->valuestring);
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
        if (access_token[0] == '\0' && !create_device_session()) {
            pulse_rgb(24, 0, 0, 500);
            vTaskDelay(pdMS_TO_TICKS(4500));
            continue;
        }

        const int heartbeat_status = send_heartbeat();
        if (heartbeat_status == 401) {
            access_token[0] = '\0';
        } else if (heartbeat_status == 200 || heartbeat_status == 201) {
            const int command_status = fetch_commands();
            if (command_status == 401) access_token[0] = '\0';
            pulse_rgb(0, 20, 4, 250);
        } else {
            pulse_rgb(24, 0, 0, 350);
        }

        vTaskDelay(pdMS_TO_TICKS(CONFIG_FIGURE_HEARTBEAT_INTERVAL_MS));
    }
}

void app_main(void)
{
    configure_rgb_led();
#ifdef CONFIG_FIGURE_ENABLE_DISPLAY
    configure_display();
#endif
#ifdef CONFIG_FIGURE_ENABLE_SPEAKER
    configure_speaker();
#endif
#ifdef CONFIG_FIGURE_ENABLE_MICROPHONE
    configure_microphone();
    run_microphone_self_test();
#endif
    print_board_info();
    init_nvs();

    pulse_rgb(24, 0, 0, 450);
    pulse_rgb(0, 24, 0, 450);
    pulse_rgb(0, 0, 24, 450);

    if (!connect_wifi()) {
        while (true) {
            pulse_rgb(0, 0, 20, 300);
            vTaskDelay(pdMS_TO_TICKS(1700));
        }
    }

    xTaskCreate(figure_network_task, "figure_network", 24576, NULL, 5, NULL);
}
