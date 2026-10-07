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
#include "accent.h"
#include "debuglog.h"
#include "ble_2fa.h"
#include "engstore.h"
#include "power.h"
#include <vector>

static WebServer server(80);
static DNSServer dns;
static bool  sdReady = false;
static uint32_t hits = 0;
static String s_clientRoot;
static String s_accentHex;
static bool s_apOk;
static String s_ssid, s_pass, s_url;
static bool s_sdReady;
static Btn s_stopBtn;   // The button state lives in a static variable. drawApInfo() sets it. webDownloadRun() reads it. Both call sites need access.
static bool s_bulkApproved = false;   // This flag resets to false at the start of each session.

static String humanSize(size_t b) {
  char s[24];
  if (b < 1024)               snprintf(s, sizeof(s), "%u B", (unsigned)b);
  else if (b < 1024UL * 1024) snprintf(s, sizeof(s), "%.1f KB", b / 1024.0);
  else                        snprintf(s, sizeof(s), "%.1f MB", b / 1048576.0);
  return String(s);
}

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

static String htmlEscape(const String &s) {
  String o;
  o.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      default:  o += c;
    }
  }
  return o;
}

static String rgb565ToHex(uint16_t c) {
  uint8_t r5 = (c >> 11) & 0x1F, g6 = (c >> 5) & 0x3F, b5 = c & 0x1F;
  uint8_t r8 = (r5 << 3) | (r5 >> 2);
  uint8_t g8 = (g6 << 2) | (g6 >> 4);
  uint8_t b8 = (b5 << 3) | (b5 >> 2);
  char buf[8];
  snprintf(buf, sizeof(buf), "#%02X%02X%02X", r8, g8, b8);
  return String(buf);
}

static void sendSecurityHeaders() {
  server.sendHeader("X-Content-Type-Options", "nosniff");
  // The page uses an inline style block. The CSP rule requires unsafe-inline. The browser strips all rules without it. No inline scripts exist. The script rule stays at the default.
  server.sendHeader("Content-Security-Policy", "default-src 'self'; style-src 'self' 'unsafe-inline'");
  server.sendHeader("X-Frame-Options", "DENY");
}

static void buildClientFiles(const String &root, String &out) {
  File dir = SD.open(root);
  if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return; }

  struct FInfo { String name; bool isDir; size_t size; };
  std::vector<FInfo> items;
  for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
    items.push_back({e.name(), e.isDirectory(), e.size()});
    e.close();
  }
  dir.close();

  std::vector<FInfo> files, dirs;
  for (const auto &it : items) {
    if (it.isDir) dirs.push_back(it);
    else files.push_back(it);
  }

  if (!files.empty()) {
    out += "<div class='group'><h2>Files</h2><div class='files'>";
    for (const auto &f : files) {
      out += "<div class='file'><a href=\"/dl?f=" + enc(root + "/" + f.name) + "\">" + htmlEscape(f.name) + "</a><span class='sz'>" + humanSize(f.size) + "</span></div>";
    }
    out += "</div></div>";
  }

  for (const auto &d : dirs) {
    String subPath = root + "/" + d.name;
    out += "<div class='group'><h2>" + htmlEscape(d.name) + "</h2><div class='files'>";
    File subDir = SD.open(subPath);
    if (subDir && subDir.isDirectory()) {
      for (File e = subDir.openNextFile(); e; e = subDir.openNextFile()) {
        out += "<div class='file'><a href=\"/dl?f=" + enc(subPath + "/" + e.name()) + "\">" + htmlEscape(e.name()) + "</a><span class='sz'>" + humanSize(e.size()) + "</span></div>";
        e.close();
      }
      subDir.close();
    }
    out += "</div></div>";
  }
}

