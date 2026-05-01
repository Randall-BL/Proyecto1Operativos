#include "ShipScheduler.h" // API del scheduler. 

#include <freertos/FreeRTOS.h> // Tipos base de FreeRTOS. 
#include <freertos/task.h> // API de tareas. 
#include <stdlib.h> // malloc y free. 

#include "ShipIO.h" // Logging por Serial. 

ShipScheduler *gScheduler = NULL; // Puntero global para callbacks. 

static void ship_scheduler_requeue_boat(ShipScheduler *scheduler, Boat *boat, bool atFront); // Declara requeue. 
static void ship_scheduler_start_next_boat(ShipScheduler *scheduler); // Declara start. 
static void ship_scheduler_finish_active_boat(ShipScheduler *scheduler); // Declara finish. 
static void ship_scheduler_preempt_active_for_rr(ShipScheduler *scheduler); // Declara preempt RR. 

static void boatTask(void *pv) { // Tarea FreeRTOS que ejecuta un barco. 
  Boat *b = (Boat *)pv; // Convierte el parametro a Boat. 
  if (!b) { // Si es nulo. 
    vTaskDelete(NULL); // Elimina la tarea. 
    return; // Termina. 
  } 

  bool running = false; // Estado de ejecucion. 
  while (b->remainingMillis > 0) { // Mientras quede tiempo. 
    uint32_t cmd = 0; // Comando recibido. 
    if (!running) { // Si no esta corriendo. 
      xTaskNotifyWait(0x00, 0xFFFFFFFF, &cmd, portMAX_DELAY); // Espera notificacion. 
      if (cmd == NOTIF_CMD_TERMINATE) break; // Si terminate, sale. 
      if (cmd == NOTIF_CMD_RUN) running = true; // Si run, inicia. 
      continue; // Repite el ciclo. 
    } 

    unsigned long step = 200; // Paso de tiempo en ms. 
    if (step > b->remainingMillis) step = b->remainingMillis; // Ajusta si queda menos. 
    unsigned long slept = 0; // Acumulado de tiempo ejecutado. 
    const unsigned long slice = 50; // Subpaso interno. 
    while (slept < step) { // Mientras falte ejecutar el paso. 
      xTaskNotifyWait(0x00, 0xFFFFFFFF, &cmd, pdMS_TO_TICKS(slice)); // Espera o recibe comando. 
      if (cmd == NOTIF_CMD_TERMINATE) { // Si terminate. 
        b->remainingMillis = 0; // Fuerza fin. 
        running = false; // Detiene ejecucion. 
        break; // Sale del while interno. 
      } 
      if (cmd == NOTIF_CMD_PAUSE) { // Si pause. 
        running = false; // Detiene ejecucion. 
        break; // Sale del while interno. 
      } 

      unsigned long doSleep = slice; // Tiempo a contar. 
      if (slept + doSleep > step) doSleep = step - slept; // Ajusta si excede. 
      slept += doSleep; // Acumula tiempo. 
    } 

    if (b->remainingMillis > step) { // Si queda tiempo. 
      b->remainingMillis -= step; // Reduce el tiempo restante. 
    } else { // Si ya no queda tiempo. 
      b->remainingMillis = 0; // Fuerza a cero. 
    } 
  } 

  if (gScheduler) { // Si hay scheduler global. 
    ship_scheduler_notify_boat_finished(gScheduler, b); // Notifica finalizacion. 
  } 

  destroyBoat(b); // Libera el Boat. 
  vTaskDelete(NULL); // Elimina la tarea. 
} // Fin de boatTask. 

void ship_scheduler_begin(ShipScheduler *scheduler) { // Inicializa el scheduler. 
  if (!scheduler) return; // Valida puntero. 
  ship_scheduler_clear(scheduler); // Limpia estado. 
  gScheduler = scheduler; // Registra global. 
} // Fin de ship_scheduler_begin. 

