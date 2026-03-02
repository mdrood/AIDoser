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
#include <nvs_flash.h>

#include "apexapi.h"
#include <esp_wifi.h>

// Forward declarations used by helpers
uint64_t getEpochMillis();

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

WiFiManager wm;
String ntfiUrl;

WebServer server(80);

char DEVICE_ID[] = "reefDoser5";
char TOPIC_SUFFIX[] = "jeffrood";
char NTFY_TOPIC[64];
const char* FW_VERSION      = "2.0.1";
bool   apexEnabled = true;

const char* NTP_SERVER     = "pool.ntp.org";
const long  GMT_OFFSET_SEC = -6 * 3600;
const int   DST_OFFSET_SEC = 3600;
bool globalEmergencyStop = false;

// ===================== FIREBASE (REST API) =====================

const char* FIREBASE_DB_URL = "https://aidoser-default-rtdb.firebaseio.com";

float pendingKalkMl = 0.0f;
float pendingAfrMl  = 0.0f;
float pendingMgMl   = 0.0f;
float pendingTbdMl  = 0.0f;

const float MIN_DOSE_SEC = 1.0f;

WiFiClientSecure secureClient;
uint64_t lastRemoteTestTimestampMs = 0;
Preferences dosingPrefs;

static const char* APEX_PREF_NS = "apex";
String apexIp = "";

void firebaseSetCalibrationStatus();
void syncTimeFromFirebaseHeader();
void pollApexAndPublish(bool allowAiUpdate);
void onNewTestInput(float ca, float alk, float mg, float ph, float tbd_val);

ApexApi *apexApi;

static const uint32_t APEX_AI_MIN_MS   = 12UL * 60UL * 60UL * 1000UL;
static const uint32_t APEX_POLL_MIN_MS = 5UL  * 60UL * 1000UL;

uint64_t lastApexAiApplyMs  = 0;
uint64_t lastApexPollMs     = 0;

float lastApexAiCa  = NAN;
float lastApexAiAlk = NAN;
float lastApexAiMg  = NAN;
float lastApexAiPh  = NAN;

static const char* NTFY_HOST  = "ntfy.sh";
static const int   NTFY_PORT  = 443;
static const char* NTFY_TOKEN = "";

static uint32_t gLastWifiOkMs = 0;
static uint32_t gNextWifiAttemptMs = 0;
static uint8_t  gWifiFailCount = 0;

void wifiTuningInit() {
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);

  gLastWifiOkMs = millis();
  gNextWifiAttemptMs = 0;
  gWifiFailCount = 0;
}

static void wifiHardResetAndReconnect() {
  Serial.println("WIFI: hard reset + reconnect");

  WiFi.disconnect(true, true);
  delay(200);

  esp_wifi_stop();
  delay(200);
  esp_wifi_start();
  delay(200);

  WiFi.reconnect();

  gWifiFailCount++;
}

void wifiKeepAliveTick() {
  const uint32_t now = millis();

  if (WiFi.status() == WL_CONNECTED) {
    gLastWifiOkMs = now;
    gWifiFailCount = 0;
    return;
  }

  if (now < gNextWifiAttemptMs) return;

  uint32_t backoff = 2000;
  if (gWifiFailCount == 1) backoff = 5000;
  else if (gWifiFailCount == 2) backoff = 10000;
  else if (gWifiFailCount == 3) backoff = 20000;
  else backoff = 30000;

  gNextWifiAttemptMs = now + backoff;

  if (now - gLastWifiOkMs > 30000) {
    wifiHardResetAndReconnect();
  } else {
    Serial.println("WIFI: reconnect()");
    WiFi.reconnect();
    gWifiFailCount++;
  }

  Serial.print("WIFI: status=");
  Serial.print((int)WiFi.status());
  Serial.print(" failCount=");
  Serial.print(gWifiFailCount);
  Serial.println();
}

