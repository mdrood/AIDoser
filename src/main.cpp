// main.cpp (AIDoser - calibration + selfTest + modeSweep)
// Fix: restore RTDB commands/calibrate support (pump run from UI)
// Notes:
//  - This file is based on the code you pasted, with ONLY the missing calibration
//    command handling added back in.
//  - Calibration is handled as a non-blocking state machine (no delay()), so the loop keeps running.
//  - UI command shape expected:
//      /devices/<id>/commands/calibrate = {
//        "durationSec": 60, "pump": 1, "status": "pending", "trigger": true, "ts": 123
//      }

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

// ---------------- SELF-TEST BUILD TOGGLE ----------------
// Set to 0 for release builds if you want to completely remove the PC test hooks.
#define ENABLE_SELFTEST 1
#include <stdint.h>
#include <Preferences.h>
#include <nvs_flash.h>

#include "apexapi.h"
#include <esp_wifi.h>

// Forward declarations used by helpers

// Time validity guard: avoid dosing before NTP has set a real date.
static bool isTimeValid(const tm& t) {
  return (t.tm_year >= 123); // 123 == 2023
}

void handleRoot();
uint64_t getEpochMillis();
String firebaseGetJson(const String& path);
static uint64_t fnv1a64(const char* s);
void pollApexAndPublish(bool allowAiUpdate);
static bool isValidIPv4(const String& ip);
static bool apexValuesLookValid(float ca, float alk, float mg, float ph);
static bool apexChangedEnough(float ca, float alk, float mg, float ph) ;

WiFiManager wm;

String ntfiUrl;

WebServer server(80);

bool performOtaFromUrl(const String& url);
static void doseAndLog(int pumpIndex, const String& pumpName, int pin, float ml, float flowMlPerMin, const String& source);

ApexApi *apexApi = nullptr;

// ===================== APEX -> AI ingestion =====================
static const uint32_t APEX_AI_MIN_MS   = 12UL * 60UL * 60UL * 1000UL;
static const uint32_t APEX_POLL_MIN_MS = 5UL  * 60UL * 1000UL;

// IMPORTANT: keep both of these in millis()-timebase, not epoch time.
uint64_t lastApexAiApplyMs  = 0;
uint64_t lastApexPollMs     = 0;

float lastApexAiCa   = NAN;
float lastApexAiAlk  = NAN;
float lastApexAiMg   = NAN;
float lastApexAiPh   = NAN;
float lastApexCurCa  = NAN;
float lastApexCurAlk = NAN;
float lastApexCurMg  = NAN;
float lastApexCurPh  = NAN;

///////////////////////////////////////////////////////////////////////////////////////
// things you must change
char DEVICE_ID[] = "reefDoser6";
char TOPIC_SUFFIX[] = "mark1958";
char NTFY_TOPIC[64];
const char* FW_VERSION      = "3.0.3";
bool   apexEnabled = false;
////////////////////////////////////////////////////////////////////////////////////////

const char* NTP_SERVER     = "pool.ntp.org";
const long  GMT_OFFSET_SEC = -6 * 3600;
const int   DST_OFFSET_SEC = 3600;

bool globalEmergencyStop = false;

// ===================== FIREBASE (REST API) =====================
const char* FIREBASE_DB_URL = "https://aidoser-default-rtdb.firebaseio.com";

// Pending buckets
float pendingKalkMl = 0.0f;
float pendingAfrMl  = 0.0f;
float pendingMgMl   = 0.0f;
float pendingTbdMl  = 0.0f;

// Minimum pump runtime threshold (sec). Adjusted in selfTest/modeSweep.
float gMinDoseSec = 1.0f;

WiFiClientSecure secureClient;
uint64_t lastRemoteTestTimestampMs = 0;

Preferences dosingPrefs;

// ===================== DOSING MODE =====================
enum DosingMode : int {
  MODE_KALK_ONLY = 1,
  MODE_AFR_ONLY  = 2,
  MODE_KALK_AFR_MG = 3,
  MODE_2PART_MG = 4,
  MODE_KALK_2PART_MG = 5,
  MODE_KALK_CACL2_NAOH_MG = 6
};

static DosingMode gDosingMode = MODE_KALK_ONLY;
static inline bool emergencyStopIsActive();
static void benchRunPumpSeconds(int pin, int seconds);
static bool runPumpSeconds(int pin, float durationSec) ;

struct ModeCfg {
  const char* keyP1;
  const char* keyP2;
  const char* keyP3;
  const char* keyP4;
};

static ModeCfg getModeCfg(DosingMode mode);

// True if a given physical pump (1..4) is used for a given dosing mode.
static bool modeHasPump(DosingMode mode, int pumpIndex) {
  if (pumpIndex < 1 || pumpIndex > 4) return false;
  const ModeCfg cfg = getModeCfg(mode);
  switch (pumpIndex) {
    case 1: return cfg.keyP1 != nullptr;
    case 2: return cfg.keyP2 != nullptr;
    case 3: return cfg.keyP3 != nullptr;
    case 4: return cfg.keyP4 != nullptr;
    default: return false;
  }
}

// Convenience wrapper uses current gDosingMode
static bool modeHasPump(int pumpIndex) { return modeHasPump(gDosingMode, pumpIndex); }

// Convenience: check current mode.
static ModeCfg getModeCfg(DosingMode mode) {
  switch (mode) {
    case MODE_KALK_ONLY:
      return { "kalk", nullptr, nullptr, nullptr };
    case MODE_AFR_ONLY:
      return { "afr", nullptr, nullptr, nullptr };
    case MODE_KALK_AFR_MG:
      return { "kalk", "afr", "mg", nullptr };
    case MODE_2PART_MG:
      return { "alk", "ca", "mg", nullptr };
    case MODE_KALK_2PART_MG:
      return { "kalk", "alk", "ca", "mg" };
    case MODE_KALK_CACL2_NAOH_MG:
      return { "kalk", "cacl2", "naoh", "mg" };
    default:
      return { "kalk", nullptr, nullptr, nullptr };
  }
}

static const char* dosingModeLabel(DosingMode mode) {
  switch (mode) {
    case MODE_KALK_ONLY:            return "Mode 1 • Kalk";
    case MODE_AFR_ONLY:             return "Mode 2 • AFR (Pump 1)";
    case MODE_KALK_AFR_MG:          return "Mode 3 • Kalk + AFR + Mg";
    case MODE_2PART_MG:             return "Mode 4 • Alk + Calcium + Mg";
    case MODE_KALK_2PART_MG:        return "Mode 5 • Kalk + 2-Part (Alk/Ca) + Mg";
    case MODE_KALK_CACL2_NAOH_MG:   return "Mode 6 • Kalk + CaCl2 + NaOH + Mg";
    default:                        return "Mode 1 • Kalk";
  }
}

static const char* DOSING_MODE_PREF_NS = "dosingMode";

static DosingMode clampDosingMode(int v) {
  if (v < 1) v = 1;
  if (v > 6) v = 6;
  return (DosingMode)v;
}

static void loadDosingModeFromPrefs() {
  Preferences p;
  if (!p.begin(DOSING_MODE_PREF_NS, true)) return;
  int v = (int)p.getUChar("m", (uint8_t)MODE_KALK_ONLY);
  p.end();
  gDosingMode = clampDosingMode(v);
}

static void saveDosingModeToPrefs(DosingMode m) {
  Preferences p;
  if (!p.begin(DOSING_MODE_PREF_NS, false)) return;
  p.putUChar("m", (uint8_t)m);
  p.end();
}

// Forward decl
void clearPendingBuckets(const char* why);

// ===================== APEX config =====================
static const char* APEX_PREF_NS = "apex";
String apexIp = "";

void firebaseSetCalibrationStatus();
void syncTimeFromFirebaseHeader();
void pollApexAndPublish(bool allowAiUpdate);
bool firebasePatchJson(const String& path, const String& jsonBody);

// ===================== AI breadcrumbs =====================
static uint64_t gAiLastRunEpochMs = 0;
static int      gAiLastRunMode    = 0;
static float    gAiLastInCa  = NAN;
static float    gAiLastInAlk = NAN;
static float    gAiLastInMg  = NAN;
static float    gAiLastInPh  = NAN;
static float    gAiLastPlanP1 = NAN;
static float    gAiLastPlanP2 = NAN;
static float    gAiLastPlanP3 = NAN;
static float    gAiLastPlanP4 = NAN;
static char     gAiLastSource[16] = "unknown";
static char     gAiLastReason[48] = "n/a";
static const char* gAiCurrentSource = "tests";

struct DosingConfig {
  float ml_per_day_kalk;
  float ml_per_day_afr;
  float ml_per_day_mg;
  float ml_per_day_tbd;
};

DosingConfig dosing = {
  2000.0f,
  20.0f,
  0.0f,
  0.0f
};

bool firebasePutJson(const String& path, const String& jsonBody);

void publishAiBreadcrumbs(const char* source, const char* reason,
                          float ca, float alk, float mg, float ph) {
  gAiLastRunEpochMs = getEpochMillis();
  gAiLastRunMode    = (int)gDosingMode;
  gAiLastInCa  = ca;
  gAiLastInAlk = alk;
  gAiLastInMg  = mg;
  gAiLastInPh  = ph;
  gAiLastPlanP1 = dosing.ml_per_day_kalk;
  gAiLastPlanP2 = dosing.ml_per_day_afr;
  gAiLastPlanP3 = dosing.ml_per_day_mg;
  gAiLastPlanP4 = dosing.ml_per_day_tbd;

  if (source && source[0]) {
    strncpy(gAiLastSource, source, sizeof(gAiLastSource) - 1);
    gAiLastSource[sizeof(gAiLastSource) - 1] = 0;
  }
  if (reason && reason[0]) {
    strncpy(gAiLastReason, reason, sizeof(gAiLastReason) - 1);
    gAiLastReason[sizeof(gAiLastReason) - 1] = 0;
  }

  String path = "/devices/" + String(DEVICE_ID) + "/state";
  String json = "{";
  json += "\"aiLastRunEpochMs\":" + String((unsigned long long)gAiLastRunEpochMs) + ",";
  json += "\"aiLastRunMode\":" + String(gAiLastRunMode) + ",";
  json += "\"aiLastSource\":\"" + String(gAiLastSource) + "\",";
  json += "\"aiLastReason\":\"" + String(gAiLastReason) + "\",";
  json += "\"aiLastInputs\":{";
  json += "\"ca\":" + String(ca, 1) + ",";
  json += "\"alk\":" + String(alk, 2) + ",";
  json += "\"mg\":" + String(mg, 1) + ",";
  json += "\"ph\":" + String(ph, 2);
  json += "},";
  json += "\"aiLastPlan\":{";
  json += "\"p1\":" + String(gAiLastPlanP1, 2) + ",";
  json += "\"p2\":" + String(gAiLastPlanP2, 2) + ",";
  json += "\"p3\":" + String(gAiLastPlanP3, 2) + ",";
  json += "\"p4\":" + String(gAiLastPlanP4, 2);
  json += "}";
  json += "}";

  firebasePatchJson(path, json);
}
String firebaseUrl(const String& path);
// Simple POST JSON helper (for logs/alerts)
bool firebasePostJson(const String& path, const String& jsonBody) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Firebase POST: WiFi not connected");
    return false;
  }

  HTTPClient https;
  String url = firebaseUrl(path);

  if (!https.begin(secureClient, url)) {
    Serial.println("Firebase POST begin() failed");
    return false;
  }
  https.setTimeout(30000);
  https.setReuse(false);
  https.addHeader("Content-Type", "application/json");

  int code = https.POST(jsonBody);
  if (code != HTTP_CODE_OK && code != HTTP_CODE_NO_CONTENT) {
    Serial.printf("Firebase POST error code: %d\n", code);
    Serial.println(https.getString());
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

  const String p = "/devices/" + String(DEVICE_ID) + "/doseRuns.json";
  return firebasePostJson(p, body);
}
void onNewTestInput(float ca, float alk, float mg, float ph, float tbd_val);

