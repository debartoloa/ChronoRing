#include "web_server_handler.h"
#include "network_manager.h"
#include "../hardware_pins.h"
#include "../config_manager.h"
#include "../relay_controller.h"
#include "../scheduler/scheduler.h"
#include "../gui/gui_manager.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Update.h>
#include <esp_timer.h>
#include <string>

AsyncWebServer WebServerHandler::_server(80);
char WebServerHandler::_sessionToken[40] = {0};
WebServerHandler::AuthRole WebServerHandler::_sessionRole = WebServerHandler::AuthRole::NONE;
uint8_t WebServerHandler::_failedLoginAttempts = 0;
uint32_t WebServerHandler::_loginLockUntilMs = 0;
bool WebServerHandler::_otaInProgress = false;
bool WebServerHandler::_otaSuccess = false;
String WebServerHandler::_otaError = "";

constexpr uint8_t  WebServerHandler::MAX_FAILED_LOGIN_ATTEMPTS;
constexpr uint32_t WebServerHandler::LOGIN_LOCKOUT_MS;

// ============================================================================
// PAGINE HTML EMBEDDED IN FLASH (PROGMEM) - TEMA "CHRONORING"
// ============================================================================

static const char LOGIN_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="it">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ChronoRing - Login</title>
<style>
  :root{--bg:#060d1f;--card:#0d1b36;--cyan:#00d4ff;--gold:#f59e0b;--text:#e2e8f0;--muted:#94a3b8;--danger:#ef4444;}
  *{box-sizing:border-box;margin:0;padding:0;font-family:'Segoe UI',system-ui,sans-serif;}
  body{background:radial-gradient(circle at center,#0f2447 0%,#050b18 100%);color:var(--text);min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px;}
  .login-card{background:var(--card);border:1px solid rgba(0,212,255,0.28);border-radius:18px;padding:34px 28px;width:100%;max-width:400px;box-shadow:0 0 35px rgba(0,212,255,0.15);text-align:center;}
  .logo-wrap{width:96px;height:96px;margin:0 auto 16px;position:relative;}
  h1{font-size:1.65rem;letter-spacing:2px;text-transform:uppercase;background:linear-gradient(90deg,#7dd3fc,#00d4ff,#fbbf24);-webkit-background-clip:text;-webkit-text-fill-color:transparent;margin-bottom:6px;}
  p.sub{color:var(--muted);font-size:0.88rem;margin-bottom:24px;}
  .input-group{text-align:left;margin-bottom:18px;}
  label{display:block;font-size:0.82rem;color:var(--cyan);margin-bottom:6px;font-weight:600;letter-spacing:0.5px;}
  input{width:100%;padding:12px 14px;border-radius:10px;border:1px solid rgba(0,212,255,0.3);background:#071022;color:#fff;font-size:1rem;outline:none;transition:0.2s;}
  input:focus{border-color:var(--cyan);box-shadow:0 0 12px rgba(0,212,255,0.3);}
  button{width:100%;padding:12px;border:none;border-radius:10px;background:linear-gradient(135deg,#0284c7,#00d4ff);color:#041022;font-weight:700;font-size:1rem;cursor:pointer;transition:0.2s;}
  button:hover{filter:brightness(1.12);transform:translateY(-1px);}
  .err{color:var(--danger);font-size:0.86rem;margin-top:12px;min-height:20px;}
</style>
</head>
<body>
<div class="login-card">
  <div class="logo-wrap">
    <svg viewBox="0 0 120 120" fill="none" xmlns="http://www.w3.org/2000/svg">
      <circle cx="60" cy="60" r="54" stroke="#00d4ff" stroke-width="3" stroke-dasharray="10 5"/>
      <circle cx="60" cy="60" r="44" stroke="#38bdf8" stroke-width="1.5" opacity="0.6"/>
      <path d="M60 26C47 26 39 36 39 50V68L31 76V80H89V76L81 68V50C81 36 73 26 60 26Z" fill="url(#bellGrad)" stroke="#fbbf24" stroke-width="2"/>
      <circle cx="60" cy="87" r="6" fill="#f59e0b"/>
      <defs>
        <linearGradient id="bellGrad" x1="35" y1="26" x2="85" y2="80" gradientUnits="userSpaceOnUse">
          <stop stop-color="#fde68a"/><stop offset="0.5" stop-color="#d97706"/><stop offset="1" stop-color="#78350f"/>
        </linearGradient>
      </defs>
    </svg>
  </div>
  <h1>ChronoRing</h1>
  <p class="sub">Sistema Automazione Campanella Scolastica &bull; <span style="color:var(--cyan);font-weight:700;">v)rawliteral" CHRONORING_FW_VERSION R"rawliteral(</span></p>
  <form id="loginForm">
    <div class="input-group">
      <label for="pwd">PASSWORD AMMINISTRATORE</label>
      <input type="password" id="pwd" placeholder="Inserisci password..." required autofocus>
    </div>
    <button type="submit">ACCEDI AL PANNELLO</button>
    <div class="err" id="errMsg"></div>
  </form>
</div>
<script>
document.getElementById('loginForm').addEventListener('submit', async (e) => {
  e.preventDefault();
  const err = document.getElementById('errMsg');
  err.textContent = '';
  try {
    const res = await fetch('/api/login', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({password: document.getElementById('pwd').value})
    });
    const d = await res.json().catch(() => ({}));
    if (res.ok) {
      window.location.href = d.needs_super_setup ? '/setup-super' : '/';
    } else {
      err.textContent = d.error || 'Password errata. Riprova.';
    }
  } catch (ex) {
    err.textContent = 'Errore di connessione al dispositivo.';
  }
});
</script>
</body>
</html>
)rawliteral";

static const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="it">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ChronoRing - Dashboard</title>
<style>
  :root{--bg:#060d1f;--card:#0d1b36;--card2:#112344;--cyan:#00d4ff;--gold:#f59e0b;--green:#10b981;--danger:#ef4444;--text:#e2e8f0;--muted:#94a3b8;}
  *{box-sizing:border-box;margin:0;padding:0;font-family:'Segoe UI',system-ui,sans-serif;}
  body{background:#060d1f;color:var(--text);padding-bottom:40px;}
  header{background:linear-gradient(90deg,#08142c,#0d2247,#08142c);border-bottom:1px solid rgba(0,212,255,0.3);padding:14px 22px;display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:12px;}
  .brand{display:flex;align-items:center;gap:12px;}
  .brand svg{width:42px;height:42px;}
  .brand h1{font-size:1.35rem;letter-spacing:1.5px;text-transform:uppercase;color:var(--cyan);}
  .brand #netInfo{font-size:0.78rem;color:var(--muted);display:block;}
  .nav-btns{display:flex;gap:10px;align-items:center;}
  .btn{padding:9px 15px;border-radius:9px;border:none;font-weight:600;font-size:0.88rem;cursor:pointer;transition:0.2s;text-decoration:none;display:inline-flex;align-items:center;gap:6px;}
  .btn-cyan{background:linear-gradient(135deg,#0284c7,#00d4ff);color:#041022;}
  .btn-gold{background:linear-gradient(135deg,#d97706,#fbbf24);color:#1e1103;font-weight:700;}
  .btn-danger{background:rgba(239,68,68,0.18);color:#fca5a5;border:1px solid rgba(239,68,68,0.4);}
  .btn-outline{background:rgba(0,212,255,0.1);color:var(--cyan);border:1px solid rgba(0,212,255,0.35);}
  .btn:hover{filter:brightness(1.12);}
  .container{max-width:1240px;margin:22px auto;padding:0 16px;display:grid;grid-template-columns:repeat(auto-fit,minmax(310px,1fr));gap:18px;}
  .card{background:var(--card);border:1px solid rgba(0,212,255,0.22);border-radius:14px;padding:20px;box-shadow:0 8px 25px rgba(0,0,0,0.35);}
  .card-full{grid-column:1 / -1;}
  .card h2{font-size:1.02rem;color:var(--cyan);margin-bottom:14px;display:flex;align-items:center;justify-content:space-between;border-bottom:1px solid rgba(255,255,255,0.07);padding-bottom:8px;text-transform:uppercase;letter-spacing:0.8px;}
  .clock-box{text-align:center;padding:10px 0;}
  .clock-time{font-size:2.9rem;font-weight:800;color:#fff;letter-spacing:2px;text-shadow:0 0 18px rgba(0,212,255,0.45);font-variant-numeric:tabular-nums;}
  .clock-date{color:var(--muted);font-size:0.95rem;margin-top:4px;}
  .badge{display:inline-block;padding:4px 10px;border-radius:99px;font-size:0.75rem;font-weight:700;}
  .badge-ok{background:rgba(16,185,129,0.2);color:#34d399;border:1px solid rgba(16,185,129,0.4);}
  .badge-warn{background:rgba(245,158,11,0.2);color:#fbbf24;border:1px solid rgba(245,158,11,0.4);}
  .badge-err{background:rgba(239,68,68,0.2);color:#f87171;border:1px solid rgba(239,68,68,0.4);}
  .stat-row{display:flex;justify-content:space-between;padding:8px 0;border-bottom:1px solid rgba(255,255,255,0.05);font-size:0.9rem;}
  .stat-row span:first-child{color:var(--muted);}
  .toggle-row{display:flex;align-items:center;justify-content:space-between;background:var(--card2);padding:14px;border-radius:10px;margin-bottom:10px;}
  .switch{position:relative;display:inline-block;width:48px;height:26px;}
  .switch input{opacity:0;width:0;height:0;}
  .slider{position:absolute;cursor:pointer;inset:0;background:#334155;transition:.25s;border-radius:26px;}
  .slider:before{position:absolute;content:"";height:20px;width:20px;left:3px;bottom:3px;background:white;transition:.25s;border-radius:50%;}
  input:checked + .slider{background:var(--cyan);}
  input:checked + .slider.danger-slider{background:var(--danger);}
  input:checked + .slider:before{transform:translateX(22px);}
  table{width:100%;border-collapse:collapse;margin-top:8px;}
  th,td{padding:10px 8px;text-align:left;border-bottom:1px solid rgba(255,255,255,0.07);font-size:0.92rem;vertical-align:middle;}
  th{color:var(--muted);font-size:0.8rem;text-transform:uppercase;}
  tbody tr{transition:background 0.2s,box-shadow 0.2s;}
  tbody tr.editing-row{background:rgba(0,212,255,0.13);box-shadow:inset 4px 0 0 var(--cyan);}
  tbody tr.new-row-flash{animation:rowFlash 0.9s ease;}
  @keyframes rowFlash{0%{background:rgba(0,212,255,0.34);}100%{background:rgba(0,212,255,0.13);}}
  input.input-missing{border-color:var(--gold)!important;box-shadow:0 0 0 2px rgba(245,158,11,0.3)!important;}
  .days-cell{min-width:230px;}
  .day-pills{display:flex;gap:4px;flex-wrap:wrap;margin-bottom:5px;}
  .day-pill{border:1px solid rgba(148,163,184,0.3);background:#071022;color:var(--muted);border-radius:6px;padding:3px 6px;font-size:0.74rem;font-weight:700;cursor:pointer;transition:0.15s;line-height:1.1;}
  .day-pill:hover{border-color:var(--cyan);color:#fff;}
  .day-pill.active{background:rgba(0,212,255,0.22);border-color:var(--cyan);color:#7dd3fc;}
  .day-pill.dom-pill.active{background:rgba(245,158,11,0.22);border-color:var(--gold);color:#fde68a;}
  .day-presets{display:flex;gap:4px;flex-wrap:wrap;}
  .preset-btn{border:1px solid rgba(255,255,255,0.12);background:rgba(255,255,255,0.04);color:var(--muted);border-radius:99px;padding:2px 7px;font-size:0.68rem;font-weight:600;cursor:pointer;transition:0.15s;}
  .preset-btn:hover{border-color:var(--cyan);color:#e2e8f0;}
  .preset-btn.active{background:rgba(16,185,129,0.22);border-color:var(--green);color:#34d399;}
  input[type="time"],input[type="number"],input[type="text"],input[type="password"]{background:#071022;border:1px solid rgba(0,212,255,0.3);color:#fff;padding:8px 10px;border-radius:8px;font-size:0.92rem;}
  .form-grid{display:grid;gap:12px;}
  .form-group label{display:block;font-size:0.8rem;color:var(--muted);margin-bottom:5px;}
  .form-group input{width:100%;}
  .logo-preview-wrap{display:flex;align-items:center;gap:14px;background:#071022;padding:12px;border-radius:10px;border:1px dashed rgba(0,212,255,0.3);}
  canvas#logoCanvas{background:#0B0F17;border:1px solid rgba(255,255,255,0.15);border-radius:8px;flex-shrink:0;}
  .modal-overlay{position:fixed;inset:0;background:rgba(4,10,24,0.82);backdrop-filter:blur(4px);display:none;align-items:center;justify-content:center;z-index:1000;padding:16px;}
  .modal-box{background:var(--card);border:1px solid var(--cyan);border-radius:16px;width:100%;max-width:440px;max-height:82vh;display:flex;flex-direction:column;box-shadow:0 15px 45px rgba(0,0,0,0.65);overflow:hidden;}
  .modal-header{padding:16px 20px;border-bottom:1px solid rgba(255,255,255,0.08);display:flex;align-items:center;justify-content:space-between;}
  .modal-header h3{color:var(--cyan);font-size:1.02rem;text-transform:uppercase;letter-spacing:0.8px;}
  .modal-body{padding:16px 20px;overflow-y:auto;flex:1;}
  .modal-footer{padding:12px 20px;border-top:1px solid rgba(255,255,255,0.08);display:flex;justify-content:space-between;gap:10px;}
  .wifi-item{display:flex;align-items:center;justify-content:space-between;background:var(--card2);border:1px solid rgba(0,212,255,0.18);border-radius:10px;padding:12px 14px;margin-bottom:8px;cursor:pointer;transition:0.18s;}
  .wifi-item:hover{border-color:var(--cyan);background:#162d54;transform:translateY(-1px);}
  .wifi-ssid{font-weight:700;color:#fff;font-size:0.95rem;margin-bottom:3px;word-break:break-all;}
  .wifi-meta{font-size:0.78rem;color:var(--muted);}
  #toast{position:fixed;bottom:20px;right:20px;background:#0f2942;border:1px solid var(--cyan);color:#fff;padding:12px 20px;border-radius:10px;box-shadow:0 8px 25px rgba(0,0,0,0.5);display:none;z-index:1100;font-size:0.9rem;}
</style>
</head>
<body>
<header>
  <div class="brand">
    <svg viewBox="0 0 120 120" fill="none">
      <circle cx="60" cy="60" r="54" stroke="#00d4ff" stroke-width="4" stroke-dasharray="12 6"/>
      <path d="M60 26C47 26 39 36 39 50V68L31 76V80H89V76L81 68V50C81 36 73 26 60 26Z" fill="#f59e0b"/>
      <circle cx="60" cy="87" r="6" fill="#fbbf24"/>
    </svg>
    <div>
      <h1>ChronoRing <span id="fwHeaderBadge" class="badge badge-ok" style="font-size:0.72rem;vertical-align:middle;margin-left:8px;">v)rawliteral" CHRONORING_FW_VERSION R"rawliteral(</span><span id="superBadge" class="badge badge-warn" style="display:none;font-size:0.72rem;vertical-align:middle;margin-left:6px;">🛠️ MODALITÀ TECNICO</span></h1>
      <span id="netInfo">Connessione in corso...</span>
    </div>
  </div>
  <div class="nav-btns">
    <a href="/update" class="btn btn-outline">⬆️ Aggiornamento OTA</a>
    <a href="/logout" class="btn btn-danger">Esci</a>
  </div>
</header>

<div class="container">
  <!-- OROLOGIO E STATO SISTEMA -->
  <div class="card">
    <h2><span>🕒 Orario & Sincronizzazione</span><span id="ntpBadge" class="badge badge-warn">Verifica...</span></h2>
    <div class="clock-box">
      <div class="clock-time" id="liveTime">--:--:--</div>
      <div class="clock-date" id="liveDate">Caricamento...</div>
    </div>
    <div class="stat-row"><span>Prossimo Squillo:</span><strong id="nextBell" style="color:var(--gold)">--</strong></div>
    <div class="stat-row"><span>Ultimo Squillo Auto:</span><strong id="lastBell">--</strong></div>
    <div class="stat-row"><span id="relayPinLabel">Stato Relè:</span><strong id="relayState">INATTIVO</strong></div>
    <div class="stat-row"><span>Modalità Operativa:</span><strong id="hwMode">--</strong></div>
    <div class="stat-row"><span>Versione Firmware:</span><strong id="fwVer" style="color:var(--cyan)">v)rawliteral" CHRONORING_FW_VERSION R"rawliteral(</strong></div>
    <div class="stat-row"><span>Indirizzo IP / mDNS:</span><strong id="ipAddr">chronoring.local</strong></div>
  </div>

  <!-- COMANDI RAPIDI & SOSPENSIONE -->
  <div class="card">
    <h2><span>⚡ Controllo Rapido Campanella</span></h2>
    <div style="margin-bottom:20px;">
      <button class="btn btn-gold" style="width:100%;justify-content:center;padding:16px;font-size:1.05rem;" onclick="ringNow()">
        🔔 SUONA ORA (TEST MANUALE)
      </button>
    </div>

    <div class="toggle-row">
      <div>
        <strong>Sospensione Globale</strong>
        <div style="font-size:0.8rem;color:var(--muted);margin-top:2px;">Blocca tutti i suoni automatici programmati</div>
      </div>
      <label class="switch">
        <input type="checkbox" id="chkSuspended" onchange="toggleSuspend(this.checked)">
        <span class="slider danger-slider"></span>
      </label>
    </div>
  </div>

  <!-- DURATA SQUILLO E SICUREZZA -->
  <div class="card">
    <h2><span>⚙️ Durata Squillo &amp; Password</span></h2>
    <div class="form-grid">
      <div class="form-group">
        <label>Durata Impulso Relè (ms) — [Min: 200 ms | Hard Cap Max: 10000 ms]</label>
        <div style="display:flex;gap:8px;">
          <input type="number" id="pulseMs" min="200" max="10000" step="100" value="3000">
          <button class="btn btn-cyan" onclick="savePulse()">Salva</button>
        </div>
      </div>
      <hr style="border:none;border-top:1px solid rgba(255,255,255,0.08);margin:4px 0;">
      <div class="form-group">
        <label>Modifica Password Web Amministratore</label>
        <div style="display:flex;gap:8px;">
          <input type="password" id="newWebPwd" placeholder="Nuova password...">
          <button class="btn btn-outline" onclick="savePassword()">Aggiorna</button>
        </div>
      </div>
    </div>
  </div>

  <!-- TABELLA ORARI PROGRAMMATI -->
  <div class="card card-full">
    <h2>
      <span>📅 Programmazione Orari Settimanale</span>
      <button class="btn btn-outline" onclick="addScheduleRow()">+ Aggiungi Orario</button>
    </h2>
    <div style="overflow-x:auto;">
      <table>
        <thead>
          <tr>
            <th>#</th>
            <th>Orario (HH:MM)</th>
            <th>Etichetta / Evento</th>
            <th>Giorni Attivi</th>
            <th>Abilitato</th>
            <th>Elimina</th>
          </tr>
        </thead>
        <tbody id="scheduleBody"></tbody>
      </table>
    </div>
    <div style="margin-top:16px;display:flex;justify-content:flex-end;">
      <button class="btn btn-cyan" onclick="saveSchedule()">💾 SALVA TABELLA ORARI</button>
    </div>
  </div>

  <!-- PERSONALIZZAZIONE ISTITUTO & RISPARMIO DISPLAY -->
  <div class="card card-full">
    <h2>
      <span>🏫 Personalizzazione Istituto &amp; Risparmio Display</span>
      <span id="logoStatusBadge" class="badge badge-warn">Nessun Logo</span>
    </h2>
    <div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:18px;align-items:start;">
      <div class="form-grid">
        <div class="form-group">
          <label>Nome dell'Istituto Scolastico (mostrato nella schermata di Standby)</label>
          <div style="display:flex;gap:8px;">
            <input type="text" id="schoolName" maxlength="60" placeholder="Es. I.I.S. Enrico Fermi">
            <button class="btn btn-cyan" onclick="saveSchoolName()">💾 Salva Nome</button>
          </div>
        </div>
        <hr style="border:none;border-top:1px solid rgba(255,255,255,0.08);margin:2px 0;">
        <div class="toggle-row" style="margin-bottom:4px;">
          <div>
            <strong>Spegni completamente lo schermo al timeout</strong>
            <div style="font-size:0.78rem;color:var(--muted);margin-top:2px;">Disattivo = Standby orologio/logo • Attivo = Schermo spento (0%). Si riaccende al tocco o su evento.</div>
          </div>
          <label class="switch">
            <input type="checkbox" id="chkDisplaySleepOff">
            <span class="slider"></span>
          </label>
        </div>
        <div class="form-group">
          <label>Timeout inattività Display (secondi) — [Min: 10s | Max: 3600s]</label>
          <div style="display:flex;gap:8px;">
            <input type="number" id="displayTimeoutSec" min="10" max="3600" step="5" value="60">
            <button class="btn btn-cyan" onclick="saveDisplaySettings()">💾 Salva Display</button>
          </div>
        </div>
      </div>

      <div>
        <div class="logo-preview-wrap">
          <canvas id="logoCanvas" width="76" height="76"></canvas>
          <div style="flex:1;">
            <label style="display:block;font-size:0.8rem;color:var(--cyan);margin-bottom:6px;font-weight:600;">LOGO DELLA SCUOLA (PNG / JPG / SVG)</label>
            <input type="file" id="logoInput" accept="image/*" style="font-size:0.82rem;width:100%;margin-bottom:8px;" onchange="previewLogoFile(this)">
            <div style="display:flex;gap:8px;flex-wrap:wrap;">
              <button class="btn btn-cyan" style="padding:7px 12px;font-size:0.82rem;" onclick="uploadLogo()">⬆️ Carica Logo sul Display</button>
              <button class="btn btn-danger" style="padding:7px 12px;font-size:0.82rem;" onclick="deleteLogo()">🗑️ Rimuovi</button>
            </div>
          </div>
        </div>
      </div>
    </div>
  </div>

  <!-- CONFIGURAZIONE WI-FI -->
  <div class="card card-full">
    <h2>
      <span>📶 Configurazione Rete Wi-Fi</span>
      <button class="btn btn-outline" style="padding:6px 12px;font-size:0.8rem;" onclick="openWifiModal()" id="btnScanWifi">🔍 Cerca Reti</button>
    </h2>
    <div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(240px,1fr));gap:14px;align-items:end;">
      <div class="form-group">
        <label>SSID Rete Wi-Fi</label>
        <input type="text" id="wifiSsid" placeholder="Nome rete Wi-Fi...">
      </div>
      <div class="form-group">
        <label>Password Wi-Fi (lascia vuoto per mantenere l'attuale)</label>
        <input type="password" id="wifiPwd" placeholder="Password Wi-Fi...">
      </div>
      <div>
        <button class="btn btn-cyan" style="width:100%;justify-content:center;padding:10px 15px;" onclick="saveWiFi()">💾 Salva e Connetti Wi-Fi</button>
      </div>
    </div>
  </div>

  <!-- SICUREZZA HOTSPOT AP, BACKUP & RIPRISTINO -->
  <div class="card card-full">
    <h2>
      <span>🛡️ Sicurezza Hotspot Dispositivo (AP), Backup &amp; Ripristino</span>
      <span id="apSecBadge" class="badge badge-warn">Verifica...</span>
    </h2>
    <div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:20px;align-items:start;">
      <div class="form-grid">
        <div class="form-group">
          <label>SSID Hotspot Dispositivo (max 31 caratteri)</label>
          <input type="text" id="apSsid" maxlength="31" placeholder="Es. ChronoRing-Setup">
        </div>
        <div class="form-group">
          <label>Password Hotspot WPA2 (min 8 caratteri — lascia vuoto per AP aperto)</label>
          <input type="password" id="apPwd" maxlength="63" placeholder="Minimo 8 caratteri oppure vuoto (aperto)...">
        </div>
        <button class="btn btn-cyan" onclick="saveApConfig()">💾 Salva Credenziali Hotspot AP</button>

        <hr style="border:none;border-top:1px solid rgba(255,255,255,0.08);margin:4px 0;">
        <div class="toggle-row" style="margin-bottom:4px;">
          <div>
            <strong>Blocca Hotspot dopo connessione Wi-Fi STA</strong>
            <div style="font-size:0.78rem;color:var(--muted);margin-top:2px;">Spegne sempre l'AP sulla rete scolastica e impedisce hotspot aperti non presidiati</div>
          </div>
          <label class="switch">
            <input type="checkbox" id="chkApLockAfterSta" checked>
            <span class="slider"></span>
          </label>
        </div>
        <div class="form-group">
          <label>Timeout spegnimento Hotspot senza client (minuti, 0 = Mai, max 120)</label>
          <div style="display:flex;gap:8px;">
            <input type="number" id="apTimeoutMin" min="0" max="120" step="1" value="15">
            <button class="btn btn-outline" onclick="saveApPolicy()">🛡️ Salva Policy</button>
          </div>
        </div>
      </div>

      <div class="form-grid">
        <div style="background:var(--card2);border:1px solid rgba(0,212,255,0.25);border-radius:12px;padding:16px;">
          <div style="font-weight:700;color:var(--cyan);margin-bottom:6px;">💾 Backup &amp; Ripristino Configurazione (.json)</div>
          <div style="font-size:0.82rem;color:var(--muted);line-height:1.45;margin-bottom:12px;">
            Esporta tutti gli orari, le etichette e le impostazioni di ChronoRing oppure ricaricali su un dispositivo nuovo.
          </div>
          <div style="display:flex;flex-direction:column;gap:10px;">
            <button class="btn btn-cyan" style="justify-content:center;" onclick="downloadBackup()">⬇️ Scarica Backup (.json)</button>
            <div style="display:flex;gap:8px;align-items:center;flex-wrap:wrap;">
              <input type="file" id="restoreFileInput" accept=".json,application/json" style="flex:1;min-width:160px;font-size:0.82rem;">
              <button class="btn btn-outline" id="btnRestoreBackup" onclick="uploadRestoreBackup()">⬆️ Ripristina</button>
            </div>
          </div>
        </div>

        <div style="background:rgba(239,68,68,0.08);border:1px solid rgba(239,68,68,0.32);border-radius:12px;padding:16px;">
          <div style="font-weight:700;color:#fca5a5;margin-bottom:6px;">⚠️ Ripristino Impostazioni di Fabbrica</div>
          <div style="font-size:0.82rem;color:var(--muted);line-height:1.45;margin-bottom:14px;">
            Cancella reti Wi-Fi salvate, personalizzazione Hotspot AP, nome/logo dell'istituto, password Web e riporta orari e durata relè ai valori predefiniti (password: <code>admin</code>, hotspot: <code>ChronoRing-Setup</code> aperto).
          </div>
          <button class="btn btn-danger" style="width:100%;justify-content:center;padding:12px;" id="btnFactoryReset" onclick="factoryResetDevice()">
            🔄 RIPRISTINA IMPOSTAZIONI DI FABBRICA
          </button>
        </div>
      </div>
    </div>
  </div>

  <!-- AREA RISERVATA TECNICO (VISIBILE SOLO CON SUPER PASSWORD) -->
  <div class="card card-full" id="superAdminCard" style="display:none;border-color:rgba(245,158,11,0.45);">
    <h2>
      <span>🛠️ Area Tecnico &amp; Sicurezza Super Password</span>
      <span class="badge badge-warn">SOLO SUPERADMIN</span>
    </h2>
    <div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:20px;align-items:start;">
      <div class="form-grid">
        <div class="form-group">
          <label>Super Password Tecnico Attuale</label>
          <input type="password" id="curSuperPwd" maxlength="63" placeholder="Password tecnico attuale...">
        </div>
        <div class="form-group">
          <label>Nuova Super Password Tecnico (min 8 caratteri)</label>
          <input type="password" id="newSuperPwd" maxlength="63" placeholder="Nuova super password (min 8 caratteri)...">
        </div>
        <div class="form-group">
          <label>Conferma Nuova Super Password</label>
          <input type="password" id="cfmSuperPwd" maxlength="63" placeholder="Ripeti nuova super password...">
        </div>
        <button class="btn btn-gold" onclick="saveSuperPassword()">🔑 Aggiorna Super Password NVS</button>
      </div>

      <div style="background:rgba(239,68,68,0.1);border:1px solid rgba(239,68,68,0.42);border-radius:12px;padding:16px;">
        <div style="font-weight:700;color:#fca5a5;margin-bottom:6px;">🗑️ Reset Totale Dispositivo (Configurazione + Super Password)</div>
        <div style="font-size:0.82rem;color:var(--muted);line-height:1.45;margin-bottom:12px;">
          Il normale Ripristino di Fabbrica della scuola <strong>mantiene intatta</strong> la Super Password in NVS. Usa questa funzione solo prima di dismettere o cedere la scheda per cancellare anche la Super Password e riattivare il wizard iniziale.
        </div>
        <div class="form-group" style="margin-bottom:12px;">
          <label>Conferma con Super Password Tecnico</label>
          <input type="password" id="fullResetSuperPwd" maxlength="63" placeholder="Inserisci Super Password...">
        </div>
        <button class="btn btn-danger" style="width:100%;justify-content:center;padding:12px;" id="btnFactoryResetFull" onclick="factoryResetFullDevice()">
          🗑️ RESET TOTALE (CONFIG + SUPER PASSWORD)
        </button>
      </div>
    </div>
  </div>
</div>

<div class="modal-overlay" id="wifiModal" onclick="if(event.target===this)closeWifiModal()">
  <div class="modal-box">
    <div class="modal-header">
      <h3>📶 Reti Wi-Fi Disponibili</h3>
      <button class="btn btn-danger" style="padding:4px 10px;font-size:0.82rem;" onclick="closeWifiModal()">✕</button>
    </div>
    <div class="modal-body" id="wifiModalBody"></div>
    <div class="modal-footer">
      <button class="btn btn-outline" id="btnRescanModal" onclick="startWifiScan()">🔄 Aggiorna Lista</button>
      <button class="btn btn-danger" onclick="closeWifiModal()">Chiudi</button>
    </div>
  </div>
</div>

<div id="toast"></div>

<script>
let scheduleData = [];
let logoReadyToUpload = false;

function showToast(msg, isErr=false) {
  const t = document.getElementById('toast');
  t.textContent = msg;
  t.style.borderColor = isErr ? '#ef4444' : '#00d4ff';
  t.style.display = 'block';
  setTimeout(() => { t.style.display = 'none'; }, 3200);
}

async function fetchStatus() {
  try {
    const r = await fetch('/api/status');
    if (r.status === 401) { window.location.href = '/login'; return; }
    const d = await r.json();
    document.getElementById('liveTime').textContent = d.time;
    document.getElementById('liveDate').textContent = d.date;
    const badge = document.getElementById('ntpBadge');
    badge.textContent = d.ntp_synced ? 'NTP SINCRONIZZATO' : 'ATTESA NTP';
    badge.className = 'badge ' + (d.ntp_synced ? 'badge-ok' : 'badge-warn');
    document.getElementById('nextBell').textContent = d.next_bell;
    document.getElementById('lastBell').textContent = d.last_bell;
    const rs = document.getElementById('relayState');
    rs.textContent = d.relay_active ? '🔔 ATTIVO (SUONANDO)' : 'INATTIVO (SICURO)';
    rs.style.color = d.relay_active ? '#fbbf24' : '#34d399';
    if (d.relay_pin !== undefined) {
      document.getElementById('relayPinLabel').textContent = 'Stato Relè (GPIO ' + d.relay_pin + '):';
    }
    const hm = document.getElementById('hwMode');
    if (hm) {
      hm.textContent = d.has_display ? '🖥️ Display Touch Attivo' : '⚡ Headless (Web/Scheduler)';
      hm.style.color = d.has_display ? '#00d4ff' : '#94a3b8';
    }
    if (d.fw_version) {
      const fv = 'v' + d.fw_version;
      const fwEl = document.getElementById('fwVer');
      const fwBadge = document.getElementById('fwHeaderBadge');
      if (fwEl) fwEl.textContent = fv;
      if (fwBadge) fwBadge.textContent = fv;
    }
    document.getElementById('netInfo').textContent = d.net_mode;
    document.getElementById('ipAddr').textContent = d.ip + ' (chronoring.local)';
    document.getElementById('chkSuspended').checked = d.suspended;
    applyRoleUi(d.role);
  } catch (e) {}
}

function applyRoleUi(role) {
  const isSup = (role === 'superadmin');
  const sb = document.getElementById('superBadge');
  const sc = document.getElementById('superAdminCard');
  if (sb) sb.style.display = isSup ? 'inline-block' : 'none';
  if (sc) sc.style.display = isSup ? 'block' : 'none';
}

async function loadConfig() {
  const r = await fetch('/api/config');
  if (r.status === 401) { window.location.href = '/login'; return; }
  const d = await r.json();
  applyRoleUi(d.role);
  document.getElementById('chkSuspended').checked = d.suspended;
  document.getElementById('pulseMs').value = d.relay_pulse_ms;
  document.getElementById('wifiSsid').value = d.wifi_ssid || '';
  document.getElementById('apSsid').value = d.ap_ssid || 'ChronoRing-Setup';
  document.getElementById('chkDisplaySleepOff').checked = !!d.display_sleep_off;
  document.getElementById('displayTimeoutSec').value = d.display_timeout_sec || 60;
  document.getElementById('chkApLockAfterSta').checked = (d.ap_lock_after_sta !== false);
  document.getElementById('apTimeoutMin').value = (d.ap_timeout_min !== undefined) ? d.ap_timeout_min : 15;
  const apBadge = document.getElementById('apSecBadge');
  if (apBadge) {
    apBadge.textContent = d.ap_has_password ? '🔒 AP PROTETTO WPA2' : '🔓 AP APERTO (NESSUNA PASSWORD)';
    apBadge.className = 'badge ' + (d.ap_has_password ? 'badge-ok' : 'badge-warn');
  }
  document.getElementById('schoolName').value = d.school_name || '';
  updateLogoBadge(d.has_school_logo);
  scheduleData = d.schedule || [];
  renderSchedule();
}

function updateLogoBadge(hasLogo) {
  const b = document.getElementById('logoStatusBadge');
  b.textContent = hasLogo ? 'LOGO CARICATO SUL DISPLAY' : 'NESSUN LOGO CARICATO';
  b.className = 'badge ' + (hasLogo ? 'badge-ok' : 'badge-warn');
}

function previewLogoFile(input) {
  if (!input.files || !input.files[0]) return;
  const reader = new FileReader();
  reader.onload = (e) => {
    const img = new Image();
    img.onload = () => {
      const canvas = document.getElementById('logoCanvas');
      const ctx = canvas.getContext('2d');
      const W = canvas.width, H = canvas.height;
      // Sfondo identico alla schermata di standby (#0B0F17) per trasparenze PNG pulite
      ctx.fillStyle = '#0B0F17';
      ctx.fillRect(0, 0, W, H);
      // Ridimensiona mantenendo l'aspect ratio centrato
      const scale = Math.min(W / img.width, H / img.height);
      const nw = img.width * scale, nh = img.height * scale;
      const nx = (W - nw) / 2, ny = (H - nh) / 2;
      ctx.drawImage(img, nx, ny, nw, nh);
      logoReadyToUpload = true;
    };
    img.src = e.target.result;
  };
  reader.readAsDataURL(input.files[0]);
}

async function uploadLogo() {
  if (!logoReadyToUpload) {
    showToast('Seleziona prima un file immagine per il logo!', true);
    return;
  }
  const canvas = document.getElementById('logoCanvas');
  const W = canvas.width, H = canvas.height;
  const imgData = canvas.getContext('2d').getImageData(0, 0, W, H).data;

  // Costruisce buffer binario RGB565: [uint16_t W][uint16_t H][W*H*2 bytes RGB565 little-endian]
  const buf = new Uint8Array(4 + W * H * 2);
  buf[0] = W & 0xFF; buf[1] = (W >> 8) & 0xFF;
  buf[2] = H & 0xFF; buf[3] = (H >> 8) & 0xFF;

  let offset = 4;
  for (let i = 0; i < imgData.length; i += 4) {
    const r = imgData[i] >> 3;
    const g = imgData[i + 1] >> 2;
    const b = imgData[i + 2] >> 3;
    const rgb565 = (r << 11) | (g << 5) | b;
    buf[offset++] = rgb565 & 0xFF;
    buf[offset++] = (rgb565 >> 8) & 0xFF;
  }

  const formData = new FormData();
  formData.append('logo', new Blob([buf], {type: 'application/octet-stream'}), 'school_logo.bin');

  const r = await fetch('/api/school-logo', {method: 'POST', body: formData});
  const d = await r.json();
  showToast(d.message, !r.ok);
  if (r.ok) updateLogoBadge(true);
}

async function deleteLogo() {
  const r = await fetch('/api/school-logo', {method: 'DELETE'});
  const d = await r.json();
  showToast(d.message, !r.ok);
  if (r.ok) {
    const canvas = document.getElementById('logoCanvas');
    const ctx = canvas.getContext('2d');
    ctx.fillStyle = '#0B0F17';
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    logoReadyToUpload = false;
    updateLogoBadge(false);
  }
}

async function saveSchoolName() {
  const name = document.getElementById('schoolName').value.trim();
  const r = await fetch('/api/school-info', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({school_name: name})
  });
  const d = await r.json();
  showToast(d.message, !r.ok);
}

function escapeAttr(s) {
  return String(s || '')
    .replace(/&/g, '&amp;')
    .replace(/"/g, '&quot;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;');
}

let activeScheduleRow = -1;
const DAY_DEFS = [
  {bit: 1, lbl: 'Lun'},
  {bit: 2, lbl: 'Mar'},
  {bit: 3, lbl: 'Mer'},
  {bit: 4, lbl: 'Gio'},
  {bit: 5, lbl: 'Ven'},
  {bit: 6, lbl: 'Sab'},
  {bit: 0, lbl: 'Dom'}
];
const DAY_PRESETS = [
  {mask: 62,  lbl: 'Lun-Ven'},
  {mask: 126, lbl: 'Lun-Sab'},
  {mask: 42,  lbl: 'Lun-Mer-Ven'},
  {mask: 84,  lbl: 'Mar-Gio-Sab'}
];

function setActiveScheduleRow(idx) {
  activeScheduleRow = idx;
  document.querySelectorAll('#scheduleBody tr').forEach((tr) => {
    const rowIdx = parseInt(tr.getAttribute('data-row'), 10);
    tr.classList.toggle('editing-row', rowIdx === idx);
  });
}

function updateRowDayUI(idx) {
  const tr = document.querySelector('#scheduleBody tr[data-row="' + idx + '"]');
  if (!tr) return;
  const mask = (scheduleData[idx].days_mask !== undefined) ? scheduleData[idx].days_mask : 126;
  tr.querySelectorAll('.day-pill').forEach((btn) => {
    const bit = parseInt(btn.getAttribute('data-bit'), 10);
    btn.classList.toggle('active', (mask & (1 << bit)) !== 0);
  });
  tr.querySelectorAll('.preset-btn').forEach((btn) => {
    const pMask = parseInt(btn.getAttribute('data-mask'), 10);
    btn.classList.toggle('active', (mask & 0x7F) === pMask);
  });
}

function toggleDayBit(idx, bit) {
  let mask = (scheduleData[idx].days_mask !== undefined) ? scheduleData[idx].days_mask : 126;
  mask ^= (1 << bit);
  scheduleData[idx].days_mask = mask;
  setActiveScheduleRow(idx);
  updateRowDayUI(idx);
}

function applyDayPreset(idx, mask) {
  scheduleData[idx].days_mask = mask;
  setActiveScheduleRow(idx);
  updateRowDayUI(idx);
}

function renderSchedule(focusIdx = -1, isNewRow = false) {
  const tbody = document.getElementById('scheduleBody');
  tbody.innerHTML = '';
  scheduleData.forEach((item, idx) => {
    const mask = (item.days_mask !== undefined) ? item.days_mask : 126;
    const pillsHtml = DAY_DEFS.map(d => {
      const active = (mask & (1 << d.bit)) !== 0 ? ' active' : '';
      const domCls = d.bit === 0 ? ' dom-pill' : '';
      return `<button type="button" class="day-pill${domCls}${active}" data-bit="${d.bit}" onclick="event.stopPropagation();toggleDayBit(${idx},${d.bit})">${d.lbl}</button>`;
    }).join('');
    const presetsHtml = DAY_PRESETS.map(p => {
      const active = (mask & 0x7F) === p.mask ? ' active' : '';
      return `<button type="button" class="preset-btn${active}" data-mask="${p.mask}" onclick="event.stopPropagation();applyDayPreset(${idx},${p.mask})">${p.lbl}</button>`;
    }).join('');

    const tr = document.createElement('tr');
    tr.setAttribute('data-row', String(idx));
    if (idx === activeScheduleRow || idx === focusIdx) {
      tr.classList.add('editing-row');
    }
    if (isNewRow && idx === focusIdx) {
      tr.classList.add('new-row-flash');
    }
    tr.setAttribute('onfocusin', `setActiveScheduleRow(${idx})`);
    tr.setAttribute('onclick', `setActiveScheduleRow(${idx})`);

    const missingCls = !item.time ? 'input-missing' : '';
    tr.innerHTML = `
      <td>${idx + 1}</td>
      <td><input type="time" id="schedTime_${idx}" value="${escapeAttr(item.time || '')}" class="${missingCls}" title="Inserisci l'orario di squillo (HH:MM)" oninput="scheduleData[${idx}].time = this.value; this.classList.toggle('input-missing', !this.value);" onchange="scheduleData[${idx}].time = this.value; this.classList.toggle('input-missing', !this.value);"></td>
      <td><input type="text" id="schedLabel_${idx}" maxlength="30" placeholder="Es. 1ª Ora, Ricreazione, Uscita..." value="${escapeAttr(item.label || '')}" style="width:100%;min-width:160px;" oninput="scheduleData[${idx}].label = this.value;"></td>
      <td class="days-cell">
        <div class="day-pills">${pillsHtml}</div>
        <div class="day-presets">${presetsHtml}</div>
      </td>
      <td>
        <label class="switch">
          <input type="checkbox" ${item.enabled ? 'checked' : ''} onchange="scheduleData[${idx}].enabled = this.checked">
          <span class="slider"></span>
        </label>
      </td>
      <td><button type="button" class="btn btn-danger" style="padding:5px 10px" onclick="event.stopPropagation();removeRow(${idx})">✕</button></td>
    `;
    tbody.appendChild(tr);
  });

  if (focusIdx >= 0 && focusIdx < scheduleData.length) {
    const targetRow = tbody.querySelector('tr[data-row="' + focusIdx + '"]');
    const targetTime = document.getElementById('schedTime_' + focusIdx);
    if (targetRow) {
      targetRow.scrollIntoView({behavior: 'smooth', block: 'center'});
    }
    if (targetTime) {
      setTimeout(() => targetTime.focus(), 60);
    }
  }
}

function addScheduleRow() {
  scheduleData.push({time: '', label: '', days_mask: 126, enabled: true});
  activeScheduleRow = scheduleData.length - 1;
  renderSchedule(activeScheduleRow, true);
  showToast('Nuova riga aggiunta: inserisci orario, etichetta e giorni, poi premi SALVA.');
}

function removeRow(idx) {
  scheduleData.splice(idx, 1);
  if (activeScheduleRow === idx) {
    activeScheduleRow = -1;
  } else if (activeScheduleRow > idx) {
    activeScheduleRow--;
  }
  renderSchedule(activeScheduleRow, false);
}

async function ringNow() {
  const r = await fetch('/api/ring', {method: 'POST'});
  const d = await r.json();
  showToast(d.message, !r.ok);
  fetchStatus();
}

async function toggleSuspend(val) {
  const r = await fetch('/api/toggle-suspend', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({suspended: val})
  });
  if (r.ok) {
    showToast(val ? 'Campanella SOSPESA' : 'Campanella ATTIVATA');
    fetchStatus();
  }
}

async function saveSchedule() {
  for (let i = 0; i < scheduleData.length; i++) {
    if (!scheduleData[i].time) {
      setActiveScheduleRow(i);
      const tEl = document.getElementById('schedTime_' + i);
      if (tEl) {
        tEl.classList.add('input-missing');
        tEl.scrollIntoView({behavior: 'smooth', block: 'center'});
        tEl.focus();
      }
      showToast('⚠️ Inserisci un orario (HH:MM) nella riga #' + (i + 1) + ' prima di salvare!', true);
      return;
    }
    if (!(scheduleData[i].days_mask & 0x7F)) {
      setActiveScheduleRow(i);
      const tr = document.querySelector('#scheduleBody tr[data-row="' + i + '"]');
      if (tr) tr.scrollIntoView({behavior: 'smooth', block: 'center'});
      showToast('⚠️ Seleziona almeno un giorno attivo nella riga #' + (i + 1) + '!', true);
      return;
    }
  }
  const r = await fetch('/api/schedule', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({schedule: scheduleData})
  });
  const d = await r.json();
  showToast(d.message || 'Orari salvati!', !r.ok);
  if (r.ok) {
    activeScheduleRow = -1;
  }
  loadConfig();
  fetchStatus();
}

async function savePulse() {
  let ms = parseInt(document.getElementById('pulseMs').value, 10);
  if (isNaN(ms) || ms < 200) ms = 200;
  if (ms > 10000) { ms = 10000; document.getElementById('pulseMs').value = 10000; }
  const r = await fetch('/api/settings', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({relay_pulse_ms: ms})
  });
  const d = await r.json();
  showToast(d.message, !r.ok);
}

async function savePassword() {
  const pwd = document.getElementById('newWebPwd').value.trim();
  if (pwd.length < 3 || pwd.length > 31) { showToast('La password deve essere tra 3 e 31 caratteri', true); return; }
  const r = await fetch('/api/password', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({password: pwd})
  });
  const d = await r.json().catch(() => ({}));
  if (r.ok) {
    document.getElementById('newWebPwd').value = '';
    if (d.keep_session) {
      showToast(d.message || '✅ Password gestori aggiornata (sessione tecnico attiva)!');
    } else {
      showToast(d.message || 'Password aggiornata! Effettua nuovamente il login.');
      setTimeout(() => window.location.href = '/login', 1500);
    }
  } else {
    showToast(d.message || 'Errore salvataggio password', true);
  }
}

let wifiScanTimer = null;

function openWifiModal() {
  document.getElementById('wifiModal').style.display = 'flex';
  startWifiScan();
}

function closeWifiModal() {
  document.getElementById('wifiModal').style.display = 'none';
  if (wifiScanTimer) {
    clearTimeout(wifiScanTimer);
    wifiScanTimer = null;
  }
}

function selectWifiFromModal(ssid) {
  document.getElementById('wifiSsid').value = ssid;
  closeWifiModal();
  showToast('Rete "' + ssid + '" selezionata! Inserisci la password e premi Salva.');
  document.getElementById('wifiPwd').focus();
}

async function startWifiScan(attempt = 0) {
  const body = document.getElementById('wifiModalBody');
  const btnRescan = document.getElementById('btnRescanModal');
  if (wifiScanTimer) {
    clearTimeout(wifiScanTimer);
    wifiScanTimer = null;
  }
  if (attempt === 0) {
    body.innerHTML = '<div style="text-align:center;padding:28px 10px;color:var(--cyan);">⏳ Scansione reti Wi-Fi in corso...<div style="font-size:0.8rem;color:var(--muted);margin-top:6px;">Attendere (2-3 secondi)</div></div>';
    btnRescan.disabled = true;
  }
  try {
    const r = await fetch('/api/wifi-scan');
    if (r.status === 401) { window.location.href = '/login'; return; }
    const d = await r.json();
    if (d.status === 'running') {
      if (attempt < 12) {
        wifiScanTimer = setTimeout(() => startWifiScan(attempt + 1), 600);
        return;
      }
      body.innerHTML = '<div style="text-align:center;padding:20px;color:#fca5a5;">Timeout scansione. Premi Aggiorna Lista per riprovare.</div>';
      btnRescan.disabled = false;
      return;
    }
    btnRescan.disabled = false;
    if (d.status === 'error') {
      body.innerHTML = '<div style="text-align:center;padding:20px;color:#fca5a5;">Impossibile eseguire la scansione in questo momento. Riprova tra un istante.</div>';
      return;
    }
    const nets = (d.networks || []).slice().sort((a, b) => b.rssi - a.rssi);
    if (nets.length === 0) {
      body.innerHTML = '<div style="text-align:center;padding:20px;color:var(--muted);">Nessuna rete Wi-Fi rilevata nelle vicinanze.</div>';
      return;
    }
    body.innerHTML = '';
    nets.forEach(n => {
      const item = document.createElement('div');
      item.className = 'wifi-item';
      item.onclick = () => selectWifiFromModal(n.ssid);

      const left = document.createElement('div');
      const ssidEl = document.createElement('div');
      ssidEl.className = 'wifi-ssid';
      ssidEl.textContent = n.ssid;
      const metaEl = document.createElement('div');
      metaEl.className = 'wifi-meta';
      metaEl.textContent = (n.open ? '🔓 Rete Aperta' : '🔒 Protetta') + ' • Segnale ' + n.rssi + ' dBm';
      left.appendChild(ssidEl);
      left.appendChild(metaEl);

      const right = document.createElement('span');
      const qual = n.rssi >= -60 ? 'Ottimo' : (n.rssi >= -75 ? 'Buono' : 'Debole');
      right.className = 'badge ' + (n.rssi >= -65 ? 'badge-ok' : 'badge-warn');
      right.textContent = qual;

      item.appendChild(left);
      item.appendChild(right);
      body.appendChild(item);
    });
  } catch (e) {
    btnRescan.disabled = false;
    body.innerHTML = '<div style="text-align:center;padding:20px;color:#fca5a5;">Errore di comunicazione durante la scansione.</div>';
  }
}

async function saveWiFi() {
  const ssid = document.getElementById('wifiSsid').value.trim();
  const pwd = document.getElementById('wifiPwd').value;
  const r = await fetch('/api/wifi', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({ssid, password: pwd})
  });
  const d = await r.json();
  showToast(d.message, !r.ok);
}

async function saveApConfig() {
  const ap_ssid = document.getElementById('apSsid').value.trim();
  const ap_password = document.getElementById('apPwd').value;
  if (!ap_ssid) {
    showToast('Inserisci un SSID valido per l\'Hotspot AP.', true);
    return;
  }
  if (ap_password.length > 0 && ap_password.length < 8) {
    showToast('La password WPA2 deve avere almeno 8 caratteri (oppure resta vuota).', true);
    return;
  }
  const r = await fetch('/api/ap-config', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({ap_ssid, ap_password})
  });
  const d = await r.json();
  showToast(d.message, !r.ok);
  if (r.ok) {
    document.getElementById('apPwd').value = '';
    loadConfig();
  }
}

async function saveDisplaySettings() {
  let sec = parseInt(document.getElementById('displayTimeoutSec').value, 10);
  if (isNaN(sec) || sec < 10) sec = 10;
  if (sec > 3600) sec = 3600;
  document.getElementById('displayTimeoutSec').value = sec;
  const sleep_off = document.getElementById('chkDisplaySleepOff').checked;
  const r = await fetch('/api/display-settings', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({display_timeout_sec: sec, display_sleep_off: sleep_off})
  });
  const d = await r.json();
  showToast(d.message, !r.ok);
}

async function saveApPolicy() {
  let min = parseInt(document.getElementById('apTimeoutMin').value, 10);
  if (isNaN(min) || min < 0) min = 0;
  if (min > 120) min = 120;
  document.getElementById('apTimeoutMin').value = min;
  const lock = document.getElementById('chkApLockAfterSta').checked;
  const r = await fetch('/api/ap-policy', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({ap_timeout_min: min, ap_lock_after_sta: lock})
  });
  const d = await r.json();
  showToast(d.message, !r.ok);
}

function downloadBackup() {
  window.location.href = '/api/backup';
}

async function uploadRestoreBackup() {
  const input = document.getElementById('restoreFileInput');
  if (!input.files || !input.files[0]) {
    showToast('Seleziona prima un file di backup .json!', true);
    return;
  }
  if (!confirm('Vuoi sovrascrivere orari e impostazioni con il backup selezionato? Il dispositivo si riavvierà al termine.')) {
    return;
  }
  const btn = document.getElementById('btnRestoreBackup');
  btn.disabled = true;
  const formData = new FormData();
  formData.append('backup', input.files[0], input.files[0].name);
  try {
    const r = await fetch('/api/restore', {method: 'POST', body: formData});
    const d = await r.json();
    showToast(d.message, !r.ok);
    if (r.ok) {
      setTimeout(() => { window.location.href = '/login'; }, 6000);
    } else {
      btn.disabled = false;
    }
  } catch (e) {
    btn.disabled = false;
    showToast('Errore di rete durante il ripristino del backup.', true);
  }
}

async function factoryResetDevice() {
  if (!confirm('ATTENZIONE: Vuoi davvero cancellare tutte le impostazioni (Wi-Fi, Hotspot, Password, Orari, Logo) e tornare ai valori di fabbrica?')) {
    return;
  }
  if (!confirm('Conferma definitiva: il dispositivo verrà ripristinato e riavviato immediatamente. Procedere?')) {
    return;
  }
  const btn = document.getElementById('btnFactoryReset');
  btn.disabled = true;
  try {
    const r = await fetch('/api/factory-reset', {method: 'POST'});
    const d = await r.json();
    showToast(d.message, !r.ok);
    if (r.ok) {
      setTimeout(() => { window.location.href = '/login'; }, 6000);
    } else {
      btn.disabled = false;
    }
  } catch (e) {
    btn.disabled = false;
    showToast('Errore di rete durante il ripristino.', true);
  }
}

async function saveSuperPassword() {
  const cur = document.getElementById('curSuperPwd').value;
  const nw = document.getElementById('newSuperPwd').value;
  const cfm = document.getElementById('cfmSuperPwd').value;
  if (!cur) { showToast('Inserisci la Super Password attuale.', true); return; }
  if (nw.length < 8 || nw.length > 63) { showToast('La nuova Super Password deve avere tra 8 e 63 caratteri.', true); return; }
  if (nw !== cfm) { showToast('Le due nuove Super Password non coincidono.', true); return; }
  try {
    const r = await fetch('/api/super-password', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({
        current_super_password: cur,
        new_super_password: nw,
        confirm_super_password: cfm
      })
    });
    const d = await r.json().catch(() => ({}));
    showToast(d.message || (r.ok ? 'Super Password aggiornata!' : 'Errore aggiornamento Super Password'), !r.ok);
    if (r.ok) {
      setTimeout(() => { window.location.href = '/login'; }, 1600);
    }
  } catch (e) {
    showToast('Errore di comunicazione con il dispositivo.', true);
  }
}

async function factoryResetFullDevice() {
  const sp = document.getElementById('fullResetSuperPwd').value;
  if (!sp) { showToast('Inserisci la Super Password Tecnico per confermare il Reset Totale.', true); return; }
  if (!confirm('ATTENZIONE MASSIMA: Questo cancellerà TUTTO, inclusa la Super Password Tecnico dalla NVS!')) return;
  if (!confirm('Conferma finale: ripristinare completamente la scheda allo stato vergine e riavviarla?')) return;
  const btn = document.getElementById('btnFactoryResetFull');
  btn.disabled = true;
  try {
    const r = await fetch('/api/factory-reset-full', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({current_super_password: sp})
    });
    const d = await r.json().catch(() => ({}));
    showToast(d.message || 'Operazione completata', !r.ok);
    if (r.ok) {
      setTimeout(() => { window.location.href = '/login'; }, 6000);
    } else {
      btn.disabled = false;
    }
  } catch (e) {
    btn.disabled = false;
    showToast('Errore di rete durante il Reset Totale.', true);
  }
}

loadConfig();
fetchStatus();
setInterval(fetchStatus, 1000);
</script>
</body>
</html>
)rawliteral";

static const char SETUP_SUPER_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="it">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ChronoRing - Configurazione Iniziale Tecnico</title>
<style>
  :root{--bg:#060d1f;--card:#0d1b36;--cyan:#00d4ff;--gold:#f59e0b;--text:#e2e8f0;--muted:#94a3b8;--danger:#ef4444;}
  *{box-sizing:border-box;margin:0;padding:0;font-family:'Segoe UI',system-ui,sans-serif;}
  body{background:radial-gradient(circle at center,#0f2447 0%,#050b18 100%);color:var(--text);min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px;}
  .setup-card{background:var(--card);border:1px solid rgba(245,158,11,0.4);border-radius:18px;padding:30px 26px;width:100%;max-width:460px;box-shadow:0 0 38px rgba(0,212,255,0.14);}
  h1{font-size:1.35rem;letter-spacing:1.2px;text-transform:uppercase;color:var(--gold);margin-bottom:8px;text-align:center;}
  p.sub{color:var(--muted);font-size:0.86rem;line-height:1.45;margin-bottom:20px;text-align:center;}
  .note{background:rgba(0,212,255,0.08);border:1px solid rgba(0,212,255,0.28);border-radius:10px;padding:11px 13px;font-size:0.8rem;color:#bae6fd;line-height:1.45;margin-bottom:18px;}
  .input-group{margin-bottom:15px;}
  label{display:block;font-size:0.8rem;color:var(--cyan);margin-bottom:6px;font-weight:600;}
  input{width:100%;padding:11px 13px;border-radius:10px;border:1px solid rgba(0,212,255,0.3);background:#071022;color:#fff;font-size:0.96rem;outline:none;}
  input:focus{border-color:var(--cyan);box-shadow:0 0 10px rgba(0,212,255,0.25);}
  button{width:100%;padding:13px;border:none;border-radius:10px;background:linear-gradient(135deg,#d97706,#fbbf24);color:#1e1103;font-weight:800;font-size:0.96rem;cursor:pointer;margin-top:6px;}
  button:hover{filter:brightness(1.08);}
  .err{color:#f87171;font-size:0.85rem;margin-top:12px;min-height:20px;text-align:center;}
</style>
</head>
<body>
<div class="setup-card">
  <h1>🛡️ Configurazione Iniziale Tecnico</h1>
  <p class="sub">Prima configurazione di sicurezza (<strong>ChronoRing v)rawliteral" CHRONORING_FW_VERSION R"rawliteral(</strong>)</p>
  <div class="note">
    Imposta la tua <strong>Super Password personale da Tecnico</strong> per garantire il recupero di emergenza qualora la scuola dimentichi la password.<br>
    • Salvata in area isolata <strong>NVS (SHA-256 + Salt)</strong>.<br>
    • Non viene inclusa nei backup e sopravvive al Factory Reset della scuola.<br>
    • Dopo il salvataggio questa schermata si <strong>autoblocca per sempre</strong>.
  </div>
  <form id="superSetupForm">
    <div class="input-group">
      <label for="superPwd">SUPER PASSWORD TECNICO (min 8 caratteri) *</label>
      <input type="password" id="superPwd" minlength="8" maxlength="63" placeholder="Inserisci la tua Super Password..." required autofocus>
    </div>
    <div class="input-group">
      <label for="superPwdConfirm">CONFERMA SUPER PASSWORD TECNICO *</label>
      <input type="password" id="superPwdConfirm" minlength="8" maxlength="63" placeholder="Ripeti Super Password..." required>
    </div>
    <div class="input-group">
      <label for="schoolPwd">PASSWORD GESTORI SCUOLA (opzionale, min 4 caratteri)</label>
      <input type="password" id="schoolPwd" maxlength="31" placeholder="Lascia vuoto per mantenere l'attuale (consigliato cambiarla)">
    </div>
    <button type="submit" id="btnSaveSetup">🔒 SALVA E ATTIVA MODALITÀ TECNICO</button>
    <div class="err" id="errMsg"></div>
  </form>
</div>
<script>
document.getElementById('superSetupForm').addEventListener('submit', async (e) => {
  e.preventDefault();
  const err = document.getElementById('errMsg');
  err.textContent = '';
  const superPwd = document.getElementById('superPwd').value;
  const confirmPwd = document.getElementById('superPwdConfirm').value;
  const schoolPwd = document.getElementById('schoolPwd').value;
  if (superPwd.length < 8 || superPwd.length > 63) {
    err.textContent = 'La Super Password deve avere tra 8 e 63 caratteri.';
    return;
  }
  if (superPwd !== confirmPwd) {
    err.textContent = 'Le due Super Password non coincidono.';
    return;
  }
  if (schoolPwd.length > 0 && (schoolPwd.length < 4 || schoolPwd.length > 31)) {
    err.textContent = 'La Password Scuola deve avere tra 4 e 31 caratteri (oppure resta vuota).';
    return;
  }
  if (schoolPwd.length > 0 && schoolPwd === superPwd) {
    err.textContent = 'La Password Scuola deve essere diversa dalla Super Password.';
    return;
  }
  const btn = document.getElementById('btnSaveSetup');
  btn.disabled = true;
  try {
    const res = await fetch('/api/super-setup', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({
        super_password: superPwd,
        super_password_confirm: confirmPwd,
        school_password: schoolPwd
      })
    });
    const d = await res.json().catch(() => ({}));
    if (res.ok) {
      window.location.href = '/';
    } else {
      err.textContent = d.message || 'Errore durante la configurazione.';
      btn.disabled = false;
    }
  } catch (ex) {
    err.textContent = 'Errore di comunicazione con il dispositivo.';
    btn.disabled = false;
  }
});
</script>
</body>
</html>
)rawliteral";

static const char OTA_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="it">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ChronoRing - Aggiornamento Firmware OTA</title>
<style>
  :root{--bg:#060d1f;--card:#0d1b36;--cyan:#00d4ff;--gold:#f59e0b;--text:#e2e8f0;--muted:#94a3b8;}
  *{box-sizing:border-box;margin:0;padding:0;font-family:'Segoe UI',system-ui,sans-serif;}
  body{background:#060d1f;color:var(--text);min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px;}
  .ota-card{background:var(--card);border:1px solid rgba(0,212,255,0.28);border-radius:16px;padding:30px;width:100%;max-width:480px;text-align:center;box-shadow:0 10px 35px rgba(0,0,0,0.45);}
  h1{color:var(--cyan);font-size:1.35rem;margin-bottom:8px;text-transform:uppercase;letter-spacing:1px;}
  p{color:var(--muted);font-size:0.88rem;margin-bottom:20px;}
  .file-box{border:2px dashed rgba(0,212,255,0.4);border-radius:12px;padding:24px;margin-bottom:18px;background:#071022;}
  input[type="file"]{color:var(--text);width:100%;}
  .progress-wrap{display:none;margin:18px 0;}
  .progress-bar{width:100%;height:18px;background:#071022;border-radius:99px;overflow:hidden;border:1px solid rgba(0,212,255,0.3);}
  .progress-fill{height:100%;width:0%;background:linear-gradient(90deg,#0284c7,#00d4ff,#10b981);transition:width 0.15s;}
  .btn{padding:12px 18px;border-radius:10px;border:none;font-weight:700;cursor:pointer;text-decoration:none;display:inline-block;font-size:0.95rem;}
  .btn-cyan{background:linear-gradient(135deg,#0284c7,#00d4ff);color:#041022;width:100%;margin-bottom:12px;}
  .btn-back{color:var(--muted);font-size:0.86rem;}
  #statusMsg{margin-top:12px;font-weight:600;font-size:0.92rem;}
</style>
</head>
<body>
<div class="ota-card">
  <h1>⬆️ Aggiornamento Firmware OTA</h1>
  <p>Versione attualmente installata: <strong style="color:var(--cyan)">v)rawliteral" CHRONORING_FW_VERSION R"rawliteral(</strong></p>
  <p>Seleziona il file compilato <code>firmware.bin</code> per aggiornare la partizione OTA di <strong>ChronoRing</strong>.</p>
  <form id="otaForm">
    <div class="file-box">
      <input type="file" id="fwFile" name="update" accept=".bin" required>
    </div>
    <div class="progress-wrap" id="progWrap">
      <div class="progress-bar"><div class="progress-fill" id="progFill"></div></div>
      <div id="progText" style="margin-top:6px;font-size:0.85rem;color:var(--cyan)">0%</div>
    </div>
    <button type="submit" class="btn btn-cyan" id="uploadBtn">AVVIA AGGIORNAMENTO FIRMWARE</button>
  </form>
  <div id="statusMsg"></div>
  <div style="margin-top:16px;"><a href="/" class="btn-back">← Torna alla Dashboard</a></div>
</div>
<script>
document.getElementById('otaForm').addEventListener('submit', (e) => {
  e.preventDefault();
  const fileInput = document.getElementById('fwFile');
  if (!fileInput.files.length) return;
  const file = fileInput.files[0];
  const formData = new FormData();
  formData.append('update', file, file.name);

  document.getElementById('uploadBtn').disabled = true;
  document.getElementById('progWrap').style.display = 'block';
  const status = document.getElementById('statusMsg');
  status.style.color = '#00d4ff';
  status.textContent = 'Caricamento e scrittura flash in corso... Non spegnere il dispositivo!';

  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/update', true);
  xhr.upload.onprogress = (ev) => {
    if (ev.lengthComputable) {
      const pct = Math.round((ev.loaded / ev.total) * 100);
      document.getElementById('progFill').style.width = pct + '%';
      document.getElementById('progText').textContent = pct + '% (' + Math.round(ev.loaded/1024) + ' KB)';
    }
  };
  xhr.onload = () => {
    if (xhr.status === 200) {
      status.style.color = '#10b981';
      status.textContent = '✅ Aggiornamento completato! Riavvio di ChronoRing in corso...';
      setTimeout(() => { window.location.href = '/'; }, 8000);
    } else {
      status.style.color = '#ef4444';
      status.textContent = '❌ Errore OTA: ' + xhr.responseText;
      document.getElementById('uploadBtn').disabled = false;
    }
  };
  xhr.onerror = () => {
    status.style.color = '#ef4444';
    status.textContent = '❌ Errore di rete durante il caricamento.';
    document.getElementById('uploadBtn').disabled = false;
  };
  xhr.send(formData);
});
</script>
</body>
</html>
)rawliteral";

// ============================================================================
// IMPLEMENTAZIONE METODI WEBSERVERHANDLER
// ============================================================================

void WebServerHandler::generateNewSessionToken() {
    snprintf(_sessionToken, sizeof(_sessionToken), "CR_%08lX%08lX",
             (unsigned long)esp_random(), (unsigned long)millis());
}

WebServerHandler::AuthRole WebServerHandler::getAuthRole(AsyncWebServerRequest* request) {
    if (!request || _sessionRole == AuthRole::NONE || strlen(_sessionToken) == 0) {
        return AuthRole::NONE;
    }
    if (!request->hasHeader("Cookie")) {
        return AuthRole::NONE;
    }
    String cookie = request->header("Cookie");
    String expected = String("CHRONORING_SESSION=") + _sessionToken;
    return (cookie.indexOf(expected) != -1) ? _sessionRole : AuthRole::NONE;
}

bool WebServerHandler::isAuthenticated(AsyncWebServerRequest* request) {
    return getAuthRole(request) != AuthRole::NONE;
}

bool WebServerHandler::isSuperAdmin(AsyncWebServerRequest* request) {
    return getAuthRole(request) == AuthRole::SUPERADMIN;
}

void WebServerHandler::scheduleRestart(uint32_t delayMs) {
    static esp_timer_handle_t restartTimer = nullptr;
    if (restartTimer == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = [](void*) {
            RelayController::forceOff();
            ESP.restart();
        };
        args.name = "ota_restart_tmr";
        esp_timer_create(&args, &restartTimer);
    }
    esp_timer_start_once(restartTimer, static_cast<uint64_t>(delayMs) * 1000ULL);
}

void WebServerHandler::begin() {
    generateNewSessionToken();
    _sessionRole = AuthRole::NONE;

    setupRoutes();
    _server.begin();
    Serial.println("[WEB] Web Server ChronoRing attivo sulla porta 80.");
}

void WebServerHandler::setupRoutes() {
    // 1. PAGINA DI LOGIN
    _server.on("/login", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (isAuthenticated(request)) {
            request->redirect(ConfigManager::isSuperPasswordConfigured() ? "/" : "/setup-super");
            return;
        }
        request->send(200, "text/html; charset=utf-8", LOGIN_HTML);
    });

    // 2. LOGOUT
    _server.on("/logout", HTTP_GET, [](AsyncWebServerRequest* request) {
        generateNewSessionToken();
        _sessionRole = AuthRole::NONE;
        AsyncWebServerResponse* response = request->beginResponse(302, "text/plain", "Redirecting...");
        response->addHeader("Location", "/login");
        response->addHeader("Set-Cookie", "CHRONORING_SESSION=; Path=/; Max-Age=0");
        request->send(response);
    });

    // 3. DASHBOARD PRINCIPALE (Protetta + controllo Wizard iniziale una tantum)
    _server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (!isAuthenticated(request)) {
            request->redirect("/login");
            return;
        }
        if (!ConfigManager::isSuperPasswordConfigured()) {
            request->redirect("/setup-super");
            return;
        }
        request->send(200, "text/html; charset=utf-8", DASHBOARD_HTML);
    });

    // 3b. WIZARD UNA TANTUM CONFIGURAZIONE SUPER PASSWORD TECNICO (/setup-super)
    _server.on("/setup-super", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (!isAuthenticated(request)) {
            request->redirect("/login");
            return;
        }
        if (ConfigManager::isSuperPasswordConfigured()) {
            request->redirect("/");
            return;
        }
        request->send(200, "text/html; charset=utf-8", SETUP_SUPER_HTML);
    });

    // 4. API LOGIN A DOPPIO RUOLO CON ANTI BRUTE-FORCE (POST JSON)
    _server.on("/api/login", HTTP_POST,
        [](AsyncWebServerRequest* request) {},
        nullptr,
        [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
            uint32_t now = millis();
            if (_loginLockUntilMs != 0 && static_cast<int32_t>(_loginLockUntilMs - now) > 0) {
                uint32_t remSec = (_loginLockUntilMs - now + 999UL) / 1000UL;
                char msg[112];
                snprintf(msg, sizeof(msg),
                         "{\"error\":\"Troppi tentativi falliti. Riprova tra %lu secondi.\"}",
                         (unsigned long)remSec);
                request->send(429, "application/json", msg);
                return;
            }
            if (index != 0 || len != total || total > 1024) {
                request->send(400, "application/json", "{\"error\":\"Payload non valido\"}");
                return;
            }

            JsonDocument doc;
            if (deserializeJson(doc, data, len)) {
                request->send(400, "application/json", "{\"error\":\"JSON non valido\"}");
                return;
            }
            const char* pwd = doc["password"] | "";

            AuthRole matchedRole = AuthRole::NONE;
            if (ConfigManager::checkSuperPassword(pwd)) {
                matchedRole = AuthRole::SUPERADMIN;
            } else if (ConfigManager::checkWebPassword(pwd)) {
                matchedRole = AuthRole::ADMIN;
            }

            if (matchedRole != AuthRole::NONE) {
                _failedLoginAttempts = 0;
                _loginLockUntilMs = 0;
                generateNewSessionToken();
                _sessionRole = matchedRole;

                bool needsSetup = !ConfigManager::isSuperPasswordConfigured();
                JsonDocument resDoc;
                resDoc["status"] = "ok";
                resDoc["role"] = (matchedRole == AuthRole::SUPERADMIN) ? "superadmin" : "admin";
                resDoc["needs_super_setup"] = needsSetup;

                String out;
                serializeJson(resDoc, out);
                AsyncWebServerResponse* response = request->beginResponse(200, "application/json", out);
                String cookieHeader = String("CHRONORING_SESSION=") + _sessionToken + "; Path=/; HttpOnly; SameSite=Lax";
                response->addHeader("Set-Cookie", cookieHeader);
                request->send(response);
                Serial.printf("[WEB] Login effettuato con ruolo: %s.\n",
                              (matchedRole == AuthRole::SUPERADMIN) ? "SUPERADMIN (Tecnico)" : "ADMIN (Scuola)");
            } else {
                if (_failedLoginAttempts < 255) {
                    _failedLoginAttempts++;
                }
                if (_failedLoginAttempts >= MAX_FAILED_LOGIN_ATTEMPTS) {
                    _loginLockUntilMs = now + LOGIN_LOCKOUT_MS;
                    _failedLoginAttempts = 0;
                    Serial.printf("[WEB] Anti brute-force: login bloccato per %u s dopo %u tentativi errati.\n",
                                  (unsigned)(LOGIN_LOCKOUT_MS / 1000), MAX_FAILED_LOGIN_ATTEMPTS);
                    request->send(429, "application/json",
                                  "{\"error\":\"Troppi tentativi falliti. Accesso bloccato per 60 secondi.\"}");
                } else {
                    Serial.printf("[WEB] Tentativo di login fallito (%u/%u).\n",
                                  _failedLoginAttempts, MAX_FAILED_LOGIN_ATTEMPTS);
                    request->send(401, "application/json", "{\"error\":\"Password errata\"}");
                }
            }
        }
    );

    // 4b. API PROVISIONING UNA TANTUM SUPER PASSWORD TECNICO (POST /api/super-setup)
    _server.on("/api/super-setup", HTTP_POST,
        [](AsyncWebServerRequest* request) {},
        nullptr,
        [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
            if (!isAuthenticated(request)) {
                request->send(401, "application/json", "{\"message\":\"Non autorizzato\"}");
                return;
            }
            if (ConfigManager::isSuperPasswordConfigured()) {
                request->send(403, "application/json",
                              "{\"message\":\"Super Password già configurata: wizard bloccato permanentemente.\"}");
                return;
            }
            if (index != 0 || len != total || total > 1024) {
                request->send(400, "application/json", "{\"message\":\"Payload non valido\"}");
                return;
            }

            JsonDocument doc;
            if (deserializeJson(doc, data, len)) {
                request->send(400, "application/json", "{\"message\":\"JSON non valido\"}");
                return;
            }

            const char* superPwd = doc["super_password"] | "";
            const char* cfmPwd   = doc["super_password_confirm"] | "";
            const char* schoolPwd = doc["school_password"] | "";

            size_t spLen = strlen(superPwd);
            if (spLen < ConfigManager::SUPER_PWD_MIN_LEN || spLen > ConfigManager::SUPER_PWD_MAX_LEN) {
                request->send(400, "application/json",
                              "{\"message\":\"La Super Password deve contenere tra 8 e 63 caratteri.\"}");
                return;
            }
            if (strcmp(superPwd, cfmPwd) != 0) {
                request->send(400, "application/json", "{\"message\":\"Le due Super Password non coincidono.\"}");
                return;
            }

            size_t schLen = strlen(schoolPwd);
            if (schLen > 0) {
                if (schLen < 4 || schLen > 31) {
                    request->send(400, "application/json",
                                  "{\"message\":\"La password scuola deve contenere tra 4 e 31 caratteri.\"}");
                    return;
                }
                if (strcmp(schoolPwd, superPwd) == 0) {
                    request->send(400, "application/json",
                                  "{\"message\":\"La password scuola deve essere diversa dalla Super Password.\"}");
                    return;
                }
            }

            if (!ConfigManager::setSuperPassword(superPwd)) {
                request->send(500, "application/json", "{\"message\":\"Errore salvataggio Super Password in NVS.\"}");
                return;
            }
            if (schLen > 0 && !ConfigManager::setWebPassword(schoolPwd)) {
                request->send(500, "application/json", "{\"message\":\"Super Password salvata, ma errore aggiornamento password scuola.\"}");
                return;
            }

            _sessionRole = AuthRole::SUPERADMIN;
            request->send(200, "application/json", "{\"message\":\"✅ Configurazione Tecnico completata!\"}");
        }
    );

    // 5. API STATO IN TEMPO REALE (GET /api/status)
    _server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (!isAuthenticated(request)) {
            request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
            return;
        }
        JsonDocument doc;
        doc["role"] = isSuperAdmin(request) ? "superadmin" : "admin";
        doc["time"] = NetworkManager::getFormattedTime();
        doc["date"] = NetworkManager::getFormattedDate();
        doc["ntp_synced"] = NetworkManager::isTimeSynced();
        doc["next_bell"] = Scheduler::getNextBellString();
        doc["last_bell"] = Scheduler::getLastTriggeredString();
        doc["relay_active"] = RelayController::isActive();
        doc["relay_pin"] = RELAY_PIN;
        doc["has_display"] = g_has_display;
        doc["fw_version"] = CHRONORING_FW_VERSION;
        doc["suspended"] = ConfigManager::isSuspended();
        doc["net_mode"] = NetworkManager::getNetworkModeString();
        doc["ip"] = NetworkManager::getIpAddress();
        doc["rssi"] = NetworkManager::getRssi();

        String out;
        serializeJson(doc, out);
        request->send(200, "application/json", out);
    });

    // 6. API LETTURA CONFIGURAZIONE COMPLETA (GET /api/config)
    _server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (!isAuthenticated(request)) {
            request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
            return;
        }
        SystemConfig cfg = ConfigManager::getConfig();
        JsonDocument doc;
        doc["role"] = isSuperAdmin(request) ? "superadmin" : "admin";
        doc["wifi_ssid"] = cfg.wifiSsid;
        doc["ap_ssid"] = (strlen(cfg.apSsid) > 0) ? cfg.apSsid : ConfigManager::DEFAULT_AP_SSID;
        doc["ap_has_password"] = (strlen(cfg.apPassword) >= 8);
        doc["school_name"] = cfg.schoolName;
        doc["has_school_logo"] = ConfigManager::hasSchoolLogo();
        doc["relay_pulse_ms"] = cfg.relayPulseMs;
        doc["suspended"] = cfg.suspended;
        doc["display_timeout_sec"] = cfg.displayTimeoutSec;
        doc["display_sleep_off"] = cfg.displaySleepOff;
        doc["ap_timeout_min"] = cfg.apTimeoutMin;
        doc["ap_lock_after_sta"] = cfg.apLockAfterSta;

        JsonArray arr = doc["schedule"].to<JsonArray>();
        for (const auto& item : cfg.schedule) {
            JsonObject obj = arr.add<JsonObject>();
            char tBuf[6];
            snprintf(tBuf, sizeof(tBuf), "%02u:%02u", item.hour, item.minute);
            obj["time"] = tBuf;
            obj["days_mask"] = item.daysMask;
            obj["enabled"] = item.enabled;
            obj["label"] = item.label;
        }

        String out;
        serializeJson(doc, out);
        request->send(200, "application/json", out);
    });

    // 7. API SALVATAGGIO NOME ISTITUTO (POST /api/school-info)
    _server.on("/api/school-info", HTTP_POST,
        [](AsyncWebServerRequest* request) {},
        nullptr,
        [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
            if (!isAuthenticated(request)) {
                request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
                return;
            }
            JsonDocument doc;
            if (deserializeJson(doc, data, len)) {
                request->send(400, "application/json", "{\"message\":\"JSON non valido\"}");
                return;
            }
            const char* name = doc["school_name"] | "";
            if (ConfigManager::setSchoolName(name)) {
                if (g_has_display) {
                    GuiManager::requestBrandingReload();
                }
                request->send(200, "application/json", "{\"message\":\"✅ Nome Istituto salvato correttamente!\"}");
            } else {
                request->send(500, "application/json", "{\"message\":\"Errore salvataggio Nome Istituto\"}");
            }
        }
    );

    // 8. API UPLOAD E RIMOZIONE LOGO SCUOLA (/api/school-logo)
    _server.on("/api/school-logo", HTTP_POST,
        [](AsyncWebServerRequest* request) {
            if (!isAuthenticated(request)) {
                request->send(401, "application/json", "{\"message\":\"Non autorizzato\"}");
                return;
            }
            if (g_has_display) {
                GuiManager::requestBrandingReload();
            }
            request->send(200, "application/json", "{\"message\":\"✅ Logo della scuola salvato correttamente!\"}");
        },
        [](AsyncWebServerRequest* request, String filename, size_t index, uint8_t* data, size_t len, bool final) {
            if (!isAuthenticated(request)) return;
            static File logoFile;
            if (index == 0) {
                logoFile = LittleFS.open(ConfigManager::SCHOOL_LOGO_PATH, FILE_WRITE);
            }
            if (logoFile && len > 0) {
                logoFile.write(data, len);
            }
            if (final && logoFile) {
                logoFile.close();
                Serial.printf("[WEB] Logo scuola salvato su %s (%u byte).\n",
                              ConfigManager::SCHOOL_LOGO_PATH, (unsigned)(index + len));
            }
        }
    );

    _server.on("/api/school-logo", HTTP_DELETE, [](AsyncWebServerRequest* request) {
        if (!isAuthenticated(request)) {
            request->send(401, "application/json", "{\"message\":\"Non autorizzato\"}");
            return;
        }
        if (ConfigManager::removeSchoolLogo()) {
            if (g_has_display) {
                GuiManager::requestBrandingReload();
            }
            request->send(200, "application/json", "{\"message\":\"🗑️ Logo scuola rimosso.\"}");
        } else {
            request->send(500, "application/json", "{\"message\":\"Errore rimozione logo\"}");
        }
    });

    // 9. API SUONA ORA MANUALE (POST /api/ring)
    _server.on("/api/ring", HTTP_POST, [](AsyncWebServerRequest* request) {
        if (!isAuthenticated(request)) {
            request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
            return;
        }
        uint32_t pulseMs = ConfigManager::getRelayPulseMs();
        if (RelayController::trigger(pulseMs)) {
            char msg[80];
            snprintf(msg, sizeof(msg), "{\"message\":\"🔔 Campanella attivata manualmente per %u ms!\"}", pulseMs);
            request->send(200, "application/json", msg);
        } else {
            request->send(409, "application/json", "{\"message\":\"Relè già attivo in questo momento.\"}");
        }
    });

    // 10. API TOGGLE SOSPENSIONE GLOBALE (POST /api/toggle-suspend)
    _server.on("/api/toggle-suspend", HTTP_POST,
        [](AsyncWebServerRequest* request) {},
        nullptr,
        [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
            if (!isAuthenticated(request)) {
                request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
                return;
            }
            JsonDocument doc;
            if (deserializeJson(doc, data, len)) {
                request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                return;
            }
            bool susp = doc["suspended"] | false;
            ConfigManager::setSuspended(susp);
            if (g_has_display) {
                gui_update_state(susp);
            }
            request->send(200, "application/json", "{\"status\":\"ok\"}");
        }
    );

    // 11. API SALVATAGGIO TABELLA ORARI (POST /api/schedule)
    static std::string s_schedBuf;
    _server.on("/api/schedule", HTTP_POST,
        [](AsyncWebServerRequest* request) {},
        nullptr,
        [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
            if (!isAuthenticated(request)) {
                s_schedBuf.clear();
                request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
                return;
            }
            if (index == 0) {
                s_schedBuf.clear();
            }
            if (s_schedBuf.size() + len > 32768) {
                s_schedBuf.clear();
                request->send(413, "application/json", "{\"message\":\"Tabella orari troppo grande\"}");
                return;
            }
            if (len > 0) {
                s_schedBuf.append(reinterpret_cast<const char*>(data), len);
            }
            if (index + len < total) {
                return; // Attende gli altri chunk TCP del body JSON
            }

            JsonDocument doc;
            DeserializationError jsonErr = deserializeJson(doc, s_schedBuf);
            s_schedBuf.clear();
            s_schedBuf.shrink_to_fit();

            if (jsonErr || !doc["schedule"].is<JsonArray>()) {
                request->send(400, "application/json", "{\"message\":\"Formato orari non valido\"}");
                return;
            }

            SystemConfig cfg = ConfigManager::getConfig();
            cfg.schedule.clear();

            JsonArray arr = doc["schedule"].as<JsonArray>();
            for (JsonObject obj : arr) {
                const char* timeStr = obj["time"] | "";
                int h = -1, m = -1;
                if (sscanf(timeStr, "%d:%d", &h, &m) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60) {
                    BellScheduleItem item;
                    item.hour = static_cast<uint8_t>(h);
                    item.minute = static_cast<uint8_t>(m);
                    item.daysMask = obj["days_mask"].is<int>()
                                        ? static_cast<uint8_t>(obj["days_mask"].as<int>() & 0x7F)
                                        : ConfigManager::DAYS_MON_TO_SAT_MASK;
                    item.enabled = obj["enabled"] | true;
                    String lblStr = String(obj["label"] | "Campanella");
                    lblStr.trim();
                    const char* finalLbl = lblStr.length() > 0 ? lblStr.c_str() : "Campanella";
                    strlcpy(item.label, finalLbl, sizeof(item.label));
                    cfg.schedule.push_back(item);
                }
            }

            if (ConfigManager::saveConfig(cfg)) {
                request->send(200, "application/json", "{\"message\":\"✅ Programmazione oraria salvata su LittleFS!\"}");
            } else {
                request->send(500, "application/json", "{\"message\":\"Errore salvataggio su LittleFS\"}");
            }
        }
    );

    // 12. API SALVATAGGIO DURATA IMPULSO RELÈ (POST /api/settings)
    _server.on("/api/settings", HTTP_POST,
        [](AsyncWebServerRequest* request) {},
        nullptr,
        [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
            if (!isAuthenticated(request)) {
                request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
                return;
            }
            JsonDocument doc;
            if (deserializeJson(doc, data, len)) {
                request->send(400, "application/json", "{\"message\":\"JSON non valido\"}");
                return;
            }
            uint32_t pulse = doc["relay_pulse_ms"] | ConfigManager::DEFAULT_RELAY_PULSE_MS;
            if (pulse > ConfigManager::MAX_RELAY_PULSE_MS) {
                pulse = ConfigManager::MAX_RELAY_PULSE_MS;
            }
            if (ConfigManager::setRelayPulseMs(pulse)) {
                char msg[96];
                snprintf(msg, sizeof(msg), "{\"message\":\"✅ Durata squillo salvata (%u ms)\"}", ConfigManager::getRelayPulseMs());
                request->send(200, "application/json", msg);
            } else {
                request->send(500, "application/json", "{\"message\":\"Errore salvataggio\"}");
            }
        }
    );

    // 13. API MODIFICA PASSWORD WEB SCUOLA (POST /api/password)
    _server.on("/api/password", HTTP_POST,
        [](AsyncWebServerRequest* request) {},
        nullptr,
        [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
            if (!isAuthenticated(request)) {
                request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
                return;
            }
            if (index != 0 || len != total || total > 1024) {
                request->send(400, "application/json", "{\"message\":\"Payload non valido\"}");
                return;
            }
            JsonDocument doc;
            if (deserializeJson(doc, data, len)) {
                request->send(400, "application/json", "{\"message\":\"JSON non valido\"}");
                return;
            }
            const char* newPwd = doc["password"] | "";
            size_t pwdLen = strlen(newPwd);
            if (pwdLen < 3 || pwdLen > 31) {
                request->send(400, "application/json", "{\"message\":\"La password deve essere tra 3 e 31 caratteri\"}");
                return;
            }
            if (ConfigManager::setWebPassword(newPwd)) {
                if (isSuperAdmin(request)) {
                    // Il tecnico mantiene la sessione attiva dopo aver reimpostato la password della scuola
                    request->send(200, "application/json",
                                  "{\"message\":\"✅ Password scuola aggiornata! Sessione tecnico mantenuta.\",\"keep_session\":true}");
                } else {
                    generateNewSessionToken();
                    _sessionRole = AuthRole::NONE;
                    request->send(200, "application/json",
                                  "{\"message\":\"Password aggiornata\",\"keep_session\":false}");
                }
            } else {
                request->send(500, "application/json", "{\"message\":\"Errore salvataggio password\"}");
            }
        }
    );

    // 13b. API CAMBIO SUPER PASSWORD TECNICO IN NVS (POST /api/super-password - SOLO SUPERADMIN)
    _server.on("/api/super-password", HTTP_POST,
        [](AsyncWebServerRequest* request) {},
        nullptr,
        [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
            if (!isSuperAdmin(request)) {
                request->send(403, "application/json", "{\"message\":\"Accesso riservato alla modalità Tecnico.\"}");
                return;
            }
            if (index != 0 || len != total || total > 1024) {
                request->send(400, "application/json", "{\"message\":\"Payload non valido\"}");
                return;
            }
            JsonDocument doc;
            if (deserializeJson(doc, data, len)) {
                request->send(400, "application/json", "{\"message\":\"JSON non valido\"}");
                return;
            }
            const char* curPwd = doc["current_super_password"] | "";
            const char* newPwd = doc["new_super_password"] | "";
            const char* cfmPwd = doc["confirm_super_password"] | "";

            if (!ConfigManager::checkSuperPassword(curPwd)) {
                request->send(403, "application/json", "{\"message\":\"Super Password attuale errata.\"}");
                return;
            }
            size_t newLen = strlen(newPwd);
            if (newLen < ConfigManager::SUPER_PWD_MIN_LEN || newLen > ConfigManager::SUPER_PWD_MAX_LEN) {
                request->send(400, "application/json",
                              "{\"message\":\"La nuova Super Password deve contenere tra 8 e 63 caratteri.\"}");
                return;
            }
            if (strcmp(newPwd, cfmPwd) != 0) {
                request->send(400, "application/json", "{\"message\":\"Le due nuove Super Password non coincidono.\"}");
                return;
            }
            if (ConfigManager::setSuperPassword(newPwd)) {
                generateNewSessionToken();
                _sessionRole = AuthRole::NONE;
                request->send(200, "application/json",
                              "{\"message\":\"✅ Super Password NVS aggiornata! Effettua nuovamente il login.\"}");
            } else {
                request->send(500, "application/json", "{\"message\":\"Errore salvataggio Super Password in NVS.\"}");
            }
        }
    );

    // 14. API SCANSIONE RETI WI-FI CIRCOSTANTI (GET /api/wifi-scan)
    _server.on("/api/wifi-scan", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (!isAuthenticated(request)) {
            request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
            return;
        }
        String json = NetworkManager::getOrStartWifiScanJson();
        request->send(200, "application/json", json);
    });

    // 15. API CONFIGURAZIONE CREDENZIALI WI-FI (POST /api/wifi)
    _server.on("/api/wifi", HTTP_POST,
        [](AsyncWebServerRequest* request) {},
        nullptr,
        [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
            if (!isAuthenticated(request)) {
                request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
                return;
            }
            JsonDocument doc;
            if (deserializeJson(doc, data, len)) {
                request->send(400, "application/json", "{\"message\":\"JSON non valido\"}");
                return;
            }
            const char* ssid = doc["ssid"] | "";
            const char* pwd  = doc["password"] | "";

            SystemConfig current = ConfigManager::getConfig();
            const char* finalPwd = (strlen(pwd) > 0) ? pwd : current.wifiPassword;

            if (ConfigManager::setWiFiCredentials(ssid, finalPwd)) {
                NetworkManager::requestReconnect();
                request->send(200, "application/json", "{\"message\":\"Credenziali Wi-Fi salvate. Riconnessione in corso...\"}");
            } else {
                request->send(500, "application/json", "{\"message\":\"Errore salvataggio Wi-Fi\"}");
            }
        }
    );

    // 16. API CONFIGURAZIONE SSID & PASSWORD HOTSPOT ACCESS POINT (POST /api/ap-config)
    _server.on("/api/ap-config", HTTP_POST,
        [](AsyncWebServerRequest* request) {},
        nullptr,
        [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
            if (!isAuthenticated(request)) {
                request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
                return;
            }
            JsonDocument doc;
            if (deserializeJson(doc, data, len)) {
                request->send(400, "application/json", "{\"message\":\"JSON non valido\"}");
                return;
            }
            const char* apSsid = doc["ap_ssid"] | "";
            const char* apPwd  = doc["ap_password"] | "";

            size_t pwdLen = strlen(apPwd);
            if (strlen(apSsid) == 0 || strlen(apSsid) > 31) {
                request->send(400, "application/json", "{\"message\":\"SSID Hotspot non valido (1-31 caratteri).\"}");
                return;
            }
            if (pwdLen > 0 && pwdLen < 8) {
                request->send(400, "application/json", "{\"message\":\"La password WPA2 deve contenere almeno 8 caratteri (o resta vuota).\"}");
                return;
            }

            if (ConfigManager::setApCredentials(apSsid, apPwd)) {
                NetworkManager::requestApRestart();
                request->send(200, "application/json",
                              pwdLen >= 8 ? "{\"message\":\"✅ Hotspot AP salvato e protetto (WPA2)!\"}"
                                          : "{\"message\":\"✅ Hotspot AP salvato (rete aperta)!\"}" );
            } else {
                request->send(500, "application/json", "{\"message\":\"Errore salvataggio configurazione Hotspot AP\"}");
            }
        }
    );

    // 17. API RIPRISTINO IMPOSTAZIONI DI FABBRICA SCUOLA (MANTIENE SUPER PASSWORD IN NVS)
    _server.on("/api/factory-reset", HTTP_POST, [](AsyncWebServerRequest* request) {
        if (!isAuthenticated(request)) {
            request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
            return;
        }
        RelayController::forceOff();
        if (ConfigManager::resetToFactoryDefaults()) {
            generateNewSessionToken();
            _sessionRole = AuthRole::NONE;
            AsyncWebServerResponse* response = request->beginResponse(
                200, "application/json",
                "{\"message\":\"🔄 Ripristino di fabbrica completato! Riavvio in corso...\"}"
            );
            response->addHeader("Connection", "close");
            request->send(response);
            Serial.println("[WEB] Factory Reset eseguito su richiesta amministratore. Riavvio tra 1.5s...");
            scheduleRestart(1500);
        } else {
            request->send(500, "application/json", "{\"message\":\"Errore durante il ripristino di fabbrica.\"}");
        }
    });

    // 17b. API RESET TOTALE TECNICO (CANCELLA CONFIGURAZIONE + SUPER PASSWORD IN NVS - SOLO SUPERADMIN)
    _server.on("/api/factory-reset-full", HTTP_POST,
        [](AsyncWebServerRequest* request) {},
        nullptr,
        [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
            if (!isSuperAdmin(request)) {
                request->send(403, "application/json", "{\"message\":\"Accesso riservato alla modalità Tecnico.\"}");
                return;
            }
            if (index != 0 || len != total || total > 1024) {
                request->send(400, "application/json", "{\"message\":\"Payload non valido\"}");
                return;
            }
            JsonDocument doc;
            if (deserializeJson(doc, data, len)) {
                request->send(400, "application/json", "{\"message\":\"JSON non valido\"}");
                return;
            }
            const char* curPwd = doc["current_super_password"] | "";
            if (!ConfigManager::checkSuperPassword(curPwd)) {
                request->send(403, "application/json", "{\"message\":\"Super Password Tecnico errata.\"}");
                return;
            }

            RelayController::forceOff();
            ConfigManager::clearSuperPassword();
            if (ConfigManager::resetToFactoryDefaults()) {
                generateNewSessionToken();
                _sessionRole = AuthRole::NONE;
                AsyncWebServerResponse* response = request->beginResponse(
                    200, "application/json",
                    "{\"message\":\"🗑️ Reset totale completato (config + Super Password)! Riavvio in corso...\"}"
                );
                response->addHeader("Connection", "close");
                request->send(response);
                Serial.println("[WEB] Reset totale (config + Super Password NVS) eseguito da SUPERADMIN. Riavvio tra 1.5s...");
                scheduleRestart(1500);
            } else {
                request->send(500, "application/json", "{\"message\":\"Errore durante il reset totale.\"}");
            }
        }
    );

    // 18. API IMPOSTAZIONI TIMEOUT E SPEGNIMENTO DISPLAY (POST /api/display-settings)
    _server.on("/api/display-settings", HTTP_POST,
        [](AsyncWebServerRequest* request) {},
        nullptr,
        [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
            if (!isAuthenticated(request)) {
                request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
                return;
            }
            JsonDocument doc;
            if (deserializeJson(doc, data, len)) {
                request->send(400, "application/json", "{\"message\":\"JSON non valido\"}");
                return;
            }
            uint32_t timeoutSec = doc["display_timeout_sec"] | ConfigManager::DEFAULT_DISPLAY_TIMEOUT_SEC;
            bool sleepOff = doc["display_sleep_off"] | false;
            if (ConfigManager::setDisplaySettings(timeoutSec, sleepOff)) {
                GuiManager::requestDisplaySettingsReload();
                char msg[112];
                snprintf(msg, sizeof(msg),
                         "{\"message\":\"✅ Display aggiornato (%u s, %s)\"}",
                         ConfigManager::getDisplayTimeoutSec(),
                         sleepOff ? "Spegnimento totale" : "Standby orologio");
                request->send(200, "application/json", msg);
            } else {
                request->send(500, "application/json", "{\"message\":\"Errore salvataggio impostazioni display\"}");
            }
        }
    );

    // 19. API POLICY SICUREZZA HOTSPOT AP (POST /api/ap-policy)
    _server.on("/api/ap-policy", HTTP_POST,
        [](AsyncWebServerRequest* request) {},
        nullptr,
        [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
            if (!isAuthenticated(request)) {
                request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
                return;
            }
            JsonDocument doc;
            if (deserializeJson(doc, data, len)) {
                request->send(400, "application/json", "{\"message\":\"JSON non valido\"}");
                return;
            }
            uint16_t apTimeoutMin = doc["ap_timeout_min"] | ConfigManager::DEFAULT_AP_TIMEOUT_MIN;
            bool apLock = doc["ap_lock_after_sta"] | true;
            if (ConfigManager::setApPolicy(apTimeoutMin, apLock)) {
                if (NetworkManager::isStaConnected()) {
                    NetworkManager::stopFallbackAP();
                }
                char msg[112];
                snprintf(msg, sizeof(msg),
                         "{\"message\":\"🛡️ Policy Hotspot salvata (Timeout: %u min, Blocco STA: %s)\"}",
                         ConfigManager::getApTimeoutMin(),
                         apLock ? "Attivo" : "Disattivo");
                request->send(200, "application/json", msg);
            } else {
                request->send(500, "application/json", "{\"message\":\"Errore salvataggio policy Hotspot\"}");
            }
        }
    );

    // 20. API EXPORT BACKUP CONFIGURAZIONE (GET /api/backup)
    _server.on("/api/backup", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (!isAuthenticated(request)) {
            request->redirect("/login");
            return;
        }
        String json = ConfigManager::exportConfigJson();
        AsyncWebServerResponse* response = request->beginResponse(200, "application/json; charset=utf-8", json);
        response->addHeader("Content-Disposition", "attachment; filename=\"chronoring-backup.json\"");
        response->addHeader("Cache-Control", "no-store");
        request->send(response);
    });

    // 21. API IMPORT / RIPRISTINO BACKUP CONFIGURAZIONE (POST /api/restore)
    static std::string s_restoreBuf;
    static bool s_restoreTooLarge = false;
    _server.on("/api/restore", HTTP_POST,
        [](AsyncWebServerRequest* request) {
            if (!isAuthenticated(request)) {
                s_restoreBuf.clear();
                request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
                return;
            }
            if (s_restoreTooLarge) {
                s_restoreBuf.clear();
                s_restoreBuf.shrink_to_fit();
                request->send(413, "application/json", "{\"message\":\"File di backup troppo grande (max 32 KB).\"}");
                return;
            }
            if (s_restoreBuf.empty()) {
                request->send(400, "application/json", "{\"message\":\"Nessun file di backup ricevuto.\"}");
                return;
            }

            String jsonInput(s_restoreBuf.c_str());
            s_restoreBuf.clear();
            s_restoreBuf.shrink_to_fit();

            String errMsg;
            if (ConfigManager::importConfigJson(jsonInput, errMsg)) {
                AsyncWebServerResponse* response = request->beginResponse(
                    200, "application/json",
                    "{\"message\":\"✅ Backup importato correttamente! Riavvio in corso...\"}"
                );
                response->addHeader("Connection", "close");
                request->send(response);
                Serial.println("[WEB] Backup importato con successo! Riavvio programmato tra 1.5s...");
                scheduleRestart(1500);
            } else {
                JsonDocument errDoc;
                errDoc["message"] = errMsg.length() ? errMsg : "Errore importazione backup";
                String out;
                serializeJson(errDoc, out);
                request->send(400, "application/json", out);
            }
        },
        [](AsyncWebServerRequest* request, String filename, size_t index, uint8_t* data, size_t len, bool final) {
            (void)filename;
            (void)final;
            if (!isAuthenticated(request)) {
                return;
            }
            if (index == 0) {
                s_restoreBuf.clear();
                s_restoreTooLarge = false;
            }
            if (s_restoreTooLarge) {
                return;
            }
            if (s_restoreBuf.size() + len > 32768) {
                s_restoreTooLarge = true;
                s_restoreBuf.clear();
                return;
            }
            if (len > 0) {
                s_restoreBuf.append(reinterpret_cast<const char*>(data), len);
            }
        }
    );

    // 15. PAGINA E HANDLER AGGIORNAMENTO FIRMWARE OTA (/update)
    _server.on("/update", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (!isAuthenticated(request)) {
            request->redirect("/login");
            return;
        }
        request->send(200, "text/html; charset=utf-8", OTA_HTML);
    });

    _server.on("/update", HTTP_POST,
        [](AsyncWebServerRequest* request) {
            if (!isAuthenticated(request)) {
                request->send(401, "text/plain", "Non autorizzato");
                return;
            }
            if (_otaSuccess) {
                AsyncWebServerResponse* response = request->beginResponse(200, "text/plain", "OK");
                response->addHeader("Connection", "close");
                request->send(response);
                Serial.println("[OTA] Aggiornamento completato con successo! Riavvio programmato tra 1.5s...");
                scheduleRestart(1500);
            } else {
                request->send(500, "text/plain", _otaError.length() ? _otaError : "Aggiornamento OTA fallito");
            }
        },
        [](AsyncWebServerRequest* request, String filename, size_t index, uint8_t* data, size_t len, bool final) {
            if (!isAuthenticated(request)) {
                return;
            }

            if (index == 0) {
                _otaInProgress = true;
                _otaSuccess = false;
                _otaError = "";

                RelayController::forceOff();

                Serial.printf("\n[OTA] Avvio aggiornamento firmware da file: '%s'\n", filename.c_str());
                if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
                    _otaError = Update.errorString();
                    Serial.printf("[OTA] ERRORE Update.begin(): %s\n", _otaError.c_str());
                }
            }

            if (_otaError.length() == 0 && len > 0) {
                if (Update.write(data, len) != len) {
                    _otaError = Update.errorString();
                    Serial.printf("[OTA] ERRORE Update.write(): %s\n", _otaError.c_str());
                }
            }

            if (final) {
                _otaInProgress = false;
                if (_otaError.length() == 0) {
                    if (Update.end(true)) {
                        _otaSuccess = true;
                        Serial.printf("[OTA] Scrittura completata (%u byte totali). Verifica immagine OK!\n", index + len);
                    } else {
                        _otaError = Update.errorString();
                        Serial.printf("[OTA] ERRORE Update.end(): %s\n", _otaError.c_str());
                    }
                } else {
                    Update.abort();
                }
            }
        }
    );

    // 16. ENDPOINT CAPTIVE PORTAL (Android, iOS/macOS, Windows) PER CONFIGURAZIONE IMMEDIATA SU AP
    auto captiveRedirectHandler = [](AsyncWebServerRequest* request) {
        if (NetworkManager::isApActive()) {
            String target = String("http://") + NetworkManager::getIpAddress() + "/";
            request->redirect(target);
        } else {
            request->redirect("/");
        }
    };

    const char* captivePaths[] = {
        "/generate_204",
        "/gen_204",
        "/hotspot-detect.html",
        "/library/test/success.html",
        "/connecttest.txt",
        "/ncsi.txt",
        "/redirect",
        "/canonical.html",
        "/success.txt"
    };
    for (const char* path : captivePaths) {
        _server.on(path, HTTP_GET, captiveRedirectHandler);
    }

    _server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send(204);
    });

    _server.onNotFound([captiveRedirectHandler](AsyncWebServerRequest* request) {
        if (NetworkManager::isApActive()) {
            captiveRedirectHandler(request);
        } else {
            request->send(404, "text/plain", "Risorsa non trovata");
        }
    });
}