static void WiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.printf("WIFI EVENT: disconnected, reason=%d\n", info.wifi_sta_disconnected.reason);
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.print("WIFI EVENT: got ip: ");
      Serial.println(WiFi.localIP());
      break;
    default:
      break;
  }
}

// Build full Firebase URL from a path
String firebaseUrl(const String& path) {
  String base = path;
  String query = "";

  int q = base.indexOf('?');
  if (q >= 0) {
    query = base.substring(q);     // includes '?'
    base  = base.substring(0, q);  // path without query
  }

  String url = String(FIREBASE_DB_URL);
  if (!url.endsWith("/")) url += "/";

  if (base.startsWith("/")) url += base.substring(1);
  else url += base;

  if (!url.endsWith(".json")) url += ".json";

  url += query;
  return url;
}

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
  https.setTimeout(30000);
  https.setReuse(false);

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

const float TARGET_ALK = 8.5f;
const float TARGET_CA  = 450.0f;
const float TARGET_MG  = 1440.0f;
const float TARGET_PH  = 8.3f;

float TANK_VOLUME_L = 1135.6f; // Default for 300 gallons

// ===================== PUMP PINS & FLOW RATES =====================

const int PIN_PUMP_KALK = 25;
const int PIN_PUMP_AFR  = 26;
const int PIN_PUMP_MG   = 27;
const int PIN_PUMP_TBD  = 22;

float FLOW_KALK_ML_PER_MIN = 675.0f;
float FLOW_AFR_ML_PER_MIN  = 645.0f;
float FLOW_MG_ML_PER_MIN   = 50.0f;
float FLOW_TBD_ML_PER_MIN  = 50.0f;

// ===================== CHEMISTRY CONSTANTS =====================
//
// MIN-RISK 4-PART PLAN (keep RTDB keys unchanged):
//  - kalk = Kalkwasser (alk + some Ca)
//  - afr  = CaCl2 solution (Calcium Chloride, anhydrous)  [Ca only]
//  - mg   = NaOH solution (Sodium Hydroxide)               [Alk only]
//  - tbd  = Magnesium solution                              [Mg only]
//

// ---------- KALKWASSER (saturated) ----------
float DKH_PER_ML_KALK_TANK    = 0.00010f;
float CA_PPM_PER_ML_KALK_TANK = 0.00070f;

// --- Legacy AFR constants kept for compatibility/history (not used by AI/safety in 4-part mode) ---
float DKH_PER_ML_AFR_TANK     = 0.0052f;
float CA_PPM_PER_ML_AFR_TANK  = 0.037f;
float MG_PPM_PER_ML_AFR_TANK  = 0.006f;

// ---------- NEW 4-PART CHEM CONSTANTS (used) ----------
float CA_PPM_PER_ML_CACL2_TANK = 0.037f;
float DKH_PER_ML_NAOH_TANK     = 0.0052f;
float MG_PPM_PER_ML_MAG_TANK   = 0.20f;

// AUX legacy placeholder
float TBD_PPM_PER_ML_TANK     = 0.00f;

// ===================== DOSING CONFIG =====================

struct TestPoint {
  uint32_t t;
  float ca;
  float alk;
  float mg;
  float ph;
  float tbd;
};

struct DosingConfig {
  float ml_per_day_kalk;
  float ml_per_day_afr; // CaCl2
  float ml_per_day_mg;  // NaOH
  float ml_per_day_tbd; // Magnesium
};

DosingConfig dosing = { 2000.0f, 20.0f, 0.0f, 0.0f };

float MAX_KALK_ML_PER_DAY = 2500.0f;
float MAX_AFR_ML_PER_DAY  = 500.0f;
float MAX_MG_ML_PER_DAY   = 500.0f;
float MAX_TBD_ML_PER_DAY  = 500.0f;

uint32_t lastSafetyBackoffTs = 0;

uint32_t nowSeconds() { return millis()/1000; }

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

