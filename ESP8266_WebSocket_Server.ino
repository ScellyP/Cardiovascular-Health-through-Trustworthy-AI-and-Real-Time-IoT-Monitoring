#include <Arduino.h>
#include <SoftwareSerial.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WebSocketsServer.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <ArduinoJson.h>

const char* WIFI_SSID     = "";
const char* WIFI_PASSWORD = "";

const char* GEMINI_API_KEY = "AQ.Ab8RN6L6TTJmfESS90s46Ng8kZqonJuxxUkCGaayEa69TwXgNw";

const char* GEMINI_MODEL   = "gemini-1.5-flash";

#define AI_REPORT_INTERVAL_MS  30000UL

SoftwareSerial ArduinoSerial(4, 5);   // RX=GPIO4(D2), TX=GPIO5(D1)

ESP8266WebServer httpServer(80);
WebSocketsServer wsServer(81);

uint32_t wsFrameCount   = 0;
uint32_t lastPing       = 0;
uint32_t lastAIReport   = 0;
String   incomingLine   = "";

int   aiMin   = 1023, aiMax = 0;
long  aiSum   = 0;
int   aiCount = 0;
int   loFaultCount = 0;
bool  currentLO   = false;

String buildGeminiPrompt(int minVal, int maxVal, int avgVal,
                          int pp, int samples, int loEvents,
                          const char* quality) {
    String p;
    p.reserve(600);
    p  = F("You are an ECG signal quality assistant for a simple DIY monitor "
           "(Arduino Uno + AD8232 + 10-bit ADC, 50 Hz sample rate). "
           "Raw ADC values range 0–1023 (midpoint ~512).\n\n");
    p += F("Last 30-second window stats:\n");
    p += F("  Samples : "); p += samples;  p += '\n';
    p += F("  Min ADC : "); p += minVal;   p += '\n';
    p += F("  Max ADC : "); p += maxVal;   p += '\n';
    p += F("  Avg ADC : "); p += avgVal;   p += '\n';
    p += F("  Peak-peak: "); p += pp;      p += '\n';
    p += F("  Quality  : "); p += quality; p += '\n';
    p += F("  Leads-off events: "); p += loEvents; p += '\n';
    p += F("\nIn 2-3 short sentences:\n"
           "1. Comment on signal quality and likely electrode contact.\n"
           "2. Note any basic waveform observations from these stats.\n"
           "3. Suggest one actionable improvement if quality is not GOOD.\n"
           "Do NOT make clinical diagnoses. Keep it brief and technical.");
    return p;
}

String callGeminiAPI(const String& prompt) {
    if (WiFi.status() != WL_CONNECTED) return F("WiFi not connected");

    BearSSL::WiFiClientSecure client;
    client.setInsecure();

    HTTPClient https;
    String url = F("https://generativelanguage.googleapis.com/v1beta/models/");
    url += GEMINI_MODEL;
    url += F(":generateContent?key=");
    url += GEMINI_API_KEY;

    if (!https.begin(client, url)) return F("HTTPS begin failed");

    https.addHeader(F("Content-Type"), F("application/json"));
    https.setTimeout(15000);

    DynamicJsonDocument reqDoc(2048);
    JsonArray contents = reqDoc.createNestedArray("contents");
    JsonObject msg     = contents.createNestedObject();
    JsonArray parts    = msg.createNestedArray("parts");
    JsonObject part    = parts.createNestedObject();
    part["text"]       = prompt;

    JsonArray safety = reqDoc.createNestedArray("safetySettings");
    const char* categories[] = {
        "HARM_CATEGORY_HARASSMENT",
        "HARM_CATEGORY_HATE_SPEECH",
        "HARM_CATEGORY_SEXUALLY_EXPLICIT",
        "HARM_CATEGORY_DANGEROUS_CONTENT"
    };
    for (auto& cat : categories) {
        JsonObject s = safety.createNestedObject();
        s["category"] = cat;
        s["threshold"] = "BLOCK_NONE";
    }

    JsonObject genCfg = reqDoc.createNestedObject("generationConfig");
    genCfg["maxOutputTokens"] = 200;
    genCfg["temperature"]     = 0.3;

    String body;
    serializeJson(reqDoc, body);

    int httpCode = https.POST(body);
    String result;

    if (httpCode == 200) {
        String payload = https.getString();
        DynamicJsonDocument resDoc(4096);
        DeserializationError err = deserializeJson(resDoc, payload);
        if (!err) {
            result = resDoc["candidates"][0]["content"]["parts"][0]["text"]
                     .as<String>();
            if (result.isEmpty()) result = F("Empty AI response");
        } else {
            result = F("JSON parse error: ");
            result += err.c_str();
        }
    } else {
        result = F("HTTP error: ");
        result += httpCode;
        if (httpCode > 0) {
            result += F(" — ");
            result += https.getString().substring(0, 120);
        }
    }

    https.end();
    return result;
}

