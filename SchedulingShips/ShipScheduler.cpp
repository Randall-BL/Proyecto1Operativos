#include "ShipScheduler.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Puntero global para que las tareas reporten finalizacion al scheduler.
ShipScheduler *gScheduler = nullptr;

// Tarea de FreeRTOS que simula el avance del barco en rebanadas de tiempo.
static void boatTask(void *pv) {
  Boat *b = (Boat *)pv;
  if (!b) {
    vTaskDelete(NULL);
    return;
  }

  // Ciclo principal: termina cuando remainingMillis llega a cero o se solicita terminar.
  bool running = false;
  while (b->remainingMillis > 0) {
    uint32_t cmd = 0;
    // Si no esta corriendo, bloquea hasta recibir RUN o TERMINATE.
    if (!running) {
      xTaskNotifyWait(0x00, 0xFFFFFFFF, &cmd, portMAX_DELAY);
      if (cmd == NOTIF_CMD_TERMINATE) break;
      if (cmd == NOTIF_CMD_RUN) running = true;
      continue;
    }

    // Cuando esta corriendo, avanza por rebanadas pequenas y revisa comandos.
    unsigned long step = 200;
    if (step > b->remainingMillis) step = b->remainingMillis;
    unsigned long slept = 0;
    const unsigned long slice = 50;
    while (slept < step) {
      // Espera comandos por 'slice' ms (tambien actua como quantum interno).
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

      // Sin comando: cuenta este tramo como tiempo transcurrido.
      unsigned long doSleep = slice;
      if (slept + doSleep > step) doSleep = step - slept;
      slept += doSleep;
    }

    if (b->remainingMillis > step) b->remainingMillis -= step;
    else b->remainingMillis = 0;
  }

  // Notifica al scheduler (puede ser null en pruebas unitarias).
  if (gScheduler) {
    gScheduler->notifyBoatFinished(b);
  }

  // Libera el Boat propio y elimina la tarea actual.
  destroyBoat(b);
  vTaskDelete(NULL);
}

void ShipScheduler::begin() {
  // Limpia el estado y registra el puntero global.
  clear();
  gScheduler = this;
}

void ShipScheduler::clear() {
  // Solicita terminar boats/tareas restantes y deja que limpien recursos.
  ignoreCompletions = true;
  for (uint8_t i = 0; i < readyCount; i++) {
    Boat *b = readyQueue[i];
    if (b) {
      b->cancelled = true;
      if (b->taskHandle) xTaskNotify(b->taskHandle, NOTIF_CMD_TERMINATE, eSetValueWithOverwrite);
      else destroyBoat(b);
    }
  }
  readyCount = 0;

  // Termina el barco activo si existe.
  if (hasActiveBoat && activeBoat) {
    activeBoat->cancelled = true;
    if (activeBoat->taskHandle) xTaskNotify(activeBoat->taskHandle, NOTIF_CMD_TERMINATE, eSetValueWithOverwrite);
    else destroyBoat(activeBoat);
  }

  // Reinicia contadores de ejecucion.
  activeBoat = nullptr;
  hasActiveBoat = false;
  completedLeftToRight = 0;
  completedRightToLeft = 0;
  completedTotal = 0;
  totalWaitMillis = 0;
  totalTurnaroundMillis = 0;
  totalServiceMillis = 0;
  completionCount = 0;
  crossingStartedAt = 0;
}