// ===================== MANUAL TEST INPUT (from RTDB) =====================
// This is the "normal mode" manual input path written by your Python runner:
//   /devices/<id>/tests/latest = { ca, alk, mg, ph, ... , timestamp/ts }
// We ALWAYS cache the latest sample, but we do NOT run AI immediately here.
// AI is run by the scheduler (default 12 hours) using the newest available input.
//
// NOTE: This does NOT remove/replace Apex input. Scheduler prefers Apex if it's fresh.
static uint64_t gLastManualSampleKey = 0;
static uint64_t gLastManualSampleEpochMs = 0;
static float    gLastManualCa  = NAN;
static float    gLastManualAlk = NAN;
static float    gLastManualMg  = NAN;
static float    gLastManualPh  = NAN;
static float    gLastManualTbd = 0.0f;

// Read /tests/latest and cache newest values (non-blocking, throttled)
static void pollManualInputs() {
  static uint32_t lastPollMs = 0;
  const uint32_t now = millis();
  if (now - lastPollMs < 1500) return;
  lastPollMs = now;

  const String body = firebaseGetJson(
    "/devices/" + String(DEVICE_ID) + "/tests?orderBy=%22timestamp%22&limitToLast=1"
  );
  if (body.length() == 0 || body == "null") return;

  StaticJsonDocument<1024> doc;
  if (deserializeJson(doc, body)) return;

  JsonObject root = doc.as<JsonObject>();
  if (root.isNull()) return;

  JsonObject sample;
  for (JsonPair kv : root) {
    if (kv.value().is<JsonObject>()) {
      sample = kv.value().as<JsonObject>();
      break;
    }
  }
  if (sample.isNull()) return;

  uint64_t key = 0;
  if (sample["timestamp"].is<unsigned long long>()) key = sample["timestamp"].as<unsigned long long>();
  if (key == 0 && sample["ts"].is<unsigned long long>()) key = sample["ts"].as<unsigned long long>();
  if (key == 0) key = fnv1a64(body.c_str());
  if (key == 0 || key == gLastManualSampleKey) return;

  gLastManualSampleKey = key;
  gLastManualSampleEpochMs = key;
  gLastManualCa  = sample["ca"].is<float>()  || sample["ca"].is<int>()  ? sample["ca"].as<float>()  : NAN;
  gLastManualAlk = sample["alk"].is<float>() || sample["alk"].is<int>() ? sample["alk"].as<float>() : NAN;
  gLastManualMg  = sample["mg"].is<float>()  || sample["mg"].is<int>()  ? sample["mg"].as<float>()  : NAN;
  gLastManualPh  = sample["ph"].is<float>()  || sample["ph"].is<int>()  ? sample["ph"].as<float>()  : NAN;
  gLastManualTbd = sample["tbd"].is<float>() || sample["tbd"].is<int>() ? sample["tbd"].as<float>() : 0.0f;

  Serial.printf("MANUAL INPUT cached: ts=%llu ca=%.1f alk=%.2f mg=%.1f ph=%.2f\n",
                (unsigned long long)gLastManualSampleEpochMs,
                gLastManualCa, gLastManualAlk, gLastManualMg, gLastManualPh);
}
// ===================== AI SCHEDULER (manual OR apex every 12h) =====================
// Default: run once every 12 hours.
// For testing, you may set: /devices/<id>/settings/aiIntervalMin = 1 (minutes)
// This changes ONLY the interval gating, not the AI logic.
// If the setting is missing/null/invalid, it falls back to 12h.
static uint64_t gLastAiRunMs = 0;

static uint64_t readAiIntervalMs() {
  String body = firebaseGetJson("/devices/" + String(DEVICE_ID) + "/settings/aiIntervalMin");
  body.trim();
  if (body.length() && body != "null") {
    int minv = body.toInt();
    if (minv >= 1 && minv <= 7 * 24 * 60) {
      return (uint64_t)minv * 60ULL * 1000ULL;
    }
  }
  return 12ULL * 60ULL * 60ULL * 1000ULL;
}
bool gSelfTestActive = false;
bool gModeSweepActive = false;

static bool apexInputIsFresh() {
  const uint64_t nowMs = millis();
  if (lastApexPollMs == 0) return false;
  if (nowMs - lastApexPollMs > 10ULL * 60ULL * 1000ULL) return false;
  return (isfinite(lastApexCurCa) && isfinite(lastApexCurAlk) && isfinite(lastApexCurMg) && isfinite(lastApexCurPh));
}

static bool manualInputIsValid() {
  return (isfinite(gLastManualCa) && isfinite(gLastManualAlk) && isfinite(gLastManualMg) && isfinite(gLastManualPh));
}

// Runs AI on schedule using latest available input (Apex preferred if enabled+fresh; else manual)
static void runAiScheduler() {
  if (gSelfTestActive || gModeSweepActive) return; // don't interfere with bench modes

  const uint64_t now = millis();
  const uint64_t intervalMs = readAiIntervalMs();
  if (intervalMs == 0) return;

  if (gLastAiRunMs != 0 && (now - gLastAiRunMs) < intervalMs) return;

  // Choose inputs
  bool useApex = (apexEnabled && apexInputIsFresh());
  bool useManual = (!useApex && manualInputIsValid());

  if (!useApex && !useManual) return; // nothing to run with yet

  gLastAiRunMs = now;

  const char* prev = gAiCurrentSource;
  if (useApex) {
    gAiCurrentSource = "apex";
    Serial.println("AI SCHED: running from APEX inputs");
    onNewTestInput(lastApexCurCa, lastApexCurAlk, lastApexCurMg, lastApexCurPh, 0.0f);
    publishAiBreadcrumbs("apex", "sched", lastApexCurCa, lastApexCurAlk, lastApexCurMg, lastApexCurPh);
  } else {
    gAiCurrentSource = "manual";
Serial.println("AI SCHED: running from MANUAL inputs (/tests latest by timestamp)");
    onNewTestInput(gLastManualCa, gLastManualAlk, gLastManualMg, gLastManualPh, gLastManualTbd);
    publishAiBreadcrumbs("manual", "sched", gLastManualCa, gLastManualAlk, gLastManualMg, gLastManualPh);
  }
  gAiCurrentSource = prev;
}

// ===================== WIFI keep alive =====================
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

// ===================== Firebase URL + helpers =====================
String firebaseUrl(const String& path) {
  String base = path;
  String query = "";

  int q = base.indexOf('?');
  if (q >= 0) {
    query = base.substring(q);
    base  = base.substring(0, q);
  }

  String url = String(FIREBASE_DB_URL);
  if (!url.endsWith("/")) url += "/";

  if (base.startsWith("/")) url += base.substring(1);
  else url += base;

  if (!url.endsWith(".json")) url += ".json";

  url += query;
  return url;
}

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
  https.setTimeout(30000);
  https.setReuse(false);

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

bool firebasePatchJson(const String& path, const String& jsonBody) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Firebase PATCH: WiFi not connected");
    return false;
  }

  HTTPClient https;
  String url = firebaseUrl(path);

  if (!https.begin(secureClient, url)) {
    Serial.println("Firebase PATCH begin() failed");
    return false;
  }

  https.setTimeout(30000);
  https.setReuse(false);
  https.addHeader("Content-Type", "application/json");

  Serial.println("Firebase PATCH: " + url);
  Serial.println("Body: " + jsonBody);

  int code = https.sendRequest("PATCH", (uint8_t*)jsonBody.c_str(), jsonBody.length());
  if (code > 0) {
    String resp = https.getString();
    if (code >= 200 && code < 300) {
      https.end();
      return true;
    }
    Serial.printf("Firebase PATCH error code: %d\n", code);
    Serial.println(resp);
  } else {
    Serial.printf("Firebase PATCH failed: %s\n", https.errorToString(code).c_str());
  }
  https.end();
  return false;
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

// ===================== Targets & Tank Info =====================
const float TARGET_ALK = 8.5f;
const float TARGET_CA  = 450.0f;
const float TARGET_MG  = 1440.0f;
const float TARGET_PH  = 8.3f;
float TANK_VOLUME_L = 1135.6f; // 300 gallons default

// ===================== Pump pins & flow =====================
const int PIN_PUMP_KALK = 25;
const int PIN_PUMP_AFR  = 26;
const int PIN_PUMP_MG   = 27;
const int PIN_PUMP_TBD  = 22;

float FLOW_KALK_ML_PER_MIN = 675.0f;
float FLOW_AFR_ML_PER_MIN  = 645.0f;
float FLOW_MG_ML_PER_MIN   = 50.0f;
float FLOW_TBD_ML_PER_MIN  = 50.0f;

// ===================== Chemistry constants =====================
// Kalk
float DKH_PER_ML_KALK_TANK    = 0.00010f;
float CA_PPM_PER_ML_KALK_TANK = 0.00070f;

// AFR
float DKH_PER_ML_AFR_TANK     = 0.0052f;
float CA_PPM_PER_ML_AFR_TANK  = 0.037f;
float MG_PPM_PER_ML_AFR_TANK  = 0.006f;

// Mg only
float MG_PPM_PER_ML_MG_TANK   = 0.20f;

// Mode 2 / 3 placeholders
float DKH_PER_ML_NAOH_TANK     = 0.0f;
float CA_PPM_PER_ML_NACL_TANK  = 0.0f;
float MG_PPM_PER_ML_MODE2_MG_TANK = 0.20f;

float DKH_PER_ML_BRS_ALK_TANK   = 0.0f;
float CA_PPM_PER_ML_BRS_CA_TANK = 0.0f;
float MG_PPM_PER_ML_MODE3_MG_TANK = 0.20f;

// ===================== History =====================
struct TestPoint {
  uint32_t t;
  float ca;
  float alk;
  float mg;
  float ph;
  float tbd;
};

const int MAX_HISTORY = 64;
TestPoint historyBuf[MAX_HISTORY];
int historyCount = 0;

TestPoint lastTest    = {0, 0, 0, 0, 0, 0};
TestPoint currentTest = {0, 0, 0, 0, 0, 0};

// ===================== Caps =====================
float MAX_KALK_ML_PER_DAY = 2500.0f;
float MAX_AFR_ML_PER_DAY  = 200.0f;
float MAX_MG_ML_PER_DAY   = 40.0f;
float MAX_TBD_ML_PER_DAY  = 40.0f;

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

