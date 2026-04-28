#pragma once

#include "ShipModel.h"

class ShipScheduler {
public:
  void begin();
  void loadDemoManifest();
  void clear();
  void enqueue(Boat *boat);
  void update();

  enum Algo { ALG_FCFS = 0, ALG_STRN = 1, ALG_EDF = 2 };
  void setAlgorithm(Algo a) { algorithm = a; }

  const Boat *getActiveBoat() const;
  uint8_t getReadyCount() const;
  const Boat *getReadyBoat(uint8_t index) const;
  uint8_t getWaitingCount(BoatSide side) const;
  const Boat *getWaitingBoat(BoatSide side, uint8_t index) const;
  uint16_t getCompletedLeftToRight() const;
  uint16_t getCompletedRightToLeft() const;
  unsigned long getActiveElapsedMillis() const;

  // Called by boat tasks when they finish
  void notifyBoatFinished(Boat *b);

  // control API for tasks
  void pauseActive();
  void resumeActive();
  void dumpStatus();

private:
  void startNextBoat();
  void finishActiveBoat();

  Boat *readyQueue[MAX_BOATS];
  uint8_t readyCount = 0;
  Boat *activeBoat = nullptr;
  bool hasActiveBoat = false;
  unsigned long crossingStartedAt = 0;
  uint16_t completedLeftToRight = 0;
  uint16_t completedRightToLeft = 0;
  Algo algorithm = ALG_FCFS;
};

// global scheduler pointer for boat tasks
extern ShipScheduler *gScheduler;

// Notification bits for boat tasks
constexpr uint32_t NOTIF_TERMINATE_BIT = (1UL << 1);
// Command values for xTaskNotify value mode
constexpr uint32_t NOTIF_CMD_RUN = 1;
constexpr uint32_t NOTIF_CMD_PAUSE = 2;
constexpr uint32_t NOTIF_CMD_TERMINATE = 3;