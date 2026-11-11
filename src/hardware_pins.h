#pragma once

#include <Arduino.h>

/**
 * @file hardware_pins.h
 * @brief Mappatura multi-board basata sul silicio (ESP32-S3, ESP32, ESP32-C3, ESP32-C6)
 *        e supporto autorilevamento Display / Headless.
 */

// Flag globale definito in display_touch_driver.cpp che indica se il display/touch è fisicamente collegato
extern bool g_has_display;

// ============================================================================
// ALLOCAZIONE CORE FREERTOS (Dual-Core vs Single-Core RISC-V)
// ============================================================================
#if CONFIG_FREERTOS_UNICORE
    #define CHRONO_NET_CORE     0
    #define CHRONO_APP_CORE     0
#else
    #define CHRONO_NET_CORE     0
    #define CHRONO_APP_CORE     1
#endif

// ============================================================================
// CONFIGURAZIONE PIN MULTI-TARGET IN BASE AL SILICIO
// ============================================================================
#if CONFIG_IDF_TARGET_ESP32S3
    // --- FREENOVE FNK0104B (ESP32-S3-WROOM-1 N16R8) ---
    #define BOARD_TARGET_NAME   "ESP32-S3 (FNK0104B)"
    #define RELAY_PIN           43  // Pin TXD del connettore UART 4P
    #define PIN_RGB_LED         42  // WS2812 On-board (-1 se assente)

    // 1. Display TFT IPS 2.8" 240x320 (Driver ILI9341V - 4-Wire SPI)
    #define PIN_LCD_MOSI        11
    #define PIN_LCD_MISO        13
    #define PIN_LCD_SCLK        12
    #define PIN_LCD_CS          10
    #define PIN_LCD_DC          46
    #define PIN_LCD_RST         -1  // Connesso a EN
    #define PIN_LCD_BL          45  // HIGH = ON

    // 2. Touch Screen Capacitivo (Controller FT6336G - I2C 0x38)
    #define PIN_TOUCH_SDA       16
    #define PIN_TOUCH_SCL       15
    #define PIN_TOUCH_INT       17
    #define PIN_TOUCH_RST       18
    #define TOUCH_I2C_ADDR      0x38

#elif CONFIG_IDF_TARGET_ESP32
    // --- ESP32 CLASSICO (WROOM-32 / WROVER) ---
    #define BOARD_TARGET_NAME   "ESP32 Classic"
    #define RELAY_PIN           4
    #define PIN_RGB_LED         -1

    #define PIN_LCD_MOSI        23
    #define PIN_LCD_MISO        19
    #define PIN_LCD_SCLK        18
    #define PIN_LCD_CS          15
    #define PIN_LCD_DC          2
    #define PIN_LCD_RST         -1
    #define PIN_LCD_BL          21

    #define PIN_TOUCH_SDA       33
    #define PIN_TOUCH_SCL       32
    #define PIN_TOUCH_INT       -1
    #define PIN_TOUCH_RST       25
    #define TOUCH_I2C_ADDR      0x38

#elif CONFIG_IDF_TARGET_ESP32C3
    // --- ESP32-C3 (RISC-V Single-Core) ---
    #define BOARD_TARGET_NAME   "ESP32-C3 (RISC-V)"
    #define RELAY_PIN           2
    #define PIN_RGB_LED         8

    #define PIN_LCD_MOSI        -1
    #define PIN_LCD_MISO        -1
    #define PIN_LCD_SCLK        -1
    #define PIN_LCD_CS          -1
    #define PIN_LCD_DC          -1
    #define PIN_LCD_RST         -1
    #define PIN_LCD_BL          -1

    #define PIN_TOUCH_SDA       -1
    #define PIN_TOUCH_SCL       -1
    #define PIN_TOUCH_INT       -1
    #define PIN_TOUCH_RST       -1
    #define TOUCH_I2C_ADDR      0x38

#elif CONFIG_IDF_TARGET_ESP32C6
    // --- ESP32-C6 (RISC-V) ---
    #define BOARD_TARGET_NAME   "ESP32-C6 (RISC-V)"
    #define RELAY_PIN           15
    #define PIN_RGB_LED         8

    #define PIN_LCD_MOSI        -1
    #define PIN_LCD_MISO        -1
    #define PIN_LCD_SCLK        -1
    #define PIN_LCD_CS          -1
    #define PIN_LCD_DC          -1
    #define PIN_LCD_RST         -1
    #define PIN_LCD_BL          -1

    #define PIN_TOUCH_SDA       -1
    #define PIN_TOUCH_SCL       -1
    #define PIN_TOUCH_INT       -1
    #define PIN_TOUCH_RST       -1
    #define TOUCH_I2C_ADDR      0x38

#else
    #define BOARD_TARGET_NAME   "ESP32 Generic"
    #define RELAY_PIN           4
    #define PIN_RGB_LED         -1
    #define PIN_LCD_MOSI        -1
    #define PIN_LCD_MISO        -1
    #define PIN_LCD_SCLK        -1
    #define PIN_LCD_CS          -1
    #define PIN_LCD_DC          -1
    #define PIN_LCD_RST         -1
    #define PIN_LCD_BL          -1
    #define PIN_TOUCH_SDA       -1
    #define PIN_TOUCH_SCL       -1
    #define PIN_TOUCH_INT       -1
    #define PIN_TOUCH_RST       -1
    #define TOUCH_I2C_ADDR      0x38
#endif

// Alias e polarità del modulo Relè
#define PIN_RELAY_TRIGGER       RELAY_PIN
#define RELAY_ACTIVE_LOW        false // false = Active HIGH (0V riposo, 3.3V attivo); true = Active LOW
#define LCD_BL_ACTIVE_LEVEL     HIGH
