#include "apexapi.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>

// ------------------------------
// Shared readings struct (yours)
// ------------------------------
struct ApexLatest {
  String date;     // xml date string or json epoch string
  float tempF = NAN;
  float ph    = NAN;
  float alk   = NAN;
  float ca    = NAN;
  float cond  = NAN;   // salt/cond reading from Apex
  float mg    = NAN;
};

static ApexLatest latest;

// ------------------------------
// ctor / config
// ------------------------------
ApexApi::ApexApi() {}

void ApexApi::setIpAddr(String ip) {
  apexIp = ip;
}

// =====================================================
// XML PARSER (your existing code, unchanged)
// =====================================================

bool extractTagValue(const String& src, const String& openTag, const String& closeTag, int from, String& out) {
  int a = src.indexOf(openTag, from);
  if (a < 0) return false;
  a += openTag.length();
  int b = src.indexOf(closeTag, a);
  if (b < 0) return false;
  out = src.substring(a, b);
  out.trim();
  return true;
}

bool getLastRecordBlock(const String& xml, String& recordOut) {
  int start = xml.lastIndexOf("<record>");
  if (start < 0) return false;
  int end = xml.indexOf("</record>", start);
  if (end < 0) return false;
  end += String("</record>").length();
  recordOut = xml.substring(start, end);
  return true;
}

// Finds <probe> ... <name>PROBE_NAME</name> ... <value>XYZ</value> ... </probe>
bool getProbeValueByName(const String& record, const String& probeName, float& outVal) {
  int pos = 0;
  while (true) {
    int p0 = record.indexOf("<probe>", pos);
    if (p0 < 0) return false;
    int p1 = record.indexOf("</probe>", p0);
    if (p1 < 0) return false;

    String probeBlock = record.substring(p0, p1);
    String name;
    if (extractTagValue(probeBlock, "<name>", "</name>", 0, name)) {
      if (name == probeName) {
        String valStr;
        if (!extractTagValue(probeBlock, "<value>", "</value>", 0, valStr)) return false;
        outVal = valStr.toFloat();
        return true;
      }
    }
    pos = p1 + 8; // len("</probe>")
  }
}

static bool parseApexLatestXml(const String& xml, ApexLatest& out) {
  String record;
  if (!getLastRecordBlock(xml, record)) return false;

  String date;
  if (extractTagValue(record, "<date>", "</date>", 0, date)) out.date = date;

  // Use YOUR probe names
  getProbeValueByName(record, "Tmp",    out.tempF);
  getProbeValueByName(record, "SumppH", out.ph);
  getProbeValueByName(record, "Salt",   out.cond);
  getProbeValueByName(record, "Alkx3",  out.alk);
  getProbeValueByName(record, "Cax3",   out.ca);
  getProbeValueByName(record, "Mgx3",   out.mg);

  return true;
}

// =====================================================
// JSON PARSER for /cgi-bin/status.json
// Supports BOTH shapes:
//  A) { "istat": { "date":..., "inputs":[...] } }
//  B) { "system":{ "date":... }, "inputs":[...], ... }   <-- your REST payload
// =====================================================
static bool parseApexStatusJson(const String& payload, ApexLatest& out) {
  if (payload.length() < 10) return false;
  out = ApexLatest();

  // If you see memory issues, switch to StaticJsonDocument with a fixed size.
  DynamicJsonDocument doc(96 * 1024); // bumped a bit for big payloads
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("APEX JSON parse error: %s\n", err.c_str());
    return false;
  }

  // Detect where the inputs array lives
  JsonArray inputs;
  String dateStr;

  // Shape A
  JsonVariant istat = doc["istat"];
  if (!istat.isNull()) {
    if (!istat["date"].isNull()) dateStr = String((long)istat["date"]);
    inputs = istat["inputs"].as<JsonArray>();
  }

  // Shape B (REST)
  if (inputs.isNull()) {
    JsonVariant sys = doc["system"];
    if (!sys.isNull() && !sys["date"].isNull()) dateStr = String((long)sys["date"]);
    inputs = doc["inputs"].as<JsonArray>();
  }

  if (inputs.isNull()) return false;
  if (dateStr.length()) out.date = dateStr;

  // We’ll prefer your known names if present, but also accept “first of type” fallbacks.
  float tempFound = NAN;
  float phFound   = NAN;

  for (JsonObject it : inputs) {
    const char* typeC = it["type"] | "";
    const char* nameC = it["name"] | "";
    float v = it["value"].isNull() ? NAN : (float)it["value"];

    if (!isfinite(v)) continue;

    String type(typeC);
    String name(nameC);

    // Temp
    if (type == "Temp") {
      if (name == "Tmp" || name == "TMP" || name.indexOf("Tmp") >= 0) tempFound = v;
      else if (!isfinite(tempFound)) tempFound = v; // fallback: first Temp
    }

    // pH
    if (type == "pH") {
      if (name == "SumppH" || name.indexOf("Sump") >= 0) phFound = v;
      else if (!isfinite(phFound)) phFound = v; // fallback: first pH
    }

    // Conductivity / salinity
    if (type == "Cond") {
      if (name == "Salt" || name.indexOf("Salt") >= 0) out.cond = v;
      else if (!isfinite(out.cond)) out.cond = v; // fallback: first Cond
    }

    // Trident
    if (type == "alk") out.alk = v;
    if (type == "ca")  out.ca  = v;
    if (type == "mg")  out.mg  = v;
  }

  out.tempF = tempFound;
  out.ph    = phFound;

  // Must have at least one meaningful value to call it valid
  return (isfinite(out.tempF) || isfinite(out.ph) || isfinite(out.cond) ||
          isfinite(out.alk) || isfinite(out.ca) || isfinite(out.mg));
}