void ship_scheduler_clear(ShipScheduler *scheduler) { // Limpia colas y tareas. 
  if (!scheduler) return; // Valida puntero. 

  scheduler->ignoreCompletions = true; // Ignora callbacks. 
  for (uint8_t i = 0; i < scheduler->readyCount; i++) { // Recorre cola. 
    Boat *b = scheduler->readyQueue[i]; // Obtiene barco. 
    if (!b) continue; // Si es nulo, salta. 
    b->cancelled = true; // Marca cancelacion. 
    if (b->taskHandle) { // Si hay tarea. 
      xTaskNotify(b->taskHandle, NOTIF_CMD_TERMINATE, eSetValueWithOverwrite); // Ordena terminar. 
    } else { // Si no hay tarea. 
      destroyBoat(b); // Libera memoria. 
    } 
    scheduler->readyQueue[i] = NULL; // Limpia el slot. 
  } 
  scheduler->readyCount = 0; // Resetea la cola. 

  if (scheduler->hasActiveBoat && scheduler->activeBoat) { // Si hay activo. 
    scheduler->activeBoat->cancelled = true; // Marca cancelacion. 
    if (scheduler->activeBoat->taskHandle) { // Si hay tarea. 
      xTaskNotify(scheduler->activeBoat->taskHandle, NOTIF_CMD_TERMINATE, eSetValueWithOverwrite); // Termina. 
    } else { // Si no hay tarea. 
      destroyBoat(scheduler->activeBoat); // Libera memoria. 
    } 
  } 

  scheduler->activeBoat = NULL; // Limpia activo. 
  scheduler->hasActiveBoat = false; // Resetea flag. 
  scheduler->completedLeftToRight = 0; // Resetea contador izq-der. 
  scheduler->completedRightToLeft = 0; // Resetea contador der-izq. 
  scheduler->completedTotal = 0; // Resetea total. 
  scheduler->totalWaitMillis = 0; // Resetea espera acumulada. 
  scheduler->totalTurnaroundMillis = 0; // Resetea turnaround. 
  scheduler->totalServiceMillis = 0; // Resetea servicio. 
  scheduler->completionCount = 0; // Resetea orden final. 
  scheduler->crossingStartedAt = 0; // Resetea tiempo de cruce. 
} // Fin de ship_scheduler_clear. 

void ship_scheduler_set_algorithm(ShipScheduler *scheduler, ShipAlgo algo) { // Configura algoritmo. 
  if (!scheduler) return; // Valida puntero. 
  scheduler->algorithm = algo; // Asigna algoritmo. 
} // Fin de ship_scheduler_set_algorithm. 

ShipAlgo ship_scheduler_get_algorithm(const ShipScheduler *scheduler) { // Lee algoritmo. 
  if (!scheduler) return ALG_FCFS; // Retorna default si es nulo. 
  return scheduler->algorithm; // Retorna algoritmo actual. 
} // Fin de ship_scheduler_get_algorithm. 

const char *ship_scheduler_get_algorithm_label(const ShipScheduler *scheduler) { // Etiqueta del algoritmo. 
  if (!scheduler) return "?"; // Retorna placeholder. 
  switch (scheduler->algorithm) { // Selecciona segun algoritmo. 
    case ALG_FCFS: return "FCFS"; // Etiqueta FCFS. 
    case ALG_SJF: return "SJF"; // Etiqueta SJF. 
    case ALG_STRN: return "STRN"; // Etiqueta STRN. 
    case ALG_EDF: return "EDF"; // Etiqueta EDF. 
    case ALG_RR: return "RR"; // Etiqueta RR. 
    case ALG_PRIORITY: return "PRIO"; // Etiqueta prioridad. 
  } // Fin del switch. 
  return "?"; // Fallback. 
} // Fin de ship_scheduler_get_algorithm_label. 

void ship_scheduler_set_round_robin_quantum(ShipScheduler *scheduler, unsigned long quantumMillis) { // Configura quantum. 
  if (!scheduler) return; // Valida puntero. 
  if (quantumMillis < 100) quantumMillis = 100; // Asegura minimo. 
  scheduler->rrQuantumMillis = quantumMillis; // Asigna quantum. 
} // Fin de ship_scheduler_set_round_robin_quantum. 