static void drawApInfo(bool apOk, const char *ssid, const char *pass, const String &url, bool sdReady) {
  uiDrawTopBar("WiFi download");
  uiClearBelow(29);
  tft.setTextSize(1);
  int y = 40;
  auto row = [&](uint16_t col, const char *k, const String &v) {
    tft.setTextColor(accentLabel());  tft.setCursor(6, y);  tft.print(k);
    tft.setTextColor(col);           tft.setCursor(94, y); tft.print(v);
    y += 16;
  };
  row(apOk ? ILI9341_WHITE : ILI9341_RED, "SSID", apOk ? String(ssid) : String("AP FAILED"));
  row(ILI9341_WHITE, "pass",   pass);
  row(ILI9341_GREEN, "URL",    url);
  row(sdReady ? ILI9341_GREEN : ILI9341_RED, "SD card", sdReady ? "ready" : "not found");
  y += 8;
  tft.setTextColor(ILI9341_YELLOW);
  tft.setCursor(6, y);      tft.print("Join that Wi-Fi (ignore the");
  tft.setCursor(6, y + 14); tft.print("'no internet' prompt), then");
  tft.setCursor(6, y + 28); tft.print("open the URL in a browser.");
  y += 14;
  Btn stop = {8, tft.height() - 42, tft.width() - 16, 32, "stop"};
  uiDrawMenuButton(stop);
  s_stopBtn = stop;
}

static bool confirmDownloadOnDevice(const String &filename) {
  uiClearBelow(0);
  tft.setTextColor(ILI9341_YELLOW);
  tft.setTextSize(2);
  tft.setCursor(8, 40);
  tft.print("Download request");
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(1);
  tft.setCursor(8, 76);
  tft.print("Request:");
  tft.setCursor(8, 92);
  tft.print(filename.length() > 40 ? filename.substring(0, 37) + "..." : filename);
  Btn allow = {8, 140, tft.width() - 16, 34, "Allow"};
  Btn deny  = {8, 184, tft.width() - 16, 34, "Deny"};
  uiDrawMenuButton(allow);
  uiDrawMenuButton(deny);
  bool result = false;
  bool decided = false;
  while (!decided) {
    TouchPoint t = uiReadTouch();
    if (t.pressed) powerNoteActivity();
    powerServiceAutoOff();
    if (t.pressed) {
      if (uiTouchInButton(t, allow)) { uiWaitForRelease(); result = true; decided = true; }
      else if (uiTouchInButton(t, deny) || uiTouchInBackButton(t)) { uiWaitForRelease(); result = false; decided = true; }
    }
    delay(15);
  }
  return result;
}

