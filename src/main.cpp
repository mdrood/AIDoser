#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <math.h>

// ===================== WIFI / WEB SETUP =====================

// You can change these if you want
const char* AP_SSID     = "ReefDoserESP32";
const char* AP_PASSWORD = "reefpassword";

WebServer server(80);

// ===================== TARGETS & TANK INFO =====================

// Your target parameters
const float TARGET_ALK = 9.0f;      // dKH
const float TARGET_CA  = 420.0f;    // ppm
const float TARGET_MG  = 1440.0f;   // ppm
const float TARGET_PH  = 8.20f;     // NEW FOR pH

// Your 300 gallon system
// 300 gal × 3.78541 = 1135.6 L
const float TANK_VOLUME_L = 1135.6f;


// ===================== PUMP PINS & FLOW RATES =====================

// Set your actual GPIO pins here:
const int PIN_PUMP_KALK = 25;
const int PIN_PUMP_AFR  = 26;
const int PIN_PUMP_MG   = 27;

// ---- FLOW RATES ----
// Measure each pump: run for 60 seconds into a cup, measure ml.
float FLOW_KALK_ML_PER_MIN = 50.0f;  
float FLOW_AFR_ML_PER_MIN  = 50.0f;
float FLOW_MG_ML_PER_MIN   = 50.0f;


// ===================== CHEMISTRY CONSTANTS =====================
//
// These are the “ballpark guessed values” so no calibration experiments needed.
// The AI portion will tune ml/day automatically as you test every ~3 days.
//

// ---------- KALKWASSER (saturated) ----------
float DKH_PER_ML_KALK_TANK    = 0.00010f;   // +0.00010 dKH/ml in 300g
float CA_PPM_PER_ML_KALK_TANK = 0.00070f;   // +0.00070 ppm Ca/ml

// ---------- TROPIC MARIN ALL-FOR-REEF ----------
float DKH_PER_ML_AFR_TANK     = 0.0052f;    // +0.0052 dKH/ml in 300g
float CA_PPM_PER_ML_AFR_TANK  = 0.037f;     // +0.037 ppm Ca/ml
float MG_PPM_PER_ML_AFR_TANK  = 0.006f;     // +0.006 ppm Mg/ml

// ---------- MAGNESIUM-ONLY ----------
float MG_PPM_PER_ML_MG_TANK   = 0.20f;      // +0.20 ppm Mg/ml


// ===================== DOSING CONFIG & TEST DATA =====================

struct TestPoint {
  uint32_t t;  // seconds since boot
  float ca;
  float alk;
  float mg;
  float ph;    // NEW FOR pH
};

struct DosingConfig {
  float ml_per_day_kalk;
  float ml_per_day_afr;
  float ml_per_day_mg;
};

// Initial starting doses (safe, conservative)
DosingConfig dosing = {
  2000.0f, // ml/day kalk (2L/day)
  20.0f,   // ml/day AFR
  0.0f     // ml/day Mg
};

// Safety limits (per day)
float MAX_KALK_ML_PER_DAY = 8000.0f;  // max ~8L/day
float MAX_AFR_ML_PER_DAY  = 400.0f;
float MAX_MG_ML_PER_DAY   = 80.0f;


// ===================== TEST HISTORY FOR GRAPHS =====================

const int MAX_HISTORY = 64;
TestPoint historyBuf[MAX_HISTORY];
int historyCount = 0;

TestPoint lastTest    = {0, 0, 0, 0, 0};
TestPoint currentTest = {0, 0, 0, 0, 0};


// ===================== DOSING SCHEDULE =====================

const int DOSES_PER_DAY_KALK = 24;
const int DOSES_PER_DAY_AFR  = 8;
const int DOSES_PER_DAY_MG   = 4;

float SEC_PER_DOSE_KALK = 0.0f;
float SEC_PER_DOSE_AFR  = 0.0f;
float SEC_PER_DOSE_MG   = 0.0f;


// ===================== HELPERS =====================

uint32_t nowSeconds() {
  return millis()/1000;
}

float clampf(float v, float vmin, float vmax){
  if(v < vmin) return vmin;
  if(v > vmax) return vmax;
  return v;
}

float adjustWithLimit(float current, float suggested){
  float maxChange = fabsf(current) * 0.15f;
  if(maxChange < 1.0f) maxChange = 1.0f;
  float delta = suggested - current;
  if(delta >  maxChange) delta =  maxChange;
  if(delta < -maxChange) delta = -maxChange;
  return current + delta;
}

