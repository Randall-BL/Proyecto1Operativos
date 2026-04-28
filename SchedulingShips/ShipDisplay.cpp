#include "ShipDisplay.h"

constexpr uint16_t SHIP_DARK_GREY = 0x7BEF;
constexpr uint16_t SHIP_NAVY = 0x000F;
// Rotate display to landscape: swap width/height
constexpr uint8_t SCREEN_W = 160;
constexpr uint8_t SCREEN_H = 128;
constexpr uint8_t HEADER_H = 12;
constexpr uint8_t FOOTER_H = 14;
constexpr uint8_t SIDE_PANEL_W = 30;
constexpr uint8_t CANAL_X = SIDE_PANEL_W;
constexpr uint8_t CANAL_W = SCREEN_W - (SIDE_PANEL_W * 2);
constexpr uint8_t CANAL_Y = HEADER_H;
constexpr uint8_t CANAL_H = SCREEN_H - HEADER_H - FOOTER_H;
constexpr uint8_t BOAT_SIZE = 12;
constexpr uint8_t LABEL_Y = HEADER_H + 2;
constexpr uint8_t FOOTER_Y = SCREEN_H - FOOTER_H;
constexpr uint8_t WAIT_AREA_Y = CANAL_Y + 12;
constexpr uint8_t WAIT_SLOT_SPACING = 24;
constexpr uint8_t WAIT_COUNT_Y = CANAL_Y + CANAL_H - 12;
constexpr uint8_t INFO_H = 10;
constexpr uint8_t INFO_Y = FOOTER_Y - INFO_H - 1;
constexpr uint8_t PANEL_INSET_X = 2;

static int16_t mapProgress(unsigned long elapsed, unsigned long total, int16_t from, int16_t to) {
  if (total == 0) {
    return to;
  }

  long delta = (long)to - (long)from;
  return from + (int16_t)((delta * (long)elapsed) / (long)total);
}

void ShipDisplay::begin() {
  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  pinMode(TFT_RST, OUTPUT);
  digitalWrite(TFT_RST, HIGH);
  delay(10);

  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1); // rotate 90 degrees to landscape
  tft.fillScreen(ST77XX_BLACK);
}

void ShipDisplay::drawStaticLayout() {
  tft.fillScreen(ST77XX_BLACK);

  tft.fillRect(0, 0, SCREEN_W, HEADER_H, SHIP_NAVY);
  tft.setTextWrap(true);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setCursor(8, 4);
  tft.print("Scheduling Ships - FCFS");

  tft.fillRect(0, HEADER_H, SIDE_PANEL_W, CANAL_H, ST77XX_BLACK);
  tft.fillRect(CANAL_X, HEADER_H, CANAL_W, CANAL_H, 0x18E3);
  tft.fillRect(SCREEN_W - SIDE_PANEL_W, HEADER_H, SIDE_PANEL_W, CANAL_H, ST77XX_BLACK);

  tft.drawRect(0, HEADER_H, SIDE_PANEL_W, CANAL_H, ST77XX_BLUE);
  tft.drawRect(CANAL_X, HEADER_H, CANAL_W, CANAL_H, ST77XX_WHITE);
  tft.drawRect(SCREEN_W - SIDE_PANEL_W, HEADER_H, SIDE_PANEL_W, CANAL_H, ST77XX_BLUE);

  tft.setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
  tft.setCursor(PANEL_INSET_X, LABEL_Y);
  tft.print("IZQ");
  tft.setCursor(CANAL_X + 6, LABEL_Y);
  tft.print("CANAL");
  tft.setCursor(SCREEN_W - SIDE_PANEL_W + PANEL_INSET_X, LABEL_Y);
  tft.print("DER");

  layoutDrawn = true;
}

void ShipDisplay::drawBoatSquare(int16_t x, int16_t y, const Boat &boat, bool highlight) {
  uint16_t color = boatColor(boat.type);
  tft.fillRect(x, y, BOAT_SIZE, BOAT_SIZE, color);
  tft.drawRect(x, y, BOAT_SIZE, BOAT_SIZE, highlight ? ST77XX_WHITE : SHIP_DARK_GREY);
  tft.setTextColor(highlight ? ST77XX_WHITE : ST77XX_BLACK, color);
  tft.setCursor(x + 3, y + 3);
  tft.print(boatTypeShort(boat.type));
}