unsigned long ship_scheduler_get_round_robin_quantum(const ShipScheduler *scheduler) { // Lee quantum. 
  if (!scheduler) return 0; // Retorna cero si es nulo. 
  return scheduler->rrQuantumMillis; // Retorna quantum. 
} // Fin de ship_scheduler_get_round_robin_quantum. 

static int findIndexForAlgo(ShipAlgo algo, Boat *readyQueue[], uint8_t readyCount) { // Selecciona indice segun algoritmo. 
  if (readyCount == 0) return -1; // Si no hay listos, retorna -1. 
  int best = 0; // Indice mejor por defecto. 
  if (algo == ALG_FCFS || algo == ALG_RR) return 0; // FCFS/RR eligen el primero. 

  if (algo == ALG_SJF) { // Caso SJF. 
    unsigned long minService = readyQueue[0]->serviceMillis; // Tiempo de servicio minimo. 
    unsigned long bestArrival = readyQueue[0]->arrivalOrder; // Orden de llegada para desempate. 
    for (uint8_t i = 1; i < readyCount; i++) { // Recorre la cola. 
      unsigned long candidateService = readyQueue[i]->serviceMillis; // Tiempo de servicio candidato. 
      unsigned long candidateArrival = readyQueue[i]->arrivalOrder; // Orden de llegada candidato. 
      if (candidateService < minService || 
          (candidateService == minService && candidateArrival < bestArrival)) { // Compara servicio y llegada. 
        minService = candidateService; // Actualiza minimo. 
        bestArrival = candidateArrival; // Actualiza orden. 
        best = i; // Actualiza indice. 
      } 
    } 
    return best; // Retorna el mejor. 
  } 

  if (algo == ALG_STRN) { // Caso STRN. 
    unsigned long minRem = readyQueue[0]->remainingMillis; // Tiempo restante minimo. 
    for (uint8_t i = 1; i < readyCount; i++) { // Recorre la cola. 
      if (readyQueue[i]->remainingMillis < minRem) { // Si encuentra menor. 
        minRem = readyQueue[i]->remainingMillis; // Actualiza minimo. 
        best = i; // Actualiza indice. 
      } 
    } 
    return best; // Retorna el mejor. 
  } 

  if (algo == ALG_PRIORITY) { // Caso prioridad. 
    uint8_t bestPriority = readyQueue[0]->priority; // Mejor prioridad inicial. 
    unsigned long bestArrival = readyQueue[0]->arrivalOrder; // Mejor orden inicial. 
    for (uint8_t i = 1; i < readyCount; i++) { // Recorre la cola. 
      uint8_t candidatePriority = readyQueue[i]->priority; // Prioridad candidata. 
      unsigned long candidateArrival = readyQueue[i]->arrivalOrder; // Orden candidato. 
      if (candidatePriority > bestPriority ||
          (candidatePriority == bestPriority && candidateArrival < bestArrival)) { // Compara prioridad y orden. 
        bestPriority = candidatePriority; // Actualiza prioridad. 
        bestArrival = candidateArrival; // Actualiza orden. 
        best = i; // Actualiza indice. 
      } 
    } 
    return best; // Retorna el mejor. 
  } 

  if (algo == ALG_EDF) { // Caso EDF. 
    unsigned long minDead = readyQueue[0]->deadlineMillis; // Deadline minimo. 
    for (uint8_t i = 1; i < readyCount; i++) { // Recorre la cola. 
      if (readyQueue[i]->deadlineMillis < minDead) { // Si encuentra menor. 
        minDead = readyQueue[i]->deadlineMillis; // Actualiza minimo. 
        best = i; // Actualiza indice. 
      } 
    } 
    return best; // Retorna el mejor. 
  } 

  return 0; // Retorno por defecto. 
} // Fin de findIndexForAlgo. 

