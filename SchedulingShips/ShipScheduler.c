#include "ShipScheduler.h" // API del scheduler. 

#include <freertos/FreeRTOS.h> // Tipos base de FreeRTOS. 
#include <freertos/task.h> // API de tareas. 
#include <stdlib.h> // malloc y free. 
#include <stdio.h> // fopen para cargar channel_config.txt
#include <freertos/semphr.h> // Semaphores and mutexes
#include <string.h>
#include <limits.h> // ULONG_MAX para comparaciones de servicio en SJF.

#include "ShipIO.h" // Logging por Serial. 
#include "ShipDisplay.h" // API de pantalla (para que cada barco pueda redibujar). 
#include "ShipCommands.h" // Parser de comandos para leer config.

ShipScheduler *gScheduler = NULL; // Puntero global para callbacks. 

static SemaphoreHandle_t *gSlotSemaphores = NULL; // Semaforos por casilla del canal.
static uint16_t gSlotSemaphoreCount = 0; // Cantidad de semaforos activos.

static void ship_scheduler_requeue_boat(ShipScheduler *scheduler, Boat *boat, bool atFront); // Declara requeue. 
static bool ship_scheduler_start_next_boat(ShipScheduler *scheduler); // Declara start. 
static void ship_scheduler_finish_active_boat(ShipScheduler *scheduler, Boat *boat); // Declara finish. 
static bool ship_scheduler_preempt_active_for_rr(ShipScheduler *scheduler); // Declara preempt RR. 
static void ship_scheduler_yield_active_for_rr(ShipScheduler *scheduler, Boat *boat); // Yield voluntario cuando el barco se bloquea.
static BoatSide opposite_side(BoatSide side); // Declara lado opuesto.
static bool queue_has_side(const ShipScheduler *scheduler, BoatSide side); // Declara busqueda por lado.
static bool candidate_is_better(ShipAlgo algo, const Boat *candidate, const Boat *best); // Declara comparador.
static int findIndexForAlgoAndSide(ShipAlgo algo, Boat *readyQueue[], uint8_t readyCount, bool useSide, BoatSide side); // Declara selector filtrado.
static void ship_scheduler_tick_sign(ShipScheduler *scheduler); // Declara tick de letrero.
static int ship_scheduler_select_next_index(ShipScheduler *scheduler); // Declara selector con flujo.
static unsigned long ship_scheduler_boat_elapsed_millis(const Boat *boat); // Declara elapsed por barco.
static unsigned long ship_scheduler_estimate_service_millis(const ShipScheduler *scheduler, const Boat *boat); // Declara estimador de servicio.
static void ship_scheduler_sync_primary_active(ShipScheduler *scheduler); // Declara sync de activo.
static void ship_scheduler_sync_rr_permissions(ShipScheduler *scheduler); // Declara sync de permisos RR.
static void ship_scheduler_promote_active_to_front(ShipScheduler *scheduler, Boat *boat); // Promueve un activo al frente.
static void ship_scheduler_add_active(ShipScheduler *scheduler, Boat *boat); // Declara alta de activo.
static void ship_scheduler_remove_active(ShipScheduler *scheduler, Boat *boat); // Declara baja de activo.
static bool ship_scheduler_is_tico_safe(const ShipScheduler *scheduler, const Boat *candidate); // Declara seguridad TICO.
static void ship_scheduler_destroy_slot_resources(void); // Libera semaforos de casillas.
static unsigned long ship_scheduler_compute_default_deadline(const ShipScheduler *scheduler, const Boat *boat); // Declara deadline EDF por defecto.
static bool ship_scheduler_init_slot_resources(uint16_t count); // Crea semaforos de casillas.
static SemaphoreHandle_t ship_scheduler_get_slot_semaphore(uint16_t slotIndex); // Obtiene semaforo de casilla.
static bool ship_scheduler_wait_for_slot(ShipScheduler *scheduler, uint16_t slotIndex, Boat *boat); // Espera una casilla libre.
static void ship_scheduler_lock_slot_owner(ShipScheduler *scheduler, uint16_t slotIndex, uint8_t boatId); // Marca casilla ocupada.
static void ship_scheduler_unlock_slot_owner(ShipScheduler *scheduler, uint16_t slotIndex); // Marca casilla libre.
static void ship_scheduler_restore_parked_boats(ShipScheduler *scheduler); // Reubica barcos retirados por emergencia.

// Envuelve notificaciones a tareas con un llamado centralizado para evitar
// duplicar llamadas a xTaskNotify en varios sitios. Simple y no-ISR.
static void safe_task_notify(TaskHandle_t taskHandle, uint32_t notificationValue) {
  if (!taskHandle) return;
  xTaskNotify(taskHandle, notificationValue, eSetValueWithOverwrite);
}

// Selecciona el indice del mejor barco en `readyQueue` aplicando el filtro
// opcional de lado (`useSide`). Retorna -1 si no hay candidato.
static int findIndexForAlgoAndSide(ShipAlgo algo, Boat *readyQueue[], uint8_t readyCount, bool useSide, BoatSide side) {
  if (!readyQueue || readyCount == 0) return -1;
  int bestIdx = -1;
  Boat *best = NULL;
  for (uint8_t i = 0; i < readyCount; i++) {
    Boat *c = readyQueue[i];
    if (!c) continue;
    if (useSide && c->origin != side) continue;
    if (!best) {
      best = c; bestIdx = (int)i; continue;
    }
    if (candidate_is_better(algo, c, best)) {
      best = c; bestIdx = (int)i;
    }
  }
  return bestIdx;
}

// Intenta reservar un rango contiguo de casillas [startIndex, startIndex + dir*(steps-1)].
// Para succeed debe que todas las casillas estén libres (slotOwner == 0) y se
// adquieren sus semáforos. Si falla, libera cualquier semáforo tomado.
bool ship_scheduler_try_reserve_range(ShipScheduler *scheduler, int startIndex, uint8_t steps, Boat *boat) {
  if (!scheduler || !boat || steps == 0) return false;
  if (steps > 16) return false; // Guarda: el buffer taken[] tiene capacidad maxima de 16.
  if (scheduler->listLength == 0 || !scheduler->slotOwner) return false;
  int dir = (boat->origin == SIDE_LEFT) ? 1 : -1;
  int endIndex = startIndex + dir * ((int)steps - 1);
  if (startIndex < 0 || endIndex < 0 || startIndex >= (int)scheduler->listLength || endIndex >= (int)scheduler->listLength) return false;

  if ((SemaphoreHandle_t)scheduler->channelSlotsGuard == NULL) return false;
  if (xSemaphoreTake((SemaphoreHandle_t)scheduler->channelSlotsGuard, pdMS_TO_TICKS(100)) != pdTRUE) return false;

  // Verificar propietarios actuales
  int idx = startIndex;
  for (uint8_t s = 0; s < steps; s++) {
    if (scheduler->slotOwner[idx] != 0) {
      xSemaphoreGive((SemaphoreHandle_t)scheduler->channelSlotsGuard);
      return false;
    }
    idx += dir;
  }

  // Intentar tomar los semaforos de las casillas (no bloquear):
  idx = startIndex;
  SemaphoreHandle_t taken[16]; // max steps razonable; no se esperan >16
  uint8_t takenCount = 0;
  for (uint8_t s = 0; s < steps; s++) {
    SemaphoreHandle_t sem = ship_scheduler_get_slot_semaphore((uint16_t)idx);
    if (!sem) {
      // Libera los ya tomados
      for (uint8_t t = 0; t < takenCount; t++) xSemaphoreGive(taken[t]);
      xSemaphoreGive((SemaphoreHandle_t)scheduler->channelSlotsGuard);
      return false;
    }
    if (xSemaphoreTake(sem, 0) != pdTRUE) {
      // No pudo tomar; libera los ya tomados
      for (uint8_t t = 0; t < takenCount; t++) xSemaphoreGive(taken[t]);
      xSemaphoreGive((SemaphoreHandle_t)scheduler->channelSlotsGuard);
      return false;
    }
    taken[takenCount++] = sem;
    idx += dir;
  }

  // Asignar ownership ahora que se tomaron los semaforos
  idx = startIndex;
  for (uint8_t s = 0; s < steps; s++) {
    ship_scheduler_lock_slot_owner(scheduler, (uint16_t)idx, boat->id);
    idx += dir;
  }

  xSemaphoreGive((SemaphoreHandle_t)scheduler->channelSlotsGuard);
  return true;
}

// Valores por defecto si no existe channel_config.txt
#define DEFAULT_LIST_LENGTH 3U
#define DEFAULT_VISUAL_CHANNEL_LENGTH 6U

#define FLOW_LOG(schedulerPtr, fmt, ...) do { if ((schedulerPtr) && (schedulerPtr)->flowLoggingEnabled) ship_logf(fmt, ##__VA_ARGS__); } while (0) // Macro de trazas de flujo.

static void ship_scheduler_destroy_slot_resources(void) {
  if (gSlotSemaphores) {
    for (uint16_t i = 0; i < gSlotSemaphoreCount; i++) {
      if (gSlotSemaphores[i]) {
        vSemaphoreDelete(gSlotSemaphores[i]);
        gSlotSemaphores[i] = NULL;
      }
    }
    free(gSlotSemaphores);
    gSlotSemaphores = NULL;
  }
  gSlotSemaphoreCount = 0;
}

static bool ship_scheduler_init_slot_resources(uint16_t count) {
  ship_scheduler_destroy_slot_resources();
  if (count == 0) return false;
  gSlotSemaphores = (SemaphoreHandle_t *)calloc(count, sizeof(SemaphoreHandle_t));
  if (!gSlotSemaphores) return false;
  gSlotSemaphoreCount = count;
  for (uint16_t i = 0; i < count; i++) {
    gSlotSemaphores[i] = xSemaphoreCreateBinary();
    if (!gSlotSemaphores[i]) {
      ship_scheduler_destroy_slot_resources();
      return false;
    }
    xSemaphoreGive(gSlotSemaphores[i]);
  }
  return true;
}

static SemaphoreHandle_t ship_scheduler_get_slot_semaphore(uint16_t slotIndex) {
  if (!gSlotSemaphores || slotIndex >= gSlotSemaphoreCount) return NULL;
  return gSlotSemaphores[slotIndex];
}

static void ship_scheduler_lock_slot_owner(ShipScheduler *scheduler, uint16_t slotIndex, uint8_t boatId) {
  if (!scheduler || !scheduler->slotOwner || slotIndex >= scheduler->listLength) return;
  scheduler->slotOwner[slotIndex] = boatId;
}

static void ship_scheduler_unlock_slot_owner(ShipScheduler *scheduler, uint16_t slotIndex) {
  if (!scheduler || !scheduler->slotOwner || slotIndex >= scheduler->listLength) return;
  scheduler->slotOwner[slotIndex] = 0;
}

static void ship_scheduler_restore_parked_boats(ShipScheduler *scheduler) {
  if (!scheduler) return;
  for (uint8_t i = 0; i < scheduler->activeCount; i++) {
    Boat *activeBoat = scheduler->activeBoats[i];
    if (!activeBoat || !activeBoat->emergencyParked) continue;
    int16_t savedSlot = activeBoat->emergencySavedSlot;
    activeBoat->emergencyParked = false;
    activeBoat->emergencySavedSlot = -1;
    if (savedSlot >= 0 && scheduler->listLength > 0 && scheduler->slotOwner) {
      // Restaurar con stepSize casillas para que la reserva sea consistente
      // con el tamano real que ocupa el barco al moverse.
      uint8_t slotsToRestore = (activeBoat->stepSize > 0) ? activeBoat->stepSize : 1;
      if (slotsToRestore > 16) slotsToRestore = 16;
      if (ship_scheduler_try_reserve_range(scheduler, (int)savedSlot, slotsToRestore, activeBoat)) {
        activeBoat->currentSlot = savedSlot;
        FLOW_LOG(scheduler, "[EMERGENCY] Barco #%u restaurado en casilla %d (steps=%u)\n", activeBoat->id, savedSlot, slotsToRestore);
      } else {
        // Fallback: intentar con solo 1 casilla si el rango completo no esta libre.
        if (ship_scheduler_try_reserve_range(scheduler, (int)savedSlot, 1, activeBoat)) {
          activeBoat->currentSlot = savedSlot;
          FLOW_LOG(scheduler, "[EMERGENCY] Barco #%u restaurado en casilla %d (fallback 1 slot)\n", activeBoat->id, savedSlot);
        } else {
          FLOW_LOG(scheduler, "[EMERGENCY] No se pudo restaurar casilla %d para barco #%u\n", savedSlot, activeBoat->id);
        }
      }
    }
  }
}

static bool ship_scheduler_wait_for_slot(ShipScheduler *scheduler, uint16_t slotIndex, Boat *boat) {
  if (!scheduler || !boat) return false;
  SemaphoreHandle_t slotSem = ship_scheduler_get_slot_semaphore(slotIndex);
  if (!slotSem) return false;

    uint32_t waitCmd = 0; 
    for (;;) { 
      if (xSemaphoreTake(slotSem, pdMS_TO_TICKS(100)) == pdTRUE) return true; 
      if (xTaskNotifyWait(0x00, 0xFFFFFFFF, &waitCmd, pdMS_TO_TICKS(0)) == pdTRUE) { 
        if (waitCmd == NOTIF_CMD_TERMINATE) return false; 
        if (waitCmd == NOTIF_CMD_PAUSE) return false; 
      } 
    } 
}

static bool ship_scheduler_wait_for_slot_range(ShipScheduler *scheduler, int startIndex, int steps, int dir, Boat *boat) {
  if (!scheduler || !boat || steps <= 0) return false;
  if (xSemaphoreTake((SemaphoreHandle_t)scheduler->channelSlotsGuard, pdMS_TO_TICKS(100)) != pdTRUE) return false;

  bool allFree = true;
  int idx = startIndex;
  
  for (int s = 0; s < steps; s++) {
    if (idx < 0 || idx >= scheduler->listLength || scheduler->slotOwner[idx] != 0) {
      allFree = false;
      break;
    }
    idx += dir;
  }
  
  xSemaphoreGive((SemaphoreHandle_t)scheduler->channelSlotsGuard);
  return allFree;
}

static BoatSide opposite_side(BoatSide side) {
  return side == SIDE_LEFT ? SIDE_RIGHT : SIDE_LEFT;
}

static bool queue_has_side(const ShipScheduler *scheduler, BoatSide side) {
  if (!scheduler) return false;
  for (uint8_t i = 0; i < scheduler->readyCount; i++) {
    Boat *candidate = scheduler->readyQueue[i];
    if (candidate && candidate->origin == side) return true;
  }
  return false;
}

