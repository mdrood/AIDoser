#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <math.h>
#include <time.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <stdint.h>
#include <Preferences.h>

// Forward declarations used by helpers
uint64_t getEpochMillis();


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
//const char* WIFI_SSID     = "roods";
//const char* WIFI_PASSWORD = "Frinov25!+!";
WiFiManager wm;

WebServer server(80);

// NTP server and timezone (Central Time / Chicago)
const char* NTP_SERVER     = "pool.ntp.org";
const long  GMT_OFFSET_SEC = -6 * 3600;  // UTC-6 standard time
const int   DST_OFFSET_SEC = 3600;       // DST +1h (simple)
bool globalEmergencyStop = false; // If true, no pumps can move.

// ===================== FIREBASE (REST API) =====================

const char* FIREBASE_DB_URL = "https://aidoser-default-rtdb.firebaseio.com";
const char* DEVICE_ID       = "reefDoser6";   // 👈 must match your DB path
const char* FW_VERSION      = "1.0.4";  // 👈 bump when you flash new firmware
 

// Add these to your global variables at the top
float pendingKalkMl = 0.0f;
float pendingAfrMl  = 0.0f;
float pendingMgMl   = 0.0f;
float pendingTbdMl   = 0.0f;

const float MIN_DOSE_SEC = 1.0f; // Minimum time we allow a pump to run

WiFiClientSecure secureClient;

Preferences dosingPrefs;

void firebaseSetCalibrationStatus();
void updateChemistryConstants();
void syncTimeFromFirebaseHeader();


// Build full Firebase URL from a path (e.g. "/devices/reefDoser1/commands/resetAi")
String firebaseUrl(const String& path) {
  String url = String(FIREBASE_DB_URL);
  if (!url.endsWith("/")) url += "/";

  if (path.startsWith("/")) {
    url += path.substring(1);
  } else {
    url += path;
  }

  if (!url.endsWith(".json")) {
    url += ".json";
  }
  return url;
}

// Simple PUT JSON helper
bool firebasePutJson(const String& path, const String& jsonBody) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Firebase PUT: WiFi not connected");
    return false;
  }

  HTTPClient https;
  String url = firebaseUrl(path);

  Serial.print("Firebase PUT: ");
  Serial.println(url);
  Serial.print("Body: ");
  Serial.println(jsonBody);

  if (!https.begin(secureClient, url)) {
    Serial.println("Firebase PUT begin() failed");
    return false;
  }

  https.addHeader("Content-Type", "application/json");
  int code = https.PUT(jsonBody);
  if (code != HTTP_CODE_OK && code != HTTP_CODE_NO_CONTENT) {
    Serial.print("Firebase PUT error code: ");
    Serial.println(code);
    String resp = https.getString();
    Serial.println(resp);
    https.end();
    return false;
  }

  https.end();
  return true;
}

// Simple POST JSON helper (for alerts, pushes, etc.)
bool firebasePostJson(const String& path, const String& jsonBody) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Firebase POST: WiFi not connected");
    return false;
  }

  HTTPClient https;
  String url = firebaseUrl(path);

  Serial.print("Firebase POST: ");
  Serial.println(url);
  Serial.print("Body: ");
  Serial.println(jsonBody);

  if (!https.begin(secureClient, url)) {
    Serial.println("Firebase POST begin() failed");
    return false;
  }

  https.addHeader("Content-Type", "application/json");
  int code = https.POST(jsonBody);
  if (code != HTTP_CODE_OK && code != HTTP_CODE_NO_CONTENT) {
    Serial.print("Firebase POST error code: ");
    Serial.println(code);
    String resp = https.getString();
    Serial.println(resp);
    https.end();
    return false;
  }

  https.end();
  return true;
}


// Log a completed dose run to RTDB so the web UI can build the dosing history graph.
// Writes to: /devices/<DEVICE_ID>/doseRuns (auto-push key).
bool firebaseLogDoseRun(int pumpIndex,
                        const String& pumpName,
                        float ml,
                        float durationSec,
                        float flowMlPerMin,
                        const String& source) {
  StaticJsonDocument<256> doc;
  doc["ts"] = (unsigned long long)getEpochMillis();
  doc["source"] = source;
  doc["pumpIndex"] = pumpIndex;
  doc["pump"] = pumpName;
  doc["ml"] = ml;
  doc["durationSec"] = durationSec;
  doc["flowMlPerMin"] = flowMlPerMin;

  String body;
  serializeJson(doc, body);

  const String path = "/devices/" + String(DEVICE_ID) + "/doseRuns.json";
  return firebasePostJson(path, body);
}



// Simple GET helper (returns body or empty string)
String firebaseGetJson(const String& path) {
  String result;
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Firebase GET: WiFi not connected");
    return result;
  }

  HTTPClient https;
  String url = firebaseUrl(path);

  Serial.print("Firebase GET: ");
  Serial.println(url);

  if (!https.begin(secureClient, url)) {
    Serial.println("Firebase GET begin() failed");
    return result;
  }

  int code = https.GET();
  if (code != HTTP_CODE_OK) {
    Serial.print("Firebase GET error code: ");
    Serial.println(code);
    https.end();
    return result;
  }

  result = https.getString();
  https.end();
  return result;
}


// ===================== TARGETS & TANK INFO =====================

const float TARGET_ALK = 8.5f;      // dKH   eric 8.5
const float TARGET_CA  = 450.0f;    // ppm   450
const float TARGET_MG  = 1440.0f;   // ppm   1400
const float TARGET_PH  = 8.3f;      // pH    8.3

// 300 gal × 3.78541 = 1135.6 L
float TANK_VOLUME_L = 1135.6f; // Default for 300 gallons


// ===================== PUMP PINS & FLOW RATES =====================

// Set your actual GPIO pins here:
const int PIN_PUMP_KALK = 25;
const int PIN_PUMP_AFR  = 26;
const int PIN_PUMP_MG   = 27;
const int PIN_PUMP_TBD = 22; // Pump 4 (aux)


// Measure each pump: run for 60 seconds into a cup, measure ml.
float FLOW_KALK_ML_PER_MIN = 675.0f;
float FLOW_AFR_ML_PER_MIN  = 645.0f;
float FLOW_MG_ML_PER_MIN   = 50.0f;
float FLOW_TBD_ML_PER_MIN   = 50.0f;



float FLOW_AUX_ML_PER_MIN = 0.0f;   // Pump 4 (optional)
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

// ---------- PUMP 4 (TBD / AUX) ----------
float TBD_PPM_PER_ML_TANK     = 0.00f;

// ===================== DOSING CONFIG & TEST DATA =====================

struct TestPoint {
  uint32_t t;  // seconds since boot (for graph; not wall time)
  float ca;
  float alk;
  float mg;
  float ph;
  float tbd;
};

struct DosingConfig {
  float ml_per_day_kalk;
  float ml_per_day_afr;
  float ml_per_day_mg;
  float ml_per_day_tbd;
};

// Initial starting doses (safe, conservative)
DosingConfig dosing = {
  2000.0f, // ml/day kalk (2L/day)
  20.0f,   // ml/day AFR
  0.0f ,    // ml/day Mg
  0.0f     //ml of tbd
};

// SAFETY LIMITS (ml/day caps)
float MAX_KALK_ML_PER_DAY = 2500.0f;  // 2.5L/day max Kalk
float MAX_AFR_ML_PER_DAY  = 200.0f;   // 200 ml/day max AFR
float MAX_MG_ML_PER_DAY   = 40.0f;    // 40 ml/day max Mg
float MAX_TBD_ML_PER_DAY   = 40.0f;    // 40 ml/day max Mg

// SAFETY: throttle dosing if no tests for a while
uint32_t lastSafetyBackoffTs = 0;

// Persist AI dosing plan (ml/day) across reboots/OTA
void loadDosingFromPrefs() {
  if (!dosingPrefs.begin("dosing", true)) {
    Serial.println("Prefs: failed to open dosing (read)");
    return;
  }

  dosing.ml_per_day_kalk = dosingPrefs.getFloat("kalk", dosing.ml_per_day_kalk);
  dosing.ml_per_day_afr  = dosingPrefs.getFloat("afr",  dosing.ml_per_day_afr);
  dosing.ml_per_day_mg   = dosingPrefs.getFloat("mg",   dosing.ml_per_day_mg);
  dosing.ml_per_day_tbd   = dosingPrefs.getFloat("tbd",   dosing.ml_per_day_tbd);

  dosingPrefs.end();

  Serial.print("Prefs: loaded dosing KALK=");
  Serial.print(dosing.ml_per_day_kalk);
  Serial.print(" AFR=");
  Serial.print(dosing.ml_per_day_afr);
  Serial.print(" MG=");
  Serial.println(dosing.ml_per_day_mg);
}


