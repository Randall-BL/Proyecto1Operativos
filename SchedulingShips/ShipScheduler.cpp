#include "ShipScheduler.h"

void ShipScheduler::begin() {
  clear();
}

void ShipScheduler::clear() {
  readyCount = 0;
  hasActiveBoat = false;
  completedLeftToRight = 0;
  completedRightToLeft = 0;
  crossingStartedAt = 0;
}

void ShipScheduler::enqueue(const Boat &boat) {
  if (readyCount >= MAX_BOATS || getWaitingCount(boat.origin) >= 3) {
    Serial.println("Cola llena; no se agrego el barco.");
    return;
  }

  uint8_t insertAt = readyCount;
  while (insertAt > 0 && readyQueue[insertAt - 1].arrivalOrder > boat.arrivalOrder) {
    readyQueue[insertAt] = readyQueue[insertAt - 1];
    insertAt--;
  }

  readyQueue[insertAt] = boat;
  readyCount++;
}

void ShipScheduler::loadDemoManifest() {
  clear();
  resetBoatSequence();

  enqueue(makeBoat(SIDE_LEFT, BOAT_NORMAL));
  enqueue(makeBoat(SIDE_RIGHT, BOAT_PESQUERA));
  enqueue(makeBoat(SIDE_LEFT, BOAT_PATRULLA));
  enqueue(makeBoat(SIDE_RIGHT, BOAT_NORMAL));
  enqueue(makeBoat(SIDE_LEFT, BOAT_PESQUERA));

  Serial.println("Manifesto FCFS cargado.");
}

void ShipScheduler::startNextBoat() {
  if (readyCount == 0) {
    return;
  }

  activeBoat = readyQueue[0];
  for (uint8_t i = 1; i < readyCount; i++) {
    readyQueue[i - 1] = readyQueue[i];
  }
  readyCount--;

  activeBoat.state = STATE_CROSSING;
  activeBoat.startedAt = millis();
  crossingStartedAt = activeBoat.startedAt;
  hasActiveBoat = true;

  Serial.print("FCFS -> cruza barco #");
  Serial.print(activeBoat.id);
  Serial.print(" (");
  Serial.print(boatTypeName(activeBoat.type));
  Serial.print(", ");
  Serial.print(boatSideName(activeBoat.origin));
  Serial.println(")");
}

void ShipScheduler::finishActiveBoat() {
  if (!hasActiveBoat) {
    return;
  }

  if (activeBoat.origin == SIDE_LEFT) {
    completedLeftToRight++;
  } else {
    completedRightToLeft++;
  }

  activeBoat.state = STATE_DONE;
  hasActiveBoat = false;
  Serial.print("Barco #");
  Serial.print(activeBoat.id);
  Serial.println(" finalizo su cruce.");
}

void ShipScheduler::update() {
  if (hasActiveBoat) {
    unsigned long elapsed = millis() - crossingStartedAt;
    if (elapsed >= activeBoat.serviceMillis + CROSSING_MARGIN_MS) {
      finishActiveBoat();
    }
  }

  if (!hasActiveBoat && readyCount > 0) {
    startNextBoat();
  }
}

const Boat *ShipScheduler::getActiveBoat() const {
  return hasActiveBoat ? &activeBoat : nullptr;
}

uint8_t ShipScheduler::getReadyCount() const {
  return readyCount;
}

const Boat *ShipScheduler::getReadyBoat(uint8_t index) const {
  if (index >= readyCount) {
    return nullptr;
  }

  return &readyQueue[index];
}

uint8_t ShipScheduler::getWaitingCount(BoatSide side) const {
  uint8_t count = 0;
  for (uint8_t i = 0; i < readyCount; i++) {
    if (readyQueue[i].origin == side) {
      count++;
    }
  }

  return count;
}

const Boat *ShipScheduler::getWaitingBoat(BoatSide side, uint8_t index) const {
  uint8_t seen = 0;
  for (uint8_t i = 0; i < readyCount; i++) {
    if (readyQueue[i].origin != side) {
      continue;
    }

    if (seen == index) {
      return &readyQueue[i];
    }

    seen++;
  }

  return nullptr;
}

uint16_t ShipScheduler::getCompletedLeftToRight() const {
  return completedLeftToRight;
}

uint16_t ShipScheduler::getCompletedRightToLeft() const {
  return completedRightToLeft;
}

unsigned long ShipScheduler::getActiveElapsedMillis() const {
  if (!hasActiveBoat) {
    return 0;
  }

  return millis() - crossingStartedAt;
}