static void handleBulkUnlock() {
  hits++;
  sendSecurityHeaders();
  if (!s_bulkApproved) {
    s_bulkApproved = confirmDownloadOnDevice("allow ALL downloads this session (no more prompts)");
  }
  drawApInfo(s_apOk, s_ssid.c_str(), s_pass.c_str(), s_url, s_sdReady);
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

static void handleRoot() {
  hits++;
  sendSecurityHeaders();
  if (s_clientRoot == "/") {
    server.send(200, "text/html",
      "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
      "<title>WIFI D_VICE</title><style>"
      "body{font:15px -apple-system,system-ui,sans-serif;margin:0;padding:30px 20px;background:#0b0a14;color:#eef0fa}"
      "h2{color:" + s_accentHex + "}</style>"
      "<h2>No client selected</h2>");
    return;
  }
  String h =
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>WIFI D_VICE files</title><style>"
    ":root{"
    "--accent:" + s_accentHex + ";"
    "--bg:#0b0a14;"
    "--panel:color-mix(in srgb, var(--bg), white 7%);"
    "--panel-2:color-mix(in srgb, var(--bg), white 12%);"
    "--line:color-mix(in srgb, var(--bg), white 20%);"
    "--ink:#eef0fa;"
    "--ink-dim:color-mix(in srgb, var(--ink), var(--bg) 42%);"
    "--glow:color-mix(in srgb, var(--accent), transparent 80%);"
    "}"
    "*{box-sizing:border-box}html,body{margin:0}"
    "body{background:var(--bg);color:var(--ink);font:15px/1.5 -apple-system,system-ui,\"Segoe UI\",Roboto,sans-serif;padding:30px 20px 64px;min-height:100vh}"
    ".glow{position:fixed;inset:0;z-index:-1;pointer-events:none;background:"
      "radial-gradient(360px 260px at 0% 0%, var(--glow), transparent 70%),"
      "radial-gradient(300px 220px at 100% 0%, var(--glow), transparent 70%)}"
    ".wrap{max-width:880px;margin:0 auto}"
    ".eyebrow{font:600 11px/1 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;letter-spacing:.18em;text-transform:uppercase;color:var(--accent);margin:0 0 10px}"
    "h1{font-size:clamp(22px,4.2vw,32px);font-weight:800;letter-spacing:-.01em;margin:0 0 6px;"
      "background:linear-gradient(95deg,color-mix(in srgb,var(--accent),white 55%),var(--accent) 65%);"
      "-webkit-background-clip:text;background-clip:text;color:transparent}"
    ".sub{color:var(--ink-dim);font-size:13.5px;margin:0 0 30px;max-width:48ch}"
    ".bulk{display:inline-flex;align-items:center;gap:8px;background:var(--panel);border:1px solid var(--line);color:var(--ink-dim);font-size:13px;text-decoration:none;border-radius:999px;padding:9px 16px;margin-bottom:26px}"
    ".bulk:hover{border-color:var(--accent);color:var(--accent)}"
    ".bulk.on{border-color:var(--accent);color:var(--accent);background:var(--panel-2)}"
    ".bulk .dot{width:7px;height:7px;border-radius:50%;background:currentColor}"
    ".group{margin-bottom:24px}"
    ".group h2{font-size:12px;letter-spacing:.08em;text-transform:uppercase;color:var(--ink-dim);font-weight:600;margin:0 0 10px;display:flex;align-items:center;gap:8px}"
    ".group h2::before{content:'';width:6px;height:6px;border-radius:50%;background:var(--accent);box-shadow:0 0 6px var(--accent)}"
    ".files{display:grid;grid-template-columns:repeat(auto-fit,minmax(240px,1fr));gap:8px}"
    ".file{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:11px 14px;"
      "display:flex;align-items:center;justify-content:space-between;gap:10px;min-width:0;"
      "transition:border-color .15s,background .15s}"
    ".file:hover{border-color:var(--accent);background:var(--panel-2)}"
    ".file a{color:var(--ink);text-decoration:none;font-size:13.5px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;min-width:0}"
    ".file:hover a{color:var(--accent)}"
    ".sz{font:12px ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;color:var(--ink-dim);background:var(--panel-2);"
      "border-radius:999px;padding:3px 9px;white-space:nowrap;font-variant-numeric:tabular-nums;flex:none}"
    "</style>"
    "<div class='glow'></div><div class='wrap'>"
    "<div class='eyebrow'>// wifi_d_vice &middot; engagement download</div>"
    "<h1>" + htmlEscape(s_clientRoot) + "</h1>"
    "<p class='sub'>Tap a file to request it &mdash; every download needs an on-device approval before it streams.</p>"
    + (s_bulkApproved
      ? "<a class='bulk on' href='/dl-unlock'><span class='dot'></span>bulk downloads: unlocked for this session</a>"
      : "<a class='bulk' href='/dl-unlock'><span class='dot'></span>enable bulk downloads (skip per-file approval)</a>");
  buildClientFiles(s_clientRoot, h);
  h += "</div>";
  server.send(200, "text/html", h);
}

static void handleDownload() {
  hits++;
  sendSecurityHeaders();
  if (s_clientRoot == "/") { server.send(403, "text/plain", "forbidden"); return; }
  if (!server.hasArg("f")) { server.send(400, "text/plain", "missing f"); return; }
  String f = server.arg("f");
  if (f.indexOf("..") >= 0) { server.send(403, "text/plain", "forbidden"); return; }
  if (!f.startsWith(s_clientRoot + "/")) { server.send(403, "text/plain", "forbidden"); return; }

  if (!s_bulkApproved && !confirmDownloadOnDevice(f)) {
    server.send(403, "text/plain", "denied");
    drawApInfo(s_apOk, s_ssid.c_str(), s_pass.c_str(), s_url, s_sdReady);
    return;
  }

  File file = SD.open(f, FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) file.close();
    server.send(404, "text/plain", "not found");
    drawApInfo(s_apOk, s_ssid.c_str(), s_pass.c_str(), s_url, s_sdReady);
    return;
  }
  String name = f.substring(f.lastIndexOf('/') + 1);
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
  server.streamFile(file, "application/octet-stream");
  file.close();
  drawApInfo(s_apOk, s_ssid.c_str(), s_pass.c_str(), s_url, s_sdReady);
}