// Map a mode "pumpKey" (kalk/afr/mg/tbd/none) to the current per-day dosing target (ml/day).
// NOTE: These dosing.ml_per_day_* values are "per CHEM", while pending* buckets are "per PUMP".
static float mlPerDayForKey(const char* key) {
  if (!key) return 0.0f;
  if (strcmp(key, "kalk") == 0) return dosing.ml_per_day_kalk;
  if (strcmp(key, "afr")  == 0) return dosing.ml_per_day_afr;
  if (strcmp(key, "mg")   == 0) return dosing.ml_per_day_mg;
  if (strcmp(key, "tbd")  == 0) return dosing.ml_per_day_tbd;
  if (strcmp(key, "alk")  == 0) return dosing.ml_per_day_afr;
  if (strcmp(key, "ca")   == 0) return dosing.ml_per_day_mg;
  if (strcmp(key, "cacl2") == 0) return dosing.ml_per_day_afr;
  if (strcmp(key, "naoh")  == 0) return dosing.ml_per_day_mg;
  return 0.0f; // "none" or unknown
}

void pushHistory(const TestPoint& tp){
  if(historyCount < MAX_HISTORY){
    historyBuf[historyCount++] = tp;
  } else {
    for(int i=1;i<MAX_HISTORY;i++) historyBuf[i-1] = historyBuf[i];
    historyBuf[MAX_HISTORY-1] = tp;
  }
}

// ===================== Schedule config / slots =====================
static const int MAX_DOSE_SLOTS = 96;
int  DOSE_SLOTS_PER_DAY = 3;
float SEC_PER_DOSE_KALK = 0.0f;
float SEC_PER_DOSE_AFR  = 0.0f;
float SEC_PER_DOSE_MG   = 0.0f;
float SEC_PER_DOSE_TBD  = 0.0f;

struct DoseScheduleCfg {
  bool enabled   = false;
  int  startHour = 0;
  int  endHour   = 0;
  int  everyMin  = 60;
  uint64_t updatedAt = 0;
};
DoseScheduleCfg doseScheduleCfg;

int  DOSE_HOURS[MAX_DOSE_SLOTS]   = {9, 12, 15};
int  DOSE_MINUTES[MAX_DOSE_SLOTS] = {30, 30, 30};
bool slotDone[MAX_DOSE_SLOTS]     = {false};
int  lastDoseWindowDay = -1;
bool doseSlotsPrimed   = false;

static int clampInt(int v, int lo, int hi) { return (v < lo) ? lo : (v > hi) ? hi : v; }
static bool scheduleWraps(int startHour, int endHour) {
  startHour = clampInt(startHour, 0, 23);
  endHour   = clampInt(endHour,   0, 23);
  return (endHour <= startHour);
}

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
    if (nowMin < startMin || nowMin >= endMin) return -1;
    windowLen = endMin - startMin;
    offset = nowMin - startMin;
  } else {
    windowLen = (24*60 - startMin) + endMin;
    if (nowMin >= startMin) offset = nowMin - startMin;
    else if (nowMin < endMin) offset = (24*60 - startMin) + nowMin;
    else return -1;
  }

  if (windowLen <= 0) return -1;
  const int slots = windowLen / everyMin;
  if (slots <= 0) return -1;

  int idx = offset / everyMin;
  if (idx < 0) idx = 0;
  if (idx >= slots) idx = slots - 1;
  return idx;
}

static int windowStartYday(const tm& t, int startHour, int endHour) {
  if (!scheduleWraps(startHour, endHour)) return t.tm_yday;
  if (t.tm_hour < endHour) return t.tm_yday - 1;
  return t.tm_yday;
}

static void rebuildScheduleSlots() {
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
  int windowLen = (!scheduleWraps(startHour, endHour)) ? (endMin - startMin)
                                                       : ((24*60 - startMin) + endMin);
  if (windowLen <= 0) windowLen = 24*60;

  int slots = windowLen / everyMin;
  if (slots < 1) slots = 1;
  if (slots > MAX_DOSE_SLOTS) slots = MAX_DOSE_SLOTS;

  DOSE_SLOTS_PER_DAY = slots;
  for (int i = 0; i < DOSE_SLOTS_PER_DAY; i++) {
    int m = (startMin + i * everyMin) % (24*60);
    DOSE_HOURS[i] = m / 60;
    DOSE_MINUTES[i] = m % 60;
  }
}

void primeDoseSlotsForToday() {
  struct tm t;
  if (!getLocalTime(&t)) return;
  if (!isTimeValid(t)) return;

  rebuildScheduleSlots();
  lastDoseWindowDay = doseScheduleCfg.enabled ? windowStartYday(t, doseScheduleCfg.startHour, doseScheduleCfg.endHour)
                                              : t.tm_yday;

  for (int i = 0; i < DOSE_SLOTS_PER_DAY; i++) slotDone[i] = false;

  if (doseScheduleCfg.enabled) {
    const int nowIdx = scheduleSlotIndex(t, doseScheduleCfg.startHour, doseScheduleCfg.endHour, doseScheduleCfg.everyMin);
    if (nowIdx >= 0) {
      for (int i = 0; i <= nowIdx && i < DOSE_SLOTS_PER_DAY; i++) slotDone[i] = true;
    }
  } else {
    for (int i = 0; i < DOSE_SLOTS_PER_DAY; i++) {
      bool reached = (t.tm_hour > DOSE_HOURS[i]) || (t.tm_hour == DOSE_HOURS[i] && t.tm_min >= DOSE_MINUTES[i]);
      if (reached) slotDone[i] = true;
    }
  }

  doseSlotsPrimed = true;
}

static ModeCfg getModeCfg(DosingMode mode);

void updatePumpSchedules() {
  int activeSlots = (doseScheduleCfg.enabled) ? DOSE_SLOTS_PER_DAY : 3;
  if (activeSlots < 1) activeSlots = 1;

  SEC_PER_DOSE_KALK = SEC_PER_DOSE_AFR = SEC_PER_DOSE_MG = SEC_PER_DOSE_TBD = 0.0f;

  if (modeHasPump(1) && FLOW_KALK_ML_PER_MIN > 0 && activeSlots > 0) {
    float secondsPerDay = (dosing.ml_per_day_kalk / FLOW_KALK_ML_PER_MIN) * 60.0f;
    SEC_PER_DOSE_KALK = secondsPerDay / activeSlots;
  }
  if (modeHasPump(2) && FLOW_AFR_ML_PER_MIN > 0 && activeSlots > 0) {
    float secondsPerDay = (dosing.ml_per_day_afr / FLOW_AFR_ML_PER_MIN) * 60.0f;
    SEC_PER_DOSE_AFR = secondsPerDay / activeSlots;
  }
  if (modeHasPump(3) && FLOW_MG_ML_PER_MIN > 0 && activeSlots > 0) {
    float secondsPerDay = (dosing.ml_per_day_mg / FLOW_MG_ML_PER_MIN) * 60.0f;
    SEC_PER_DOSE_MG = secondsPerDay / activeSlots;
  }
  if (modeHasPump(4) && FLOW_TBD_ML_PER_MIN > 0 && activeSlots > 0) {
    float secondsPerDay = (dosing.ml_per_day_tbd / FLOW_TBD_ML_PER_MIN) * 60.0f;
    SEC_PER_DOSE_TBD = secondsPerDay / activeSlots;
  }

  Serial.printf("Schedules updated: P1=%.2fs P2=%.2fs P3=%.2fs P4=%.2fs (slots=%d)\n",
                SEC_PER_DOSE_KALK, SEC_PER_DOSE_AFR, SEC_PER_DOSE_MG, SEC_PER_DOSE_TBD, activeSlots);
}

void enforceChemSafetyCaps() {
  // Chemistry-based caps (keep rise per day sane). This is a scale-back, not a hard stop.
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
    scale = min(scale, MAX_ALK_RISE_DKH_PER_DAY / alkRise);
  }
  if (caRise > MAX_CA_RISE_PPM_PER_DAY && caRise > 0.0f) {
    scale = min(scale, MAX_CA_RISE_PPM_PER_DAY / caRise);
  }
  if (mgRise > MAX_MG_RISE_PPM_PER_DAY && mgRise > 0.0f) {
    scale = min(scale, MAX_MG_RISE_PPM_PER_DAY / mgRise);
  }

  if (scale < 1.0f) {
    dosing.ml_per_day_kalk *= scale;
    dosing.ml_per_day_afr  *= scale;
    dosing.ml_per_day_mg   *= scale;
    dosing.ml_per_day_tbd  *= scale;
    Serial.printf("SAFETY: Scaling dosing by %.3f\n", scale);
  }
}

// ===================== Pump driver =====================
void firebaseSendStateHeartbeat() {
  if (WiFi.status() != WL_CONNECTED) return;

  time_t nowSec = time(NULL);
  uint64_t tsMs = (nowSec > 0) ? (uint64_t)nowSec * 1000ULL : (uint64_t)millis();

  // pending buckets from NVS (so UI can show catch-up)
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

  const String pathState = "/devices/" + String(DEVICE_ID) + "/state";

  String json = "{";
  json += "\"online\":true,";
  json += "\"fwVersion\":\"" + String(FW_VERSION) + "\",";
  json += "\"lastSeen\":" + String((unsigned long long)tsMs) + ",";
  json += "\"dosingMode\":" + String((int)gDosingMode) + ",";
  json += "\"doseSlotsPerDay\":" + String(max(1, DOSE_SLOTS_PER_DAY)) + ",";

  json += "\"dosingMlPerDay\":{";
  json += "\"kalk\":" + String(dosing.ml_per_day_kalk, 2) + ",";
  json += "\"afr\":"  + String(dosing.ml_per_day_afr,  2) + ",";
  json += "\"mg\":"   + String(dosing.ml_per_day_mg,   2) + ",";
  json += "\"tbd\":"  + String(dosing.ml_per_day_tbd,  2);
  json += "},";

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

  firebasePutJson(pathState, json);
}

// ===================== TIME =====================
uint64_t getEpochMillis() {
  time_t nowSec = time(NULL);
  if (nowSec > 1700000000) { // ~2023 sanity
    return (uint64_t)nowSec * 1000ULL;
  }
  return (uint64_t)millis();
}

// ===================== Pending buckets (prefs) =====================
void clearPendingBuckets(const char* why) {
  // Clear in-RAM pending dose accumulators
  pendingKalkMl = 0.0f;
  pendingAfrMl  = 0.0f;
  pendingMgMl   = 0.0f;
  pendingTbdMl  = 0.0f;

  // Clear persisted buckets too (prevents reboot from re-loading old pending dose)
  Preferences p;
  if (p.begin("doser-buckets", false)) {
    p.putFloat("p_kalk", 0.0f);
    p.putFloat("p_afr",  0.0f);
    p.putFloat("p_mg",   0.0f);
    p.putFloat("p_tbd",  0.0f);
    p.end();
  }

  Serial.printf("Buckets cleared (%s)\n", why ? why : "");
}
void initBucketPrefs() {
  Preferences prefs;
  if (!prefs.begin("doser-buckets", false)) {
    Serial.println("Prefs: failed to open doser-buckets (write)");
    return;
  }
  if (!prefs.isKey("p_kalk")) prefs.putFloat("p_kalk", 0.0f);
  if (!prefs.isKey("p_afr"))  prefs.putFloat("p_afr",  0.0f);
  if (!prefs.isKey("p_mg"))   prefs.putFloat("p_mg",   0.0f);
  if (!prefs.isKey("p_tbd"))  prefs.putFloat("p_tbd",  0.0f);
  prefs.end();
}