// ===================== FLOW CALIBRATION (NVS PREFS) =====================
// Persist calibrated pump flow rates so they survive reboots.
// Keys: fk, fa, fm, fx
void loadFlowFromPrefs() {
  if (!dosingPrefs.begin("flow", true)) {
    Serial.println("Prefs: failed to open flow (read)");
    return;
  }

  FLOW_KALK_ML_PER_MIN = dosingPrefs.getFloat("fk", FLOW_KALK_ML_PER_MIN);
  FLOW_AFR_ML_PER_MIN  = dosingPrefs.getFloat("fa", FLOW_AFR_ML_PER_MIN);
  FLOW_MG_ML_PER_MIN   = dosingPrefs.getFloat("fm", FLOW_MG_ML_PER_MIN);
  FLOW_TBD_ML_PER_MIN  = dosingPrefs.getFloat("fx", FLOW_TBD_ML_PER_MIN);
  FLOW_AUX_ML_PER_MIN = FLOW_TBD_ML_PER_MIN; // alias for legacy code

  dosingPrefs.end();

  Serial.print("Prefs: loaded flow KALK=");
  Serial.print(FLOW_KALK_ML_PER_MIN);
  Serial.print(" AFR=");
  Serial.print(FLOW_AFR_ML_PER_MIN);
  Serial.print(" MG=");
  Serial.print(FLOW_MG_ML_PER_MIN);
  Serial.print(" AUX=");
  Serial.println(FLOW_TBD_ML_PER_MIN);
}



static void validateFlow(const char* name, float &flow, float fallback) {
  // Reasonable range for typical dosing pumps (ml/min). Adjust if needed.
  if (!isfinite(flow) || flow < 30.0f || flow > 5000.0f) {
    Serial.printf("Prefs: %s flow %.2f is invalid. Using fallback %.2f\n", name, flow, fallback);
    flow = fallback;
  }
}

// Forward-declared here; implementation is below dosing schedule globals.


void saveFlowToPrefs() {
  if (!dosingPrefs.begin("flow", false)) {
    Serial.println("Prefs: failed to open flow (write)");
    return;
  }

  dosingPrefs.putFloat("fk", FLOW_KALK_ML_PER_MIN);
  dosingPrefs.putFloat("fa", FLOW_AFR_ML_PER_MIN);
  dosingPrefs.putFloat("fm", FLOW_MG_ML_PER_MIN);
  dosingPrefs.putFloat("fx", FLOW_TBD_ML_PER_MIN);

  dosingPrefs.end();
}

void saveDosingToPrefs() {
  if (!dosingPrefs.begin("dosing", false)) {
    Serial.println("Prefs: failed to open dosing (write)");
    return;
  }

  dosingPrefs.putFloat("kalk", dosing.ml_per_day_kalk);
  dosingPrefs.putFloat("afr",  dosing.ml_per_day_afr);
  dosingPrefs.putFloat("mg",   dosing.ml_per_day_mg);
  dosingPrefs.putFloat("tbd",   dosing.ml_per_day_tbd);

  dosingPrefs.end();

  Serial.print("Prefs: saved dosing KALK=");
  Serial.print(dosing.ml_per_day_kalk);
  Serial.print(" AFR=");
  Serial.print(dosing.ml_per_day_afr);
  Serial.print(" MG=");
  Serial.println(dosing.ml_per_day_mg);
}



// ===================== TEST HISTORY FOR GRAPHS =====================

const int MAX_HISTORY = 64;
TestPoint historyBuf[MAX_HISTORY];
int historyCount = 0;

TestPoint lastTest    = {0, 0, 0, 0, 0};
TestPoint currentTest = {0, 0, 0, 0, 0};

// For Firebase-based AI timing (if we later pull tests from RTDB)
uint64_t lastRemoteTestTimestampMs = 0;


// ===================== DOSING SCHEDULE (REAL-TIME, 3 DOSES/DAY) =====================

// 3 doses per day per pump
const int DOSES_PER_DAY_KALK = 3;
const int DOSES_PER_DAY_AFR  = 3;
const int DOSES_PER_DAY_MG   = 3;

// AUX pump is optional; not part of the daily schedule by default.
const int   DOSES_PER_DAY_AUX  = 0;
float       SEC_PER_DOSE_TBD   = 0.0f;

// per-dose run time (seconds)
float SEC_PER_DOSE_KALK = 0.0f;
float SEC_PER_DOSE_AFR  = 0.0f;
float SEC_PER_DOSE_MG   = 0.0f;

// ===================== DOSING SCHEDULE (DAILY WINDOWS) =====================
//
// We support either:
//  - Default (legacy) fixed times: 09:30, 12:30, 15:30
//  - Configurable window + interval from RTDB: /devices/<id>/settings/doseSchedule
//
// Internals use a fixed max slot array so we can change the active slot count at runtime.
static const int MAX_DOSE_SLOTS = 96; // 24h @ 15-minute resolution

int  DOSE_SLOTS_PER_DAY = 3; // active slots count (runtime)
int  DOSE_HOURS[MAX_DOSE_SLOTS]   = {9, 12, 15};
int  DOSE_MINUTES[MAX_DOSE_SLOTS] = {30, 30, 30};
bool slotDone[MAX_DOSE_SLOTS]     = {false};

// Track day/window reset
int  lastDoseWindowDay = -1; // yday of the window start (handles wrap windows)
bool doseSlotsPrimed   = false;

// Config pulled from RTDB
struct DoseScheduleCfg {
  bool enabled   = false;
  int  startHour = 0;   // 0..23
  int  endHour   = 0;   // 0..23
  int  everyMin  = 60;  // 15/60/120/240 etc
  uint64_t updatedAt = 0;
};
DoseScheduleCfg doseScheduleCfg;

// --- schedule helper prototypes ---
static int  clampInt(int v, int lo, int hi);
static bool scheduleWraps(int startHour, int endHour);
static int  scheduleSlotIndex(const tm& t, int startHour, int endHour, int everyMin);
static int  windowStartYday(const tm& t, int startHour, int endHour);
static void rebuildScheduleSlots();


// Time validity guard: avoid dosing before NTP sync is real.
// tm_year is years since 1900. 123 == 2023.
/*bool isTimeValid(const tm& t) {
  return (t.tm_year >= 123);
}*/

bool isTimeValid(const tm& t) {
  if (t.tm_year >= 123) { // 123 = Year 2023
    return true;
  } else {
    // Time is invalid! 
    static unsigned long lastFallbackAttempt = 0;
    // Only try Firebase sync once every 5 minutes so we don't lag the loop
    if (millis() - lastFallbackAttempt > 300000UL) { 
      lastFallbackAttempt = millis();
      Serial.println("Invalid time detected in loop. Attempting Firebase fallback...");
      syncTimeFromFirebaseHeader(); 
    }
    return false;
  }
}

// ===================== schedule helpers =====================
static int clampInt(int v, int lo, int hi) { return (v < lo) ? lo : (v > hi) ? hi : v; }

static bool scheduleWraps(int startHour, int endHour) {
  startHour = clampInt(startHour, 0, 23);
  endHour   = clampInt(endHour,   0, 23);
  return (endHour <= startHour);
}

// Returns slot index within the dosing window (0..slots-1) or -1 if now is outside window
static int scheduleSlotIndex(const tm& t, int startHour, int endHour, int everyMin) {
  startHour = clampInt(startHour, 0, 23);
  endHour   = clampInt(endHour,   0, 23);
  everyMin  = clampInt(everyMin,  1, 1440);

  const int nowMin   = t.tm_hour * 60 + t.tm_min;
  const int startMin = startHour * 60;
  const int endMin   = endHour * 60;

  int windowLen = 0;
  int offset = 0;

  if (!scheduleWraps(startHour, endHour)) {
    // simple window [start,end)
    if (nowMin < startMin || nowMin >= endMin) return -1;
    windowLen = endMin - startMin;
    offset = nowMin - startMin;
  } else {
    // wraps over midnight: [start, 24h) U [0, end)
    windowLen = (24*60 - startMin) + endMin;
    if (nowMin >= startMin) {
      offset = nowMin - startMin;
    } else if (nowMin < endMin) {
      offset = (24*60 - startMin) + nowMin;
    } else {
      return -1;
    }
  }

  if (windowLen <= 0) return -1;
  const int slots = windowLen / everyMin; // floor
  if (slots <= 0) return -1;

  int idx = offset / everyMin;
  if (idx < 0) idx = 0;
  if (idx >= slots) idx = slots - 1;
  return idx;
}

// Identify which "window day" we're in (yday of the window start). This makes wrap windows stable.
static int windowStartYday(const tm& t, int startHour, int endHour) {
  if (!scheduleWraps(startHour, endHour)) return t.tm_yday;
  // window starts at startHour; if we're before endHour, we're still in yesterday's window
  if (t.tm_hour < endHour) return t.tm_yday - 1;
  return t.tm_yday;
}

// Build DOSE_HOURS / DOSE_MINUTES arrays from doseScheduleCfg (or legacy defaults).
static void rebuildScheduleSlots() {
  // default legacy schedule if not enabled
  if (!doseScheduleCfg.enabled) {
    DOSE_SLOTS_PER_DAY = 3;
    DOSE_HOURS[0] = 9;  DOSE_MINUTES[0] = 30;
    DOSE_HOURS[1] = 12; DOSE_MINUTES[1] = 30;
    DOSE_HOURS[2] = 15; DOSE_MINUTES[2] = 30;
    return;
  }

  int startHour = clampInt(doseScheduleCfg.startHour, 0, 23);
  int endHour   = clampInt(doseScheduleCfg.endHour,   0, 23);
  int everyMin  = clampInt(doseScheduleCfg.everyMin,  1, 240);

  const int startMin = startHour * 60;
  const int endMin   = endHour * 60;
  int windowLen = 0;
  if (!scheduleWraps(startHour, endHour)) {
    windowLen = endMin - startMin;
  } else {
    windowLen = (24*60 - startMin) + endMin;
  }
  if (windowLen <= 0) windowLen = 24*60; 

  int slots = windowLen / everyMin;
  if (slots < 1) slots = 1;
  if (slots > MAX_DOSE_SLOTS) slots = MAX_DOSE_SLOTS;

  DOSE_SLOTS_PER_DAY = slots;

  for (int i = 0; i < DOSE_SLOTS_PER_DAY; i++) {
    int m = startMin + i * everyMin;
    m %= (24*60);
    DOSE_HOURS[i] = m / 60;
    DOSE_MINUTES[i] = m % 60;
    // --- REMOVED: slotDone[i] = false; --- 
    // Do NOT reset the 'done' status here!
  }

  Serial.printf("DoseSchedule rebuilt: %d slots total.\n", DOSE_SLOTS_PER_DAY);
}