static bool ship_scheduler_try_move_range(ShipScheduler *scheduler, int startIndex, uint8_t steps, Boat *boat, int16_t *outNewSlot) {
  if (!scheduler || !boat || steps == 0) return false;
  if (steps > 16) return false; // Guarda: el buffer acquiredSems[] tiene capacidad maxima de 16.
  if (scheduler->listLength == 0 || !scheduler->slotOwner) return false;
  if ((SemaphoreHandle_t)scheduler->channelSlotsGuard == NULL) return false;

  int dir = (boat->origin == SIDE_LEFT) ? 1 : -1;
  int currentIndex = boat->currentSlot;
  if (currentIndex < 0 || currentIndex >= scheduler->listLength) return false;
  if (startIndex < 0 || startIndex >= scheduler->listLength) return false;

  int targetIndex = currentIndex + dir * steps;
  if (targetIndex < 0 || targetIndex >= scheduler->listLength) return false;

  int startReserve = currentIndex + dir;
  if (!ship_scheduler_wait_for_slot_range(scheduler, startReserve, steps, dir, boat)) return false;

  if (xSemaphoreTake((SemaphoreHandle_t)scheduler->channelSlotsGuard, pdMS_TO_TICKS(100)) != pdTRUE) {
    return false;
  }

  // Verificar que todas las casillas del rango [startReserve..targetIndex] esten libres.
  int idx = startReserve;
  for (uint8_t s = 0; s < steps; s++) {
    if (scheduler->slotOwner[idx] != 0 && scheduler->slotOwner[idx] != boat->id) {
      xSemaphoreGive((SemaphoreHandle_t)scheduler->channelSlotsGuard);
      return false;
    }
    idx += dir;
  }

  // Adquirir el semaforo de TODAS las casillas intermedias y la casilla destino
  // ANTES de liberar el origen. Esto evita que otro barco se cuele entre medio
  // (bug de adelantamiento): mientras el barco "viaja" por el rango, cada casilla
  // que atraviesa queda bloqueada hasta que el origen se libera.
  SemaphoreHandle_t acquiredSems[16]; // max steps razonable
  uint8_t acquiredCount = 0;
  idx = startReserve;
  for (uint8_t s = 0; s < steps; s++) {
    if ((uint16_t)idx != (uint16_t)currentIndex) { // El origen se libera despues; no tomarlo.
      SemaphoreHandle_t sem = ship_scheduler_get_slot_semaphore((uint16_t)idx);
      if (!sem || xSemaphoreTake(sem, 0) != pdTRUE) {
        // No se pudo adquirir esta casilla: liberar las ya tomadas y abortar.
        for (uint8_t t = 0; t < acquiredCount; t++) xSemaphoreGive(acquiredSems[t]);
        xSemaphoreGive((SemaphoreHandle_t)scheduler->channelSlotsGuard);
        return false;
      }
      acquiredSems[acquiredCount++] = sem;
      ship_scheduler_lock_slot_owner(scheduler, (uint16_t)idx, boat->id);
    }
    idx += dir;
  }

  // Ahora que todas las casillas del trayecto estan reservadas, liberar el origen.
  if (scheduler->slotOwner[currentIndex] == boat->id) {
    ship_scheduler_unlock_slot_owner(scheduler, (uint16_t)currentIndex);
    SemaphoreHandle_t currentSem = ship_scheduler_get_slot_semaphore((uint16_t)currentIndex);
    if (currentSem) xSemaphoreGive(currentSem);
  }

  // Liberar las casillas intermedias (todas menos la destino): el barco ya no
  // las ocupa fisicamente, solo ocupa su casilla final.
  idx = startReserve;
  for (uint8_t s = 0; s < steps - 1; s++) {
    ship_scheduler_unlock_slot_owner(scheduler, (uint16_t)idx);
    SemaphoreHandle_t sem = ship_scheduler_get_slot_semaphore((uint16_t)idx);
    if (sem) xSemaphoreGive(sem);
    idx += dir;
  }
  // idx apunta ahora a targetIndex, que permanece bloqueado por este barco.

  xSemaphoreGive((SemaphoreHandle_t)scheduler->channelSlotsGuard);

  if (outNewSlot) *outNewSlot = (int16_t)targetIndex;
  return true;
}

static bool candidate_is_better(ShipAlgo algo, const Boat *candidate, const Boat *best) { // Compara dos candidatos.
  if (!candidate) return false; // Valida candidato.
  if (!best) return true; // Si no hay mejor actual, gana candidato.

  if (algo == ALG_FCFS || algo == ALG_RR) { // FCFS y RR priorizan llegada.
    return candidate->arrivalOrder < best->arrivalOrder; // Menor llegada es mejor.
  }

  if (algo == ALG_SJF) { // SJF usa servicio total.
    // Si un candidato no tiene serviceMillis calculado aun (placeholder 0), pierde contra cualquiera que si lo tenga.
    unsigned long candSvc = candidate->serviceMillis > 0 ? candidate->serviceMillis : ULONG_MAX;
    unsigned long bestSvc = best->serviceMillis > 0 ? best->serviceMillis : ULONG_MAX;
    if (candSvc != bestSvc) return candSvc < bestSvc; // Menor servicio gana.
    return candidate->arrivalOrder < best->arrivalOrder; // Desempata por llegada.
  }

  if (algo == ALG_STRN) { // STRN usa tiempo restante.
    unsigned long candRem = 0UL;
    unsigned long bestRem = 0UL;
    if (candidate->remainingMillis > 1) candRem = candidate->remainingMillis;
    else if (candidate->serviceMillis > 0) candRem = candidate->serviceMillis;
    else candRem = ship_scheduler_estimate_service_millis(gScheduler, candidate);

    if (best->remainingMillis > 1) bestRem = best->remainingMillis;
    else if (best->serviceMillis > 0) bestRem = best->serviceMillis;
    else bestRem = ship_scheduler_estimate_service_millis(gScheduler, best);

    if (candRem != bestRem) return candRem < bestRem; // Menor restante gana.
    if (candidate->stepSize != best->stepSize) return candidate->stepSize > best->stepSize;
    return candidate->arrivalOrder < best->arrivalOrder; // Desempata por llegada.
  }

  if (algo == ALG_EDF) { // EDF usa deadline.
    unsigned long now = millis();
    unsigned long candRem = candidate->deadlineMillis > now ? candidate->deadlineMillis - now : 0UL;
    unsigned long bestRem = best->deadlineMillis > now ? best->deadlineMillis - now : 0UL;
    if (candRem != bestRem) return candRem < bestRem; // Menor tiempo restante gana.
    return candidate->arrivalOrder < best->arrivalOrder; // Desempata por llegada.
  }

  if (algo == ALG_PRIORITY) { // Prioridad estatica.
    if (candidate->priority != best->priority) return candidate->priority > best->priority; // Mayor prioridad gana.
    return candidate->arrivalOrder < best->arrivalOrder; // Desempata por llegada.
  }

  return candidate->arrivalOrder < best->arrivalOrder; // Alternativa por llegada.
} // Fin de candidate_is_better.

static unsigned long ship_scheduler_boat_elapsed_millis(const Boat *boat) { // Calcula elapsed de un barco.
  if (!boat) return 0; // Valida puntero.
  if (boat->serviceMillis <= boat->remainingMillis) return 0; // Evita underflow.
  return boat->serviceMillis - boat->remainingMillis; // Retorna elapsed.
} // Fin de ship_scheduler_boat_elapsed_millis.

static unsigned long ship_scheduler_estimate_service_millis(const ShipScheduler *scheduler, const Boat *boat) { // Estima el tiempo de servicio.
  if (!scheduler || !boat) return 0; // Valida punteros.
  if (scheduler->channelLengthMeters == 0 || scheduler->boatSpeedMetersPerSec == 0) return 5000UL; // Fallback razonable.

  float fullChannelMs = ((float)scheduler->channelLengthMeters * 1000.0f) / (float)scheduler->boatSpeedMetersPerSec; // Tiempo total del canal.
  unsigned long baseMs = (unsigned long)(fullChannelMs + 0.5f); // Redondeo al ms.
  if (baseMs == 0) baseMs = 1; // Evita cero.

  float typeFactor = 1.0f; // Ajuste por tipo.
  switch (boat->type) { // Selecciona factor.
    case BOAT_NORMAL: typeFactor = 1.0f; break;
    case BOAT_PESQUERA: typeFactor = 0.6f; break;
    case BOAT_PATRULLA: typeFactor = 0.3f; break;
    default: typeFactor = 1.0f; break;
  }

  unsigned long estimated = (unsigned long)((float)baseMs * typeFactor); // Aplica factor.
  if (estimated == 0) estimated = baseMs; // Garantiza valor util.
  return estimated < 50 ? 50 : estimated; // Impone minimo operativo.
} // Fin de ship_scheduler_estimate_service_millis.
static unsigned long ship_scheduler_compute_default_deadline(const ShipScheduler *scheduler, const Boat *boat) { // Calcula deadline EDF por defecto.
  unsigned long now = millis(); // Base temporal actual.
  unsigned long serviceMs = boat && boat->serviceMillis > 0 ? boat->serviceMillis : ship_scheduler_estimate_service_millis(scheduler, boat); // Servicio real o estimado.
  if (serviceMs == 0) serviceMs = 50UL; // Evita deadlines nulos.
  return now + (serviceMs * 2UL); // Margen EDF conservador.
} // Fin de ship_scheduler_compute_default_deadline.

static void ship_scheduler_sync_primary_active(ShipScheduler *scheduler) { // Sincroniza el activo primario.
  if (!scheduler) return; // Valida scheduler.
  if (scheduler->activeCount > 0) {
    scheduler->activeBoat = scheduler->activeBoats[0]; // Usa el primer activo como primario.
    scheduler->hasActiveBoat = true; // Marca activo.
  } else {
    scheduler->activeBoat = NULL; // Limpia activo.
    scheduler->hasActiveBoat = false; // Limpia bandera.
  }
} // Fin de ship_scheduler_sync_primary_active.

static void ship_scheduler_sync_rr_permissions(ShipScheduler *scheduler) { // Sincroniza permisos de RR.
  if (!scheduler || scheduler->algorithm != ALG_RR) return; // Solo aplica en RR.
  for (uint8_t i = 0; i < scheduler->activeCount; i++) { // Recorre activos.
    Boat *active = scheduler->activeBoats[i]; // Toma activo.
    if (!active) continue; // Salta nulos.
    active->allowedToMove = (active == scheduler->activeBoat); // Solo el primario avanza.
  }
} // Fin de ship_scheduler_sync_rr_permissions.

static void ship_scheduler_promote_active_to_front(ShipScheduler *scheduler, Boat *boat) {
  if (!scheduler || !boat || scheduler->activeCount == 0) return;
  int foundIndex = -1;
  for (uint8_t i = 0; i < scheduler->activeCount; i++) {
    if (scheduler->activeBoats[i] == boat) {
      foundIndex = (int)i;
      break;
    }
  }
  if (foundIndex < 0) return;
  if (foundIndex > 0) {
    Boat *promoted = scheduler->activeBoats[foundIndex];
    for (int i = foundIndex; i > 0; i--) {
      scheduler->activeBoats[i] = scheduler->activeBoats[i - 1];
    }
    scheduler->activeBoats[0] = promoted;
  }
  scheduler->activeBoat = scheduler->activeBoats[0];
  scheduler->hasActiveBoat = (scheduler->activeBoat != NULL);
}

static void ship_scheduler_reset_active_quantum(ShipScheduler *scheduler) {
  if (!scheduler) return;
  scheduler->activeQuantumAccumulatedMillis = 0;
  scheduler->activeQuantumStartedAt = 0;
}

// Avanza el indice circular RR al siguiente activo y devuelve el barco elegido.
// Si ningun activo puede avanzar sus steps completos, devuelve el siguiente en
// orden circular de todas formas (fallback) para no bloquear la rotacion.
static Boat *ship_scheduler_rr_next_active(ShipScheduler *scheduler) {
  if (!scheduler || scheduler->activeCount == 0) return NULL;
  if (scheduler->activeCount == 1) {
    scheduler->rrTurnIndex = 0;
    return scheduler->activeBoats[0];
  }
  // Buscar desde el siguiente al actual en orden circular.
  uint8_t start = (uint8_t)((scheduler->rrTurnIndex + 1) % scheduler->activeCount);
  Boat *fallback = NULL;
  for (uint8_t offset = 0; offset < scheduler->activeCount; offset++) {
    uint8_t idx = (uint8_t)((start + offset) % scheduler->activeCount);
    Boat *candidate = scheduler->activeBoats[idx];
    if (!candidate || candidate->currentSlot < 0) continue;
    if (!fallback) fallback = candidate; // Primer candidato como respaldo.
    // Verificar si puede avanzar sus steps completos.
    int dir = (candidate->origin == SIDE_LEFT) ? 1 : -1;
    int endIndex = (candidate->origin == SIDE_LEFT) ? (int)(scheduler->listLength - 1) : 0;
    int remSlots = (candidate->origin == SIDE_LEFT)
                   ? (endIndex - candidate->currentSlot)
                   : (candidate->currentSlot - endIndex);
    if (remSlots <= 0) {
      // Barco en el final: que reciba el turno para terminar.
      scheduler->rrTurnIndex = idx;
      return candidate;
    }
    uint8_t need = (uint8_t)((remSlots < (int)candidate->stepSize) ? remSlots : candidate->stepSize);
    bool canMove = true;
    int checkSlot = candidate->currentSlot + dir;
    for (uint8_t s = 0; s < need; s++) {
      if (checkSlot < 0 || checkSlot >= (int)scheduler->listLength) { canMove = false; break; }
      if (scheduler->slotOwner && scheduler->slotOwner[checkSlot] != 0
          && scheduler->slotOwner[checkSlot] != candidate->id) { canMove = false; break; }
      checkSlot += dir;
    }
    if (canMove) {
      scheduler->rrTurnIndex = idx;
      return candidate;
    }
  }
  // Ninguno puede avanzar ahora: rotar al siguiente de todas formas (fallback).
  if (fallback) {
    for (uint8_t idx = 0; idx < scheduler->activeCount; idx++) {
      if (scheduler->activeBoats[idx] == fallback) { scheduler->rrTurnIndex = idx; break; }
    }
  }
  return fallback;
}

static void ship_scheduler_freeze_active_quantum(ShipScheduler *scheduler) {
  if (!scheduler) return;
  if (scheduler->activeQuantumStartedAt > 0) {
    unsigned long now = millis();
    if (now >= scheduler->activeQuantumStartedAt) {
      scheduler->activeQuantumAccumulatedMillis += now - scheduler->activeQuantumStartedAt;
    }
    scheduler->activeQuantumStartedAt = 0;
  }
}