void updatePumpSchedules() {
  if(FLOW_KALK_ML_PER_MIN > 0 && DOSES_PER_DAY_KALK > 0){
    float secondsPerDay = (dosing.ml_per_day_kalk / FLOW_KALK_ML_PER_MIN) * 60.0f;
    SEC_PER_DOSE_KALK = secondsPerDay / DOSES_PER_DAY_KALK;
  }

  if(FLOW_AFR_ML_PER_MIN > 0 && DOSES_PER_DAY_AFR > 0){
    float secondsPerDay = (dosing.ml_per_day_afr / FLOW_AFR_ML_PER_MIN) * 60.0f;
    SEC_PER_DOSE_AFR = secondsPerDay / DOSES_PER_DAY_AFR;
  }

  if(FLOW_MG_ML_PER_MIN > 0 && DOSES_PER_DAY_MG > 0){
    float secondsPerDay = (dosing.ml_per_day_mg / FLOW_MG_ML_PER_MIN) * 60.0f;
    SEC_PER_DOSE_MG = secondsPerDay / DOSES_PER_DAY_MG;
  }
}


// ===================== PUSH HISTORY =====================

void pushHistory(const TestPoint& tp){
  if(historyCount < MAX_HISTORY){
    historyBuf[historyCount++] = tp;
  } else {
    for(int i=1;i<MAX_HISTORY;i++) historyBuf[i-1] = historyBuf[i];
    historyBuf[MAX_HISTORY-1] = tp;
  }
}


// ===================== “AI” CONTROL (with pH bias) =====================

void onNewTestInput(float ca, float alk, float mg, float ph){
  lastTest = currentTest;
  currentTest.t   = nowSeconds();
  currentTest.ca  = ca;
  currentTest.alk = alk;
  currentTest.mg  = mg;
  currentTest.ph  = ph;
  pushHistory(currentTest);

  // First test: just update schedules
  if(lastTest.t == 0){
    updatePumpSchedules();
    return;
  }

  float days = float(currentTest.t - lastTest.t) / 86400.0f;
  if(days <= 0.25f) return;  // ignore if tests are too close

  // Consumption per day (positive means the tank used that much)
  float consAlk = (lastTest.alk - currentTest.alk) / days;
  float consCa  = (lastTest.ca  - currentTest.ca ) / days;
  float consMg  = (lastTest.mg  - currentTest.mg ) / days;

  // If alk is rising, don't treat as consumption
  float alkNeeded = consAlk;
  if(alkNeeded < 0.0f) alkNeeded = 0.0f;

  // --- pH-based bias: more Kalk if pH is low, less Kalk if pH is high ---
  float kalkFrac = 0.8f;  // default: 80% of alk from Kalk

  if (!isnan(currentTest.ph)) {
    float phError = currentTest.ph - TARGET_PH;

    // small deadband so it doesn't flap constantly
    if (phError < -0.05f) {
      // pH is LOW -> favor Kalk a bit more
      kalkFrac = 0.90f;   // 90% Kalk / 10% AFR
    } else if (phError > 0.05f) {
      // pH is HIGH -> favor AFR a bit more (less high-pH Kalk)
      kalkFrac = 0.70f;   // 70% Kalk / 30% AFR
    }
  }

  // clamp just in case
  kalkFrac = clampf(kalkFrac, 0.6f, 0.95f);

  float targetAlkFromKalk = kalkFrac        * alkNeeded;
  float targetAlkFromAfr  = (1.0f-kalkFrac) * alkNeeded;

  // Suggest daily ml for each source based on alk
  float suggested_ml_kalk = (DKH_PER_ML_KALK_TANK > 0.0f) ? (targetAlkFromKalk / DKH_PER_ML_KALK_TANK) : 0.0f;
  float suggested_ml_afr  = (DKH_PER_ML_AFR_TANK  > 0.0f) ? (targetAlkFromAfr  / DKH_PER_ML_AFR_TANK ) : 0.0f;

  // Predict Ca/Mg from those suggestions
  float caFromKalk = suggested_ml_kalk * CA_PPM_PER_ML_KALK_TANK;
  float caFromAfr  = suggested_ml_afr  * CA_PPM_PER_ML_AFR_TANK;
  float mgFromAfr  = suggested_ml_afr  * MG_PPM_PER_ML_AFR_TANK;

  float caError = consCa - (caFromKalk + caFromAfr);
  float mgError = consMg - (mgFromAfr);

  // Adjust AFR to help with Ca if way off
  if(fabsf(caError) > 5.0f){
    float afrCorrection = (caError / CA_PPM_PER_ML_AFR_TANK) * 0.3f;
    suggested_ml_afr += afrCorrection;
  }

  // Magnesium-only line
  float suggested_ml_mg = dosing.ml_per_day_mg;
  if(consMg > mgFromAfr + 0.5f){
    float mgCorrection = (consMg - mgFromAfr) / MG_PPM_PER_ML_MG_TANK;
    mgCorrection *= 0.3f;
    suggested_ml_mg += mgCorrection;
  }

  // Enforce non-negative
  suggested_ml_kalk = max(0.0f, suggested_ml_kalk);
  suggested_ml_afr  = max(0.0f, suggested_ml_afr);
  suggested_ml_mg   = max(0.0f, suggested_ml_mg);

  // Smooth changes
  dosing.ml_per_day_kalk = adjustWithLimit(dosing.ml_per_day_kalk, suggested_ml_kalk);
  dosing.ml_per_day_afr  = adjustWithLimit(dosing.ml_per_day_afr,  suggested_ml_afr);
  dosing.ml_per_day_mg   = adjustWithLimit(dosing.ml_per_day_mg,   suggested_ml_mg);

  // Clamp to safety limits
  dosing.ml_per_day_kalk = clampf(dosing.ml_per_day_kalk, 0.0f, MAX_KALK_ML_PER_DAY);
  dosing.ml_per_day_afr  = clampf(dosing.ml_per_day_afr,  0.0f, MAX_AFR_ML_PER_DAY);
  dosing.ml_per_day_mg   = clampf(dosing.ml_per_day_mg,   0.0f, MAX_MG_ML_PER_DAY);

  // Recompute schedule
  updatePumpSchedules();
}


