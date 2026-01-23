#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <math.h>
#include <time.h>

// ===================== WIFI / NTP SETUP =====================

// CHANGE THESE TO YOUR HOME WIFI
const char* WIFI_SSID     = "roods";
const char* WIFI_PASSWORD = "Frinov25!+!";

WebServer server(80);

// NTP server and timezone (Central Time / Chicago)
const char* NTP_SERVER     = "pool.ntp.org";
const long  GMT_OFFSET_SEC = -6 * 3600;  // UTC-6 standard time
const int   DST_OFFSET_SEC = 3600;       // DST +1h (simple)

// ===================== TARGETS & TANK INFO =====================

const float TARGET_ALK = 8.5f;      // dKH   eric 8.5
const float TARGET_CA  = 450.0f;    // ppm   450
const float TARGET_MG  = 1440.0f;   // ppm   1400
const float TARGET_PH  = 8.3f;     // pH    8.3

// 300 gal × 3.78541 = 1135.6 L
const float TANK_VOLUME_L = 1135.6f;


// ===================== PUMP PINS & FLOW RATES =====================

// Set your actual GPIO pins here:
const int PIN_PUMP_KALK = 25;
const int PIN_PUMP_AFR  = 26;
const int PIN_PUMP_MG   = 27;

// Measure each pump: run for 60 seconds into a cup, measure ml.
float FLOW_KALK_ML_PER_MIN = 675.0f;
float FLOW_AFR_ML_PER_MIN  = 645.0f;
float FLOW_MG_ML_PER_MIN   = 50.0f;


// ===================== CHEMISTRY CONSTANTS =====================

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
  uint32_t t;  // seconds since boot (for graph; not wall time)
  float ca;
  float alk;
  float mg;
  float ph;
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

// SAFETY LIMITS (ml/day caps)
float MAX_KALK_ML_PER_DAY = 2500.0f;  // 2.5L/day max Kalk
float MAX_AFR_ML_PER_DAY  = 200.0f;   // 200 ml/day max AFR
float MAX_MG_ML_PER_DAY   = 40.0f;    // 40 ml/day max Mg

// SAFETY: throttle dosing if no tests for a while
uint32_t lastSafetyBackoffTs = 0;


// ===================== TEST HISTORY FOR GRAPHS =====================

const int MAX_HISTORY = 64;
TestPoint historyBuf[MAX_HISTORY];
int historyCount = 0;

TestPoint lastTest    = {0, 0, 0, 0, 0};
TestPoint currentTest = {0, 0, 0, 0, 0};


// ===================== DOSING SCHEDULE (REAL-TIME, 3 DOSES/DAY) =====================

// 3 doses per day per pump
const int DOSES_PER_DAY_KALK = 3;
const int DOSES_PER_DAY_AFR  = 3;
const int DOSES_PER_DAY_MG   = 3;

// per-dose run time (seconds)
float SEC_PER_DOSE_KALK = 0.0f;
float SEC_PER_DOSE_AFR  = 0.0f;
float SEC_PER_DOSE_MG   = 0.0f;

// Real-time schedule: 3 time slots per day (HH:MM)
// 9:30 AM, 12:30 PM, 3:30 PM (Central Time)
const int DOSE_SLOTS_PER_DAY = 3;
int DOSE_HOURS[DOSE_SLOTS_PER_DAY]   = {9, 12, 15};
int DOSE_MINUTES[DOSE_SLOTS_PER_DAY] = {30, 30, 30};

// Track whether we've dosed each slot for the current day
int  lastDoseDay = -1;                        // tm_yday of last day we reset
bool slotDone[DOSE_SLOTS_PER_DAY] = {false, false, false};


// ===================== IFTTT WEBHOOK SETUP =====================
// Get your key from: https://ifttt.com/maker_webhooks
// It will look like: https://maker.ifttt.com/use/<YOUR_KEY_HERE>

const char* IFTTT_HOST = "maker.ifttt.com";
const int   IFTTT_PORT = 80;

