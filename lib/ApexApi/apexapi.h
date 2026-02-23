#ifndef APEXAPI___h
#define APEXAPI___h
#include <Arduino.h>

class ApexApi
{
    private:
        const char* WIFI_SSID = "roods";
        const char* WIFI_PASS = "Frinov25!+!";
        String apexIp = "";
        float tempF = 0;
        float  ph  = 0;
        float cond = 0;
        float alk = 0;
        float ca = 0;
        float mg = 0;
        //ApexLatest latest;

        
    public: 
        ApexApi(); 
        float getTempF();
        float getPh();
        float getCond();
        float getAlk();
        float getCa();
        float getMg();
        void setTempF(float tempF_);
        void setPh(float ph_);
        void setCond(float cond_);
        void setAlk(float alk_);
        void setCa(float ca_);
        void setMg(float mg_);

        String getState();
        void connectWiFi();
        void setIpAddr(String ip);
        static bool extractTagValue(const String& src, const String& openTag, const String& closeTag, int from, String& out);
        static bool getLastRecordBlock(const String& xml, String& recordOut);
        static bool getProbeValueByName(const String& record, const String& probeName, float& outVal);

};
#endif