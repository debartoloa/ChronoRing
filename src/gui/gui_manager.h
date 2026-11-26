#pragma once

#include <Arduino.h>
#if !defined(CHRONO_HEADLESS_BUILD)
#include <lvgl.h>
#endif

/**
 * @file gui_manager.h
 * @brief Gestione completa dell'interfaccia Touch LVGL v8 (320x240), Icone Meteo Moderne,
 *        Standby (60s) con Logo/Nome Scuola e supporto sicuro per modalità Headless (g_has_display).
 */

/**
 * @brief Aggiorna in modo thread-safe lo stato di sospensione (protetto da g_has_display).
 */
void gui_update_state(bool is_suspended);

/**
 * @brief Notifica in modo thread-safe lo stato di eccitazione del relè campanella (protetto da g_has_display).
 */
void gui_set_bell_ringing(bool is_ringing);

class GuiManager {
public:
    static constexpr uint32_t STANDBY_TIMEOUT_MS = 60000; // 1 minuto di inattività touch (default)
    static constexpr uint8_t  BRIGHTNESS_ACTIVE  = 255;   // 100% luminosità attiva
    static constexpr uint8_t  BRIGHTNESS_STANDBY = 45;    // ~18% luminosità ridotta in standby
    static constexpr uint8_t  BRIGHTNESS_OFF     = 0;     // 0% retroilluminazione spenta

    /**
     * @brief Esegue l'autorilevamento I2C del display; se presente avvia LVGL e il task su Core 1,
     *        altrimenti lascia il sistema in modalità Headless senza allocare memoria grafica.
     */
    static bool begin();

    static void requestStateUpdate(bool isSuspended);
    static void requestBellRingingUpdate(bool isRinging);
    static void requestBrandingReload();
    static void requestDisplaySettingsReload();
    static void notifyIpAcquired();

#if !defined(CHRONO_HEADLESS_BUILD)
private:
    static TaskHandle_t _guiTaskHandle;
    static volatile bool _stateDirty;
    static volatile bool _targetSuspendedState;
    static volatile bool _brandingDirty;
    static volatile bool _ringingDirty;
    static volatile bool _displaySettingsDirty;
    static bool _lastAppliedSuspended;
    static bool _lastRelayActive;
    static int  _lastRenderedWeatherCode;
    static bool _inStandby;
    static uint32_t _displayTimeoutMs;
    static bool _displaySleepOff;
    static uint32_t _lastTextRefreshMs;
    static uint32_t _showIpUntilMs;
    static volatile bool _headerIpDirty;

    // Buffer PSRAM/SRAM per il Logo Scuola RGB565
    static uint8_t*     _logoPixelBuf;
    static lv_img_dsc_t _logoImgDsc;
    static bool         _logoValid;

    // Oggetti LVGL principali (Dashboard)
    static lv_obj_t* _screenFrame;
    static lv_obj_t* _lblDate;
    static lv_obj_t* _lblNtpStatus;
    static lv_obj_t* _lblWifiStatus;

    // Quadrante Sinistro (Orologio Grande HH:MM + :SS & Box Evento Compatto)
    static lv_obj_t* _lblClockHM;
    static lv_obj_t* _lblClockSec;
    static lv_obj_t* _eventBox;
    static lv_obj_t* _lblEventTitle;
    static lv_obj_t* _lblEventSub;

    // Quadrante Destro (Icona Meteo Vettoriale Moderna & Dati Open-Meteo)
    static lv_obj_t* _weatherIconBox;
    static lv_obj_t* _lblWeatherTemp;
    static lv_obj_t* _lblWeatherHum;
    static lv_obj_t* _lblWeatherWind;

    // Footer Inferiore (Pulsanti Touch & Spia Relè)
    static lv_obj_t* _btnRingNow;
    static lv_obj_t* _btnSuspend;
    static lv_obj_t* _lblBtnSuspend;
    static lv_obj_t* _btnReset;
    static lv_obj_t* _relayIndicatorBox;
    static lv_obj_t* _relayLedDot;
    static lv_obj_t* _lblRelayState;

    // Modale Ripristino di Fabbrica protetta da Password / Super Password
    static lv_obj_t* _resetModal;
    static lv_obj_t* _taResetPwd;
    static lv_obj_t* _lblResetMsg;
    static lv_obj_t* _btnResetEye;
    static lv_obj_t* _kbReset;
    static uint8_t   _resetFailedAttempts;
    static uint32_t  _resetLockUntilMs;

    // Schermata di Standby (Dopo 60s di inattività)
    static lv_obj_t* _standbyOverlay;
    static lv_obj_t* _imgStandbyLogo;
    static lv_obj_t* _lblStandbyClock;
    static lv_obj_t* _lblStandbySchool;
    static lv_obj_t* _lblStandbyDate;

    static void buildUi();
    static void loadSchoolLogoFromFs();
    static void updateStandbyBrandingLayout();
    static void applyStandbyModeVisuals();
    static void enterStandby();
    static void exitStandby();
    static void renderModernWeatherIcon(int wmoCode);
    static void applySuspendedVisuals(bool isSuspended);
    static void updateOnboardRgbLed(bool isSuspended);
    static void refreshDynamicData();
    static String getItalianHeaderDate();

    static void onRingNowClicked(lv_event_t* e);
    static void onSuspendToggleClicked(lv_event_t* e);
    static void onStandbyTouched(lv_event_t* e);
    static void onHeaderClicked(lv_event_t* e);
    static void openResetModal();
    static void closeResetModal();
    static void onResetBtnClicked(lv_event_t* e);
    static void onResetCancelClicked(lv_event_t* e);
    static void onResetEyeClicked(lv_event_t* e);
    static void onResetKeyboardEvent(lv_event_t* e);
    static void guiTask(void* pvParameters);
#endif
};