// 🔴 REPLACE THIS with your actual Maker key
const char* IFTTT_KEY = "fBplW8jJqqotTqTxck4oTdK_oHTJKAawKfja-WlcgW-";

// Forward declarations
int sendIFTTT(String eventName,
              String value1 = "",
              String value2 = "",
              String value3 = "");
String getLocalTimeString();

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

void pushHistory(const TestPoint& tp){
  if(historyCount < MAX_HISTORY){
    historyBuf[historyCount++] = tp;
  } else {
    for(int i=1;i<MAX_HISTORY;i++) historyBuf[i-1] = historyBuf[i];
    historyBuf[MAX_HISTORY-1] = tp;
  }
}


// ===================== IFTTT HELPERS =====================

int sendIFTTT(String eventName,
              String value1,
              String value2,
              String value3) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("IFTTT: WiFi not connected");
    return -1;
  }

  WiFiClient client;
  if (!client.connect(IFTTT_HOST, IFTTT_PORT)) {
    Serial.println("IFTTT: connection failed");
    return -2;
  }

  // URL: /trigger/{event}/with/key/{KEY}
  String url = "/trigger/" + eventName + "/with/key/" + String(IFTTT_KEY);

  // Build JSON body
  String json = "{";
  bool first = true;
  if (value1.length()) { json += "\"value1\":\"" + value1 + "\""; first = false; }
  if (value2.length()) { if (!first) json += ","; json += "\"value2\":\"" + value2 + "\""; first = false; }
  if (value3.length()) { if (!first) json += ","; json += "\"value3\":\"" + value3 + "\""; }
  json += "}";

  String request;
  request  = "POST " + url + " HTTP/1.1\r\n";
  request += "Host: " + String(IFTTT_HOST) + "\r\n";
  request += "Content-Type: application/json\r\n";
  request += "Content-Length: " + String(json.length()) + "\r\n";
  request += "Connection: close\r\n\r\n";
  request += json;

  client.print(request);

  unsigned long start = millis();
  while (client.connected() && millis() - start < 3000) {
    while (client.available()) {
      char c = client.read();
      // Serial.write(c); // Uncomment if you want to see HTTP response
    }
  }
  client.stop();

  Serial.print("IFTTT: event ");
  Serial.print(eventName);
  Serial.println(" sent");

  return 0;
}

String getLocalTimeString() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "time unknown";
  }
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &timeinfo);
  return String(buf);
}


// ===================== SAFETY: CHEMISTRY-BASED CAPS =====================

void enforceChemSafetyCaps() {
  float alkRise =
      dosing.ml_per_day_kalk * DKH_PER_ML_KALK_TANK +
      dosing.ml_per_day_afr  * DKH_PER_ML_AFR_TANK;

  float caRise =
      dosing.ml_per_day_kalk * CA_PPM_PER_ML_KALK_TANK +
      dosing.ml_per_day_afr  * CA_PPM_PER_ML_AFR_TANK;

  float mgRise =
      dosing.ml_per_day_afr  * MG_PPM_PER_ML_AFR_TANK +
      dosing.ml_per_day_mg   * MG_PPM_PER_ML_MG_TANK;

  const float MAX_ALK_RISE_DKH_PER_DAY = 0.8f;
  const float MAX_CA_RISE_PPM_PER_DAY  = 20.0f;
  const float MAX_MG_RISE_PPM_PER_DAY  = 30.0f;

  float scale = 1.0f;

  if (alkRise > MAX_ALK_RISE_DKH_PER_DAY && alkRise > 0.0f) {
    float s = MAX_ALK_RISE_DKH_PER_DAY / alkRise;
    if (s < scale) scale = s;
  }

  if (caRise > MAX_CA_RISE_PPM_PER_DAY && caRise > 0.0f) {
    float s = MAX_CA_RISE_PPM_PER_DAY / caRise;
    if (s < scale) scale = s;
  }

  if (mgRise > MAX_MG_RISE_PPM_PER_DAY && mgRise > 0.0f) {
    float s = MAX_MG_RISE_PPM_PER_DAY / mgRise;
    if (s < scale) scale = s;
  }

  if (scale < 1.0f) {
    dosing.ml_per_day_kalk *= scale;
    dosing.ml_per_day_afr  *= scale;
    dosing.ml_per_day_mg   *= scale;
    Serial.print("SAFETY: Scaling dosing by ");
    Serial.println(scale, 3);

    // IFTTT alert: dosing scaled by safety
    sendIFTTT("reef_dose_scaled",
              "Dosing scaled by safety",
              "scale=" + String(scale, 3),
              getLocalTimeString());
  }
}


