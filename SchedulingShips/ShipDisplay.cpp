#include "ShipDisplay.h"

constexpr uint16_t SHIP_DARK_GREY = 0x7BEF;
constexpr uint16_t SHIP_NAVY = 0x000F;
constexpr uint8_t SCREEN_W = 128;
constexpr uint8_t SCREEN_H = 160;
constexpr uint8_t HEADER_H = 16;
constexpr uint8_t FOOTER_H = 14;
constexpr uint8_t SIDE_PANEL_W = 30;
constexpr uint8_t CANAL_X = SIDE_PANEL_W;
constexpr uint8_t CANAL_W = SCREEN_W - (SIDE_PANEL_W * 2);
constexpr uint8_t CANAL_Y = HEADER_H;
constexpr uint8_t CANAL_H = SCREEN_H - HEADER_H - FOOTER_H;
constexpr uint8_t BOAT_SIZE = 12;
constexpr uint8_t SLOT_Y[3] = {30, 60, 90};

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
  tft.setRotation(0);
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
  tft.setCursor(2, 18);
  tft.print("IZQ");
  tft.setCursor(45, 18);
  tft.print("CANAL");
  tft.setCursor(99, 18);
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
  int16_t panelX = side == SIDE_LEFT ? 3 : 113;
  tft.fillRect(panelX - 1, 26, 14, 94, ST77XX_BLACK);

  uint8_t waitingCount = scheduler.getWaitingCount(side);
  uint8_t visibleCount = waitingCount > 3 ? 3 : waitingCount;

  for (uint8_t i = 0; i < visibleCount; i++) {
    const Boat *boat = scheduler.getWaitingBoat(side, i);
    if (boat == nullptr) {
      continue;
    }

    drawBoatSquare(panelX, SLOT_Y[i], *boat, false);
  }

  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setCursor(panelX - 1, 122);
  if (waitingCount == 0) {
    tft.print("0");
  } else {
    tft.print(waitingCount);
  }
}

void ShipDisplay::drawActiveBoat(const ShipScheduler &scheduler) {
  tft.fillRect(CANAL_X + 1, HEADER_H + 1, CANAL_W - 2, CANAL_H - 2, 0x18E3);
  tft.fillRect(CANAL_X + 1, HEADER_H + 1, CANAL_W - 2, 12, ST77XX_BLACK);

  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setCursor(42, 20);
  tft.print("Canal");

  const Boat *activeBoat = scheduler.getActiveBoat();
  if (activeBoat != nullptr) {
    unsigned long elapsed = scheduler.getActiveElapsedMillis();
    unsigned long remaining = activeBoat->serviceMillis > elapsed ? activeBoat->serviceMillis - elapsed : 0;

    int16_t travelStart = activeBoat->origin == SIDE_RIGHT ? CANAL_X + CANAL_W - BOAT_SIZE - 2 : CANAL_X + 2;
    int16_t travelEnd = activeBoat->origin == SIDE_RIGHT ? CANAL_X + 2 : CANAL_X + CANAL_W - BOAT_SIZE - 2;
    int16_t boatX = mapProgress(elapsed, activeBoat->serviceMillis, travelStart, travelEnd);
    int16_t boatY = HEADER_H + (CANAL_H / 2) - (BOAT_SIZE / 2);

    drawBoatSquare(boatX, boatY, *activeBoat, true);

    tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    tft.setCursor(36, 137);
    tft.print(boatTypeName(activeBoat->type));
    tft.print(" #");
    tft.print(activeBoat->id);
    tft.print(" ");
    tft.print(boatSideName(activeBoat->origin));
    tft.print(" ");
    tft.print(remaining / 1000);
    tft.print('s');
  }
}

void ShipDisplay::drawStatistics(const ShipScheduler &scheduler) {
  tft.fillRect(0, 146, tft.width(), 14, SHIP_NAVY);
  tft.setTextColor(ST77XX_GREEN, ST77XX_BLACK);
  tft.setCursor(4, 150);
  tft.print("I->D:");
  tft.print(scheduler.getCompletedLeftToRight());
  tft.setCursor(58, 150);
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