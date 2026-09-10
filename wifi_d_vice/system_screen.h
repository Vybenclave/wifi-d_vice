#pragma once
#include "ui.h"
void systemEnter(); void systemLoop(); void systemTouch(const TouchPoint &t); void systemExit();

// Exposed so the first-boot onboarding wizard can reuse the same pickers
// instead of duplicating them.
void systemShowRotationPicker();
void systemShowVolumePicker();
