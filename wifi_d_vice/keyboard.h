#pragma once
#include <Arduino.h>
// Blocking modal on-screen keyboard -- the resistive touchscreen has no
// native text input, so every text field (Engagement Page etc.) goes
// through this. Returns the entered string, or `initial` unchanged if the
// user hits CANCEL.
String uiTextInput(const char *prompt, const String &initial = "", bool mask = false);
