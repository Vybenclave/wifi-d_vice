#pragma once
// Blocking full-screen modal: raises a WPA2 SoftAP + a tiny HTTP server
// that lists and serves the SD card, so engagement / wardrive files can be
// pulled onto a phone or laptop. Tap Back or Stop to tear it down and
// return. Reached from a button on the Engagement screen.
void webDownloadRun();