// ===================== PUMP SCHEDULER =====================

unsigned long lastDoseCheckMs = 0;
int dosesGivenTodayKalk = 0;
int dosesGivenTodayAfr  = 0;
int dosesGivenTodayMg   = 0;
unsigned long dayStartMs = 0;

void giveDose(int pin, float seconds){
  if(seconds <= 0) return;
  digitalWrite(pin, HIGH);
  delay((unsigned long)(seconds*1000.0f));
  digitalWrite(pin, LOW);
}

void maybeDosePumps(){
  unsigned long nowMs = millis();

  // Reset daily counters
  if(nowMs - dayStartMs > 86400000UL){
    dayStartMs = nowMs;
    dosesGivenTodayKalk = 0;
    dosesGivenTodayAfr  = 0;
    dosesGivenTodayMg   = 0;
  }

  // Check once per minute
  if(nowMs - lastDoseCheckMs < 60000UL) return;
  lastDoseCheckMs = nowMs;

  if(dosesGivenTodayKalk < DOSES_PER_DAY_KALK){
    giveDose(PIN_PUMP_KALK, SEC_PER_DOSE_KALK);
    dosesGivenTodayKalk++;
  }

  if(dosesGivenTodayAfr < DOSES_PER_DAY_AFR){
    giveDose(PIN_PUMP_AFR, SEC_PER_DOSE_AFR);
    dosesGivenTodayAfr++;
  }

  if(dosesGivenTodayMg < DOSES_PER_DAY_MG){
    giveDose(PIN_PUMP_MG, SEC_PER_DOSE_MG);
    dosesGivenTodayMg++;
  }
}


// ===================== WEB UI (HTML + API) =====================

