#pragma once
// First-boot wizard: touch calibration (already handled by uiInit() itself,
// since it auto-runs whenever the current rotation has no stored data) ->
// screen orientation -> beep volume -> BLE time sync. Runs once ever, then
// a no-op on every later boot (the "onboarded" flag is in NVS, so it
// survives a reflash -- that's why it doesn't reappear just by reflashing).
void onboardingRunIfNeeded();
// Same 3 steps, unconditional -- for System screen's "Run setup wizard"
// button, so the wizard is still reachable on demand after first boot
// without needing to clear NVS.
void onboardingRunWizard();