static void ship_scheduler_resume_active_quantum(ShipScheduler *scheduler) {
  if (!scheduler) return;
  if (scheduler->activeQuantumStartedAt == 0) scheduler->activeQuantumStartedAt = millis();
}

static void ship_scheduler_add_active(ShipScheduler *scheduler, Boat *boat) { // Agrega barco activo.
  if (!scheduler || !boat) return; // Valida punteros.
  if (scheduler->activeCount >= MAX_BOATS) return; // Evita overflow.
  scheduler->activeBoats[scheduler->activeCount++] = boat; // Agrega al final.
  ship_scheduler_sync_primary_active(scheduler); // Actualiza activo primario.
  // El nuevo barco entra al final; el rrTurnIndex apuntara a el en el proximo turno
  // si se inserta antes del indice actual. No es necesario ajustar aqui porque
  // ship_scheduler_rr_next_active siempre avanza desde rrTurnIndex+1.
} // Fin de ship_scheduler_add_active.

static void ship_scheduler_remove_active(ShipScheduler *scheduler, Boat *boat) { // Quita barco activo.
  if (!scheduler || !boat || scheduler->activeCount == 0) return; // Valida estado.
  int removedIndex = -1;
  for (uint8_t i = 0; i < scheduler->activeCount; i++) {
    if (scheduler->activeBoats[i] == boat) {
      removedIndex = (int)i;
      for (uint8_t j = i + 1; j < scheduler->activeCount; j++) {
        scheduler->activeBoats[j - 1] = scheduler->activeBoats[j];
      }
      scheduler->activeBoats[scheduler->activeCount - 1] = NULL;
      scheduler->activeCount--;
      break;
    }
  }
    ship_scheduler_sync_primary_active(scheduler); // Actualiza activo primario.
  // Ajustar rrTurnIndex correctamente segun donde cayo el barco eliminado.
  if (scheduler->activeCount == 0) {
    scheduler->rrTurnIndex = 0;
  } else if (removedIndex >= 0 && (int)scheduler->rrTurnIndex > removedIndex) {
    // El barco eliminado estaba ANTES del indice actual: el array se comprimo,
    // el indice apunta ahora al barco siguiente al que deberia.
    scheduler->rrTurnIndex--;
  } else if (scheduler->rrTurnIndex >= scheduler->activeCount) {
    // Ajuste de seguridad: no dejar fuera de rango.
    scheduler->rrTurnIndex = (uint8_t)(scheduler->activeCount - 1);
  }
} // Fin de ship_scheduler_remove_active.

static bool ship_scheduler_is_tico_safe(const ShipScheduler *scheduler, const Boat *candidate) { // Evalua seguridad TICO.
  if (!scheduler || !candidate) return false; // Valida punteros.
  if (scheduler->activeCount == 0) return true; // Sin activos, no hay riesgo.

  BoatSide side = candidate->origin; // Lado del candidato.
  for (uint8_t i = 0; i < scheduler->activeCount; i++) { // Evalua contra cada activo.
    Boat *active = scheduler->activeBoats[i];
    if (!active) continue; // Salta nulos.
    if (active->origin != side) return false; // Evita sentidos opuestos.
    if (!active->allowedToMove) return false; // Evita iniciar si un activo esta detenido.

    unsigned long elapsed = ship_scheduler_boat_elapsed_millis(active); // Progreso del activo.
    float pairFactor = 1.0f;
    if (scheduler) {
      BoatType at = active->type;
      BoatType ct = candidate->type;
      if (at >= 0 && at < 3 && ct >= 0 && ct < 3) pairFactor = scheduler->ticoMarginFactor[at][ct];
    }
    float serviceScale = 1.0f;
    float speedScale = 1.0f;
    if (scheduler && scheduler->boatSpeedMetersPerSec > 0) speedScale = 18.0f / (float)scheduler->boatSpeedMetersPerSec;
    unsigned long minGapMs = (unsigned long)((float)TICO_INITIAL_MARGIN * pairFactor * serviceScale * speedScale);
    if (elapsed < minGapMs) return false; // Evita solapamiento inicial.

    if (candidate->serviceMillis < active->serviceMillis && active->serviceMillis > 0) { // Riesgo si el candidato es mas rapido.
      float gapFraction = (float)minGapMs / (float)active->serviceMillis; // Margen relativo.
      if (gapFraction > 0.02f) gapFraction = TICO_SAFETY_MARGIN; // Evita margenes excesivos.
      float ratio = (float)candidate->serviceMillis / (float)active->serviceMillis; // Relacion de tiempos.
      float requiredElapsed = (1.0f - (1.0f - gapFraction) * ratio) * (float)active->serviceMillis; // Elapsed minimo.
      if ((float)elapsed < requiredElapsed) return false; // No es seguro iniciar.
    }
  }

  return true; // Cumple condiciones de seguridad.
} // Fin de ship_scheduler_is_tico_safe.

// Libera un rango de casillas [startIndex, startIndex + steps -1] que pertenecian al barco.
void ship_scheduler_release_range(ShipScheduler *scheduler, int startIndex, uint8_t steps, Boat *boat) {
  if (!scheduler || !boat || steps == 0) return;
  if (scheduler->listLength == 0 || !scheduler->slotOwner) return;
  if ((SemaphoreHandle_t)scheduler->channelSlotsGuard == NULL) return;

  if (xSemaphoreTake((SemaphoreHandle_t)scheduler->channelSlotsGuard, pdMS_TO_TICKS(100)) != pdTRUE) return;
  int dir = (boat->origin == SIDE_LEFT) ? 1 : -1;
  int endIndex = startIndex + dir * (steps - 1);
  if (startIndex < 0 || startIndex >= scheduler->listLength || endIndex < 0 || endIndex >= scheduler->listLength) {
    xSemaphoreGive((SemaphoreHandle_t)scheduler->channelSlotsGuard);
    // Notify active boats that slots changed so they can retry without busy-waiting
      for (uint8_t i = 0; i < scheduler->activeCount; i++) { 
        Boat *active = scheduler->activeBoats[i]; 
        if (!active) continue; 
        if (active->taskHandle) safe_task_notify(active->taskHandle, NOTIF_CMD_SLOT_UPDATE); 
      } 
    return;
  }
  int idx = startIndex;
  for (uint8_t s = 0; s < steps; s++) {
    if (scheduler->slotOwner[idx] == boat->id) {
      ship_scheduler_unlock_slot_owner(scheduler, (uint16_t)idx);
      SemaphoreHandle_t slotSem = ship_scheduler_get_slot_semaphore((uint16_t)idx);
      if (slotSem) xSemaphoreGive(slotSem);
    }
    idx += dir;
  }
  xSemaphoreGive((SemaphoreHandle_t)scheduler->channelSlotsGuard);
}

void ship_scheduler_load_channel_config(ShipScheduler *scheduler, const char *path) { // Lee channel_config.txt con formato de comandos
  if (!scheduler) return;
  if (!path) path = "channel_config.txt"; // Ruta por defecto relativa al ejecutable

  FILE *f = fopen(path, "r");
  if (!f) {
    // Usa valores por defecto
    scheduler->listLength = DEFAULT_LIST_LENGTH;
    scheduler->visualChannelLength = DEFAULT_VISUAL_CHANNEL_LENGTH;
    ship_logf("[CONFIG] No se encontro %s, usando defaults list=%u visual=%u\n", path, scheduler->listLength, scheduler->visualChannelLength);
    return;
  }

  char line[160];
  uint16_t applied = 0;
  while (fgets(line, sizeof(line), f)) {
    // Trim basico y salto de comentarios
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t')) {
      line[len - 1] = '\0';
      len--;
    }
    char *cursor = line;
    while (*cursor == ' ' || *cursor == '\t') cursor++;
    if (*cursor == '\0' || *cursor == '#') continue;
    process_serial_command(scheduler, cursor);
    applied++;
  }
  fclose(f);
  ship_logf("[CONFIG] Cargado %s (%u lineas)\n", path, applied);
}

void ship_scheduler_demo_clear(ShipScheduler *scheduler) {
  if (!scheduler) return;
  scheduler->demoCount = 0;
}

bool ship_scheduler_demo_add(ShipScheduler *scheduler, BoatSide side, BoatType type, uint8_t priority, uint8_t stepSize) {
  if (!scheduler) return false;
  if (scheduler->demoCount >= MAX_BOATS) return false;
  uint8_t idx = scheduler->demoCount++;
  scheduler->demoSide[idx] = side;
  scheduler->demoType[idx] = type;
  scheduler->demoPrio[idx] = priority;
  scheduler->demoStep[idx] = stepSize;
  return true;
}

void ship_scheduler_rebuild_slots(ShipScheduler *scheduler) {
  if (!scheduler) return;
  if (scheduler->listLength == 0) scheduler->listLength = DEFAULT_LIST_LENGTH;
  if (scheduler->visualChannelLength == 0) scheduler->visualChannelLength = scheduler->listLength;

  ship_scheduler_destroy_slot_resources();

  if (scheduler->slotOwner) {
    free(scheduler->slotOwner);
    scheduler->slotOwner = NULL;
  }

  scheduler->slotOwner = (uint8_t *)malloc(scheduler->listLength * sizeof(uint8_t));
  if (scheduler->slotOwner) {
    for (uint16_t i = 0; i < scheduler->listLength; i++) scheduler->slotOwner[i] = 0;
  }
  ship_scheduler_init_slot_resources(scheduler->listLength);
  if (!scheduler->channelSlotsGuard) scheduler->channelSlotsGuard = (void *)xSemaphoreCreateMutex();
}

void ship_scheduler_set_list_length(ShipScheduler *scheduler, uint16_t listLength) {
  if (!scheduler) return;
  scheduler->listLength = listLength > 0 ? listLength : DEFAULT_LIST_LENGTH;
}

uint16_t ship_scheduler_get_list_length(const ShipScheduler *scheduler) {
  return scheduler ? scheduler->listLength : DEFAULT_LIST_LENGTH;
}

void ship_scheduler_set_visual_channel_length(ShipScheduler *scheduler, uint16_t visualLength) {
  if (!scheduler) return;
  scheduler->visualChannelLength = visualLength > 0 ? visualLength : scheduler->listLength;
}

uint16_t ship_scheduler_get_visual_channel_length(const ShipScheduler *scheduler) {
  return scheduler ? scheduler->visualChannelLength : DEFAULT_VISUAL_CHANNEL_LENGTH;
}

float ship_scheduler_get_list_to_visual_ratio(const ShipScheduler *scheduler) {
  if (!scheduler) return 1.0f;
  if (scheduler->listLength == 0) return 1.0f;
  return (float)scheduler->visualChannelLength / (float)scheduler->listLength; // ratio visual per list-slot
}

