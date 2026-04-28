#pragma once

#include "ShipPins.h"
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

#include "ShipScheduler.h"

class ShipDisplay {
public:
  void begin();
  void render(const ShipScheduler &scheduler);
  void renderIfNeeded(const ShipScheduler &scheduler);

private:
  void drawStaticLayout();
  void drawBoatSquare(int16_t x, int16_t y, const Boat &boat, bool highlight = false);
  void drawWaitingSide(const ShipScheduler &scheduler, BoatSide side);
  void drawActiveBoat(const ShipScheduler &scheduler);
  void drawStatistics(const ShipScheduler &scheduler);

  Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);
  unsigned long lastUiRefresh = 0;
  bool layoutDrawn = false;
};