// ===================== Dosing prefs (minimal) =====================
void loadDosingFromPrefs() {
  if (!dosingPrefs.begin("dosing", true)) {
    Serial.println("Prefs: failed to open dosing (read)");
    return;
  }
  dosing.ml_per_day_kalk = dosingPrefs.getFloat("kalk", dosing.ml_per_day_kalk);
  dosing.ml_per_day_afr  = dosingPrefs.getFloat("afr",  dosing.ml_per_day_afr);
  dosing.ml_per_day_mg   = dosingPrefs.getFloat("mg",   dosing.ml_per_day_mg);
  dosing.ml_per_day_tbd  = dosingPrefs.getFloat("tbd",  dosing.ml_per_day_tbd);
  dosingPrefs.end();

  Serial.printf("Prefs: loaded dosing KALK=%.2f AFR=%.2f MG=%.2f TBD=%.2f\n",
                dosing.ml_per_day_kalk, dosing.ml_per_day_afr, dosing.ml_per_day_mg, dosing.ml_per_day_tbd);
}
void saveDosingToPrefs() {
  if (!dosingPrefs.begin("dosing", false)) {
    Serial.println("Prefs: failed to open dosing (write)");
    return;
  }
  dosingPrefs.putFloat("kalk", dosing.ml_per_day_kalk);
  dosingPrefs.putFloat("afr",  dosing.ml_per_day_afr);
  dosingPrefs.putFloat("mg",   dosing.ml_per_day_mg);
  dosingPrefs.putFloat("tbd",  dosing.ml_per_day_tbd);
  dosingPrefs.end();

  Serial.printf("Prefs: saved dosing KALK=%.2f AFR=%.2f MG=%.2f TBD=%.2f\n",
                dosing.ml_per_day_kalk, dosing.ml_per_day_afr, dosing.ml_per_day_mg, dosing.ml_per_day_tbd);
}
void loadFlowFromPrefs() {
  if (!dosingPrefs.begin("flow", true)) {
    Serial.println("Prefs: failed to open flow (read)");
    return;
  }
  FLOW_KALK_ML_PER_MIN = dosingPrefs.getFloat("fk", FLOW_KALK_ML_PER_MIN);
  FLOW_AFR_ML_PER_MIN  = dosingPrefs.getFloat("fa", FLOW_AFR_ML_PER_MIN);
  FLOW_MG_ML_PER_MIN   = dosingPrefs.getFloat("fm", FLOW_MG_ML_PER_MIN);
  FLOW_TBD_ML_PER_MIN  = dosingPrefs.getFloat("fx", FLOW_TBD_ML_PER_MIN);
  dosingPrefs.end();

  Serial.printf("Prefs: loaded flow KALK=%.2f AFR=%.2f MG=%.2f TBD=%.2f\n",
                FLOW_KALK_ML_PER_MIN, FLOW_AFR_ML_PER_MIN, FLOW_MG_ML_PER_MIN, FLOW_TBD_ML_PER_MIN);
}
static void validateFlow(const char* name, float &flow, float fallback) {
  if (!isfinite(flow) || flow < 0.5f || flow > 2000.0f) flow = fallback;
  (void)name;
}

// ===================== Recovered production logic =====================
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

void firebaseSetCalibrationStatus() {
  String path = "/devices/" + String(DEVICE_ID) + "/calibration/status";
  uint64_t tsMs = getEpochMillis();
  String json = "{";
  json += "\"appliedAt\":" + String((unsigned long long)tsMs) + ",";
  json += "\"flows\":{";
  json += "\"kalk\":" + String(FLOW_KALK_ML_PER_MIN, 2) + ",";
  json += "\"afr\":" + String(FLOW_AFR_ML_PER_MIN, 2) + ",";
  json += "\"mg\":" + String(FLOW_MG_ML_PER_MIN, 2) + ",";
  json += "\"aux\":" + String(FLOW_TBD_ML_PER_MIN, 2);
  json += "}}";
  firebasePutJson(path, json);
}

void resetAIState() {
  Serial.println("=== AI RESET requested ===");
  historyCount = 0;
  memset(historyBuf, 0, sizeof(historyBuf));
  lastTest    = {0, 0, 0, 0, 0, 0};
  currentTest = {0, 0, 0, 0, 0, 0};
  dosing.ml_per_day_kalk = 2000.0f;
  dosing.ml_per_day_afr  = 20.0f;
  dosing.ml_per_day_mg   = 0.0f;
  dosing.ml_per_day_tbd  = 0.0f;
  lastSafetyBackoffTs = nowSeconds();
  lastRemoteTestTimestampMs = 0;
  updatePumpSchedules();
  saveDosingToPrefs();
}

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
  dosing.ml_per_day_tbd  *= 0.7f;

  dosing.ml_per_day_kalk = clampf(dosing.ml_per_day_kalk, 0.0f, MAX_KALK_ML_PER_DAY);
  dosing.ml_per_day_afr  = clampf(dosing.ml_per_day_afr,  0.0f, MAX_AFR_ML_PER_DAY);
  dosing.ml_per_day_mg   = clampf(dosing.ml_per_day_mg,   0.0f, MAX_MG_ML_PER_DAY);
  dosing.ml_per_day_tbd  = clampf(dosing.ml_per_day_tbd,  0.0f, MAX_TBD_ML_PER_DAY);

  enforceChemSafetyCaps();
  updatePumpSchedules();
  saveDosingToPrefs();
  lastSafetyBackoffTs = now;
}

void firebaseSyncTankSize() {
  String path = "/devices/" + String(DEVICE_ID) + "/settings/tankSize";
  String val = firebaseGetJson(path);
  if (val == "" || val == "null") return;

  float gallons = val.toFloat();
  if (gallons <= 0.0f) return;
  float newLiters = gallons * 3.78541f;
  if (fabsf(newLiters - TANK_VOLUME_L) <= 0.1f) return;

  TANK_VOLUME_L = newLiters;
  float scaleFactor = 1135.6f / TANK_VOLUME_L;
  DKH_PER_ML_KALK_TANK    = 0.00010f * scaleFactor;
  CA_PPM_PER_ML_KALK_TANK = 0.00070f * scaleFactor;
  DKH_PER_ML_AFR_TANK     = 0.0052f  * scaleFactor;
  CA_PPM_PER_ML_AFR_TANK  = 0.037f   * scaleFactor;
  MG_PPM_PER_ML_AFR_TANK  = 0.006f   * scaleFactor;
  MG_PPM_PER_ML_MG_TANK   = 0.20f    * scaleFactor;
  updatePumpSchedules();
}

void firebaseSyncDoseScheduleOnce() {
  String path = "/devices/" + String(DEVICE_ID) + "/settings/doseSchedule";
  String payload = firebaseGetJson(path);
  if (payload.length() == 0 || payload == "null") return;

  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, payload)) return;

  bool enabled    = doc["enabled"]   | false;
  int  startHour  = doc["startHour"] | 0;
  int  endHour    = doc["endHour"]   | 0;
  int  everyMin   = doc["everyMin"]  | 60;

  if (enabled == doseScheduleCfg.enabled &&
      startHour == doseScheduleCfg.startHour &&
      endHour == doseScheduleCfg.endHour &&
      everyMin == doseScheduleCfg.everyMin) return;

  doseScheduleCfg.enabled = enabled;
  doseScheduleCfg.startHour = clampInt(startHour, 0, 23);
  doseScheduleCfg.endHour   = clampInt(endHour,   0, 23);
  doseScheduleCfg.everyMin  = clampInt(everyMin,  1, 240);

  rebuildScheduleSlots();
  doseSlotsPrimed = false;
  primeDoseSlotsForToday();
  updatePumpSchedules();
  clearPendingBuckets("schedule changed");
}

void firebaseSyncDosingPlanOnce() {
  String path = "/devices/" + String(DEVICE_ID) + "/dosingPlan";
  String payload = firebaseGetJson(path);
  if (payload.length() == 0 || payload == "null") return;

  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, payload)) return;

  auto readFloat = [&](const char* key, float fallback) -> float {
    if (!doc.containsKey(key)) return fallback;
    JsonVariant v = doc[key];
    if (v.is<float>() || v.is<int>() || v.is<double>()) return (float)v.as<double>();
    if (v.is<const char*>()) return String(v.as<const char*>()).toFloat();
    return fallback;
  };

  float nk = clampf(readFloat("kalk", dosing.ml_per_day_kalk), 0.0f, MAX_KALK_ML_PER_DAY);
  float na = clampf(readFloat("afr",  dosing.ml_per_day_afr),  0.0f, MAX_AFR_ML_PER_DAY);
  float nm = clampf(readFloat("mg",   dosing.ml_per_day_mg),   0.0f, MAX_MG_ML_PER_DAY);
  float nt = clampf(readFloat("tbd",  dosing.ml_per_day_tbd),  0.0f, MAX_TBD_ML_PER_DAY);

  bool changed = fabsf(nk - dosing.ml_per_day_kalk) > 0.01f ||
                 fabsf(na - dosing.ml_per_day_afr)  > 0.01f ||
                 fabsf(nm - dosing.ml_per_day_mg)   > 0.01f ||
                 fabsf(nt - dosing.ml_per_day_tbd)  > 0.01f;
  if (!changed) return;

  dosing.ml_per_day_kalk = nk;
  dosing.ml_per_day_afr  = na;
  dosing.ml_per_day_mg   = nm;
  dosing.ml_per_day_tbd  = nt;
  updatePumpSchedules();
  saveDosingToPrefs();
  clearPendingBuckets("plan changed");
}

bool firebaseSyncFlowCalibrationOnce() {
  if (WiFi.status() != WL_CONNECTED) return false;
  String path = "/devices/" + String(DEVICE_ID) + "/calibration/pumps";
  String payload = firebaseGetJson(path);
  if (payload.length() == 0 || payload == "null") return false;

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
    return (v > 0.0f) ? v : currentVal;
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

  if (changed) {
    saveFlowToPrefs();
    updatePumpSchedules();
    firebaseSetCalibrationStatus();
  }
  return changed;
}

// ===================== SELF-TEST =====================
// moved declaration earlier: gSelfTestActive
static uint64_t gSelfTestUntilMs = 0;
static uint64_t gSelfTestLastTickMs = 0;
static uint64_t gSelfTestUntilEpochMs = 0;
static bool     gSelfTestRunOnce = false; // if true, run pumps only once per selfTest enable

static uint32_t SELFTEST_DURATION_MS  = 30UL * 1000UL;// 30s: one pump-run per selfTest command during bench testing
static uint32_t SELFTEST_INTERVAL_MS  = 60UL * 1000UL;
static float    SELFTEST_PUMP_SEC     = 2.0f;
static float    SELFTEST_gMinDoseSec = 0.20f;

/** Publish key self-test fields so the PC runner can observe them */
static void publishSelfTestState() {
  String statePath = "/devices/" + String(DEVICE_ID) + "/state";
  firebasePutJson(statePath + "/selfTestActive", gSelfTestActive ? "true" : "false");
  firebasePutJson(statePath + "/selfTestUntil", String((uint64_t)gSelfTestUntilEpochMs));
}

