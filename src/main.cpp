/*
 * WiFi Radar — RSSI-based motion / presence detector for ESP8266
 * ---------------------------------------------------------------
 * Detects human motion using nothing but the WiFi radio.
 *
 * Physics: every WiFi packet the board receives carries an RSSI
 * (signal-strength) value. In a still room RSSI is steady (low
 * variance). A moving body absorbs / reflects 2.4 GHz waves and
 * scrambles the multipath, so RSSI starts to jitter (high variance).
 * We don't care about the level, only the variance: we compare the
 * live RSSI standard-deviation against an auto-calibrated empty-room
 * baseline. score = liveStd / baselineStd. score >> 1  =>  motion.
 *
 * Outputs:
 *   - Serial @115200 : live ASCII meter + IDLE/MOTION + stats
 *   - Onboard LED    : lit while MOTION (active-low)
 *   - Web dashboard  : http://wifiradar.local  (live scope + recalibrate)
 *   - Config page    : http://wifiradar.local/config
 *   - SNMP agent     : optional, polled by something like Zabbix (see below)
 *
 * WiFi setup: no credentials are compiled in. On first boot (or whenever
 * saved credentials fail to connect) the board opens its own access point,
 * "WiFiRadar-Setup" (password "radarsetup"), with a captive portal at
 * 192.168.4.1 where you pick your network and enter the password. The
 * choice is saved to flash, so normal boots reconnect automatically.
 * Browse to http://wifiradar.local/resetwifi to forget the saved network
 * and re-open the setup portal.
 *
 * Optional external motion sensor: a PIR or RCWL-0516-style digital-output
 * module can be wired to one GPIO. Disabled by default; turn it on and pick
 * the pin from the config page. When enabled it's read alongside the
 * RSSI-based detector and reported separately (dashboard, /data, SNMP).
 *
 * Optional SNMP agent (SNMPv2c, read-only): exposes RSSI, score, baseline,
 * motion state, channel, uptime and (if enabled) the external sensor's
 * state as OIDs under a private enterprise arc. Turn it on from the config
 * page, set the community string, and point a poller (Zabbix, etc.) at the
 * board's IP on UDP 161. NOTE: the OID prefix below (.1.3.6.1.4.1.52145.1.*)
 * is a placeholder, not a real IANA-registered Private Enterprise Number —
 * fine for a LAN/internal Zabbix setup, but get your own PEN from IANA
 * (https://pen.iana.org) before using this scheme anywhere it might collide
 * with someone else's.
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <FS.h>         // FSInfo, used for the [fs] mounted-bytes diagnostic below
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <SNMP_Agent.h>     // https://github.com/0neblock/Arduino_SNMP
#include <math.h>
#include <WiFiManager.h>    // https://github.com/tzapu/WiFiManager

// ------------------------- Tunables -------------------------
static const uint32_t SAMPLE_INTERVAL_MS = 50;    // ~20 Hz RSSI sampling
static const uint16_t WINDOW             = 64;    // live sliding window (~3.2 s)
static const uint16_t CAL_SAMPLES        = 200;   // calibration length (~10 s)
static const float    TRIP_FACTOR        = 2.2f;  // score to ENTER motion
static const float    CLEAR_FACTOR       = 1.5f;  // score to LEAVE motion
static const uint32_t MOTION_HOLD_MS     = 2000;  // min time held in MOTION
static const float    MIN_BASELINE_STD   = 0.8f;  // floor so a dead-quiet room can't div-by-~0
static const char*    CONFIG_PATH        = "/config.json";
static const char*    SNMP_OID_PREFIX    = ".1.3.6.1.4.1.52145.1"; // placeholder PEN — see header note

// Pins considered safe to offer for the external sensor on a bare ESP-12E/F.
// GPIO2 is excluded here because it's the onboard LED used for the motion
// indicator; GPIO1/3 are excluded because they're the Serial TX/RX pins.
static const uint8_t RADAR_PIN_CHOICES[] = {4, 5, 12, 13, 14, 15, 16};

// ------------------------- Config (persisted to LittleFS) -------------------------
// Set once in setup(); true only if the filesystem actually mounted (and has
// usable space) on this chip. Checked before every save, and surfaced in the
// /config page and the save-result message, so a failure says WHY instead of
// just "failed" — see the flash-size note at the top of platformio.ini.
bool fsAvailable = false;
struct Config {
  bool radarEnabled   = false;
  uint8_t radarPin    = 14;     // GPIO14, a safe default if enabled
  bool radarActiveHigh = true;  // most PIR / RCWL-0516 modules idle low, go high on motion
  bool snmpEnabled    = false;
  char snmpCommunity[24] = "public";
  char sysName[32]       = "wifiradar";
  char sysLocation[32]   = "";
};
Config cfg;

bool isAllowedRadarPin(uint8_t pin) {
  for (uint8_t p : RADAR_PIN_CHOICES) if (p == pin) return true;
  return false;
}

void loadConfig() {
  if (!LittleFS.exists(CONFIG_PATH)) return;      // keep defaults
  File f = LittleFS.open(CONFIG_PATH, "r");
  if (!f) return;
  StaticJsonDocument<384> doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.printf("[config] Failed to parse %s: %s — using defaults\n", CONFIG_PATH, err.c_str());
    return;
  }
  cfg.radarEnabled    = doc["radarEnabled"]    | cfg.radarEnabled;
  cfg.radarPin        = doc["radarPin"]        | cfg.radarPin;
  cfg.radarActiveHigh = doc["radarActiveHigh"] | cfg.radarActiveHigh;
  cfg.snmpEnabled     = doc["snmpEnabled"]     | cfg.snmpEnabled;
  strlcpy(cfg.snmpCommunity, doc["snmpCommunity"] | cfg.snmpCommunity, sizeof(cfg.snmpCommunity));
  strlcpy(cfg.sysName,       doc["sysName"]       | cfg.sysName,       sizeof(cfg.sysName));
  strlcpy(cfg.sysLocation,   doc["sysLocation"]   | cfg.sysLocation,   sizeof(cfg.sysLocation));
  if (!isAllowedRadarPin(cfg.radarPin)) cfg.radarPin = 14;
}

bool saveConfig() {
  if (!fsAvailable) {
    Serial.println(F("[config] Save skipped: no usable filesystem on this chip "
                      "(see the [fs] lines logged at boot)."));
    return false;
  }
  StaticJsonDocument<384> doc;
  doc["radarEnabled"]    = cfg.radarEnabled;
  doc["radarPin"]        = cfg.radarPin;
  doc["radarActiveHigh"] = cfg.radarActiveHigh;
  doc["snmpEnabled"]     = cfg.snmpEnabled;
  doc["snmpCommunity"]   = cfg.snmpCommunity;
  doc["sysName"]         = cfg.sysName;
  doc["sysLocation"]     = cfg.sysLocation;
  File f = LittleFS.open(CONFIG_PATH, "w");
  if (!f) {
    Serial.println(F("[config] LittleFS.open(\"w\") failed — filesystem mounted but "
                      "out of space, or the partition is smaller than expected. "
                      "Check platformio.ini's board_build.ldscript."));
    return false;
  }
  size_t written = serializeJson(doc, f);
  f.close();
  if (written == 0) {
    Serial.println(F("[config] serializeJson wrote 0 bytes — treating as a failed save."));
    return false;
  }
  return true;
}

// ------------------------- State -------------------------
ESP8266WebServer server(80);
WiFiUDP   udp;        // keepalive "poke" traffic to the gateway
WiFiUDP   snmpUdp;    // SNMP agent transport (only bound if SNMP is enabled)
SNMPAgent *snmpAgent = nullptr;
IPAddress gatewayIP;

// Hard-coded to GPIO2 (active-low). LED_BUILTIN is NOT safe here: on the generic
// variant used by esp01_1m it resolves to GPIO1, which is the Serial TX pin.
const uint8_t LED = 2;

// live sliding window (ring buffer)
float    ring[WINDOW];
uint16_t ringCount = 0, ringHead = 0;

// calibration (Welford online variance)
uint32_t calN = 0;
float    calMean = 0, calM2 = 0;
uint16_t calCollected = 0;
float    baselineStd = MIN_BASELINE_STD;
bool     calibrated  = false;

// detection
bool     motion = false;
uint32_t motionSince = 0;
float    lastScore = 0;
int32_t  lastRSSI = 0;
bool     radarMotion = false;   // external sensor, only meaningful if cfg.radarEnabled

uint32_t lastSampleMs = 0;

// SNMP-exposed mirrors. The Arduino_SNMP library's gauge/integer handlers
// hold a pointer to the variable, so these just need to stay updated —
// the agent reads them live on every GET, no re-registration needed.
int      snmpRssi = 0;
uint32_t snmpScoreX100 = 0;
uint32_t snmpStdX100 = 0;
uint32_t snmpBaselineX100 = 0;
int      snmpCalibrated = 0;
int      snmpMotion = 0;
int      snmpChannel = 0;
uint32_t snmpUptimeSec = 0;
int      snmpRadarEnabled = 0;
int      snmpRadarMotion = 0;

// ------------------------- Helpers -------------------------
void pushSample(float v) {
  ring[ringHead] = v;
  ringHead = (ringHead + 1) % WINDOW;
  if (ringCount < WINDOW) ringCount++;
}

float windowStd() {
  if (ringCount < 2) return 0;
  float mean = 0;
  for (uint16_t i = 0; i < ringCount; i++) mean += ring[i];
  mean /= ringCount;
  float var = 0;
  for (uint16_t i = 0; i < ringCount; i++) { float d = ring[i] - mean; var += d * d; }
  return sqrtf(var / ringCount);
}

void poke() {
  // Provoke a little two-way traffic so WiFi.RSSI() refreshes promptly,
  // even on a quiet network. Port 9 = discard; nothing needs to listen.
  udp.beginPacket(gatewayIP, 9);
  udp.write((const uint8_t*)"r", 1);
  udp.endPacket();
}

void recalibrate() {
  ringCount = ringHead = 0;
  calN = 0; calMean = 0; calM2 = 0; calCollected = 0;
  calibrated = false;
  motion = false;
  digitalWrite(LED, HIGH);          // LED off (active-low)
  Serial.println(F("[cal] Hold still / leave the room for ~10 s..."));
}

void printMeter(int32_t rssi, float stdv, float score) {
  static uint32_t last = 0;
  uint32_t now = millis();
  if (now - last < 100) return;     // throttle serial meter to ~10 Hz
  last = now;
  const int BARW = 30;
  int n = (int)(score / (TRIP_FACTOR * 1.5f) * BARW);
  if (n < 0) n = 0;
  if (n > BARW) n = BARW;
  char bar[BARW + 1];
  for (int i = 0; i < BARW; i++) bar[i] = (i < n) ? '#' : '-';
  bar[BARW] = 0;
  Serial.printf("RSSI %4d dBm | std %5.2f | x%5.2f [%s] %s%s\n",
                (int)rssi, stdv, score, bar, motion ? "** MOTION **" : "idle",
                (cfg.radarEnabled && radarMotion) ? " + SENSOR" : "");
}

// ------------------------- SNMP -------------------------
void setupSnmp() {
  if (!cfg.snmpEnabled) return;
  snmpAgent = new SNMPAgent(cfg.snmpCommunity);
  snmpAgent->setUDP(&snmpUdp);

  // Standard MIB-II identity fields, read-only.
  char descr[48];
  snprintf(descr, sizeof(descr), "WiFi Radar RSSI motion sensor");
  snmpAgent->addReadOnlyStaticStringHandler(".1.3.6.1.2.1.1.1.0", std::string(descr));
  snmpAgent->addReadOnlyStaticStringHandler(".1.3.6.1.2.1.1.5.0", std::string(cfg.sysName));
  snmpAgent->addReadOnlyStaticStringHandler(".1.3.6.1.2.1.1.6.0", std::string(cfg.sysLocation));

  // Custom metrics. All pointer-based: the variable is updated in loop(),
  // the agent just reads whatever it currently points to.
  char oid[48];
  auto mkoid = [&](int n) -> const char* {
    snprintf(oid, sizeof(oid), "%s.%d", SNMP_OID_PREFIX, n);
    return oid;
  };
  snmpAgent->addIntegerHandler(mkoid(1), &snmpRssi);           // dBm
  snmpAgent->addGaugeHandler(mkoid(2),   &snmpScoreX100);      // score * 100
  snmpAgent->addGaugeHandler(mkoid(3),   &snmpStdX100);        // RSSI std-dev * 100
  snmpAgent->addGaugeHandler(mkoid(4),   &snmpBaselineX100);   // baseline std * 100
  snmpAgent->addIntegerHandler(mkoid(5), &snmpCalibrated);     // 0/1
  snmpAgent->addIntegerHandler(mkoid(6), &snmpMotion);         // 0/1, RSSI-based
  snmpAgent->addIntegerHandler(mkoid(7), &snmpChannel);
  snmpAgent->addGaugeHandler(mkoid(8),   &snmpUptimeSec);
  snmpAgent->addIntegerHandler(mkoid(9), &snmpRadarEnabled);   // 0/1
  snmpAgent->addIntegerHandler(mkoid(10), &snmpRadarMotion);   // 0/1, external sensor

  snmpAgent->sortHandlers();
  snmpAgent->begin();
  Serial.printf("[snmp] Agent enabled on UDP 161, community \"%s\"\n", cfg.snmpCommunity);
  Serial.printf("[snmp] Custom OIDs under %s.*  (1=rssi 2=scoreX100 3=stdX100 4=baselineX100\n", SNMP_OID_PREFIX);
  Serial.println(F("[snmp]   5=calibrated 6=motion 7=channel 8=uptimeSec 9=radarEnabled 10=radarMotion)"));
}

// ------------------------- Web: dashboard -------------------------
// Phosphor "signals console" dashboard. Served once from PROGMEM; the browser
// then polls the tiny /data JSON and keeps its own history for the scope.
static const char PAGE[] PROGMEM = R"=====(<!doctype html><html lang=en><head>
<meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name=theme-color content="#04080a">
<title>WiFi Radar // Console</title>
<link rel=preconnect href="https://fonts.googleapis.com">
<link rel=preconnect href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Chakra+Petch:wght@500;600;700&family=Share+Tech+Mono&display=swap" rel=stylesheet>
<style>
*{box-sizing:border-box;margin:0;padding:0}
:root{--ink:#04080a;--edge:rgba(47,230,194,.16);--phos:#2fe6c2;--amber:#ffb000;--alert:#ff3b3b;--txt:#bfd6cf;--mut:#5e7570}
html,body{height:100%}
body{font-family:"Share Tech Mono",ui-monospace,monospace;color:var(--txt);
 background:radial-gradient(1100px 520px at 50% -8%,rgba(47,230,194,.10),transparent 62%),
  radial-gradient(800px 800px at 92% 112%,rgba(255,176,0,.05),transparent 60%),var(--ink);
 -webkit-font-smoothing:antialiased;padding:20px 16px 40px;position:relative;overflow-x:hidden}
body::before{content:"";position:fixed;inset:0;pointer-events:none;z-index:9;
 background:repeating-linear-gradient(0deg,rgba(0,0,0,0) 0 2px,rgba(0,0,0,.16) 2px 3px);mix-blend-mode:multiply;opacity:.5}
body::after{content:"";position:fixed;inset:0;pointer-events:none;z-index:9;
 background:radial-gradient(120% 90% at 50% 38%,transparent 60%,rgba(0,0,0,.55) 100%)}
.wrap{max-width:600px;margin:0 auto;position:relative;z-index:1;animation:rise .6s cubic-bezier(.2,.7,.2,1) both}
@keyframes rise{from{opacity:0;transform:translateY(10px)}}
h1,.tag,.state-title,.k,button,.rec,a.navlink{font-family:"Chakra Petch",sans-serif}
header{display:flex;align-items:center;gap:14px;margin-bottom:18px}
.title{flex:1;min-width:0}
h1{font-size:19px;font-weight:700;letter-spacing:.34em;color:#dff5ef}
h1 b{color:var(--phos)}
.tag{font-size:11px;letter-spacing:.2em;color:var(--mut);margin-top:3px}
.rec{display:flex;align-items:center;gap:7px;font-size:11px;letter-spacing:.16em;color:var(--mut);white-space:nowrap}
.rec .led{width:7px;height:7px;border-radius:50%;background:var(--alert);box-shadow:0 0 8px var(--alert);animation:blink 1.3s infinite}
@keyframes blink{50%{opacity:.2}}
.sweep{--swp:var(--phos);width:48px;height:48px;border-radius:50%;flex:none;position:relative;
 background:repeating-radial-gradient(circle at 50% 50%,transparent 0 6px,rgba(47,230,194,.10) 6px 7px);
 border:1px solid var(--edge);overflow:hidden}
.sweep::before{content:"";position:absolute;inset:-1px;border-radius:50%;
 background:conic-gradient(from 0deg,var(--swp) 0deg,transparent 75deg);opacity:.85;animation:spin 1.9s linear infinite}
.sweep::after{content:"";position:absolute;inset:42%;border-radius:50%;background:var(--swp);box-shadow:0 0 9px var(--swp)}
@keyframes spin{to{transform:rotate(360deg)}}
.hud{position:relative;border:1px solid var(--edge);background:linear-gradient(180deg,rgba(47,230,194,.03),transparent)}
.hud::before,.hud::after{content:"";position:absolute;width:13px;height:13px;pointer-events:none}
.hud::before{top:-1px;left:-1px;border-top:2px solid currentColor;border-left:2px solid currentColor}
.hud::after{bottom:-1px;right:-1px;border-bottom:2px solid currentColor;border-right:2px solid currentColor}
.status{color:var(--acc,var(--phos));display:flex;align-items:center;gap:18px;padding:18px;border-radius:4px;transition:.35s}
.status .sweep{width:58px;height:58px}
.info{flex:1;min-width:0}
.state-title{font-size:27px;font-weight:700;letter-spacing:.06em;line-height:1;color:var(--acc,var(--phos))}
.state-sub{font-family:"Share Tech Mono",monospace;font-size:12px;color:var(--mut);margin-top:7px;letter-spacing:.06em}
.meter{height:6px;border-radius:3px;background:rgba(255,255,255,.06);margin-top:12px;overflow:hidden}
.meter i{display:block;height:100%;width:0;background:var(--acc,var(--phos));box-shadow:0 0 10px var(--acc,var(--phos));transition:width .25s}
.s-cal{--acc:var(--amber)}.s-cal .sweep{--swp:var(--amber)}
.s-clear{--acc:var(--phos)}.s-clear .sweep{--swp:var(--phos)}
.s-motion{--acc:var(--alert);animation:alarm 1.1s ease-in-out infinite}.s-motion .sweep{--swp:var(--alert)}
@keyframes alarm{50%{box-shadow:0 0 34px rgba(255,59,59,.35)}}
.scope{color:var(--phos);margin:14px 0;border-radius:4px;overflow:hidden;position:relative}
canvas{display:block;width:100%;height:210px}
.scan{position:absolute;inset:0;pointer-events:none;background:repeating-linear-gradient(0deg,rgba(0,0,0,0) 0 3px,rgba(0,0,0,.22) 3px 4px);opacity:.35}
.tele{display:grid;grid-template-columns:repeat(2,1fr);gap:10px;margin-top:4px}
@media(min-width:480px){.tele{grid-template-columns:repeat(4,1fr)}}
.cell{color:var(--phos);padding:12px;border-radius:4px}
.cell.off{color:var(--mut)}
.k{font-size:10px;letter-spacing:.16em;color:var(--mut);display:flex;justify-content:space-between;align-items:center;min-height:13px}
.val{font-family:"Share Tech Mono",monospace;font-size:21px;color:var(--txt);margin-top:9px;letter-spacing:.02em}
.val u{font-style:normal;font-size:11px;color:var(--mut)}
.bars{display:inline-flex;gap:2px;align-items:flex-end;height:13px}
.bars b{width:3px;height:40%;background:var(--mut);opacity:.28;border-radius:1px}
.bars b.on{background:var(--phos);opacity:1;box-shadow:0 0 5px var(--phos)}
button,a.navlink{margin-top:16px;width:100%;padding:14px;border:1px solid var(--edge);border-radius:4px;cursor:pointer;
 background:linear-gradient(180deg,rgba(47,230,194,.14),rgba(47,230,194,.04));color:var(--phos);
 font-size:13px;font-weight:600;letter-spacing:.22em;text-transform:uppercase;transition:.18s;
 display:block;text-align:center;text-decoration:none}
button:hover,a.navlink:hover{background:linear-gradient(180deg,rgba(47,230,194,.22),rgba(47,230,194,.08))}
button:active{transform:translateY(1px)}
.foot{text-align:center;color:var(--mut);font-size:10px;letter-spacing:.13em;margin-top:14px}
</style></head><body>
<div class=wrap>
 <header>
  <div class=sweep></div>
  <div class=title><h1>WIFI&nbsp;<b>RADAR</b></h1><div class=tag>// RSSI MOTION CONSOLE</div></div>
  <div class=rec><span class=led></span><span id=rec>LIVE</span></div>
 </header>
 <div class="status hud s-cal" id=status>
  <div class=sweep></div>
  <div class=info>
   <div class=state-title id=stitle>BOOT</div>
   <div class=state-sub id=ssub>establishing link...</div>
   <div class=meter><i id=mfill></i></div>
  </div>
 </div>
 <div class="scope hud">
  <canvas id=g></canvas>
  <div class=scan></div>
 </div>
 <div class=tele>
  <div class="cell hud"><div class=k>SIGNAL <span class=bars id=bars><b></b><b></b><b></b><b></b></span></div><div class=val><span id=rssi>--</span> <u>dBm</u></div></div>
  <div class="cell hud"><div class=k>SCORE</div><div class=val><span id=score>--</span> <u>x</u></div></div>
  <div class="cell hud"><div class=k>VARIANCE</div><div class=val><span id=std>--</span> <u>dB</u></div></div>
  <div class="cell hud"><div class=k>UPTIME</div><div class=val id=up>--</div></div>
  <div class="cell hud" id=sensorCell><div class=k>SENSOR</div><div class=val id=sensorVal>N/A</div></div>
 </div>
 <button onclick="recal()">&#8635; Recalibrate Baseline</button>
 <a class=navlink href="/config">&#9881; Configuration</a>
 <div class=foot id=foot>awaiting telemetry...</div>
</div>
<script>
const $=s=>document.getElementById(s);
const cv=$('g'),cx=cv.getContext('2d');
let W=0,H=0,DPR=1;
function fit(){DPR=Math.min(devicePixelRatio||1,2);W=cv.clientWidth;H=cv.clientHeight;cv.width=W*DPR;cv.height=H*DPR;cx.setTransform(DPR,0,0,DPR,0,0);}
addEventListener('resize',fit);
const N=160,INT=220;
let pts=[],lastT=0,trip=2.2,motion=false,sc=0,scT=0;
function fmtUp(s){let h=s/3600|0,m=(s%3600)/60|0,x=s%60;return (h?h+'h ':'')+m+'m '+(x<10?'0':'')+x+'s';}
function poll(){
 fetch('/data').then(r=>r.json()).then(d=>{
  trip=d.trip;motion=d.motion;
  pts.push(d.score);if(pts.length>N)pts.shift();lastT=performance.now();scT=d.score;
  const st=$('status');
  if(!d.calibrated){st.className='status hud s-cal';$('stitle').textContent='CALIBRATING';$('ssub').textContent='learning empty-room baseline';$('mfill').style.width=(d.cal/d.calTotal*100)+'%';}
  else if(d.motion){st.className='status hud s-motion';$('stitle').textContent='MOTION';$('ssub').textContent='movement detected in range';$('mfill').style.width='100%';}
  else{st.className='status hud s-clear';$('stitle').textContent='ALL CLEAR';$('ssub').textContent='no movement / monitoring';$('mfill').style.width=Math.min(100,d.score/trip*100)+'%';}
  $('rssi').textContent=d.rssi;$('std').textContent=d.std.toFixed(2);$('up').textContent=fmtUp(d.up);
  const sc2=$('sensorCell'),sv=$('sensorVal');
  if(d.radarEnabled){sc2.classList.remove('off');sv.textContent=d.radarMotion?'MOTION':'CLEAR';}
  else{sc2.classList.add('off');sv.textContent='N/A';}
  const q=Math.max(0,Math.min(4,Math.round((d.rssi+95)/11))),b=$('bars').children;
  for(let i=0;i<4;i++){b[i].className=i<q?'on':'';b[i].style.height=(40+i*20)+'%';}
  $('foot').textContent='LINK '+d.rssi+' dBm / BASELINE '+d.baseline.toFixed(2)+' dB / CH '+d.ch+' / '+(d.calibrated?'ARMED':'CAL');
  $('rec').textContent='LIVE';
 }).catch(()=>{$('foot').textContent='// SIGNAL LOST - reconnecting...';$('rec').textContent='----';});
}
function scope(now){
 requestAnimationFrame(scope);
 cx.clearRect(0,0,W,H);
 const pad=12,maxS=Math.max(3,trip*1.7),yOf=v=>H-pad-(Math.max(0,v)/maxS)*(H-2*pad);
 cx.lineWidth=1;cx.strokeStyle='rgba(47,230,194,.07)';
 for(let i=1;i<6;i++){let X=W*i/6;cx.beginPath();cx.moveTo(X,0);cx.lineTo(X,H);cx.stroke();}
 for(let i=1;i<4;i++){let Y=H*i/4;cx.beginPath();cx.moveTo(0,Y);cx.lineTo(W,Y);cx.stroke();}
 const ty=yOf(trip);
 cx.strokeStyle='rgba(255,176,0,.5)';cx.setLineDash([6,6]);cx.beginPath();cx.moveTo(0,ty);cx.lineTo(W,ty);cx.stroke();cx.setLineDash([]);
 cx.fillStyle='rgba(255,176,0,.7)';cx.font='10px "Share Tech Mono",monospace';cx.fillText('TRIP '+trip.toFixed(1)+'x',8,ty-6);
 if(pts.length>1){
  const step=W/(N-1),frac=Math.min(1,(now-lastT)/INT),n=pts.length,X=i=>W-(n-1-i+(1-frac))*step;
  const grd=cx.createLinearGradient(0,pad,0,H-pad);
  grd.addColorStop(0,'#ff3b3b');grd.addColorStop(.45,'#ffb000');grd.addColorStop(1,'#2fe6c2');
  cx.beginPath();cx.moveTo(X(0),H-pad);
  for(let i=0;i<n;i++)cx.lineTo(X(i),yOf(pts[i]));
  cx.lineTo(X(n-1),H-pad);cx.closePath();
  const fg=cx.createLinearGradient(0,0,0,H);
  fg.addColorStop(0,motion?'rgba(255,59,59,.26)':'rgba(47,230,194,.20)');fg.addColorStop(1,'rgba(47,230,194,0)');
  cx.fillStyle=fg;cx.fill();
  cx.beginPath();for(let i=0;i<n;i++){let xx=X(i),yy=yOf(pts[i]);i?cx.lineTo(xx,yy):cx.moveTo(xx,yy);}
  cx.strokeStyle=motion?'rgba(255,59,59,.25)':'rgba(47,230,194,.22)';cx.lineWidth=7;cx.stroke();
  cx.beginPath();for(let i=0;i<n;i++){let xx=X(i),yy=yOf(pts[i]);i?cx.lineTo(xx,yy):cx.moveTo(xx,yy);}
  cx.strokeStyle=motion?'#ff5252':grd;cx.lineWidth=2;cx.shadowBlur=12;
  cx.shadowColor=motion?'rgba(255,59,59,.85)':'rgba(47,230,194,.7)';cx.stroke();cx.shadowBlur=0;
  let bx=X(n-1),by=yOf(pts[n-1]);
  cx.fillStyle=motion?'#ff5252':'#2fe6c2';cx.beginPath();cx.arc(bx,by,3,0,7);cx.fill();
  cx.fillStyle=motion?'rgba(255,59,59,.22)':'rgba(47,230,194,.22)';cx.beginPath();cx.arc(bx,by,8,0,7);cx.fill();
 }
 sc+=(scT-sc)*.16;$('score').textContent=sc.toFixed(2);
}
function recal(){fetch('/recal');$('status').className='status hud s-cal';$('stitle').textContent='CALIBRATING';}
fit();requestAnimationFrame(scope);poll();setInterval(poll,INT);
</script></body></html>)=====";

void handleRoot() { server.send_P(200, PSTR("text/html"), PAGE); }

void handleData() {
  // Tiny fixed-size payload built on the stack — no String heap churn, no
  // history array. The browser keeps its own scope history.
  char buf[288];
  snprintf(buf, sizeof(buf),
    "{\"rssi\":%d,\"std\":%.2f,\"score\":%.2f,\"motion\":%s,\"calibrated\":%s,"
    "\"cal\":%d,\"calTotal\":%d,\"baseline\":%.2f,\"trip\":%.2f,\"up\":%lu,\"ch\":%d,"
    "\"radarEnabled\":%s,\"radarMotion\":%s,\"snmpEnabled\":%s}",
    (int)lastRSSI, windowStd(), lastScore,
    motion ? "true" : "false", calibrated ? "true" : "false",
    (int)calCollected, (int)CAL_SAMPLES, baselineStd, TRIP_FACTOR,
    (unsigned long)(millis() / 1000), WiFi.channel(),
    cfg.radarEnabled ? "true" : "false",
    (cfg.radarEnabled && radarMotion) ? "true" : "false",
    cfg.snmpEnabled ? "true" : "false");
  server.send(200, "application/json", buf);
}

// ------------------------- Web: configuration page -------------------------
void handleConfigGet() {
  String pinOptions;
  for (uint8_t p : RADAR_PIN_CHOICES) {
    pinOptions += "<option value=" + String(p) + (p == cfg.radarPin ? " selected" : "") +
                  ">GPIO" + String(p) + "</option>";
  }
  String page;
  page.reserve(3200);
  page += F("<!doctype html><html lang=en><head><meta charset=utf-8>"
             "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
             "<title>WiFi Radar // Config</title>"
             "<style>"
             "*{box-sizing:border-box}body{font-family:ui-monospace,monospace;background:#04080a;color:#bfd6cf;"
             "max-width:480px;margin:0 auto;padding:20px 16px 40px}"
             "h1{font-size:18px;letter-spacing:.1em;color:#dff5ef;margin-bottom:18px}"
             "fieldset{border:1px solid rgba(47,230,194,.25);border-radius:4px;margin-bottom:16px;padding:14px}"
             "legend{color:#2fe6c2;font-size:12px;letter-spacing:.14em;padding:0 6px}"
             "label{display:block;font-size:12px;color:#5e7570;margin:10px 0 4px}"
             "input[type=text],select{width:100%;padding:9px;background:#0a1210;border:1px solid rgba(47,230,194,.25);"
             "border-radius:4px;color:#bfd6cf;font-family:inherit}"
             "input[type=checkbox]{transform:scale(1.3);margin-right:8px}"
             ".row{display:flex;align-items:center;margin:8px 0}"
             "button,a.back{display:block;width:100%;margin-top:10px;padding:13px;border:1px solid rgba(47,230,194,.3);"
             "border-radius:4px;background:linear-gradient(180deg,rgba(47,230,194,.14),rgba(47,230,194,.04));"
             "color:#2fe6c2;font-size:13px;letter-spacing:.16em;text-transform:uppercase;cursor:pointer;text-align:center;"
             "text-decoration:none}"
             "small{color:#5e7570;display:block;margin-top:4px;font-size:11px;line-height:1.5}"
             ".warn{border:1px solid rgba(255,82,82,.4);background:rgba(255,82,82,.08);color:#ff9d9d;"
             "border-radius:4px;padding:12px;font-size:12px;line-height:1.6;margin-bottom:16px}"
             "</style></head><body><h1>WIFI RADAR // CONFIGURATION</h1>");

  if (!fsAvailable) {
    page += F("<div class=warn><b>No usable filesystem on this board.</b> Settings below can be "
               "changed and saved, but won't actually persist — every reboot resets them to "
               "defaults. This almost always means platformio.ini's flash size/board setting "
               "doesn't match this chip, or (on a 1&nbsp;MB module) its linker script reserves no "
               "space for a filesystem. Check serial at 115200 for the exact [fs] message, and see "
               "the flash-size note at the top of platformio.ini.</div>");
  }

  page += F("<form method=POST action=/config>");

  page += F("<fieldset><legend>External motion sensor</legend>"
             "<div class=row><input type=checkbox name=radarEnabled id=re");
  page += cfg.radarEnabled ? F(" checked") : F("");
  page += F("><label for=re style=\"margin:0\">Enable external sensor (PIR / RCWL-0516 etc.)</label></div>"
             "<label>Input pin</label><select name=radarPin>");
  page += pinOptions;
  page += F("</select><small>GPIO2 (the onboard LED) and GPIO1/3 (Serial TX/RX) aren't offered — they're "
             "already in use. Pick whichever GPIO you actually wired the sensor to.</small>"
             "<div class=row style=\"margin-top:12px\"><input type=checkbox name=radarActiveHigh id=ah");
  page += cfg.radarActiveHigh ? F(" checked") : F("");
  page += F("><label for=ah style=\"margin:0\">Sensor output is active-HIGH on motion</label></div>"
             "<small>Most PIR and RCWL-0516 modules are active-high (idle LOW, pulse HIGH on motion). "
             "Uncheck this if yours works the other way around.</small>"
             "</fieldset>");

  page += F("<fieldset><legend>SNMP agent</legend>"
             "<div class=row><input type=checkbox name=snmpEnabled id=se");
  page += cfg.snmpEnabled ? F(" checked") : F("");
  page += F("><label for=se style=\"margin:0\">Enable SNMP (v2c, read-only, UDP 161)</label></div>"
             "<label>Community string</label><input type=text name=snmpCommunity maxlength=23 value=\"");
  page += cfg.snmpCommunity;
  page += F("\"><label>sysName</label><input type=text name=sysName maxlength=31 value=\"");
  page += cfg.sysName;
  page += F("\"><label>sysLocation</label><input type=text name=sysLocation maxlength=31 value=\"");
  page += cfg.sysLocation;
  page += F("\"><small>Point your poller (Zabbix, etc.) at this board's IP, UDP 161, with the community "
             "string above. Custom OIDs live under <code>.1.3.6.1.4.1.52145.1.*</code> — "
             "1=rssi 2=scoreX100 3=stdX100 4=baselineX100 5=calibrated 6=motion 7=channel "
             "8=uptimeSec 9=radarEnabled 10=radarMotion. That enterprise number is a placeholder, "
             "not a real registered PEN — fine for internal use, get your own from IANA before "
             "using this scheme more widely.</small>"
             "</fieldset>");

  page += F("<button type=submit>Save &amp; Reboot</button></form>"
             "<a class=back href=/>&larr; Back to dashboard</a></body></html>");
  server.send(200, "text/html", page);
}

void handleConfigPost() {
  cfg.radarEnabled    = server.hasArg("radarEnabled");
  cfg.radarActiveHigh = server.hasArg("radarActiveHigh");
  if (server.hasArg("radarPin")) {
    uint8_t pin = (uint8_t)server.arg("radarPin").toInt();
    cfg.radarPin = isAllowedRadarPin(pin) ? pin : cfg.radarPin;
  }
  cfg.snmpEnabled = server.hasArg("snmpEnabled");
  if (server.hasArg("snmpCommunity") && server.arg("snmpCommunity").length() > 0)
    strlcpy(cfg.snmpCommunity, server.arg("snmpCommunity").c_str(), sizeof(cfg.snmpCommunity));
  if (server.hasArg("sysName"))
    strlcpy(cfg.sysName, server.arg("sysName").c_str(), sizeof(cfg.sysName));
  if (server.hasArg("sysLocation"))
    strlcpy(cfg.sysLocation, server.arg("sysLocation").c_str(), sizeof(cfg.sysLocation));

  bool ok = saveConfig();
  String page = "<body style='font-family:monospace;background:#04080a;padding:40px;"
                "text-align:center;max-width:420px;margin:0 auto;line-height:1.6'>";
  if (ok) {
    page += "<span style='color:#bfd6cf'>Saved. Rebooting to apply...</span>";
  } else if (!fsAvailable) {
    page += "<span style='color:#ff9d9d'>Not saved — this board has no usable filesystem "
            "(see the config page's warning banner, or check serial for the exact [fs] "
            "message). The settings above took effect for THIS session only and will "
            "reset on the next reboot.</span>"
            "<br><br><a style='color:#2fe6c2' href=/config>&larr; Back</a>";
  } else {
    page += "<span style='color:#ff5252'>Failed to write config.json — the filesystem is "
            "mounted but the write itself failed (see serial for the exact reason). "
            "Settings NOT saved.</span>"
            "<br><br><a style='color:#2fe6c2' href=/config>&larr; Back</a>";
  }
  page += "</body>";
  server.send(200, "text/html", page);
  if (ok) {
    delay(400);
    ESP.restart();
  }
}

// ------------------------- Setup / Loop -------------------------
void setup() {
  pinMode(LED, OUTPUT);
  digitalWrite(LED, HIGH);          // off
  Serial.begin(115200);
  delay(50);
  Serial.println(F("\n\n=== WiFi Radar :: RSSI motion sensing ==="));

  // Touching the flash filesystem is only safe if the flash size this
  // firmware was BUILT for (platformio.ini's `board`, which sets the linker's
  // assumed layout) matches the flash size actually ON the chip. If they
  // don't match — most commonly because this is flashed onto a 1 MB module
  // (e.g. an ESP-01) while platformio.ini's `board = esp12e` assumes 4 MB —
  // LittleFS.begin()/format() can read/write/erase flash addresses that
  // don't physically exist, which can hang or crash the chip in a boot loop
  // instead of failing cleanly. Detect that up front and skip the filesystem
  // entirely rather than risk it; the board still runs fine, it just can't
  // persist /config.json (radar sensor + SNMP settings reset to defaults
  // every boot) until platformio.ini's board/flash size is corrected to
  // match the real hardware.
  fsAvailable = (ESP.getFlashChipRealSize() >= ESP.getFlashChipSize());
  if (!fsAvailable) {
    Serial.printf(
      "[fs] WARNING: firmware was built for %u bytes of flash but this chip "
      "reports %u bytes. Skipping the filesystem — config will NOT persist. "
      "Fix platformio.ini's `board` (and board_build.flash_size) to match "
      "your actual module, then re-flash.\n",
      (unsigned)ESP.getFlashChipSize(), (unsigned)ESP.getFlashChipRealSize());
  } else if (!LittleFS.begin()) {
    Serial.println(F("[fs] LittleFS mount failed — formatting..."));
    if (!LittleFS.format() || !LittleFS.begin()) {
      Serial.println(F("[fs] LittleFS still unavailable after format. This usually means "
                        "the board's linker script reserves little or no space for a "
                        "filesystem (common on default 1 MB board layouts) — check "
                        "platformio.ini's board_build.ldscript. Continuing without "
                        "persistent config."));
      fsAvailable = false;
    }
  }
  if (fsAvailable) {
    FSInfo info;
    LittleFS.info(info);
    Serial.printf("[fs] LittleFS mounted: %u / %u bytes used\n",
                  (unsigned)info.usedBytes, (unsigned)info.totalBytes);
    loadConfig();
  }
  if (cfg.radarEnabled) {
    pinMode(cfg.radarPin, INPUT);
    Serial.printf("[radar] External sensor enabled on GPIO%d (active-%s)\n",
                  cfg.radarPin, cfg.radarActiveHigh ? "HIGH" : "LOW");
  }

  // WiFiManager: tries the credentials saved in flash from a previous
  // setup. If none are saved, or they fail to connect, it brings up the
  // "WiFiRadar-Setup" access point + captive portal so you can pick a
  // network from a phone or laptop — no recompile needed.
  WiFi.mode(WIFI_STA);
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);   // give up and retry later if nobody configures it
  Serial.println(F("Connecting to saved WiFi (or opening setup portal)..."));
  if (!wm.autoConnect("WiFiRadar-Setup", "radarsetup")) {
    Serial.println(F("Setup portal timed out with no WiFi configured — restarting in 3 s."));
    delay(3000);
    ESP.restart();
  }

  Serial.printf("Connected.  IP %s  ch %d  RSSI %d dBm\n",
                WiFi.localIP().toString().c_str(), WiFi.channel(), (int)WiFi.RSSI());
  gatewayIP = WiFi.gatewayIP();
  udp.begin(2390);

  if (MDNS.begin("wifiradar")) {
    MDNS.addService("http", "tcp", 80);
    if (cfg.snmpEnabled) MDNS.addService("snmp", "udp", 161);
    Serial.println(F("Dashboard: http://wifiradar.local  (or the IP above)"));
  }

  setupSnmp();   // no-op if SNMP is disabled in config

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/recal", []() { recalibrate(); server.send(200, "text/plain", "ok"); });
  server.on("/config", HTTP_GET, handleConfigGet);
  server.on("/config", HTTP_POST, handleConfigPost);
  server.on("/resetwifi", []() {
    server.send(200, "text/plain", "WiFi credentials erased. Restarting into setup portal \"WiFiRadar-Setup\"...");
    delay(300);
    WiFiManager wmReset;
    wmReset.resetSettings();
    ESP.restart();
  });
  server.begin();

  recalibrate();                    // begin baseline calibration
  Serial.println(F("Watch the meter below, or open the dashboard.\n"));
}

void loop() {
  server.handleClient();
  MDNS.update();
  if (snmpAgent) snmpAgent->loop();

  // External sensor is read every loop tick regardless of the RSSI sample
  // timing below, so it stays responsive even while calibration is running.
  if (cfg.radarEnabled) {
    int lvl = digitalRead(cfg.radarPin);
    radarMotion = cfg.radarActiveHigh ? (lvl == HIGH) : (lvl == LOW);
  } else {
    radarMotion = false;
  }
  snmpRadarEnabled = cfg.radarEnabled ? 1 : 0;
  snmpRadarMotion  = radarMotion ? 1 : 0;
  snmpUptimeSec    = millis() / 1000;
  snmpChannel      = WiFi.channel();

  uint32_t now = millis();
  if (now - lastSampleMs < SAMPLE_INTERVAL_MS) return;
  lastSampleMs = now;

  poke();
  int32_t rssi = WiFi.RSSI();
  if (rssi >= 0 || rssi < -100) return;   // guard invalid readings
  lastRSSI = rssi;
  snmpRssi = (int)rssi;
  float x = (float)rssi;
  pushSample(x);

  // ---- calibration phase ----
  if (!calibrated) {
    calN++;
    float delta = x - calMean;
    calMean += delta / calN;
    calM2   += delta * (x - calMean);
    calCollected = (uint16_t)calN;
    snmpCalibrated = 0;
    if (calN >= CAL_SAMPLES) {
      baselineStd = sqrtf(calM2 / calN);
      if (baselineStd < MIN_BASELINE_STD) baselineStd = MIN_BASELINE_STD;
      calibrated = true;
      snmpCalibrated = 1;
      snmpBaselineX100 = (uint32_t)(baselineStd * 100.0f);
      Serial.printf("\n[cal] Done. baseline RSSI std = %.2f dB. Watching for motion...\n\n", baselineStd);
    } else if (calN % 40 == 0) {
      Serial.printf("[cal] %d / %d\n", (int)calN, (int)CAL_SAMPLES);
    }
    return;
  }

  // ---- live detection ----
  float stdv  = windowStd();
  float score = stdv / baselineStd;
  lastScore = score;
  snmpScoreX100 = (uint32_t)(score * 100.0f);
  snmpStdX100   = (uint32_t)(stdv * 100.0f);

  if (!motion && score >= TRIP_FACTOR) {
    motion = true;
    motionSince = now;
  } else if (motion && score < CLEAR_FACTOR && (now - motionSince) > MOTION_HOLD_MS) {
    motion = false;
  }
  snmpMotion = motion ? 1 : 0;
  // Drive the onboard LED from either detector so it stays a useful
  // at-a-glance indicator whether or not the external sensor is in use.
  bool anyMotion = motion || (cfg.radarEnabled && radarMotion);
  digitalWrite(LED, anyMotion ? LOW : HIGH);   // active-low

  printMeter(rssi, stdv, score);
}
