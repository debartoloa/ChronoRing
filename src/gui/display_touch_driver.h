#pragma once

#include <Arduino.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lvgl.h>
#include "../hardware_pins.h"

/**
 * @file display_touch_driver.h
 * @brief Driver Hardware LovyanGFX (ILI9341V SPI + FT6336G I2C Touch) con Probe I2C per modalità Headless
 */

class LGFX_ChronoRing : public lgfx::LGFX_Device {
    lgfx::Panel_ILI9341 _panel_instance;
    lgfx::Bus_SPI       _bus_instance;
    lgfx::Light_PWM     _light_instance;
    lgfx::Touch_FT5x06  _touch_instance; // Compatibile a livello registri con FT6336G/FT6336U (0x38)

public:
    LGFX_ChronoRing();
};

class DisplayTouchDriver {
public:
    static constexpr uint16_t SCREEN_WIDTH   = 320; // Landscape
    static constexpr uint16_t SCREEN_HEIGHT  = 240; // Landscape
    static constexpr size_t   LVGL_BUF_LINES = 120; // Metà schermo per ciascuno dei 2 buffer in PSRAM

    /**
     * @brief Esegue un probe I2C sull'indirizzo 0x38 (FT6336G) sui pin TOUCH_SDA/TOUCH_SCL.
     * @return true se il controller Touch/Display risponde con ACK, false se assente.
     */
    static bool probeTouchI2C();

    /**
     * @brief Verifica la presenza dello schermo via I2C e, se presente, inizializza Display, Touch e buffer LVGL.
     * @return true se il display è presente e inizializzato, false se avviato in modalità Headless.
     */
    static bool begin();

    /**
     * @brief Imposta la luminosità della retroilluminazione (protetta da g_has_display).
     */
    static void setBacklightBrightness(uint8_t brightness);

private:
    static LGFX_ChronoRing    _tft;
    static lv_disp_draw_buf_t _drawBuf;
    static lv_disp_drv_t      _dispDrv;
    static lv_indev_drv_t     _indevDrv;
    static lv_color_t*        _buf1;
    static lv_color_t*        _buf2;

    static void lvglFlushCb(lv_disp_drv_t* disp, const lv_area_t* area, lv_color_t* color_p);
    static void lvglTouchReadCb(lv_indev_drv_t* indev_driver, lv_indev_data_t* data);
};
