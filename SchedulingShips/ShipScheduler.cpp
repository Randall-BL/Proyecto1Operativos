#include "ShipScheduler.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// global pointer
ShipScheduler *gScheduler = nullptr;

static void boatTask(void *pv) {
  Boat *b = (Boat *)pv;
  if (!b) {
    vTaskDelete(NULL);
    return;
  }

  // loop until remainingMillis reaches zero or termination requested
  bool running = false;
  while (b->remainingMillis > 0) {
    uint32_t cmd = 0;
    // if not running, block until we receive a RUN or TERMINATE command
    if (!running) {
      xTaskNotifyWait(0x00, 0xFFFFFFFF, &cmd, portMAX_DELAY);
      if (cmd == NOTIF_CMD_TERMINATE) break;
      if (cmd == NOTIF_CMD_RUN) running = true;
      continue;
    }

    // when running, do work in small slices and check for commands
    unsigned long step = 200;
    if (step > b->remainingMillis) step = b->remainingMillis;
    unsigned long slept = 0;
    const unsigned long slice = 50;
    while (slept < step) {
      // wait for command for up to 'slice' ms (also acts as the time slice)
      xTaskNotifyWait(0x00, 0xFFFFFFFF, &cmd, pdMS_TO_TICKS(slice));
      if (cmd == NOTIF_CMD_TERMINATE) {
        b->remainingMillis = 0;
        running = false;
        break;
      }
      if (cmd == NOTIF_CMD_PAUSE) {
        running = false;
        break;
      }

      // no command received; count this slice as elapsed time
      unsigned long doSleep = slice;
      if (slept + doSleep > step) doSleep = step - slept;
      slept += doSleep;
    }

    if (b->remainingMillis > step) b->remainingMillis -= step;
    else b->remainingMillis = 0;
  }

  // notify scheduler (may be null in unit tests)
  if (gScheduler) {
    gScheduler->notifyBoatFinished(b);
  }

  // free our own Boat object and delete self
  destroyBoat(b);
  vTaskDelete(NULL);
}

void ShipScheduler::begin() {
  clear();
  gScheduler = this;
}

void ShipScheduler::clear() {
  // request termination for remaining boats/tasks and let them clean up
  for (uint8_t i = 0; i < readyCount; i++) {
    Boat *b = readyQueue[i];
    if (b) {
      if (b->taskHandle) xTaskNotify(b->taskHandle, NOTIF_TERMINATE_BIT, eSetBits);
      else destroyBoat(b);
    }
  }
  readyCount = 0;

  if (hasActiveBoat && activeBoat) {
    if (activeBoat->taskHandle) xTaskNotify(activeBoat->taskHandle, NOTIF_TERMINATE_BIT, eSetBits);
    else destroyBoat(activeBoat);
  }

  activeBoat = nullptr;
  hasActiveBoat = false;
  completedLeftToRight = 0;
  completedRightToLeft = 0;
  crossingStartedAt = 0;
}

void ShipScheduler::enqueue(Boat *boat) {
  if (!boat) return;
  if (readyCount >= MAX_BOATS || getWaitingCount(boat->origin) >= 3) {
    Serial.println("Cola llena; no se agrego el barco.");
    destroyBoat(boat);
    return;
  }

  // insert ordered by arrivalOrder
  uint8_t insertAt = readyCount;
  while (insertAt > 0 && readyQueue[insertAt - 1]->arrivalOrder > boat->arrivalOrder) {
    readyQueue[insertAt] = readyQueue[insertAt - 1];
    insertAt--;
  }

  readyQueue[insertAt] = boat;
  readyCount++;

  // create the FreeRTOS task for this boat (it will wait until receiving RUN)
  xTaskCreate(boatTask, "boat", 4096, boat, 1, &boat->taskHandle);

  // If algorithm is preemptive (STRN or EDF), consider preempting the active boat now
  if (hasActiveBoat && activeBoat) {
    bool shouldPreempt = false;
    if (algorithm == ALG_STRN) {
      if (boat->remainingMillis < activeBoat->remainingMillis) shouldPreempt = true;
    } else if (algorithm == ALG_EDF) {
      if (boat->deadlineMillis < activeBoat->deadlineMillis) shouldPreempt = true;
    }

    if (shouldPreempt) {
      Serial.print("Preemption: barco #"); Serial.print(boat->id); Serial.println(" solicita preemp.");
      // stop active (notify PAUSE)
      if (activeBoat->taskHandle) xTaskNotify(activeBoat->taskHandle, NOTIF_CMD_PAUSE, eSetValueWithOverwrite);
      // re-enqueue active at front
      if (readyCount < MAX_BOATS) {
        for (int i = readyCount; i > 0; i--) readyQueue[i] = readyQueue[i - 1];
        readyQueue[0] = activeBoat;
        readyCount++;
      } else {
        // no space: request active to terminate (safer than immediate destroy)
        if (activeBoat->taskHandle) {
          xTaskNotify(activeBoat->taskHandle, NOTIF_CMD_TERMINATE, eSetValueWithOverwrite);
        } else {
          destroyBoat(activeBoat);
        }
      }

      activeBoat = nullptr;
      hasActiveBoat = false;

      // start next according to algorithm (will pick the best)
      startNextBoat();
    }
  }
}

