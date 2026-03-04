// main.cpp (updated with MODE SWEEP self-test runner)
// Goal: Run each dosing mode (1..6) and force AI to run once per mode, then move on.
// Added:
//  - /devices/<id>/commands/modeSweep = true  (or "true")
//  - modeSweepTick() state machine (non-blocking)
//  - AI timing gates relaxed during selfTest/modeSweep so AI can run immediately
//
// IMPORTANT: This is a bench/testing feature. Disable/remove once validated.

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
String firebaseGetJson(const String& path);

WiFiManager wm;

String ntfiUrl;

WebServer server(80);

///////////////////////////////////////////////////////////////////////////////////////
// things you must change
char DEVICE_ID[] = "reefDoser6";
char TOPIC_SUFFIX[] = "mark1958";
char NTFY_TOPIC[64];
const char* FW_VERSION      = "3.0.2";
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

struct ModeCfg {
  const char* keyP1;
  const char* keyP2;
  const char* keyP3;
  const char* keyP4;
};

static ModeCfg getModeCfg(DosingMode mode);
static bool modeHasPump(int pumpIndex) {
  ModeCfg cfg = getModeCfg(gDosingMode);
  if (pumpIndex == 1) return (cfg.keyP1 != nullptr);
  if (pumpIndex == 2) return (cfg.keyP2 != nullptr);
  if (pumpIndex == 3) return (cfg.keyP3 != nullptr);
  if (pumpIndex == 4) return (cfg.keyP4 != nullptr);
  return false;
}

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

void onNewTestInput(float ca, float alk, float mg, float ph, float tbd_val);

ApexApi *apexApi = nullptr;

// ===================== APEX -> AI ingestion =====================
static const uint32_t APEX_AI_MIN_MS   = 12UL * 60UL * 60UL * 1000UL;
static const uint32_t APEX_POLL_MIN_MS = 5UL  * 60UL * 1000UL;

uint64_t lastApexAiApplyMs  = 0;
uint64_t lastApexPollMs     = 0;

float lastApexAiCa  = NAN;
float lastApexAiAlk = NAN;
float lastApexAiMg  = NAN;
float lastApexAiPh  = NAN;

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
const float TARGET_PH  = 8.3f;

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

void pushHistory(const TestPoint& tp){
  if(historyCount < MAX_HISTORY){
    historyBuf[historyCount++] = tp;
  } else {
    for(int i=1;i<MAX_HISTORY;i++) historyBuf[i-1] = historyBuf[i];
    historyBuf[MAX_HISTORY-1] = tp;
  }
}

// ===================== Schedule placeholders =====================
static const int MAX_DOSE_SLOTS = 96;
int  DOSE_SLOTS_PER_DAY = 3;
float SEC_PER_DOSE_KALK = 0.0f;
float SEC_PER_DOSE_AFR  = 0.0f;
float SEC_PER_DOSE_MG   = 0.0f;
float SEC_PER_DOSE_TBD  = 0.0f;

// Minimal schedule config
struct DoseScheduleCfg {
  bool enabled   = false;
  int  startHour = 0;
  int  endHour   = 0;
  int  everyMin  = 60;
  uint64_t updatedAt = 0;
};
DoseScheduleCfg doseScheduleCfg;

static void rebuildScheduleSlots() {}

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

void enforceChemSafetyCaps() {}

// ===================== Pump driver =====================
void firebaseSendStateHeartbeat() {}

bool giveDose(int pin, float seconds) {
  if (globalEmergencyStop) {
    Serial.println("Pump execution blocked: E-Stop is ACTIVE.");
    return false;
  }
  if (seconds <= 0) return false;

  Serial.printf("[PUMP] START pin=%d seconds=%.2f\n", pin, seconds);

  digitalWrite(pin, HIGH);
  delay((uint32_t)(seconds * 1000.0f));
  digitalWrite(pin, LOW);

  Serial.printf("[PUMP] STOP  pin=%d ran=%.2fs\n", pin, seconds);
  return true;
}

// ===================== TIME =====================
uint64_t getEpochMillis() { return (uint64_t)millis(); }

// ===================== Pending buckets (prefs) =====================
void clearPendingBuckets(const char* why) {
  pendingKalkMl = pendingAfrMl = pendingMgMl = pendingTbdMl = 0.0f;
  Serial.print("Pending buckets CLEARED: ");
  Serial.println(why ? why : "");
}
void initBucketPrefs() {}

// ===================== Dosing prefs (minimal) =====================
void loadDosingFromPrefs() {}
void saveDosingToPrefs() {}
void loadFlowFromPrefs() {}
static void validateFlow(const char* name, float &flow, float fallback) {
  if (!isfinite(flow) || flow < 0.5f || flow > 2000.0f) flow = fallback;
}

// ===================== SELF-TEST =====================
static bool     gSelfTestActive = false;
static uint64_t gSelfTestUntilMs = 0;
static uint64_t gSelfTestLastTickMs = 0;
static uint64_t gSelfTestUntilEpochMs = 0;
static bool     gSelfTestRunOnce = false; // if true, run pumps only once per selfTest enable


static uint32_t SELFTEST_DURATION_MS  = 30UL * 1000UL;// 30s: one pump-run per selfTest command during bench testing
static uint32_t SELFTEST_INTERVAL_MS  = 60UL * 1000UL;
static float    SELFTEST_PUMP_SEC     = 2.0f;
static float    SELFTEST_MIN_DOSE_SEC = 0.20f;

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

  gMinDoseSec = SELFTEST_MIN_DOSE_SEC;

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