// ===================== “AI” CONTROL (with pH bias & safety) =====================

void onNewTestInput(float ca, float alk, float mg, float ph){
  // Update history for graph
  lastTest = currentTest;
  currentTest.t   = nowSeconds();
  currentTest.ca  = ca;
  currentTest.alk = alk;
  currentTest.mg  = mg;
  currentTest.ph  = ph;
  pushHistory(currentTest);

  // Sanity check ranges (ignore for dosing if crazy)
  if (ca   < 300.0f || ca   > 550.0f ||
      alk  <   5.0f || alk  > 14.0f  ||
      mg   <1100.0f || mg   >1600.0f ||
      ph   <   7.0f || ph   >  9.0f) {
    Serial.println("SAFETY: IGNORING TEST for dosing (out-of-range). Graph updated only.");
    return;
  }

  // First valid test: just set schedules from starting dosing
  if(lastTest.t == 0){
    updatePumpSchedules();
    return;
  }

  float days = float(currentTest.t - lastTest.t) / 86400.0f;
  if(days <= 0.25f) {
    Serial.println("SAFETY: Tests too close together, ignoring for dosing updates.");
    return;
  }

  // Consumption per day
  float consAlk = (lastTest.alk - currentTest.alk) / days;
  float consCa  = (lastTest.ca  - currentTest.ca ) / days;
  float consMg  = (lastTest.mg  - currentTest.mg ) / days;

  float alkNeeded = consAlk;
  if(alkNeeded < 0.0f) alkNeeded = 0.0f;

  // pH bias
  float kalkFrac = 0.8f;  // default: 80% alk from kalk

  if (!isnan(currentTest.ph)) {
    float phError = currentTest.ph - TARGET_PH;

    if (phError < -0.05f) {
      // pH low -> more kalk
      kalkFrac = 0.90f;
    } else if (phError > 0.05f) {
      // pH high -> more AFR, less kalk
      kalkFrac = 0.70f;
    }
  }

  kalkFrac = clampf(kalkFrac, 0.6f, 0.95f);

  float targetAlkFromKalk = kalkFrac        * alkNeeded;
  float targetAlkFromAfr  = (1.0f-kalkFrac) * alkNeeded;

  float suggested_ml_kalk = (DKH_PER_ML_KALK_TANK > 0.0f) ? (targetAlkFromKalk / DKH_PER_ML_KALK_TANK) : 0.0f;
  float suggested_ml_afr  = (DKH_PER_ML_AFR_TANK  > 0.0f) ? (targetAlkFromAfr  / DKH_PER_ML_AFR_TANK ) : 0.0f;

  float caFromKalk = suggested_ml_kalk * CA_PPM_PER_ML_KALK_TANK;
  float caFromAfr  = suggested_ml_afr  * CA_PPM_PER_ML_AFR_TANK;
  float mgFromAfr  = suggested_ml_afr  * MG_PPM_PER_ML_AFR_TANK;

  float caError = consCa - (caFromKalk + caFromAfr);
  float mgError = consMg - (mgFromAfr);

  if(fabsf(caError) > 5.0f){
    float afrCorrection = (caError / CA_PPM_PER_ML_AFR_TANK) * 0.3f;
    suggested_ml_afr += afrCorrection;
  }

  float suggested_ml_mg = dosing.ml_per_day_mg;
  if(consMg > mgFromAfr + 0.5f){
    float mgCorrection = (consMg - mgFromAfr) / MG_PPM_PER_ML_MG_TANK;
    mgCorrection *= 0.3f;
    suggested_ml_mg += mgCorrection;
  }

  suggested_ml_kalk = max(0.0f, suggested_ml_kalk);
  suggested_ml_afr  = max(0.0f, suggested_ml_afr);
  suggested_ml_mg   = max(0.0f, suggested_ml_mg);

  dosing.ml_per_day_kalk = adjustWithLimit(dosing.ml_per_day_kalk, suggested_ml_kalk);
  dosing.ml_per_day_afr  = adjustWithLimit(dosing.ml_per_day_afr,  suggested_ml_afr);
  dosing.ml_per_day_mg   = adjustWithLimit(dosing.ml_per_day_mg,   suggested_ml_mg);

  dosing.ml_per_day_kalk = clampf(dosing.ml_per_day_kalk, 0.0f, MAX_KALK_ML_PER_DAY);
  dosing.ml_per_day_afr  = clampf(dosing.ml_per_day_afr,  0.0f, MAX_AFR_ML_PER_DAY);
  dosing.ml_per_day_mg   = clampf(dosing.ml_per_day_mg,   0.0f, MAX_MG_ML_PER_DAY);

  enforceChemSafetyCaps();
  updatePumpSchedules();

  lastSafetyBackoffTs = nowSeconds();
}