// =====================================================
// HTTP helper
// =====================================================
static bool httpGetBody(const String& url, String& bodyOut) {
  bodyOut = "";
  WiFiClient client;
  HTTPClient http;

  if (!http.begin(client, url)) {
    Serial.println("http.begin() failed");
    return false;
  }

  // keep it snappy so fallback doesn’t stall your loop
  http.setTimeout(2500);

  int httpCode = http.GET();
  if (httpCode <= 0) {
    Serial.printf("HTTP GET failed (%s): %s\n", url.c_str(), http.errorToString(httpCode).c_str());
    http.end();
    return false;
  }

  if (httpCode != 200) {
    Serial.printf("HTTP GET %s -> %d\n", url.c_str(), httpCode);
    http.end();
    return false;
  }

  bodyOut = http.getString();
  http.end();
  return bodyOut.length() > 0;
}

// =====================================================
// GET STATE with fallback:
// - If apexJson==true:  try JSON first, then XML
// - If apexJson==false: try XML first, then JSON
// =====================================================
String ApexApi::getState() {
  if (apexIp.length() == 0) {
    Serial.println("ApexApi: apexIp not set");
    return "";
  }

  const String urlJson = "http://" + apexIp + "/cgi-bin/status.json";
  const String urlXml  = "http://" + apexIp + "/cgi-bin/status.xml";

  auto tryJson = [&](String& body) -> bool {
    if (!httpGetBody(urlJson, body)) return false;
    bool ok = parseApexStatusJson(body, latest);
    if (!ok) Serial.println("Apex JSON fetched but parse/values invalid");
    return ok;
  };

  auto tryXml = [&](String& body) -> bool {
    if (!httpGetBody(urlXml, body)) return false;
    bool ok = parseApexLatestXml(body, latest);
    if (!ok) Serial.println("Apex XML fetched but parse/values invalid");
    return ok;
  };

  String body = "";
  bool ok = false;

  // Prefer based on flag, but ALWAYS fallback to the other.
  if (apexJson) {
    ok = tryJson(body);
    if (!ok) ok = tryXml(body);
  } else {
    ok = tryXml(body);
    if (!ok) ok = tryJson(body);
  }

  if (ok) {
    Serial.println("Apex date: " + latest.date);
    Serial.printf("TempF=%.2f pH=%.2f Cond=%.2f Alk=%.2f Ca=%.1f Mg=%.0f\n",
                  latest.tempF, latest.ph, latest.cond, latest.alk, latest.ca, latest.mg);

    setTempF(latest.tempF);
    setPh(latest.ph);
    setCond(latest.cond);
    setAlk(latest.alk);
    setCa(latest.ca);
    setMg(latest.mg);

    Serial.printf("CLASS: TempF=%.2f pH=%.2f Cond=%.2f Alk=%.2f Ca=%.1f Mg=%.0f\n",
                  getTempF(), getPh(), getCond(), getAlk(), getCa(), getMg());
  } else {
    Serial.println("Failed to fetch/parse Apex via JSON and XML.");
    body = "";
  }

  return body;
}

// =====================================================
// getters / setters (your fixed ones)
// =====================================================
float ApexApi::getTempF(){ return tempF; }
float ApexApi::getPh(){ return ph; }
float ApexApi::getCond(){ return cond; }
float ApexApi::getAlk(){ return alk; }
float ApexApi::getCa(){ return ca; }
float ApexApi::getMg(){ return mg; }

void ApexApi::setTempF(float v){ tempF = v; }
void ApexApi::setPh(float v){ ph = v; }
void ApexApi::setCond(float v){ cond = v; }
void ApexApi::setAlk(float v){ alk = v; }
void ApexApi::setCa(float v){ ca = v; }
void ApexApi::setMg(float v){ mg = v; }