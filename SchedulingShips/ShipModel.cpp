#include <Adafruit_ST7735.h>

#include "ShipModel.h"

static uint8_t nextBoatId = 1;
static unsigned long nextArrivalOrder = 1;

void resetBoatSequence() {
  nextBoatId = 1;
  nextArrivalOrder = 1;
}

const char *boatTypeName(BoatType type) {
  switch (type) {
    case BOAT_NORMAL: return "Normal";
    case BOAT_PESQUERA: return "Pesquera";
    case BOAT_PATRULLA: return "Patrulla";
  }
  return "Desconocido";
}

const char *boatSideName(BoatSide side) {
  return side == SIDE_LEFT ? "Izq" : "Der";
}

const char *boatTypeShort(BoatType type) {
  switch (type) {
    case BOAT_NORMAL: return "N";
    case BOAT_PESQUERA: return "P";
    case BOAT_PATRULLA: return "U";
  }
  return "?";
}

uint16_t boatColor(BoatType type) {
  switch (type) {
    case BOAT_NORMAL: return ST77XX_WHITE;
    case BOAT_PESQUERA: return ST77XX_CYAN;
    case BOAT_PATRULLA: return ST77XX_RED;
  }
  return ST77XX_YELLOW;
}

unsigned long serviceTimeForType(BoatType type) {
  switch (type) {
    case BOAT_NORMAL: return 6500;
    case BOAT_PESQUERA: return 4500;
    case BOAT_PATRULLA: return 3000;
  }
  return 5000;
}
Boat *createBoat(BoatSide origin, BoatType type) {
  Boat *boat = new Boat();
  boat->id = nextBoatId++;
  boat->type = type;
  boat->origin = origin;
  boat->arrivalOrder = nextArrivalOrder++;
  boat->serviceMillis = serviceTimeForType(type);
  boat->startedAt = 0;
  boat->state = STATE_WAITING;
  boat->taskHandle = NULL;
  boat->remainingMillis = boat->serviceMillis;
  // default deadline: now + 2 * service time (can be overridden)
  boat->deadlineMillis = millis() + (boat->serviceMillis * 2UL);
  return boat;
}

void destroyBoat(Boat *b) {
  if (!b) return;
  delete b;
}