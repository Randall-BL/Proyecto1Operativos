#pragma once

#include "ShipModel.h"

class ShipScheduler {
public:
  void begin();
  void loadDemoManifest();
  void clear();
  void enqueue(const Boat &boat);
  void update();

  const Boat *getActiveBoat() const;
  uint8_t getReadyCount() const;
  const Boat *getReadyBoat(uint8_t index) const;
  uint8_t getWaitingCount(BoatSide side) const;
  const Boat *getWaitingBoat(BoatSide side, uint8_t index) const;
  uint16_t getCompletedLeftToRight() const;
  uint16_t getCompletedRightToLeft() const;
  unsigned long getActiveElapsedMillis() const;

private:
  void startNextBoat();
  void finishActiveBoat();

  Boat readyQueue[MAX_BOATS];
  uint8_t readyCount = 0;
  Boat activeBoat = {};
  bool hasActiveBoat = false;
  unsigned long crossingStartedAt = 0;
  uint16_t completedLeftToRight = 0;
  uint16_t completedRightToLeft = 0;
};