// ===================== TEST INPUT (from RTDB) =====================
// PC runner can write test samples here:
//   /devices/<id>/tests/latest = {"ts":123, "ca":440, "alk":8.2, "mg":1350, "ph":8.1, "tbd":0}
// If "ts" is omitted, we fall back to a hash of the JSON.
static uint64_t gLastTestSampleKey = 0;
static int      gAiRanForMode = 0; // when selfTest/modeSweep, allow exactly 1 AI run per mode
static bool     gModeSweepActive = false; // forward decl for pollLatestTestSampleAndMaybeRunAi

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

  const String body = firebaseGetJson("/devices/" + String(DEVICE_ID) + "/tests/latest");
  if (body.length() == 0 || body == "null") return;

  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, body)) return;
  JsonObject o = doc.as<JsonObject>();
  if (o.isNull()) return;

  uint64_t key = 0;
  if (o.containsKey("ts")) key = (uint64_t)o["ts"].as<unsigned long long>();
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

static void selfTestTick() {
  if (!gSelfTestActive) return;
  uint64_t now = (uint64_t)millis();
  if (now >= gSelfTestUntilMs) { disableSelfTest("expired"); return; }
  if (gSelfTestRunOnce && gSelfTestLastTickMs != 0) return; // already ran once
  if (gSelfTestLastTickMs != 0 && (now - gSelfTestLastTickMs) < SELFTEST_INTERVAL_MS) return;
  gSelfTestLastTickMs = now;
  ModeCfg cfg = getModeCfg(gDosingMode);
  giveDose(PIN_PUMP_KALK, SELFTEST_PUMP_SEC);
  if (cfg.keyP2) giveDose(PIN_PUMP_AFR, SELFTEST_PUMP_SEC);
  if (cfg.keyP3) giveDose(PIN_PUMP_MG,  SELFTEST_PUMP_SEC);
  if (cfg.keyP4) giveDose(PIN_PUMP_TBD, SELFTEST_PUMP_SEC);
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
  disableSelfTest("mode sweep done");
  Serial.printf("\n==================== MODE SWEEP STOPPED (%s) ====================\n\n", (why ? why : ""));
}

static void modeSweepInjectTestsForAi(uint8_t mode) {
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
  giveDose(PIN_PUMP_KALK, SELFTEST_PUMP_SEC);
  if (cfg.keyP2) giveDose(PIN_PUMP_AFR, SELFTEST_PUMP_SEC);
  if (cfg.keyP3) giveDose(PIN_PUMP_MG,  SELFTEST_PUMP_SEC);
  if (cfg.keyP4) giveDose(PIN_PUMP_TBD, SELFTEST_PUMP_SEC);
}

static void modeSweepTick() {
  if (!gModeSweepActive) return;
  if ((int32_t)(millis() - gModeSweepNextMs) < 0) return;

  switch (gModeSweepStep) {
    case 0: {
      gDosingMode = clampDosingMode((int)gModeSweepMode);
      saveDosingModeToPrefs(gDosingMode);
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

  if (lastTest.t == 0) {
    updatePumpSchedules();
    return;
  }

  float days = float(currentTest.t - lastTest.t) / 86400.0f;

  // CHANGED: allow AI to run immediately during selfTest/modeSweep
  if (days <= 0.25f && !(gSelfTestActive || gModeSweepActive)) {
    Serial.println("SAFETY: Tests too close together, ignoring for dosing updates.");
    return;
  }
  if (days <= 0.0f) days = 0.0001f;

  // Placeholder: In your full build, this continues with your real AI allocation logic.
  publishAiBreadcrumbs(gAiCurrentSource, (gModeSweepActive ? "modeSweep" : "onNewTestInput"), ca, alk, mg, ph);
  Serial.printf("AI Update: mode=%d ran (test harness).\n", (int)gDosingMode);
}

// ===================== Commands poll (includes new modeSweep) =====================
void firebasePollCommandsFast() {
  if (WiFi.status() != WL_CONNECTED) return;

  const String base = "/devices/" + String(DEVICE_ID) + "/commands";
  const String payload = firebaseGetJson(base);
  if (payload.length() == 0 || payload == "null") return;

  StaticJsonDocument<2048> doc;
  if (deserializeJson(doc, payload)) return;

  JsonObject root = doc.as<JsonObject>();
  if (root.isNull()) return;

  if (root.containsKey("modeSweep")) {
    bool want = root["modeSweep"].is<bool>() ? root["modeSweep"].as<bool>() : false;
    if (want) {
      // Clear the command FIRST to avoid repeated triggers from fast polling.
      firebasePutJson(base + "/modeSweep", "false");
      modeSweepStart("RTDB command");
    }
  }

  if (root.containsKey("selfTest")) {
    bool want = root["selfTest"].is<bool>() ? root["selfTest"].as<bool>() : false;
    if (want) {
      // Clear the command FIRST to avoid repeated triggers from fast polling.
      firebasePutJson(base + "/selfTest", "false");
      enableSelfTest("RTDB command");
    }
  }

// Also watch settings/dosingMode so the PC runner can change modes.
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
  initBucketPrefs();

  if(!wm.autoConnect("ESP32_Config")) {
    Serial.println("WiFi not configured yet.");
  } else {
    Serial.println("WiFi connected.");
  }

  secureClient.setInsecure();
  configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER);

  server.on("/", handleRoot);
  server.begin();
}

void loop(){
  server.handleClient();
  wifiKeepAliveTick();

  if (!gModeSweepActive) selfTestTick();
  modeSweepTick();

  // During bench tests, allow the PC runner to feed samples and force AI once per mode.
  pollLatestTestSampleAndMaybeRunAi();

  static unsigned long lastCmdPollMs  = 0;
  unsigned long nowMs = millis();
  if (nowMs - lastCmdPollMs >= 3000UL) {
    lastCmdPollMs = nowMs;
    firebasePollCommandsFast();
  }
}