void broadcastAIResult(const String& aiText, const char* quality,
                        int pp, int avg) {
    DynamicJsonDocument doc(1024);
    unsigned long s = millis() / 1000;
    char ts[12];
    snprintf(ts, sizeof(ts), "%02lu:%02lu:%02lu",
             (s / 3600) % 24, (s / 60) % 60, s % 60);

    doc["ts"]      = ts;
    doc["quality"] = quality;
    doc["pp"]      = pp;
    doc["avg"]     = avg;
    doc["text"]    = aiText;

    String out = "AI:";
    serializeJson(doc, out);
    
    String broadcastPayload = out;
    wsServer.broadcastTXT(broadcastPayload);
    Serial.println(F("[AI] Broadcast sent to dashboard"));
}

void runAIAnalysis() {
    if (aiCount == 0) return;

    int  avgVal  = (int)(aiSum / aiCount);
    int  pp      = aiMax - aiMin;
    const char* quality =
        pp > 400 ? "GOOD" : pp > 150 ? "FAIR" : "POOR";

    Serial.println(F("[AI] Requesting Gemini analysis..."));
    String prompt = buildGeminiPrompt(aiMin, aiMax, avgVal,
                                       pp, aiCount, loFaultCount, quality);
    String response = callGeminiAPI(prompt);
    Serial.print(F("[AI] Response: "));
    Serial.println(response);

    broadcastAIResult(response, quality, pp, avgVal);

    aiMin = 1023; aiMax = 0; aiSum = 0;
    aiCount = 0; loFaultCount = 0;
}

