#include "gui_manager.h"
#include "display_touch_driver.h"
#include "../hardware_pins.h"
#include "../config_manager.h"
#include "../relay_controller.h"
#include "../network/network_manager.h"
#include "../network/weather_service.h"
#include "../scheduler/scheduler.h"
#include <LittleFS.h>
#include <esp_heap_caps.h>

TaskHandle_t GuiManager::_guiTaskHandle = nullptr;
volatile bool GuiManager::_stateDirty = false;
volatile bool GuiManager::_targetSuspendedState = false;
volatile bool GuiManager::_brandingDirty = false;
volatile bool GuiManager::_ringingDirty = false;
bool GuiManager::_lastAppliedSuspended = false;
bool GuiManager::_lastRelayActive = false;
int  GuiManager::_lastRenderedWeatherCode = -999;
bool GuiManager::_inStandby = false;

uint8_t*     GuiManager::_logoPixelBuf = nullptr;
lv_img_dsc_t GuiManager::_logoImgDsc   = {};
bool         GuiManager::_logoValid    = false;

lv_obj_t* GuiManager::_screenFrame       = nullptr;
lv_obj_t* GuiManager::_lblDate           = nullptr;
lv_obj_t* GuiManager::_lblNtpStatus      = nullptr;
lv_obj_t* GuiManager::_lblWifiStatus     = nullptr;

lv_obj_t* GuiManager::_lblClockHM        = nullptr;
lv_obj_t* GuiManager::_lblClockSec       = nullptr;
lv_obj_t* GuiManager::_eventBox          = nullptr;
lv_obj_t* GuiManager::_lblEventTitle     = nullptr;
lv_obj_t* GuiManager::_lblEventSub       = nullptr;

lv_obj_t* GuiManager::_weatherIconBox    = nullptr;
lv_obj_t* GuiManager::_lblWeatherTemp    = nullptr;
lv_obj_t* GuiManager::_lblWeatherHum     = nullptr;
lv_obj_t* GuiManager::_lblWeatherWind    = nullptr;

lv_obj_t* GuiManager::_btnRingNow        = nullptr;
lv_obj_t* GuiManager::_btnSuspend        = nullptr;
lv_obj_t* GuiManager::_lblBtnSuspend     = nullptr;
lv_obj_t* GuiManager::_relayIndicatorBox = nullptr;
lv_obj_t* GuiManager::_relayLedDot       = nullptr;
lv_obj_t* GuiManager::_lblRelayState     = nullptr;

lv_obj_t* GuiManager::_standbyOverlay    = nullptr;
lv_obj_t* GuiManager::_imgStandbyLogo    = nullptr;
lv_obj_t* GuiManager::_lblStandbyClock   = nullptr;
lv_obj_t* GuiManager::_lblStandbySchool  = nullptr;
lv_obj_t* GuiManager::_lblStandbyDate    = nullptr;

constexpr uint32_t GuiManager::STANDBY_TIMEOUT_MS;
constexpr uint8_t  GuiManager::BRIGHTNESS_ACTIVE;
constexpr uint8_t  GuiManager::BRIGHTNESS_STANDBY;

// ============================================================================
// FUNZIONI DI STATO GLOBALI DISACCOPPIATE (PROTETTI DA g_has_display)
// ============================================================================
void gui_update_state(bool is_suspended) {
    GuiManager::requestStateUpdate(is_suspended);
}

void gui_set_bell_ringing(bool is_ringing) {
    GuiManager::requestBellRingingUpdate(is_ringing);
}

void GuiManager::updateOnboardRgbLed(bool isSuspended) {
#if (PIN_RGB_LED >= 0)
    if (isSuspended) {
        neopixelWrite(PIN_RGB_LED, 45, 0, 0); // Rosso fisso
    } else {
        neopixelWrite(PIN_RGB_LED, 0, 14, 0); // Verde fisso a bassa intensità
    }
#else
    (void)isSuspended;
#endif
}

void GuiManager::requestStateUpdate(bool isSuspended) {
    // Aggiorna sempre il LED RGB onboard se presente sulla scheda
    updateOnboardRgbLed(isSuspended);

    // Se siamo in modalità Headless (nessun display), non toccare mai lo stato LVGL
    if (!g_has_display) {
        return;
    }

    _targetSuspendedState = isSuspended;
    _stateDirty = true;
}

void GuiManager::requestBellRingingUpdate(bool isRinging) {
    (void)isRinging;
    if (!g_has_display) {
        return;
    }
    _ringingDirty = true;
}

void GuiManager::requestBrandingReload() {
    if (!g_has_display) {
        return;
    }
    _brandingDirty = true;
}