void ship_scheduler_enqueue(ShipScheduler *scheduler, Boat *boat) { // Encola un barco nuevo. 
  if (!scheduler || !boat) return; // Valida punteros. 
  scheduler->ignoreCompletions = false; // Habilita callbacks. 
  if (scheduler->readyCount >= MAX_BOATS || ship_scheduler_get_waiting_count(scheduler, boat->origin) >= 3) { // Verifica limites. 
    ship_logln("Cola llena; no se agrego el barco."); // Informa cola llena. 
    destroyBoat(boat); // Libera el barco. 
    return; // Termina. 
  } 

  boat->cancelled = false; // Limpia cancelacion. 
  if (boat->enqueuedAt == 0) boat->enqueuedAt = millis(); // Marca encolado si aplica. 
  boat->state = STATE_WAITING; // Estado en espera. 

  uint8_t insertAt = scheduler->readyCount; // Posicion de insercion. 
  while (insertAt > 0 && scheduler->readyQueue[insertAt - 1]->arrivalOrder > boat->arrivalOrder) { // Mantiene orden por llegada. 
    scheduler->readyQueue[insertAt] = scheduler->readyQueue[insertAt - 1]; // Desplaza a la derecha. 
    insertAt--; // Decrementa indice. 
  } 

  scheduler->readyQueue[insertAt] = boat; // Inserta el barco. 
  scheduler->readyCount++; // Incrementa la cola. 

  ship_logf("Barco agregado: #%u tipo=%s origen=%s\n", boat->id, boatTypeName(boat->type), boatSideName(boat->origin)); // Log detallado de alta. 

  xTaskCreate(boatTask, "boat", 4096, boat, 1, &boat->taskHandle); // Crea la tarea del barco. 

  if (scheduler->hasActiveBoat && scheduler->activeBoat) { // Si hay activo. 
    bool shouldPreempt = false; // Bandera de preempcion. 
    if (scheduler->algorithm == ALG_STRN) { // Si STRN. 
      if (boat->remainingMillis < scheduler->activeBoat->remainingMillis) shouldPreempt = true; // Compara restante. 
    } else if (scheduler->algorithm == ALG_EDF) { // Si EDF. 
      if (boat->deadlineMillis < scheduler->activeBoat->deadlineMillis) shouldPreempt = true; // Compara deadline. 
    } else if (scheduler->algorithm == ALG_PRIORITY) { // Si prioridad. 
      if (boat->priority > scheduler->activeBoat->priority) shouldPreempt = true; // Compara prioridad. 
    } 

    if (shouldPreempt) { // Si se debe preemptar. 
      ship_logf("Preemption: barco #%u solicita preemp.\n", boat->id); // Log de preempcion. 
      if (scheduler->activeBoat->taskHandle) { // Si hay tarea activa. 
        xTaskNotify(scheduler->activeBoat->taskHandle, NOTIF_CMD_PAUSE, eSetValueWithOverwrite); // Envia pausa. 
      } 
      Boat *preempted = scheduler->activeBoat; // Guarda el activo. 
      scheduler->activeBoat = NULL; // Limpia activo. 
      scheduler->hasActiveBoat = false; // Limpia flag. 
      ship_scheduler_requeue_boat(scheduler, preempted, true); // Reencola el activo. 
      ship_scheduler_start_next_boat(scheduler); // Inicia el siguiente. 
    } 
  } 
} // Fin de ship_scheduler_enqueue. 

void ship_scheduler_load_demo_manifest(ShipScheduler *scheduler) { // Carga manifiesto de demo. 
  if (!scheduler) return; // Valida puntero. 
  ship_scheduler_clear(scheduler); // Limpia estado. 
  resetBoatSequence(); // Reinicia secuencias. 

  ship_scheduler_enqueue(scheduler, createBoat(SIDE_LEFT, BOAT_NORMAL)); // Encola normal izq. 
  ship_scheduler_enqueue(scheduler, createBoat(SIDE_RIGHT, BOAT_PESQUERA)); // Encola pesquera der. 
  ship_scheduler_enqueue(scheduler, createBoat(SIDE_LEFT, BOAT_PATRULLA)); // Encola patrulla izq. 
  ship_scheduler_enqueue(scheduler, createBoat(SIDE_RIGHT, BOAT_NORMAL)); // Encola normal der. 
  ship_scheduler_enqueue(scheduler, createBoat(SIDE_LEFT, BOAT_PESQUERA)); // Encola pesquera izq. 

  ship_logln("Manifesto cargado."); // Log de carga. 
} // Fin de ship_scheduler_load_demo_manifest. 