/** Publish current dosing mode into RTDB state */
static void publishDosingModeState() {
  String statePath = "/devices/" + String(DEVICE_ID) + "/state";
  firebasePutJson(statePath + "/dosingMode", String((int)gDosingMode));
}

static DosingMode clampModeInt(int v) {
  if (v < 1) v = 1;
  if (v > 6) v = 6;
  return (DosingMode)v;
}

/** Apply dosing mode immediately (RAM + NVS + RTDB state) */
static void applyDosingMode(DosingMode m, const char* why) {
  if (m == gDosingMode) return;
  gDosingMode = m;
  saveDosingModeToPrefs(gDosingMode);
  clearPendingBuckets("mode changed");
  publishDosingModeState();
  Serial.printf("[MODE] Applied dosingMode=%d (%s)\n", (int)gDosingMode, why ? why : "");
}

static DosingConfig gDosingBackup = {0,0,0,0};
static bool gDosingBackupValid = false;

static DoseScheduleCfg gScheduleBackup;
static bool gScheduleBackupValid = false;

static void applySelfTestDosingOverrides() {}
static void seedBucketsForSelfTest() {}

static void enableSelfTest(const char* why) {
  // If already active, don't restart the timer (prevents duplicate pump runs when
  // the PC script re-writes commands/selfTest=true before we clear it).
  if (gSelfTestActive) {
    Serial.printf("SELFTEST: already active (ignored re-enable: %s)\n", (why ? why : ""));
    return;
  }
  if (!gDosingBackupValid) { gDosingBackup = dosing; gDosingBackupValid = true; }
  if (!gScheduleBackupValid) { gScheduleBackup = doseScheduleCfg; gScheduleBackupValid = true; }

  gSelfTestActive = true;
  gSelfTestUntilMs = (uint64_t)millis() + (uint64_t)SELFTEST_DURATION_MS;
  gSelfTestUntilEpochMs = (uint64_t)getEpochMillis() + (uint64_t)SELFTEST_DURATION_MS;
  gSelfTestLastTickMs = 0;
  gSelfTestRunOnce = true; // default: command-driven selfTest runs once
  publishSelfTestState();
  publishDosingModeState();

  gMinDoseSec = SELFTEST_gMinDoseSec;

  Serial.printf("\n==================== SELFTEST ENABLED (%s) ====================\n\n", (why ? why : ""));
}

static void disableSelfTest(const char* why) {
  if (!gSelfTestActive) return;
  gSelfTestActive = false;
  gMinDoseSec = 1.0f;
  gSelfTestLastTickMs = 0;
  gSelfTestRunOnce = false;
  Serial.printf("\n==================== SELFTEST DISABLED (%s) ====================\n\n", (why ? why : ""));
  publishSelfTestState();
}

// ===================== CALIBRATE COMMAND (RESTORED) =====================
// This is what we "lost": the firmware was no longer watching commands/calibrate
// so the UI would set status=pending forever and nothing would run.
static bool     gCalActive = false;
static uint8_t  gCalPumpIndex = 0;     // 1..4
static uint32_t gCalDurationMs = 0;
static uint32_t gCalStartMs = 0;

static int pumpIndexToPin(uint8_t pump) {
  switch (pump) {
    case 1: return PIN_PUMP_KALK;
    case 2: return PIN_PUMP_AFR;
    case 3: return PIN_PUMP_MG;
    case 4: return PIN_PUMP_TBD;
    default: return -1;
  }
}

static void setPumpOn(uint8_t pump, bool on) {
  if (on && emergencyStopIsActive()) return;
  int pin = pumpIndexToPin(pump);
  if (pin < 0) return;
  digitalWrite(pin, on ? HIGH : LOW);
}

/////////////////////apex stuff chatGPT threw away////////////////////////////////////
static void loadApexFromPrefs() {
  Preferences p;
  if (!p.begin(APEX_PREF_NS, true)) {
    Serial.println("Prefs: failed to open apex (read)");
    return;
  }
  apexEnabled = p.getBool("en", false);
  apexIp = p.getString("ip", "");
  p.end();

  Serial.printf("Prefs: APEX loaded en=%s ip=%s\n", apexEnabled ? "true" : "false", apexIp.c_str());
}

static void saveApexToPrefs(bool en, const String& ip) {
  Preferences p;
  if (!p.begin(APEX_PREF_NS, false)) {
    Serial.println("Prefs: failed to open apex (write)");
    return;
  }
  p.putBool("en", en);
  p.putString("ip", ip);
  p.end();
}

static void applyApexConfig(bool en, const String& ip) {
  apexEnabled = en;
  apexIp = ip;

  if (apexApi) {
    if (apexEnabled && isValidIPv4(apexIp)) {
      Serial.printf("APEX: enabled, setting IP to %s\n", apexIp.c_str());
      apexApi->setIpAddr(apexIp);
    } else {
      Serial.println("APEX: disabled (or invalid IP)");
    }
  }
}

static void pollApexSettings() {
  static uint32_t lastPollMs = 0;
  const uint32_t now = millis();
  if (now - lastPollMs < 3000UL) return;
  lastPollMs = now;

  const String body = firebaseGetJson("/devices/" + String(DEVICE_ID) + "/sensors/Apex");
  if (body.length() == 0 || body == "null") return;

  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, body)) return;
  JsonObject o = doc.as<JsonObject>();
  if (o.isNull()) return;

//TODOO might need to fix this?
  const bool wantEnabled = o.containsKey("enable") ? o["enable"].as<bool>() : apexEnabled;
  const String wantIp = o.containsKey("ipAddress") ? String((const char*)o["ipAddress"]) : apexIp;

  if (wantEnabled != apexEnabled || wantIp != apexIp) {
    saveApexToPrefs(wantEnabled, wantIp);
    applyApexConfig(wantEnabled, wantIp);
    Serial.printf("APEX settings applied from RTDB: en=%s ip=%s\n",
                  wantEnabled ? "true" : "false", wantIp.c_str());
  }
}
///////////////////////////////////////////////////////////////////////////////////

static void calibWriteStatus(const String& status, const String& extraJsonFields = "") {
  String base = "/devices/" + String(DEVICE_ID) + "/commands/calibrate";
  String json = "{";
  json += "\"status\":\"" + status + "\"";
  if (extraJsonFields.length()) json += "," + extraJsonFields;
  json += "}";
  firebasePatchJson(base, json);
}

static void calibStart(uint8_t pump, uint32_t durationSec) {
  if (globalEmergencyStop) {
    calibWriteStatus("error", "\"error\":\"E-STOP active\",\"trigger\":false");
    return;
  }

  int pin = pumpIndexToPin(pump);
  if (pin < 0 || durationSec == 0) {
    calibWriteStatus("error", "\"error\":\"bad pump or duration\",\"trigger\":false");
    return;
  }

  // If a calibration is already running, ignore new start requests
  if (gCalActive) {
    calibWriteStatus("busy", "\"trigger\":false");
    return;
  }

  gCalActive = true;
  gCalPumpIndex = pump;
  gCalDurationMs = durationSec * 1000UL;
  gCalStartMs = millis();

  // ACK immediately so UI knows we received it
  calibWriteStatus(
    "running",
    "\"trigger\":false,"
    "\"startedAt\":" + String((unsigned long long)getEpochMillis()) + ","
    "\"pump\":" + String(pump) + ","
    "\"durationSec\":" + String(durationSec)
  );

  Serial.printf("[CAL] START pump=%u pin=%d duration=%lus\n", pump, pin, (unsigned long)durationSec);
  setPumpOn(pump, true);
}

static void calibTick() {
  if (!gCalActive) return;

  uint32_t now = millis();
  if ((uint32_t)(now - gCalStartMs) >= gCalDurationMs) {
    setPumpOn(gCalPumpIndex, false);

    Serial.printf("[CAL] DONE pump=%u ran=%lu ms\n", gCalPumpIndex, (unsigned long)(now - gCalStartMs));

    calibWriteStatus(
      "done",
      "\"finishedAt\":" + String((unsigned long long)getEpochMillis())
    );

    gCalActive = false;
    gCalPumpIndex = 0;
    gCalDurationMs = 0;
    gCalStartMs = 0;
  }
}

// ===================== TEST INPUT (from RTDB) =====================
static uint64_t gLastTestSampleKey = 0;
static int      gAiRanForMode = 0; // when selfTest/modeSweep, allow exactly 1 AI run per mode
// moved declaration earlier: gModeSweepActive

static uint64_t fnv1a64(const char* s) {
  uint64_t h = 1469598103934665603ULL;
  while (s && *s) {
    h ^= (uint8_t)(*s++);
    h *= 1099511628211ULL;
  }
  return h;
}

static void pollLatestTestSampleAndMaybeRunAi() {
  // Only needed during bench runs.
  if (!(gSelfTestActive || gModeSweepActive)) return;

  static uint32_t lastPollMs = 0;
  const uint32_t now = millis();
  if (now - lastPollMs < 1500) return;
  lastPollMs = now;

  const String body = firebaseGetJson("/devices/" + String(DEVICE_ID) + "/tests?orderBy=%22timestamp%22&limitToLast=1");
  if (body.length() == 0 || body == "null") return;

  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, body)) return;
  JsonObject o = doc.as<JsonObject>();
  if (o.isNull()) return;

  uint64_t key = 0;
  if (o.containsKey("timestamp")) key = (uint64_t)o["timestamp"].as<unsigned long long>();
  if (key == 0 && o.containsKey("ts")) key = (uint64_t)o["ts"].as<unsigned long long>();
  if (key == 0) key = fnv1a64(body.c_str());
  if (key == 0 || key == gLastTestSampleKey) return;
  gLastTestSampleKey = key;

  const float ca  = o.containsKey("ca")  ? o["ca"].as<float>()  : NAN;
  const float alk = o.containsKey("alk") ? o["alk"].as<float>() : NAN;
  const float mg  = o.containsKey("mg")  ? o["mg"].as<float>()  : NAN;
  const float ph  = o.containsKey("ph")  ? o["ph"].as<float>()  : NAN;
  const float tbd = o.containsKey("tbd") ? o["tbd"].as<float>() : 0.0f;

  // Enforce "AI runs once per mode" during bench tests.
  if ((gSelfTestActive || gModeSweepActive)) {
    if (gAiRanForMode == (int)gDosingMode) {
      Serial.printf("AI: skipping extra test sample (already ran for mode %d)\n", (int)gDosingMode);
      return;
    }
    gAiRanForMode = (int)gDosingMode;
  }

  Serial.printf("TEST SAMPLE: ts=%llu ca=%.1f alk=%.2f mg=%.1f ph=%.2f\n",
                (unsigned long long)key, ca, alk, mg, ph);

  const char* prev = gAiCurrentSource;
  gAiCurrentSource = "tests";
  onNewTestInput(ca, alk, mg, ph, tbd);
  gAiCurrentSource = prev;
}

// Simple pump-on helper used by bench tests (selfTest/modeSweep).
// NOTE: This is time-blocking by design (bench only).
// BENCH ONLY
static void benchRunPumpSeconds(int pin, int seconds) {
  if (seconds <= 0) return;
  Serial.printf("BENCH: pump pin %d ON for %d sec\n", pin, seconds);
  digitalWrite(pin, HIGH);
  delay((uint32_t)seconds * 1000UL);
  digitalWrite(pin, LOW);
}