// On boot/restart, we do NOT want to "catch up" on earlier slots.
// When time becomes valid, mark any already-passed slots as done.
void primeDoseSlotsForToday() {
  struct tm t;
  if (!getLocalTime(&t)) {
    Serial.println("WARN: cannot prime slots (no time yet)");
    return;
  }
  if (!isTimeValid(t)) {
    Serial.println("WARN: cannot prime slots (time invalid yet)");
    return;
  }

  // Ensure slot list reflects current config
  rebuildScheduleSlots();

  // Reset flags for this dosing window/day
  lastDoseWindowDay = doseScheduleCfg.enabled ? windowStartYday(t, doseScheduleCfg.startHour, doseScheduleCfg.endHour) : t.tm_yday;
  
  // Initialize all slots to false first
  for (int i = 0; i < DOSE_SLOTS_PER_DAY; i++) {
    slotDone[i] = false;
  }

  if (doseScheduleCfg.enabled) {
    const int nowIdx = scheduleSlotIndex(t, doseScheduleCfg.startHour, doseScheduleCfg.endHour, doseScheduleCfg.everyMin);
    if (nowIdx >= 0) {
      // Mark current and past slots as true so we don't "catch up" rapidly
      for (int i = 0; i <= nowIdx && i < DOSE_SLOTS_PER_DAY; i++) {
        slotDone[i] = true; 
      }
    }
  } else {
    // Legacy compare by time-of-day
    for (int i = 0; i < DOSE_SLOTS_PER_DAY; i++) {
      int sh = DOSE_HOURS[i];
      int sm = DOSE_MINUTES[i];
      bool reached = (t.tm_hour > sh) || (t.tm_hour == sh && t.tm_min >= sm);
      if (reached) slotDone[i] = true;
    }
  }

  doseSlotsPrimed = true;
  Serial.printf("Dose slots primed for today (yday=%d, now=%02d:%02d:%02d)\n", t.tm_yday, t.tm_hour, t.tm_min, t.tm_sec);
}


// ===================== IFTTT WEBHOOK SETUP =====================
// Get your key from: https://ifttt.com/maker_webhooks

const char* IFTTT_HOST = "maker.ifttt.com";
const int   IFTTT_PORT = 80;

// 🔴 REPLACE THIS with your actual Maker key
const char* IFTTT_KEY = "fBplW8jJqqotTqTxck4oTdK_oHTJKAawKfja-WlcgW-";

// Forward declaration (IFTTT no longer used; kept for reference)
// int sendIFTTT(String eventName,
//               String value1 = "",
//               String value2 = "",
//               String value3 = "");

String getLocalTimeString();
bool isTimeValid(const tm& t);



void firebaseSendStateHeartbeat();
void primeDoseSlotsForToday();
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
  // Use DOSE_SLOTS_PER_DAY (the actual active slots) instead of the hardcoded '3'
  int activeSlots = (doseScheduleCfg.enabled) ? DOSE_SLOTS_PER_DAY : 3;

  if(FLOW_KALK_ML_PER_MIN > 0 && activeSlots > 0){
    float secondsPerDay = (dosing.ml_per_day_kalk / FLOW_KALK_ML_PER_MIN) * 60.0f;
    SEC_PER_DOSE_KALK = secondsPerDay / activeSlots;
    Serial.print("SEC_PER_DOSE_KALK updated to: "); Serial.println(SEC_PER_DOSE_KALK);
  }

  if(FLOW_AFR_ML_PER_MIN > 0 && activeSlots > 0){
    float secondsPerDay = (dosing.ml_per_day_afr / FLOW_AFR_ML_PER_MIN) * 60.0f;
    SEC_PER_DOSE_AFR = secondsPerDay / activeSlots;
    Serial.print("SEC_PER_DOSE_AFR updated to: "); Serial.println(SEC_PER_DOSE_AFR);
  }

  if(FLOW_MG_ML_PER_MIN > 0 && activeSlots > 0){
    float secondsPerDay = (dosing.ml_per_day_mg / FLOW_MG_ML_PER_MIN) * 60.0f;
    SEC_PER_DOSE_MG = secondsPerDay / activeSlots;
    Serial.print("SEC_PER_DOSE_MG updated to: "); Serial.println(SEC_PER_DOSE_MG);
  }
  if(FLOW_TBD_ML_PER_MIN > 0 && activeSlots > 0){
  float secondsPerDay = (dosing.ml_per_day_tbd / FLOW_TBD_ML_PER_MIN) * 60.0f;
  SEC_PER_DOSE_TBD = secondsPerDay / activeSlots; 
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


// ===================== IFTTT HELPERS (LEGACY, NOT USED) =====================

// int sendIFTTT(String eventName,
//               String value1,
//               String value2,
//               String value3) {

/*
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

  // return 0;
}*/
// )LEGACY

void syncTimeFromFirebaseHeader() {
  HTTPClient http;
  // Use your actual Firebase URL
  http.begin("https://aidoser-default-rtdb.firebaseio.com/.json"); 
  
  const char* headerKeys[] = {"Date"};
  http.collectHeaders(headerKeys, 1);

  int httpCode = http.GET();
  if (httpCode > 0) {
    String dateStr = http.header("Date"); // Looks like: "Wed, 21 Oct 2023 07:28:00 GMT"
    
    struct tm tm;
    // This parses the standard web time format into the ESP32 time structure
    if (strptime(dateStr.c_str(), "%a, %d %b %Y %H:%M:%S GMT", &tm)) {
      time_t t = mktime(&tm);
      
      // Apply the time to the system
      struct timeval tv = { .tv_sec = t };
      settimeofday(&tv, NULL);
      
      Serial.println("System clock updated via Firebase Header: " + dateStr);
    }
  }
  http.end();
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


// ===================== SAFETY: CHEMISTRY

// Helper: get a best-effort timestamp in ms since epoch
uint64_t getEpochMillis() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    time_t t = mktime(&timeinfo);
    if (t > 0) {
      return (uint64_t)t * 1000ULL;
    }
  }
  // Fallback to millis() if we don't have real time yet
  return (uint64_t)millis();
}


// ===================== ALERT / PUSH THROTTLING =====================
// Prevent RTDB from filling up with duplicate alerts & pushes.
// We keep a small in-RAM "cooldown" table keyed by a short string.
struct ThrottleEntry { const char* key; uint64_t lastTs; };
static ThrottleEntry gThrottle[] = {
  {"boot_push", 0},
  {"offline_push", 0},
  {"safety_scale", 0},
  {"test_ignored", 0},
  {"ota_fail", 0},
  {"wifi_fail", 0},
  {"generic_alert", 0},
    {"no_tests", 0},
    {"dose_kalk", 0},
    {"dose_afr", 0},
    {"dose_mg", 0},
    {"dose_live", 0},
    {"online_state", 0},
};
static const size_t gThrottleCount = sizeof(gThrottle)/sizeof(gThrottle[0]);

bool allowThrottled(const char* key, uint64_t cooldownMs) {
  uint64_t now = getEpochMillis();
  for (size_t i = 0; i < gThrottleCount; i++) {
    if (strcmp(gThrottle[i].key, key) == 0) {
      if (gThrottle[i].lastTs == 0 || (now - gThrottle[i].lastTs) >= cooldownMs) {
        gThrottle[i].lastTs = now;
        return true;
      }
      return false;
    }
  }
  // Unknown key: allow but don't track (or track under generic)
  return true;
}


// Helper: basic JSON string escaping (quotes, backslash, newlines)
String jsonEscape(const String& s) {
  String out;
  out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '\\') out += "\\\\";
    else if (c == '"') out += "\\\"";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else out += c;
  }
  return out;
}

// Helper: push an alert into Firebase RTDB under /devices/{DEVICE_ID}/alerts
// This replaces the old IFTTT push usage – your Cloud Function can listen
// to /devices/{deviceId}/alerts and fan out SMS / email.

// Helper: push an alert into Firebase RTDB.
// To stop the DB growing out of control:
//  1) Always overwrite a "latest" slot: /devices/{DEVICE_ID}/alertsLatest/{type}
//  2) Optionally POST into /devices/{DEVICE_ID}/alerts (history) only if throttle allows.
void firebasePushAlert(const String& type,
                       const String& title,
                       const String& body,
                       const String& extra,
                       const char* throttleKey,
                       uint64_t cooldownMs) {
  uint64_t ts = getEpochMillis();

  String json = "{";
  json += "\"type\":\"" + jsonEscape(type) + "\"";
  json += ",\"title\":\"" + jsonEscape(title) + "\"";
  json += ",\"body\":\"" + jsonEscape(body) + "\"";
  json += ",\"extra\":\"" + jsonEscape(extra) + "\"";
  json += ",\"deviceId\":\"" + String(DEVICE_ID) + "\"";
  json += ",\"timestamp\":" + String((unsigned long long)ts);
  json += "}";

  // Overwrite latest
  firebasePutJson("/devices/" + String(DEVICE_ID) + "/alertsLatest/" + type, json);

  // Push to history only occasionally
  if (allowThrottled(throttleKey ? throttleKey : "generic_alert", cooldownMs)) {
    firebasePostJson("/devices/" + String(DEVICE_ID) + "/alerts", json);
  }
}