// ===================== SAFETY: BACKOFF IF NO TESTS =====================

void safetyBackoffIfNoTests() {
  if (currentTest.t == 0) return;

  uint32_t now = nowSeconds();
  float daysSinceLastTest = float(now - currentTest.t) / 86400.0f;

  if (daysSinceLastTest <= 5.0f) return;
  if (now - lastSafetyBackoffTs < 86400UL) return;

  Serial.println("SAFETY: No tests >5 days. Backing off dosing to 70%.");
  dosing.ml_per_day_kalk *= 0.7f;
  dosing.ml_per_day_afr  *= 0.7f;
  dosing.ml_per_day_mg   *= 0.7f;

  dosing.ml_per_day_kalk = clampf(dosing.ml_per_day_kalk, 0.0f, MAX_KALK_ML_PER_DAY);
  dosing.ml_per_day_afr  = clampf(dosing.ml_per_day_afr,  0.0f, MAX_AFR_ML_PER_DAY);
  dosing.ml_per_day_mg   = clampf(dosing.ml_per_day_mg,   0.0f, MAX_MG_ML_PER_DAY);

  enforceChemSafetyCaps();
  updatePumpSchedules();

  lastSafetyBackoffTs = now;

  // IFTTT alert: no tests + backoff
  sendIFTTT("reef_no_tests_backoff",
            "No tests >5 days",
            "Dosing backed off to 70%",
            getLocalTimeString());
}


// ===================== PUMP SCHEDULER (REAL-TIME SLOTS) =====================

void giveDose(int pin, float seconds){
  if(seconds <= 0) return;
  digitalWrite(pin, HIGH);
  delay((unsigned long)(seconds*1000.0f));
  digitalWrite(pin, LOW);
}