static void selfTestTick() {
  if (!gSelfTestActive) return;
  uint64_t now = (uint64_t)millis();
  if (now >= gSelfTestUntilMs) { disableSelfTest("expired"); return; }
  if (gSelfTestRunOnce && gSelfTestLastTickMs != 0) return; // already ran once
  if (gSelfTestLastTickMs != 0 && (now - gSelfTestLastTickMs) < SELFTEST_INTERVAL_MS) return;
  gSelfTestLastTickMs = now;
  ModeCfg cfg = getModeCfg(gDosingMode);
  benchRunPumpSeconds(PIN_PUMP_KALK, (int)(SELFTEST_PUMP_SEC));
  if (cfg.keyP2) benchRunPumpSeconds(PIN_PUMP_AFR, (int)(SELFTEST_PUMP_SEC));
  if (cfg.keyP3) benchRunPumpSeconds(PIN_PUMP_MG, (int)(SELFTEST_PUMP_SEC));
  if (cfg.keyP4) benchRunPumpSeconds(PIN_PUMP_TBD, (int)(SELFTEST_PUMP_SEC));
}

// ===================== MODE SWEEP (NEW) =====================
static uint8_t  gModeSweepMode = 1;
static uint8_t  gModeSweepStep = 0;
static uint32_t gModeSweepNextMs = 0;
static DosingMode gModeSweepPrevMode = MODE_KALK_ONLY;

static void modeSweepStart(const char* why) {
  if (gModeSweepActive) return;
  gModeSweepActive = true;
  gModeSweepMode = 1;
  gModeSweepStep = 0;
  gModeSweepNextMs = millis();
  gModeSweepPrevMode = gDosingMode;
  if (!gSelfTestActive) enableSelfTest(why ? why : "mode sweep");
  Serial.printf("\n==================== MODE SWEEP STARTED (%s) ====================\n\n", (why ? why : ""));
}

static void modeSweepStop(const char* why) {
  if (!gModeSweepActive) return;
  gModeSweepActive = false;
  gDosingMode = gModeSweepPrevMode;
  saveDosingModeToPrefs(gDosingMode);
  clearPendingBuckets("mode changed");
  disableSelfTest("mode sweep done");
  Serial.printf("\n==================== MODE SWEEP STOPPED (%s) ====================\n\n", (why ? why : ""));
}

static void modeSweepInjectTestsForAi(uint8_t mode) {
  (void)mode;
  const float ca1  = 440.0f, alk1 = 8.30f, mg1 = 1350.0f, ph1 = 8.10f;
  const float ca2  = 438.0f, alk2 = 8.10f, mg2 = 1340.0f, ph2 = 8.08f;

  const char* prev = gAiCurrentSource;
  gAiCurrentSource = "tests";
  onNewTestInput(ca1, alk1, mg1, ph1, 0.0f);
  onNewTestInput(ca2, alk2, mg2, ph2, 0.0f);
  gAiCurrentSource = prev;
}

static void modeSweepRunPumpsOnceForMode() {
  ModeCfg cfg = getModeCfg(gDosingMode);
  runPumpSeconds(PIN_PUMP_KALK, (float)(SELFTEST_PUMP_SEC));
  if (cfg.keyP2) runPumpSeconds(PIN_PUMP_AFR, (float)(SELFTEST_PUMP_SEC));
  if (cfg.keyP3) runPumpSeconds(PIN_PUMP_MG, (float)(SELFTEST_PUMP_SEC));
  if (cfg.keyP4) runPumpSeconds(PIN_PUMP_TBD, (float)(SELFTEST_PUMP_SEC));
}

static void modeSweepTick() {
  if (!gModeSweepActive) return;
  if ((int32_t)(millis() - gModeSweepNextMs) < 0) return;

  switch (gModeSweepStep) {
    case 0: {
      gDosingMode = clampDosingMode((int)gModeSweepMode);
      saveDosingModeToPrefs(gDosingMode);
      clearPendingBuckets("mode changed");
      historyCount = 0;
      memset(historyBuf, 0, sizeof(historyBuf));
      lastTest = {0,0,0,0,0,0};
      currentTest = {0,0,0,0,0,0};
      clearPendingBuckets("mode sweep new mode");
      updatePumpSchedules();
      Serial.printf("\n[MODE SWEEP] >>> MODE %u START (%s)\n", gModeSweepMode, dosingModeLabel(gDosingMode));
      gModeSweepStep = 1;
      gModeSweepNextMs = millis() + 250;
    } break;

    case 1: {
      modeSweepInjectTestsForAi(gModeSweepMode);
      gModeSweepStep = 2;
      gModeSweepNextMs = millis() + 250;
    } break;

    case 2: {
      modeSweepRunPumpsOnceForMode();
      gModeSweepStep = 3;
      gModeSweepNextMs = millis() + 350;
    } break;

    case 3: {
      Serial.printf("[MODE SWEEP] <<< MODE %u DONE\n", gModeSweepMode);
      gModeSweepMode++;
      if (gModeSweepMode > 6) { modeSweepStop("completed 1..6"); return; }
      gModeSweepStep = 0;
      gModeSweepNextMs = millis() + 500;
    } break;

    default:
      gModeSweepStep = 0;
      gModeSweepNextMs = millis() + 500;
      break;
  }
}

// ===================== AI core (relaxed timing when selfTest/modeSweep) =====================
void onNewTestInput(float ca, float alk, float mg, float ph, float tbd_val) {
  lastTest = currentTest;
  currentTest.t   = nowSeconds();
  currentTest.ca  = ca;
  currentTest.alk = alk;
  currentTest.mg  = mg;
  currentTest.ph  = ph;
  currentTest.tbd = tbd_val;
  pushHistory(currentTest);

  if (ca   < 300.0f || ca  > 550.0f ||
      alk  <   5.0f || alk > 14.0f  ||
      mg   < 1100.0f || mg > 1600.0f ||
      ph   <   7.0f || ph  >  9.0f) {
    Serial.println("SAFETY: IGNORING TEST for dosing (out-of-range). Graph updated only.");
    return;
  }

  if (lastTest.t == 0) {
    updatePumpSchedules();
    publishAiBreadcrumbs(gAiCurrentSource, "firstTest", ca, alk, mg, ph);
    return;
  }

  float days = float(currentTest.t - lastTest.t) / 86400.0f;
  if (days <= 0.25f && !(gSelfTestActive || gModeSweepActive)) {
    Serial.println("SAFETY: Tests too close together, ignoring for dosing updates.");
    return;
  }
  if (days <= 0.0f) days = 0.0001f;

  float consAlk = (lastTest.alk - currentTest.alk) / days;
  float consCa  = (lastTest.ca  - currentTest.ca ) / days;
  float consMg  = (lastTest.mg  - currentTest.mg ) / days;
  float consTbd = (lastTest.tbd - currentTest.tbd) / days;

  float alkNeeded = consAlk;
  if (alkNeeded < 0.0f) alkNeeded = 0.0f;

  float kalkFrac = 0.8f;
  if (!isnan(currentTest.ph)) {
    float phError = currentTest.ph - TARGET_PH;
    if (phError < -0.05f) kalkFrac = 0.90f;
    else if (phError > 0.05f) kalkFrac = 0.70f;
  }
  kalkFrac = clampf(kalkFrac, 0.6f, 0.95f);

  float targetAlkFromKalk = kalkFrac * alkNeeded;
  float targetAlkFromAfr  = (1.0f - kalkFrac) * alkNeeded;

  float suggested_ml_kalk = (DKH_PER_ML_KALK_TANK > 0.0f) ? (targetAlkFromKalk / DKH_PER_ML_KALK_TANK) : 0.0f;
  float suggested_ml_afr  = (DKH_PER_ML_AFR_TANK  > 0.0f) ? (targetAlkFromAfr  / DKH_PER_ML_AFR_TANK ) : 0.0f;

  float caFromKalk = suggested_ml_kalk * CA_PPM_PER_ML_KALK_TANK;
  float caFromAfr  = suggested_ml_afr  * CA_PPM_PER_ML_AFR_TANK;
  float mgFromAfr  = suggested_ml_afr  * MG_PPM_PER_ML_AFR_TANK;

  float caError = consCa - (caFromKalk + caFromAfr);
  if (fabsf(caError) > 5.0f && CA_PPM_PER_ML_AFR_TANK > 0.0f) {
    float afrCorrection = (caError / CA_PPM_PER_ML_AFR_TANK) * 0.3f;
    suggested_ml_afr += afrCorrection;
  }

  float suggested_ml_mg = dosing.ml_per_day_mg;
  if (consMg > mgFromAfr + 0.5f && MG_PPM_PER_ML_MG_TANK > 0.0f) {
    float mgCorrection = (consMg - mgFromAfr) / MG_PPM_PER_ML_MG_TANK;
    mgCorrection *= 0.3f;
    suggested_ml_mg += mgCorrection;
  }

  float suggested_ml_tbd = dosing.ml_per_day_tbd;
  if (consTbd > 0.1f) suggested_ml_tbd += (consTbd * 0.2f);

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

  enforceChemSafetyCaps();
  updatePumpSchedules();
  saveDosingToPrefs();
  lastSafetyBackoffTs = nowSeconds();

  publishAiBreadcrumbs(gAiCurrentSource, (gModeSweepActive ? "modeSweep" : "onNewTestInput"), ca, alk, mg, ph);
  Serial.printf("AI Update: mode=%d recalculated dosing plan.\n", (int)gDosingMode);
}