bool GuiManager::begin() {
    bool initialSuspended = ConfigManager::isSuspended();
    updateOnboardRgbLed(initialSuspended);

    // Esegue il probe I2C (0x38) tramite DisplayTouchDriver::begin()
    if (!DisplayTouchDriver::begin()) {
        // g_has_display è già false: salta allocazione widget e creazione task LVGL
        return false;
    }

    _targetSuspendedState = initialSuspended;
    _lastAppliedSuspended = initialSuspended;

    buildUi();
    loadSchoolLogoFromFs();
    updateStandbyBrandingLayout();
    applySuspendedVisuals(initialSuspended);
    refreshDynamicData();

    if (_guiTaskHandle == nullptr) {
        xTaskCreatePinnedToCore(
            GuiManager::guiTask,
            "ChronoGuiTask",
            8192,
            nullptr,
            3,
            &_guiTaskHandle,
            CHRONO_APP_CORE
        );
        Serial.printf("[GUI] Task LVGL avviato su Core %d.\n", CHRONO_APP_CORE);
    }

    return true;
}

// ============================================================================
// CARICAMENTO LOGO SCUOLA RGB565 DA LITTLEFS (/school_logo.bin)
// ============================================================================
void GuiManager::loadSchoolLogoFromFs() {
    if (!g_has_display) return;
    _logoValid = false;

    if (!LittleFS.exists(ConfigManager::SCHOOL_LOGO_PATH)) {
        return;
    }

    File f = LittleFS.open(ConfigManager::SCHOOL_LOGO_PATH, FILE_READ);
    if (!f || f.size() < 4) {
        if (f) f.close();
        return;
    }

    uint16_t w = 0, h = 0;
    f.read(reinterpret_cast<uint8_t*>(&w), 2);
    f.read(reinterpret_cast<uint8_t*>(&h), 2);

    size_t expectedBytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 2;
    if (w == 0 || w > 160 || h == 0 || h > 120 || f.size() < (4 + expectedBytes)) {
        Serial.println("[GUI] File /school_logo.bin non valido o dimensioni fuori range.");
        f.close();
        return;
    }

    if (_logoPixelBuf != nullptr) {
        free(_logoPixelBuf);
        _logoPixelBuf = nullptr;
    }

#ifdef BOARD_HAS_PSRAM
    if (psramFound()) {
        _logoPixelBuf = static_cast<uint8_t*>(heap_caps_malloc(expectedBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
#endif
    if (_logoPixelBuf == nullptr) {
        _logoPixelBuf = static_cast<uint8_t*>(malloc(expectedBytes));
    }

    if (_logoPixelBuf == nullptr) {
        Serial.println("[GUI] Impossibile allocare buffer per il logo scuola.");
        f.close();
        return;
    }

    size_t bytesRead = f.read(_logoPixelBuf, expectedBytes);
    f.close();

    if (bytesRead != expectedBytes) {
        free(_logoPixelBuf);
        _logoPixelBuf = nullptr;
        return;
    }

    memset(&_logoImgDsc, 0, sizeof(_logoImgDsc));
    _logoImgDsc.header.always_zero = 0;
    _logoImgDsc.header.w = w;
    _logoImgDsc.header.h = h;
    _logoImgDsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    _logoImgDsc.data_size = expectedBytes;
    _logoImgDsc.data = _logoPixelBuf;

    _logoValid = true;
    Serial.printf("[GUI] Logo Scuola caricato (%ux%u px, %u byte).\n", w, h, (unsigned)expectedBytes);
}

void GuiManager::updateStandbyBrandingLayout() {
    if (!g_has_display || !_standbyOverlay) return;

    String schoolName = ConfigManager::getSchoolName();
    schoolName.trim();
    bool hasName = (schoolName.length() > 0);

    if (_logoValid) {
        lv_img_set_src(_imgStandbyLogo, &_logoImgDsc);
        lv_obj_clear_flag(_imgStandbyLogo, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(_imgStandbyLogo, LV_OBJ_FLAG_HIDDEN);
    }

    if (hasName) {
        lv_label_set_text(_lblStandbySchool, schoolName.c_str());
        lv_obj_clear_flag(_lblStandbySchool, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(_lblStandbySchool, LV_OBJ_FLAG_HIDDEN);
    }

    if (_logoValid && hasName) {
        lv_obj_align(_imgStandbyLogo,   LV_ALIGN_TOP_MID,    0, 12);
        lv_obj_align(_lblStandbyClock,  LV_ALIGN_CENTER,     0, 12);
        lv_obj_align(_lblStandbySchool, LV_ALIGN_BOTTOM_MID, 0, -24);
        lv_obj_align(_lblStandbyDate,   LV_ALIGN_BOTTOM_MID, 0, -6);
    } else if (_logoValid && !hasName) {
        lv_obj_align(_imgStandbyLogo,   LV_ALIGN_TOP_MID,    0, 18);
        lv_obj_align(_lblStandbyClock,  LV_ALIGN_CENTER,     0, 22);
        lv_obj_align(_lblStandbyDate,   LV_ALIGN_BOTTOM_MID, 0, -12);
    } else if (!_logoValid && hasName) {
        lv_obj_align(_lblStandbyClock,  LV_ALIGN_CENTER,     0, -22);
        lv_obj_align(_lblStandbySchool, LV_ALIGN_CENTER,     0, 28);
        lv_obj_align(_lblStandbyDate,   LV_ALIGN_BOTTOM_MID, 0, -10);
    } else {
        lv_obj_align(_lblStandbyClock,  LV_ALIGN_CENTER,     0, -12);
        lv_obj_align(_lblStandbyDate,   LV_ALIGN_CENTER,     0, 32);
    }
}

void GuiManager::enterStandby() {
    if (!g_has_display || _inStandby || !_standbyOverlay) return;
    _inStandby = true;
    lv_obj_clear_flag(_standbyOverlay, LV_OBJ_FLAG_HIDDEN);
    DisplayTouchDriver::setBacklightBrightness(BRIGHTNESS_STANDBY);
    Serial.println("[GUI] Ingresso in modalita Standby (luminosita ridotta + orologio a tutto schermo).");
}

void GuiManager::exitStandby() {
    if (!g_has_display || !_inStandby || !_standbyOverlay) return;
    _inStandby = false;
    lv_obj_add_flag(_standbyOverlay, LV_OBJ_FLAG_HIDDEN);
    DisplayTouchDriver::setBacklightBrightness(BRIGHTNESS_ACTIVE);
    lv_disp_trig_activity(nullptr);
    Serial.println("[GUI] Risveglio dallo Standby (luminosita 100%).");
}

void GuiManager::onStandbyTouched(lv_event_t* e) {
    (void)e;
    exitStandby();
}

// ============================================================================
// GENERATORE DI ICONE METEO VETTORIALI MODERNE (Flat / Glassmorphism Style)
// ============================================================================
static lv_obj_t* createCirclePart(lv_obj_t* parent, lv_coord_t x, lv_coord_t y, lv_coord_t diameter, uint32_t hexColor, lv_opa_t opa = LV_OPA_COVER) {
    lv_obj_t* c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, diameter, diameter);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(hexColor), 0);
    lv_obj_set_style_bg_opa(c, opa, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static lv_obj_t* createPillPart(lv_obj_t* parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h, uint32_t hexColor, lv_opa_t opa = LV_OPA_COVER) {
    lv_obj_t* p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, w, h);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_style_radius(p, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(p, lv_color_hex(hexColor), 0);
    lv_obj_set_style_bg_opa(p, opa, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

static void drawModernCloud(lv_obj_t* box, lv_coord_t offsetX, lv_coord_t offsetY, uint32_t hexColor) {
    createPillPart(box, offsetX + 2,  offsetY + 11, 34, 13, hexColor);
    createCirclePart(box, offsetX + 6,  offsetY + 3,  16, hexColor);
    createCirclePart(box, offsetX + 16, offsetY + 1,  18, hexColor);
}

void GuiManager::renderModernWeatherIcon(int wmoCode) {
    if (!g_has_display || !_weatherIconBox) return;
    lv_obj_clean(_weatherIconBox);

    if (wmoCode < 0) {
        createCirclePart(_weatherIconBox, 6, 4, 20, 0xF59E0B, LV_OPA_70);
        drawModernCloud(_weatherIconBox, 10, 10, 0x94A3B8);
        return;
    }

    if (wmoCode == 0) {
        createCirclePart(_weatherIconBox, 9,  1, 34, 0xF59E0B, LV_OPA_30);
        createCirclePart(_weatherIconBox, 13, 5, 26, 0xFBBF24, LV_OPA_COVER);
        createCirclePart(_weatherIconBox, 16, 8, 20, 0xFDE68A, LV_OPA_COVER);
    } else if (wmoCode == 1 || wmoCode == 2) {
        createCirclePart(_weatherIconBox, 3, 1, 24, 0xF59E0B, LV_OPA_40);
        createCirclePart(_weatherIconBox, 6, 4, 18, 0xFBBF24, LV_OPA_COVER);
        drawModernCloud(_weatherIconBox, 10, 10, 0xE2E8F0);
    } else if (wmoCode == 3) {
        drawModernCloud(_weatherIconBox, 2,  4,  0x64748B);
        drawModernCloud(_weatherIconBox, 10, 10, 0xE2E8F0);
    } else if (wmoCode == 45 || wmoCode == 48) {
        drawModernCloud(_weatherIconBox, 7, 2, 0x94A3B8);
        createPillPart(_weatherIconBox, 6,  27, 38, 3, 0xCBD5E1);
        createPillPart(_weatherIconBox, 10, 32, 30, 3, 0x94A3B8);
    } else if ((wmoCode >= 51 && wmoCode <= 67) || (wmoCode >= 80 && wmoCode <= 82)) {
        drawModernCloud(_weatherIconBox, 7, 1, 0xCBD5E1);
        createPillPart(_weatherIconBox, 13, 27, 4, 8, 0x00D4FF);
        createPillPart(_weatherIconBox, 23, 29, 4, 8, 0x00D4FF);
        createPillPart(_weatherIconBox, 33, 27, 4, 8, 0x00D4FF);
    } else if ((wmoCode >= 71 && wmoCode <= 77) || wmoCode == 85 || wmoCode == 86) {
        drawModernCloud(_weatherIconBox, 7, 1, 0xE2E8F0);
        createCirclePart(_weatherIconBox, 13, 28, 5, 0xE0F2FE);
        createCirclePart(_weatherIconBox, 23, 30, 5, 0x7DD3FC);
        createCirclePart(_weatherIconBox, 33, 28, 5, 0xE0F2FE);
    } else if (wmoCode >= 95) {
        drawModernCloud(_weatherIconBox, 7, 1, 0x64748B);
        createPillPart(_weatherIconBox, 22, 23, 5, 9, 0xFACC15);
        createPillPart(_weatherIconBox, 19, 29, 8, 4, 0xFACC15);
        createPillPart(_weatherIconBox, 21, 31, 4, 7, 0xF59E0B);
        createPillPart(_weatherIconBox, 12, 28, 3, 7, 0x38BDF8);
        createPillPart(_weatherIconBox, 34, 28, 3, 7, 0x38BDF8);
    } else {
        createCirclePart(_weatherIconBox, 6, 4, 18, 0xFBBF24, LV_OPA_COVER);
        drawModernCloud(_weatherIconBox, 10, 10, 0xE2E8F0);
    }
}

void GuiManager::buildUi() {
    if (!g_has_display) return;

    lv_obj_t* scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x121212), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    _screenFrame = lv_obj_create(scr);
    lv_obj_set_size(_screenFrame, 320, 240);
    lv_obj_set_pos(_screenFrame, 0, 0);
    lv_obj_set_style_bg_color(_screenFrame, lv_color_hex(0x121212), 0);
    lv_obj_set_style_bg_opa(_screenFrame, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(_screenFrame, 0, 0);
    lv_obj_set_style_pad_all(_screenFrame, 0, 0);
    lv_obj_clear_flag(_screenFrame, LV_OBJ_FLAG_SCROLLABLE);

    // 1. HEADER SUPERIORE (Status Bar)
    lv_obj_t* header = lv_obj_create(_screenFrame);
    lv_obj_set_size(header, 308, 28);
    lv_obj_set_pos(header, 6, 6);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x1C2430), 0);
    lv_obj_set_style_border_color(header, lv_color_hex(0x2A3B4C), 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_radius(header, 6, 0);
    lv_obj_set_style_pad_hor(header, 8, 0);
    lv_obj_set_style_pad_ver(header, 2, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    _lblDate = lv_label_create(header);
    lv_obj_set_style_text_font(_lblDate, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lblDate, lv_color_hex(0xE2E8F0), 0);
    lv_label_set_text(_lblDate, "Sincronizzazione...");
    lv_obj_align(_lblDate, LV_ALIGN_LEFT_MID, 0, 0);

    _lblWifiStatus = lv_label_create(header);
    lv_obj_set_style_text_font(_lblWifiStatus, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lblWifiStatus, lv_color_hex(0x38BDF8), 0);
    lv_label_set_text(_lblWifiStatus, LV_SYMBOL_WIFI " --");
    lv_obj_align(_lblWifiStatus, LV_ALIGN_RIGHT_MID, 0, 0);

    _lblNtpStatus = lv_label_create(header);
    lv_obj_set_style_text_font(_lblNtpStatus, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lblNtpStatus, lv_color_hex(0xF59E0B), 0);
    lv_label_set_text(_lblNtpStatus, LV_SYMBOL_REFRESH " NTP");
    lv_obj_align(_lblNtpStatus, LV_ALIGN_RIGHT_MID, -68, 0);

    // 2. QUADRANTE SINISTRO: OROLOGIO GRANDE & BOX COMPATTO
    lv_obj_t* leftQuad = lv_obj_create(_screenFrame);
    lv_obj_set_size(leftQuad, 172, 144);
    lv_obj_set_pos(leftQuad, 6, 38);
    lv_obj_set_style_bg_color(leftQuad, lv_color_hex(0x18202C), 0);
    lv_obj_set_style_border_color(leftQuad, lv_color_hex(0x2A3B4C), 0);
    lv_obj_set_style_border_width(leftQuad, 1, 0);
    lv_obj_set_style_radius(leftQuad, 8, 0);
    lv_obj_set_style_pad_all(leftQuad, 6, 0);
    lv_obj_clear_flag(leftQuad, LV_OBJ_FLAG_SCROLLABLE);

    _lblClockHM = lv_label_create(leftQuad);
    lv_obj_set_style_text_font(_lblClockHM, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(_lblClockHM, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(_lblClockHM, "--:--");
    lv_obj_set_pos(_lblClockHM, 2, 16);

    _lblClockSec = lv_label_create(leftQuad);
    lv_obj_set_style_text_font(_lblClockSec, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_lblClockSec, lv_color_hex(0x38BDF8), 0);
    lv_label_set_text(_lblClockSec, ":--");
    lv_obj_align(_lblClockSec, LV_ALIGN_TOP_RIGHT, -2, 39);

    _eventBox = lv_obj_create(leftQuad);
    lv_obj_set_size(_eventBox, 158, 46);
    lv_obj_align(_eventBox, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(_eventBox, 6, 0);
    lv_obj_set_style_pad_all(_eventBox, 3, 0);
    lv_obj_clear_flag(_eventBox, LV_OBJ_FLAG_SCROLLABLE);

    _lblEventTitle = lv_label_create(_eventBox);
    lv_obj_set_width(_lblEventTitle, 150);
    lv_label_set_long_mode(_lblEventTitle, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(_lblEventTitle, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(_lblEventTitle, &lv_font_montserrat_12, 0);
    lv_label_set_text(_lblEventTitle, "Prossimo suono");
    lv_obj_align(_lblEventTitle, LV_ALIGN_TOP_MID, 0, 2);

    _lblEventSub = lv_label_create(_eventBox);
    lv_obj_set_width(_lblEventSub, 150);
    lv_obj_set_style_text_align(_lblEventSub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(_lblEventSub, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_lblEventSub, lv_color_hex(0xFBBF24), 0);
    lv_label_set_text(_lblEventSub, "--m --s");
    lv_obj_align(_lblEventSub, LV_ALIGN_BOTTOM_MID, 0, -1);

    // 3. QUADRANTE DESTRO: ICONA METEO MODERNA & DATI
    lv_obj_t* rightQuad = lv_obj_create(_screenFrame);
    lv_obj_set_size(rightQuad, 132, 144);
    lv_obj_set_pos(rightQuad, 182, 38);
    lv_obj_set_style_bg_color(rightQuad, lv_color_hex(0x18202C), 0);
    lv_obj_set_style_border_color(rightQuad, lv_color_hex(0x2A3B4C), 0);
    lv_obj_set_style_border_width(rightQuad, 1, 0);
    lv_obj_set_style_radius(rightQuad, 8, 0);
    lv_obj_set_style_pad_all(rightQuad, 6, 0);
    lv_obj_clear_flag(rightQuad, LV_OBJ_FLAG_SCROLLABLE);

    _weatherIconBox = lv_obj_create(rightQuad);
    lv_obj_remove_style_all(_weatherIconBox);
    lv_obj_set_size(_weatherIconBox, 52, 38);
    lv_obj_align(_weatherIconBox, LV_ALIGN_TOP_MID, 0, 2);
    lv_obj_clear_flag(_weatherIconBox, LV_OBJ_FLAG_SCROLLABLE);
    renderModernWeatherIcon(-1);

    _lblWeatherTemp = lv_label_create(rightQuad);
    lv_obj_set_style_text_font(_lblWeatherTemp, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(_lblWeatherTemp, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(_lblWeatherTemp, "--.- \xC2\xB0" "C");
    lv_obj_align(_lblWeatherTemp, LV_ALIGN_TOP_MID, 0, 48);

    _lblWeatherHum = lv_label_create(rightQuad);
    lv_obj_set_style_text_font(_lblWeatherHum, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lblWeatherHum, lv_color_hex(0x94A3B8), 0);
    lv_label_set_text(_lblWeatherHum, "Umidita: --%");
    lv_obj_align(_lblWeatherHum, LV_ALIGN_BOTTOM_MID, 0, -20);

    _lblWeatherWind = lv_label_create(rightQuad);
    lv_obj_set_style_text_font(_lblWeatherWind, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lblWeatherWind, lv_color_hex(0x94A3B8), 0);
    lv_label_set_text(_lblWeatherWind, "Vento: -- km/h");
    lv_obj_align(_lblWeatherWind, LV_ALIGN_BOTTOM_MID, 0, -2);

    // 4. FOOTER INFERIORE (Touch Bar)
    _btnRingNow = lv_btn_create(_screenFrame);
    lv_obj_set_size(_btnRingNow, 118, 44);
    lv_obj_set_pos(_btnRingNow, 6, 188);
    lv_obj_set_style_bg_color(_btnRingNow, lv_color_hex(0x0284C7), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(_btnRingNow, lv_color_hex(0xF59E0B), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(_btnRingNow, 8, 0);
    lv_obj_set_style_shadow_width(_btnRingNow, 0, 0);
    lv_obj_add_event_cb(_btnRingNow, GuiManager::onRingNowClicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* lblRing = lv_label_create(_btnRingNow);
    lv_obj_set_style_text_font(lblRing, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lblRing, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(lblRing, LV_SYMBOL_BELL " SUONA ORA");
    lv_obj_center(lblRing);

    _btnSuspend = lv_btn_create(_screenFrame);
    lv_obj_set_size(_btnSuspend, 122, 44);
    lv_obj_set_pos(_btnSuspend, 128, 188);
    lv_obj_set_style_radius(_btnSuspend, 8, 0);
    lv_obj_set_style_shadow_width(_btnSuspend, 0, 0);
    lv_obj_add_event_cb(_btnSuspend, GuiManager::onSuspendToggleClicked, LV_EVENT_CLICKED, nullptr);

    _lblBtnSuspend = lv_label_create(_btnSuspend);
    lv_obj_set_style_text_font(_lblBtnSuspend, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lblBtnSuspend, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(_lblBtnSuspend);

    _relayIndicatorBox = lv_obj_create(_screenFrame);
    lv_obj_set_size(_relayIndicatorBox, 60, 44);
    lv_obj_set_pos(_relayIndicatorBox, 254, 188);
    lv_obj_set_style_bg_color(_relayIndicatorBox, lv_color_hex(0x18202C), 0);
    lv_obj_set_style_border_color(_relayIndicatorBox, lv_color_hex(0x2A3B4C), 0);
    lv_obj_set_style_border_width(_relayIndicatorBox, 1, 0);
    lv_obj_set_style_radius(_relayIndicatorBox, 8, 0);
    lv_obj_set_style_pad_all(_relayIndicatorBox, 2, 0);
    lv_obj_clear_flag(_relayIndicatorBox, LV_OBJ_FLAG_SCROLLABLE);

    _relayLedDot = lv_obj_create(_relayIndicatorBox);
    lv_obj_set_size(_relayLedDot, 12, 12);
    lv_obj_set_style_radius(_relayLedDot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(_relayLedDot, lv_color_hex(0x10B981), 0);
    lv_obj_set_style_border_width(_relayLedDot, 0, 0);
    lv_obj_align(_relayLedDot, LV_ALIGN_TOP_MID, 0, 4);

    _lblRelayState = lv_label_create(_relayIndicatorBox);
    lv_obj_set_style_text_font(_lblRelayState, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(_lblRelayState, lv_color_hex(0x94A3B8), 0);
    lv_label_set_text(_lblRelayState, "RIPOSO");
    lv_obj_align(_lblRelayState, LV_ALIGN_BOTTOM_MID, 0, -2);

    // 5. SCHERMATA DI STANDBY / SCREENSAVER (Overlay 310x230 dentro la cornice)
    _standbyOverlay = lv_obj_create(_screenFrame);
    lv_obj_set_size(_standbyOverlay, 310, 230);
    lv_obj_center(_standbyOverlay);
    lv_obj_set_style_bg_color(_standbyOverlay, lv_color_hex(0x0B0F17), 0);
    lv_obj_set_style_bg_opa(_standbyOverlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_standbyOverlay, 0, 0);
    lv_obj_set_style_radius(_standbyOverlay, 0, 0);
    lv_obj_set_style_pad_all(_standbyOverlay, 4, 0);
    lv_obj_clear_flag(_standbyOverlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_standbyOverlay, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(_standbyOverlay, GuiManager::onStandbyTouched, LV_EVENT_PRESSED, nullptr);

    _imgStandbyLogo = lv_img_create(_standbyOverlay);
    lv_obj_add_flag(_imgStandbyLogo, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_imgStandbyLogo, LV_OBJ_FLAG_CLICKABLE);

    _lblStandbyClock = lv_label_create(_standbyOverlay);
    lv_obj_set_style_text_font(_lblStandbyClock, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(_lblStandbyClock, lv_color_hex(0xF8FAFC), 0);
    lv_label_set_text(_lblStandbyClock, "--:--:--");
    lv_obj_clear_flag(_lblStandbyClock, LV_OBJ_FLAG_CLICKABLE);

    _lblStandbySchool = lv_label_create(_standbyOverlay);
    lv_obj_set_width(_lblStandbySchool, 296);
    lv_label_set_long_mode(_lblStandbySchool, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(_lblStandbySchool, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(_lblStandbySchool, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_lblStandbySchool, lv_color_hex(0x38BDF8), 0);
    lv_label_set_text(_lblStandbySchool, "");
    lv_obj_add_flag(_lblStandbySchool, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_lblStandbySchool, LV_OBJ_FLAG_CLICKABLE);

    _lblStandbyDate = lv_label_create(_standbyOverlay);
    lv_obj_set_style_text_font(_lblStandbyDate, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lblStandbyDate, lv_color_hex(0x64748B), 0);
    lv_label_set_text(_lblStandbyDate, "");
    lv_obj_clear_flag(_lblStandbyDate, LV_OBJ_FLAG_CLICKABLE);
}

void GuiManager::applySuspendedVisuals(bool isSuspended) {
    updateOnboardRgbLed(isSuspended);
    if (!g_has_display || !_screenFrame) return;

    if (!isSuspended) {
        lv_obj_set_style_border_color(_screenFrame, lv_color_hex(0x2A3B4C), 0);
        lv_obj_set_style_border_width(_screenFrame, 2, 0);

        lv_obj_set_style_bg_color(_eventBox, lv_color_hex(0x111822), 0);
        lv_obj_set_style_border_color(_eventBox, lv_color_hex(0x2A3B4C), 0);
        lv_obj_set_style_border_width(_eventBox, 1, 0);
        lv_obj_set_style_text_color(_lblEventTitle, lv_color_hex(0x94A3B8), 0);
        lv_obj_align(_lblEventTitle, LV_ALIGN_TOP_MID, 0, 2);
        lv_obj_clear_flag(_lblEventSub, LV_OBJ_FLAG_HIDDEN);

        lv_obj_set_style_bg_color(_btnSuspend, lv_color_hex(0xE65100), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_label_set_text(_lblBtnSuspend, LV_SYMBOL_PAUSE " SOSPENDI");
    } else {
        lv_obj_set_style_border_color(_screenFrame, lv_color_hex(0xD32F2F), 0);
        lv_obj_set_style_border_width(_screenFrame, 5, 0);

        lv_obj_set_style_bg_color(_eventBox, lv_color_hex(0xD32F2F), 0);
        lv_obj_set_style_border_color(_eventBox, lv_color_hex(0xFFCDD2), 0);
        lv_obj_set_style_border_width(_eventBox, 1, 0);
        lv_obj_set_style_text_color(_lblEventTitle, lv_color_hex(0xFFFFFF), 0);
        lv_label_set_text(_lblEventTitle, LV_SYMBOL_MUTE " SUONI SOSPESI");
        lv_obj_align(_lblEventTitle, LV_ALIGN_CENTER, 0, 0);
        lv_obj_add_flag(_lblEventSub, LV_OBJ_FLAG_HIDDEN);

        lv_obj_set_style_bg_color(_btnSuspend, lv_color_hex(0x2E7D32), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_label_set_text(_lblBtnSuspend, LV_SYMBOL_PLAY " RIATTIVA");
    }
}

String GuiManager::getItalianHeaderDate() {
    struct tm timeinfo;
    if (!NetworkManager::getCurrentLocalTime(timeinfo)) {
        return "Attesa Sincronizzazione...";
    }

    static const char* giorni[] = {
        "Domenica", "Lunedi", "Martedi", "Mercoledi", "Giovedi", "Venerdi", "Sabato"
    };
    static const char* mesi[] = {
        "Gen", "Feb", "Mar", "Apr", "Mag", "Giu",
        "Lug", "Ago", "Set", "Ott", "Nov", "Dic"
    };

    int w = (timeinfo.tm_wday >= 0 && timeinfo.tm_wday <= 6) ? timeinfo.tm_wday : 0;
    int m = (timeinfo.tm_mon >= 0 && timeinfo.tm_mon <= 11) ? timeinfo.tm_mon : 0;

    char buf[36];
    snprintf(buf, sizeof(buf), "%s, %02d %s", giorni[w], timeinfo.tm_mday, mesi[m]);
    return String(buf);
}

void GuiManager::refreshDynamicData() {
    if (!g_has_display || !_screenFrame) return;

    if (_brandingDirty) {
        _brandingDirty = false;
        loadSchoolLogoFromFs();
        updateStandbyBrandingLayout();
    }

    bool currentCfgSuspended = ConfigManager::isSuspended();
    if (_stateDirty || (currentCfgSuspended != _lastAppliedSuspended)) {
        bool newState = _stateDirty ? _targetSuspendedState : currentCfgSuspended;
        _stateDirty = false;
        _lastAppliedSuspended = newState;
        applySuspendedVisuals(newState);
    }

    uint32_t inactiveMs = lv_disp_get_inactive_time(nullptr);
    if (!_inStandby && inactiveMs >= STANDBY_TIMEOUT_MS && !RelayController::isActive()) {
        enterStandby();
    }

    if (_inStandby && RelayController::isActive()) {
        exitStandby();
    }

    String fullTimeStr = NetworkManager::getFormattedTime();
    String dateStr     = getItalianHeaderDate();

    lv_label_set_text(_lblStandbyClock, fullTimeStr.c_str());
    lv_label_set_text(_lblStandbyDate,  dateStr.c_str());

    bool relayActive = RelayController::isActive();
    if (_ringingDirty || (relayActive != _lastRelayActive)) {
        _ringingDirty = false;
        _lastRelayActive = relayActive;
        if (relayActive) {
            lv_obj_set_style_bg_color(_relayLedDot, lv_color_hex(0xF59E0B), 0);
            lv_obj_set_style_border_color(_relayIndicatorBox, lv_color_hex(0xF59E0B), 0);
            lv_obj_set_style_text_color(_lblRelayState, lv_color_hex(0xFBBF24), 0);
            lv_label_set_text(_lblRelayState, "ATTIVO");
        } else {
            lv_obj_set_style_bg_color(_relayLedDot, lv_color_hex(0x10B981), 0);
            lv_obj_set_style_border_color(_relayIndicatorBox, lv_color_hex(0x2A3B4C), 0);
            lv_obj_set_style_text_color(_lblRelayState, lv_color_hex(0x94A3B8), 0);
            lv_label_set_text(_lblRelayState, "RIPOSO");
        }
    }

    lv_label_set_text(_lblClockHM,  NetworkManager::getFormattedHoursMinutes().c_str());
    lv_label_set_text(_lblClockSec, NetworkManager::getFormattedSeconds().c_str());
    lv_label_set_text(_lblDate, dateStr.c_str());

    if (NetworkManager::isTimeSynced()) {
        lv_obj_set_style_text_color(_lblNtpStatus, lv_color_hex(0x10B981), 0);
        lv_label_set_text(_lblNtpStatus, LV_SYMBOL_OK " NTP");
    } else {
        lv_obj_set_style_text_color(_lblNtpStatus, lv_color_hex(0xF59E0B), 0);
        lv_label_set_text(_lblNtpStatus, LV_SYMBOL_REFRESH " NTP");
    }

    if (NetworkManager::isStaConnected()) {
        lv_obj_set_style_text_color(_lblWifiStatus, lv_color_hex(0x38BDF8), 0);
        char wifiBuf[20];
        snprintf(wifiBuf, sizeof(wifiBuf), "%s %lddB", LV_SYMBOL_WIFI, (long)NetworkManager::getRssi());
        lv_label_set_text(_lblWifiStatus, wifiBuf);
    } else if (NetworkManager::isApActive()) {
        lv_obj_set_style_text_color(_lblWifiStatus, lv_color_hex(0xF59E0B), 0);
        lv_label_set_text(_lblWifiStatus, LV_SYMBOL_WIFI " AP-Setup");
    } else {
        lv_obj_set_style_text_color(_lblWifiStatus, lv_color_hex(0xEF4444), 0);
        lv_label_set_text(_lblWifiStatus, LV_SYMBOL_WARNING " NoNet");
    }

    if (!_lastAppliedSuspended) {
        String evLabel, evCountdown;
        Scheduler::getNextBellDetails(evLabel, evCountdown);
        lv_label_set_text(_lblEventTitle, evLabel.c_str());
        lv_label_set_text(_lblEventSub, evCountdown.c_str());
    }

    WeatherData wd = WeatherService::getWeatherData();
    if (wd.valid) {
        if (wd.weatherCode != _lastRenderedWeatherCode) {
            _lastRenderedWeatherCode = wd.weatherCode;
            renderModernWeatherIcon(wd.weatherCode);
        }

        char tBuf[20];
        snprintf(tBuf, sizeof(tBuf), "%.1f \xC2\xB0" "C", wd.temperatureC);
        lv_label_set_text(_lblWeatherTemp, tBuf);

        char hBuf[24];
        snprintf(hBuf, sizeof(hBuf), "Umidita: %d%%", wd.humidityPct);
        lv_label_set_text(_lblWeatherHum, hBuf);

        char wBuf[28];
        snprintf(wBuf, sizeof(wBuf), "Vento: %.1f km/h", wd.windSpeedKmh);
        lv_label_set_text(_lblWeatherWind, wBuf);
    }
}

void GuiManager::onRingNowClicked(lv_event_t* e) {
    (void)e;
    if (!g_has_display) return;
    uint32_t pulseMs = ConfigManager::getRelayPulseMs();
    Serial.printf("[GUI] Pressione touch [SUONA ORA] -> Attivazione relè per %u ms\n", pulseMs);
    RelayController::trigger(pulseMs);
    refreshDynamicData();
}

void GuiManager::onSuspendToggleClicked(lv_event_t* e) {
    (void)e;
    if (!g_has_display) return;
    bool newSusp = !ConfigManager::isSuspended();
    Serial.printf("[GUI] Pressione touch [SOSPENDI/RIATTIVA] -> Nuovo stato suspended=%s\n",
                  newSusp ? "true" : "false");
    ConfigManager::setSuspended(newSusp);
    gui_update_state(newSusp);
    refreshDynamicData();
}

void GuiManager::guiTask(void* pvParameters) {
    (void)pvParameters;
    uint32_t lastRefreshMs = 0;

    for (;;) {
        if (!g_has_display) {
            vTaskDelete(nullptr);
            return;
        }

        uint32_t now = millis();
        if (_stateDirty || _brandingDirty || _ringingDirty || (RelayController::isActive() != _lastRelayActive) || (now - lastRefreshMs >= 250)) {
            lastRefreshMs = now;
            refreshDynamicData();
        }

        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