void ShipDisplay::drawWaitingSide(const ShipScheduler &scheduler, BoatSide side) {
  int16_t panelX = side == SIDE_LEFT ? PANEL_INSET_X : (SCREEN_W - SIDE_PANEL_W + PANEL_INSET_X);
  tft.fillRect(panelX - 1, WAIT_AREA_Y - 2, BOAT_SIZE + 4, CANAL_H - 18, ST77XX_BLACK);

  uint8_t waitingCount = scheduler.getWaitingCount(side);
  uint8_t visibleCount = waitingCount > 3 ? 3 : waitingCount;

  for (uint8_t i = 0; i < visibleCount; i++) {
    const Boat *boat = scheduler.getWaitingBoat(side, i);
    if (boat == nullptr) {
      continue;
    }

    int16_t slotY = WAIT_AREA_Y + (WAIT_SLOT_SPACING * i);
    drawBoatSquare(panelX, slotY, *boat, false);
  }

  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setCursor(panelX - 1, WAIT_COUNT_Y);
  if (waitingCount == 0) {
    tft.print("0");
  } else {
    tft.print(waitingCount);
  }
}

void ShipDisplay::drawActiveBoat(const ShipScheduler &scheduler) {
  tft.fillRect(CANAL_X + 1, CANAL_Y + 1, CANAL_W - 2, CANAL_H - 2, 0x18E3);
  tft.fillRect(CANAL_X + 1, CANAL_Y + 1, CANAL_W - 2, 12, ST77XX_BLACK);

  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setCursor(CANAL_X + 6, CANAL_Y + 3);
  tft.print("Canal");

  const Boat *activeBoat = scheduler.getActiveBoat();
  if (activeBoat != nullptr) {
    unsigned long elapsed = scheduler.getActiveElapsedMillis();
    unsigned long remaining = activeBoat->serviceMillis > elapsed ? activeBoat->serviceMillis - elapsed : 0;

    int16_t travelStart = activeBoat->origin == SIDE_RIGHT ? CANAL_X + CANAL_W - BOAT_SIZE - 2 : CANAL_X + 2;
    int16_t travelEnd = activeBoat->origin == SIDE_RIGHT ? CANAL_X + 2 : CANAL_X + CANAL_W - BOAT_SIZE - 2;
    int16_t boatX = mapProgress(elapsed, activeBoat->serviceMillis, travelStart, travelEnd);
    int16_t minX = CANAL_X + 2;
    int16_t maxX = CANAL_X + CANAL_W - BOAT_SIZE - 2;
    if (boatX < minX) boatX = minX;
    if (boatX > maxX) boatX = maxX;
    int16_t boatY = CANAL_Y + (CANAL_H / 2) - (BOAT_SIZE / 2);

    drawBoatSquare(boatX, boatY, *activeBoat, true);

    tft.fillRect(CANAL_X + 2, INFO_Y - 1, CANAL_W - 4, INFO_H + 2, 0x18E3);
    tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    tft.setTextWrap(false);
    tft.setCursor(CANAL_X + 4, INFO_Y);
    tft.print(boatTypeShort(activeBoat->type));
    tft.print('#');
    tft.print(activeBoat->id);
    tft.print(' ');
    tft.print(boatSideName(activeBoat->origin));
    tft.print(' ');
    tft.print(remaining / 1000);
    tft.print('s');
    tft.setTextWrap(true);
  } else {
    // clear info strip when no active boat
    tft.fillRect(CANAL_X + 2, INFO_Y - 1, CANAL_W - 4, INFO_H + 2, 0x18E3);
  }
}

void ShipDisplay::drawStatistics(const ShipScheduler &scheduler) {
  tft.fillRect(0, FOOTER_Y, SCREEN_W, FOOTER_H, SHIP_NAVY);
  tft.setTextColor(ST77XX_GREEN, ST77XX_BLACK);
  tft.setCursor(4, FOOTER_Y + 3);
  tft.print("I->D:");
  tft.print(scheduler.getCompletedLeftToRight());
  tft.setCursor((SCREEN_W / 2) + 4, FOOTER_Y + 3);
  tft.print("D->I:");
  tft.print(scheduler.getCompletedRightToLeft());
}

void ShipDisplay::render(const ShipScheduler &scheduler) {
  if (!layoutDrawn) {
    drawStaticLayout();
  }

  drawActiveBoat(scheduler);
  drawWaitingSide(scheduler, SIDE_LEFT);
  drawWaitingSide(scheduler, SIDE_RIGHT);
  drawStatistics(scheduler);
}

void ShipDisplay::renderIfNeeded(const ShipScheduler &scheduler) {
  if (millis() - lastUiRefresh < UI_REFRESH_MS) {
    return;
  }

  lastUiRefresh = millis();
  render(scheduler);
}