static void boatTask(void *pv) { // Tarea FreeRTOS que ejecuta un barco. 
  Boat *b = (Boat *)pv; // Convierte el parametro a Boat. 
  if (!b) { // Si es nulo. 
    vTaskDelete(NULL); // Elimina la tarea. 
    return; // Termina. 
  } 

  bool running = false; // Estado de ejecucion.
  unsigned long lastTickAt = millis(); // Marca del ultimo descuento real.
  // Variables para movimiento en lista
  int dir = 0;
  int16_t currentSlot = -1;
  unsigned long perMoveMs = 0;
  unsigned long moveAccum = 0;
  int totalSlotsToTravel = 0;
  int movesCount = 0;
  // visualOnly: true cuando el tiempo de servicio ya se agoto pero el barco
  // todavia no llego fisicamente al final del canal. En este estado el barco
  // sigue moviendose casilla a casilla pero ya NO descuenta remainingMillis
  // (que permanece en 1 para que el scheduler no lo finalize prematuramente).
  // CRITICO: remainingMillis=1 se pone UNA SOLA VEZ al entrar en visualOnly y
  // nunca vuelve a tocarse aqui; el scheduler es el unico que lo puede poner a 0.
  bool visualOnly = false;
  ship_logf("[BOAT TASK] Barco #%u iniciada. serviceMillis=%lu\n", b->id, b->serviceMillis); // Depuracion: inicio de tarea.
  while (b->remainingMillis > 0) { // Mientras quede tiempo.
    uint32_t cmd = 0; // Comando recibido.
    if (!running) { // Si no esta corriendo.
      ship_logf("[BOAT TASK #%u] Esperando NOTIF_CMD_RUN (remainingMillis=%lu)...\n", b->id, b->remainingMillis); // Depuracion: esperando comando.
      xTaskNotifyWait(0x00, 0xFFFFFFFF, &cmd, portMAX_DELAY); // Espera notificacion.
      ship_logf("[BOAT TASK #%u] Recibido comando: %u\n", b->id, cmd); // Depuracion: comando recibido.
      if (cmd == NOTIF_CMD_TERMINATE) break; // Si terminate, sale.
      if (cmd == NOTIF_CMD_RUN) { // Si run, inicia.
        running = true; // Marca ejecucion activa.
        b->allowedToMove = true; // Permite avanzar.
        // FIX #3: Resetear lastTickAt en CADA RUN, no solo en el primero.
        // Si no se resetea, al reanudar tras una pausa (sensor u otro motivo)
        // el 'elapsed' calculado incluye todo el tiempo de pausa y se descuenta
        // erroneamente de remainingMillis. Esto afecta especialmente a EDF.
        lastTickAt = millis(); // Reinicia base temporal en cada reanudacion.
        // Si ya estamos en modo visual-only, garantizar que remainingMillis=1
        // para que el scheduler no mate el barco en el proximo tick de update.
        if (visualOnly && b->remainingMillis == 0) b->remainingMillis = 1;
        ship_logf("[BOAT TASK #%u] RUN recibido. running=true, lastTickAt=%lu, remainingMillis=%lu\n", b->id, lastTickAt, b->remainingMillis); // Depuracion: iniciando ejecucion.
        // Preparar parametros de movimiento en lista
        if (gScheduler) {
          ShipScheduler *s = gScheduler;
          if (s->listLength > 1) {
              totalSlotsToTravel = s->listLength - 1;
            movesCount = (totalSlotsToTravel + b->stepSize - 1) / b->stepSize;
            if (movesCount <= 0) movesCount = 1;
            perMoveMs = b->serviceMillis / (unsigned long)movesCount;
            if (perMoveMs == 0) perMoveMs = 1;
          } else {
            totalSlotsToTravel = 0;
            movesCount = 1;
            perMoveMs = b->serviceMillis;
          }
          dir = (b->origin == SIDE_LEFT) ? 1 : -1;
          // Intentar reservar la casilla de entrada SOLO si el barco no esta
          // ya ubicado en el canal (regreso de preempcion: currentSlot >= 0).
          int entryIndex = (b->origin == SIDE_LEFT) ? 0 : (int)(s->listLength - 1);
          ship_logf("[BOAT TASK #%u] movesCount=%d perMoveMs=%lu totalSlots=%d stepSize=%u currentSlot=%d\n", b->id, movesCount, perMoveMs, totalSlotsToTravel, b->stepSize, b->currentSlot);
          if (b->currentSlot < 0) { // Solo reservar entrada si aun no esta en el canal.
            while (b->remainingMillis > 0 && b->currentSlot < 0) {
              if (ship_scheduler_try_reserve_range(s, entryIndex, 1, b)) {
                b->currentSlot = entryIndex;
                currentSlot = entryIndex;
                break;
              }
              // Espera hasta que haya cambio en los slots o reciba PAUSE/TERMINATE (sin busy-wait)
              uint32_t innerCmd = 0;
              xTaskNotifyWait(0x00, 0xFFFFFFFF, &innerCmd, portMAX_DELAY);
              if (innerCmd == NOTIF_CMD_TERMINATE) break;
              if (innerCmd == NOTIF_CMD_PAUSE) {
                running = false;
                b->allowedToMove = false;
                break;
              }
            }
          } else {
            // FIX #2: El barco ya esta en el canal (viene de preempcion o regreso
            // de emergencia). Sincronizar currentSlot local Y recalcular perMoveMs
            // desde remainingMillis actual, no desde serviceMillis original.
            // Sin este recalculo, el barco se mueve a velocidad incorrecta despues
            // de ser desalojado por STRN (el perMoveMs refleja el canal completo
            // pero ahora solo le quedan N casillas proporcionales a remainingMillis).
            currentSlot = b->currentSlot;
            if (s->listLength > 1 && b->stepSize > 0) {
              int endIdx = (b->origin == SIDE_LEFT) ? (int)(s->listLength - 1) : 0;
              int remSlots = (b->origin == SIDE_LEFT)
                             ? (endIdx - b->currentSlot)
                             : (b->currentSlot - endIdx);
              if (remSlots > 0) {
                movesCount = (remSlots + (int)b->stepSize - 1) / (int)b->stepSize;
                if (movesCount <= 0) movesCount = 1;
                // Usar remainingMillis real si esta calculado (>1 es real; ==1 podria ser placeholder)
                unsigned long baseMs = (b->remainingMillis > 1) ? b->remainingMillis : b->serviceMillis;
                if (baseMs == 0) baseMs = ship_scheduler_estimate_service_millis(s, b);
                perMoveMs = baseMs / (unsigned long)movesCount;
                if (perMoveMs == 0) perMoveMs = 1;
                ship_logf("[BOAT TASK #%u] Recalculo tras preempcion: remSlots=%d movesCount=%d perMoveMs=%lu remainingMillis=%lu\n",
                          b->id, remSlots, movesCount, perMoveMs, b->remainingMillis);
              }
            }
          }
        }
      }
      continue; // Repite el ciclo.
    } 

    const unsigned long slice = 50; // Subpaso interno. 
    bool interrupted = false; // Indica si se interrumpio el paso por pausa/termino. 
    xTaskNotifyWait(0x00, 0xFFFFFFFF, &cmd, pdMS_TO_TICKS(slice)); // Espera o recibe comando.

    // Procesar comandos ANTES de descontar tiempo para que PAUSE/TERMINATE
    // tengan efecto inmediato sin importar cuanto tiempo haya pasado.
    if (cmd == NOTIF_CMD_TERMINATE) { // Si terminate. 
      b->remainingMillis = 0; // Fuerza fin. 
      running = false; // Detiene ejecucion. 
      b->allowedToMove = false; // Bloquea movimiento. 
      if (gScheduler) ship_display_render_forced(gScheduler); // Refresca para mostrar estado terminado.
      break; // Sale del while principal para terminar la tarea. 
    } 
    if (cmd == NOTIF_CMD_PAUSE) { // Si pause.
      ship_logf("[BOAT TASK #%u] PAUSE recibido. running=false, remainingMillis=%lu\n", b->id, b->remainingMillis); // Depuracion: pausa.
      running = false; // Detiene ejecucion.
      b->allowedToMove = false; // Bloquea movimiento.
      interrupted = true; // Marca interrupcion.
      moveAccum = 0; // FIX #11: Resetear acumulador para no moverse de inmediato al reanudar.
      if (gScheduler) ship_display_render_forced(gScheduler); // Refresca para mostrar pausa.
      continue; // Vuelve al inicio del bucle para esperar NOTIF_CMD_RUN nuevamente.
    } 

    unsigned long now = millis(); // Marca temporal actual.
    unsigned long elapsed = now - lastTickAt; // Tiempo real transcurrido desde el ultimo descuento.
    lastTickAt = now; // Actualiza base temporal.

    // Descontar tiempo de servicio solo si aun no expiro.
    if (!visualOnly && elapsed > 0) {
      if (elapsed >= b->remainingMillis) {
        // Tiempo de servicio agotado: entrar en modo visual-only.
        // Poner remainingMillis=1 UNA SOLA VEZ para mantener vivo el barco
        // mientras recorre las casillas que le faltan visualmente.
        // El scheduler detectara visualOnly=true y no finalizara el barco
        // hasta que llegue al final del canal.
        b->remainingMillis = 0;
        if (b->currentSlot >= 0 && gScheduler) {
          ShipScheduler *s = gScheduler;
          int endIndex = (b->origin == SIDE_LEFT) ? (int)(s->listLength - 1) : 0;
          bool needsVisual = (b->origin == SIDE_LEFT)
                             ? (b->currentSlot < endIndex)
                             : (b->currentSlot > endIndex);
          if (needsVisual) {
            visualOnly = true;
            b->remainingMillis = 1; // Mantiene vivo; no se vuelve a tocar aqui.
          }
          // Si ya esta en el final, remainingMillis queda en 0 y el scheduler lo finalizara.
        }
      } else {
        b->remainingMillis -= elapsed; // Reduce por tiempo real transcurrido.
      }
    }
    // En modo visualOnly no tocamos remainingMillis: permanece en 1 hasta que
    // el barco llegue al final y lo ponga a 0 el propio loop de movimiento.

    if (interrupted || !running) { // Si se interrumpio o quedo pausado. 
      continue; // No acumula tiempo de movimiento. 
    } 

    // Acumula para decidir mover de casilla.
    if (b->allowedToMove && perMoveMs > 0 && b->currentSlot >= 0) {
      moveAccum += elapsed;
    }

    // Intentar mover cuando se acumulo suficiente tiempo
    if (b->allowedToMove && b->currentSlot >= 0 && moveAccum >= perMoveMs) {
      ShipScheduler *s = gScheduler;
      if (s && s->algorithm == ALG_RR && s->activeBoat != b) {
        moveAccum = 0;
        continue;
      }
      int16_t slot = b->currentSlot;
      int endIndex = (b->origin == SIDE_LEFT) ? (s->listLength - 1) : 0;
      int remainingSlots = (b->origin == SIDE_LEFT) ? (s->listLength - 1 - slot) : (slot - 0);
      if (remainingSlots <= 0) {
        // Llego al final; libera su casilla y termina.
        ship_scheduler_release_range(s, slot, 1, b);
        b->currentSlot = -1;
        b->remainingMillis = 0; // Senaliza al scheduler que puede finalizarlo.
        break;
      }

      // Decide cuantas casillas mover en este impulso sin sobrepasar el final.
      uint8_t desiredSteps = (uint8_t)((remainingSlots < b->stepSize) ? remainingSlots : b->stepSize);

      int startReserve = slot + dir; // primer casilla a reservar
      ship_logf("[BOAT TASK #%u] desiredSteps=%u remainingSlots=%d startReserve=%d\n", b->id, desiredSteps, remainingSlots, startReserve);

      if (desiredSteps == 0) {
        continue;
      }

      int16_t newSlot = -1;

      if (ship_scheduler_try_move_range(s, startReserve, desiredSteps, b, &newSlot)) {
        b->currentSlot = newSlot;
        currentSlot = newSlot;
        moveAccum = 0; // reinicia acumulador
        ship_logf("[BOAT TASK #%u] moved to slot %d\n", b->id, newSlot);
        ship_display_render_forced(s);
      } else {
        ship_logf("[BOAT TASK #%u] blocked waiting for slot at startReserve=%d desiredSteps=%u\n", b->id, startReserve, desiredSteps);
        moveAccum = 0;
        // En RR ceder el turno para que el scheduler rote al siguiente activo.
        if (gScheduler && gScheduler->algorithm == ALG_RR) {
          ship_scheduler_yield_active_for_rr(gScheduler, b);
          running = false;
          b->allowedToMove = false;
          continue;
        }
        // En otros algoritmos nos quedamos esperando por SLOT_UPDATE.
      }
    }
  } 

  if (gScheduler && b->currentSlot >= 0) { // Libera casilla si quedo ocupada.
    ship_scheduler_release_range(gScheduler, b->currentSlot, 1, b);
    b->currentSlot = -1;
  }

  if (gScheduler) { // Si hay scheduler global. 
    ship_scheduler_notify_boat_finished(gScheduler, b); // Notifica finalizacion. 
    // Una ultima actualizacion de pantalla FORZADA para reflejar el cambio inmediato (ignora limite de refresco)
    ship_display_render_forced(gScheduler);
  } 

  destroyBoat(b); // Libera el Boat. 
  vTaskDelete(NULL); // Elimina la tarea. 
} // Fin de boatTask. 

void ship_scheduler_begin(ShipScheduler *scheduler) { // Inicializa el scheduler. 
  if (!scheduler) return; // Valida puntero. 
  if (scheduler->rrQuantumMillis < 100) scheduler->rrQuantumMillis = 1200; // RR por defecto.
  if (scheduler->fairnessWindowW == 0) scheduler->fairnessWindowW = 2; // W por defecto.
  if (scheduler->signIntervalMillis < 1000) scheduler->signIntervalMillis = 8000; // Letrero por defecto.
  if (scheduler->maxReadyQueueConfigured == 0 || scheduler->maxReadyQueueConfigured > MAX_BOATS) scheduler->maxReadyQueueConfigured = MAX_BOATS; // Limite de cola por defecto.
  if (scheduler->channelLengthMeters == 0) scheduler->channelLengthMeters = 120; // Largo por defecto del canal.
  if (scheduler->boatSpeedMetersPerSec == 0) scheduler->boatSpeedMetersPerSec = 18; // Velocidad por defecto.
  scheduler->activeQuantumStartedAt = 0;
  scheduler->flowMode = FLOW_TICO; // Flujo por defecto (sin control).
  // Configuracion fija de margen TICO por par (activo, candidato).
  // Filas (activo): NORMAL, PESQUERA, PATRULLA.
  // Columnas (candidato): NORMAL, PESQUERA, PATRULLA.
  scheduler->ticoMarginFactor[BOAT_NORMAL][BOAT_NORMAL] = 1.00f;
  scheduler->ticoMarginFactor[BOAT_NORMAL][BOAT_PESQUERA] = 1.20f;
  scheduler->ticoMarginFactor[BOAT_NORMAL][BOAT_PATRULLA] = 1.35f;

  scheduler->ticoMarginFactor[BOAT_PESQUERA][BOAT_NORMAL] = 0.40f;
  scheduler->ticoMarginFactor[BOAT_PESQUERA][BOAT_PESQUERA] = 0.50f;
  scheduler->ticoMarginFactor[BOAT_PESQUERA][BOAT_PATRULLA] = 1.20f;

  scheduler->ticoMarginFactor[BOAT_PATRULLA][BOAT_NORMAL] = 0.35f;
  scheduler->ticoMarginFactor[BOAT_PATRULLA][BOAT_PESQUERA] = 0.30f;
  scheduler->ticoMarginFactor[BOAT_PATRULLA][BOAT_PATRULLA] = 0.30f;
  scheduler->signDirection = SIDE_LEFT; // Letrero por defecto a la izquierda.
  scheduler->signLastSwitchAt = millis(); // Marca inicial de letrero.
  scheduler->fairnessCurrentSide = SIDE_LEFT; // Lado inicial de equidad.
  scheduler->fairnessPassedInWindow = 0; // Reinicia ventana de equidad.
  scheduler->collisionDetections = 0; // Reinicia contador de colisiones.
  // Inicializa sensor e interrupciones
  scheduler->sensorActive = false; // Sensor deshabilitado por defecto.
  scheduler->proximityThresholdCm = 10; // Umbral de 10cm por defecto.
  scheduler->proximityCurrentDistanceCm = 999; // Distancia inicial "lejana".
  scheduler->proximityDistanceIsSimulated = false; // Distancia real por defecto.
  scheduler->emergencyMode = EMERGENCY_NONE; // Sin emergencia.
  scheduler->emergencyStartedAt = 0; // Sin timestamp.
  scheduler->emergencyDispatchBlockedLogged = false; // No hemos registrado mensaje de despacho bloqueado.
  scheduler->gateLeftClosed = 0; // Puerta izquierda abierta.
  scheduler->gateRightClosed = 0; // Puerta derecha abierta.
  scheduler->gateLockDurationMs = 5000; // Cierre de 5 segundos por defecto.
  scheduler->demoCount = 0; // Reinicia manifiesto demo.
  ship_scheduler_clear(scheduler); // Limpia estado. 
  // Cargar configuracion de canal (listLength y visualChannelLength)
#ifndef ARDUINO
  ship_scheduler_load_channel_config(scheduler, "channel_config.txt");
#endif
  // Inicializa slots del canal segun la config
  ship_scheduler_rebuild_slots(scheduler);
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
      safe_task_notify(b->taskHandle, NOTIF_CMD_TERMINATE); // Ordena terminar. 
    } else { // Si no hay tarea. 
      destroyBoat(b); // Libera memoria. 
    } 
    scheduler->readyQueue[i] = NULL; // Limpia el slot. 
  } 
  scheduler->readyCount = 0; // Resetea la cola. 

  for (uint8_t i = 0; i < scheduler->activeCount; i++) { // Recorre activos.
    Boat *active = scheduler->activeBoats[i]; // Toma el activo.
    if (!active) continue; // Salta nulos.
    active->cancelled = true; // Marca cancelacion.
    if (active->taskHandle) { // Si hay tarea.
      safe_task_notify(active->taskHandle, NOTIF_CMD_TERMINATE); // Termina.
    } else { // Si no hay tarea.
      destroyBoat(active); // Libera memoria.
    }
    scheduler->activeBoats[i] = NULL; // Limpia el slot.
  }
  scheduler->activeCount = 0; // Resetea activos.
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
  scheduler->activeQuantumAccumulatedMillis = 0; // Resetea quantum acumulado.
  scheduler->rrTurnIndex = 0; // Resetea indice circular RR.
  scheduler->fairnessPassedInWindow = 0; // Resetea ventana de equidad.
  scheduler->signLastSwitchAt = millis(); // Reinicia reloj de letrero.
  // Liberar estructura de slots si existe
  ship_scheduler_destroy_slot_resources();
  if (scheduler->slotOwner) {
    free(scheduler->slotOwner);
    scheduler->slotOwner = NULL;
  }
  if (scheduler->channelSlotsGuard) {
    vSemaphoreDelete((SemaphoreHandle_t)scheduler->channelSlotsGuard);
    scheduler->channelSlotsGuard = NULL;
  }
  ship_scheduler_rebuild_slots(scheduler); // Restaura recursos de canal para poder agregar barcos otra vez.
} // Fin de ship_scheduler_clear. 