// ===================== Commands poll (includes calibrate + modeSweep + selfTest) =====================
void firebasePollCommandsFast() {
  if (WiFi.status() != WL_CONNECTED) return;

  const String base = "/devices/" + String(DEVICE_ID) + "/commands";
  const String payload = firebaseGetJson(base);
  if (payload.length() == 0 || payload == "null") return;

  StaticJsonDocument<2048> doc;
  if (deserializeJson(doc, payload)) return;

  JsonObject root = doc.as<JsonObject>();
  if (root.isNull()) return;

  if (root.containsKey("resetAi")) {
    bool wantReset = root["resetAi"].is<bool>() ? root["resetAi"].as<bool>() : false;
    if (!wantReset && root["resetAi"].is<const char*>()) {
      String v = String(root["resetAi"].as<const char*>());
      v.toLowerCase();
      wantReset = (v == "true");
    }
    if (wantReset) {
      resetAIState();
      firebasePutJson(base + "/resetAi", "false");
    }
  }

  if (root.containsKey("liveDose") && root["liveDose"].is<JsonObject>()) {
    JsonObject ld = root["liveDose"].as<JsonObject>();
    bool trigger = ld["trigger"] | false;
    if (trigger) {
      int pump = ld["pump"] | 0;
      float ml = ld["ml"] | 0.0f;
      int pin = -1;
      float flow = 0.0f;
      String pumpName;
      switch (pump) {
        case 1: pin = PIN_PUMP_KALK; flow = FLOW_KALK_ML_PER_MIN; pumpName = "kalk"; break;
        case 2: pin = PIN_PUMP_AFR;  flow = FLOW_AFR_ML_PER_MIN;  pumpName = "afr";  break;
        case 3: pin = PIN_PUMP_MG;   flow = FLOW_MG_ML_PER_MIN;   pumpName = "mg";   break;
        case 4: pin = PIN_PUMP_TBD;  flow = FLOW_TBD_ML_PER_MIN;  pumpName = "tbd";  break;
      }
      if (pin >= 0 && flow > 0.0f && ml > 0.0f) {
        if (ml > 2000.0f) ml = 2000.0f;
        doseAndLog(pump, pumpName, pin, ml, flow, "live");
      }
      StaticJsonDocument<128> out;
      out["trigger"] = false;
      out["lastRun"] = (unsigned long long)getEpochMillis();
      out["pump"] = pump;
      out["ml"] = ml;
      String clearJson;
      serializeJson(out, clearJson);
      firebasePutJson(base + "/liveDose", clearJson);
    }
  }

  // ---- CALIBRATE (RESTORED) ----
  // Expect: commands/calibrate { trigger:true, pump:1..4, durationSec:N, status:"pending" }
  if (root.containsKey("calibrate")) {
    JsonObject c = root["calibrate"].as<JsonObject>();
    if (!c.isNull()) {
      bool trig = c.containsKey("trigger") ? c["trigger"].as<bool>() : false;
      int  pump = c.containsKey("pump") ? c["pump"].as<int>() : 0;
      int  dur  = c.containsKey("durationSec") ? c["durationSec"].as<int>() : 0;

      if (trig && !gCalActive) {
        // Start calibration and immediately flip trigger=false + status=running
        calibStart((uint8_t)pump, (uint32_t)dur);
      }
    }
  }

  if (root.containsKey("modeSweep")) {
    bool want = root["modeSweep"].is<bool>() ? root["modeSweep"].as<bool>() : false;
    if (want) {
      firebasePutJson(base + "/modeSweep", "false");
      modeSweepStart("RTDB command");
    }
  }

  if (root.containsKey("selfTest")) {
    bool want = root["selfTest"].is<bool>() ? root["selfTest"].as<bool>() : false;
    if (want) {
      firebasePutJson(base + "/selfTest", "false");
      enableSelfTest("RTDB command");
    }
  }
  ///////////////////////////////ota//////////////////////////////////////////
if (root.containsKey("otaRequest") && root["otaRequest"].is<JsonObject>()) {
  JsonObject ota = root["otaRequest"].as<JsonObject>();
  bool trig = ota["trigger"] | false;
  if (trig) {
    Serial.println("OTA Trigger command detected!");
    String myCorrectUrl = "https://aidoser.web.app/devices/" + String(DEVICE_ID) + "/firmware.bin";
    Serial.print("Forcing update from: ");
    Serial.println(myCorrectUrl);

    firebasePutJson(base + "/otaRequest", "null");
    performOtaFromUrl(myCorrectUrl);
  }
}
  ////////////////////////////////////////////////////////////////////////////

  // Watch settings/dosingMode so the PC runner can change modes.
  static uint64_t lastModePollMs = 0;
  uint64_t nowMs = millis();
  if (nowMs - lastModePollMs >= 1500) {
    lastModePollMs = nowMs;
    String modeBody = firebaseGetJson("/devices/" + String(DEVICE_ID) + "/settings/dosingMode");
    modeBody.trim();
    if (modeBody.length() && modeBody != "null") {
      int v = modeBody.toInt();
      if (v >= 1 && v <= 6) {
        applyDosingMode(clampModeInt(v), "RTDB settings/dosingMode");
      }
    }
  }
}

// ===================== Web server minimal =====================
const char MAIN_PAGE_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8"><title>Reef Doser ESP32</title></head>
<body><h2>Reef Doser ESP32</h2><p>This ESP32 is online and controlled via Firebase.</p></body></html>
)rawliteral";

void handleRoot(){ server.send_P(200, "text/html", MAIN_PAGE_HTML); }

// ===================== SIMPLE LEGACY DAILY SCHEDULE (3 slots) =====================
// This restores the original "3 doses per day" behavior without the PC self-test harness.
// If you enable the newer window-based schedule later, you can replace this with that logic.
static const int LEGACY_SLOTS = 3;
static const int LEGACY_DOSE_H[LEGACY_SLOTS] = {9, 12, 15};
static const int LEGACY_DOSE_M[LEGACY_SLOTS] = {30, 30, 30};
static bool legacySlotDone[LEGACY_SLOTS] = {false, false, false};
static int legacyLastYday = -1;

// Run a dose on a physical pump pin for durationSec, then turn it off.
static bool runPumpSeconds(int pin, float durationSec) {
  if (durationSec <= 0.0f) return false;
  if (emergencyStopIsActive()) return false;
  digitalWrite(pin, HIGH);
  uint32_t ms = (uint32_t)(durationSec * 1000.0f);
  uint32_t start = millis();
  while ((uint32_t)(millis() - start) < ms) {
    delay(25);
    if (emergencyStopIsActive()) {
      digitalWrite(pin, LOW);
      return false;
    }
  }
  digitalWrite(pin, LOW);
  return true;
}
///////////////////////////////////ota////////////////////////////////////////////
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
  https.setTimeout(30000);
  https.setReuse(false);

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

  delay(1000);
  ESP.restart();
  return true; // never reached
}
//////////////////////////////////////////////////////////////////////////////

/////////////////////////////Emergency switch////////////////////////////////
// ===================== EMERGENCY STOP (killSwitch) =====================
// UI path: /devices/<id>/settings/killSwitch  (boolean)
// When true: force all pumps OFF and block any pump ON attempts.

static void emergencyStopAllPumpsOff(const char* why) {
  digitalWrite(PIN_PUMP_KALK, LOW);
  digitalWrite(PIN_PUMP_AFR,  LOW);
  digitalWrite(PIN_PUMP_MG,   LOW);
  digitalWrite(PIN_PUMP_TBD,  LOW);

  // If calibration is running, stop it immediately.
  if (gCalActive) {
    setPumpOn(gCalPumpIndex, false);
    calibWriteStatus("error", "\"error\":\"E-STOP activated\",\"trigger\":false");
    gCalActive = false;
    gCalPumpIndex = 0;
    gCalDurationMs = 0;
    gCalStartMs = 0;
  }

  Serial.printf("!!! EMERGENCY STOP: pumps forced OFF (%s)\n", (why ? why : ""));
}

// Poll Firebase for killSwitch and update globalEmergencyStop
static void pollKillSwitch() {
  static uint32_t lastPollMs = 0;
  uint32_t now = millis();
  if (now - lastPollMs < 1500) return;   // ~1.5s poll
  lastPollMs = now;

  String body = firebaseGetJson("/devices/" + String(DEVICE_ID) + "/settings/killSwitch");
  body.trim();

  bool want = false;
  if (body.length() && body != "null") {
    // RTDB REST returns "true"/"false"
    want = (body.indexOf("true") >= 0);
  }

  if (want && !globalEmergencyStop) {
    globalEmergencyStop = true;
    emergencyStopAllPumpsOff("killSwitch=true");
  } else if (!want && globalEmergencyStop) {
    globalEmergencyStop = false;
    Serial.println("Emergency Stop cleared (killSwitch=false). Pumps re-enabled.");
  }
}

// True if E-STOP is active (also enforces pumps OFF)
static inline bool emergencyStopIsActive() {
  if (globalEmergencyStop) {
    emergencyStopAllPumpsOff("guard");
    return true;
  }
  return false;
}
////////////////////////////////////////////////////////////////////////////

// Doses 'ml' on a given physical pump, logs to RTDB doseRuns.
static void doseAndLog(int pumpIndex, const String& pumpName, int pin, float ml, float flowMlPerMin, const String& source) {
  if (emergencyStopIsActive()) return;
  if (ml <= 0.0f || flowMlPerMin <= 0.0f) return;
  const float sec = (ml / flowMlPerMin) * 60.0f;
  if (sec < gMinDoseSec) return;
  if (!runPumpSeconds(pin, sec)) return;
  firebaseLogDoseRun(pumpIndex, pumpName, ml, sec, flowMlPerMin, source);
}

void pollApexAndPublish(bool allowAiUpdate) {
  if (!apexEnabled) return;
  if (!apexApi) return;
  if (!isValidIPv4(apexIp)) return;

  String ret = apexApi->getState();
  Serial.println(ret);

  // Pull latest values from ApexApi
  float ph  = apexApi->getPh();
  float alk = apexApi->getAlk();
  float ca  = apexApi->getCa();
  float mg  = apexApi->getMg();
  float cond = apexApi->getCond();
  float tempF = apexApi->getTempF();

  if (!apexValuesLookValid(ca, alk, mg, ph)) {
    Serial.printf("APEX read invalid: ca=%.1f alk=%.2f mg=%.1f ph=%.2f\n", ca, alk, mg, ph);
    return;
  }

  // Remember latest good values for scheduler freshness/use.
  lastApexCurCa  = ca;
  lastApexCurAlk = alk;
  lastApexCurMg  = mg;
  lastApexCurPh  = ph;

  // 1) Publish to RTDB sensors for UI pills
  {
    String sensorsPatch = String("{") +
      "\"pH\":"   + String(ph, 2) + "," +
      "\"alk\":"  + String(alk, 2) + "," +
      "\"ca\":"   + String(ca, 1) + "," +
      "\"mg\":"   + String(mg, 1) + "," +
      "\"tempF\":" + String(tempF, 2) + "," +
      "\"temp\":"  + String(tempF, 2) + "," +
      "\"sg\":"   + String(cond, 2) +
    "}";
    firebasePatchJson("/devices/" + String(DEVICE_ID) + "/sensors", sensorsPatch);
  }

  // Optional “last apex snapshot” block
  {
    uint64_t ts = getEpochMillis();
    String json = "{";
    json += "\"ts\":" + String((unsigned long long)ts) + ",";
    json += "\"ph\":" + String(ph, 2) + ",";
    json += "\"alk\":" + String(alk, 2) + ",";
    json += "\"ca\":" + String(ca, 1) + ",";
    json += "\"mg\":" + String(mg, 1);
    json += "}";
    firebasePutJson("/devices/" + String(DEVICE_ID) + "/state/apexLatest", json);
  }

  // 2) Optionally feed the AI
  if (allowAiUpdate) {
    // Don’t AI-update if changes are tiny (noise) unless it’s the first time.
    if (!apexChangedEnough(ca, alk, mg, ph)) {
      Serial.println("APEX: change too small, skipping AI update.");
      return;
    }

    Serial.printf("APEX -> AI update: ca=%.1f alk=%.2f mg=%.1f ph=%.2f\n", ca, alk, mg, ph);

    const char* prev = gAiCurrentSource;
    gAiCurrentSource = "apex";
    onNewTestInput(ca, alk, mg, ph, 0.0f);
    publishAiBreadcrumbs("apex", "poll", ca, alk, mg, ph);
    gAiCurrentSource = prev;
    lastApexAiCa  = ca;
    lastApexAiAlk = alk;
    lastApexAiMg  = mg;
    lastApexAiPh  = ph;

    // (optional) log a “test” into /tests so it shows in history table/chart
    StaticJsonDocument<256> doc;
    doc["ca"] = ca;
    doc["alk"] = alk;
    doc["mg"] = mg;
    doc["ph"] = ph;
    doc["timestamp"] = (unsigned long long)getEpochMillis();
    doc["source"] = "apex";
    String body;
    serializeJson(doc, body);
    firebasePostJson("/devices/" + String(DEVICE_ID) + "/tests", body);
  }
}

