#include "display_touch_driver.h"
#include <Wire.h>
#include <esp_heap_caps.h>

// Definizione del flag globale di presenza hardware del display
bool g_has_display = false;

LGFX_ChronoRing    DisplayTouchDriver::_tft;
lv_disp_draw_buf_t DisplayTouchDriver::_drawBuf;
lv_disp_drv_t      DisplayTouchDriver::_dispDrv;
lv_indev_drv_t     DisplayTouchDriver::_indevDrv;
lv_color_t*        DisplayTouchDriver::_buf1 = nullptr;
lv_color_t*        DisplayTouchDriver::_buf2 = nullptr;

constexpr uint16_t DisplayTouchDriver::SCREEN_WIDTH;
constexpr uint16_t DisplayTouchDriver::SCREEN_HEIGHT;
constexpr size_t   DisplayTouchDriver::LVGL_BUF_LINES;

LGFX_ChronoRing::LGFX_ChronoRing() {
#if (PIN_LCD_SCLK >= 0) && (PIN_LCD_MOSI >= 0) && (PIN_LCD_DC >= 0)
    // 1. Configurazione Bus SPI2 a 4 fili per ILI9341V
    {
        auto cfg = _bus_instance.config();
        cfg.spi_host    = SPI2_HOST;
        cfg.spi_mode    = 0;
        cfg.freq_write  = 40000000;
        cfg.freq_read   = 16000000;
        cfg.spi_3wire   = false;
        cfg.use_lock    = true;
        cfg.dma_channel = SPI_DMA_CH_AUTO;
        cfg.pin_sclk    = PIN_LCD_SCLK;
        cfg.pin_mosi    = PIN_LCD_MOSI;
        cfg.pin_miso    = PIN_LCD_MISO;
        cfg.pin_dc      = PIN_LCD_DC;
        _bus_instance.config(cfg);
        _panel_instance.setBus(&_bus_instance);
    }

    // 2. Configurazione Pannello IPS 2.8" 240x320 (ILI9341V)
    {
        auto cfg = _panel_instance.config();
        cfg.pin_cs           = PIN_LCD_CS;
        cfg.pin_rst          = PIN_LCD_RST;
        cfg.pin_busy         = -1;
        cfg.panel_width      = 240;
        cfg.panel_height     = 320;
        cfg.offset_x         = 0;
        cfg.offset_y         = 0;
        cfg.offset_rotation  = 0;
        cfg.dummy_read_pixel = 8;
        cfg.dummy_read_bits  = 1;
        cfg.readable         = true;
        cfg.invert           = true;
        cfg.rgb_order        = false;
        cfg.dlen_16bit       = false;
        cfg.bus_shared       = false;
        _panel_instance.config(cfg);
    }

    // 3. Configurazione Backlight PWM
    if (PIN_LCD_BL >= 0) {
        auto cfg = _light_instance.config();
        cfg.pin_bl      = PIN_LCD_BL;
        cfg.invert      = false;
        cfg.freq        = 12000;
        cfg.pwm_channel = 7;
        _light_instance.config(cfg);
        _panel_instance.setLight(&_light_instance);
    }

    // 4. Configurazione Touch Capacitivo FT6336G via I2C (0x38)
    if (PIN_TOUCH_SDA >= 0 && PIN_TOUCH_SCL >= 0) {
        auto cfg = _touch_instance.config();
        cfg.x_min           = 0;
        cfg.x_max           = 239;
        cfg.y_min           = 0;
        cfg.y_max           = 319;
        cfg.pin_int         = PIN_TOUCH_INT;
        cfg.pin_rst         = PIN_TOUCH_RST;
        cfg.bus_shared      = false;
        cfg.offset_rotation = 0;
        cfg.i2c_port        = 0;
        cfg.i2c_addr        = TOUCH_I2C_ADDR;
        cfg.pin_sda         = PIN_TOUCH_SDA;
        cfg.pin_scl         = PIN_TOUCH_SCL;
        cfg.freq            = 400000;
        _touch_instance.config(cfg);
        _panel_instance.setTouch(&_touch_instance);
    }

    setPanel(&_panel_instance);
#endif
}

bool DisplayTouchDriver::probeTouchI2C() {
#if (PIN_TOUCH_SDA >= 0) && (PIN_TOUCH_SCL >= 0)
    // Impulso di reset hardware sul pin RST del touch (se presente) per risvegliare l'FT6336G prima del probe
    if (PIN_TOUCH_RST >= 0) {
        pinMode(PIN_TOUCH_RST, OUTPUT);
        digitalWrite(PIN_TOUCH_RST, LOW);
        delay(20);
        digitalWrite(PIN_TOUCH_RST, HIGH);
        delay(100);
    }

    if (PIN_TOUCH_INT >= 0) {
        pinMode(PIN_TOUCH_INT, INPUT_PULLUP);
    }

    // Avvio temporaneo del bus I2C per verificare l'ACK sull'indirizzo 0x38
    if (!Wire.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL, 100000)) {
        return false;
    }

    Wire.beginTransmission(TOUCH_I2C_ADDR);
    uint8_t error = Wire.endTransmission(true);
    Wire.end(); // Rilascia il bus I2C così LovyanGFX può inizializzarlo senza conflitti

    return (error == 0);