static int findIndexForAlgo(ShipAlgo algo, Boat *readyQueue[], uint8_t readyCount) { // Selecciona indice segun algoritmo. 
  return findIndexForAlgoAndSide(algo, readyQueue, readyCount, false, SIDE_LEFT); // Delega selector sin filtro de lado.
} // Fin de findIndexForAlgo. 

static void ship_scheduler_tick_sign(ShipScheduler *scheduler) { // Actualiza letrero por tiempo.
  if (!scheduler || scheduler->flowMode != FLOW_SIGN) return; // Aplica solo en modo letrero.
  if (scheduler->signIntervalMillis == 0) return; // Evita intervalos invalidos.
  unsigned long now = millis(); // Lee reloj actual.
  if (now - scheduler->signLastSwitchAt >= scheduler->signIntervalMillis) { // Verifica vencimiento.
    scheduler->signDirection = opposite_side(scheduler->signDirection); // Alterna lado del letrero.
    scheduler->signLastSwitchAt = now; // Guarda el instante del cambio.
    ship_logf("Letrero -> %s\n", boatSideName(scheduler->signDirection)); // Reporta cambio.
    FLOW_LOG(scheduler, "[FLOW][SIGN] Cambio de direccion por tiempo. Nueva=%s\n", boatSideName(scheduler->signDirection)); // Traza de cambio.
  }
} // Fin de ship_scheduler_tick_sign.

static int ship_scheduler_select_next_index(ShipScheduler *scheduler) { // Selecciona el siguiente indice segun algoritmo y flujo.
  if (!scheduler || scheduler->readyCount == 0) return -1; // Valida estado.

  if (scheduler->flowMode == FLOW_TICO) { // Tico no restringe el lado.
    FLOW_LOG(scheduler, "[FLOW][TICO] Seleccion libre por algoritmo %s\n", ship_scheduler_get_algorithm_label(scheduler)); // Traza de seleccion tico.
    return findIndexForAlgo(scheduler->algorithm, scheduler->readyQueue, scheduler->readyCount); // Elige por algoritmo sin bloquear por lado.
  }

  if (scheduler->flowMode == FLOW_SIGN) { // Modo letrero.
    BoatSide allowed = scheduler->signDirection; // Lado permitido por letrero.
    BoatSide fallback = opposite_side(allowed); // Lado alterno para no bloquear flujo.
    if (queue_has_side(scheduler, allowed)) { // Si hay barcos del lado permitido.
      FLOW_LOG(scheduler, "[FLOW][SIGN] Letrero=%s, seleccionando ese lado\n", boatSideName(allowed)); // Traza lado permitido.
      return findIndexForAlgoAndSide(scheduler->algorithm, scheduler->readyQueue, scheduler->readyCount, true, allowed); // Elige permitido.
    }
    FLOW_LOG(scheduler, "[FLOW][SIGN] Letrero=%s sin barcos; fallback a %s\n", boatSideName(allowed), boatSideName(fallback)); // Traza alternativa.
    return findIndexForAlgoAndSide(scheduler->algorithm, scheduler->readyQueue, scheduler->readyCount, true, fallback); // Garantiza flujo.
  }

  if (scheduler->flowMode == FLOW_FAIRNESS) { // Modo equidad con ventana W.
    uint8_t w = scheduler->fairnessWindowW == 0 ? 1 : scheduler->fairnessWindowW; // Normaliza W.
    BoatSide current = scheduler->fairnessCurrentSide; // Lado actual de la ventana.
    BoatSide opposite = opposite_side(current); // Lado alterno.
    bool hasCurrent = queue_has_side(scheduler, current); // Disponibilidad lado actual.
    bool hasOpposite = queue_has_side(scheduler, opposite); // Disponibilidad lado opuesto.

    if (hasCurrent && !hasOpposite) { // Solo hay barcos del lado actual.
      FLOW_LOG(scheduler, "[FLOW][FAIR] Solo hay barcos en %s; continuo sin alternar\n", boatSideName(current)); // Traza continuidad.
      return findIndexForAlgoAndSide(scheduler->algorithm, scheduler->readyQueue, scheduler->readyCount, true, current); // Mantiene flujo.
    }

    if (!hasCurrent && hasOpposite) { // Solo hay barcos del lado opuesto.
      scheduler->fairnessCurrentSide = opposite; // Cambia lado activo para no bloquear.
      scheduler->fairnessPassedInWindow = 0; // Reinicia ventana.
      FLOW_LOG(scheduler, "[FLOW][FAIR] Lado %s vacio; cambio inmediato a %s\n", boatSideName(current), boatSideName(opposite)); // Traza cambio por vacio.
      return findIndexForAlgoAndSide(scheduler->algorithm, scheduler->readyQueue, scheduler->readyCount, true, opposite); // Atiende lado disponible.
    }

    if (!hasCurrent && !hasOpposite) return -1; // No hay barcos en cola.

    if (scheduler->fairnessPassedInWindow >= w) { // Si se completo la cuota W.
      scheduler->fairnessCurrentSide = opposite; // Alterna lado.
      scheduler->fairnessPassedInWindow = 0; // Reinicia conteo de ventana.
      FLOW_LOG(scheduler, "[FLOW][FAIR] Se cumplio W=%u; alterno a %s\n", w, boatSideName(scheduler->fairnessCurrentSide)); // Traza alternancia.
    }

    FLOW_LOG(scheduler, "[FLOW][FAIR] Ventana actual lado=%s usados=%u/%u\n", boatSideName(scheduler->fairnessCurrentSide), scheduler->fairnessPassedInWindow, w); // Traza ventana.
    return findIndexForAlgoAndSide(scheduler->algorithm, scheduler->readyQueue, scheduler->readyCount, true, scheduler->fairnessCurrentSide); // Atiende lado de ventana.
  }

  return findIndexForAlgo(scheduler->algorithm, scheduler->readyQueue, scheduler->readyCount); // Alternativa.
} // Fin de ship_scheduler_select_next_index.

void ship_scheduler_enqueue(ShipScheduler *scheduler, Boat *boat) { // Encola un barco nuevo. 
  ship_scheduler_enqueue_with_deadline(scheduler, boat, 0UL); // Usa deadline por defecto.
} // Fin de ship_scheduler_enqueue.

void ship_scheduler_enqueue_with_deadline(ShipScheduler *scheduler, Boat *boat, unsigned long deadlineMillis) { // Encola un barco con deadline explicito. 
  if (!scheduler || !boat) return; // Valida punteros. 
  scheduler->ignoreCompletions = false; // Habilita callbacks. 
  if (scheduler->readyCount >= MAX_BOATS || scheduler->readyCount >= ship_scheduler_get_max_ready_queue(scheduler)) { // Verifica limites. 
    ship_logln("Cola llena; no se agrego el barco."); // Informa cola llena. 
    destroyBoat(boat); // Libera el barco. 
    return; // Termina. 
  } 

  boat->cancelled = false; // Limpia cancelacion. 
  if (boat->enqueuedAt == 0) boat->enqueuedAt = millis(); // Marca encolado si aplica. 
  boat->state = STATE_WAITING; // Estado en espera. 
  // Si es un barco preemptado que se reencola, preserva su remainingMillis; si es nuevo, usa placeholder
  if (boat->serviceMillis > 0 && boat->remainingMillis == 0) {
    boat->remainingMillis = 1; // Placeholder para nuevo barco en lista
  }
  if (deadlineMillis > 0) { // Si el llamado pide un deadline concreto.
    boat->deadlineMillis = deadlineMillis; // Aplica deadline absoluto.
  } else if (boat->deadlineMillis == 0) { // Si no trae deadline explicito.
    boat->deadlineMillis = ship_scheduler_compute_default_deadline(scheduler, boat); // Calcula uno real para EDF.
  }

  uint8_t insertAt = scheduler->readyCount; // Posicion de insercion. 
  while (insertAt > 0 && scheduler->readyQueue[insertAt - 1]->arrivalOrder > boat->arrivalOrder) { // Mantiene orden por llegada. 
    scheduler->readyQueue[insertAt] = scheduler->readyQueue[insertAt - 1]; // Desplaza a la derecha. 
    insertAt--; // Decrementa indice. 
  } 

  scheduler->readyQueue[insertAt] = boat; // Inserta el barco. 
  scheduler->readyCount++; // Incrementa la cola. 

  ship_logf("Barco agregado: #%u tipo=%s origen=%s\n", boat->id, boatTypeName(boat->type), boatSideName(boat->origin)); // Log detallado de alta. 

  // FIX #9: Crear la tarea ANTES de decidir si preemptar, pero el barco queda
  // bloqueado en NOTIF_CMD_RUN hasta que el scheduler lo despache. La tarea
  // en estado 'waiting' es inofensiva. Esto elimina la ventana de carrera
  // en la que start_next_boat podia despachar el barco antes de que la tarea
  // existiera al crearse en un punto posterior del flujo de preempcion.
  xTaskCreate(boatTask, "boat", 4096, boat, 1, &boat->taskHandle); // Crea la tarea del barco.

  if (scheduler->activeCount == 0) { // Si el scheduler estaba vacio.
    ship_scheduler_start_next_boat(scheduler); // Arranca inmediatamente el primer barco en cola.
    return; // Ya no hace falta preempcion.
  }

  if (scheduler->activeCount > 0 && scheduler->activeBoat) { // Si hay activo. 
    if (scheduler->flowMode == FLOW_TICO && scheduler->algorithm != ALG_RR) {
      // En TICO puro (algoritmo no-RR) no se preempta: los barcos conviven en el
      // canal controlados por reserva de casillas, no por quantum de tiempo.
      return; // Mantiene la ejecucion concurrente.
    }
    bool shouldPreempt = false; // Bandera de preempcion. 
    if (scheduler->algorithm == ALG_STRN) { // Si STRN. 
      // Calcula remaining efectivo del candidato (nuevo barco)
      unsigned long candRem = 0UL;
      if (boat->remainingMillis > 1) candRem = boat->remainingMillis;
      else if (boat->serviceMillis > 0) candRem = boat->serviceMillis;
      else candRem = ship_scheduler_estimate_service_millis(scheduler, boat);
      // Calcula remaining efectivo del activo
      unsigned long activeRem = 0UL;
      if (scheduler->activeBoat->remainingMillis > 1) activeRem = scheduler->activeBoat->remainingMillis;
      else if (scheduler->activeBoat->serviceMillis > 0) activeRem = scheduler->activeBoat->serviceMillis;
      else activeRem = ship_scheduler_estimate_service_millis(scheduler, scheduler->activeBoat);
      if (candRem < activeRem) shouldPreempt = true; // Compara restante efectivo. 
    } else if (scheduler->algorithm == ALG_EDF) { // Si EDF. 
      unsigned long now = millis();
      unsigned long candRem = boat->deadlineMillis > now ? boat->deadlineMillis - now : 0UL;
      unsigned long activeRem = scheduler->activeBoat->deadlineMillis > now ? scheduler->activeBoat->deadlineMillis - now : 0UL;
      if (candRem < activeRem) shouldPreempt = true; // Compara tiempo restante a deadline.
    } else if (scheduler->algorithm == ALG_PRIORITY) { // Si prioridad. 
      if (boat->priority > scheduler->activeBoat->priority) shouldPreempt = true; // Compara prioridad. 
    } 

    if (shouldPreempt) { // Si se debe preemptar.
      Boat *preempted = scheduler->activeBoat; // Barco que sera desalojado.
      ship_logf("Preemption: barco #%u desaloja a #%u\n", boat->id, preempted->id); // Log de preempcion.

      // 1. Detener la tarea del barco desalojado.
      preempted->allowedToMove = false;
      if (preempted->taskHandle) {
        safe_task_notify(preempted->taskHandle, NOTIF_CMD_PAUSE);
      }
      ship_scheduler_freeze_active_quantum(scheduler);

      // 2. Recalcular remainingMillis desde la posicion actual en el canal.
      //    Esto es critico para STRN: el tiempo restante debe reflejar cuanto
      //    camino le queda al barco en el canal, no el tiempo de reloj consumido.
      if (preempted->currentSlot >= 0 && scheduler->listLength > 0 && preempted->serviceMillis > 0) {
        int endIndex = (preempted->origin == SIDE_LEFT) ? (int)(scheduler->listLength - 1) : 0;
        int slotsRemaining = (preempted->origin == SIDE_LEFT)
                             ? (endIndex - preempted->currentSlot)
                             : (preempted->currentSlot - endIndex);
        int totalSlots = (int)(scheduler->listLength - 1);
        if (totalSlots > 0 && slotsRemaining >= 0) {
          // Tiempo restante proporcional a las casillas que le quedan.
          preempted->remainingMillis = (unsigned long)(
            ((float)slotsRemaining / (float)totalSlots) * (float)preempted->serviceMillis + 0.5f);
          if (preempted->remainingMillis == 0) preempted->remainingMillis = 1; // Al menos 1 para no finalizarlo.
        }
      }

      // 3. Guardar la posicion actual en el canal y LIBERAR la casilla.
      //    A diferencia de la emergencia del sensor (donde todo se detiene y nadie
      //    puede avanzar), en preempcion el sucesor sigue moviendose en el mismo
      //    sentido. Si no liberamos la casilla, el sucesor llega a ella y se bloquea
      //    indefinidamente porque slotOwner[slot] sigue siendo el id del desalojado.
      //    Al liberar: el sucesor puede pasar, y cuando el desalojado regrese,
      //    start_next_boat intentara reservar su savedSlot; si el sucesor ya lo
      //    paso, estara libre; si no, el desalojado esperara un tick mas en cola.
      preempted->emergencySavedSlot = preempted->currentSlot; // Guarda slot para restauracion.
      preempted->emergencyParked = true; // Marca como estacionado por preempcion.
      if (preempted->currentSlot >= 0) {
        ship_scheduler_release_range(scheduler, preempted->currentSlot, 1, preempted); // Libera la casilla.
        preempted->currentSlot = -1; // El barco ya no esta fisicamente en el canal.
      }

      // 4. Quitar el barco de la lista de activos para que start_next_boat pueda
      //    despachar al sucesor (el guard activeCount>0 lo bloquearia si no).
      ship_scheduler_remove_active(scheduler, preempted);

      // 5. Reencolarlo al frente para que sea el primero en regresar cuando
      //    el sucesor termine o sea a su vez desalojado.
      ship_scheduler_requeue_boat(scheduler, preempted, true);

      // 6. Arrancar el barco que gano la preempcion.
      ship_scheduler_start_next_boat(scheduler);
    } 
  } 
} // Fin de ship_scheduler_enqueue_with_deadline.

