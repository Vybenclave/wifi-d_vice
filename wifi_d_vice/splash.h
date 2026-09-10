#pragma once
#include <stdint.h>
// WIFI D_VICE boot splash -- the pixel-art scene, orientation-matched.
// Shown after the boot passphrase (or straight away if no engagement).
// No-ops instantly when splashEnabled() is false (System > Display).
void showSplash(uint32_t holdMs);

// Boot-splash-image on/off, persisted in NVS ("disp"/"splash"). Default on.
bool splashEnabled();
void splashSetEnabled(bool on);

// About-page QR tap: the landscape splash art, aspect-scaled to fit the
// current orientation (shrunk to fit width in 0/90 portrait), centred on
// black. Holds until the screen is tapped. Replaces the Outrun demo.
void showSplashArt();
