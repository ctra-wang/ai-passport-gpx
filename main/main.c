#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "route_app.h"

#include "esp_log.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "CTRA GPX roadbook navigator starting");

    esp_err_t err = bsp_i2c_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "shared I2C init failed: %s", esp_err_to_name(err));
    }

    if (bsp_display_init() != ESP_OK || bsp_lvgl_init() == NULL) {
        ESP_LOGE(TAG,
                 "display/LVGL init failed (MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    err = route_app_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "route application failed to start: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "CTRA route application ready");
}
