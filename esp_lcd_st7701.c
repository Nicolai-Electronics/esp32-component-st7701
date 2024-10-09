/*
 * SPDX-FileCopyrightText: 2024 Nicolai Electronics
 * SPDX-FileCopyrightText: 2023-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "soc/soc_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_lcd_st7701.h"

#define ST7701_CMD_BGR_BIT         (1ULL << 3)
#define ST7701_CMD_ML_BIT          (1ULL << 4)
#define ST7701_MDCTL_VALUE_DEFAULT (0x00)

typedef struct {
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    uint8_t madctl_val; // save current value of LCD_CMD_MADCTL register
    uint8_t colmod_val; // save current value of LCD_CMD_COLMOD register
    const st7701_lcd_init_cmd_t *init_cmds;
    uint16_t init_cmds_size;
    uint8_t lane_num;
    struct {
        unsigned int reset_level: 1;
    } flags;
    // To save the original functions of MIPI DPI panel
    esp_err_t (*del)(esp_lcd_panel_t *panel);
    esp_err_t (*init)(esp_lcd_panel_t *panel);
} st7701_panel_t;

static const char *TAG = "ST7701";

static esp_err_t panel_st7701_send_init_cmds(st7701_panel_t *st7701);

static esp_err_t panel_st7701_del(esp_lcd_panel_t *panel);
static esp_err_t panel_st7701_init(esp_lcd_panel_t *panel);
static esp_err_t panel_st7701_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_st7701_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_st7701_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);

esp_err_t esp_lcd_new_panel_st7701(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel)
{
    ESP_LOGI(TAG, "version: %d.%d.%d", ESP_LCD_ST7701_VER_MAJOR, ESP_LCD_ST7701_VER_MINOR,
             ESP_LCD_ST7701_VER_PATCH);
    ESP_RETURN_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, TAG, "invalid arguments");
    st7701_vendor_config_t *vendor_config = (st7701_vendor_config_t *)panel_dev_config->vendor_config;
    ESP_RETURN_ON_FALSE(vendor_config && vendor_config->mipi_config.dpi_config && vendor_config->mipi_config.dsi_bus, ESP_ERR_INVALID_ARG, TAG,
                        "invalid vendor config");

    esp_err_t ret = ESP_OK;
    st7701_panel_t *st7701 = (st7701_panel_t *)calloc(1, sizeof(st7701_panel_t));
    ESP_RETURN_ON_FALSE(st7701, ESP_ERR_NO_MEM, TAG, "no mem for st7701 panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        gpio_config_t io_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure GPIO for RST line failed");
    }

    switch (panel_dev_config->rgb_ele_order) {
    case LCD_RGB_ELEMENT_ORDER_RGB:
        st7701->madctl_val &= ~(ST7701_CMD_BGR_BIT);
        break;
    case LCD_RGB_ELEMENT_ORDER_BGR:
        st7701->madctl_val |= ST7701_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported color space");
        break;
    }

    switch (panel_dev_config->bits_per_pixel) {
    case 16: // RGB565
        st7701->colmod_val = 0x50;
        break;
    case 18: // RGB666
        st7701->colmod_val = 0x60;
        break;
    case 24: // RGB888
        st7701->colmod_val = 0x70;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported pixel width");
        break;
    }

    st7701->io = io;
    st7701->init_cmds = vendor_config->init_cmds;
    st7701->init_cmds_size = vendor_config->init_cmds_size;
    st7701->lane_num = vendor_config->mipi_config.lane_num;
    st7701->reset_gpio_num = panel_dev_config->reset_gpio_num;
    st7701->flags.reset_level = panel_dev_config->flags.reset_active_high;
    st7701->madctl_val = ST7701_MDCTL_VALUE_DEFAULT;

    // Create MIPI DPI panel
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_dpi(vendor_config->mipi_config.dsi_bus, vendor_config->mipi_config.dpi_config, ret_panel), err, TAG,
                      "create MIPI DPI panel failed");
    ESP_LOGD(TAG, "new MIPI DPI panel @%p", *ret_panel);

    // Save the original functions of MIPI DPI panel
    st7701->del = (*ret_panel)->del;
    st7701->init = (*ret_panel)->init;
    // Overwrite the functions of MIPI DPI panel
    (*ret_panel)->del = panel_st7701_del;
    (*ret_panel)->init = panel_st7701_init;
    (*ret_panel)->reset = panel_st7701_reset;
    (*ret_panel)->mirror = panel_st7701_mirror;
    (*ret_panel)->invert_color = panel_st7701_invert_color;
    (*ret_panel)->user_data = st7701;
    ESP_LOGD(TAG, "new st7701 panel @%p", st7701);

    return ESP_OK;

err:
    if (st7701) {
        if (panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        free(st7701);
    }
    return ret;
}

static const st7701_lcd_init_cmd_t vendor_specific_init_default[] = {
    //  {cmd, { data }, data_size, delay_ms}
    {0xFF,           (uint8_t []){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},                                                                    // Regular command function
    {LCD_CMD_NORON,  (uint8_t []){0x00}, 0, 0},                                                                                            // Turn on normal display mode
    {0xEF,           (uint8_t []){0x08}, 1, 0},                                                                                            //

    {0xFF,           (uint8_t []){0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},                                                                    // Command 2 BK0 function
    {0xC0,           (uint8_t []){0x63, 0x00}, 2, 0},                                                                                      // LNESET (Display Line Setting): (0x63+1)*8 = 800 lines
    {0xC1,           (uint8_t []){0x10, 0x02}, 2, 0},                                                                                      // PORCTRL (Porch Control): VBP = 16, VFP = 2
    {0xC2,           (uint8_t []){0x37, 0x08}, 2, 0},                                                                                      // INVSET (Inversion sel. & frame rate control): PCLK=512+(8*16) = 640
    {0xCC,           (uint8_t []){0x38}, 1, 0},                                                                                            //
    {0xB0,           (uint8_t []){0x40, 0xC9, 0x90, 0x0D, 0x0F, 0x04, 0x00, 0x07, 0x07, 0x1C, 0x04, 0x52, 0x0F, 0xDF, 0x26, 0xCF}, 16, 0}, // PVGAMCTRL
    {0xB1,           (uint8_t []){0x40, 0xC9, 0xCF, 0x0C, 0x90, 0x04, 0x00, 0x07, 0x08, 0x1B, 0x06, 0x55, 0x13, 0x62, 0xE7, 0xCF}, 16, 0}, // NVGAMCTRL

    {0xFF,           (uint8_t []){0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},                                                                    // Command 2 BK1 function
    {0xB0,           (uint8_t []){0x5D}, 1, 0},                                                                                            // VRHS
    {0xB1,           (uint8_t []){0x2D}, 1, 0},                                                                                            // VCOMS
    {0xB2,           (uint8_t []){0x07}, 1, 0},                                                                                            // VGH
    {0xB3,           (uint8_t []){0x80}, 1, 0},                                                                                            // TESTCMD
    {0xB5,           (uint8_t []){0x08}, 1, 0},                                                                                            // VGLS
    {0xB7,           (uint8_t []){0x85}, 1, 0},                                                                                            // PWCTRL1
    {0xB8,           (uint8_t []){0x20}, 1, 0},                                                                                            // PWCTRL2
    {0xB9,           (uint8_t []){0x10}, 1, 0},                                                                                            // DGMLUTR
    {0xC1,           (uint8_t []){0x78}, 1, 0},                                                                                            // SPD1
    {0xC2,           (uint8_t []){0x78}, 1, 0},                                                                                            // SPD2
    {0xD0,           (uint8_t []){0x88}, 1, 100},                                                                                          // MIPISET1
    {0xE0,           (uint8_t []){0x00, 0x19, 0x02}, 3, 0},                                                                                //
    {0xE1,           (uint8_t []){0x05, 0xA0, 0x07, 0xA0, 0x04, 0xA0, 0x06, 0xA0, 0x00, 0x44, 0x44}, 11, 0},                               //
    {0xE2,           (uint8_t []){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 13, 0},                   //
    {0xE3,           (uint8_t []){0x00, 0x00, 0x33, 0x33}, 5, 0},                                                                          //
    {0xE4,           (uint8_t []){0x44, 0x44}, 2, 0},                                                                                      //
    {0xE5,           (uint8_t []){0x0D, 0x31, 0xC8, 0xAF, 0x0F, 0x33, 0xC8, 0xAF, 0x09, 0x2D, 0xC8, 0xAF, 0x0B, 0x2F, 0xC8, 0xAF}, 16, 0}, //
    {0xE6,           (uint8_t []){0x00, 0x00, 0x33, 0x33}, 4, 0},                                                                          //
    {0xE7,           (uint8_t []){0x44, 0x44}, 2, 0},                                                                                      //
    {0xE8,           (uint8_t []){0x0C, 0x30, 0xC8, 0xAF, 0x0E, 0x32, 0xC8, 0xAF, 0x08, 0x2C, 0xC8, 0xAF, 0x0A, 0x2E, 0xC8, 0xAF}, 16, 0}, //
    {0xEB,           (uint8_t []){0x02, 0x00, 0xE4, 0xE4, 0x44, 0x00, 0x40}, 7, 0},                                                        //
    {0xEC,           (uint8_t []){0x3C, 0x00}, 2, 0},                                                                                      //
    {0xED,           (uint8_t []){0xAB, 0x89, 0x76, 0x54, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x10, 0x45, 0x67, 0x98, 0xBA}, 16, 0}, //

    {0xFF,           (uint8_t []){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},                                                                    // Regular command function
    {LCD_CMD_SLPOUT, (uint8_t []){0x00}, 0, 120},                                                                                          // Exit sleep mode
    {LCD_CMD_DISPON, (uint8_t []){0x00}, 0, 50},                                                                                           // Display on (enable frame buffer output)
};

static esp_err_t panel_st7701_send_init_cmds(st7701_panel_t *st7701)
{
    esp_lcd_panel_io_handle_t io = st7701->io;
    const st7701_lcd_init_cmd_t *init_cmds = NULL;
    uint16_t init_cmds_size = 0;

    // vendor specific initialization, it can be different between manufacturers
    // should consult the LCD supplier for initialization sequence code
    if (st7701->init_cmds) {
        init_cmds = st7701->init_cmds;
        init_cmds_size = st7701->init_cmds_size;
    } else {
        init_cmds = vendor_specific_init_default;
        init_cmds_size = sizeof(vendor_specific_init_default) / sizeof(st7701_lcd_init_cmd_t);
    }

    for (int i = 0; i < init_cmds_size; i++) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, init_cmds[i].cmd, init_cmds[i].data, init_cmds[i].data_bytes), TAG, "send command failed");
        vTaskDelay(pdMS_TO_TICKS(init_cmds[i].delay_ms));
    }

    ESP_LOGD(TAG, "send init commands success");

    return ESP_OK;
}

static esp_err_t panel_st7701_del(esp_lcd_panel_t *panel)
{
    st7701_panel_t *st7701 = (st7701_panel_t *)panel->user_data;

    if (st7701->reset_gpio_num >= 0) {
        gpio_reset_pin(st7701->reset_gpio_num);
    }
    // Delete MIPI DPI panel
    st7701->del(panel);
    free(st7701);
    ESP_LOGD(TAG, "del st7701 panel @%p", st7701);

    return ESP_OK;
}

static esp_err_t panel_st7701_init(esp_lcd_panel_t *panel)
{
    st7701_panel_t *st7701 = (st7701_panel_t *)panel->user_data;

    ESP_RETURN_ON_ERROR(panel_st7701_send_init_cmds(st7701), TAG, "send init commands failed");
    ESP_RETURN_ON_ERROR(st7701->init(panel), TAG, "init MIPI DPI panel failed");

    return ESP_OK;
}

static esp_err_t panel_st7701_reset(esp_lcd_panel_t *panel)
{
    st7701_panel_t *st7701 = (st7701_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = st7701->io;

    // Perform hardware reset
    if (st7701->reset_gpio_num >= 0) {
        gpio_set_level(st7701->reset_gpio_num, st7701->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(st7701->reset_gpio_num, !st7701->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(20));
    } else if (io) { // Perform software reset
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SWRESET, NULL, 0), TAG, "send command failed");
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    return ESP_OK;
}

static esp_err_t panel_st7701_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    (void)mirror_y;
    st7701_panel_t *st7701 = (st7701_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = st7701->io;
    uint8_t madctl_val = st7701->madctl_val;

    ESP_RETURN_ON_FALSE(io, ESP_ERR_INVALID_STATE, TAG, "invalid panel IO");

    // Control mirror through LCD command
    if (mirror_x) {
        madctl_val |= ST7701_CMD_ML_BIT;
    } else {
        madctl_val &= ~ST7701_CMD_ML_BIT;
    }

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t []) {
        madctl_val
    }, 1), TAG, "send command failed");
    st7701->madctl_val = madctl_val;

    return ESP_OK;
}

static esp_err_t panel_st7701_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    st7701_panel_t *st7701 = (st7701_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = st7701->io;
    uint8_t command = 0;

    ESP_RETURN_ON_FALSE(io, ESP_ERR_INVALID_STATE, TAG, "invalid panel IO");

    if (invert_color_data) {
        command = LCD_CMD_INVON;
    } else {
        command = LCD_CMD_INVOFF;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), TAG, "send command failed");

    return ESP_OK;
}