// Helper: push a PUSH notification into Firebase RTDB under
// /devices/{DEVICE_ID}/notifications (Cloud Function listens for child create)
bool firebasePushNotification(const String& severity,
                              const String& title,
                              const String& body) {
  String path = "/devices/" + String(DEVICE_ID) + "/notifications";

  uint64_t ts = getEpochMillis();

  String json = "{";
  json += "\"severity\":\"" + jsonEscape(severity) + "\"";
  json += ",\"title\":\"" + jsonEscape(title) + "\"";
  json += ",\"body\":\"" + jsonEscape(body) + "\"";
  json += ",\"deviceId\":\"" + String(DEVICE_ID) + "\"";
  json += ",\"ts\":" + String((unsigned long long)ts);
  json += "}";

  // POST -> creates a unique key each time (required for onCreate trigger)
  return firebasePostJson(path, json);
}

// Throttled push notification helper (reduces spam).
bool firebasePushNotificationThrottled(const char* throttleKey,
                                      uint64_t cooldownMs,
                                      const String& severity,
                                      const String& title,
                                      const String& body) {
  if (!allowThrottled(throttleKey ? throttleKey : "generic_alert", cooldownMs)) return false;
  return firebasePushNotification(severity, title, body);
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
    dosing.ml_per_day_tbd *= scale; // Keep Pump 4 in sync with safety scaling
    Serial.print("SAFETY: Scaling dosing by ");
    Serial.println(scale, 3);

    // Firebase alert: dosing scaled by safety
    firebasePushAlert("safety",
                      "Dosing scaled by safety",
                      "scale=" + String(scale, 3),
                      getLocalTimeString(),
                      "safety_scale",
                      6ULL*60ULL*60ULL*1000ULL);  // 6 hours
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

  // Save default dosing so it becomes the new baseline
  saveDosingToPrefs();

  // Optional: Firebase alert for AI reset
  firebasePushAlert("reset",
                    "AI dosing engine reset",
                    WiFi.localIP().toString(),
                    getLocalTimeString(),
                    "generic_alert",
                    30ULL*60ULL*1000ULL);  // 30 min

  Serial.println("AI state reset complete.");
}


// ===================== “AI” CONTROL (with pH bias & safety) =====================

void onNewTestInput(float ca, float alk, float mg, float ph, float tbd_val) {
  // 1. Update history for graph
  lastTest = currentTest;
  currentTest.t   = nowSeconds();
  currentTest.ca  = ca;
  currentTest.alk = alk;
  currentTest.mg  = mg;
  currentTest.ph  = ph;
  currentTest.tbd = tbd_val; // Added TBD to history struct
  pushHistory(currentTest);

  // 2. Sanity check ranges (Safety First)
  if (ca   < 300.0f || ca   > 550.0f ||
      alk  <   5.0f || alk  > 14.0f  ||
      mg   < 1100.0f || mg   > 1600.0f ||
      ph   <   7.0f || ph   >   9.0f) {
    Serial.println("SAFETY: IGNORING TEST for dosing (out-of-range). Graph updated only.");
    return;
  }

  // 3. First valid test handling
  if(lastTest.t == 0){
    updatePumpSchedules();
    return;
  }

  // 4. Time delta check
  float days = float(currentTest.t - lastTest.t) / 86400.0f;
  if(days <= 0.25f) {
    Serial.println("SAFETY: Tests too close together, ignoring for dosing updates.");
    return;
  }

  // 5. Consumption per day calculations
  float consAlk = (lastTest.alk - currentTest.alk) / days;
  float consCa  = (lastTest.ca  - currentTest.ca ) / days;
  float consMg  = (lastTest.mg  - currentTest.mg ) / days;
  float consTbd = (lastTest.tbd - currentTest.tbd) / days; // Track Pump 4 consumption

  float alkNeeded = consAlk;
  if(alkNeeded < 0.0f) alkNeeded = 0.0f;

  // --- 6. PH BIAS LOGIC (Kalk vs AFR) ---
  float kalkFrac = 0.8f;  // default: 80% alk from kalk
  if (!isnan(currentTest.ph)) {
    float phError = currentTest.ph - TARGET_PH;
    if (phError < -0.05f) {
      kalkFrac = 0.90f; // pH low -> prioritize Kalk
    } else if (phError > 0.05f) {
      kalkFrac = 0.70f; // pH high -> shift toward AFR
    }
  }
  kalkFrac = clampf(kalkFrac, 0.6f, 0.95f);

  // --- 7. SUGGESTED RATES ---
  float targetAlkFromKalk = kalkFrac * alkNeeded;
  float targetAlkFromAfr  = (1.0f - kalkFrac) * alkNeeded;

  float suggested_ml_kalk = (DKH_PER_ML_KALK_TANK > 0.0f) ? (targetAlkFromKalk / DKH_PER_ML_KALK_TANK) : 0.0f;
  float suggested_ml_afr  = (DKH_PER_ML_AFR_TANK  > 0.0f) ? (targetAlkFromAfr  / DKH_PER_ML_AFR_TANK ) : 0.0f;

  // Calculate what AFR/Kalk provide for Ca/Mg
  float caFromKalk = suggested_ml_kalk * CA_PPM_PER_ML_KALK_TANK;
  float caFromAfr  = suggested_ml_afr  * CA_PPM_PER_ML_AFR_TANK;
  float mgFromAfr  = suggested_ml_afr  * MG_PPM_PER_ML_AFR_TANK;

  // Ca Error Correction
  float caError = consCa - (caFromKalk + caFromAfr);
  if(fabsf(caError) > 5.0f){
    float afrCorrection = (caError / CA_PPM_PER_ML_AFR_TANK) * 0.3f;
    suggested_ml_afr += afrCorrection;
  }

  // Mg Error Correction (Pump 3)
  float suggested_ml_mg = dosing.ml_per_day_mg;
  if(consMg > mgFromAfr + 0.5f){
    float mgCorrection = (consMg - mgFromAfr) / MG_PPM_PER_ML_MG_TANK;
    mgCorrection *= 0.3f;
    suggested_ml_mg += mgCorrection;
  }

  // TBD Placeholder Correction (Pump 4)
  float suggested_ml_tbd = dosing.ml_per_day_tbd;
  if (consTbd > 0.1f) {
    // If you add a TBD_PPM_PER_ML constant, use it here like Mg
    suggested_ml_tbd += (consTbd * 0.2f); 
  }

  // 8. FINAL LIMITS & CLAMPS
  suggested_ml_kalk = max(0.0f, suggested_ml_kalk);
  suggested_ml_afr  = max(0.0f, suggested_ml_afr);
  suggested_ml_mg   = max(0.0f, suggested_ml_mg);
  suggested_ml_tbd  = max(0.0f, suggested_ml_tbd);

  dosing.ml_per_day_kalk = adjustWithLimit(dosing.ml_per_day_kalk, suggested_ml_kalk);
  dosing.ml_per_day_afr  = adjustWithLimit(dosing.ml_per_day_afr,  suggested_ml_afr);
  dosing.ml_per_day_mg   = adjustWithLimit(dosing.ml_per_day_mg,   suggested_ml_mg);
  dosing.ml_per_day_tbd  = adjustWithLimit(dosing.ml_per_day_tbd,  suggested_ml_tbd);

  dosing.ml_per_day_kalk = clampf(dosing.ml_per_day_kalk, 0.0f, MAX_KALK_ML_PER_DAY);
  dosing.ml_per_day_afr  = clampf(dosing.ml_per_day_afr,  0.0f, MAX_AFR_ML_PER_DAY);
  dosing.ml_per_day_mg   = clampf(dosing.ml_per_day_mg,   0.0f, MAX_MG_ML_PER_DAY);
  dosing.ml_per_day_tbd  = clampf(dosing.ml_per_day_tbd,  0.0f, MAX_TBD_ML_PER_DAY);

  // 9. WRAP UP
  enforceChemSafetyCaps();
  updatePumpSchedules();
  saveDosingToPrefs();
  lastSafetyBackoffTs = nowSeconds();

  Serial.println("AI Update: 4-Pump Dosing Plan Recalculated.");
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

  // Persist backed-off dosing plan
  saveDosingToPrefs();

  lastSafetyBackoffTs = now;

  // Firebase alert: no tests + backoff
  firebasePushAlert("safety",
                    "No tests >5 days",
                    "Dosing backed off to 70%",
                    getLocalTimeString(),
                    "no_tests",
                    24ULL*60ULL*60ULL*1000ULL);
}


// ===================== PUMP SCHEDULER (REAL-TIME SLOTS) =====================

