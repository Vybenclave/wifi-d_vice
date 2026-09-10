#include "sd_bus.h"
#include "pins.h"

SPIClass sdSPI(HSPI);
static bool started = false;

void sdBusBegin() {
  if (started) return;
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  started = true;
}
