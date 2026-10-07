#pragma once
#include <stdint.h>

// Show the power menu. Sleep wakes on a touchscreen tap. Power off wakes on the RESET button.
// The system checks TP_IRQ directly to avoid false wakes.
void powerShowMenu();   // System screen's power icon -- picks a tier

void powerShowDisplayTimeoutSettings();

// Sleep breaks the context. The system returns to the main menu.
// The function uses a pending-jump pattern. The main loop drains the jump.
bool powerTakePendingWake();

// Enforce the screen timeout. The display blanks after inactivity.
// The screen un-blanks on the next touch.
void powerDisplayTimeoutLoad();
void powerNoteActivity();
void powerServiceAutoOff();

// Reset the idle clock for `ms` milliseconds. The function prevents the screen from blanking.
// It does not wake an already-blanked screen.
void powerWakeFor(uint32_t ms);