bool giveDose(int pin, float seconds) {
  if (globalEmergencyStop) {
        Serial.println("Pump execution blocked: E-Stop is ACTIVE.");
        return false;
    }
  if (seconds <= 0) return false;

  const uint32_t totalMs = (uint32_t)(seconds * 1000.0f);
  const uint32_t beatEveryMs = 5000;   // keep RTDB heartbeat fresh while dosing
  const uint32_t loopDelayMs = 50;     // yield to WiFi stack

  digitalWrite(pin, HIGH);

  uint32_t start = millis();
  uint32_t lastBeat = start;

  while ((uint32_t)(millis() - start) < totalMs) {
    delay(loopDelayMs);

    // Keep the device marked online while a long dose is running
    if ((uint32_t)(millis() - lastBeat) >= beatEveryMs) {
      lastBeat = millis();
      firebaseSendStateHeartbeat();
    }
  }

  digitalWrite(pin, LOW);
  return true;
}



// Doses a specific amount on a specific pump, then logs it to RTDB.
void doseAndLog(int pumpIndex, const String& pumpName, int pin, float ml, float flowMlPerMin, const String& source) {
  if (ml <= 0.0f || flowMlPerMin <= 0.0f) return;
  const float durationSec = (ml / flowMlPerMin) * 60.0f;
  if (!giveDose(pin, durationSec)) return;
  firebaseLogDoseRun(pumpIndex, pumpName, ml, durationSec, flowMlPerMin, source);
}


void maybeDosePumpsRealTime() {
  if (WiFi.status() != WL_CONNECTED) return;
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;
  if (!isTimeValid(timeinfo)) return;

  // ... [Day change logic remains exactly as you have it] ...
  const int windowDay = doseScheduleCfg.enabled ? windowStartYday(timeinfo, doseScheduleCfg.startHour, doseScheduleCfg.endHour)
                                                 : timeinfo.tm_yday;

  if (!doseSlotsPrimed || windowDay != lastDoseWindowDay) {
    // [Your existing Day Change code here]
    lastDoseWindowDay = windowDay; 
    doseSlotsPrimed = true; 
    rebuildScheduleSlots();
    for (int i = 0; i < DOSE_SLOTS_PER_DAY; i++) slotDone[i] = false;
    // ...
  }

  // Determine nowIdx as you already do...
  int nowIdx = -1;
  if (doseScheduleCfg.enabled) {
    nowIdx = scheduleSlotIndex(timeinfo, doseScheduleCfg.startHour, doseScheduleCfg.endHour, doseScheduleCfg.everyMin);
  } else {
    // legacy window logic...
  }

  // --- ACCUMULATION DOSING LOGIC WITH MEMORY ---
  if (nowIdx >= 0 && nowIdx < DOSE_SLOTS_PER_DAY) {
    if (!slotDone[nowIdx]) {
      
      // 1. LOAD current buckets from permanent memory
      Preferences prefs;
      prefs.begin("doser-buckets", false); // false = read/write mode
      pendingKalkMl = prefs.getFloat("p_kalk", 0.0f);
      pendingAfrMl  = prefs.getFloat("p_afr", 0.0f);
      pendingMgMl   = prefs.getFloat("p_mg", 0.0f);
      pendingTbdMl   = prefs.getFloat("p_tbd", 0.0f);

      // 2. Add current slot's requirement
      pendingKalkMl += dosing.ml_per_day_kalk / (float)max(1, DOSE_SLOTS_PER_DAY);
      pendingAfrMl  += dosing.ml_per_day_afr  / (float)max(1, DOSE_SLOTS_PER_DAY);
      pendingMgMl   += dosing.ml_per_day_mg   / (float)max(1, DOSE_SLOTS_PER_DAY);
      pendingTbdMl   += dosing.ml_per_day_tbd   / (float)max(1, DOSE_SLOTS_PER_DAY);

      Serial.printf("Slot %d: Buckets Loaded (Kalk:%.2fml, AFR:%.2fml, MG:%.2fml, TBD:%.2fml)\n", nowIdx + 1, pendingKalkMl, pendingAfrMl,pendingMgMl,pendingTbdMl);

      // 3. KALK 
      if (pendingKalkMl > 0.0f && FLOW_KALK_ML_PER_MIN > 0.0f) {
        float sec = (pendingKalkMl / FLOW_KALK_ML_PER_MIN) * 60.0f;
        if (sec >= MIN_DOSE_SEC) {
          if (giveDose(PIN_PUMP_KALK, sec)) {
          firebaseLogDoseRun(1, "kalk", pendingKalkMl, sec, FLOW_KALK_ML_PER_MIN, "schedule");
          pendingKalkMl = 0.0f;
        } else {
          Serial.println("E-Stop active: skipped KALK dosing, kept pending volume.");
        } 
        } else {
          Serial.println("Kalk deferred (under 1s).");
        }
      }

      // 4. AFR 
      if (pendingAfrMl > 0.0f && FLOW_AFR_ML_PER_MIN > 0.0f) {
        float sec = (pendingAfrMl / FLOW_AFR_ML_PER_MIN) * 60.0f;
        if (sec >= MIN_DOSE_SEC) {
          if (giveDose(PIN_PUMP_AFR, sec)) {
          firebaseLogDoseRun(2, "afr", pendingAfrMl, sec, FLOW_AFR_ML_PER_MIN, "schedule");
          pendingAfrMl = 0.0f;
        } else {
          Serial.println("E-Stop active: skipped AFR dosing, kept pending volume.");
        } 
        } else {
          Serial.println("AFR deferred (under 1s).");
        }
      }

      // 5. MG 
      if (pendingMgMl > 0.0f && FLOW_MG_ML_PER_MIN > 0.0f) {
        float sec = (pendingMgMl / FLOW_MG_ML_PER_MIN) * 60.0f;
        if (sec >= MIN_DOSE_SEC) {
          if (giveDose(PIN_PUMP_MG, sec)) {
          firebaseLogDoseRun(3, "mg", pendingMgMl, sec, FLOW_MG_ML_PER_MIN, "schedule");
          pendingMgMl = 0.0f;
        } else {
          Serial.println("E-Stop active: skipped MG dosing, kept pending volume.");
        } 
        }else {
          Serial.println("Mg deferred (under 1s).");
        }
      }
      // 5. TBD
      if (pendingTbdMl > 0.0f && FLOW_TBD_ML_PER_MIN > 0.0f) {
        float sec = (pendingTbdMl / FLOW_TBD_ML_PER_MIN) * 60.0f;
        if (sec >= MIN_DOSE_SEC) {
          if (giveDose(PIN_PUMP_TBD, sec)) {
          firebaseLogDoseRun(4, "aux", pendingTbdMl, sec, FLOW_TBD_ML_PER_MIN, "schedule");
          pendingTbdMl = 0.0f;
        } else {
          Serial.println("E-Stop active: skipped AUX dosing, kept pending volume.");
        } 
        }else {
          Serial.println("TBD deferred (under 1s).");
        }
      }


      // 6. SAVE buckets back to memory so they survive a reboot
      prefs.putFloat("p_kalk", pendingKalkMl);
      prefs.putFloat("p_afr", pendingAfrMl);
      prefs.putFloat("p_mg", pendingMgMl);
      prefs.putFloat("p_tbd", pendingTbdMl);
      prefs.end();

      slotDone[nowIdx] = true;
    }
  }
}
// ===================== FIREBASE: resetAi COMMAND =====================

// Check /devices/{DEVICE_ID}/commands/resetAi
// If true, reset AI and clear the flag.
// Returns true if a reset was performed.
bool firebaseCheckAndHandleResetAi() {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  String path = "/devices/" + String(DEVICE_ID) + "/commands/resetAi";
  String payload = firebaseGetJson(path);
  if (payload.length() == 0 || payload == "null") {
    return false;
  }

  Serial.print("resetAi payload: ");
  Serial.println(payload);

  // If it's literally true (could be true or "true")
  if (payload.indexOf("true") != -1) {
    // Perform reset
    resetAIState();

    // Clear the flag back to false
    firebasePutJson(path, "false");
    Serial.println("resetAi flag cleared in Firebase.");
    return true;
  }

  return false;
}


// ===================== FIREBASE: Live Dose COMMAND =====================

// Perform one "live" dose using current per-day plan (one of the 3 doses)
void runLiveDoseOnce() {
  Serial.println("=== LIVE DOSE REQUESTED ===");

  // This function is used by the web UI "dose now" action (if you keep it),
  // and it doses one "slot" worth (SEC_PER_DOSE_*) for each pump.
  if (SEC_PER_DOSE_KALK > 0.0f) {
    const float ml = (SEC_PER_DOSE_KALK / 60.0f) * FLOW_KALK_ML_PER_MIN;
    doseAndLog(1, "kalk", PIN_PUMP_KALK, ml, FLOW_KALK_ML_PER_MIN, "slot");
  }
  if (SEC_PER_DOSE_AFR > 0.0f) {
    const float ml = (SEC_PER_DOSE_AFR / 60.0f) * FLOW_AFR_ML_PER_MIN;
    doseAndLog(2, "afr", PIN_PUMP_AFR, ml, FLOW_AFR_ML_PER_MIN, "slot");
  }
  if (SEC_PER_DOSE_MG > 0.0f) {
    const float ml = (SEC_PER_DOSE_MG / 60.0f) * FLOW_MG_ML_PER_MIN;
    doseAndLog(3, "mg", PIN_PUMP_MG, ml, FLOW_MG_ML_PER_MIN, "slot");
  }
  if (SEC_PER_DOSE_TBD > 0.0f) {
    const float ml = (SEC_PER_DOSE_TBD / 60.0f) * FLOW_AUX_ML_PER_MIN;
    doseAndLog(4, "aux", PIN_PUMP_TBD, ml, FLOW_AUX_ML_PER_MIN, "slot");
  }

  Serial.println("=== LIVE DOSE COMPLETE ===");
}

