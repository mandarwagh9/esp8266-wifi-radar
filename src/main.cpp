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
 *
 * WiFi setup: credentials are read from src/secrets.h (gitignored), so the
 * board connects to your network automatically on boot.
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <math.h>
#include "secrets.h"   // WIFI_SSID / WIFI_PASS

// ------------------------- Tunables -------------------------
static const uint32_t SAMPLE_INTERVAL_MS = 50;    // ~20 Hz RSSI sampling
static const uint16_t WINDOW             = 64;    // live sliding window (~3.2 s)
static const uint16_t CAL_SAMPLES        = 200;   // calibration length (~10 s)
static const float    TRIP_FACTOR        = 2.2f;  // score to ENTER motion
static const float    CLEAR_FACTOR       = 1.5f;  // score to LEAVE motion
static const uint32_t MOTION_HOLD_MS     = 2000;  // min time held in MOTION
static const float    MIN_BASELINE_STD   = 0.8f;  // floor so a dead-quiet room can't div-by-~0

// ------------------------- State -------------------------
ESP8266WebServer server(80);
WiFiUDP   udp;
IPAddress gatewayIP;

const uint8_t LED = LED_BUILTIN;   // GPIO2 on ESP-12 modules, active-low

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

uint32_t lastSampleMs = 0;

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
  Serial.printf("RSSI %4d dBm | std %5.2f | x%5.2f [%s] %s\n",
                (int)rssi, stdv, score, bar, motion ? "** MOTION **" : "idle");
}

// ------------------------- Web -------------------------
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
h1,.tag,.state-title,.k,button,.rec{font-family:"Chakra Petch",sans-serif}
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
.k{font-size:10px;letter-spacing:.16em;color:var(--mut);display:flex;justify-content:space-between;align-items:center;min-height:13px}
.val{font-family:"Share Tech Mono",monospace;font-size:21px;color:var(--txt);margin-top:9px;letter-spacing:.02em}
.val u{font-style:normal;font-size:11px;color:var(--mut)}
.bars{display:inline-flex;gap:2px;align-items:flex-end;height:13px}
.bars b{width:3px;height:40%;background:var(--mut);opacity:.28;border-radius:1px}
.bars b.on{background:var(--phos);opacity:1;box-shadow:0 0 5px var(--phos)}
button{margin-top:16px;width:100%;padding:14px;border:1px solid var(--edge);border-radius:4px;cursor:pointer;
 background:linear-gradient(180deg,rgba(47,230,194,.14),rgba(47,230,194,.04));color:var(--phos);
 font-size:13px;font-weight:600;letter-spacing:.22em;text-transform:uppercase;transition:.18s}
button:hover{background:linear-gradient(180deg,rgba(47,230,194,.22),rgba(47,230,194,.08))}
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
 </div>
 <button onclick="recal()">&#8635; Recalibrate Baseline</button>
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
  char buf[224];
  snprintf(buf, sizeof(buf),
    "{\"rssi\":%d,\"std\":%.2f,\"score\":%.2f,\"motion\":%s,\"calibrated\":%s,"
    "\"cal\":%d,\"calTotal\":%d,\"baseline\":%.2f,\"trip\":%.2f,\"up\":%lu,\"ch\":%d}",
    (int)lastRSSI, windowStd(), lastScore,
    motion ? "true" : "false", calibrated ? "true" : "false",
    (int)calCollected, (int)CAL_SAMPLES, baselineStd, TRIP_FACTOR,
    (unsigned long)(millis() / 1000), WiFi.channel());
  server.send(200, "application/json", buf);
}

// ------------------------- Setup / Loop -------------------------
void setup() {
  pinMode(LED, OUTPUT);
  digitalWrite(LED, HIGH);          // off
  Serial.begin(115200);
  delay(50);
  Serial.println(F("\n\n=== WiFi Radar :: RSSI motion sensing ==="));

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("Connecting to \"%s\"", WIFI_SSID);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 30000) {
    delay(400);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("WiFi connect failed — restarting in 3 s."));
    delay(3000);
    ESP.restart();
  }

  Serial.printf("Connected.  IP %s  ch %d  RSSI %d dBm\n",
                WiFi.localIP().toString().c_str(), WiFi.channel(), (int)WiFi.RSSI());
  gatewayIP = WiFi.gatewayIP();
  udp.begin(2390);

  if (MDNS.begin("wifiradar")) {
    MDNS.addService("http", "tcp", 80);
    Serial.println(F("Dashboard: http://wifiradar.local  (or the IP above)"));
  }

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/recal", []() { recalibrate(); server.send(200, "text/plain", "ok"); });
  server.begin();

  recalibrate();                    // begin baseline calibration
  Serial.println(F("Watch the meter below, or open the dashboard.\n"));
}

void loop() {
  server.handleClient();
  MDNS.update();

  uint32_t now = millis();
  if (now - lastSampleMs < SAMPLE_INTERVAL_MS) return;
  lastSampleMs = now;

  poke();
  int32_t rssi = WiFi.RSSI();
  if (rssi >= 0 || rssi < -100) return;   // guard invalid readings
  lastRSSI = rssi;
  float x = (float)rssi;
  pushSample(x);

  // ---- calibration phase ----
  if (!calibrated) {
    calN++;
    float delta = x - calMean;
    calMean += delta / calN;
    calM2   += delta * (x - calMean);
    calCollected = (uint16_t)calN;
    if (calN >= CAL_SAMPLES) {
      baselineStd = sqrtf(calM2 / calN);
      if (baselineStd < MIN_BASELINE_STD) baselineStd = MIN_BASELINE_STD;
      calibrated = true;
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

  if (!motion && score >= TRIP_FACTOR) {
    motion = true;
    motionSince = now;
  } else if (motion && score < CLEAR_FACTOR && (now - motionSince) > MOTION_HOLD_MS) {
    motion = false;
  }
  digitalWrite(LED, motion ? LOW : HIGH);   // active-low

  printMeter(rssi, stdv, score);
}
