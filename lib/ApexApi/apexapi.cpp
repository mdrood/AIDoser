#include "apexapi.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

        struct ApexLatest {
            String date;     // "02/22/2026 00:04:00"
            float tempF = NAN;
            float ph    = NAN;
            float alk   = NAN;
            float ca    = NAN;
            float cond  = NAN;   // salt/cond reading from Apex
            float mg    = NAN;
        };
        ApexLatest latest;

ApexApi::ApexApi() {}

void ApexApi::setIpAddr(String ip){
    apexIp = ip;
}

////////////////////////////xml parser ///////////////////////////////


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

static bool parseApexLatest(const String& xml, ApexLatest& out) {
  String record;
  if (!getLastRecordBlock(xml, record)) return false;

  // record date
  String date;
  if (extractTagValue(record, "<date>", "</date>", 0, date)) out.date = date;

  // --- IMPORTANT ---
  // Use YOUR Apex probe names here (these come from <name> in the XML)
  // Based on the sample you uploaded:
  // Temp: "Tmp" (77.1)
  // pH: "SumppH" (8.00)  [there are other pH probes too: "pH14ft", "CaRXPH"]
  // Cond: "Salt" (34.9)
  // Alk: "Alkx3" (6.83)
  // Ca:  "Cax3"  (456)
  // Mg:  "Mgx3"  (1351)

  getProbeValueByName(record, "Tmp",    out.tempF);
  getProbeValueByName(record, "SumppH", out.ph);
  getProbeValueByName(record, "Salt",   out.cond);
  getProbeValueByName(record, "Alkx3",  out.alk);
  getProbeValueByName(record, "Cax3",   out.ca);
  getProbeValueByName(record, "Mgx3",   out.mg);

  return true;
}
///////////////////////////////////////////////////////////////////////



String ApexApi::getState() {
    String xml = "";
    WiFiClientSecure client;
    client.setInsecure(); // quick test; for production, use root CA

    HTTPClient https;
    // URL to call
    String url = "http://"+apexIp+"/cgi-bin/status.xml"; // Make sure to use ".xml" instead of ".xlm"

    if (!https.begin(client, url)) {
        Serial.println("https.begin() failed");
        return "";
    }

    // Send the request
    int httpCode = https.GET(); // Use GET() to perform the request

    // Check for successful response
    if (httpCode > 0) {
        // HTTP header has been sent and Server response header has been handled
        Serial.printf("HTTP GET... code: %d\n", httpCode);
        
        // Get the response payload
        xml = https.getString();
        Serial.println("Response: " + xml);
    } else {
        Serial.printf("HTTP GET failed, error: %s\n", https.errorToString(httpCode).c_str());
    }

    // Close connection
    https.end();
    if (parseApexLatest(xml, latest)) {
  Serial.println("Latest record date: " + latest.date);
  Serial.printf("TempF=%.2f pH=%.2f Cond=%.2f Alk=%.2f Ca=%.1f Mg=%.0f\n",
                latest.tempF, latest.ph, latest.cond, latest.alk, latest.ca, latest.mg);
    setTempF(latest.tempF);
    setPh(latest.ph);
    setCond(latest.cond);
    setAlk(latest.alk);
    setCa(latest.ca);
    setMg(latest.mg);

  // then write to RTDB however you do it:
  // devices/<deviceId>/sensors/tempF, pH, cond, alk, ca, mg, etc.
} else {
  Serial.println("Failed to parse Apex XML (no record found).");
}
    return xml; // Return the XML response
}

// ===================== WiFi =====================
void ApexApi::connectWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true);
    delay(100);

    Serial.println("Connecting to WiFi...");
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
        if (millis() - start > 20000) {
            Serial.println("\nWiFi connect timeout!");
            return;
        }
    }

    Serial.println("\nWiFi connected");
    Serial.print("IP: ");  Serial.println(WiFi.localIP());
    Serial.print("GW: ");  Serial.println(WiFi.gatewayIP());
    Serial.print("DNS: "); Serial.println(WiFi.dnsIP());

    // If you ever hit DNS issues again, uncomment:
    // WiFi.setDNS(IPAddress(8, 8, 8, 8), IPAddress(1, 1, 1, 1));
}

float ApexApi::getTempF(){
    return tempF;
}
float ApexApi::getPh(){
    return ph;
}
float ApexApi::getCond(){
    return cond;
}
float ApexApi::getAlk(){
    return alk;
}
float ApexApi::getCa(){
    return ca;
}
float ApexApi::getMg(){
    return mg;
}
void ApexApi::setTempF(float tempF_){
    tempF_ = tempF;
}
void ApexApi::setPh(float ph_){
    ph_ = ph;
}
void ApexApi::setCond(float cond_){
    cond_ = cond;
}
void ApexApi::setAlk(float alk_){
    alk_ = alk;
}
void ApexApi::setCa(float ca_){
    ca_ = ca;
}
void ApexApi::setMg(float mg_){
    mg_ = mg;
}