// Check /devices/{DEVICE_ID}/commands/liveDose
bool firebaseCheckAndHandleLiveDose() {
  const String path = "/devices/" + String(DEVICE_ID) + "/commands/liveDose";
  String payload = firebaseGetJson(path);

  // Typical payload:
  // {"trigger":true,"pump":1,"ml":5}
  if (payload.length() == 0 || payload == "null") return false;

  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.println("LiveDose: JSON parse error, ignoring");
    return false;
  }

  bool trigger = doc["trigger"] | false;
  if (!trigger) return false;

  int pump = doc["pump"] | 0;
  float ml = doc["ml"] | 0.0f;

  // Safety sanity checks
  if (pump < 1 || pump > 4 || ml <= 0.0f) {
    Serial.println("LiveDose: invalid pump/ml, clearing trigger");
    String clearJson = "{\"trigger\":false,\"lastRun\":" + String((unsigned long long)getEpochMillis()) + "}";
    firebasePutJson(path, clearJson);
    return true;
  }

  int pin = -1;
  float flow = 0.0f;
  String pumpName;

  switch (pump) {
    case 1: pin = PIN_PUMP_KALK; flow = FLOW_KALK_ML_PER_MIN; pumpName = "kalk"; break;
    case 2: pin = PIN_PUMP_AFR;  flow = FLOW_AFR_ML_PER_MIN;  pumpName = "afr";  break;
    case 3: pin = PIN_PUMP_MG;   flow = FLOW_MG_ML_PER_MIN;   pumpName = "mg";   break;
    case 4: pin = PIN_PUMP_TBD;  flow = FLOW_TBD_ML_PER_MIN;  pumpName = "tbd";  break;
  }

  if (pin < 0 || flow <= 0.0f) {
    Serial.println("LiveDose: invalid pin/flow, clearing trigger");
    String clearJson = "{\"trigger\":false,\"lastRun\":" + String((unsigned long long)getEpochMillis()) + "}";
    firebasePutJson(path, clearJson);
    return true;
  }

  // Clamp to something sane so you don't accidentally run for hours from a typo.
  const float maxMl = 2000.0f; // adjust later if you want
  if (ml > maxMl) ml = maxMl;

  const float durationSec = (ml / flow) * 60.0f;
  Serial.printf("LiveDose: pump %d (%s) pin %d, %.2f ml @ %.2f ml/min => %.2f sec\n",
                pump, pumpName.c_str(), pin, ml, flow, durationSec);

  doseAndLog(pump, pumpName, pin, ml, flow, "live");

  // Clear trigger + record lastRun + echo values
  StaticJsonDocument<128> out;
  out["trigger"] = false;
  out["lastRun"] = (unsigned long long)getEpochMillis();
  out["pump"] = pump;
  out["ml"] = ml;
  String clearJson;
  serializeJson(out, clearJson);
  firebasePutJson(path, clearJson);

  return true;
}

void firebaseSyncTankSize() {
  String path = "/devices/" + String(DEVICE_ID) + "/settings/tankSize";
  String val = firebaseGetJson(path);

  if (val != "" && val != "null") {
    float gallons = val.toFloat();
    if (gallons > 0) {
      float newLiters = gallons * 3.78541f;
      
      if (abs(newLiters - TANK_VOLUME_L) > 0.1f) {
        Serial.print("TANK UPDATE DETECTED! New Gallons: ");
        Serial.println(gallons);

        TANK_VOLUME_L = newLiters;

        // Baseline is 300g (1135.6L)
        float scaleFactor = 1135.6f / TANK_VOLUME_L;

        // 1. Kalk Scaling
        DKH_PER_ML_KALK_TANK    = 0.00010f * scaleFactor; 
        CA_PPM_PER_ML_KALK_TANK = 0.00070f * scaleFactor;

        // 2. AFR Scaling (Scaling all 3 impacts)
        DKH_PER_ML_AFR_TANK     = 0.0052f  * scaleFactor; // Fixed typo (AF -> AFR)
        CA_PPM_PER_ML_AFR_TANK  = 0.037f   * scaleFactor; // Added
        MG_PPM_PER_ML_AFR_TANK  = 0.006f   * scaleFactor; // Added

        // 3. Magnesium Pump Scaling
        MG_PPM_PER_ML_MG_TANK   = 0.20f    * scaleFactor; // Added

        updatePumpSchedules(); 
      }
    }
  }
}
// Pull /settings/doseSchedule.json and apply changes.
// Expected shape:
// { "enabled": true, "startHour": 0, "endHour": 9, "everyMin": 15, "updatedAt": 1234567890 }
void firebaseSyncDoseScheduleOnce() {
  String path = "/devices/" + String(DEVICE_ID) + "/settings/doseSchedule.json";
  String payload = firebaseGetJson(path);
  if (payload.length() == 0 || payload == "null") return;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) return;

  bool enabled    = doc["enabled"]   | false;
  int  startHour  = doc["startHour"] | 0;
  int  endHour    = doc["endHour"]   | 0;
  int  everyMin   = doc["everyMin"]  | 60;

  // --- THE NEW GATEKEEPER ---
  // Compare current values against the NEW values from Firebase
  if (enabled == doseScheduleCfg.enabled &&
      startHour == doseScheduleCfg.startHour &&
      endHour == doseScheduleCfg.endHour &&
      everyMin == doseScheduleCfg.everyMin) {
      // Everything is the same. Exit quietly.
      return; 
  }

  // If we got here, something actually changed!
  Serial.println(">>> New Dose Schedule detected. Updating...");

  doseScheduleCfg.enabled = enabled;
  doseScheduleCfg.startHour = clampInt(startHour, 0, 23);
  doseScheduleCfg.endHour   = clampInt(endHour,   0, 23);
  doseScheduleCfg.everyMin  = clampInt(everyMin,  1, 240);

  rebuildScheduleSlots();
  updatePumpSchedules(); 
  
  Serial.println(">>> Schedule update complete.");
}



// ===================== FIREBASE: CALIBRATE COMMAND =====================
// UI writes:
//  devices/<id>/commands/calibrate = {trigger:true, pump:1..4, durationSec:60, ts:<ms>}
// ESP32 runs that pump for durationSec seconds, then clears trigger and stores lastRun.
int pumpNumToPin(int pump) {
  switch (pump) {
    case 1: return PIN_PUMP_KALK;
    case 2: return PIN_PUMP_AFR;
    case 3: return PIN_PUMP_MG;
    case 4: return PIN_PUMP_TBD;
    default: return -1;
  }
}

bool firebaseCheckAndHandleCalibrate() {
  if (WiFi.status() != WL_CONNECTED) return false;

  String path = "/devices/" + String(DEVICE_ID) + "/commands/calibrate";
  String payload = firebaseGetJson(path);
  if (payload.length() == 0 || payload == "null") return false;

  Serial.print("calibrate payload: ");
  Serial.println(payload);

  // trigger
  int trigIdx = payload.indexOf("\"trigger\"");
  if (trigIdx < 0) return false;
  int colonIdx = payload.indexOf(':', trigIdx);
  if (colonIdx < 0) return false;
  String valStr = payload.substring(colonIdx + 1);
  valStr.trim();
  bool trigger = valStr.startsWith("true");

  if (!trigger) return false;

  // pump number (default 1)
  int pump = 1;
  int pIdx = payload.indexOf("\"pump\"");
  if (pIdx >= 0) {
    int pColon = payload.indexOf(':', pIdx);
    if (pColon >= 0) {
      String pStr = payload.substring(pColon + 1);
      pStr.trim();
      pump = pStr.toInt();
    }
  }

  // duration seconds (default 60)
  int durationSec = 60;
  int dIdx = payload.indexOf("\"durationSec\"");
  if (dIdx >= 0) {
    int dColon = payload.indexOf(':', dIdx);
    if (dColon >= 0) {
      String dStr = payload.substring(dColon + 1);
      dStr.trim();
      durationSec = dStr.toInt();
      if (durationSec <= 0) durationSec = 60;
      if (durationSec > 300) durationSec = 300; // safety cap
    }
  }

  int pin = pumpNumToPin(pump);
  if (pin < 0) {
    Serial.println("Calibrate: invalid pump number");
  } else {
    Serial.printf("Calibrate: running pump %d on pin %d for %d sec...\n", pump, pin, durationSec);
    giveDose(pin, (float)durationSec);
Serial.println("Calibrate: done.");
  }

  // Clear trigger and write lastRun
  time_t nowSec = time(NULL);
  uint64_t tsMs = (nowSec > 0) ? (uint64_t)nowSec * 1000ULL : (uint64_t)millis();

  String clearJson = "{";
  clearJson += "\"trigger\":false,";
  clearJson += "\"lastRun\":" + String((unsigned long long)tsMs) + ",";
  clearJson += "\"pump\":" + String(pump) + ",";
  clearJson += "\"durationSec\":" + String(durationSec);
  clearJson += "}";

  firebasePutJson(path, clearJson);

  return true;
}