const char MAIN_PAGE_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8"/>
<title>Reef AI Doser</title>
<style>
body { font-family: Arial, sans-serif; margin:20px;}
.card{border:1px solid #ccc;padding:16px;margin-bottom:20px;border-radius:8px;}
input{width:120px;}
table{border-collapse:collapse;width:100%;}
th,td{border:1px solid #ccc;padding:4px;text-align:right;}
th{background:#eee;}
.tag{display:inline-block;padding:3px 8px;background:#ddd;border-radius:4px;margin-right:5px;}
</style>
<script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
</head>
<body>
<h1>Reef AI Doser</h1>

<div class="card">
<h2>Enter Test (every ~3 days)</h2>
<form method="POST" action="/submit_test">
Calcium (ppm): <input type="number" step="0.1" name="ca" required><br><br>
Alkalinity (dKH): <input type="number" step="0.01" name="alk" required><br><br>
Magnesium (ppm): <input type="number" step="1" name="mg" required><br><br>
pH: <input type="number" step="0.01" name="ph" required><br><br>
<button type="submit">Submit</button>
</form>
</div>

<div class="card">
<h2>Dosing (ml/day)</h2>
<div id="dosing"></div>
</div>

<div class="card">
<h2>History</h2>
<canvas id="paramsChart" height="120"></canvas>
</div>

<script>
async function update(){
  try {
    const res = await fetch('/api/history');
    const data = await res.json();
    const dosingDiv = document.getElementById('dosing');
    dosingDiv.innerHTML = `
      <span class="tag">Kalk: ${data.dosing.kalk.toFixed(1)}</span>
      <span class="tag">AFR: ${data.dosing.afr.toFixed(1)}</span>
      <span class="tag">Mg: ${data.dosing.mg.toFixed(1)}</span>
    `;

    const tests = data.tests || [];
    const labels = [], ca = [], alk = [], mg = [], ph = [];
    if(tests.length > 0){
      const t0 = tests[0].t;
      tests.forEach(tp => {
        labels.push(((tp.t - t0) / 86400).toFixed(1));
        ca.push(tp.ca);
        alk.push(tp.alk);
        mg.push(tp.mg);
        ph.push(tp.ph);
      });
    }

    const ctx = document.getElementById('paramsChart').getContext('2d');
    if(window.c) window.c.destroy();
    window.c = new Chart(ctx,{
      type:'line',
      data:{
        labels,
        datasets:[
          {label:'Ca (ppm)', data:ca, borderWidth:1, yAxisID:'y'},
          {label:'Mg (ppm)', data:mg, borderWidth:1, yAxisID:'y'},
          {label:'Alk (dKH)', data:alk, borderWidth:1, yAxisID:'y1'},
          {label:'pH', data:ph, borderWidth:1, yAxisID:'y1'}
        ]
      },
      options:{
        responsive:true,
        scales:{
          y:{type:'linear',position:'left',title:{display:true,text:'Ca / Mg'}},
          y1:{type:'linear',position:'right',title:{display:true,text:'Alk / pH'}}
        }
      }
    });
  } catch(e){
    console.error(e);
  }
}

update();
setInterval(update, 10000);
</script>
</body>
</html>
)rawliteral";


// ---------- HTTP Handlers ----------

void handleRoot(){
  server.send_P(200, "text/html", MAIN_PAGE_HTML);
}

void handleSubmitTest(){
  if (server.method() != HTTP_POST) {
    server.send(405, "text/plain", "Method Not Allowed");
    return;
  }

  float ca  = server.arg("ca").toFloat();
  float alk = server.arg("alk").toFloat();
  float mg  = server.arg("mg").toFloat();
  float ph  = server.arg("ph").toFloat();

  onNewTestInput(ca, alk, mg, ph);

  server.sendHeader("Location", "/");
  server.send(303);
}

void handleApiHistory(){
  String json = "{";

  // dosing
  json += "\"dosing\":{";
  json += "\"kalk\":" + String(dosing.ml_per_day_kalk, 1) + ",";
  json += "\"afr\":"  + String(dosing.ml_per_day_afr,  1) + ",";
  json += "\"mg\":"   + String(dosing.ml_per_day_mg,   1);
  json += "},";

  // history
  json += "\"tests\":[";
  for(int i = 0; i < historyCount; i++){
    const TestPoint& tp = historyBuf[i];
    if(i > 0) json += ",";
    json += "{";
    json += "\"t\":"   + String(tp.t);
    json += ",\"ca\":" + String(tp.ca, 1);
    json += ",\"alk\":"+ String(tp.alk,2);
    json += ",\"mg\":" + String(tp.mg, 1);
    json += ",\"ph\":" + String(tp.ph, 2);
    json += "}";
  }
  json += "]";

  json += "}";

  server.send(200, "application/json", json);
}


// ===================== SETUP & LOOP =====================

void setup(){
  Serial.begin(115200);
  delay(1000);

  pinMode(PIN_PUMP_KALK, OUTPUT);
  pinMode(PIN_PUMP_AFR,  OUTPUT);
  pinMode(PIN_PUMP_MG,   OUTPUT);
  digitalWrite(PIN_PUMP_KALK, LOW);
  digitalWrite(PIN_PUMP_AFR,  LOW);
  digitalWrite(PIN_PUMP_MG,   LOW);

  dayStartMs = millis();
  updatePumpSchedules();

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.print("AP IP address: ");
  Serial.println(WiFi.softAPIP());

  server.on("/", handleRoot);
  server.on("/submit_test", handleSubmitTest);
  server.on("/api/history", handleApiHistory);
  server.begin();
  Serial.println("HTTP server started");
}

void loop(){
  server.handleClient();
  maybeDosePumps();
}