static void ship_scheduler_requeue_boat(ShipScheduler *scheduler, Boat *boat, bool atFront) { // Reencola un barco. 
  if (!scheduler || !boat) return; // Valida punteros. 
  boat->state = STATE_WAITING; // Vuelve a estado de espera. 

  if (scheduler->readyCount >= MAX_BOATS) { // Si la cola esta llena. 
    boat->cancelled = true; // Marca cancelacion. 
    if (boat->taskHandle) { // Si hay tarea. 
      xTaskNotify(boat->taskHandle, NOTIF_CMD_TERMINATE, eSetValueWithOverwrite); // Termina tarea. 
    } else { // Si no hay tarea. 
      destroyBoat(boat); // Libera memoria. 
    } 
    return; // Sale. 
  } 

  if (atFront) { // Si se reencola al frente. 
    for (int i = scheduler->readyCount; i > 0; i--) scheduler->readyQueue[i] = scheduler->readyQueue[i - 1]; // Desplaza. 
    scheduler->readyQueue[0] = boat; // Inserta al frente. 
  } else { // Si se reencola al final. 
    scheduler->readyQueue[scheduler->readyCount] = boat; // Inserta al final. 
  } 
  scheduler->readyCount++; // Incrementa la cola. 
} // Fin de ship_scheduler_requeue_boat. 

static void ship_scheduler_start_next_boat(ShipScheduler *scheduler) { // Selecciona y arranca el siguiente barco. 
  if (!scheduler || scheduler->readyCount == 0) return; // Valida estado. 

  int idx = findIndexForAlgo(scheduler->algorithm, scheduler->readyQueue, scheduler->readyCount); // Busca el mejor. 
  if (idx < 0) return; // Sale si no hay indice. 

  Boat *b = scheduler->readyQueue[idx]; // Selecciona el barco. 
  for (uint8_t i = idx + 1; i < scheduler->readyCount; i++) scheduler->readyQueue[i - 1] = scheduler->readyQueue[i]; // Compacta cola. 
  scheduler->readyCount--; // Reduce contador. 

  if (scheduler->algorithm == ALG_STRN && scheduler->hasActiveBoat && scheduler->activeBoat) { // Preempcion STRN. 
    if (scheduler->activeBoat->remainingMillis > b->remainingMillis) { // Si el nuevo es mas corto. 
      Boat *preempted = scheduler->activeBoat; // Guarda el activo. 
      scheduler->hasActiveBoat = false; // Limpia flag. 
      scheduler->activeBoat = NULL; // Limpia activo. 
      ship_scheduler_requeue_boat(scheduler, preempted, true); // Reencola el activo. 
    } 
  } 

  b->state = STATE_CROSSING; // Cambia estado a cruzando. 
  if (b->startedAt == 0) b->startedAt = millis(); // Marca inicio si aplica. 
  scheduler->crossingStartedAt = millis(); // Marca el inicio del cruce. 
  scheduler->activeBoat = b; // Asigna activo. 
  scheduler->hasActiveBoat = true; // Marca flag. 
  if (b->taskHandle) xTaskNotify(b->taskHandle, NOTIF_CMD_RUN, eSetValueWithOverwrite); // Arranca la tarea. 

  ship_logf("Start -> barco #%u\n", b->id); // Log del inicio. 
} // Fin de ship_scheduler_start_next_boat. 