// ===================== FIREBASE: READ CALIBRATION VALUES =====================
// UI saves to:
//  devices/<id>/calibration/pumps/pumpN = {ml_per_min:<float>, ts:<ms>}
// ESP32 pulls these periodically and updates FLOW_* variables + persists them.
bool firebaseSyncFlowCalibrationOnce() {
  if (WiFi.status() != WL_CONNECTED) return false;

  String path = "/devices/" + String(DEVICE_ID) + "/calibration/pumps";
  String payload = firebaseGetJson(path);
  if (payload.length() == 0 || payload == "null") return false;

  // Very small JSON pull parser (no ArduinoJson dependency).
  auto extractFloat = [&](const String& key, float currentVal) -> float {
    int k = payload.indexOf(key);
    if (k < 0) return currentVal;
    int c = payload.indexOf(':', k);
    if (c < 0) return currentVal;
    String s = payload.substring(c + 1);
    s.trim();
    // stop at comma/brace
    int end = s.indexOf(',');
    if (end < 0) end = s.indexOf('}');
    if (end > 0) s = s.substring(0, end);
    s.trim();
    float v = s.toFloat();
    if (v <= 0.0f) return currentVal;
    return v;
  };

  float newK = extractFloat("\"ml_per_min\"", FLOW_KALK_ML_PER_MIN); // pump1 likely first hit
  // Because keys repeat per pump, we parse per pump blocks:
  auto parsePump = [&](int pump, float currentVal) -> float {
    String pumpKey = "\"pump" + String(pump) + "\"";
    int p = payload.indexOf(pumpKey);
    if (p < 0) return currentVal;
    int mp = payload.indexOf("\"ml_per_min\"", p);
    if (mp < 0) return currentVal;
    int c = payload.indexOf(':', mp);
    if (c < 0) return currentVal;
    String s = payload.substring(c + 1);
    s.trim();
    int end = s.indexOf(',');
    if (end < 0) end = s.indexOf('}');
    if (end > 0) s = s.substring(0, end);
    s.trim();
    float v = s.toFloat();
    if (v <= 0.0f) return currentVal;
    return v;
  };

  float fk = parsePump(1, FLOW_KALK_ML_PER_MIN);
  float fa = parsePump(2, FLOW_AFR_ML_PER_MIN);
  float fm = parsePump(3, FLOW_MG_ML_PER_MIN);
  float fx = parsePump(4, FLOW_TBD_ML_PER_MIN);

  bool changed = (fk != FLOW_KALK_ML_PER_MIN) || (fa != FLOW_AFR_ML_PER_MIN) || (fm != FLOW_MG_ML_PER_MIN) || (fx != FLOW_TBD_ML_PER_MIN);

  FLOW_KALK_ML_PER_MIN = fk;
  FLOW_AFR_ML_PER_MIN  = fa;
  FLOW_MG_ML_PER_MIN   = fm;
  FLOW_TBD_ML_PER_MIN  = fx;
  FLOW_AUX_ML_PER_MIN  = fx; // keep in sync (legacy)

  if (changed) {
    Serial.print("Flow updated from RTDB: KALK=");
    Serial.print(FLOW_KALK_ML_PER_MIN);
    Serial.print(" AFR=");
    Serial.print(FLOW_AFR_ML_PER_MIN);
    Serial.print(" MG=");
    Serial.print(FLOW_MG_ML_PER_MIN);
    Serial.print(" AUX=");
    Serial.println(FLOW_AUX_ML_PER_MIN);
    saveFlowToPrefs();
    firebaseSetCalibrationStatus();
  }

  return changed;
}


// ===================== FIREBASE: CALIBRATION STATUS (ACK) =====================
// Writes /devices/<id>/calibration/status so the UI can show "ESP applied" acknowledgement.
void firebaseSetCalibrationStatus() {
  String path = "/devices/" + String(DEVICE_ID) + "/calibration/status";

  // Use epoch ms
  uint64_t tsMs = getEpochMillis();
  String json = "{";
  json += "\"appliedAt\":" + String((unsigned long long)tsMs) + ",";
  json += "\"flows\":{";
  json += "\"kalk\":" + String(FLOW_KALK_ML_PER_MIN, 2) + ",";
  json += "\"afr\":" + String(FLOW_AFR_ML_PER_MIN, 2) + ",";
  json += "\"mg\":" + String(FLOW_MG_ML_PER_MIN, 2) + ",";
  json += "\"aux\":" + String(FLOW_AUX_ML_PER_MIN, 2);
  json += "}";
  json += "}";
  firebasePutJson(path, json);
}

// ===================== FIREBASE: OTA STATUS & REQUEST =====================

void firebaseSetOtaStatus(const String& status, const String& error) {
  String path = "/devices/" + String(DEVICE_ID) + "/otaStatus";

  time_t nowSec = time(NULL);
  uint64_t tsMs = (nowSec > 0) ? (uint64_t)nowSec * 1000ULL : (uint64_t)millis();

  String json = "{";
  json += "\"status\":\"" + status + "\"";
  if (error.length() > 0) {
    json += ",\"error\":\"" + error + "\"";
  }
  json += ",\"updatedAt\":" + String((unsigned long long)tsMs);
  json += "}";

  firebasePutJson(path, json);
}

// Perform OTA from a HTTPS URL
bool performOtaFromUrl(const String& url) {
  firebaseSetOtaStatus("starting", "");

  if (WiFi.status() != WL_CONNECTED) {
    firebaseSetOtaStatus("error", "WiFi not connected");
    return false;
  }

  WiFiClientSecure otaClient;
  otaClient.setInsecure();

  HTTPClient https;
  Serial.print("Starting OTA from URL: ");
  Serial.println(url);

  if (!https.begin(otaClient, url)) {
    Serial.println("OTA: https.begin failed");
    firebaseSetOtaStatus("error", "https.begin failed");
    return false;
  }

  int httpCode = https.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.print("OTA: HTTP GET failed, code=");
    Serial.println(httpCode);
    firebaseSetOtaStatus("error", "HTTP code " + String(httpCode));
    https.end();
    return false;
  }

  int contentLength = https.getSize();
  if (contentLength <= 0) {
    Serial.println("OTA: Content-Length not set");
    firebaseSetOtaStatus("error", "Content length not set");
    https.end();
    return false;
  }

  firebaseSetOtaStatus("downloading", "");

  bool canBegin = Update.begin(contentLength);
  if (!canBegin) {
    Serial.println("OTA: Not enough space for update");
    firebaseSetOtaStatus("error", "Not enough space");
    https.end();
    return false;
  }

  WiFiClient * stream = https.getStreamPtr();
  size_t written = Update.writeStream(*stream);
  if (written != (size_t)contentLength) {
    Serial.print("OTA: Written only ");
    Serial.print(written);
    Serial.print(" / ");
    Serial.println(contentLength);
    firebaseSetOtaStatus("error", "WriteStream mismatch");
    https.end();
    return false;
  }

  if (!Update.end()) {
    Serial.print("OTA: Update.end() error: ");
    Serial.println(Update.getError());
    firebaseSetOtaStatus("error", "Update.end failed");
    https.end();
    return false;
  }

  https.end();

  if (!Update.isFinished()) {
    Serial.println("OTA: Update not finished");
    firebaseSetOtaStatus("error", "Update not finished");
    return false;
  }

  Serial.println("OTA: Update successful, rebooting...");
  firebaseSetOtaStatus("success", "");

  // Clear otaRequest so we don't try again after reboot
  firebasePutJson("/devices/" + String(DEVICE_ID) + "/otaRequest", "null");

  delay(1000);
  ESP.restart();
  return true; // never reached
}

// Check /devices/{DEVICE_ID}/commands/otaRequest for an update trigger
void firebaseCheckAndHandleOtaRequest() {
  if (WiFi.status() != WL_CONNECTED) return;

  // This ensures we are looking at the "reefDoser6" folder in the database
  String path = "/devices/" + String(DEVICE_ID) + "/commands/otaRequest";
  
  String payload = firebaseGetJson(path);
  
  // If nothing is there, or it's "null", just exit
  if (payload.length() == 0 || payload == "null") return;

  Serial.println("OTA Trigger command detected!");

  // HARD-FIX: We ignore the URL inside the payload. 
  // We force it to use reefDoser6 based on the DEVICE_ID at the top of this file.
  String myCorrectUrl = "https://aidoser.web.app/devices/" + String(DEVICE_ID) + "/firmware.bin";

  Serial.print("Forcing update from: ");
  Serial.println(myCorrectUrl);

  // 1. CLEANUP: Clear the request in Firebase so it doesn't reboot into an infinite update loop
  firebasePutJson(path, "null");

  // 2. EXECUTE: Use the URL we just built
  performOtaFromUrl(myCorrectUrl); 
}

void checkEmergencyStop() {
    // We check a specific "killSwitch" path in your RTDB
    String stopStatus = firebaseGetJson("/devices/" + String(DEVICE_ID) + "/settings/killSwitch");
    
    if (stopStatus == "true") {
        if (!globalEmergencyStop) {
            Serial.println("!!! EMERGENCY STOP ACTIVATED VIA FIREBASE !!!");
            // Physical safety: Force all pins LOW immediately
            digitalWrite(PIN_PUMP_KALK, LOW);
            digitalWrite(PIN_PUMP_AFR,  LOW);
            digitalWrite(PIN_PUMP_MG,   LOW);
            digitalWrite(PIN_PUMP_TBD,  LOW);
            
            firebasePushNotification("CRITICAL", "E-STOP ACTIVE", "All dosing pumps have been hard-disabled.");
        }
        globalEmergencyStop = true;
    } else {
        globalEmergencyStop = false;
    }
}

// ===================== FIREBASE: STATE HEARTBEAT =====================