void ShipScheduler::enqueue(Boat *boat) {
  // Inserta un barco en la cola de listos y evalua preempcion.
  if (!boat) return;
  ignoreCompletions = false;
  if (readyCount >= MAX_BOATS || getWaitingCount(boat->origin) >= 3) {
    Serial.println("Cola llena; no se agrego el barco.");
    destroyBoat(boat);
    return;
  }

  boat->cancelled = false;
  if (boat->enqueuedAt == 0) boat->enqueuedAt = millis();
  boat->state = STATE_WAITING;

  // Inserta ordenado por arrivalOrder para desempate FCFS.
  uint8_t insertAt = readyCount;
  while (insertAt > 0 && readyQueue[insertAt - 1]->arrivalOrder > boat->arrivalOrder) {
    readyQueue[insertAt] = readyQueue[insertAt - 1];
    insertAt--;
  }

  readyQueue[insertAt] = boat;
  readyCount++;

  // Crea la tarea de FreeRTOS para este barco (espera RUN para iniciar).
  xTaskCreate(boatTask, "boat", 4096, boat, 1, &boat->taskHandle);

  // Si el algoritmo es preemptivo, evalua si debe sacar al activo.
  if (hasActiveBoat && activeBoat) {
    bool shouldPreempt = false;
    if (algorithm == ALG_STRN) {
      if (boat->remainingMillis < activeBoat->remainingMillis) shouldPreempt = true;
    } else if (algorithm == ALG_EDF) {
      if (boat->deadlineMillis < activeBoat->deadlineMillis) shouldPreempt = true;
    } else if (algorithm == ALG_PRIORITY) {
      if (boat->priority > activeBoat->priority) shouldPreempt = true;
    }

    if (shouldPreempt) {
      Serial.print("Preemption: barco #"); Serial.print(boat->id); Serial.println(" solicita preemp.");
      // Detiene al activo (PAUSE) y lo reencola.
      if (activeBoat->taskHandle) xTaskNotify(activeBoat->taskHandle, NOTIF_CMD_PAUSE, eSetValueWithOverwrite);
      Boat *preempted = activeBoat;
      activeBoat = nullptr;
      hasActiveBoat = false;
      requeueBoat(preempted, true);

      // Inicia el siguiente segun el algoritmo (elige el mejor candidato).
      startNextBoat();
    }
  }
}

void ShipScheduler::loadDemoManifest() {
  // Encola un manifiesto fijo para demos rapidas.
  clear();
  resetBoatSequence();

  enqueue(createBoat(SIDE_LEFT, BOAT_NORMAL));
  enqueue(createBoat(SIDE_RIGHT, BOAT_PESQUERA));
  enqueue(createBoat(SIDE_LEFT, BOAT_PATRULLA));
  enqueue(createBoat(SIDE_RIGHT, BOAT_NORMAL));
  enqueue(createBoat(SIDE_LEFT, BOAT_PESQUERA));

  Serial.println("Manifesto cargado.");
}

const char *ShipScheduler::getAlgorithmLabel() const {
  // Etiqueta corta usada por la interfaz y los logs.
  switch (algorithm) {
    case ALG_FCFS: return "FCFS";
    case ALG_STRN: return "STRN";
    case ALG_EDF: return "EDF";
    case ALG_RR: return "RR";
    case ALG_PRIORITY: return "PRIO";
    case ALG_SJF: return "SJF";
  }
  return "?";
}

void ShipScheduler::setRoundRobinQuantum(unsigned long quantumMillis) {
  // Limita el quantum RR a un minimo para evitar valores muy bajos.
  if (quantumMillis < 100) quantumMillis = 100;
  rrQuantumMillis = quantumMillis;
}

