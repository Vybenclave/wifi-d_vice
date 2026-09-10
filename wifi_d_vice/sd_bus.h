#pragma once
#include <SPI.h>
// Dedicated SPI bus for the SD card, kept separate from the LCD's global
// `SPI` bus so neither driver fights over pin remapping. (Originally also
// shared with a PN532 on a second CS line -- NFC/RFID was dropped, see
// skimmer_detect.cpp -- but this stays its own SPIClass instance in case a
// future addon wants to share the bus again.)
extern SPIClass sdSPI;
void sdBusBegin();   // idempotent
