#pragma once
// Runtime-overridable GPIOs for the SPI / IRQ lines you rewire while
// working with add-on radios, plus which add-on radios are installed.
// Everything lives in NVS ("pins" namespace) and is read once at boot into
// effective values; a change needs a reboot to take effect. Holding BOOT at
// power-on clears all overrides.

// The touch IRQ (TP_IRQ) is deliberately NOT here -- a wrong value there
// would take out the only input to the device, and it never needs to move.
enum {
  PIN_CC1101_CS,     // CC1101 sub-GHz chip-select
  PIN_RADIO24_CS,    // 2.4 GHz add-on radio chip-select
  PIN_RADIO24_IRQ,   // 2.4 GHz add-on radio IRQ (nRF24 IRQ / CC2500 GDO0)
  PIN_N
};

void        pincfgLoad();                 // call once at boot, before uiInit()
int         pincfgGet(int id);            // effective GPIO (override or default), -1 = none
int         pincfgDefault(int id);        // the compiled-in default from pins.h
const char *pincfgName(int id);
void        pincfgSet(int id, int gpio);  // persist; pass -1 for "not connected"
void        pincfgResetAll();             // wipe overrides + radio choices, back to defaults

// --- add-on radio presence ---
// A feature whose radio is not marked installed is greyed out in the menus.
// The 2.4 GHz slot holds ONE radio: nRF24L01+ or CC2500, not both.
enum { RADIO24_NONE = 0, RADIO24_NRF24, RADIO24_CC2500 };

bool        pincfgCC1101();               // CC1101 sub-GHz radio installed (default true)
void        pincfgSetCC1101(bool on);
int         pincfgRadio24();              // RADIO24_* (default RADIO24_NONE)
void        pincfgSetRadio24(int which);
const char *pincfgRadio24Name(int which); // "none" / "nRF24L01+" / "CC2500"