#else
    return false;
#endif
}

bool DisplayTouchDriver::begin() {
    // 1. Esegue il probe I2C sull'indirizzo 0x38 prima di inizializzare LovyanGFX
    if (!probeTouchI2C()) {
        g_has_display = false;
        if (PIN_LCD_BL >= 0) {
            pinMode(PIN_LCD_BL, OUTPUT);
            digitalWrite(PIN_LCD_BL, LOW);
        }
        Serial.println("[DISPLAY] Schermo non rilevato. Avvio in modalità Headless (solo Web/Scheduler).");
        return false;
    }

    g_has_display = true;
    Serial.printf("[DISPLAY] Controller Touch FT6336G (0x%02X) rilevato! Avvio modalita Display Touch.\n", TOUCH_I2C_ADDR);

    // 2. Inizializzazione display e touch tramite LovyanGFX
    _tft.init();
    _tft.setRotation(1);     // Landscape 320x240
    _tft.setSwapBytes(true); // Gestisce lo swap endianness RGB565 in hardware/DMA
    _tft.setBrightness(255); // Backlight accesa al 100%
    _tft.fillScreen(TFT_BLACK);

    // 3. Inizializzazione core LVGL
    lv_init();

    // 4. Allocazione double-buffer LVGL (in PSRAM se disponibile, altrimenti fallback in SRAM interna)
    const size_t bufPixels = SCREEN_WIDTH * LVGL_BUF_LINES;
    const size_t bufBytes  = bufPixels * sizeof(lv_color_t);

#ifdef BOARD_HAS_PSRAM
    if (psramFound()) {
        _buf1 = static_cast<lv_color_t*>(heap_caps_malloc(bufBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        _buf2 = static_cast<lv_color_t*>(heap_caps_malloc(bufBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
#endif

    if (_buf1 == nullptr || _buf2 == nullptr) {
        Serial.println("[DISPLAY] Allocazione buffer LVGL in SRAM interna ridotta (40 linee).");
        const size_t fallbackPixels = SCREEN_WIDTH * 40;
        _buf1 = static_cast<lv_color_t*>(heap_caps_malloc(fallbackPixels * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        _buf2 = nullptr;
        if (_buf1 == nullptr) {
            Serial.println("[DISPLAY] ERRORE CRITICO: Memoria insufficiente per il buffer LVGL! Passaggio a Headless.");
            g_has_display = false;
            return false;
        }
        lv_disp_draw_buf_init(&_drawBuf, _buf1, nullptr, fallbackPixels);
    } else {
        Serial.printf("[DISPLAY] Double-buffer LVGL allocato in PSRAM (2 x %u KB = %u KB totali)\n",
                      (unsigned)(bufBytes / 1024), (unsigned)((bufBytes * 2) / 1024));
        lv_disp_draw_buf_init(&_drawBuf, _buf1, _buf2, bufPixels);
    }

    // 5. Registrazione driver display in LVGL
    lv_disp_drv_init(&_dispDrv);
    _dispDrv.hor_res  = SCREEN_WIDTH;
    _dispDrv.ver_res  = SCREEN_HEIGHT;
    _dispDrv.flush_cb = DisplayTouchDriver::lvglFlushCb;
    _dispDrv.draw_buf = &_drawBuf;
    lv_disp_drv_register(&_dispDrv);

    // 6. Registrazione driver touch capacitivo in LVGL
    lv_indev_drv_init(&_indevDrv);
    _indevDrv.type    = LV_INDEV_TYPE_POINTER;
    _indevDrv.read_cb = DisplayTouchDriver::lvglTouchReadCb;
    lv_indev_drv_register(&_indevDrv);

    Serial.println("[DISPLAY] ILI9341V (320x240 Landscape) + Touch FT6336G (0x38) inizializzati con successo.");
    return true;
}

void DisplayTouchDriver::setBacklightBrightness(uint8_t brightness) {
    if (!g_has_display) return;
    _tft.setBrightness(brightness);
}

void DisplayTouchDriver::lvglFlushCb(lv_disp_drv_t* disp, const lv_area_t* area, lv_color_t* color_p) {
    if (!g_has_display) {
        lv_disp_flush_ready(disp);
        return;
    }
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

    _tft.startWrite();
    _tft.pushImage(area->x1, area->y1, w, h, reinterpret_cast<uint16_t*>(&color_p->full));
    _tft.endWrite();

    lv_disp_flush_ready(disp);
}

void DisplayTouchDriver::lvglTouchReadCb(lv_indev_drv_t* indev_driver, lv_indev_data_t* data) {
    if (!g_has_display) {
        data->state = LV_INDEV_STATE_REL;
        return;
    }

    uint16_t rawX = 0, rawY = 0;
    bool touched = _tft.getTouch(&rawX, &rawY);

    if (!touched) {
        data->state = LV_INDEV_STATE_REL;
    } else {
        uint16_t x = (rawX < SCREEN_WIDTH)  ? (SCREEN_WIDTH - 1 - rawX)  : 0;
        uint16_t y = (rawY < SCREEN_HEIGHT) ? (SCREEN_HEIGHT - 1 - rawY) : 0;

        data->state   = LV_INDEV_STATE_PR;
        data->point.x = x;
        data->point.y = y;
    }
}
