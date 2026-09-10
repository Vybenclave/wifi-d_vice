#include "wifiauto.h"
#include <WiFi.h>
#include <Preferences.h>

static const char *NS = "wifiauto";

void wifiAutoStore(const String &ssid, const String &pass) {
  Preferences p;
  p.begin(NS, false);
  p.putString("ssid", ssid);
  p.putString("pass", pass);
  p.putBool("on", true);
  p.end();
}

void wifiAutoClear() {
  Preferences p;
  p.begin(NS, false);
  p.clear();
  p.end();
}

bool wifiAutoEnabled() {
  Preferences p;
  p.begin(NS, true);
  bool on = p.getBool("on", false) && p.getString("ssid", "").length() > 0;
  p.end();
  return on;
}

String wifiAutoSSID() {
  Preferences p;
  p.begin(NS, true);
  String s = p.getString("ssid", "");
  p.end();
  return s;
}

bool wifiAutoIsFor(const String &ssid) {
  return wifiAutoEnabled() && wifiAutoSSID() == ssid;
}

bool wifiAutoConnectOnBoot() {
  Preferences p;
  p.begin(NS, true);
  bool on = p.getBool("on", false);
  String ssid = p.getString("ssid", "");
  String pass = p.getString("pass", "");
  p.end();
  if (!on || ssid.length() == 0) return false;

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  if (pass.length()) WiFi.begin(ssid.c_str(), pass.c_str());
  else               WiFi.begin(ssid.c_str());
  Serial.printf("[wifiauto] boot connect -> %s\n", ssid.c_str());
  return true;
}