// ===================== SAFETY: CHEMISTRY-BASED CAPS =====================
void enforceChemSafetyCaps() {
  float alkRise =
      dosing.ml_per_day_kalk * DKH_PER_ML_KALK_TANK +
      dosing.ml_per_day_mg   * DKH_PER_ML_NAOH_TANK;

  float caRise =
      dosing.ml_per_day_kalk * CA_PPM_PER_ML_KALK_TANK +
      dosing.ml_per_day_afr  * CA_PPM_PER_ML_CACL2_TANK;

  float mgRise =
      dosing.ml_per_day_tbd  * MG_PPM_PER_ML_MAG_TANK;

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
    dosing.ml_per_day_tbd  *= scale;

    Serial.print("SAFETY: Scaling dosing by ");
    Serial.println(scale, 3);
  }
}

// ===================== “AI” CONTROL (4-PART, MIN-RISK KEYS) =====================

TestPoint lastTest    = {0, 0, 0, 0, 0, 0};
TestPoint currentTest = {0, 0, 0, 0, 0, 0};

void updatePumpSchedules() { /* stub */ }
void saveDosingToPrefs()   { /* stub */ }

void onNewTestInput(float ca, float alk, float mg, float ph, float tbd_val) {
  lastTest = currentTest;
  currentTest.t   = nowSeconds();
  currentTest.ca  = ca;
  currentTest.alk = alk;
  currentTest.mg  = mg;
  currentTest.ph  = ph;
  currentTest.tbd = tbd_val;

  if (ca   < 300.0f || ca   > 550.0f ||
      alk  <   5.0f || alk  > 14.0f  ||
      mg   < 1100.0f || mg  > 1600.0f ||
      ph   <   7.0f || ph   >   9.0f) {
    Serial.println("SAFETY: IGNORING TEST for dosing (out-of-range).");
    return;
  }

  if(lastTest.t == 0){
    updatePumpSchedules();
    return;
  }

  float days = float(currentTest.t - lastTest.t) / 86400.0f;
  if(days <= 0.25f) {
    Serial.println("SAFETY: Tests too close together, ignoring for dosing updates.");
    return;
  }

  float consAlk = (lastTest.alk - currentTest.alk) / days;  if (consAlk < 0) consAlk = 0;
  float consCa  = (lastTest.ca  - currentTest.ca ) / days;  if (consCa  < 0) consCa  = 0;
  float consMg  = (lastTest.mg  - currentTest.mg ) / days;  if (consMg  < 0) consMg  = 0;

  float kalkFrac = 0.80f;
  if (isfinite(currentTest.ph)) {
    float phError = currentTest.ph - TARGET_PH;
    if (phError < -0.05f)      kalkFrac = 0.90f;
    else if (phError > 0.05f)  kalkFrac = 0.65f;
  }
  kalkFrac = clampf(kalkFrac, 0.50f, 0.95f);

  float alkFromKalk = consAlk * kalkFrac;
  float alkFromNaoh = consAlk * (1.0f - kalkFrac);

  float suggested_ml_kalk = (DKH_PER_ML_KALK_TANK > 0.0f) ? (alkFromKalk / DKH_PER_ML_KALK_TANK) : 0.0f;
  float suggested_ml_naoh = (DKH_PER_ML_NAOH_TANK > 0.0f) ? (alkFromNaoh / DKH_PER_ML_NAOH_TANK) : 0.0f;

  float caFromKalk = suggested_ml_kalk * CA_PPM_PER_ML_KALK_TANK;
  float caDeficit  = consCa - caFromKalk;
  if (caDeficit < 0.0f) caDeficit = 0.0f;

  float suggested_ml_cacl2 = (CA_PPM_PER_ML_CACL2_TANK > 0.0f) ? (caDeficit / CA_PPM_PER_ML_CACL2_TANK) : 0.0f;
  float suggested_ml_mag   = (MG_PPM_PER_ML_MAG_TANK > 0.0f)   ? (consMg / MG_PPM_PER_ML_MAG_TANK)       : 0.0f;

  float suggested_ml_afr = suggested_ml_cacl2; // pump2 afr -> CaCl2
  float suggested_ml_mg  = suggested_ml_naoh;  // pump3 mg  -> NaOH
  float suggested_ml_tbd = suggested_ml_mag;   // pump4 tbd -> Magnesium

  dosing.ml_per_day_kalk = adjustWithLimit(dosing.ml_per_day_kalk, max(0.0f, suggested_ml_kalk));
  dosing.ml_per_day_afr  = adjustWithLimit(dosing.ml_per_day_afr,  max(0.0f, suggested_ml_afr));
  dosing.ml_per_day_mg   = adjustWithLimit(dosing.ml_per_day_mg,   max(0.0f, suggested_ml_mg));
  dosing.ml_per_day_tbd  = adjustWithLimit(dosing.ml_per_day_tbd,  max(0.0f, suggested_ml_tbd));

  dosing.ml_per_day_kalk = clampf(dosing.ml_per_day_kalk, 0.0f, MAX_KALK_ML_PER_DAY);
  dosing.ml_per_day_afr  = clampf(dosing.ml_per_day_afr,  0.0f, MAX_AFR_ML_PER_DAY);
  dosing.ml_per_day_mg   = clampf(dosing.ml_per_day_mg,   0.0f, MAX_MG_ML_PER_DAY);
  dosing.ml_per_day_tbd  = clampf(dosing.ml_per_day_tbd,  0.0f, MAX_TBD_ML_PER_DAY);

  enforceChemSafetyCaps();
  updatePumpSchedules();
  saveDosingToPrefs();

  Serial.println("AI Update: MIN-RISK 4-PART dosing plan recalculated (kalk / CaCl2 / NaOH / Mg).");
}

