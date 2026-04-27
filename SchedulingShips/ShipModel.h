#pragma once

#include <Arduino.h>

enum BoatType : uint8_t {
  BOAT_NORMAL,
  BOAT_PESQUERA,
  BOAT_PATRULLA
};

enum BoatSide : uint8_t {
  SIDE_LEFT,
  SIDE_RIGHT
};

enum BoatState : uint8_t {
  STATE_WAITING,
  STATE_CROSSING,
  STATE_DONE
};

struct Boat {
  uint8_t id;
  BoatType type;
  BoatSide origin;
  unsigned long arrivalOrder;
  unsigned long serviceMillis;
  unsigned long startedAt;
  BoatState state;
};

constexpr uint8_t MAX_BOATS = 16;
constexpr uint8_t VISIBLE_QUEUE = 6;
constexpr unsigned long UI_REFRESH_MS = 200;
constexpr unsigned long CROSSING_MARGIN_MS = 250;

const char *boatTypeName(BoatType type);
const char *boatSideName(BoatSide side);
const char *boatTypeShort(BoatType type);
uint16_t boatColor(BoatType type);
unsigned long serviceTimeForType(BoatType type);
void resetBoatSequence();
Boat makeBoat(BoatSide origin, BoatType type);