// Minimal driver for the 360x360 QSPI panel on the Waveshare ESP32-S3 Knob.
// Pins and the init sequence come from Waveshare's 08_LVGL_Test demo.
#include "display.h"
#include "esp_lcd_sh8601.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/ledc.h"
#include "lcd_init_cmds.h"

#define PIN_LCD_CS    14
#define PIN_LCD_PCLK  13
#define PIN_LCD_D0    15
#define PIN_LCD_D1    16
#define PIN_LCD_D2    17
#define PIN_LCD_D3    18
#define PIN_LCD_RST   21
#define PIN_BACKLIGHT 47

#define BACKLIGHT_HZ   10000
#define BACKLIGHT_BITS 12

#define BUF_PIXELS (LCD_W * 20)

static esp_lcd_panel_handle_t panel;
static SemaphoreHandle_t done_sem;
static uint16_t *dma_buf;
static bool backlight_pwm;

static bool on_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(done_sem, &woken);
    return woken == pdTRUE;
}

// Sends dma_buf to the panel and waits so the buffer can be reused.
static void flush(int x, int y, int w, int h)
{
    esp_lcd_panel_draw_bitmap(panel, x, y, x + w, y + h, dma_buf);
    xSemaphoreTake(done_sem, pdMS_TO_TICKS(100));
}

static int rows_per_chunk(int w, int h)
{
    int rows = (BUF_PIXELS / w) & ~1;
    if (rows < 2) rows = 2;
    return rows > h ? h : rows;
}

void display_init(void)
{
    done_sem = xSemaphoreCreateBinary();
    dma_buf = heap_caps_malloc(BUF_PIXELS * sizeof(uint16_t), MALLOC_CAP_DMA);

    const spi_bus_config_t buscfg = SH8601_PANEL_BUS_QSPI_CONFIG(PIN_LCD_PCLK, PIN_LCD_D0, PIN_LCD_D1,
                                                                 PIN_LCD_D2, PIN_LCD_D3,
                                                                 BUF_PIXELS * sizeof(uint16_t));
    ESP_ERROR_CHECK_WITHOUT_ABORT(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_panel_io_spi_config_t io_config = SH8601_PANEL_IO_QSPI_CONFIG(PIN_LCD_CS, on_trans_done, NULL);
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io));

    sh8601_vendor_config_t vendor_config = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .flags = { .use_qspi_interface = 1 },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_new_panel_sh8601(io, &panel_config, &panel));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_init(panel));

    display_fill_rect(0, 0, LCD_W, LCD_H, 0x0000);

    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BACKLIGHT_BITS,
        .timer_num = LEDC_TIMER_3,
        .freq_hz = BACKLIGHT_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t channel = {
        .gpio_num = PIN_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_7,
        .timer_sel = LEDC_TIMER_3,
        .duty = (1 << BACKLIGHT_BITS) - 1,
    };
    // Uses the IDF driver directly: Arduino's ledcAttach() fails on this pin.
    backlight_pwm = ledc_timer_config(&timer) == ESP_OK && ledc_channel_config(&channel) == ESP_OK;
    if (!backlight_pwm) {
        // No dimming, but at least the screen is lit.
        gpio_set_direction(PIN_BACKLIGHT, GPIO_MODE_OUTPUT);
        gpio_set_level(PIN_BACKLIGHT, 1);
    }
}

void display_set_backlight(float level)
{
    static int last_duty = -1;
    const int max_duty = (1 << BACKLIGHT_BITS) - 1;
    if (level < 0) level = 0;
    if (level > 1) level = 1;
    // Squared so equal steps look evenly spaced to the eye.
    int duty = (int)(level * level * max_duty + 0.5f);
    if (level > 0 && duty < 1) duty = 1;
    if (duty == last_duty) return;
    last_duty = duty;
    if (!backlight_pwm) return;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_7, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_7);
}

void display_fill_rect(int x, int y, int w, int h, uint16_t rgb565)
{
    if (w <= 0 || h <= 0) return;
    // The panel expects big-endian pixels.
    uint16_t swapped = (uint16_t)((rgb565 >> 8) | (rgb565 << 8));
    int rows = rows_per_chunk(w, h);
    for (int i = 0; i < w * rows; i++) dma_buf[i] = swapped;
    for (int yy = 0; yy < h; yy += rows) {
        int n = (h - yy < rows) ? h - yy : rows;
        flush(x, y + yy, w, n);
    }
}

void display_draw_bitmap(int x, int y, int w, int h, const uint16_t *rgb565)
{
    if (w <= 0 || h <= 0) return;
    int rows = rows_per_chunk(w, h);
    for (int yy = 0; yy < h; yy += rows) {
        int n = (h - yy < rows) ? h - yy : rows;
        const uint16_t *src = rgb565 + yy * w;
        for (int i = 0; i < w * n; i++) dma_buf[i] = (uint16_t)((src[i] >> 8) | (src[i] << 8));
        flush(x, y + yy, w, n);
    }
}