static void ship_scheduler_preempt_active_for_rr(ShipScheduler *scheduler) { // Preempcion por RR. 
  if (!scheduler || !scheduler->hasActiveBoat || !scheduler->activeBoat) return; // Valida activo. 
  if (scheduler->readyCount == 0) return; // Si no hay listos, sale. 
  if (ship_scheduler_get_active_elapsed_millis(scheduler) < scheduler->rrQuantumMillis) return; // Si no consume quantum, sale. 

  Boat *preempted = scheduler->activeBoat; // Guarda el activo. 
  if (preempted->taskHandle) xTaskNotify(preempted->taskHandle, NOTIF_CMD_PAUSE, eSetValueWithOverwrite); // Pausa la tarea. 
  scheduler->activeBoat = NULL; // Limpia activo. 
  scheduler->hasActiveBoat = false; // Limpia flag. 
  ship_scheduler_requeue_boat(scheduler, preempted, false); // Reencola al final. 
  ship_scheduler_start_next_boat(scheduler); // Arranca el siguiente. 
} // Fin de ship_scheduler_preempt_active_for_rr. 

static void ship_scheduler_finish_active_boat(ShipScheduler *scheduler) { // Finaliza el barco activo. 
  if (!scheduler || !scheduler->hasActiveBoat || !scheduler->activeBoat) return; // Valida activo. 

  Boat *b = scheduler->activeBoat; // Obtiene el activo. 
  if (b->origin == SIDE_LEFT) scheduler->completedLeftToRight++; // Incrementa izq-der. 
  else scheduler->completedRightToLeft++; // Incrementa der-izq. 
  scheduler->completedTotal++; // Incrementa total. 
  if (scheduler->completionCount < MAX_BOATS) scheduler->completionOrder[scheduler->completionCount++] = b->id; // Guarda orden. 
  if (b->enqueuedAt > 0 && b->startedAt >= b->enqueuedAt) { // Si hay tiempos validos. 
    scheduler->totalWaitMillis += (b->startedAt - b->enqueuedAt); // Acumula espera. 
  } 
  if (b->enqueuedAt > 0) { // Si hay encolado. 
    unsigned long finishedAt = millis(); // Tiempo de fin. 
    if (finishedAt >= b->enqueuedAt) scheduler->totalTurnaroundMillis += (finishedAt - b->enqueuedAt); // Acumula turnaround. 
  } 
  scheduler->totalServiceMillis += b->serviceMillis; // Acumula servicio. 
  b->state = STATE_DONE; // Marca estado final. 
  scheduler->activeBoat = NULL; // Limpia activo. 
  scheduler->hasActiveBoat = false; // Limpia flag. 
  ship_logf("Barco finalizado: #%u tipo=%s origen=%s\n", b->id, boatTypeName(b->type), boatSideName(b->origin)); // Log de finalizacion con tipo. 
} // Fin de ship_scheduler_finish_active_boat. 

void ship_scheduler_update(ShipScheduler *scheduler) { // Ejecuta un tick de planificacion. 
  if (!scheduler) return; // Valida puntero. 

  if (scheduler->hasActiveBoat && scheduler->activeBoat) { // Si hay activo. 
    if (scheduler->activeBoat->remainingMillis == 0) { // Si ya termino. 
      ship_scheduler_finish_active_boat(scheduler); // Finaliza. 
    } else if (scheduler->algorithm == ALG_RR) { // Si es RR. 
      ship_scheduler_preempt_active_for_rr(scheduler); // Aplica preempcion. 
    } 
  } 

  if (!scheduler->hasActiveBoat && scheduler->readyCount > 0) { // Si no hay activo y hay listos. 
    ship_scheduler_start_next_boat(scheduler); // Inicia el siguiente. 
  } 
} // Fin de ship_scheduler_update. 

const Boat *ship_scheduler_get_active_boat(const ShipScheduler *scheduler) { // Devuelve barco activo. 
  if (!scheduler || !scheduler->hasActiveBoat) return NULL; // Valida estado. 
  return scheduler->activeBoat; // Retorna activo. 
} // Fin de ship_scheduler_get_active_boat. 

uint8_t ship_scheduler_get_ready_count(const ShipScheduler *scheduler) { // Devuelve cantidad en cola. 
  return scheduler ? scheduler->readyCount : 0; // Retorna count o cero. 
} // Fin de ship_scheduler_get_ready_count. 

const Boat *ship_scheduler_get_ready_boat(const ShipScheduler *scheduler, uint8_t index) { // Devuelve barco en cola. 
  if (!scheduler || index >= scheduler->readyCount) return NULL; // Valida rango. 
  return scheduler->readyQueue[index]; // Retorna el barco. 
} // Fin de ship_scheduler_get_ready_boat. 

