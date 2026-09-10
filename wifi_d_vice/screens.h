#pragma once
// Each feature module exposes Enter/Loop/Touch. Enter() is called once when
// the menu switches into that screen (draw static chrome, (re)start
// scanners). Loop() runs every main-loop iteration while that screen is
// active. Touch() gets every touch sample (back-button handling is done by
// the caller in wireless_multitool.ino before these are reached).
#include "ui.h"

// Exit() is called once when the menu switches away from that screen (radio
// teardown etc.) -- default to a no-op by defining a weak stub per module
// that only overrides it when it actually needs one.
// *HandleBack() (where present): the top-bar back button is offered to the
// module first -- return true if it stepped up an internal sub-view and the
// screen should stay open; false to let the main loop pop out of the screen.
void wifiScanEnter();  void wifiScanLoop();  void wifiScanTouch(const TouchPoint &t);  void wifiScanExit();
bool wifiScanHandleBack();
// Post-connect shortcut buttons: wifi_scan stashes one of these, the main
// loop drains it and switches screens. WSJUMP_NONE (0) = nothing pending.
enum { WSJUMP_NONE = 0, WSJUMP_NETSTATS_SPEED, WSJUMP_NETSTATS_CONN };
int wifiScanTakePendingJump();
void netstatsEnter();  void netstatsLoop();  void netstatsTouch(const TouchPoint &t);  void netstatsExit();
bool netstatsHandleBack();
void netstatsGoSpeedTest();   // jump straight into the speed-test sub-page
void netstatsGoConn();        // jump straight into the connection + NAT sub-page
void bleScanEnter();   void bleScanLoop();   void bleScanTouch(const TouchPoint &t);   void bleScanExit();
bool bleScanHandleBack();
void trackerEnter();   void trackerLoop();   void trackerTouch(const TouchPoint &t);   void trackerExit();
bool trackerHandleBack();
void widsEnter();      void widsLoop();      void widsTouch(const TouchPoint &t);      void widsExit();
void flockEnter();     void flockLoop();     void flockTouch(const TouchPoint &t);     void flockExit();
void skimmerEnter();   void skimmerLoop();   void skimmerTouch(const TouchPoint &t);   void skimmerExit();
void subghzEnter();    void subghzLoop();    void subghzTouch(const TouchPoint &t);    void subghzExit();
// Recon category (Marauder / Wireless Wizard / Flipper feature parity, all passive).
void probeWatchEnter(); void probeWatchLoop(); void probeWatchTouch(const TouchPoint &t); void probeWatchExit();
void clientMapEnter();  void clientMapLoop();  void clientMapTouch(const TouchPoint &t);  void clientMapExit();
void cameraEnter();     void cameraLoop();     void cameraTouch(const TouchPoint &t);     void cameraExit();
void droneEnter();      void droneLoop();      void droneTouch(const TouchPoint &t);      void droneExit();
void bleSpamEnter();    void bleSpamLoop();    void bleSpamTouch(const TouchPoint &t);    void bleSpamExit();
void meshEnter();      void meshLoop();      void meshTouch(const TouchPoint &t);      void meshExit();
bool meshHandleBack();
void gpsEnter();       void gpsLoop();       void gpsTouch(const TouchPoint &t);       void gpsExit();
void engagementEnter(); void engagementLoop(); void engagementTouch(const TouchPoint &t); void engagementExit();
void rogueEnter();    void rogueLoop();    void rogueTouch(const TouchPoint &t);    void rogueExit();
bool rogueHandleBack();
bool widsTakeJumpToRogue();   // WiFi IDS "no baseline" prompt chose "Go to Rogue AP"