static const char DASHBOARD_HTML[] PROGMEM = R"HTMLEOF(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>ECG Live Monitor + AI</title>
<style>
  @import url('https://fonts.googleapis.com/css2?family=JetBrains+Mono:wght@400;600;700&family=Syne:wght@400;600;800&display=swap');
  :root {
    --bg: #0e0d0d; --surface: #161514; --surface2: #1e1c1b;
    --border: #2a2726; --green: #4ade80; --green-dim: #166534;
    --amber: #f59e0b; --blue: #60a5fa; --red: #ef4444;
    --purple: #a78bfa; --pink: #f472b6;
    --text: #e8e3de; --muted: #8a837c;
    --font-mono: 'JetBrains Mono', monospace;
    --font-display: 'Syne', sans-serif;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { background: var(--bg); color: var(--text); font-family: var(--font-mono); min-height: 100vh; }

  header {
    display: flex; align-items: center; justify-content: space-between;
    padding: 16px 24px; border-bottom: 1px solid var(--border);
    background: var(--surface);
  }
  .brand { font-family: var(--font-display); font-weight: 800; font-size: 18px;
           letter-spacing: .04em; color: var(--text); display: flex; align-items: center; gap: 10px; }
  .brand-dot { width: 10px; height: 10px; border-radius: 50%; background: var(--green);
               animation: pulse 1.4s ease-in-out infinite; }
  @keyframes pulse { 0%,100%{opacity:1;transform:scale(1)} 50%{opacity:.4;transform:scale(.8)} }
  .conn-badge { font-size: 11px; padding: 4px 10px; border-radius: 20px; font-weight: 600;
                background: #052e16; color: var(--green); border: 1px solid var(--green-dim); }
  .conn-badge.disconnected { background: #2d1515; color: var(--red); border-color: #7f1d1d; }

  .main { padding: 20px 24px; display: grid; grid-template-columns: 1fr 340px; gap: 16px;
          max-width: 1280px; margin: 0 auto; }
  @media (max-width: 900px) { .main { grid-template-columns: 1fr; } }

  .panel { background: var(--surface); border: 1px solid var(--border);
           border-radius: 14px; padding: 16px; }
  .panel-title { font-family: var(--font-display); font-size: 11px; font-weight: 600;
                 color: var(--muted); text-transform: uppercase; letter-spacing: .1em;
                 margin-bottom: 14px; display: flex; align-items: center; gap: 8px; }

  .metrics { display: grid; grid-template-columns: repeat(4, 1fr); gap: 10px; margin-bottom: 14px; }
  @media (max-width: 600px) { .metrics { grid-template-columns: repeat(2, 1fr); } }
  .metric { background: var(--surface2); border: 1px solid var(--border);
            border-radius: 10px; padding: 12px 10px; text-align: center; }
  .metric-val { font-family: var(--font-display); font-size: 26px; font-weight: 800;
                color: #fff; letter-spacing: -.02em; line-height: 1; margin-bottom: 4px; }
  .metric-lbl { font-size: 9px; color: var(--muted); font-weight: 600;
                text-transform: uppercase; letter-spacing: .08em; }
  .metric-val.warn  { color: var(--amber); }
  .metric-val.danger{ color: var(--red);   }
  .metric-val.ok    { color: var(--green); }

  .ecg-wrap { position: relative; border-radius: 10px; overflow: hidden;
              border: 1px solid var(--border); background: #080808; margin-bottom: 14px; }
  canvas#ecg { display: block; width: 100%; height: 120px; }

  .alert { background: #2d1515; border: 1px solid var(--red); border-radius: 10px;
           padding: 10px 14px; font-size: 12px; color: #fca5a5; font-weight: 600;
           margin-bottom: 14px; display: none; }
  .alert.show { display: block; }

  .log { background: #080808; border-radius: 10px; border: 1px solid var(--border);
         padding: 12px; font-size: 11px; height: 120px; overflow: hidden;
         display: flex; flex-direction: column; gap: 3px; justify-content: flex-end; }
  .log-line { color: var(--green); white-space: nowrap; overflow: hidden;
              text-overflow: ellipsis; opacity: 0; animation: fadein .15s forwards; }
  @keyframes fadein { to { opacity: 1; } }

  .sidebar { display: flex; flex-direction: column; gap: 14px; }
  .stat-row { display: flex; justify-content: space-between; align-items: center;
              padding: 8px 0; border-bottom: 1px solid var(--border); font-size: 12px; }
  .stat-row:last-child { border-bottom: none; }
  .stat-key { color: var(--muted); }
  .stat-val { color: var(--text); font-weight: 600; }

  .qual-bar { height: 6px; border-radius: 3px; background: var(--surface2); margin-top: 10px; overflow: hidden; }
  .qual-fill { height: 100%; border-radius: 3px; background: var(--green); transition: width .5s, background .5s; }

  .leads-indicator { display: flex; align-items: center; gap: 8px; font-size: 12px; margin-bottom: 14px; }
  .leads-dot { width: 10px; height: 10px; border-radius: 50%; background: var(--green); }
  .leads-dot.fault { background: var(--red); animation: pulse .6s infinite; }

  canvas#mini { width: 100%; height: 50px; display: block; border-radius: 8px;
                background: #080808; border: 1px solid var(--border); }

  .ai-panel { background: var(--surface); border: 1px solid #3d2e6b;
              border-radius: 14px; padding: 16px; margin-top: 16px; }
  .ai-panel .panel-title { color: var(--purple); }
  .ai-status { display: flex; align-items: center; gap: 8px; font-size: 11px;
               color: var(--muted); margin-bottom: 12px; }
  .ai-dot { width: 8px; height: 8px; border-radius: 50%; background: #3d2e6b;
            transition: background .4s; }
  .ai-dot.thinking { background: var(--purple); animation: pulse .8s infinite; }
  .ai-dot.done     { background: var(--green); animation: none; }
  .ai-badge { font-size: 10px; padding: 2px 8px; border-radius: 12px; font-weight: 600; border: 1px solid; }
  .ai-badge.GOOD { color: var(--green); border-color: var(--green-dim); background: #052e16; }
  .ai-badge.FAIR { color: var(--amber); border-color: #78350f; background: #1c0e03; }
  .ai-badge.POOR { color: var(--red);   border-color: #7f1d1d; background: #2d1515; }
  .ai-text { font-size: 12px; line-height: 1.7; color: var(--text); background: #080808;
             border: 1px solid var(--border); border-radius: 10px; padding: 12px;
             min-height: 80px; white-space: pre-wrap; }
  .ai-meta { margin-top: 8px; font-size: 10px; color: var(--muted);
             display: flex; justify-content: space-between; }
  .ai-countdown { font-size: 10px; color: var(--muted); margin-top: 6px; }

  footer { text-align: center; padding: 16px; font-size: 10px; color: var(--muted);
           border-top: 1px solid var(--border); font-family: var(--font-display);
           letter-spacing: .06em; }
</style>
</head>
<body>

<header>
  <div class="brand">
    <div class="brand-dot"></div>
    ECG LIVE MONITOR + AI
  </div>
  <span class="conn-badge disconnected" id="conn-badge">Connecting…</span>
</header>

<div class="main">
  <div>
    <div class="alert" id="alert-box">⚠ Leads-off detected — check electrode pads</div>

    <div class="panel" style="margin-bottom:16px">
      <div class="panel-title">
        <svg width="8" height="8" viewBox="0 0 8 8"><circle cx="4" cy="4" r="4" fill="#60a5fa"/></svg>
        Live ECG — AD8232
      </div>
      <div class="metrics">
        <div class="metric"><div class="metric-val ok" id="m-adc">—</div><div class="metric-lbl">ADC (A0)</div></div>
        <div class="metric"><div class="metric-val" id="m-min">—</div><div class="metric-lbl">Min</div></div>
        <div class="metric"><div class="metric-val" id="m-max">—</div><div class="metric-lbl">Max</div></div>
        <div class="metric"><div class="metric-val" id="m-pp">—</div><div class="metric-lbl">Peak-Peak</div></div>
      </div>
      <div class="ecg-wrap"><canvas id="ecg" height="120"></canvas></div>
      <div class="leads-indicator">
        <div class="leads-dot" id="leads-dot"></div>
        <span id="leads-txt">Leads: OK</span>
      </div>
      <div class="qual-bar"><div class="qual-fill" id="qual-fill" style="width:0%"></div></div>
    </div>

    <div class="panel" style="margin-bottom:16px">
      <div class="panel-title">
        <svg width="8" height="8" viewBox="0 0 8 8"><circle cx="4" cy="4" r="4" fill="#4ade80"/></svg>
        Serial stream
      </div>
      <div class="log" id="log-box"></div>
    </div>

    <div class="ai-panel">
      <div class="panel-title">
        <svg width="8" height="8" viewBox="0 0 8 8"><circle cx="4" cy="4" r="4" fill="#a78bfa"/></svg>
        Gemini AI Analysis
        <span style="margin-left:auto;font-size:10px;color:#5b4e9b">free tier · every 30 s</span>
      </div>
      <div class="ai-status">
        <div class="ai-dot" id="ai-dot"></div>
        <span id="ai-status-txt">Waiting for first report…</span>
        <span id="ai-quality-badge" style="margin-left:auto"></span>
      </div>
      <div class="ai-text" id="ai-text">AI analysis will appear here after the first 30-second window.</div>
      <div class="ai-meta">
        <span id="ai-ts">—</span>
        <span id="ai-stats">—</span>
      </div>
      <div class="ai-countdown" id="ai-countdown"></div>
    </div>
  </div>

  <div class="sidebar">
    <div class="panel">
      <div class="panel-title">
        <svg width="8" height="8" viewBox="0 0 8 8"><circle cx="4" cy="4" r="4" fill="#f59e0b"/></svg>
        ESP8266 WebSocket
      </div>
      <div class="stat-row"><span class="stat-key">Server IP</span><span class="stat-val" id="s-ip">—</span></div>
      <div class="stat-row"><span class="stat-key">Frames received</span><span class="stat-val" id="s-frames">0</span></div>
      <div class="stat-row"><span class="stat-key">Last update</span><span class="stat-val" id="s-ts">—</span></div>
      <div class="stat-row"><span class="stat-key">Connection</span><span class="stat-val" id="s-conn">—</span></div>
    </div>

    <div class="panel">
      <div class="panel-title">
        <svg width="8" height="8" viewBox="0 0 8 8"><circle cx="4" cy="4" r="4" fill="#a78bfa"/></svg>
        Rolling stats (5 s window)
      </div>
      <div class="stat-row"><span class="stat-key">Average ADC</span><span class="stat-val" id="r-avg">—</span></div>
      <div class="stat-row"><span class="stat-key">Signal quality</span><span class="stat-val" id="r-qual">—</span></div>
      <div class="stat-row"><span class="stat-key">Samples</span><span class="stat-val" id="r-cnt">0</span></div>
    </div>

    <div class="panel">
      <div class="panel-title">
        <svg width="8" height="8" viewBox="0 0 8 8"><circle cx="4" cy="4" r="4" fill="#f472b6"/></svg>
        Trend (mini)
      </div>
      <canvas id="mini" height="50"></canvas>
    </div>

    <div class="panel" style="border-color:#1e3a2e">
      <div class="panel-title" style="color:#4ade80">AI model info</div>
      <div class="stat-row"><span class="stat-key">Model</span><span class="stat-val" style="color:#a78bfa">gemini-1.5-flash</span></div>
      <div class="stat-row"><span class="stat-key">Tier</span><span class="stat-val" style="color:#4ade80">Free</span></div>
      <div class="stat-row"><span class="stat-key">Rate limit</span><span class="stat-val">15 req/min</span></div>
      <div class="stat-row"><span class="stat-key">Daily limit</span><span class="stat-val">1M tokens</span></div>
      <div class="stat-row"><span class="stat-key">Analysis every</span><span class="stat-val">30 s</span></div>
    </div>
  </div>
</div>

<footer>ECG MONITOR · ESP8266 WebSocket · AD8232 · GEMINI AI</footer>

<script>
const ecgCanvas  = document.getElementById('ecg');
const ctx        = ecgCanvas.getContext('2d');
const miniCanvas = document.getElementById('mini');
const mctx       = miniCanvas.getContext('2d');

const BUF_W = 800;
ecgCanvas.width  = BUF_W; ecgCanvas.height = 120;
miniCanvas.width = 400;   miniCanvas.height = 50;

const ecgBuf = new Float32Array(BUF_W);
let ptr = 0;

let winMin = 1023, winMax = 0, winSum = 0, winCnt = 0;
const WINDOW = 250;

// ── WebSocket ──
let ws, frameCount = 0;
const WS_URL = `ws://${location.hostname}:81/`;

let aiNextIn = 30;
setInterval(() => {
  aiNextIn--;
  if (aiNextIn < 0) aiNextIn = 30;
  const el = document.getElementById('ai-countdown');
  if (el) el.textContent = `Next AI report in ~${aiNextIn}s`;
}, 1000);

function connect() {
  document.getElementById('s-ip').textContent = location.hostname;
  ws = new WebSocket(WS_URL);

  ws.onopen = () => {
    document.getElementById('conn-badge').textContent = 'Connected';
    document.getElementById('conn-badge').classList.remove('disconnected');
    document.getElementById('s-conn').textContent = 'Open';
  };

  ws.onclose = () => {
    document.getElementById('conn-badge').textContent = 'Disconnected';
    document.getElementById('conn-badge').classList.add('disconnected');
    document.getElementById('s-conn').textContent = 'Closed';
    setTimeout(connect, 2000);
  };

  ws.onmessage = (evt) => {
    const msg = evt.data.trim();

    if (msg.startsWith('AI:')) {
      handleAIMessage(msg.substring(3));
      return;
    }

    if (msg === 'PING') return;
    const match = msg.match(/ECG:(\d+),LO:([01])/);
    if (!match) return;

    const ecgVal  = parseInt(match[1]);
    const loFault = match[2] === '1';
    frameCount++;

    ecgBuf[ptr] = ecgVal;
    ptr = (ptr + 1) % BUF_W;

    winSum += ecgVal; winCnt++;
    if (ecgVal < winMin) winMin = ecgVal;
    if (ecgVal > winMax) winMax = ecgVal;
    if (winCnt > WINDOW) { winMin = 1023; winMax = 0; winSum = 0; winCnt = 0; }

    const pp     = winMax - winMin;
    const avg    = winCnt > 0 ? Math.round(winSum / winCnt) : 0;
    const qual   = pp > 400 ? 'GOOD' : pp > 150 ? 'FAIR' : 'POOR';
    const qualPct = Math.min(100, Math.round(pp / 600 * 100));

    document.getElementById('m-adc').textContent    = ecgVal;
    document.getElementById('m-min').textContent    = winMin === 1023 ? '—' : winMin;
    document.getElementById('m-max').textContent    = winMax;
    document.getElementById('m-pp').textContent     = pp;
    document.getElementById('s-frames').textContent = frameCount;
    document.getElementById('s-ts').textContent     = new Date().toLocaleTimeString();
    document.getElementById('r-avg').textContent    = avg;
    document.getElementById('r-qual').textContent   = qual;
    document.getElementById('r-cnt').textContent    = winCnt;
    document.getElementById('qual-fill').style.width      = qualPct + '%';
    document.getElementById('qual-fill').style.background =
      qual === 'GOOD' ? '#4ade80' : qual === 'FAIR' ? '#f59e0b' : '#ef4444';

    const ldDot = document.getElementById('leads-dot');
    const ldTxt = document.getElementById('leads-txt');
    const alert = document.getElementById('alert-box');
    if (loFault) {
      ldDot.classList.add('fault'); ldTxt.textContent = 'Leads: FAULT';
      alert.classList.add('show');
    } else {
      ldDot.classList.remove('fault'); ldTxt.textContent = 'Leads: OK';
      alert.classList.remove('show');
    }

    addLog(msg);
    draw();
  };
}

function handleAIMessage(jsonStr) {
  try {
    const d = JSON.parse(jsonStr);
    const dot    = document.getElementById('ai-dot');
    const status = document.getElementById('ai-status-txt');
    const badge  = document.getElementById('ai-quality-badge');
    const text   = document.getElementById('ai-text');
    const ts     = document.getElementById('ai-ts');
    const stats  = document.getElementById('ai-stats');

    dot.className = 'ai-dot done';
    status.textContent = 'Analysis complete';
    text.textContent   = d.text || '(no text)';
    ts.textContent     = 'Last report: ' + (d.ts || '—');
    stats.textContent  = `PP=${d.pp}  AVG=${d.avg}`;

    badge.className   = 'ai-badge ' + (d.quality || '');
    badge.textContent = d.quality || '—';

    aiNextIn = 30;

    setTimeout(() => { dot.className = 'ai-dot'; }, 3000);
  } catch(e) {
    document.getElementById('ai-text').textContent = 'Parse error: ' + e.message;
  }
}

function addLog(line) {
  const box = document.getElementById('log-box');
  const d   = document.createElement('div');
  d.className  = 'log-line';
  d.textContent = `> ${line}`;
  box.appendChild(d);
  while (box.children.length > 8) box.removeChild(box.firstChild);
}

function draw() {
  const W = BUF_W, H = 120;
  ctx.clearRect(0, 0, W, H);
  ctx.fillStyle = '#080808'; ctx.fillRect(0, 0, W, H);
  ctx.strokeStyle = '#161514'; ctx.lineWidth = 1;
  for (let x = 0; x < W; x += 50) { ctx.beginPath(); ctx.moveTo(x,0); ctx.lineTo(x,H); ctx.stroke(); }
  for (let y = 0; y < H; y += 20) { ctx.beginPath(); ctx.moveTo(0,y); ctx.lineTo(W,y); ctx.stroke(); }
  ctx.strokeStyle = '#4ade80'; ctx.lineWidth = 1.5;
  ctx.beginPath();
  for (let i = 0; i < W; i++) {
    const idx = (ptr + i) % W;
    const v = ecgBuf[idx];
    const x = i;
    const y = H/2 - ((v - 512) / 512) * (H * 0.45);
    i === 0 ? ctx.moveTo(x,y) : ctx.lineTo(x,y);
  }
  ctx.stroke();
  ctx.strokeStyle = '#166534'; ctx.lineWidth = 1;
  ctx.beginPath(); ctx.moveTo(ptr,0); ctx.lineTo(ptr,H); ctx.stroke();

  mctx.clearRect(0, 0, 400, 50);
  mctx.strokeStyle = '#f59e0b'; mctx.lineWidth = 1.2;
  mctx.beginPath();
  for (let i = 0; i < 400; i++) {
    const idx = (ptr + Math.floor(i * W / 400)) % W;
    const v = ecgBuf[idx];
    const x = i, y = 25 - ((v-512)/512)*20;
    i === 0 ? mctx.moveTo(x,y) : mctx.lineTo(x,y);
  }
  mctx.stroke();
}

connect();
</script>
</body>
</html>
)HTMLEOF";

void handleArduinoData(String& line) {
    if (line.length() < 6) return;
    if (!line.startsWith("ECG:")) return;

    wsServer.broadcastTXT(line);
    wsFrameCount++;

    int commaIdx = line.indexOf(',');
    if (commaIdx < 5) return;
    int  ecgVal = line.substring(4, commaIdx).toInt();
    bool lo     = line.endsWith("LO:1");
    currentLO   = lo;

    if (lo) {
        loFaultCount++;
    } else {
        aiSum += ecgVal;
        aiCount++;
        if (ecgVal < aiMin) aiMin = ecgVal;
        if (ecgVal > aiMax) aiMax = ecgVal;
    }
}

void onWebSocketEvent(uint8_t num, WStype_t type,
                       uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_CONNECTED:
            Serial.printf("[WS] Client %u connected\n", num);
            break;
        case WStype_DISCONNECTED:
            Serial.printf("[WS] Client %u disconnected\n", num);
            break;
        default:
            break;
    }
}

void setup() {
    Serial.begin(115200);
    ArduinoSerial.begin(9600);

    Serial.println(F("\n=== ESP8266 ECG WebSocket Server + Gemini AI ==="));

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print(F("Connecting to WiFi"));
    uint8_t tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 40) {
        delay(500); Serial.print('.');
        tries++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.print(F("IP address: "));
        Serial.println(WiFi.localIP());
        Serial.println(F("Gemini AI reports enabled (every 30s)"));
    } else {
        Serial.println(F("WiFi failed — running AP mode (AI disabled)"));
        WiFi.mode(WIFI_AP);
        WiFi.softAP("ECG_Monitor", "ecgmonitor");
        Serial.print(F("AP IP: "));
        Serial.println(WiFi.softAPIP());
    }

    httpServer.on("/", []() {
        httpServer.send_P(200, "text/html", DASHBOARD_HTML);
    });
    httpServer.begin();
    Serial.println(F("HTTP server started on port 80"));

    wsServer.begin();
    wsServer.onEvent(onWebSocketEvent);
    Serial.println(F("WebSocket server started on port 81"));

    lastAIReport = millis();
}

void loop() {
    httpServer.handleClient();
    wsServer.loop();

    while (ArduinoSerial.available()) {
        char c = ArduinoSerial.read();
        if (c == '\n') {
            incomingLine.trim();
            if (incomingLine.length() > 0) {
                handleArduinoData(incomingLine);
            }
            incomingLine = "";
        } else if (c != '\r') {
            if (incomingLine.length() < 64) incomingLine += c;
        }
    }

    uint32_t now = millis();
    if (now - lastPing > 5000) {
        lastPing = now;
        String pingPayload = "PING";
        wsServer.broadcastTXT(pingPayload);
    }

    if (WiFi.status() == WL_CONNECTED &&
        now - lastAIReport >= AI_REPORT_INTERVAL_MS) {
        lastAIReport = now;
        String statusPayload = "AI:{\"ts\":\"...\",\"quality\":\"...\",\"pp\":0,\"avg\":0,\"text\":\"Analyzing…\"}";
        wsServer.broadcastTXT(statusPayload);
        runAIAnalysis();
    }
}