uint8_t ship_scheduler_get_completion_id(const ShipScheduler *scheduler, uint8_t index) { // Devuelve ID de finalizacion. 
  if (!scheduler || index >= scheduler->completionCount) return 0; // Valida rango. 
  return scheduler->completionOrder[index]; // Retorna el ID. 
} // Fin de ship_scheduler_get_completion_id. 

uint8_t ship_scheduler_get_waiting_count(const ShipScheduler *scheduler, BoatSide side) { // Cuenta barcos por lado. 
  if (!scheduler) return 0; // Valida puntero. 
  uint8_t count = 0; // Contador local. 
  for (uint8_t i = 0; i < scheduler->readyCount; i++) { // Recorre cola. 
    if (scheduler->readyQueue[i]->origin == side) count++; // Cuenta si coincide el lado. 
  } 
  return count; // Retorna el conteo. 
} // Fin de ship_scheduler_get_waiting_count. 

const Boat *ship_scheduler_get_waiting_boat(const ShipScheduler *scheduler, BoatSide side, uint8_t index) { // Devuelve barco por lado e indice. 
  if (!scheduler) return NULL; // Valida puntero. 
  uint8_t seen = 0; // Contador de vistos. 
  for (uint8_t i = 0; i < scheduler->readyCount; i++) { // Recorre la cola. 
    if (scheduler->readyQueue[i]->origin != side) continue; // Salta si no coincide. 
    if (seen == index) return scheduler->readyQueue[i]; // Retorna si coincide el indice. 
    seen++; // Incrementa vistos. 
  } 
  return NULL; // Retorna nulo si no existe. 
} // Fin de ship_scheduler_get_waiting_boat. 

uint16_t ship_scheduler_get_completed_left_to_right(const ShipScheduler *scheduler) { // Devuelve completados izq-der. 
  return scheduler ? scheduler->completedLeftToRight : 0; // Retorna contador o cero. 
} // Fin de ship_scheduler_get_completed_left_to_right. 

uint16_t ship_scheduler_get_completed_right_to_left(const ShipScheduler *scheduler) { // Devuelve completados der-izq. 
  return scheduler ? scheduler->completedRightToLeft : 0; // Retorna contador o cero. 
} // Fin de ship_scheduler_get_completed_right_to_left. 

unsigned long ship_scheduler_get_active_elapsed_millis(const ShipScheduler *scheduler) { // Devuelve tiempo transcurrido. 
  if (!scheduler || !scheduler->hasActiveBoat || !scheduler->activeBoat) return 0; // Valida estado. 
  return millis() - scheduler->crossingStartedAt; // Retorna diferencia. 
} // Fin de ship_scheduler_get_active_elapsed_millis. 

uint16_t ship_scheduler_get_completed_total(const ShipScheduler *scheduler) { // Devuelve total completados. 
  return scheduler ? scheduler->completedTotal : 0; // Retorna contador o cero. 
} // Fin de ship_scheduler_get_completed_total. 

unsigned long ship_scheduler_get_total_wait_millis(const ShipScheduler *scheduler) { // Devuelve espera total. 
  return scheduler ? scheduler->totalWaitMillis : 0; // Retorna acumulado o cero. 
} // Fin de ship_scheduler_get_total_wait_millis. 

unsigned long ship_scheduler_get_total_turnaround_millis(const ShipScheduler *scheduler) { // Devuelve turnaround total. 
  return scheduler ? scheduler->totalTurnaroundMillis : 0; // Retorna acumulado o cero. 
} // Fin de ship_scheduler_get_total_turnaround_millis. 

unsigned long ship_scheduler_get_total_service_millis(const ShipScheduler *scheduler) { // Devuelve servicio total. 
  return scheduler ? scheduler->totalServiceMillis : 0; // Retorna acumulado o cero. 
} // Fin de ship_scheduler_get_total_service_millis. 

