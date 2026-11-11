#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

/**
 * @file web_server_handler.h
 * @brief Web Server locale ChronoRing (Porta 80), Autenticazione Cookie, API REST e Aggiornamento OTA (/update)
 */

class WebServerHandler {
public:
    /**
     * @brief Inizializza le rotte HTTP, le API REST, l'autenticazione e il gestore OTA sulla porta 80.
     */
    static void begin();

private:
    static AsyncWebServer _server;
    static char _sessionToken[40];
    static bool _otaInProgress;
    static bool _otaSuccess;
    static String _otaError;

    static void generateNewSessionToken();
    static bool isAuthenticated(AsyncWebServerRequest* request);
    static void scheduleRestart(uint32_t delayMs = 1500);

    static void setupRoutes();
};
