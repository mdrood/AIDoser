#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <math.h>
#include <time.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// If MAIN_PAGE_HTML is defined in another file, this keeps it linking cleanly:
// Simple placeholder page – ESP32 is now mainly a backend.
// Your real UI lives on Firebase Hosting.
const char MAIN_PAGE_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
  <head>
    <meta charset="utf-8">
    <title>Reef Doser ESP32</title>
  </head>
  <body>
    <h2>Reef Doser ESP32</h2>
    <p>This ESP32 is online and controlled via Firebase.</p>
  </body>
</html>
)rawliteral";

// ===================== WIFI / NTP SETUP =====================

// CHANGE THESE TO YOUR HOME WIFI
const char* WIFI_SSID     = "roods";
const char* WIFI_PASSWORD = "Frinov25!+!";

WebServer server(80);

// NTP server and timezone (Central Time / Chicago)
const char* NTP_SERVER     = "pool.ntp.org";
const long  GMT_OFFSET_SEC = -6 * 3600;  // UTC-6 standard time
const int   DST_OFFSET_SEC = 3600;       // DST +1h (simple)


// ===================== FIREBASE (for resetAi command only) =====================

const char* FIREBASE_DB_URL = "https://aidoser-default-rtdb.firebaseio.com";
const char* DEVICE_ID       = "reefDoser1";   // 👈 make sure this matches your DB path

WiFiClientSecure secureClient;

// build full Firebase URL from a path (e.g. "/devices/reefDoser1/commands/resetAi")
String firebaseUrl(const String& path) {
  String url = String(FIREBASE_DB_URL);
  if (!url.endsWith("/")) url += "/";
  if (path.startsWith("/")) {
    url += path.substring(1);
  } else {
    url += path;
  }
  if (!url.endsWith(".json")) url += ".json";
  return url;
}


// ===================== TARGETS & TANK INFO =====================

const float TARGET_ALK = 8.5f;      // dKH   eric 8.5
const float TARGET_CA  = 450.0f;    // ppm   450
const float TARGET_MG  = 1440.0f;   // ppm   1400
const float TARGET_PH  = 8.3f;      // pH    8.3

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

// For Firebase-based AI timing (used by resetAIState)
uint64_t lastRemoteTestTimestampMs = 0;


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


// ===================== AI RESET (LOCAL STATE) =====================

void resetAIState() {
  Serial.println("=== AI RESET requested ===");

  // Clear test history in RAM
  historyCount = 0;
  memset(historyBuf, 0, sizeof(historyBuf));

  // Clear last / current tests
  lastTest    = {0, 0, 0, 0, 0};
  currentTest = {0, 0, 0, 0, 0};

  // Reset dosing back to your conservative defaults
  dosing.ml_per_day_kalk = 2000.0f;  // 2L/day
  dosing.ml_per_day_afr  = 20.0f;
  dosing.ml_per_day_mg   = 0.0f;

  // Reset safety / timing
  lastSafetyBackoffTs = nowSeconds();
  lastRemoteTestTimestampMs = 0;

  // Recompute per-dose seconds
  updatePumpSchedules();

  // Optional: notify IFTTT
  sendIFTTT("reef_ai_reset",
            "AI dosing engine reset",
            WiFi.localIP().toString(),
            getLocalTimeString());

  Serial.println("AI state reset complete.");
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


// ===================== FIREBASE: CHECK resetAi COMMAND =====================

// Check /devices/{DEVICE_ID}/commands/resetAi
// If true, reset AI and clear the flag.
// Returns true if a reset was performed.
bool firebaseCheckAndHandleResetAi() {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  HTTPClient https;

  String path = "/devices/" + String(DEVICE_ID) + "/commands/resetAi";
  String url  = firebaseUrl(path);

  Serial.print("Firebase GET resetAi: ");
  Serial.println(url);

  if (!https.begin(secureClient, url)) {
    Serial.println("https.begin() failed (resetAi)");
    return false;
  }

  int httpCode = https.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.print("HTTP GET resetAi error: ");
    Serial.println(httpCode);
    https.end();
    return false;
  }

  String payload = https.getString();
  https.end();

  Serial.print("resetAi payload: ");
  Serial.println(payload);

  // No node yet
  if (payload == "null" || payload.length() == 0) {
    return false;
  }

  // If it's literally true (could be true or "true")
  if (payload.indexOf("true") != -1) {
    // Perform reset
    resetAIState();

    // Clear the flag back to false
    HTTPClient https2;
    String url2 = firebaseUrl(path);

    if (!https2.begin(secureClient, url2)) {
      Serial.println("https.begin() failed (clear resetAi)");
      return true;  // we *did* reset locally, even if DB didn't clear
    }

    https2.addHeader("Content-Type", "application/json");
    int code2 = https2.PUT("false");
    if (code2 != HTTP_CODE_OK && code2 != HTTP_CODE_NO_CONTENT) {
      Serial.print("HTTP PUT clear resetAi error: ");
      Serial.println(code2);
      String resp2 = https2.getString();
      Serial.println(resp2);
    }
    https2.end();

    Serial.println("resetAi flag cleared in Firebase.");
    return true;
  }

  // Anything else (false, etc.)
  return false;
}


// ===================== HTTP HANDLERS =====================

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

  // Allow insecure HTTPS for Firebase
  secureClient.setInsecure();

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

  // Periodically poll Firebase for resetAi command (every ~60s)
  static unsigned long lastResetPollMs = 0;
  unsigned long nowMs = millis();
  if (nowMs - lastResetPollMs >= 60000UL) {
    lastResetPollMs = nowMs;
    firebaseCheckAndHandleResetAi();
  }
}