uint8_t ship_scheduler_get_completion_count(const ShipScheduler *scheduler) { // Devuelve cantidad en orden final. 
  return scheduler ? scheduler->completionCount : 0; // Retorna count o cero. 
} // Fin de ship_scheduler_get_completion_count. 

void ship_scheduler_notify_boat_finished(ShipScheduler *scheduler, Boat *b) { // Callback de finalizacion. 
  if (!scheduler || !b) return; // Valida punteros. 
  if (scheduler->ignoreCompletions || b->cancelled) return; // Ignora si corresponde. 

  if (scheduler->hasActiveBoat && scheduler->activeBoat == b) { // Si era el activo. 
    ship_scheduler_finish_active_boat(scheduler); // Finaliza normalmente. 
  } else { // Si estaba en cola. 
    for (uint8_t i = 0; i < scheduler->readyCount; i++) { // Recorre cola. 
      if (scheduler->readyQueue[i] == b) { // Si encuentra el barco. 
        for (uint8_t j = i + 1; j < scheduler->readyCount; j++) scheduler->readyQueue[j - 1] = scheduler->readyQueue[j]; // Compacta cola. 
        scheduler->readyCount--; // Reduce contador. 
        return; // Sale. 
      } 
    } 
  } 
} // Fin de ship_scheduler_notify_boat_finished. 

void ship_scheduler_pause_active(ShipScheduler *scheduler) { // Pausa el barco activo. 
  if (scheduler && scheduler->hasActiveBoat && scheduler->activeBoat) { // Valida activo. 
    if (scheduler->activeBoat->taskHandle) { // Si hay tarea. 
      xTaskNotify(scheduler->activeBoat->taskHandle, NOTIF_CMD_PAUSE, eSetValueWithOverwrite); // Envia pausa. 
    } 
    ship_logf("Pausado barco #%u\n", scheduler->activeBoat->id); // Log de pausa. 
  } else { // Si no hay activo. 
    ship_logln("No hay barco activo para pausar."); // Mensaje de error. 
  } 
} // Fin de ship_scheduler_pause_active. 

void ship_scheduler_resume_active(ShipScheduler *scheduler) { // Reanuda el barco activo. 
  if (scheduler && scheduler->hasActiveBoat && scheduler->activeBoat) { // Valida activo. 
    if (scheduler->activeBoat->taskHandle) { // Si hay tarea. 
      xTaskNotify(scheduler->activeBoat->taskHandle, NOTIF_CMD_RUN, eSetValueWithOverwrite); // Envia run. 
    } 
    ship_logf("Reanudado barco #%u\n", scheduler->activeBoat->id); // Log de reanudacion. 
  } else { // Si no hay activo. 
    ship_logln("No hay barco activo para reanudar."); // Mensaje de error. 
  } 
} // Fin de ship_scheduler_resume_active. 

void ship_scheduler_dump_status(const ShipScheduler *scheduler) { // Imprime estado del scheduler. 
  if (!scheduler) return; // Valida puntero. 
  ship_logln("--- Scheduler Status ---"); // Encabezado. 
  ship_logf("Algorithm: %s\n", ship_scheduler_get_algorithm_label(scheduler)); // Algoritmo actual. 
  ship_logf("Ready count: %u\n", scheduler->readyCount); // Cantidad en cola. 
  for (uint8_t i = 0; i < scheduler->readyCount; i++) { // Recorre cola. 
    Boat *b = scheduler->readyQueue[i]; // Obtiene barco. 
    ship_logf("%u: #%u %s from %s rem=%lu\n", i, b->id, boatTypeShort(b->type), boatSideName(b->origin), b->remainingMillis); // Imprime linea. 
  } 
  if (scheduler->hasActiveBoat && scheduler->activeBoat) { // Si hay activo. 
    ship_logf("Active: #%u rem=%lu\n", scheduler->activeBoat->id, scheduler->activeBoat->remainingMillis); // Imprime activo. 
  } else { // Si no hay activo. 
    ship_logln("Active: none"); // Informa sin activo. 
  } 
  ship_logln("------------------------"); // Cierra el bloque. 
} // Fin de ship_scheduler_dump_status. 