// These handlers answer OS connectivity probes. The phone stays on this network. The user opens the file list manually.
static void installCaptiveHandlers() {
  server.on("/generate_204", []() { sendSecurityHeaders(); server.send(204); });
  server.on("/gen_204",      []() { sendSecurityHeaders(); server.send(204); });
  server.on("/ncsi.txt",     []() { sendSecurityHeaders(); server.send(200, "text/plain", "Microsoft NCSI"); });
  server.on("/connecttest.txt", []() { sendSecurityHeaders(); server.send(200, "text/plain", "Microsoft Connect Test"); });
  auto iosOk = []() {
    sendSecurityHeaders();
    server.send(200, "text/html",
                "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
  };
  server.on("/hotspot-detect.html", iosOk);
  server.on("/library/test/success.html", iosOk);
  server.onNotFound([]() {
    sendSecurityHeaders();
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  });
}

void webDownloadRun() {
  uiDrawTopBar("WiFi download");
  uiClearBelow(29);

  sdBusBegin();
  sdReady = SD.begin(SD_CS, sdSPI);

  // The BLE stack must stop before WiFi starts. stopAdvertising() alone fails. ble2faEnd() clears internal pointers. This prevents dangling references.
  ble2faEnd();

  uint8_t mac[6];
  WiFi.macAddress(mac);
  char ssid[24], pass[16];
  snprintf(ssid, sizeof(ssid), "WIFID_VICE-%02X%02X", mac[4], mac[5]);
  snprintf(pass, sizeof(pass), "vice-%06u", (unsigned)(esp_random() % 1000000));

  DLOG("webdl", "AP up, free heap %u", (unsigned)ESP.getFreeHeap());
  WiFi.persistent(false);       // This prevents the system from writing credentials to NVS. It avoids a boot loop.
  WiFi.mode(WIFI_AP);
  bool apOk = WiFi.softAP(ssid, pass, 6, 0, 4);   // channel 6, max 4 clients
  WiFi.setSleep(false);         // no modem sleep -> steady beacons
  delay(200);
  IPAddress ip = WiFi.softAPIP();

  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", ip);       // resolve everything here (captive portal)

  hits = 0;
  s_bulkApproved = false;
  server.on("/", handleRoot);
  server.on("/dl", handleDownload);
  server.on("/dl-unlock", handleBulkUnlock);
  installCaptiveHandlers();
  server.begin();

  s_clientRoot = "/" + engStoreCurrentClient();
  s_accentHex = rgb565ToHex(accentFill());
  s_apOk = apOk;
  s_ssid = String(ssid);
  s_pass = String(pass);
  s_url = "http://" + ip.toString() + "/";
  s_sdReady = sdReady;

  drawApInfo(s_apOk, s_ssid.c_str(), s_pass.c_str(), s_url, s_sdReady);

  uint32_t lastShown = 0, lastClients = 999;
  for (;;) {
    dns.processNextRequest();
    server.handleClient();

    uint32_t nc = WiFi.softAPgetStationNum();
    if (nc != lastClients || hits != lastShown) {
      lastClients = nc; lastShown = hits;
      uiClearRect(6, 160, tft.width() - 12, 12);
      tft.setTextColor(ILI9341_DARKGREY);
      tft.setCursor(6, 160);
      tft.printf("clients: %u   served: %u", (unsigned)nc, (unsigned)hits);
    }

    TouchPoint t = uiReadTouch();
    if (t.pressed) powerNoteActivity();
    powerServiceAutoOff();
    if (t.pressed && (uiTouchInBackButton(t) || uiTouchInButton(t, s_stopBtn))) {
      uiWaitForRelease();
      break;
    }
    delay(2);
  }

  dns.stop();
  server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  // The BLE stack stops here. The caller restarts it after this function returns. This prepares the peripheral for bonding.
}