void maybeDosePumpsRealTime(){
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    // If we can't get time, don't dose
    Serial.println("WARN: getLocalTime failed, skipping dosing.");
    return;
  }

  int dayOfYear = timeinfo.tm_yday;   // 0..365
  int hour      = timeinfo.tm_hour;   // 0..23
  int minute    = timeinfo.tm_min;    // 0..59

  // New day? Reset slot flags
  if (dayOfYear != lastDoseDay) {
    lastDoseDay = dayOfYear;
    for (int i = 0; i < DOSE_SLOTS_PER_DAY; i++) {
      slotDone[i] = false;
    }
  }

  // Precompute ml per dose for each pump
  float mlPerDoseKalk = (DOSES_PER_DAY_KALK > 0) ? (dosing.ml_per_day_kalk / DOSES_PER_DAY_KALK) : 0.0f;
  float mlPerDoseAfr  = (DOSES_PER_DAY_AFR  > 0) ? (dosing.ml_per_day_afr  / DOSES_PER_DAY_AFR ) : 0.0f;
  float mlPerDoseMg   = (DOSES_PER_DAY_MG   > 0) ? (dosing.ml_per_day_mg   / DOSES_PER_DAY_MG  ) : 0.0f;

  String timeStr = getLocalTimeString();

  // For each slot, if we are past the time and haven't dosed it yet, dose once.
  for(int i = 0; i < DOSE_SLOTS_PER_DAY; i++){
    if (slotDone[i]) continue;

    int sh = DOSE_HOURS[i];
    int sm = DOSE_MINUTES[i];

    bool timeReached =
      (hour > sh) || (hour == sh && minute >= sm);

    if (timeReached) {
      Serial.print("Dosing slot ");
      Serial.println(i);

      // --- Kalk dose & IFTTT ---
      if (SEC_PER_DOSE_KALK > 0.0f && mlPerDoseKalk > 0.0f) {
        giveDose(PIN_PUMP_KALK, SEC_PER_DOSE_KALK);
        sendIFTTT("reef_dose_kalk",
                  "Kalk",
                  String(mlPerDoseKalk, 1) + " ml",
                  timeStr);
      }

      // --- AFR dose & IFTTT ---
      if (SEC_PER_DOSE_AFR > 0.0f && mlPerDoseAfr > 0.0f) {
        giveDose(PIN_PUMP_AFR, SEC_PER_DOSE_AFR);
        sendIFTTT("reef_dose_afr",
                  "AllForReef",
                  String(mlPerDoseAfr, 1) + " ml",
                  timeStr);
      }

      // --- Mg dose & IFTTT ---
      if (SEC_PER_DOSE_MG > 0.0f && mlPerDoseMg > 0.0f) {
        giveDose(PIN_PUMP_MG, SEC_PER_DOSE_MG);
        sendIFTTT("reef_dose_mg",
                  "Magnesium",
                  String(mlPerDoseMg, 1) + " ml",
                  timeStr);
      }

      slotDone[i] = true;
    }
  }
}


// ===================== WEB UI (HTML + API) =====================