void ShipScheduler::loadDemoManifest() {
  clear();
  resetBoatSequence();

  enqueue(createBoat(SIDE_LEFT, BOAT_NORMAL));
  enqueue(createBoat(SIDE_RIGHT, BOAT_PESQUERA));
  enqueue(createBoat(SIDE_LEFT, BOAT_PATRULLA));
  enqueue(createBoat(SIDE_RIGHT, BOAT_NORMAL));
  enqueue(createBoat(SIDE_LEFT, BOAT_PESQUERA));

  Serial.println("Manifesto cargado.");
}

static int findIndexForAlgo(ShipScheduler::Algo algo, Boat *readyQueue[], uint8_t readyCount) {
  if (readyCount == 0) return -1;
  int best = 0;
  if (algo == ShipScheduler::ALG_FCFS) return 0;
  if (algo == ShipScheduler::ALG_STRN) {
    unsigned long minRem = readyQueue[0]->remainingMillis;
    for (uint8_t i = 1; i < readyCount; i++) {
      if (readyQueue[i]->remainingMillis < minRem) {
        minRem = readyQueue[i]->remainingMillis;
        best = i;
      }
    }
    return best;
  }
  if (algo == ShipScheduler::ALG_EDF) {
    unsigned long minDead = readyQueue[0]->deadlineMillis;
    for (uint8_t i = 1; i < readyCount; i++) {
      if (readyQueue[i]->deadlineMillis < minDead) {
        minDead = readyQueue[i]->deadlineMillis;
        best = i;
      }
    }
    return best;
  }
  return 0;
}

void ShipScheduler::startNextBoat() {
  if (readyCount == 0) return;

  int idx = findIndexForAlgo(algorithm, readyQueue, readyCount);
  if (idx < 0) return;

  Boat *b = readyQueue[idx];
  // remove from queue
  for (uint8_t i = idx + 1; i < readyCount; i++) readyQueue[i - 1] = readyQueue[i];
  readyCount--;

  // handle preemption for STRN
  if (algorithm == ALG_STRN && hasActiveBoat && activeBoat) {
    if (activeBoat->remainingMillis > b->remainingMillis) {
      // preempt active
      activeBoat->allowedToMove = false;
      // re-enqueue active at front
      // place active at position 0
      for (int i = readyCount; i > 0; i--) readyQueue[i] = readyQueue[i - 1];
      readyQueue[0] = activeBoat;
      readyCount++;
      hasActiveBoat = false;
      activeBoat = nullptr;
    }
  }

  // start b: set state and notify task to run
  b->state = STATE_CROSSING;
  b->startedAt = millis();
  crossingStartedAt = b->startedAt;
  activeBoat = b;
  hasActiveBoat = true;
  if (b->taskHandle) xTaskNotify(b->taskHandle, NOTIF_CMD_RUN, eSetValueWithOverwrite);

  Serial.print("Start -> barco #"); Serial.print(b->id); Serial.println();
}

void ShipScheduler::finishActiveBoat() {
  if (!hasActiveBoat || !activeBoat) return;

  Boat *b = activeBoat;
  if (b->origin == SIDE_LEFT) completedLeftToRight++; else completedRightToLeft++;
  b->state = STATE_DONE;
  // task that finished will destroy its own Boat and self-delete.
  // just clear scheduler's reference.
  activeBoat = nullptr;
  hasActiveBoat = false;
  Serial.print("Barco finalizado\n");
}