// ===================== TANK SIZE SYNC (UPDATED FOR 4-PART) =====================
void firebaseSyncTankSize() {
  String path = "/devices/" + String(DEVICE_ID) + "/settings/tankSize";
  String val = firebaseGetJson(path);

  if (val != "" && val != "null") {
    float gallons = val.toFloat();
    if (gallons > 0) {
      float newLiters = gallons * 3.78541f;

      if (fabsf(newLiters - TANK_VOLUME_L) > 0.1f) {
        Serial.print("TANK UPDATE DETECTED! New Gallons: ");
        Serial.println(gallons);

        TANK_VOLUME_L = newLiters;

        float scaleFactor = 1135.6f / TANK_VOLUME_L;

        DKH_PER_ML_KALK_TANK     = 0.00010f * scaleFactor;
        CA_PPM_PER_ML_KALK_TANK  = 0.00070f * scaleFactor;

        CA_PPM_PER_ML_CACL2_TANK = 0.037f  * scaleFactor;
        DKH_PER_ML_NAOH_TANK     = 0.0052f * scaleFactor;
        MG_PPM_PER_ML_MAG_TANK   = 0.20f   * scaleFactor;

        updatePumpSchedules();
      }
    }
  }
}

uint64_t getEpochMillis() { return (uint64_t)millis(); }

void setup() {
  Serial.begin(115200);
  delay(300);

  snprintf(NTFY_TOPIC, sizeof(NTFY_TOPIC), "aidoser-%s-%s", DEVICE_ID, TOPIC_SUFFIX);
  ntfiUrl = String("https://") + NTFY_HOST + "/" + NTFY_TOPIC;

  pinMode(PIN_PUMP_KALK, OUTPUT);
  pinMode(PIN_PUMP_AFR,  OUTPUT);
  pinMode(PIN_PUMP_MG,   OUTPUT);
  pinMode(PIN_PUMP_TBD,  OUTPUT);

  digitalWrite(PIN_PUMP_KALK, LOW);
  digitalWrite(PIN_PUMP_AFR,  LOW);
  digitalWrite(PIN_PUMP_MG,   LOW);
  digitalWrite(PIN_PUMP_TBD,  LOW);

  Serial.println("main.cpp generated: minimum-risk 4-part dosing constants + AI + safety + tank scaling applied.");
}

void loop() {
  // In your real project, keep your existing loop().
}