static bool inRange(float v, float lo, float hi) {
  return isfinite(v) && (v >= lo) && (v <= hi);
}

static bool isValidIPv4(const String& ip) {
  int parts = 0;
  int start = 0;
  while (true) {
    int dot = ip.indexOf('.', start);
    String token = (dot >= 0) ? ip.substring(start, dot) : ip.substring(start);
    token.trim();
    if (token.length() == 0) return false;

    for (size_t i = 0; i < token.length(); i++) {
      if (!isDigit(token[i])) return false;
    }

    int v = token.toInt();
    if (v < 0 || v > 255) return false;

    parts++;
    if (dot < 0) break;
    start = dot + 1;
  }
  return (parts == 4);
}

static bool apexValuesLookValid(float ca, float alk, float mg, float ph) {
  return inRange(ca,  300.0f, 550.0f) &&
         inRange(alk,   5.0f,  14.0f) &&
         inRange(mg, 1100.0f, 1600.0f) &&
         inRange(ph,    7.0f,   9.0f);
}

static bool apexChangedEnough(float ca, float alk, float mg, float ph) {
  const float CA_EPS  = 3.0f;
  const float ALK_EPS = 0.10f;
  const float MG_EPS  = 10.0f;
  const float PH_EPS  = 0.03f;

  if (!isfinite(lastApexAiCa) || !isfinite(lastApexAiAlk) ||
      !isfinite(lastApexAiMg) || !isfinite(lastApexAiPh)) {
    return true;
  }

  return (fabsf(ca  - lastApexAiCa)  >= CA_EPS)  ||
         (fabsf(alk - lastApexAiAlk) >= ALK_EPS) ||
         (fabsf(mg  - lastApexAiMg)  >= MG_EPS)  ||
         (fabsf(ph  - lastApexAiPh)  >= PH_EPS);
}

// Perform scheduled dosing (3 fixed times) using the NVS "pending buckets" so missed slots accumulate.
void maybeDosePumpsRealTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;
  if (!isTimeValid(timeinfo)) return;

  const int windowDay = doseScheduleCfg.enabled ? windowStartYday(timeinfo, doseScheduleCfg.startHour, doseScheduleCfg.endHour)
                                                : timeinfo.tm_yday;

  if (!doseSlotsPrimed || windowDay != lastDoseWindowDay) {
    lastDoseWindowDay = windowDay;
    doseSlotsPrimed = true;
    rebuildScheduleSlots();
    for (int i = 0; i < DOSE_SLOTS_PER_DAY; i++) slotDone[i] = false;
  }

  int nowIdx = -1;
  if (doseScheduleCfg.enabled) {
    nowIdx = scheduleSlotIndex(timeinfo, doseScheduleCfg.startHour, doseScheduleCfg.endHour, doseScheduleCfg.everyMin);
  } else {
    for (int i = 0; i < DOSE_SLOTS_PER_DAY; i++) {
      if (timeinfo.tm_hour == DOSE_HOURS[i] && timeinfo.tm_min == DOSE_MINUTES[i]) {
        nowIdx = i;
        break;
      }
    }
  }

  if (nowIdx < 0 || nowIdx >= DOSE_SLOTS_PER_DAY) return;
  if (slotDone[nowIdx]) return;

  Preferences prefs;
  if (!prefs.begin("doser-buckets", false)) {
    Serial.println("Prefs: failed to open doser-buckets");
    return;
  }
  pendingKalkMl = prefs.getFloat("p_kalk", 0.0f);
  pendingAfrMl  = prefs.getFloat("p_afr",  0.0f);
  pendingMgMl   = prefs.getFloat("p_mg",   0.0f);
  pendingTbdMl  = prefs.getFloat("p_tbd",  0.0f);

  const ModeCfg cfgAlloc = getModeCfg(gDosingMode);
  const float slotsPerDay = (float)max(1, DOSE_SLOTS_PER_DAY);
  if (modeHasPump(gDosingMode, 1)) pendingKalkMl += mlPerDayForKey(cfgAlloc.keyP1) / slotsPerDay;
  if (modeHasPump(gDosingMode, 2)) pendingAfrMl  += mlPerDayForKey(cfgAlloc.keyP2) / slotsPerDay;
  if (modeHasPump(gDosingMode, 3)) pendingMgMl   += mlPerDayForKey(cfgAlloc.keyP3) / slotsPerDay;
  if (modeHasPump(gDosingMode, 4)) pendingTbdMl  += mlPerDayForKey(cfgAlloc.keyP4) / slotsPerDay;

  const ModeCfg cfg = getModeCfg(gDosingMode);
  if (pendingKalkMl > 0.0f && cfg.keyP1) {
    float sec = (pendingKalkMl / FLOW_KALK_ML_PER_MIN) * 60.0f;
    if (sec >= gMinDoseSec && runPumpSeconds(PIN_PUMP_KALK, sec)) {
      firebaseLogDoseRun(1, cfg.keyP1, pendingKalkMl, sec, FLOW_KALK_ML_PER_MIN, "schedule");
      pendingKalkMl = 0.0f;
    }
  }
  if (pendingAfrMl > 0.0f && cfg.keyP2) {
    float sec = (pendingAfrMl / FLOW_AFR_ML_PER_MIN) * 60.0f;
    if (sec >= gMinDoseSec && runPumpSeconds(PIN_PUMP_AFR, sec)) {
      firebaseLogDoseRun(2, cfg.keyP2, pendingAfrMl, sec, FLOW_AFR_ML_PER_MIN, "schedule");
      pendingAfrMl = 0.0f;
    }
  }
  if (pendingMgMl > 0.0f && cfg.keyP3) {
    float sec = (pendingMgMl / FLOW_MG_ML_PER_MIN) * 60.0f;
    if (sec >= gMinDoseSec && runPumpSeconds(PIN_PUMP_MG, sec)) {
      firebaseLogDoseRun(3, cfg.keyP3, pendingMgMl, sec, FLOW_MG_ML_PER_MIN, "schedule");
      pendingMgMl = 0.0f;
    }
  }
  if (pendingTbdMl > 0.0f && cfg.keyP4) {
    float sec = (pendingTbdMl / FLOW_TBD_ML_PER_MIN) * 60.0f;
    if (sec >= gMinDoseSec && runPumpSeconds(PIN_PUMP_TBD, sec)) {
      firebaseLogDoseRun(4, cfg.keyP4, pendingTbdMl, sec, FLOW_TBD_ML_PER_MIN, "schedule");
      pendingTbdMl = 0.0f;
    }
  }

  prefs.putFloat("p_kalk", pendingKalkMl);
  prefs.putFloat("p_afr",  pendingAfrMl);
  prefs.putFloat("p_mg",   pendingMgMl);
  prefs.putFloat("p_tbd",  pendingTbdMl);
  prefs.end();

  slotDone[nowIdx] = true;
  Serial.printf("SCHEDULE: slot %d handled at %02d:%02d\n", nowIdx + 1, timeinfo.tm_hour, timeinfo.tm_min);
}

void setup(){
  Serial.begin(115200);
  delay(300);

  snprintf(NTFY_TOPIC, sizeof(NTFY_TOPIC), "aidoser-%s-%s", DEVICE_ID, TOPIC_SUFFIX);
  ntfiUrl = String("https://ntfy.sh/") + NTFY_TOPIC;

  pinMode(PIN_PUMP_KALK, OUTPUT);
  pinMode(PIN_PUMP_AFR,  OUTPUT);
  pinMode(PIN_PUMP_MG,   OUTPUT);
  pinMode(PIN_PUMP_TBD,  OUTPUT);
  digitalWrite(PIN_PUMP_KALK, LOW);
  digitalWrite(PIN_PUMP_AFR,  LOW);
  digitalWrite(PIN_PUMP_MG,   LOW);
  digitalWrite(PIN_PUMP_TBD,  LOW);

  loadDosingModeFromPrefs();
  loadDosingFromPrefs();
  loadFlowFromPrefs();
  validateFlow("KALK", FLOW_KALK_ML_PER_MIN, 675.0f);
  validateFlow("AFR",  FLOW_AFR_ML_PER_MIN,  645.0f);
  validateFlow("MG",   FLOW_MG_ML_PER_MIN,    50.0f);
  validateFlow("TBD",  FLOW_TBD_ML_PER_MIN,   50.0f);
  updatePumpSchedules();
  rebuildScheduleSlots();
  initBucketPrefs();

  wifiTuningInit();

  if(!wm.autoConnect("ESP32_Config")) {
    Serial.println("WiFi not configured yet.");
  } else {
    Serial.println("WiFi connected.");
  }

  secureClient.setInsecure();
  configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER);
  primeDoseSlotsForToday();
  lastSafetyBackoffTs = nowSeconds();

  server.on("/", handleRoot);
  server.begin();

  apexApi = new ApexApi();
  loadApexFromPrefs();
  applyApexConfig(apexEnabled, apexIp);

  // One initial sync pass so production settings are restored right after boot.
  firebaseSyncDoseScheduleOnce();
  firebaseSyncDosingPlanOnce();
  firebaseSyncTankSize();
  firebaseSyncFlowCalibrationOnce();
}

void loop(){
  unsigned long nowMs = millis();
  server.handleClient();
  wifiKeepAliveTick();
  pollKillSwitch();

  // Calibration state machine (turns pump off when done)
  calibTick();
  safetyBackoffIfNoTests();

  // Restore production scheduled dosing (3 slots/day) + buckets
  maybeDosePumpsRealTime();

  // Bench self-test/mode sweep are still available, but they do NOT replace production behavior.
  if (!gModeSweepActive) selfTestTick();
  modeSweepTick();

  // Optional: allow the PC runner to feed samples and force AI once per mode (harmless in production).
  pollLatestTestSampleAndMaybeRunAi();
  pollManualInputs();
  pollApexSettings();

  static unsigned long lastSlowPollMs = 0;
  if (nowMs - lastSlowPollMs >= 600000UL) {
    lastSlowPollMs = nowMs;
    firebaseSyncDoseScheduleOnce();
    firebaseSyncDosingPlanOnce();
    firebaseSyncTankSize();
    firebaseSyncFlowCalibrationOnce();
  }

  // Throttled Apex polling only. Do NOT poll every loop.
  if (apexEnabled) {
    const uint64_t nowMs64 = millis();
    if (nowMs64 - lastApexPollMs >= APEX_POLL_MIN_MS) {
      const bool allowAiUpdate = (nowMs64 - lastApexAiApplyMs >= APEX_AI_MIN_MS);
      pollApexAndPublish(allowAiUpdate);
      lastApexPollMs = nowMs64;
      if (allowAiUpdate) lastApexAiApplyMs = nowMs64;
    }
  }

  // Scheduler runs after Apex poll so fresh Apex data can be preferred.
  runAiScheduler();

  // Fast command poll
  static unsigned long lastCmdPollMs  = 0;
  if (nowMs - lastCmdPollMs >= 3000UL) {
    lastCmdPollMs = nowMs;
    firebasePollCommandsFast();
  }

  // State heartbeat
  static unsigned long lastStateMs = 0;
  if (nowMs - lastStateMs >= 30000UL) {
    lastStateMs = nowMs;
    firebaseSendStateHeartbeat();
  }
}