void ship_scheduler_load_demo_manifest(ShipScheduler *scheduler) { // Carga manifiesto de demo. 
  if (!scheduler) return; // Valida puntero. 
  ship_scheduler_clear(scheduler); // Limpia estado. 
  resetBoatSequence(); // Reinicia secuencias. 

  if (scheduler->demoCount > 0) {
    for (uint8_t i = 0; i < scheduler->demoCount; i++) {
      Boat *b = createBoatWithPriority(scheduler->demoSide[i], scheduler->demoType[i], scheduler->demoPrio[i]);
      if (!b) continue;
      if (scheduler->demoStep[i] > 0) b->stepSize = scheduler->demoStep[i];
      ship_scheduler_enqueue(scheduler, b);
    }
    ship_logf("Manifesto cargado desde config (%u barcos).\n", scheduler->demoCount);
    return;
  }

  ship_scheduler_enqueue(scheduler, createBoat(SIDE_LEFT, BOAT_NORMAL)); // Encola normal izq. 
  ship_scheduler_enqueue(scheduler, createBoat(SIDE_RIGHT, BOAT_PESQUERA)); // Encola pesquera der. 
  ship_scheduler_enqueue(scheduler, createBoat(SIDE_LEFT, BOAT_PATRULLA)); // Encola patrulla izq. 
  ship_scheduler_enqueue(scheduler, createBoat(SIDE_RIGHT, BOAT_NORMAL)); // Encola normal der. 
  ship_scheduler_enqueue(scheduler, createBoat(SIDE_LEFT, BOAT_PESQUERA)); // Encola pesquera izq. 

  ship_logln("Manifesto cargado (default)." ); // Log de carga. 
} // Fin de ship_scheduler_load_demo_manifest. 

