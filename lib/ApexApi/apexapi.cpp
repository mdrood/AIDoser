#include "apexapi.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>

// ------------------------------
// Shared readings struct (yours)
// ------------------------------
struct ApexLatest {
  String date;     // xml date or json epoch string
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
// =====================================================
static bool parseApexStatusJson(const String& payload, ApexLatest& out) {
  if (payload.length() < 10) return false;
    out = ApexLatest(); 
  // If you see memory issues, switch to StaticJsonDocument with a fixed size.
  DynamicJsonDocument doc(64 * 1024); // adjust if needed
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("APEX JSON parse error: %s\n", err.c_str());
    return false;
  }

  JsonVariant istat = doc["istat"];
  if (istat.isNull()) return false;

  // date is epoch in your sample: "date": 1771868644
  if (!istat["date"].isNull()) {
    out.date = String((long)istat["date"]);
  }

  JsonArray inputs = istat["inputs"].as<JsonArray>();
  if (inputs.isNull()) return false;

  float tempTank = NAN, tempSump = NAN;
  float phReef   = NAN, phKalk   = NAN;

  for (JsonObject it : inputs) {
    const char* typeC = it["type"] | "";
    const char* nameC = it["name"] | "";
    float v = it["value"].isNull() ? NAN : (float)it["value"];

    String type(typeC);
    String name(nameC);

    // Temperature
    if (type == "Temp" && isfinite(v)) {
      if (name == "TMP-TK") tempTank = v;
      else if (name == "TMP-SP") tempSump = v;
    }

    // pH
    if (type == "pH" && isfinite(v)) {
      if (name == "ReefPH") phReef = v;
      else if (name.indexOf("kalk") >= 0 || name.indexOf("Kalk") >= 0) phKalk = v;
      else if (!isfinite(phReef)) phReef = v; // fallback
    }

    // Salt/Conductivity (your sample uses Cond + name Salt)
    if (type == "Cond" && name == "Salt" && isfinite(v)) {
      out.cond = v;
    }

    // Trident values
    if (type == "alk" && isfinite(v)) out.alk = v;
    if (type == "ca"  && isfinite(v)) out.ca  = v;
    if (type == "mg"  && isfinite(v)) out.mg  = v;
  }

  out.tempF = isfinite(tempTank) ? tempTank : tempSump;
  out.ph    = isfinite(phReef)   ? phReef   : phKalk;

  // Must have at least temp or pH or something to be "valid"
  return (isfinite(out.tempF) || isfinite(out.ph) || isfinite(out.cond) ||
          isfinite(out.alk) || isfinite(out.ca) || isfinite(out.mg));
}

// =====================================================
// GET STATE: chooses XML or JSON based on apexJson flag
// =====================================================
String ApexApi::getState() {
  String body = "";

  // Your Apex URLs are http:// (NOT https://), so use WiFiClient, not WiFiClientSecure
  WiFiClient client;
  HTTPClient http;

  String url;
  if (apexJson) url = "http://" + apexIp + "/cgi-bin/status.json";
  else          url = "http://" + apexIp + "/cgi-bin/status.xml";

  if (!http.begin(client, url)) {
    Serial.println("http.begin() failed");
    return "";
  }

  int httpCode = http.GET();
  if (httpCode > 0) {
    Serial.printf("HTTP GET... code: %d\n", httpCode);
    body = http.getString();
    // Serial.println("Response: " + body); // can be huge; enable if debugging
  } else {
    Serial.printf("HTTP GET failed, error: %s\n", http.errorToString(httpCode).c_str());
    http.end();
    return "";
  }

  http.end();

  bool ok = false;
  if (apexJson) ok = parseApexStatusJson(body, latest);
  else          ok = parseApexLatestXml(body, latest);

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
    Serial.println(apexJson ? "Failed to parse Apex JSON." : "Failed to parse Apex XML (no record found).");
  }

  return body;
}

// =====================================================
// getters / setters (FIXED)
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