const char MAIN_PAGE_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1.0"/>
<title>Reef AI Doser</title>
<style>
  :root {
    --bg: #020817;
    --bg-card: rgba(15, 23, 42, 0.96);
    --accent: #22d3ee;
    --accent-soft: rgba(56, 189, 248, 0.18);
    --accent-strong: #38bdf8;
    --text: #e2e8f0;
    --text-soft: #94a3b8;
    --border: rgba(148, 163, 184, 0.35);
    --danger: #f97373;
    --success: #4ade80;
  }

  * {
    box-sizing: border-box;
  }

  body {
    margin: 0;
    min-height: 100vh;
    font-family: system-ui, -apple-system, BlinkMacSystemFont, "SF Pro Text", Arial, sans-serif;
    background: radial-gradient(circle at top, #0ea5e9 0, #020617 40%, #020617 100%);
    color: var(--text);
    display: flex;
    justify-content: center;
    padding: 16px;
  }

  .shell {
    width: 100%;
    max-width: 900px;
  }

  .glass-panel {
    background: linear-gradient(145deg, rgba(15, 23, 42, 0.96), rgba(15, 23, 42, 0.96));
    border-radius: 18px;
    border: 1px solid rgba(148, 163, 184, 0.3);
    box-shadow:
      0 24px 60px rgba(15, 23, 42, 0.9),
      0 0 0 1px rgba(15, 23, 42, 0.7);
    padding: 18px 16px 22px;
    backdrop-filter: blur(18px);
  }

  @media (min-width: 640px) {
    .glass-panel {
      padding: 22px 24px 26px;
    }
  }

  header {
    display: flex;
    align-items: center;
    gap: 14px;
    margin-bottom: 18px;
    flex-wrap: wrap;
  }

  .reef-icon {
    width: 42px;
    height: 42px;
    border-radius: 999px;
    background: radial-gradient(circle at 30% 20%, #e0f2fe, #0ea5e9 40%, #0369a1 70%, #020617 100%);
    box-shadow:
      0 0 0 2px rgba(8, 47, 73, 0.8),
      0 12px 26px rgba(12, 74, 110, 0.7);
    display: flex;
    align-items: center;
    justify-content: center;
  }

  .reef-icon-wave {
    width: 70%;
    height: 70%;
    border-radius: 50%;
    border: 2px solid rgba(226, 232, 240, 0.9);
    border-top-color: transparent;
    border-left-color: transparent;
    transform: rotate(25deg);
  }

  header h1 {
    font-size: 1.4rem;
    letter-spacing: 0.03em;
    margin: 0;
    display: flex;
    align-items: center;
    gap: 6px;
  }

  header h1 span.highlight {
    color: var(--accent);
  }

  header p {
    margin: 0;
    font-size: 0.8rem;
    color: var(--text-soft);
  }

  .pill-row {
    display: flex;
    flex-wrap: wrap;
    gap: 8px;
    margin-top: 6px;
  }

  .pill {
    font-size: 0.7rem;
    padding: 4px 8px;
    border-radius: 999px;
    border: 1px solid rgba(148, 163, 184, 0.35);
    color: var(--text-soft);
    background: rgba(15, 23, 42, 0.9);
  }

  .pill.accent {
    border-color: rgba(34, 211, 238, 0.8);
    background: radial-gradient(circle at 20% 0%, rgba(56, 189, 248, 0.25), rgba(15, 23, 42, 0.85));
    color: var(--accent);
  }

  .grid {
    display: grid;
    grid-template-columns: 1fr;
    gap: 14px;
  }

  @media (min-width: 768px) {
    .grid {
      grid-template-columns: minmax(0,1.1fr) minmax(0,1fr);
    }
  }

  .card {
    border-radius: 14px;
    border: 1px solid var(--border);
    background: linear-gradient(145deg, rgba(15, 23, 42, 0.96), rgba(15, 23, 42, 0.98));
    padding: 14px 14px 16px;
    position: relative;
    overflow: hidden;
  }

  .card::before {
    content: "";
    position: absolute;
    inset: -40%;
    background:
      radial-gradient(circle at 0% 0%, var(--accent-soft), transparent 55%),
      radial-gradient(circle at 100% 100%, rgba(59,130,246,0.12), transparent 55%);
    opacity: 0.55;
    pointer-events: none;
  }

  .card-inner {
    position: relative;
    z-index: 1;
  }

  .card h2 {
    font-size: 0.95rem;
    margin: 0 0 8px;
    letter-spacing: 0.06em;
    text-transform: uppercase;
    color: var(--text-soft);
  }

  .card h3 {
    font-size: 0.9rem;
    margin: 0 0 10px;
    color: var(--accent-strong);
  }

  .field {
    margin-bottom: 8px;
  }

  .field label {
    display: block;
    font-size: 0.75rem;
    color: var(--text-soft);
    margin-bottom: 3px;
  }

  .field input {
    width: 100%;
    border-radius: 8px;
    border: 1px solid rgba(148, 163, 184, 0.55);
    background: rgba(15, 23, 42, 0.9);
    color: var(--text);
    font-size: 0.85rem;
    padding: 7px 9px;
    outline: none;
  }

  .field input:focus {
    border-color: var(--accent);
    box-shadow: 0 0 0 1px rgba(34, 211, 238, 0.4);
  }

  button[type="submit"] {
    margin-top: 6px;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    padding: 8px 14px;
    font-size: 0.85rem;
    border-radius: 999px;
    border: none;
    background: radial-gradient(circle at 10% 0%, #22d3ee, #1d4ed8);
    color: #eff6ff;
    cursor: pointer;
    box-shadow:
      0 10px 22px rgba(37, 99, 235, 0.6),
      0 0 0 1px rgba(30, 64, 175, 0.5);
  }

  button[type="submit"]:active {
    transform: translateY(1px);
    box-shadow:
      0 6px 18px rgba(37, 99, 235, 0.6),
      0 0 0 1px rgba(30, 64, 175, 0.6);
  }

  .tags-row {
    display: flex;
    flex-wrap: wrap;
    gap: 6px;
    margin-top: 6px;
  }

  .tag {
    font-size: 0.75rem;
    padding: 4px 10px;
    border-radius: 999px;
    background: rgba(15, 23, 42, 0.95);
    border: 1px solid rgba(148, 163, 184, 0.5);
    color: var(--text-soft);
    display: inline-flex;
    align-items: center;
    gap: 4px;
  }

  .tag-dot {
    width: 7px;
    height: 7px;
    border-radius: 999px;
    background: var(--accent);
  }

  .tag.kalk .tag-dot {
    background: #f97316;
  }

  .tag.afr .tag-dot {
    background: #a855f7;
  }

  .tag.mg .tag-dot {
    background: #22c55e;
  }

  .tag small {
    font-size: 0.7rem;
    color: var(--text-soft);
  }

  .hint {
    font-size: 0.7rem;
    color: var(--text-soft);
    margin-top: 6px;
  }

  .hint span {
    color: var(--accent);
  }

  .chart-card {
    margin-top: 14px;
  }

  .chart-container {
    width: 100%;
    max-height: 260px;
  }

  footer {
    margin-top: 10px;
    font-size: 0.7rem;
    color: var(--text-soft);
    text-align: right;
  }

  footer span {
    color: var(--accent-soft);
  }
</style>
<script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
</head>
<body>
  <div class="shell">
    <div class="glass-panel">
      <header>
        <div class="reef-icon">
          <div class="reef-icon-wave"></div>
        </div>
        <div>
          <h1>
            Reef<span class="highlight">AI</span> Doser
          </h1>
          <p>Adaptive dosing for your reef — tuned by your test results.</p>
          <div class="pill-row">
            <div class="pill accent">300 gal system</div>
            <div class="pill">Targets: Alk 9 · Ca 420 · Mg 1440 · pH 8.2</div>
          </div>
        </div>
      </header>

      <div class="grid">
        <!-- Left side: test form -->
        <div class="card">
          <div class="card-inner">
            <h2>New Test</h2>
            <h3>Update tank parameters</h3>
            <form method="POST" action="/submit_test">
              <div class="field">
                <label for="ca">Calcium (ppm)</label>
                <input id="ca" type="number" step="0.1" name="ca" required placeholder="e.g. 420">
              </div>
              <div class="field">
                <label for="alk">Alkalinity (dKH)</label>
                <input id="alk" type="number" step="0.01" name="alk" required placeholder="e.g. 9.0">
              </div>
              <div class="field">
                <label for="mg">Magnesium (ppm)</label>
                <input id="mg" type="number" step="1" name="mg" required placeholder="e.g. 1440">
              </div>
              <div class="field">
                <label for="ph">pH</label>
                <input id="ph" type="number" step="0.01" name="ph" required placeholder="e.g. 8.20">
              </div>
              <button type="submit">Save Test &amp; Recalculate</button>
              <p class="hint">
                Tip: Test every <span>~3 days</span> for best tuning. Extreme values are ignored for safety.
              </p>
            </form>
          </div>
        </div>

        <!-- Right side: dosing summary -->
        <div class="card">
          <div class="card-inner">
            <h2>Current Dosing Plan</h2>
            <h3>Auto-adjusted ml / day</h3>
            <div id="dosing" class="tags-row">
              <!-- Filled by JS -->
            </div>
            <p class="hint">
              Doses are spread across three daytime windows (9:30 · 12:30 · 15:30) with safety caps and
              gradual adjustments to avoid shocking the reef.
            </p>
          </div>
        </div>
      </div>

      <!-- Chart -->
      <div class="card chart-card">
        <div class="card-inner">
          <h2>Trend History</h2>
          <h3>How your reef has been trending</h3>
          <div class="chart-container">
            <canvas id="paramsChart"></canvas>
          </div>
          <p class="hint">
            Use this to spot trends in Alk, Ca, Mg, and pH over time. Each point is one manual test.
          </p>
        </div>
      </div>

      <footer>
        <span>ESP32 · Reef AI Doser</span>
      </footer>
    </div>
  </div>

<script>
async function update(){
  try {
    const res = await fetch('/api/history');
    const data = await res.json();

    // ----- Dosing tags -----
    const dosingDiv = document.getElementById('dosing');
    const k = data.dosing.kalk;
    const a = data.dosing.afr;
    const m = data.dosing.mg;

    dosingDiv.innerHTML = `
      <div class="tag kalk">
        <div class="tag-dot"></div>
        <span>Kalkwasser</span>
        <small>${k.toFixed(1)} ml / day</small>
      </div>
      <div class="tag afr">
        <div class="tag-dot"></div>
        <span>All-For-Reef</span>
        <small>${a.toFixed(1)} ml / day</small>
      </div>
      <div class="tag mg">
        <div class="tag-dot"></div>
        <span>Magnesium</span>
        <small>${m.toFixed(1)} ml / day</small>
      </div>
    `;

    // ----- History chart -----
    const tests = data.tests || [];
    const labels = [], ca = [], alk = [], mg = [], ph = [];

    if(tests.length > 0){
      const t0 = tests[0].t;
      tests.forEach(tp => {
        labels.push(((tp.t - t0) / 86400).toFixed(1)); // days since first test
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
          {
            label:'Ca (ppm)',
            data:ca,
            borderWidth:1,
            tension:0.25,
            yAxisID:'y'
          },
          {
            label:'Mg (ppm)',
            data:mg,
            borderWidth:1,
            tension:0.25,
            yAxisID:'y'
          },
          {
            label:'Alk (dKH)',
            data:alk,
            borderWidth:1,
            tension:0.25,
            yAxisID:'y1'
          },
          {
            label:'pH',
            data:ph,
            borderWidth:1,
            tension:0.25,
            yAxisID:'y1'
          }
        ]
      },
      options:{
        responsive:true,
        maintainAspectRatio:false,
        interaction: {
          mode: 'index',
          intersect: false
        },
        scales:{
          y:{
            type:'linear',
            position:'left',
            title:{display:true,text:'Ca / Mg'},
            grid: { color:'rgba(148,163,184,0.25)' },
            ticks: { color:'#cbd5f5', font:{size:10} }
          },
          y1:{
            type:'linear',
            position:'right',
            title:{display:true,text:'Alk / pH'},
            grid: { drawOnChartArea:false },
            ticks: { color:'#cbd5f5', font:{size:10} }
          },
          x:{
            title:{display:true,text:'Days since first test'},
            grid: { color:'rgba(148,163,184,0.22)' },
            ticks: { color:'#cbd5f5', font:{size:9} }
          }
        },
        plugins:{
          legend:{
            labels:{color:'#e2e8f0', font:{size:10}}
          }
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

  json += "\"dosing\":{";
  json += "\"kalk\":" + String(dosing.ml_per_day_kalk, 1) + ",";
  json += "\"afr\":"  + String(dosing.ml_per_day_afr,  1) + ",";
  json += "\"mg\":"   + String(dosing.ml_per_day_mg,   1);
  json += "},";

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

  updatePumpSchedules();
  lastSafetyBackoffTs = nowSeconds();

  // Connect to home Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected, IP address: ");
  Serial.println(WiFi.localIP());

  // NTP time sync
  configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER);
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to obtain time from NTP");
  } else {
    Serial.println("Time synchronized from NTP");
  }

  // Web server routes
  server.on("/", handleRoot);
  server.on("/submit_test", handleSubmitTest);
  server.on("/api/history", handleApiHistory);
  server.begin();
  Serial.println("HTTP server started");

  // IFTTT: report we are online
  sendIFTTT("reef_doser_online",
            "Reef doser booted",
            WiFi.localIP().toString(),
            getLocalTimeString());
}

void loop(){
  server.handleClient();

  safetyBackoffIfNoTests();

  // Dose at 3 scheduled time slots per day
  maybeDosePumpsRealTime();
}