static void ship_scheduler_requeue_boat(ShipScheduler *scheduler, Boat *boat, bool atFront) { // Reencola un barco. 
  if (!scheduler || !boat) return; // Valida punteros. 
  boat->state = STATE_WAITING; // Vuelve a estado de espera. 

  if (scheduler->readyCount >= MAX_BOATS) { // Si la cola esta llena. 
    boat->cancelled = true; // Marca cancelacion. 
    if (boat->taskHandle) { // Si hay tarea. 
      safe_task_notify(boat->taskHandle, NOTIF_CMD_TERMINATE); // Termina tarea. 
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

static bool ship_scheduler_start_next_boat(ShipScheduler *scheduler) { // Selecciona y arranca el siguiente barco. 
  if (!scheduler || scheduler->readyCount == 0) return false; // Valida estado. 
  // Solo RR permite multiples activos simultaneos.
  if (scheduler->algorithm != ALG_RR && scheduler->activeCount > 0) return false;
  if (scheduler->activeCount >= MAX_BOATS) return false; // Evita overflow de activos.
  // En RR permitimos varios activos ocupando casillas; la rotacion se gestiona
  // por preemption/allowedToMove para que cada activo tenga su quantum.

  // Bloquea despacho si hay emergencia (puertas cerradas).
  if (scheduler->emergencyMode != EMERGENCY_NONE) {
    if (!scheduler->emergencyDispatchBlockedLogged) {
      ship_logln("[EMERGENCY] Despacho bloqueado: puertas cerradas por emergencia");
      scheduler->emergencyDispatchBlockedLogged = true;
    }
    return false; // No inicia barco mientras hay emergencia.
  }

  int idx = ship_scheduler_select_next_index(scheduler); // Busca el mejor segun politica completa. 
  if (idx < 0) return false; // Sale si no hay indice. 

  Boat *b = scheduler->readyQueue[idx]; // Selecciona el barco. 
  for (uint8_t i = idx + 1; i < scheduler->readyCount; i++) scheduler->readyQueue[i - 1] = scheduler->readyQueue[i]; // Compacta cola. 
  scheduler->readyCount--; // Reduce contador. 

  for (uint8_t i = 0; i < scheduler->activeCount; i++) { // Verifica colisiones por sentidos opuestos.
    Boat *active = scheduler->activeBoats[i];
    if (!active) continue;
    if (active->origin != b->origin) {
      // Permitimos despachar en sentido opuesto solo si hay suficiente distancia
      // entre la casilla del activo y la entrada del candidato para evitar
      // solapamiento. Si no es seguro, reencolamos.
      if (active->currentSlot < 0 || scheduler->listLength == 0) {
        scheduler->collisionDetections++; // Registra intento de colision.
        FLOW_LOG(scheduler, "[FLOW][SAFE] Requeue por seguridad: activo #%u (%s), candidato #%u (%s)\n", active->id, boatSideName(active->origin), b->id, boatSideName(b->origin));
        ship_scheduler_requeue_boat(scheduler, b, true); // Reencola el barco para reintento.
        return false; // No inicia para evitar choque.
      }
      int entryIndex = (b->origin == SIDE_LEFT) ? 0 : (int)(scheduler->listLength - 1);
      int dist = active->currentSlot - entryIndex;
      if (dist < 0) dist = -dist;
      int safeDist = (int)active->stepSize + (int)b->stepSize; // conservador
      if (dist <= safeDist) {
        scheduler->collisionDetections++; // Registra intento de colision.
        FLOW_LOG(scheduler, "[FLOW][SAFE] Requeue por seguridad: activo #%u (%s), candidato #%u (%s) dist=%d safe=%d\n", active->id, boatSideName(active->origin), b->id, boatSideName(b->origin), dist, safeDist);
        ship_scheduler_requeue_boat(scheduler, b, true); // Reencola el barco para reintento.
        return false; // No inicia para evitar choque.
      }
      // Si llegamos aqui, la distancia es suficiente y permitimos el despacho.
    }
  }

  // En modo RR, evitar despachar (y reservar entrada) mientras el quantum
  // actual del primario no haya finalizado. Esto previene que un barco se
  // agregue al canal (currentSlot) pero sin permiso de movimiento.
  // Aplica independientemente de flowMode (incluido TICO): el quantum de
  // tiempo es responsabilidad exclusiva de RR, no del control de flujo.
  if (scheduler->algorithm == ALG_RR && scheduler->activeCount > 0 && scheduler->activeBoat && scheduler->activeBoat->allowedToMove) {
    unsigned long elapsed = ship_scheduler_get_active_elapsed_millis(scheduler);
    if (elapsed < scheduler->rrQuantumMillis) {
      FLOW_LOG(scheduler, "[DISPATCH] RR: quantum activo no terminado (%lu/%lu), reencolando #%u\n", elapsed, scheduler->rrQuantumMillis, b->id);
      ship_scheduler_requeue_boat(scheduler, b, true);
      return false;
    }
  }

  // En modo RR no despachar barcos de sentido opuesto si ya hay activos en el canal.
  // Aplica independientemente del flowMode: evita colisiones de frente garantizando
  // que todos los barcos activos en RR viajen en la misma direccion.
  if (scheduler->algorithm == ALG_RR && scheduler->activeCount > 0 && scheduler->activeBoat) {
    if (b->origin != scheduler->activeBoat->origin) {
      FLOW_LOG(scheduler, "[DISPATCH] RR: candidato #%u sentido %s opuesto al activo %s, reencolando\n", b->id, boatSideName(b->origin), boatSideName(scheduler->activeBoat->origin));
      ship_scheduler_requeue_boat(scheduler, b, true);
      return false;
    }
  }

  // Evitar despachar barcos del mismo sentido demasiado cerca: requiere una
  // distancia minima igual al stepSize del candidato (evita adelantamientos
  // y que dos barcos ocupen casillas adyacentes al iniciar).
  for (uint8_t i = 0; i < scheduler->activeCount; i++) {
    Boat *active = scheduler->activeBoats[i];
    if (!active) continue;
    if (active->origin != b->origin) continue;
    if (active->currentSlot < 0) continue;
    int dist = (b->origin == SIDE_LEFT) ? (active->currentSlot - 0) : ((int)(scheduler->listLength - 1) - active->currentSlot);
    // Si la distancia desde la entrada al barco activo es menor que el stepSize
    // del candidato, no despachar.
    if (dist < (int)b->stepSize) {
      FLOW_LOG(scheduler, "[DISPATCH] Demasiado cerca del activo #%u (dist=%d < step=%u), reencolando #%u\n", active->id, dist, b->stepSize, b->id);
      ship_scheduler_requeue_boat(scheduler, b, true);
      return false;
    }
  }

  b->state = STATE_CROSSING; // Cambia estado a cruzando. 
  if (b->startedAt == 0) b->startedAt = millis(); // Marca inicio si aplica. 
  scheduler->crossingStartedAt = millis(); // Marca el inicio del cruce. 

  // Intentar colocar el barco en el canal.
  // Si viene de una preempcion tiene un slot guardado: intentar reservarlo.
  // La casilla fue LIBERADA al desalojar (para que el sucesor no se bloqueara),
  // por lo que ahora puede estar libre (sucesor ya paso) u ocupada (sucesor aun
  // no llego). En el segundo caso se busca la primera casilla libre mas atrasada
  // en el sentido de viaje del barco restaurado.
  if (scheduler->listLength > 0 && scheduler->slotOwner) {
    if (b->emergencyParked && b->emergencySavedSlot >= 0) {
      int dir = (b->origin == SIDE_LEFT) ? 1 : -1;
      int endIndex = (b->origin == SIDE_LEFT) ? (int)(scheduler->listLength - 1) : 0;
      int savedSlot = (int)b->emergencySavedSlot;
      bool restored = false;

      // Intentar primero el slot exacto donde estaba.
      if (ship_scheduler_try_reserve_range(scheduler, savedSlot, 1, b)) {
        b->currentSlot = (int16_t)savedSlot;
        restored = true;
        FLOW_LOG(scheduler, "[DISPATCH] Barco #%u restaurado en casilla exacta %d\n", b->id, savedSlot);
      } else {
        // El sucesor aun ocupa esa casilla: retroceder hacia la entrada del barco
        // buscando la primera casilla libre. "Retroceder" significa ir en direccion
        // contraria al viaje (hacia el origen del barco), porque el sucesor ya
        // esta adelante. Se limita la busqueda a listLength casillas como maximo.
        int reverseDir = -dir; // Direccion contraria al viaje (hacia la entrada).
        int searchSlot = savedSlot + reverseDir;
        for (uint16_t tries = 0; tries < scheduler->listLength; tries++, searchSlot += reverseDir) {
          if (searchSlot < 0 || searchSlot >= (int)scheduler->listLength) break;
          if (ship_scheduler_try_reserve_range(scheduler, searchSlot, 1, b)) {
            b->currentSlot = (int16_t)searchSlot;
            restored = true;
            FLOW_LOG(scheduler, "[DISPATCH] Barco #%u restaurado en casilla fallback %d (saved=%d ocupado)\n", b->id, searchSlot, savedSlot);
            break;
          }
        }
      }

      b->emergencySavedSlot = -1;
      b->emergencyParked = false;

      if (!restored) {
        // Ningun slot disponible: reenqueue para reintento en el proximo tick.
        FLOW_LOG(scheduler, "[DISPATCH] No se pudo restaurar barco #%u, reencolando\n", b->id);
        b->state = STATE_WAITING;
        ship_scheduler_requeue_boat(scheduler, b, true);
        return false;
      }
    } else {
      // Barco nuevo: reservar la casilla de entrada.
      int entryIndex = (b->origin == SIDE_LEFT) ? 0 : (int)(scheduler->listLength - 1);
      if (!ship_scheduler_try_reserve_range(scheduler, entryIndex, 1, b)) {
        FLOW_LOG(scheduler, "[DISPATCH] No se pudo reservar entrada para barco #%u, reencolando\n", b->id);
        ship_scheduler_requeue_boat(scheduler, b, true); // Reencola el barco para reintento.
        return false; // No inicia ahora.
      }
      b->currentSlot = entryIndex;
      FLOW_LOG(scheduler, "[DISPATCH] Entrada reservada: barco #%u en casilla %d\n", b->id, entryIndex);
    }
  }

  if (b->serviceMillis == 0) { // Calcula tiempos reales antes de notificar al hilo.
    b->serviceMillis = ship_scheduler_estimate_service_millis(scheduler, b); // Estimacion unica.
    b->remainingMillis = b->serviceMillis; // Sincroniza restante.
    if (b->deadlineMillis == 0) { // Solo fijar deadline heurístico si no hay uno explícito.
      b->deadlineMillis = b->startedAt + (b->serviceMillis * 2UL); // Recalcula deadline heuristico.
    }
  }

  // Si el barco fue restaurado desde preempcion (currentSlot >= 0 y serviceMillis ya
  // calculado), recalcular remainingMillis proporcional a las casillas que quedan
  // desde la casilla de restauracion real. Esto cubre tanto el caso del slot exacto
  // como el fallback a una casilla mas atrasada.
  if (b->currentSlot >= 0 && b->serviceMillis > 0 && scheduler->listLength > 1) {
    int endIdx = (b->origin == SIDE_LEFT) ? (int)(scheduler->listLength - 1) : 0;
    int remSlots = (b->origin == SIDE_LEFT)
                   ? (endIdx - (int)b->currentSlot)
                   : ((int)b->currentSlot - endIdx);
    int totalSlots = (int)(scheduler->listLength - 1);
    if (remSlots > 0 && remSlots < totalSlots) {
      // Solo recalcular si el barco no esta en la entrada (remSlots < totalSlots
      // descarta barcos nuevos que empiezan desde la entrada con todo el canal).
      unsigned long newRemaining = (unsigned long)(
        ((float)remSlots / (float)totalSlots) * (float)b->serviceMillis + 0.5f);
      if (newRemaining == 0) newRemaining = 1;
      b->remainingMillis = newRemaining;
      FLOW_LOG(scheduler, "[DISPATCH] Barco #%u remainingMillis recalculado=%lu desde slot=%d (remSlots=%d)\n",
               b->id, b->remainingMillis, (int)b->currentSlot, remSlots);
    }
  }

  ship_scheduler_add_active(scheduler, b); // Agrega a la lista de activos.
  ship_logf("Dispatching -> barco #%u (rem=%lu svc=%lu)\n", b->id, b->remainingMillis, b->serviceMillis);
  if (scheduler->flowMode == FLOW_FAIRNESS && b->origin == scheduler->fairnessCurrentSide) { // Cuenta solo despachos reales.
    scheduler->fairnessPassedInWindow++; // Incrementa barcos realmente despachados en ventana W.
    FLOW_LOG(scheduler, "[FLOW][FAIR] Despachado #%u lado=%s ventana=%u/%u\n", b->id, boatSideName(b->origin), scheduler->fairnessPassedInWindow, ship_scheduler_get_fairness_window(scheduler)); // Traza de despacho.
  }

  // En modo RR el barco nuevo se convierte en primario y recibe el quantum.
  if (scheduler->algorithm == ALG_RR) {
    ship_scheduler_promote_active_to_front(scheduler, b);
    b->allowedToMove = true;
    if (b->taskHandle) {
      FLOW_LOG(scheduler, "[DISPATCH DEBUG] Enviando NOTIF_CMD_RUN (RR primary) a barco #%u (taskHandle=%p)\n", b->id, (void*)b->taskHandle);
      safe_task_notify(b->taskHandle, NOTIF_CMD_RUN);
      scheduler->activeQuantumAccumulatedMillis = 0;
      scheduler->activeQuantumStartedAt = millis();
    } else {
      FLOW_LOG(scheduler, "[DISPATCH DEBUG] ERROR: barco #%u NO TIENE taskHandle!\n", b->id);
    }
  } else {
    // Comportamiento por defecto: notificar y permitir movimiento.
    b->allowedToMove = true;
    if (b->taskHandle) {
      FLOW_LOG(scheduler, "[DISPATCH DEBUG] Enviando NOTIF_CMD_RUN a barco #%u (taskHandle=%p)\n", b->id, (void*)b->taskHandle);
      safe_task_notify(b->taskHandle, NOTIF_CMD_RUN); // Arranca la tarea.
      // Solo reinicia el quantum para algoritmos que lo usan (no-RR no necesita quantum compartido).
      // Para RR el quantum ya se maneja en la rama de arriba.
    } else {
      FLOW_LOG(scheduler, "[DISPATCH DEBUG] ERROR: barco #%u NO TIENE taskHandle!\n", b->id);
    }
  }

  ship_logf("Start -> barco #%u\n", b->id); // Log del inicio. 
  return true; // Indica que se despacho un barco.
} // Fin de ship_scheduler_start_next_boat. 

static bool ship_scheduler_preempt_active_for_rr(ShipScheduler *scheduler) { // Preempcion por RR. 
  if (!scheduler || scheduler->activeCount == 0 || !scheduler->activeBoat) return false; // Valida activo. 
  if (scheduler->emergencyMode != EMERGENCY_NONE) return false; // Durante emergencia no rotamos ni despachamos.
  // TICO controla seguridad de casillas; RR controla quantum de tiempo. Son ortogonales:
  // no bloquear la preempcion por tiempo aunque TICO este activo.
  // Si no hay listos y solo un activo, nada que rotar.
  if (scheduler->readyCount == 0 && scheduler->activeCount <= 1) return false;
  // Solo preemptar si hay otro activo para promover o existe algun candidato listo
  // que pueda ser despachado sin riesgo de colision o cercania.
  bool hasWorkable = false;
  if (scheduler->activeCount > 1) hasWorkable = true; // ya hay otro activo en canal
  if (!hasWorkable && scheduler->readyCount > 0) {
    for (uint8_t ri = 0; ri < scheduler->readyCount; ri++) {
      Boat *cand = scheduler->readyQueue[ri];
      if (!cand) continue;
      bool safe = true;
      for (uint8_t ai = 0; ai < scheduler->activeCount; ai++) {
        Boat *active = scheduler->activeBoats[ai];
        if (!active) continue;
        // no permitir despacho de sentido opuesto si hay activo en ese sentido
        if (active->origin != cand->origin) { safe = false; break; }
        // si hay activo del mismo sentido, comprobar distancia minima
        if (active->currentSlot >= 0) {
          int dist = (cand->origin == SIDE_LEFT) ? (active->currentSlot - 0) : ((int)(scheduler->listLength - 1) - active->currentSlot);
          if (dist < (int)cand->stepSize) { safe = false; break; }
        }
      }
      if (safe) { hasWorkable = true; break; }
    }
  }
  if (!hasWorkable) return false; // No hay quien pueda correr ahora, no preemptar.
  if (!scheduler->activeBoat->allowedToMove) return false; // Ya esta pausado; no repetir la preempcion.
  // Si el barco activo está en modo "visual post-servicio" (remainingMillis==1),
  // no preemptar: necesita terminar de llegar al final del canal sin interrupciones.
  if (scheduler->activeBoat->remainingMillis == 1) return false;
  if (ship_scheduler_get_active_elapsed_millis(scheduler) < scheduler->rrQuantumMillis) return false; // Si no consume quantum, sale. 

  Boat *preempted = scheduler->activeBoat; // Activo primario que consumio su quantum.
  if (preempted->taskHandle) {
    FLOW_LOG(scheduler, "[RR] Pausando activo #%u por quantum\n", preempted->id);
    preempted->allowedToMove = false; // Detener movimiento pero conservar la casilla.
    safe_task_notify(preempted->taskHandle, NOTIF_CMD_PAUSE); // Pausa la tarea.
  }

  ship_scheduler_freeze_active_quantum(scheduler); // Congela el quantum agotado.
  return true;
} // Fin de ship_scheduler_preempt_active_for_rr. 

// Yield voluntario por parte del barco activo cuando queda bloqueado.
// Implementa rotacion circular real: busca el siguiente activo en la lista
// que pueda avanzar sus steps completos, garantizando que todos los activos
// tengan oportunidad de correr (evita inanicion de barcos como #7).
static void ship_scheduler_yield_active_for_rr(ShipScheduler *scheduler, Boat *boat) {
  if (!scheduler || scheduler->algorithm != ALG_RR || !boat) return;
  if (scheduler->emergencyMode != EMERGENCY_NONE) return;
  if (scheduler->activeBoat != boat) return; // Solo el primario puede ceder su turno.

  // Encontrar la posicion actual del primario en la lista de activos.
  int currentIdx = -1;
  for (uint8_t i = 0; i < scheduler->activeCount; i++) {
    if (scheduler->activeBoats[i] == boat) { currentIdx = (int)i; break; }
  }

  // Buscar el siguiente activo en rotacion circular que pueda avanzar sus steps.
  Boat *successor = NULL;
  if (scheduler->activeCount > 1) {
    for (uint8_t offset = 1; offset < scheduler->activeCount; offset++) {
      uint8_t idx = (uint8_t)((currentIdx + offset) % scheduler->activeCount);
      Boat *candidate = scheduler->activeBoats[idx];
      if (!candidate || candidate == boat || candidate->currentSlot < 0) continue;
      // Verificar si el candidato puede avanzar al menos stepSize casillas.
      int dir = (candidate->origin == SIDE_LEFT) ? 1 : -1;
      int endIndex = (candidate->origin == SIDE_LEFT) ? (int)(scheduler->listLength - 1) : 0;
      int remSlots = (candidate->origin == SIDE_LEFT)
                     ? (endIndex - candidate->currentSlot)
                     : (candidate->currentSlot - endIndex);
      if (remSlots <= 0) continue; // Ya en el final, no necesita moverse.
      uint8_t need = (uint8_t)((remSlots < (int)candidate->stepSize) ? remSlots : candidate->stepSize);
      bool canMove = true;
      int checkSlot = candidate->currentSlot + dir;
      for (uint8_t s = 0; s < need; s++) {
        if (checkSlot < 0 || checkSlot >= (int)scheduler->listLength) { canMove = false; break; }
        if (scheduler->slotOwner && scheduler->slotOwner[checkSlot] != 0
            && scheduler->slotOwner[checkSlot] != candidate->id) { canMove = false; break; }
        checkSlot += dir;
      }
      if (canMove) { successor = candidate; break; }
    }
  }

  // Si ningun activo puede moverse, revisar si hay candidato seguro en la cola.
  bool queueHasWorkable = false;
  if (!successor && scheduler->readyCount > 0) {
    for (uint8_t ri = 0; ri < scheduler->readyCount; ri++) {
      Boat *cand = scheduler->readyQueue[ri];
      if (!cand) continue;
      bool safe = true;
      for (uint8_t ai = 0; ai < scheduler->activeCount; ai++) {
        Boat *active = scheduler->activeBoats[ai];
        if (!active) continue;
        if (active->origin != cand->origin) { safe = false; break; }
        if (active->currentSlot >= 0) {
          int dist = (cand->origin == SIDE_LEFT)
                     ? (active->currentSlot - 0)
                     : ((int)(scheduler->listLength - 1) - active->currentSlot);
          if (dist < (int)cand->stepSize) { safe = false; break; }
        }
      }
      if (safe) { queueHasWorkable = true; break; }
    }
  }

  if (!successor && !queueHasWorkable) {
    // Canal bloqueado: ningun activo ni candidato puede avanzar ahora.
    // Reanudar al primario actual para que siga intentando en el proximo tick.
    FLOW_LOG(scheduler, "[RR] Canal bloqueado; no hay sucesor viable para barco #%u\n", boat->id);
    boat->allowedToMove = true;
    if (boat->taskHandle) safe_task_notify(boat->taskHandle, NOTIF_CMD_RUN);
    ship_scheduler_resume_active_quantum(scheduler);
    return;
  }

  // Pausar este barco y congelar su quantum.
  boat->allowedToMove = false;
  if (boat->taskHandle) {
    FLOW_LOG(scheduler, "[RR] Barco #%u cede voluntariamente por bloqueo\n", boat->id);
    safe_task_notify(boat->taskHandle, NOTIF_CMD_PAUSE);
  }
  ship_scheduler_freeze_active_quantum(scheduler);

  // Usar el indice circular para elegir al sucesor activo de forma equitativa.
  // ship_scheduler_rr_next_active ya actualiza rrTurnIndex correctamente.
  Boat *chosen = ship_scheduler_rr_next_active(scheduler);
  if (!chosen || chosen == boat) chosen = successor; // Fallback al que encontramos antes.
  if (chosen && chosen != boat) {
    ship_scheduler_promote_active_to_front(scheduler, chosen);
    // FIX #1: sync_rr_permissions baja allowedToMove de todos los activos ANTES
    // de que el elegido lo vuelva a subir. Esto garantiza que el barco saliente
    // (boat, ya pausado arriba) no tenga allowedToMove=true residual cuando la
    // tarea del entrante se despierta.
    ship_scheduler_sync_rr_permissions(scheduler); // Primero bajar permisos de todos.
    chosen->allowedToMove = true;                  // Luego subir solo al elegido.
    if (chosen->taskHandle) {
      FLOW_LOG(scheduler, "[RR] Rotando a activo #%u tras yield de #%u\n", chosen->id, boat->id);
      safe_task_notify(chosen->taskHandle, NOTIF_CMD_RUN);
      scheduler->activeQuantumAccumulatedMillis = 0;
      ship_scheduler_resume_active_quantum(scheduler);
    }
    return;
  }

  // No hay activo viable pero si candidato en cola: intentar despacharlo.
  if (queueHasWorkable && ship_scheduler_start_next_boat(scheduler)) {
    ship_scheduler_sync_rr_permissions(scheduler);
    return;
  }

  // Ultimo recurso: reanudar al primario pausado para evitar bloqueo total.
  if (scheduler->activeCount >= 1 && scheduler->activeBoat) {
    Boat *preemptedBoat = scheduler->activeBoat;
    preemptedBoat->allowedToMove = true;
    if (preemptedBoat->taskHandle) {
      FLOW_LOG(scheduler, "[RR] No hay sucesor tras yield; reanudando activo #%u\n", preemptedBoat->id);
      safe_task_notify(preemptedBoat->taskHandle, NOTIF_CMD_RUN);
      ship_scheduler_resume_active_quantum(scheduler);
    }
    ship_scheduler_sync_rr_permissions(scheduler);
  }
}

static void ship_scheduler_finish_active_boat(ShipScheduler *scheduler, Boat *b) { // Finaliza el barco activo. 
  if (!scheduler || !b) return; // Valida activo. 

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
  ship_scheduler_remove_active(scheduler, b); // Quita de activos. 
  ship_logf("Barco finalizado: #%u tipo=%s origen=%s\n", b->id, boatTypeName(b->type), boatSideName(b->origin)); // Log de finalizacion con tipo. 
  // Reiniciar contabilidad de quantum activo para que el siguiente despacho
  // no quede bloqueado por el tiempo restante del barco terminado.
  ship_scheduler_reset_active_quantum(scheduler);

  // En RR, si quedan activos, reanudar inmediatamente al nuevo primario
  // (evita que se quede esperando NOTIF_CMD_RUN tras una finalizacion).
  if (scheduler->algorithm == ALG_RR && scheduler->activeCount > 0) {
    Boat *next = scheduler->activeBoat; // ya actualizado por remove_active
    if (next) {
      next->allowedToMove = true;
      if (next->taskHandle) {
        FLOW_LOG(scheduler, "[RR] Reanudando activo #%u tras finalizacion\n", next->id);
        safe_task_notify(next->taskHandle, NOTIF_CMD_RUN);
        ship_scheduler_resume_active_quantum(scheduler);
      }
    }
    ship_scheduler_sync_rr_permissions(scheduler); // Evita que otros activos sigan moviendose.
  }
} // Fin de ship_scheduler_finish_active_boat. 

void ship_scheduler_update(ShipScheduler *scheduler) { // Ejecuta un tick de planificacion. 
  if (!scheduler) return; // Valida puntero. 

  ship_scheduler_update_emergency(scheduler); // Actualiza estado de emergencia.
  ship_scheduler_tick_sign(scheduler); // Actualiza cambio de direccion para modo letrero.

  for (uint8_t i = 0; i < scheduler->activeCount; ) { // Recorre activos.
    Boat *active = scheduler->activeBoats[i]; // Toma el activo.
    if (!active) { // Si es nulo.
      i++; // Avanza.
      continue; // Sigue.
    }
    if (active->remainingMillis == 0) { // Si ya termino.
      ship_scheduler_finish_active_boat(scheduler, active); // Finaliza.
      continue; // Reevalua el indice por cambios.
    }
    i++; // Avanza.
  }

  if (scheduler->algorithm == ALG_RR) { // Si es RR.
    if (scheduler->emergencyMode != EMERGENCY_NONE) return; // Mientras dura la emergencia no avanzamos RR.
    bool preempted = ship_scheduler_preempt_active_for_rr(scheduler); // Detecta si el quantum se agotó.
    if (preempted) { // Si ya se pausa el actual, intenta mover otro barco.
      bool started = false;
      if (scheduler->readyCount > 0) started = ship_scheduler_start_next_boat(scheduler);
      if (started) {
        ship_scheduler_sync_rr_permissions(scheduler); // Garantiza que solo el nuevo primario avance.
        return; // Se despacho un sucesor válido.
      }

      if (scheduler->activeCount > 1) { // Fallback: rota al siguiente activo usando indice circular.
        Boat *nextActive = ship_scheduler_rr_next_active(scheduler);
        if (nextActive && nextActive != scheduler->activeBoat) {
          // FIX #1: Pausar explicitamente al primario saliente ANTES de promover
          // al entrante. Sin este paso, el barco saliente continua moviendose
          // porque ya tenia allowedToMove=true y su tarea seguia en el tick de 50ms.
          // preempt_active_for_rr ya pone allowedToMove=false y envia PAUSE al
          // primario, asi que aqui solo necesitamos garantizar que sync_rr_permissions
          // se llame ANTES de enviar RUN al sucesor para que el flag este actualizado
          // cuando la tarea del sucesor despierte.
          ship_scheduler_promote_active_to_front(scheduler, nextActive);
          ship_scheduler_sync_rr_permissions(scheduler); // Primero bajar permisos de todos.
          nextActive->allowedToMove = true;             // Luego subir solo al nuevo primario.
          if (nextActive->taskHandle) {
            FLOW_LOG(scheduler, "[RR] Rotando a activo #%u tras quantum\n", nextActive->id);
            safe_task_notify(nextActive->taskHandle, NOTIF_CMD_RUN);
            scheduler->activeQuantumAccumulatedMillis = 0;
            ship_scheduler_resume_active_quantum(scheduler);
          }
          return;
        }
      }

      // Ningun sucesor valido ni otro activo: reanudar al preempted (evita quedarse bloqueado).
      if (scheduler->activeCount >= 1 && scheduler->activeBoat) {
        Boat *preemptedBoat = scheduler->activeBoat;
        preemptedBoat->allowedToMove = true;
        if (preemptedBoat->taskHandle) {
          FLOW_LOG(scheduler, "[RR] No hay sucesor valido; reanudando activo #%u\n", preemptedBoat->id);
          safe_task_notify(preemptedBoat->taskHandle, NOTIF_CMD_RUN);
          ship_scheduler_resume_active_quantum(scheduler);
        }
        ship_scheduler_sync_rr_permissions(scheduler);
        return;
      }
    }
    if (scheduler->activeCount == 0 && scheduler->readyCount > 0) { // Arranque inicial o despues de terminar todo.
      ship_scheduler_start_next_boat(scheduler);
    }
    return;
  }

  // Algoritmos no RR: ejecucion secuencial (un solo barco activo).
  if (scheduler->activeCount == 0 && scheduler->readyCount > 0) {
    ship_scheduler_start_next_boat(scheduler);
    return;
  }

  // FIX #6: Re-evaluar preempcion periodica para STRN y EDF.
  // La preempcion al encolar un barco nuevo solo cubre el caso en que un barco
  // mas urgente LLEGA. Pero en STRN el activo va consumiendo remainingMillis y
  // puede volverse mas largo que alguien ya en cola. En EDF, el tiempo que falta
  // para el deadline de barcos en cola cambia con el reloj: un barco puede
  // volverse mas urgente que el activo sin que haya llegado nadie nuevo.
  if (scheduler->activeCount > 0 && scheduler->activeBoat && scheduler->readyCount > 0) {
    if (scheduler->algorithm == ALG_STRN || scheduler->algorithm == ALG_EDF) {
      bool shouldPreempt = false;
      Boat *active = scheduler->activeBoat;

      for (uint8_t ri = 0; ri < scheduler->readyCount; ri++) {
        Boat *cand = scheduler->readyQueue[ri];
        if (!cand) continue;
        // No preemptar si el candidato tiene sentido opuesto al activo (colision).
        if (cand->origin != active->origin) continue;

        if (scheduler->algorithm == ALG_STRN) {
          unsigned long candRem = (cand->remainingMillis > 1) ? cand->remainingMillis
                                  : (cand->serviceMillis > 0 ? cand->serviceMillis
                                  : ship_scheduler_estimate_service_millis(scheduler, cand));
          unsigned long activeRem = (active->remainingMillis > 1) ? active->remainingMillis
                                    : (active->serviceMillis > 0 ? active->serviceMillis
                                    : ship_scheduler_estimate_service_millis(scheduler, active));
          if (candRem < activeRem) { shouldPreempt = true; break; }
        } else { // ALG_EDF
          unsigned long now = millis();
          unsigned long candTimeLeft = cand->deadlineMillis > now ? cand->deadlineMillis - now : 0UL;
          unsigned long activeTimeLeft = active->deadlineMillis > now ? active->deadlineMillis - now : 0UL;
          if (candTimeLeft < activeTimeLeft) { shouldPreempt = true; break; }
        }
      }

      if (shouldPreempt && active->allowedToMove) {
        FLOW_LOG(scheduler, "[PREEMPT PERIODIC] Desalojando activo #%u por candidato mas urgente en cola\n", active->id);
        // Detener el activo.
        active->allowedToMove = false;
        if (active->taskHandle) safe_task_notify(active->taskHandle, NOTIF_CMD_PAUSE);
        ship_scheduler_freeze_active_quantum(scheduler);
        // Recalcular remainingMillis del desalojado por posicion en canal.
        if (active->currentSlot >= 0 && scheduler->listLength > 0 && active->serviceMillis > 0) {
          int endIndex = (active->origin == SIDE_LEFT) ? (int)(scheduler->listLength - 1) : 0;
          int slotsRemaining = (active->origin == SIDE_LEFT)
                               ? (endIndex - active->currentSlot)
                               : (active->currentSlot - endIndex);
          int totalSlots = (int)(scheduler->listLength - 1);
          if (totalSlots > 0 && slotsRemaining >= 0) {
            active->remainingMillis = (unsigned long)(
              ((float)slotsRemaining / (float)totalSlots) * (float)active->serviceMillis + 0.5f);
            if (active->remainingMillis == 0) active->remainingMillis = 1;
          }
        }
        // Guardar posicion, liberar casilla y estacionar.
        // Igual que en la preempcion por enqueue: hay que liberar la casilla para
        // que el sucesor no se bloquee al llegar a ella.
        active->emergencySavedSlot = active->currentSlot;
        active->emergencyParked = true;
        if (active->currentSlot >= 0) {
          ship_scheduler_release_range(scheduler, active->currentSlot, 1, active);
          active->currentSlot = -1;
        }
        ship_scheduler_remove_active(scheduler, active);
        ship_scheduler_requeue_boat(scheduler, active, false); // Al final, no al frente (otro ya era mas urgente).
        ship_scheduler_start_next_boat(scheduler);
      }
    }
  }
} // Fin de ship_scheduler_update. 

const Boat *ship_scheduler_get_active_boat(const ShipScheduler *scheduler) { // Devuelve barco activo. 
  if (!scheduler || scheduler->activeCount == 0) return NULL; // Valida estado. 
  return scheduler->activeBoat; // Retorna activo. 
} // Fin de ship_scheduler_get_active_boat. 

uint8_t ship_scheduler_get_active_count(const ShipScheduler *scheduler) { // Devuelve cantidad de activos.
  return scheduler ? scheduler->activeCount : 0; // Retorna count o cero.
} // Fin de ship_scheduler_get_active_count.

const Boat *ship_scheduler_get_active_boat_at(const ShipScheduler *scheduler, uint8_t index) { // Devuelve activo por indice.
  if (!scheduler || index >= scheduler->activeCount) return NULL; // Valida rango.
  return scheduler->activeBoats[index]; // Retorna activo.
} // Fin de ship_scheduler_get_active_boat_at.

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
  if (!scheduler || scheduler->activeCount == 0 || !scheduler->activeBoat) return 0; // Valida estado. 
  // Usa el quantum acumulado más el tramo en curso.
  if (scheduler->activeQuantumStartedAt > 0) {
    unsigned long now = millis();
    if (now >= scheduler->activeQuantumStartedAt) return scheduler->activeQuantumAccumulatedMillis + (now - scheduler->activeQuantumStartedAt);
    return scheduler->activeQuantumAccumulatedMillis;
  }
  return scheduler->activeQuantumAccumulatedMillis; // Quantum congelado o pausado.
} // Fin de ship_scheduler_get_active_elapsed_millis. 

uint16_t ship_scheduler_get_completed_total(const ShipScheduler *scheduler) { // Devuelve total completados. 
  return scheduler ? scheduler->completedTotal : 0; // Retorna contador o cero. 
} // Fin de ship_scheduler_get_completed_total. 

uint16_t ship_scheduler_get_collision_detections(const ShipScheduler *scheduler) { // Devuelve colisiones detectadas.
  return scheduler ? scheduler->collisionDetections : 0; // Retorna contador o cero.
} // Fin de ship_scheduler_get_collision_detections.

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

  for (uint8_t i = 0; i < scheduler->activeCount; i++) { // Recorre activos.
    if (scheduler->activeBoats[i] == b) { // Si era activo.
      ship_scheduler_finish_active_boat(scheduler, b); // Finaliza normalmente.
      return; // Sale.
    }
  }

  for (uint8_t i = 0; i < scheduler->readyCount; i++) { // Recorre cola. 
    if (scheduler->readyQueue[i] == b) { // Si encuentra el barco. 
      for (uint8_t j = i + 1; j < scheduler->readyCount; j++) scheduler->readyQueue[j - 1] = scheduler->readyQueue[j]; // Compacta cola. 
      scheduler->readyCount--; // Reduce contador. 
      return; // Sale. 
    } 
  } 
} // Fin de ship_scheduler_notify_boat_finished. 

void ship_scheduler_pause_active(ShipScheduler *scheduler) { // Pausa el barco activo. 
  if (!scheduler || scheduler->activeCount == 0) { // Valida activo.
    ship_logln("No hay barco activo para pausar."); // Mensaje de error. 
    return; // Sale.
  }

  if (scheduler->flowMode == FLOW_TICO) { // Pausa todos los activos.
    for (uint8_t i = 0; i < scheduler->activeCount; i++) {
      Boat *active = scheduler->activeBoats[i];
      if (!active) continue;
      if (active->taskHandle) {
        active->allowedToMove = false; // Congela movimiento del barco.
        safe_task_notify(active->taskHandle, NOTIF_CMD_PAUSE); // Envia pausa.
      }
      ship_logf("Pausado barco #%u\n", active->id); // Log de pausa.
    }
    return; // Sale.
  }

  if (scheduler->activeBoat && scheduler->activeBoat->taskHandle) { // Si hay tarea. 
    scheduler->activeBoat->allowedToMove = false; // Congela movimiento del barco. 
    safe_task_notify(scheduler->activeBoat->taskHandle, NOTIF_CMD_PAUSE); // Envia pausa. 
  } 
  ship_logf("Pausado barco #%u\n", scheduler->activeBoat ? scheduler->activeBoat->id : 0); // Log de pausa. 
} // Fin de ship_scheduler_pause_active. 

void ship_scheduler_resume_active(ShipScheduler *scheduler) { // Reanuda el barco activo. 
  if (!scheduler || scheduler->activeCount == 0) { // Valida activo.
    ship_logln("No hay barco activo para reanudar."); // Mensaje de error. 
    return; // Sale.
  }

  if (scheduler->flowMode == FLOW_TICO) { // Reanuda todos los activos.
    for (uint8_t i = 0; i < scheduler->activeCount; i++) {
      Boat *active = scheduler->activeBoats[i];
      if (!active) continue;
      if (active->taskHandle) {
        active->allowedToMove = true; // Permite movimiento del barco.
        safe_task_notify(active->taskHandle, NOTIF_CMD_RUN); // Envia run.
      }
      ship_logf("Reanudado barco #%u\n", active->id); // Log de reanudacion.
    }
    return; // Sale.
  }

  if (scheduler->activeBoat && scheduler->activeBoat->taskHandle) { // Si hay tarea. 
    scheduler->activeBoat->allowedToMove = true; // Permite movimiento del barco. 
    safe_task_notify(scheduler->activeBoat->taskHandle, NOTIF_CMD_RUN); // Envia run. 
  } 
  ship_logf("Reanudado barco #%u\n", scheduler->activeBoat ? scheduler->activeBoat->id : 0); // Log de reanudacion. 
} // Fin de ship_scheduler_resume_active. 


void ship_scheduler_dump_status(const ShipScheduler *scheduler) { // Imprime estado del scheduler. 
  if (!scheduler) return; // Valida puntero. 
  ship_logln("--- Scheduler Status ---"); // Encabezado. 
  ship_logf("Algorithm: %s\n", ship_scheduler_get_algorithm_label(scheduler)); // Algoritmo actual. 
  ship_logf("Flow: %s\n", ship_scheduler_get_flow_mode_label(scheduler)); // Metodo de flujo.
  ship_logf("W=%u Sign=%s/%lums QueueMax=%u\n", ship_scheduler_get_fairness_window(scheduler), boatSideName(ship_scheduler_get_sign_direction(scheduler)), ship_scheduler_get_sign_interval(scheduler), ship_scheduler_get_max_ready_queue(scheduler)); // Parametros de flujo.
  ship_logf("Canal=%um Vel=%um/s Collisions=%u\n", ship_scheduler_get_channel_length(scheduler), ship_scheduler_get_boat_speed(scheduler), ship_scheduler_get_collision_detections(scheduler)); // Parametros de canal y colisiones.
  ship_logf("FlowLog: %s\n", ship_scheduler_get_flow_logging(scheduler) ? "ON" : "OFF"); // Estado de trazas de flujo.
  ship_logf("Ready count: %u\n", scheduler->readyCount); // Cantidad en cola. 
  for (uint8_t i = 0; i < scheduler->readyCount; i++) { // Recorre cola. 
    Boat *b = scheduler->readyQueue[i]; // Obtiene barco. 
    ship_logf("%u: #%u %s from %s rem=%lu\n", i, b->id, boatTypeShort(b->type), boatSideName(b->origin), b->remainingMillis); // Imprime linea. 
  } 
  ship_logf("Active count: %u\n", scheduler->activeCount); // Cantidad de activos.
  if (scheduler->activeCount > 0) { // Si hay activos.
    for (uint8_t i = 0; i < scheduler->activeCount; i++) { // Recorre activos.
      Boat *b = scheduler->activeBoats[i]; // Obtiene activo.
      if (!b) continue; // Salta nulos.
      ship_logf("Active %u: #%u rem=%lu\n", i, b->id, b->remainingMillis); // Imprime activo.
    }
  } else { // Si no hay activo.
    ship_logln("Active: none"); // Informa sin activo.
  } 
  ship_logln("------------------------"); // Cierra el bloque. 
} // Fin de ship_scheduler_dump_status.