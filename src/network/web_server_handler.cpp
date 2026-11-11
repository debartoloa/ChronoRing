#include "web_server_handler.h"
#include "network_manager.h"
#include "../config_manager.h"
#include "../relay_controller.h"
#include "../scheduler/scheduler.h"
#include "../gui/gui_manager.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Update.h>
#include <esp_timer.h>

AsyncWebServer WebServerHandler::_server(80);
char WebServerHandler::_sessionToken[40] = {0};
bool WebServerHandler::_otaInProgress = false;
bool WebServerHandler::_otaSuccess = false;
String WebServerHandler::_otaError = "";

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
  <p class="sub">Sistema Automazione Campanella Scolastica</p>
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
    if (res.ok) {
      window.location.href = '/';
    } else {
      err.textContent = 'Password errata. Riprova.';
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
  .brand span{font-size:0.78rem;color:var(--muted);display:block;}
  .nav-btns{display:flex;gap:10px;align-items:center;}
  .btn{padding:9px 15px;border-radius:9px;border:none;font-weight:600;font-size:0.88rem;cursor:pointer;transition:0.2s;text-decoration:none;display:inline-flex;align-items:center;gap:6px;}
  .btn-cyan{background:linear-gradient(135deg,#0284c7,#00d4ff);color:#041022;}
  .btn-gold{background:linear-gradient(135deg,#d97706,#fbbf24);color:#1e1103;font-weight:700;}
  .btn-danger{background:rgba(239,68,68,0.18);color:#fca5a5;border:1px solid rgba(239,68,68,0.4);}
  .btn-outline{background:rgba(0,212,255,0.1);color:var(--cyan);border:1px solid rgba(0,212,255,0.35);}
  .btn:hover{filter:brightness(1.12);}
  .container{max-width:1100px;margin:22px auto;padding:0 16px;display:grid;grid-template-columns:repeat(auto-fit,minmax(320px,1fr));gap:18px;}
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
  th,td{padding:10px 8px;text-align:left;border-bottom:1px solid rgba(255,255,255,0.07);font-size:0.92rem;}
  th{color:var(--muted);font-size:0.8rem;text-transform:uppercase;}
  input[type="time"],input[type="number"],input[type="text"],input[type="password"]{background:#071022;border:1px solid rgba(0,212,255,0.3);color:#fff;padding:8px 10px;border-radius:8px;font-size:0.92rem;}
  .form-grid{display:grid;gap:12px;}
  .form-group label{display:block;font-size:0.8rem;color:var(--muted);margin-bottom:5px;}
  .form-group input{width:100%;}
  .logo-preview-wrap{display:flex;align-items:center;gap:14px;background:#071022;padding:12px;border-radius:10px;border:1px dashed rgba(0,212,255,0.3);}
  canvas#logoCanvas{background:#0B0F17;border:1px solid rgba(255,255,255,0.15);border-radius:8px;flex-shrink:0;}
  #toast{position:fixed;bottom:20px;right:20px;background:#0f2942;border:1px solid var(--cyan);color:#fff;padding:12px 20px;border-radius:10px;box-shadow:0 8px 25px rgba(0,0,0,0.5);display:none;z-index:999;font-size:0.9rem;}
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
      <h1>ChronoRing</h1>
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

  <!-- PERSONALIZZAZIONE ISTITUTO & SCHERMATA DI STANDBY -->
  <div class="card card-full">
    <h2>
      <span>🏫 Personalizzazione Istituto & Schermata Standby (1 min)</span>
      <span id="logoStatusBadge" class="badge badge-warn">Nessun Logo</span>
    </h2>
    <div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:18px;align-items:center;">
      <div class="form-grid">
        <div class="form-group">
          <label>Nome dell'Istituto Scolastico (mostrato nella schermata di Standby)</label>
          <div style="display:flex;gap:8px;">
            <input type="text" id="schoolName" maxlength="60" placeholder="Es. I.I.S. Enrico Fermi">
            <button class="btn btn-cyan" onclick="saveSchoolName()">💾 Salva Nome</button>
          </div>
        </div>
        <div style="font-size:0.8rem;color:var(--muted);line-height:1.4;">
          Dopo <strong>1 minuto</strong> di inattività touch, il display abbassa la luminosità e mostra l'orologio digitale grande <code>HH:MM:SS</code> insieme al <strong>Logo</strong> e al <strong>Nome dell'Istituto</strong>. Basta un tocco sullo schermo per riattivarlo.
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

  <!-- TABELLA ORARI PROGRAMMATI -->
  <div class="card card-full">
    <h2>
      <span>📅 Programmazione Orari (Lunedì - Sabato)</span>
      <button class="btn btn-outline" onclick="addScheduleRow()">+ Aggiungi Orario</button>
    </h2>
    <div style="overflow-x:auto;">
      <table>
        <thead>
          <tr>
            <th>#</th>
            <th>Orario (HH:MM)</th>
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

  <!-- DURATA SQUILLO E SICUREZZA -->
  <div class="card">
    <h2><span>⚙️ Durata Squillo & Password</span></h2>
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

  <!-- CONFIGURAZIONE WI-FI -->
  <div class="card">
    <h2><span>📶 Configurazione Rete Wi-Fi</span></h2>
    <div class="form-grid">
      <div class="form-group">
        <label>SSID Rete Wi-Fi</label>
        <input type="text" id="wifiSsid" placeholder="Nome rete Wi-Fi...">
      </div>
      <div class="form-group">
        <label>Password Wi-Fi (lascia vuoto per mantenere l'attuale)</label>
        <input type="password" id="wifiPwd" placeholder="Password Wi-Fi...">
      </div>
      <button class="btn btn-cyan" onclick="saveWiFi()">💾 Salva e Connetti Wi-Fi</button>
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
    document.getElementById('netInfo').textContent = d.net_mode;
    document.getElementById('ipAddr').textContent = d.ip + ' (chronoring.local)';
    document.getElementById('chkSuspended').checked = d.suspended;
  } catch (e) {}
}

async function loadConfig() {
  const r = await fetch('/api/config');
  if (r.status === 401) { window.location.href = '/login'; return; }
  const d = await r.json();
  document.getElementById('chkSuspended').checked = d.suspended;
  document.getElementById('pulseMs').value = d.relay_pulse_ms;
  document.getElementById('wifiSsid').value = d.wifi_ssid || '';
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

function renderSchedule() {
  const tbody = document.getElementById('scheduleBody');
  tbody.innerHTML = '';
  scheduleData.forEach((item, idx) => {
    const tr = document.createElement('tr');
    tr.innerHTML = `
      <td>${idx + 1}</td>
      <td><input type="time" value="${item.time}" onchange="scheduleData[${idx}].time = this.value;"></td>
      <td><span class="badge badge-ok">Lun - Sab</span></td>
      <td>
        <label class="switch">
          <input type="checkbox" ${item.enabled ? 'checked' : ''} onchange="scheduleData[${idx}].enabled = this.checked">
          <span class="slider"></span>
        </label>
      </td>
      <td><button class="btn btn-danger" style="padding:5px 10px" onclick="removeRow(${idx})">✕</button></td>
    `;
    tbody.appendChild(tr);
  });
}

function addScheduleRow() {
  scheduleData.push({time: '11:10', days_mask: 126, enabled: true});
  renderSchedule();
}

function removeRow(idx) {
  scheduleData.splice(idx, 1);
  renderSchedule();
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
  const r = await fetch('/api/schedule', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({schedule: scheduleData})
  });
  const d = await r.json();
  showToast(d.message || 'Orari salvati!', !r.ok);
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
  if (pwd.length < 3) { showToast('Password troppo corta (min 3 caratteri)', true); return; }
  const r = await fetch('/api/password', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({password: pwd})
  });
  if (r.ok) {
    showToast('Password aggiornata! Effettua nuovamente il login.');
    setTimeout(() => window.location.href = '/login', 1500);
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

loadConfig();
fetchStatus();
setInterval(fetchStatus, 1000);
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

bool WebServerHandler::isAuthenticated(AsyncWebServerRequest* request) {
    if (!request->hasHeader("Cookie")) {
        return false;
    }
    String cookie = request->header("Cookie");
    String expected = String("CHRONORING_SESSION=") + _sessionToken;
    return (cookie.indexOf(expected) != -1);
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

    setupRoutes();
    _server.begin();
    Serial.println("[WEB] Web Server ChronoRing attivo sulla porta 80.");
}

void WebServerHandler::setupRoutes() {
    // 1. PAGINA DI LOGIN
    _server.on("/login", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (isAuthenticated(request)) {
            request->redirect("/");
            return;
        }
        request->send(200, "text/html; charset=utf-8", LOGIN_HTML);
    });

    // 2. LOGOUT
    _server.on("/logout", HTTP_GET, [](AsyncWebServerRequest* request) {
        generateNewSessionToken();
        AsyncWebServerResponse* response = request->beginResponse(302, "text/plain", "Redirecting...");
        response->addHeader("Location", "/login");
        response->addHeader("Set-Cookie", "CHRONORING_SESSION=; Path=/; Max-Age=0");
        request->send(response);
    });

    // 3. DASHBOARD PRINCIPALE (Protetta)
    _server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (!isAuthenticated(request)) {
            request->redirect("/login");
            return;
        }
        request->send(200, "text/html; charset=utf-8", DASHBOARD_HTML);
    });

    // 4. API LOGIN (POST JSON)
    _server.on("/api/login", HTTP_POST,
        [](AsyncWebServerRequest* request) {},
        nullptr,
        [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
            JsonDocument doc;
            if (deserializeJson(doc, data, len)) {
                request->send(400, "application/json", "{\"error\":\"JSON non valido\"}");
                return;
            }
            const char* pwd = doc["password"] | "";
            if (ConfigManager::checkWebPassword(pwd)) {
                generateNewSessionToken();
                AsyncWebServerResponse* response = request->beginResponse(200, "application/json", "{\"status\":\"ok\"}");
                String cookieHeader = String("CHRONORING_SESSION=") + _sessionToken + "; Path=/; HttpOnly";
                response->addHeader("Set-Cookie", cookieHeader);
                request->send(response);
                Serial.println("[WEB] Login amministratore effettuato con successo.");
            } else {
                Serial.println("[WEB] Tentativo di login fallito (password errata).");
                request->send(401, "application/json", "{\"error\":\"Password errata\"}");
            }
        }
    );

    // 5. API STATO IN TEMPO REALE (GET /api/status)
    _server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (!isAuthenticated(request)) {
            request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
            return;
        }
        JsonDocument doc;
        doc["time"] = NetworkManager::getFormattedTime();
        doc["date"] = NetworkManager::getFormattedDate();
        doc["ntp_synced"] = NetworkManager::isTimeSynced();
        doc["next_bell"] = Scheduler::getNextBellString();
        doc["last_bell"] = Scheduler::getLastTriggeredString();
        doc["relay_active"] = RelayController::isActive();
        doc["relay_pin"] = RELAY_PIN;
        doc["has_display"] = g_has_display;
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
        doc["wifi_ssid"] = cfg.wifiSsid;
        doc["school_name"] = cfg.schoolName;
        doc["has_school_logo"] = ConfigManager::hasSchoolLogo();
        doc["relay_pulse_ms"] = cfg.relayPulseMs;
        doc["suspended"] = cfg.suspended;

        JsonArray arr = doc["schedule"].to<JsonArray>();
        for (const auto& item : cfg.schedule) {
            JsonObject obj = arr.add<JsonObject>();
            char tBuf[6];
            snprintf(tBuf, sizeof(tBuf), "%02u:%02u", item.hour, item.minute);
            obj["time"] = tBuf;
            obj["days_mask"] = item.daysMask;
            obj["enabled"] = item.enabled;
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
    _server.on("/api/schedule", HTTP_POST,
        [](AsyncWebServerRequest* request) {},
        nullptr,
        [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
            if (!isAuthenticated(request)) {
                request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
                return;
            }
            JsonDocument doc;
            if (deserializeJson(doc, data, len) || !doc["schedule"].is<JsonArray>()) {
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
                    item.daysMask = obj["days_mask"] | ConfigManager::DAYS_MON_TO_SAT_MASK;
                    item.enabled = obj["enabled"] | true;
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

    // 13. API MODIFICA PASSWORD WEB (POST /api/password)
    _server.on("/api/password", HTTP_POST,
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
            const char* newPwd = doc["password"] | "";
            if (strlen(newPwd) < 3) {
                request->send(400, "application/json", "{\"message\":\"Password troppo corta\"}");
                return;
            }
            if (ConfigManager::setWebPassword(newPwd)) {
                generateNewSessionToken();
                request->send(200, "application/json", "{\"message\":\"Password aggiornata\"}");
            } else {
                request->send(500, "application/json", "{\"message\":\"Errore salvataggio password\"}");
            }
        }
    );

    // 14. API CONFIGURAZIONE CREDENZIALI WI-FI (POST /api/wifi)
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
}