void firebaseSendStateHeartbeat() {
  if (WiFi.status() != WL_CONNECTED) return;

  time_t nowSec = time(NULL);
  uint64_t tsMs = (nowSec > 0) ? (uint64_t)nowSec * 1000ULL : (uint64_t)millis();

  // Pull pending buckets (so UI can explain catch-up)
  float pk = 0, pa = 0, pm = 0, pt = 0;
  {
    Preferences prefs;
    if (prefs.begin("doser-buckets", true)) {
      pk = prefs.getFloat("p_kalk", 0.0f);
      pa = prefs.getFloat("p_afr",  0.0f);
      pm = prefs.getFloat("p_mg",   0.0f);
      pt = prefs.getFloat("p_tbd",  0.0f);
      prefs.end();
    }
  }

  int activeSlots = (doseScheduleCfg.enabled) ? DOSE_SLOTS_PER_DAY : 3;
  if (activeSlots < 1) activeSlots = 1;

  String path = "/devices/" + String(DEVICE_ID) + "/state";

  String json = "{";
  json += "\"online\":true,";
  json += "\"fwVersion\":\"" + String(FW_VERSION) + "\",";
  json += "\"lastSeen\":" + String((unsigned long long)tsMs) + ",";

  json += "\"dosingMlPerDay\":{";
  json += "\"kalk\":" + String(dosing.ml_per_day_kalk, 2) + ",";
  json += "\"afr\":"  + String(dosing.ml_per_day_afr,  2) + ",";
  json += "\"mg\":"   + String(dosing.ml_per_day_mg,   2) + ",";
  json += "\"tbd\":"  + String(dosing.ml_per_day_tbd,  2);
  json += "},";

  json += "\"doseSlotsPerDay\":" + String(activeSlots) + ",";

  json += "\"pendingMl\":{";
  json += "\"kalk\":" + String(pk, 2) + ",";
  json += "\"afr\":"  + String(pa, 2) + ",";
  json += "\"mg\":"   + String(pm, 2) + ",";
  json += "\"tbd\":"  + String(pt, 2);
  json += "},";

  json += "\"flowMlPerMin\":{";
  json += "\"kalk\":" + String(FLOW_KALK_ML_PER_MIN, 2) + ",";
  json += "\"afr\":"  + String(FLOW_AFR_ML_PER_MIN,  2) + ",";
  json += "\"mg\":"   + String(FLOW_MG_ML_PER_MIN,   2) + ",";
  json += "\"tbd\":"  + String(FLOW_TBD_ML_PER_MIN,  2);
  json += "}";

  json += "}";

  firebasePutJson(path, json);
}


// Remove the fixed values and use these formulas instead
void updateChemistryConstants() {
    if (TANK_VOLUME_L <= 0) return;

    // Saturated Kalk: ~1.4 dKH per Liter. 
    // Formulas: (Source Strength / Tank Volume)
    DKH_PER_ML_KALK_TANK    = 1.4f / TANK_VOLUME_L; 
    CA_PPM_PER_ML_KALK_TANK = 10.0f / TANK_VOLUME_L; // ~10ppm Ca per Liter

    // All-For-Reef: 160 dKH per Liter (approx)
    DKH_PER_ML_AFR_TANK     = 160.0f / TANK_VOLUME_L;
    CA_PPM_PER_ML_AFR_TANK  = 1140.0f / TANK_VOLUME_L;
    MG_PPM_PER_ML_AFR_TANK  = 180.0f / TANK_VOLUME_L;

    Serial.printf("AI Math Updated: 1ml Kalk = %.6f dKH | 1ml AFR = %.6f dKH\n", 
                  DKH_PER_ML_KALK_TANK, DKH_PER_ML_AFR_TANK);
}

// ===================== HTTP HANDLERS (local debug/legacy) =====================

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

 onNewTestInput(ca, alk, mg, ph, 0.0f);

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




void updateChemistryMath() {
    // If volume is 0, default to 300g (1135.6L) to prevent math errors
    if (TANK_VOLUME_L <= 0) TANK_VOLUME_L = 1135.6f;

    // This creates a ratio: (Original 300g size / New size)
    // If the tank gets smaller, this number gets larger, making the chemicals more "potent"
    float scaleFactor = 1135.6f / TANK_VOLUME_L;

    // Update your global chemistry variables based on the new size
    DKH_PER_ML_KALK_TANK    = 0.0103f * scaleFactor; 
    CA_PPM_PER_ML_KALK_TANK = 0.0720f * scaleFactor;
    DKH_PER_ML_AFR_TANK     = 0.0052f * scaleFactor;

    Serial.printf("Tank updated to %.1fL. New Kalk impact: %.6f dKH/ml\n", TANK_VOLUME_L, DKH_PER_ML_KALK_TANK);
}


// ===================== SETUP & LOOP =====================

void setup(){
  Serial.begin(115200);
  delay(1000);

  pinMode(PIN_PUMP_KALK, OUTPUT);
  pinMode(PIN_PUMP_AFR,  OUTPUT);
  pinMode(PIN_PUMP_MG,   OUTPUT);
  pinMode(PIN_PUMP_TBD,  OUTPUT);
  digitalWrite(PIN_PUMP_KALK, LOW);
  digitalWrite(PIN_PUMP_AFR,  LOW);
  digitalWrite(PIN_PUMP_MG,   LOW);
  digitalWrite(PIN_PUMP_TBD,  LOW);

  // Load last saved AI dosing plan from NVS (if any)
  loadDosingFromPrefs();
  loadFlowFromPrefs();
  // Sanity-check stored flow rates (bad values can cause hour-long pump runs)
validateFlow("KALK", FLOW_KALK_ML_PER_MIN, 675.0f);
validateFlow("AFR",  FLOW_AFR_ML_PER_MIN,  645.0f);
validateFlow("MG",   FLOW_MG_ML_PER_MIN,    50.0f);
validateFlow("TBD",  FLOW_TBD_ML_PER_MIN,   50.0f);
  updatePumpSchedules();
  doseSlotsPrimed = false; // re-prime after time sync

  lastSafetyBackoffTs = nowSeconds();

  // Connect to home Wi-Fi
  //WiFi.mode(WIFI_STA);
  //WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  //wm.resetSettings();
  Serial.print("Connecting to WiFi");
      if(!wm.autoConnect("ESP32_Config")) {
        Serial.println("Failed to connect, waiting for user config...");
    } else {
        Serial.println("Connected to WiFi!");
    }
  /*while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }*/
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
    // Option A: do NOT catch up on missed slots after a reboot
    primeDoseSlotsForToday();
  }

  // Web server routes
  server.on("/", handleRoot);
  server.on("/submit_test", handleSubmitTest);
  server.on("/api/history", handleApiHistory);
  server.begin();
  Serial.println("HTTP server started");

  // Firebase: send ONE push at boot (Cloud Function will deliver to iPhone)
  firebasePushNotificationThrottled("boot_push", 30ULL*60ULL*1000ULL,
    "critical",
    "ReefDoser Online",
    String(DEVICE_ID) + " booted. IP " + WiFi.localIP().toString());

}

void loop(){
  server.handleClient();

  // Push notification only when device goes OFFLINE (WiFi down for >2 minutes).
  // This avoids spam and relies on your Cloud Function to deliver iPhone push.
  static unsigned long wifiDownSinceMs = 0;
  static bool offlineNotified = false;
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiDownSinceMs == 0) wifiDownSinceMs = millis();
    if (!offlineNotified && (millis() - wifiDownSinceMs) > 120000UL) {
      // Throttle: at most once per 30 minutes
      firebasePushNotificationThrottled("offline_push", 30ULL*60ULL*1000ULL,
        "critical",
        "ReefDoser Offline",
        String(DEVICE_ID) + " lost WiFi. Last IP " + WiFi.localIP().toString());
      offlineNotified = true;
    }
  } else {
    wifiDownSinceMs = 0;
    offlineNotified = false; // allow a future offline push if it drops again
  }


  safetyBackoffIfNoTests();

  // Dose at 3 scheduled time slots per day
  maybeDosePumpsRealTime();

  unsigned long nowMs = millis();

  // Periodically poll Firebase commands (resetAi, liveDose, otaRequest)
  static unsigned long lastFirebasePollMs = 0;
  if (nowMs - lastFirebasePollMs >= 10000UL) { // every ~10s
    lastFirebasePollMs = nowMs;
    firebaseCheckAndHandleResetAi();
    firebaseCheckAndHandleLiveDose();
    firebaseCheckAndHandleOtaRequest();
    firebaseCheckAndHandleCalibrate();
    firebaseSendStateHeartbeat();
    firebaseSyncDoseScheduleOnce();
    firebaseSyncTankSize();
    checkEmergencyStop();

    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
        Serial.println(&timeinfo, "--- CLOCK CHECK: %A, %B %d %Y %I:%M:%S %p ---");
    } else {
        Serial.println("--- CLOCK CHECK: Time NOT SET (Still 1970) ---");
    }

  }
  

  // Periodically pull calibration values (ml/min) from RTDB (so ESP32 uses what you saved in UI)
  static unsigned long lastFlowSyncMs = 0;
  if (nowMs - lastFlowSyncMs >= 30000UL) { // every 30s
    lastFlowSyncMs = nowMs;
    firebaseSyncFlowCalibrationOnce();
  }


  // Send state heartbeat every 30s
  static unsigned long lastStateHeartbeatMs = 0;
  if (nowMs - lastStateHeartbeatMs >= 30000UL) {
    lastStateHeartbeatMs = nowMs;
    firebaseSendStateHeartbeat();
  }
}