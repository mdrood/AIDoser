#pragma once
#include <Arduino.h>

class ApexApi {
public:
  ApexApi();
  void setIpAddr(String ip);
  String getState();

  float getTempF();
  float getPh();
  float getCond();
  float getAlk();
  float getCa();
  float getMg();

  void setTempF(float v);
  void setPh(float v);
  void setCond(float v);
  void setAlk(float v);
  void setCa(float v);
  void setMg(float v);

  bool apexJson = true;   // set true to use status.json

private:
  String apexIp;
  //String apexIp = "192.168.4.68";
  float tempF = NAN;
  float ph    = NAN;
  float cond  = NAN;
  float alk   = NAN;
  float ca    = NAN;
  float mg    = NAN;
};