// Elige el siguiente barco listo segun el algoritmo activo.
static int findIndexForAlgo(ShipScheduler::Algo algo, Boat *readyQueue[], uint8_t readyCount) {
  if (readyCount == 0) return -1;
  int best = 0;
  if (algo == ShipScheduler::ALG_FCFS) return 0;
  if (algo == ShipScheduler::ALG_RR) return 0;
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
  if (algo == ShipScheduler::ALG_PRIORITY) {
    uint8_t bestPriority = readyQueue[0]->priority;
    unsigned long bestArrival = readyQueue[0]->arrivalOrder;
    for (uint8_t i = 1; i < readyCount; i++) {
      uint8_t candidatePriority = readyQueue[i]->priority;
      unsigned long candidateArrival = readyQueue[i]->arrivalOrder;
      if (candidatePriority > bestPriority ||
          (candidatePriority == bestPriority && candidateArrival < bestArrival)) {
        bestPriority = candidatePriority;
        bestArrival = candidateArrival;
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
  // SJF no-preemptivo: elige el trabajo con menor tiempo de servicio original.
  // Desempate por orden de llegada (FCFS).
  if (algo == ShipScheduler::ALG_SJF) {
    unsigned long minSvc = readyQueue[0]->serviceMillis;
    unsigned long bestArrival = readyQueue[0]->arrivalOrder;
    for (uint8_t i = 1; i < readyCount; i++) {
      unsigned long svc = readyQueue[i]->serviceMillis;
      unsigned long arr = readyQueue[i]->arrivalOrder;
      if (svc < minSvc || (svc == minSvc && arr < bestArrival)) {
        minSvc = svc;
        bestArrival = arr;
        best = i;
      }
    }
    return best;
  }
  return 0;
}

void ShipScheduler::startNextBoat() {
  // Selecciona el siguiente barco y lo pasa a estado activo.
  if (readyCount == 0) return;

  int idx = findIndexForAlgo(algorithm, readyQueue, readyCount);
  if (idx < 0) return;

  Boat *b = readyQueue[idx];
  // remove from queue
  for (uint8_t i = idx + 1; i < readyCount; i++) readyQueue[i - 1] = readyQueue[i];
  readyCount--;

  // Maneja preempcion en STRN si llego un trabajo mas corto.
  if (algorithm == ALG_STRN && hasActiveBoat && activeBoat) {
    if (activeBoat->remainingMillis > b->remainingMillis) {
      // preempt active
      Boat *preempted = activeBoat;
      hasActiveBoat = false;
      activeBoat = nullptr;
      requeueBoat(preempted, true);
    }
  }

  // Inicia b: actualiza estado y notifica al task para correr.
  b->state = STATE_CROSSING;
  if (b->startedAt == 0) b->startedAt = millis();
  crossingStartedAt = millis();
  activeBoat = b;
  hasActiveBoat = true;
  if (b->taskHandle) xTaskNotify(b->taskHandle, NOTIF_CMD_RUN, eSetValueWithOverwrite);

  Serial.print("Start -> barco #"); Serial.print(b->id); Serial.println();
}

void ShipScheduler::requeueBoat(Boat *boat, bool atFront) {
  // Reinserta un barco en la cola de listos.
  if (!boat) return;
  boat->state = STATE_WAITING;

  // Si la cola esta llena, termina y libera el barco.
  if (readyCount >= MAX_BOATS) {
    boat->cancelled = true;
    if (boat->taskHandle) {
      xTaskNotify(boat->taskHandle, NOTIF_CMD_TERMINATE, eSetValueWithOverwrite);
    } else {
      destroyBoat(boat);
    }
    return;
  }

  if (atFront) {
    for (int i = readyCount; i > 0; i--) readyQueue[i] = readyQueue[i - 1];
    readyQueue[0] = boat;
  } else {
    readyQueue[readyCount] = boat;
  }
  readyCount++;
}

void ShipScheduler::preemptActiveForRR() {
  // Preempcion RR: saca al activo si ya consumo su quantum.
  if (!hasActiveBoat || !activeBoat) return;
  if (readyCount == 0) return;
  if (getActiveElapsedMillis() < rrQuantumMillis) return;

  Boat *preempted = activeBoat;
  if (preempted->taskHandle) xTaskNotify(preempted->taskHandle, NOTIF_CMD_PAUSE, eSetValueWithOverwrite);
  activeBoat = nullptr;
  hasActiveBoat = false;
  requeueBoat(preempted, false);
  startNextBoat();
}

void ShipScheduler::finishActiveBoat() {
  // Finaliza estadisticas del activo y limpia la referencia.
  if (!hasActiveBoat || !activeBoat) return;

  Boat *b = activeBoat;
  if (b->origin == SIDE_LEFT) completedLeftToRight++; else completedRightToLeft++;
  completedTotal++;
  if (completionCount < MAX_BOATS) completionOrder[completionCount++] = b->id;
  if (b->enqueuedAt > 0 && b->startedAt >= b->enqueuedAt) {
    totalWaitMillis += (b->startedAt - b->enqueuedAt);
  }
  if (b->enqueuedAt > 0) {
    unsigned long finishedAt = millis();
    if (finishedAt >= b->enqueuedAt) totalTurnaroundMillis += (finishedAt - b->enqueuedAt);
  }
  totalServiceMillis += b->serviceMillis;
  b->state = STATE_DONE;
  // La tarea finalizada destruye su Boat y se autoelimina.
  // Aqui solo limpiamos la referencia del scheduler.
  activeBoat = nullptr;
  hasActiveBoat = false;
  Serial.print("Barco finalizado\n");
}

void ShipScheduler::update() {
  // Avanza la maquina de estados del scheduler.
  // check if active finished by remainingMillis==0
  if (hasActiveBoat && activeBoat) {
    if (activeBoat->remainingMillis == 0) {
      finishActiveBoat();
    } else if (algorithm == ALG_RR) {
      preemptActiveForRR();
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

uint8_t ShipScheduler::getCompletionId(uint8_t index) const {
  if (index >= completionCount) return 0;
  return completionOrder[index];
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
  // Ignora callbacks tardios durante clear o de boats cancelados.
  if (!b) return;
  if (ignoreCompletions || b->cancelled) return;
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
  // Pausa la tarea del barco activo.
  if (hasActiveBoat && activeBoat) {
    if (activeBoat->taskHandle) xTaskNotify(activeBoat->taskHandle, NOTIF_CMD_PAUSE, eSetValueWithOverwrite);
    Serial.print("Pausado barco #"); Serial.println(activeBoat->id);
  } else {
    Serial.println("No hay barco activo para pausar.");
  }
}

void ShipScheduler::resumeActive() {
  // Reanuda la tarea del barco activo.
  if (hasActiveBoat && activeBoat) {
    if (activeBoat->taskHandle) xTaskNotify(activeBoat->taskHandle, NOTIF_CMD_RUN, eSetValueWithOverwrite);
    Serial.print("Reanundado barco #"); Serial.println(activeBoat->id);
  } else {
    Serial.println("No hay barco activo para reanudar.");
  }
}

void ShipScheduler::dumpStatus() {
  // Imprime una captura del estado de las colas del scheduler.
  Serial.println("--- Scheduler Status ---");
  Serial.print("Algorithm: ");
  Serial.print(getAlgorithmLabel());
  if (algorithm == ALG_RR) {
    Serial.print(" q=");
    Serial.print(rrQuantumMillis);
    Serial.print("ms");
  }
  Serial.println();
  Serial.print("Ready count: "); Serial.println(readyCount);
  for (uint8_t i = 0; i < readyCount; i++) {
    Boat *b = readyQueue[i];
    Serial.print(i); Serial.print(": #"); Serial.print(b->id); Serial.print(" "); Serial.print(boatTypeShort(b->type));
    Serial.print(" prio="); Serial.print(b->priority);
    Serial.print(" from "); Serial.print(boatSideName(b->origin));
    Serial.print(" rem="); Serial.println(b->remainingMillis);
  }
  if (hasActiveBoat && activeBoat) {
    Serial.print("Active: #"); Serial.print(activeBoat->id);
    Serial.print(" prio="); Serial.print(activeBoat->priority);
    Serial.print(" rem="); Serial.println(activeBoat->remainingMillis);
  } else {
    Serial.println("Active: none");
  }
  Serial.println("------------------------");
}