void ShipScheduler::update() {
  // check if active finished by remainingMillis==0
  if (hasActiveBoat && activeBoat) {
    if (activeBoat->remainingMillis == 0) {
      finishActiveBoat();
    }
  }

  if (!hasActiveBoat && readyCount > 0) {
    startNextBoat();
  }
}

const Boat *ShipScheduler::getActiveBoat() const { return hasActiveBoat ? activeBoat : nullptr; }

uint8_t ShipScheduler::getReadyCount() const { return readyCount; }

const Boat *ShipScheduler::getReadyBoat(uint8_t index) const {
  if (index >= readyCount) return nullptr;
  return readyQueue[index];
}

uint8_t ShipScheduler::getWaitingCount(BoatSide side) const {
  uint8_t count = 0;
  for (uint8_t i = 0; i < readyCount; i++) if (readyQueue[i]->origin == side) count++;
  return count;
}

const Boat *ShipScheduler::getWaitingBoat(BoatSide side, uint8_t index) const {
  uint8_t seen = 0;
  for (uint8_t i = 0; i < readyCount; i++) {
    if (readyQueue[i]->origin != side) continue;
    if (seen == index) return readyQueue[i];
    seen++;
  }
  return nullptr;
}

uint16_t ShipScheduler::getCompletedLeftToRight() const { return completedLeftToRight; }
uint16_t ShipScheduler::getCompletedRightToLeft() const { return completedRightToLeft; }
unsigned long ShipScheduler::getActiveElapsedMillis() const { if (!hasActiveBoat || !activeBoat) return 0; return millis() - crossingStartedAt; }

void ShipScheduler::notifyBoatFinished(Boat *b) {
  // ensure this is activeBoat
  if (hasActiveBoat && activeBoat == b) {
    finishActiveBoat();
  } else {
    // remove from readyQueue if present
    for (uint8_t i = 0; i < readyCount; i++) {
      if (readyQueue[i] == b) {
        // remove from ready queue; the task that finished will free its memory
        for (uint8_t j = i + 1; j < readyCount; j++) readyQueue[j - 1] = readyQueue[j];
        readyCount--;
        return;
      }
    }
  }
}

void ShipScheduler::pauseActive() {
  if (hasActiveBoat && activeBoat) {
    if (activeBoat->taskHandle) xTaskNotify(activeBoat->taskHandle, NOTIF_CMD_PAUSE, eSetValueWithOverwrite);
    Serial.print("Pausado barco #"); Serial.println(activeBoat->id);
  } else {
    Serial.println("No hay barco activo para pausar.");
  }
}

void ShipScheduler::resumeActive() {
  if (hasActiveBoat && activeBoat) {
    if (activeBoat->taskHandle) xTaskNotify(activeBoat->taskHandle, NOTIF_CMD_RUN, eSetValueWithOverwrite);
    Serial.print("Reanundado barco #"); Serial.println(activeBoat->id);
  } else {
    Serial.println("No hay barco activo para reanudar.");
  }
}

void ShipScheduler::dumpStatus() {
  Serial.println("--- Scheduler Status ---");
  Serial.print("Algorithm: "); Serial.println(algorithm == ALG_FCFS ? "FCFS" : algorithm == ALG_STRN ? "STRN" : "EDF");
  Serial.print("Ready count: "); Serial.println(readyCount);
  for (uint8_t i = 0; i < readyCount; i++) {
    Boat *b = readyQueue[i];
    Serial.print(i); Serial.print(": #"); Serial.print(b->id); Serial.print(" "); Serial.print(boatTypeShort(b->type)); Serial.print(" from "); Serial.print(boatSideName(b->origin)); Serial.print(" rem="); Serial.println(b->remainingMillis);
  }
  if (hasActiveBoat && activeBoat) {
    Serial.print("Active: #"); Serial.print(activeBoat->id); Serial.print(" rem="); Serial.println(activeBoat->remainingMillis);
  } else {
    Serial.println("Active: none");
  }
  Serial.println("------------------------");
}