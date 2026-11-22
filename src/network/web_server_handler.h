#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

/**
 * @file web_server_handler.h
 * @brief Web Server locale ChronoRing (Porta 80), Autenticazione Cookie, API REST e Aggiornamento OTA (/update)
 */

class WebServerHandler {
public:
    enum class AuthRole : uint8_t {
        NONE = 0,
        ADMIN = 1,
        SUPERADMIN = 2
    };

    static constexpr uint8_t  MAX_FAILED_LOGIN_ATTEMPTS = 5;
    static constexpr uint32_t LOGIN_LOCKOUT_MS          = 60000; // 60s blocco anti brute-force

    /**
     * @brief Inizializza le rotte HTTP, le API REST, l'autenticazione e il gestore OTA sulla porta 80.
     */
    static void begin();

private:
    static AsyncWebServer _server;
    static char _sessionToken[40];
    static AuthRole _sessionRole;
    static uint8_t  _failedLoginAttempts;
    static uint32_t _loginLockUntilMs;
    static bool _otaInProgress;
    static bool _otaSuccess;
    static String _otaError;

    static void generateNewSessionToken();
    static AuthRole getAuthRole(AsyncWebServerRequest* request);
    static bool isAuthenticated(AsyncWebServerRequest* request);
    static bool isSuperAdmin(AsyncWebServerRequest* request);
    static void scheduleRestart(uint32_t delayMs = 1500);

    static void setupRoutes();
};
