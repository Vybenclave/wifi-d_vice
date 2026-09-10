#include "webdl.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <BLEDevice.h>
#include <SD.h>
#include <esp_random.h>
#include "ui.h"
#include "pins.h"
#include "sd_bus.h"

static WebServer server(80);
static DNSServer dns;
static bool  sdReady = false;
static uint32_t hits = 0;

static String humanSize(size_t b) {
  char s[24];
  if (b < 1024)               snprintf(s, sizeof(s), "%u B", (unsigned)b);
  else if (b < 1024UL * 1024) snprintf(s, sizeof(s), "%.1f KB", b / 1024.0);
  else                        snprintf(s, sizeof(s), "%.1f MB", b / 1048576.0);
  return String(s);
}

// %-encode everything that isn't safe in a query value (keep '/').
static String enc(const String &s) {
  static const char *hexd = "0123456789ABCDEF";
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum((unsigned char)c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~')
      o += c;
    else { o += '%'; o += hexd[(c >> 4) & 0xF]; o += hexd[c & 0xF]; }
  }
  return o;
}

static void listInto(const String &path, String &out, int depth) {
  if (depth > 6) return;
  File dir = SD.open(path);
  if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return; }
  for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
    String p = e.path();
    bool isDir = e.isDirectory();
    size_t sz = e.size();
    e.close();
    if (isDir) {
      out += "<li class=d>" + p + "/</li>";
      listInto(p, out, depth + 1);
    } else {
      out += "<li><a href=\"/dl?f=" + enc(p) + "\">" + p + "</a> <span>" + humanSize(sz) + "</span></li>";
    }
  }
  dir.close();
}

static void handleRoot() {
  hits++;
  String h =
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>WIFI D_VICE files</title><style>"
    "body{font:15px system-ui,sans-serif;margin:1.2em;background:#14121c;color:#e8e6f0}"
    "h2{color:#5cd6ff}a{color:#7dcfff;text-decoration:none}a:hover{text-decoration:underline}"
    "ul{list-style:none;padding-left:0}li{margin:.35em 0}li.d{color:#8a86a0;margin-top:.8em}"
    "span{color:#8a86a0;font-size:.85em;margin-left:.5em}</style>"
    "<h2>WIFI D_VICE &mdash; SD card</h2><ul>";
  if (sdReady) listInto("/", h, 0);
  else h += "<li style='color:#ff6b6b'>SD card not found</li>";
  h += "</ul>";
  server.send(200, "text/html", h);
}

static void handleDownload() {
  hits++;
  if (!server.hasArg("f")) { server.send(400, "text/plain", "missing f"); return; }
  String f = server.arg("f");
  File file = SD.open(f, FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) file.close();
    server.send(404, "text/plain", "not found");
    return;
  }
  String name = f.substring(f.lastIndexOf('/') + 1);
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
  server.streamFile(file, "application/octet-stream");
  file.close();
}

// Answer the OS connectivity probes as "online" so the phone doesn't drop
// this network for "no internet" and bounce back to its previous SSID or
// cellular. The user still opens the file list manually in a browser.
static void installCaptiveHandlers() {
  server.on("/generate_204", []() { server.send(204); });   // Android
  server.on("/gen_204",      []() { server.send(204); });
  server.on("/ncsi.txt",     []() { server.send(200, "text/plain", "Microsoft NCSI"); });        // Windows
  server.on("/connecttest.txt", []() { server.send(200, "text/plain", "Microsoft Connect Test"); });
  auto iosOk = []() {                                                                            // iOS / macOS
    server.send(200, "text/html",
                "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
  };
  server.on("/hotspot-detect.html", iosOk);
  server.on("/library/test/success.html", iosOk);
  server.onNotFound([]() {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  });
}

void webDownloadRun() {
  uiDrawTopBar("WiFi download");
  uiClearBelow(29);

  sdBusBegin();
  sdReady = SD.begin(SD_CS, sdSPI);

  // Radio coexistence (see README): the 2FA BLE stack must be fully down
  // before WiFi.mode(WIFI_AP) -- stopAdvertising() alone isn't enough. The
  // caller (engagement.cpp) re-inits with ble2faBegin() after this returns.
  bool bleWasUp = BLEDevice::getInitialized();
  if (bleWasUp) BLEDevice::deinit(false);

  uint8_t mac[6];
  WiFi.macAddress(mac);
  char ssid[24], pass[16];
  snprintf(ssid, sizeof(ssid), "WIFID_VICE-%02X%02X", mac[4], mac[5]);
  snprintf(pass, sizeof(pass), "vice-%06u", (unsigned)(esp_random() % 1000000));

  Serial.printf("[webdl] AP up, free heap %u\n", (unsigned)ESP.getFreeHeap());
  WiFi.persistent(false);       // never write AP creds to NVS (boot-loop guard)
  WiFi.mode(WIFI_AP);
  bool apOk = WiFi.softAP(ssid, pass, 6, 0, 4);   // channel 6, max 4 clients
  WiFi.setSleep(false);         // no modem sleep -> steady beacons
  delay(200);
  IPAddress ip = WiFi.softAPIP();

  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", ip);       // resolve everything here (captive portal)

  hits = 0;
  server.on("/", handleRoot);
  server.on("/dl", handleDownload);
  installCaptiveHandlers();
  server.begin();

  tft.setTextSize(1);
  int y = 40;
  auto row = [&](uint16_t col, const char *k, const String &v) {
    tft.setTextColor(ILI9341_CYAN);  tft.setCursor(6, y);  tft.print(k);
    tft.setTextColor(col);           tft.setCursor(94, y); tft.print(v);
    y += 16;
  };
  row(apOk ? ILI9341_WHITE : ILI9341_RED, "SSID", apOk ? String(ssid) : String("AP FAILED"));
  row(ILI9341_WHITE, "pass",   pass);
  row(ILI9341_GREEN, "URL",    "http://" + ip.toString() + "/");
  row(sdReady ? ILI9341_GREEN : ILI9341_RED, "SD card", sdReady ? "ready" : "not found");
  y += 8;
  tft.setTextColor(ILI9341_YELLOW);
  tft.setCursor(6, y);      tft.print("Join that Wi-Fi (ignore the");
  tft.setCursor(6, y + 14); tft.print("'no internet' prompt), then");
  tft.setCursor(6, y + 28); tft.print("open the URL in a browser.");
  y += 14;

  Btn stop = {8, tft.height() - 42, tft.width() - 16, 32, "stop"};
  uiDrawMenuButton(stop);

  uint32_t lastShown = 0, lastClients = 999;
  for (;;) {
    dns.processNextRequest();
    server.handleClient();

    uint32_t nc = WiFi.softAPgetStationNum();
    if (nc != lastClients || hits != lastShown) {
      lastClients = nc; lastShown = hits;
      tft.fillRect(6, y + 34, tft.width() - 12, 12, ILI9341_BLACK);
      tft.setTextColor(ILI9341_DARKGREY);
      tft.setCursor(6, y + 34);
      tft.printf("clients: %u   served: %u", (unsigned)nc, (unsigned)hits);
    }

    TouchPoint t = uiReadTouch();
    if (t.pressed && (uiTouchInBackButton(t) || uiTouchInButton(t, stop))) {
      uiWaitForRelease();
      break;
    }
    delay(2);
  }

  dns.stop();
  server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  // BLE was fully deinited above -- engagement.cpp calls ble2faBegin()
  // right after this returns to bring the peripheral back for the bond.
}
