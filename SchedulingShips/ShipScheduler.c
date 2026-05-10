#include "ShipScheduler.h" // API del scheduler. 

#include <freertos/FreeRTOS.h> // Tipos base de FreeRTOS. 
#include <freertos/task.h> // API de tareas. 
#include <stdlib.h> // malloc y free. 
#include <stdio.h> // fopen para cargar channel_config.txt
#include <freertos/semphr.h> // Semaphores and mutexes
#include <string.h>

#include "ShipIO.h" // Logging por Serial. 
#include "ShipDisplay.h" // API de pantalla (para que cada barco pueda redibujar). 
#include "ShipCommands.h" // Parser de comandos para leer config.

ShipScheduler *gScheduler = NULL; // Puntero global para callbacks. 

static void ship_scheduler_requeue_boat(ShipScheduler *scheduler, Boat *boat, bool atFront); // Declara requeue. 
static bool ship_scheduler_start_next_boat(ShipScheduler *scheduler); // Declara start. 
static void ship_scheduler_finish_active_boat(ShipScheduler *scheduler, Boat *boat); // Declara finish. 
static void ship_scheduler_preempt_active_for_rr(ShipScheduler *scheduler); // Declara preempt RR. 
static BoatSide opposite_side(BoatSide side); // Declara lado opuesto.
static bool queue_has_side(const ShipScheduler *scheduler, BoatSide side); // Declara busqueda por lado.
static bool candidate_is_better(ShipAlgo algo, const Boat *candidate, const Boat *best); // Declara comparador.
static int findIndexForAlgoAndSide(ShipAlgo algo, Boat *readyQueue[], uint8_t readyCount, bool useSide, BoatSide side); // Declara selector filtrado.
static void ship_scheduler_tick_sign(ShipScheduler *scheduler); // Declara tick de letrero.
static int ship_scheduler_select_next_index(ShipScheduler *scheduler); // Declara selector con flujo.
static unsigned long ship_scheduler_boat_elapsed_millis(const Boat *boat); // Declara elapsed por barco.
static unsigned long ship_scheduler_estimate_service_millis(const ShipScheduler *scheduler, const Boat *boat); // Declara estimador de servicio.
static void ship_scheduler_sync_primary_active(ShipScheduler *scheduler); // Declara sync de activo.
static void ship_scheduler_add_active(ShipScheduler *scheduler, Boat *boat); // Declara alta de activo.
static void ship_scheduler_remove_active(ShipScheduler *scheduler, Boat *boat); // Declara baja de activo.
static bool ship_scheduler_is_tico_safe(const ShipScheduler *scheduler, const Boat *candidate); // Declara seguridad TICO.

// Valores por defecto si no existe channel_config.txt
#define DEFAULT_LIST_LENGTH 3U
#define DEFAULT_VISUAL_CHANNEL_LENGTH 6U

#define FLOW_LOG(schedulerPtr, fmt, ...) do { if ((schedulerPtr) && (schedulerPtr)->flowLoggingEnabled) ship_logf(fmt, ##__VA_ARGS__); } while (0) // Macro de trazas de flujo.

static BoatSide opposite_side(BoatSide side) { // Retorna el lado opuesto.
  return side == SIDE_LEFT ? SIDE_RIGHT : SIDE_LEFT; // Evalua el opuesto.
} // Fin de opposite_side.

static bool queue_has_side(const ShipScheduler *scheduler, BoatSide side) { // Verifica si hay barcos por lado.
  if (!scheduler) return false; // Valida scheduler.
  for (uint8_t i = 0; i < scheduler->readyCount; i++) { // Recorre cola.
    if (scheduler->readyQueue[i] && scheduler->readyQueue[i]->origin == side) return true; // Retorna al encontrar.
  }
  return false; // No encontro barcos de ese lado.
} // Fin de queue_has_side.

static bool candidate_is_better(ShipAlgo algo, const Boat *candidate, const Boat *best) { // Compara dos candidatos.
  if (!candidate) return false; // Valida candidato.
  if (!best) return true; // Si no hay mejor actual, gana candidato.

  if (algo == ALG_FCFS || algo == ALG_RR) { // FCFS y RR priorizan llegada.
    return candidate->arrivalOrder < best->arrivalOrder; // Menor llegada es mejor.
  }

  if (algo == ALG_SJF) { // SJF usa servicio total.
    if (candidate->serviceMillis != best->serviceMillis) return candidate->serviceMillis < best->serviceMillis; // Menor servicio gana.
    return candidate->arrivalOrder < best->arrivalOrder; // Desempata por llegada.
  }

  if (algo == ALG_STRN) { // STRN usa tiempo restante.
    if (candidate->remainingMillis != best->remainingMillis) return candidate->remainingMillis < best->remainingMillis; // Menor restante gana.
    return candidate->arrivalOrder < best->arrivalOrder; // Desempata por llegada.
  }

  if (algo == ALG_EDF) { // EDF usa deadline.
    if (candidate->deadlineMillis != best->deadlineMillis) return candidate->deadlineMillis < best->deadlineMillis; // Menor deadline gana.
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

static void ship_scheduler_add_active(ShipScheduler *scheduler, Boat *boat) { // Agrega barco activo.
  if (!scheduler || !boat) return; // Valida punteros.
  if (scheduler->activeCount >= MAX_BOATS) return; // Evita overflow.
  scheduler->activeBoats[scheduler->activeCount++] = boat; // Agrega al final.
  ship_scheduler_sync_primary_active(scheduler); // Actualiza activo primario.
} // Fin de ship_scheduler_add_active.

static void ship_scheduler_remove_active(ShipScheduler *scheduler, Boat *boat) { // Quita barco activo.
  if (!scheduler || !boat || scheduler->activeCount == 0) return; // Valida estado.
  for (uint8_t i = 0; i < scheduler->activeCount; i++) {
    if (scheduler->activeBoats[i] == boat) {
      for (uint8_t j = i + 1; j < scheduler->activeCount; j++) {
        scheduler->activeBoats[j - 1] = scheduler->activeBoats[j];
      }
      scheduler->activeBoats[scheduler->activeCount - 1] = NULL;
      scheduler->activeCount--;
      break;
    }
  }
  ship_scheduler_sync_primary_active(scheduler); // Actualiza activo primario.
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
    // Calcula margen minimo dinámico según tipos (activo, candidato), factor configurable, y velocidad/servicio.
    float pairFactor = 1.0f;
    if (scheduler) {
      BoatType at = active->type;
      BoatType ct = candidate->type;
      if (at >= 0 && at < 3 && ct >= 0 && ct < 3) pairFactor = scheduler->ticoMarginFactor[at][ct];
    }
    float serviceScale = 1.0f;
    float speedScale = 1.0f;
    if (scheduler && scheduler->boatSpeedMetersPerSec > 0) speedScale = 18.0f / (float)scheduler->boatSpeedMetersPerSec; // 18 = default speed
    unsigned long minGapMs = (unsigned long)((float)TICO_INITIAL_MARGIN * pairFactor * serviceScale * speedScale);
    if (elapsed < minGapMs) return false; // Evita solapamiento inicial.

    if (candidate->serviceMillis < active->serviceMillis && active->serviceMillis > 0) { // Riesgo si el candidato es mas rapido.
      float gapFraction = (float)minGapMs / (float)active->serviceMillis; // Margen relativo.
      if (gapFraction > 0.02f) gapFraction = TICO_SAFETY_MARGIN; // Evita margenes excesivos.
      //ship_logf(" gapFraction=%.2f\n", gapFraction); // Depuracion: detalles de la evaluacion.
      float ratio = (float)candidate->serviceMillis / (float)active->serviceMillis; // Relacion de tiempos.
      float requiredElapsed = (1.0f - (1.0f - gapFraction) * ratio) * (float)active->serviceMillis; // Elapsed minimo.
      if ((float)elapsed < requiredElapsed) return false; // No es seguro iniciar.
    }
  }

  return true; // Cumple condiciones de seguridad.
} // Fin de ship_scheduler_is_tico_safe.

static int findIndexForAlgoAndSide(ShipAlgo algo, Boat *readyQueue[], uint8_t readyCount, bool useSide, BoatSide side) { // Busca el mejor indice filtrando por lado opcional.
  int bestIndex = -1; // Inicializa sin candidato.
  Boat *bestBoat = NULL; // Mejor barco temporal.
  for (uint8_t i = 0; i < readyCount; i++) { // Recorre cola completa.
    Boat *candidate = readyQueue[i]; // Obtiene candidato.
    if (!candidate) continue; // Salta nulos.
    if (useSide && candidate->origin != side) continue; // Filtra por lado cuando aplica.
    if (candidate_is_better(algo, candidate, bestBoat)) { // Evalua si mejora.
      bestBoat = candidate; // Actualiza mejor barco.
      bestIndex = i; // Actualiza mejor indice.
    }
  }
  return bestIndex; // Retorna indice o -1.
} // Fin de findIndexForAlgoAndSide.

// Intenta reservar un rango de casillas [startIndex, startIndex + steps - 1] para el barco.
// Retorna true si la reserva fue exitosa (y marca las casillas con boat->id), false si no se pudo.
bool ship_scheduler_try_reserve_range(ShipScheduler *scheduler, int startIndex, uint8_t steps, Boat *boat) {
  if (!scheduler || !boat || steps == 0) return false;
  if (scheduler->listLength == 0 || !scheduler->slotOwner) return false;

  if ((SemaphoreHandle_t)scheduler->channelSlotsGuard == NULL) return false;
  // Protege el acceso a slotOwner
  if (xSemaphoreTake((SemaphoreHandle_t)scheduler->channelSlotsGuard, pdMS_TO_TICKS(100)) != pdTRUE) return false;

  int dir = (boat->origin == SIDE_LEFT) ? 1 : -1;
  int endIndex = startIndex + dir * (steps - 1);
  // Asegura indices dentro de rango
  if (startIndex < 0 || startIndex >= scheduler->listLength || endIndex < 0 || endIndex >= scheduler->listLength) {
    xSemaphoreGive((SemaphoreHandle_t)scheduler->channelSlotsGuard);
    return false;
  }

  // Verifica que todas las casillas esten libres
  int idx = startIndex;
  for (uint8_t s = 0; s < steps; s++) {
    if (scheduler->slotOwner[idx] != 0) { // ocupado
      xSemaphoreGive((SemaphoreHandle_t)scheduler->channelSlotsGuard);
      return false;
    }
    idx += dir;
  }

  // Marca las casillas con el id del barco
  idx = startIndex;
  for (uint8_t s = 0; s < steps; s++) {
    scheduler->slotOwner[idx] = boat->id;
    idx += dir;
  }

  xSemaphoreGive((SemaphoreHandle_t)scheduler->channelSlotsGuard);
  return true;
}

// Intenta mover el barco reservando el rango y dejando solo la casilla final ocupada.
static bool ship_scheduler_try_move_range(ShipScheduler *scheduler, int startIndex, uint8_t steps, Boat *boat, int16_t *outNewSlot) {
  if (!scheduler || !boat || steps == 0) return false;
  if (scheduler->listLength == 0 || !scheduler->slotOwner) return false;
  if ((SemaphoreHandle_t)scheduler->channelSlotsGuard == NULL) return false;

  if (xSemaphoreTake((SemaphoreHandle_t)scheduler->channelSlotsGuard, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  int dir = (boat->origin == SIDE_LEFT) ? 1 : -1;
  int endIndex = startIndex + dir * (steps - 1);
  if (startIndex < 0 || startIndex >= scheduler->listLength || endIndex < 0 || endIndex >= scheduler->listLength) {
    xSemaphoreGive((SemaphoreHandle_t)scheduler->channelSlotsGuard);
    return false;
  }

  // Verifica que el rango este libre (o ya pertenezca al mismo barco).
  int idx = startIndex;
  for (uint8_t s = 0; s < steps; s++) {
    uint8_t owner = scheduler->slotOwner[idx];
    if (owner != 0 && owner != boat->id) {
      xSemaphoreGive((SemaphoreHandle_t)scheduler->channelSlotsGuard);
      return false;
    }
    idx += dir;
  }

  // Limpia cualquier casilla previa del mismo barco en el rango.
  idx = startIndex;
  for (uint8_t s = 0; s < steps; s++) {
    if (scheduler->slotOwner[idx] == boat->id) scheduler->slotOwner[idx] = 0;
    idx += dir;
  }

  // Libera la casilla actual si pertenece al barco.
  if (boat->currentSlot >= 0 && boat->currentSlot < scheduler->listLength) {
    if (scheduler->slotOwner[boat->currentSlot] == boat->id) scheduler->slotOwner[boat->currentSlot] = 0;
  }

  // Marca solo la casilla final como ocupada.
  scheduler->slotOwner[endIndex] = boat->id;
  if (outNewSlot) *outNewSlot = (int16_t)endIndex;

  xSemaphoreGive((SemaphoreHandle_t)scheduler->channelSlotsGuard);
  return true;
}

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
      if (active->taskHandle) xTaskNotify(active->taskHandle, NOTIF_CMD_SLOT_UPDATE, eSetValueWithOverwrite);
    }
    return;
  }
  int idx = startIndex;
  for (uint8_t s = 0; s < steps; s++) {
    if (scheduler->slotOwner[idx] == boat->id) scheduler->slotOwner[idx] = 0;
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

  if (scheduler->slotOwner) {
    free(scheduler->slotOwner);
    scheduler->slotOwner = NULL;
  }

  scheduler->slotOwner = (uint8_t *)malloc(scheduler->listLength * sizeof(uint8_t));
  if (scheduler->slotOwner) {
    for (uint16_t i = 0; i < scheduler->listLength; i++) scheduler->slotOwner[i] = 0;
  }
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
  bool serviceExpired = false; // Indica que el tiempo de servicio ya se agoto pero aun falta llegar visualmente al final.
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
        lastTickAt = millis(); // Reinicia base temporal al arrancar.
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
          // Intenta reservar la casilla de entrada
          int entryIndex = (b->origin == SIDE_LEFT) ? 0 : (int)(s->listLength - 1);
          ship_logf("[BOAT TASK #%u] movesCount=%d perMoveMs=%lu totalSlots=%d stepSize=%u\n", b->id, movesCount, perMoveMs, totalSlotsToTravel, b->stepSize);
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
              // Retroceder a estado pausado: permitirá reintentar al recibir RUN
              running = false;
              b->allowedToMove = false;
              break;
            }
            // Si fue NOTIF_CMD_SLOT_UPDATE o cualquier otro, reintenta reservar.
          }
          // Dibuja una vez que consiguió la entrada.
          ship_display_render_forced(s);
        }
      }
      continue; // Repite el ciclo.
    } 

    const unsigned long slice = 50; // Subpaso interno. 
    bool interrupted = false; // Indica si se interrumpio el paso por pausa/termino. 
    xTaskNotifyWait(0x00, 0xFFFFFFFF, &cmd, pdMS_TO_TICKS(slice)); // Espera o recibe comando.
    unsigned long now = millis(); // Marca temporal actual.
    unsigned long elapsed = now - lastTickAt; // Tiempo real transcurrido desde el ultimo descuento.

    if (elapsed > 0) { // Solo descuenta si hubo tiempo real.
      if (elapsed >= b->remainingMillis) { // Si ya se agoto el tiempo.
        b->remainingMillis = 0; // Fuerza a cero.
        serviceExpired = true; // El reloj ya se consumo.
      } else { // Si aun queda tiempo.
        b->remainingMillis -= elapsed; // Reduce por tiempo real transcurrido.
      }
      lastTickAt = now; // Actualiza base temporal.
    }

    if (serviceExpired && b->currentSlot >= 0 && gScheduler) { // Mantiene vivo el hilo hasta llegar al final visual.
      ShipScheduler *s = gScheduler; // Toma scheduler actual.
      int endIndex = (b->origin == SIDE_LEFT) ? (s->listLength - 1) : 0; // Ultima casilla segun origen.
      if ((b->origin == SIDE_LEFT && b->currentSlot < endIndex) || (b->origin == SIDE_RIGHT && b->currentSlot > endIndex)) {
        b->remainingMillis = 1; // Evita que el scheduler lo finalize antes de tiempo.
      }
    }

    // acumula para decidir mover de casilla cuando ocurra
    if (running && b->allowedToMove && perMoveMs > 0 && b->currentSlot >= 0) {
      moveAccum += elapsed;
    }

      if (cmd == NOTIF_CMD_TERMINATE) { // Si terminate. 
        b->remainingMillis = 0; // Fuerza fin. 
        running = false; // Detiene ejecucion. 
        b->allowedToMove = false; // Bloquea movimiento. 
        interrupted = true; // Marca interrupcion. 
        if (gScheduler) ship_display_render_forced(gScheduler); // Refresca para mostrar estado terminado.
        break; // Sale del while principal para terminar la tarea. 
      } 
      if (cmd == NOTIF_CMD_PAUSE) { // Si pause.
        ship_logf("[BOAT TASK #%u] PAUSE recibido. running=false, remainingMillis=%lu\n", b->id, b->remainingMillis); // Depuracion: pausa.
        running = false; // Detiene ejecucion.
        b->allowedToMove = false; // Bloquea movimiento.
        interrupted = true; // Marca interrupcion.
        if (gScheduler) ship_display_render_forced(gScheduler); // Refresca para mostrar pausa.
        continue; // Vuelve al inicio del bucle para esperar NOTIF_CMD_RUN nuevamente.
      } 

    if (interrupted || !running) { // Si se interrumpio o quedo pausado. 
      continue; // No descuenta tiempo restante. 
    } 

    // Intentar mover cuando se acumulo suficiente tiempo
    if (running && b->allowedToMove && b->currentSlot >= 0 && moveAccum >= perMoveMs) {
      ShipScheduler *s = gScheduler;
      int16_t slot = b->currentSlot;
      int endIndex = (b->origin == SIDE_LEFT) ? (s->listLength - 1) : 0;
      int remainingSlots = (b->origin == SIDE_LEFT) ? (s->listLength - 1 - slot) : (slot - 0);
      if (remainingSlots <= 0) {
        // Llego al final; libera su casilla y termina
        ship_scheduler_release_range(s, slot, 1, b);
        b->currentSlot = -1;
        b->remainingMillis = 0;
        break;
      }

      // Decide cuantas casillas intentar mover. Para stepSize>1 intentamos
      // mover exactamente stepSize casillas (si hay suficientes restantes);
      // si quedan menos casillas que stepSize al final del canal, se permite
      // mover las restantes para terminar.
      uint8_t desiredSteps = 0;
      if (b->stepSize > 1) {
        if (remainingSlots >= b->stepSize) desiredSteps = b->stepSize;
        else if (remainingSlots > 0) desiredSteps = remainingSlots; // permitir terminar
      } else {
        desiredSteps = (uint8_t)((remainingSlots < b->stepSize) ? remainingSlots : b->stepSize);
      }

      int startReserve = slot + dir; // primer casilla a reservar
      ship_logf("[BOAT TASK #%u] desiredSteps=%u remainingSlots=%d startReserve=%d\n", b->id, desiredSteps, remainingSlots, startReserve);

      if (desiredSteps == 0) {
        // nothing to move
        continue;
      }

      int16_t newSlot = -1;

      // Para pasos múltiples: intentamos reservar atomically el rango requerido
      // usando try_reserve_range; si no se puede, esperamos notificación.
      bool reserved = false;
      if (desiredSteps > 1) {
        // Intentar reservar el rango completo sin busy-wait
        if (ship_scheduler_try_reserve_range(s, startReserve, desiredSteps, b)) {
          reserved = true;
        } else {
          uint32_t waitCmd = 0;
          xTaskNotifyWait(0x00, 0xFFFFFFFF, &waitCmd, pdMS_TO_TICKS(500));
          if (waitCmd == NOTIF_CMD_TERMINATE) {
            b->remainingMillis = 0; break;
          }
          if (waitCmd == NOTIF_CMD_PAUSE) { running = false; b->allowedToMove = false; continue; }
        }
      }

      // Si se reservo (o no se requiere reserva para stepSize==1), intentar mover.
      if (reserved || desiredSteps == 1) {
        if (ship_scheduler_try_move_range(s, startReserve, desiredSteps, b, &newSlot)) {
          b->currentSlot = newSlot;
          currentSlot = newSlot;
          moveAccum = 0; // reinicia acumulador
          ship_logf("[BOAT TASK #%u] moved to slot %d\n", b->id, newSlot);
          // Barco actualiza pantalla; ship_display_render gestiona el mutex internamente.
          ship_display_render_forced(s);
        } else {
          // Si teniamos reserva previa y el move fallo, registrar y liberar el rango y esperar
          ship_logf("[BOAT TASK #%u] blocked move: desiredSteps=%u remainingSlots=%d startReserve=%d\n", b->id, desiredSteps, remainingSlots, startReserve);
          if (reserved) ship_scheduler_release_range(s, startReserve, desiredSteps, b);
          uint32_t waitCmd = 0;
          xTaskNotifyWait(0x00, 0xFFFFFFFF, &waitCmd, pdMS_TO_TICKS(500));
          if (waitCmd == NOTIF_CMD_TERMINATE) { b->remainingMillis = 0; break; }
          if (waitCmd == NOTIF_CMD_PAUSE) { running = false; b->allowedToMove = false; continue; }
        }
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
  ship_scheduler_load_channel_config(scheduler, "channel_config.txt");
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
      xTaskNotify(b->taskHandle, NOTIF_CMD_TERMINATE, eSetValueWithOverwrite); // Ordena terminar. 
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
      xTaskNotify(active->taskHandle, NOTIF_CMD_TERMINATE, eSetValueWithOverwrite); // Termina.
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
  scheduler->fairnessPassedInWindow = 0; // Resetea ventana de equidad.
  scheduler->signLastSwitchAt = millis(); // Reinicia reloj de letrero.
  // Liberar estructura de slots si existe
  if (scheduler->slotOwner) {
    free(scheduler->slotOwner);
    scheduler->slotOwner = NULL;
  }
  if (scheduler->channelSlotsGuard) {
    vSemaphoreDelete((SemaphoreHandle_t)scheduler->channelSlotsGuard);
    scheduler->channelSlotsGuard = NULL;
  }
} // Fin de ship_scheduler_clear. 

void ship_scheduler_set_algorithm(ShipScheduler *scheduler, ShipAlgo algo) { // Configura algoritmo. 
  if (!scheduler) return; // Valida puntero. 
  scheduler->algorithm = algo; // Asigna algoritmo. 
} // Fin de ship_scheduler_set_algorithm. 

ShipAlgo ship_scheduler_get_algorithm(const ShipScheduler *scheduler) { // Lee algoritmo. 
  if (!scheduler) return ALG_FCFS; // Retorna por defecto si es nulo. 
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
  return "?"; // Alternativa. 
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

void ship_scheduler_set_flow_mode(ShipScheduler *scheduler, ShipFlowMode mode) { // Configura metodo de flujo.
  if (!scheduler) return; // Valida puntero.
  scheduler->flowMode = mode; // Asigna modo.
  scheduler->fairnessPassedInWindow = 0; // Reinicia ventana de equidad.
  scheduler->signLastSwitchAt = millis(); // Reinicia reloj del letrero.
} // Fin de ship_scheduler_set_flow_mode.

ShipFlowMode ship_scheduler_get_flow_mode(const ShipScheduler *scheduler) { // Lee metodo de flujo.
  if (!scheduler) return FLOW_TICO; // Retorna por defecto si es nulo.
  return scheduler->flowMode; // Retorna modo configurado.
} // Fin de ship_scheduler_get_flow_mode.

const char *ship_scheduler_get_flow_mode_label(const ShipScheduler *scheduler) { // Etiqueta de flujo.
  if (!scheduler) return "?"; // Retorna placeholder.
  switch (scheduler->flowMode) { // Selecciona segun modo.
    case FLOW_TICO: return "TICO"; // Sin control de lado.
    case FLOW_FAIRNESS: return "EQUIDAD"; // Ventana W.
    case FLOW_SIGN: return "LETRERO"; // Cambio por tiempo.
  }
  return "?"; // Alternativa.
} // Fin de ship_scheduler_get_flow_mode_label.

void ship_scheduler_set_fairness_window(ShipScheduler *scheduler, uint8_t windowW) { // Ajusta W.
  if (!scheduler) return; // Valida puntero.
  if (windowW == 0) windowW = 1; // Normaliza minimo.
  scheduler->fairnessWindowW = windowW; // Asigna W.
  scheduler->fairnessPassedInWindow = 0; // Reinicia conteo de ventana.
} // Fin de ship_scheduler_set_fairness_window.

uint8_t ship_scheduler_get_fairness_window(const ShipScheduler *scheduler) { // Lee W.
  if (!scheduler) return 1; // Retorna por defecto.
  return scheduler->fairnessWindowW == 0 ? 1 : scheduler->fairnessWindowW; // Retorna W normalizado.
} // Fin de ship_scheduler_get_fairness_window.

void ship_scheduler_set_sign_direction(ShipScheduler *scheduler, BoatSide side) { // Ajusta direccion del letrero.
  if (!scheduler) return; // Valida puntero.
  scheduler->signDirection = side; // Asigna lado.
  scheduler->signLastSwitchAt = millis(); // Reinicia reloj del letrero.
} // Fin de ship_scheduler_set_sign_direction.

BoatSide ship_scheduler_get_sign_direction(const ShipScheduler *scheduler) { // Lee direccion del letrero.
  if (!scheduler) return SIDE_LEFT; // Retorna por defecto.
  return scheduler->signDirection; // Retorna lado.
} // Fin de ship_scheduler_get_sign_direction.

void ship_scheduler_set_sign_interval(ShipScheduler *scheduler, unsigned long intervalMillis) { // Ajusta periodo del letrero.
  if (!scheduler) return; // Valida puntero.
  if (intervalMillis < 1000) intervalMillis = 1000; // Fuerza minimo estable.
  scheduler->signIntervalMillis = intervalMillis; // Asigna periodo.
  scheduler->signLastSwitchAt = millis(); // Reinicia reloj del letrero.
} // Fin de ship_scheduler_set_sign_interval.

unsigned long ship_scheduler_get_sign_interval(const ShipScheduler *scheduler) { // Lee periodo del letrero.
  if (!scheduler) return 0; // Retorna cero si es nulo.
  return scheduler->signIntervalMillis; // Retorna periodo.
} // Fin de ship_scheduler_get_sign_interval.

void ship_scheduler_set_max_ready_queue(ShipScheduler *scheduler, uint8_t limit) { // Ajusta tamano maximo de cola.
  if (!scheduler) return; // Valida puntero.
  if (limit == 0) limit = 1; // Fuerza minimo.
  if (limit > MAX_BOATS) limit = MAX_BOATS; // Fuerza maximo.
  scheduler->maxReadyQueueConfigured = limit; // Asigna limite.
} // Fin de ship_scheduler_set_max_ready_queue.

uint8_t ship_scheduler_get_max_ready_queue(const ShipScheduler *scheduler) { // Lee cola maxima.
  if (!scheduler) return MAX_BOATS; // Retorna por defecto.
  if (scheduler->maxReadyQueueConfigured == 0 || scheduler->maxReadyQueueConfigured > MAX_BOATS) return MAX_BOATS; // Normaliza.
  return scheduler->maxReadyQueueConfigured; // Retorna limite.
} // Fin de ship_scheduler_get_max_ready_queue.

void ship_scheduler_set_channel_length(ShipScheduler *scheduler, uint16_t meters) { // Ajusta largo del canal.
  if (!scheduler) return; // Valida puntero.
  if (meters == 0) meters = 1; // Fuerza minimo valido.
  scheduler->channelLengthMeters = meters; // Asigna largo.
} // Fin de ship_scheduler_set_channel_length.

uint16_t ship_scheduler_get_channel_length(const ShipScheduler *scheduler) { // Lee largo del canal.
  if (!scheduler) return 0; // Retorna cero si es nulo.
  return scheduler->channelLengthMeters; // Retorna largo.
} // Fin de ship_scheduler_get_channel_length.

void ship_scheduler_set_boat_speed(ShipScheduler *scheduler, uint16_t metersPerSec) { // Ajusta velocidad base.
  if (!scheduler) return; // Valida puntero.
  if (metersPerSec == 0) metersPerSec = 1; // Fuerza minimo valido.
  scheduler->boatSpeedMetersPerSec = metersPerSec; // Asigna velocidad.
} // Fin de ship_scheduler_set_boat_speed.

uint16_t ship_scheduler_get_boat_speed(const ShipScheduler *scheduler) { // Lee velocidad base.
  if (!scheduler) return 0; // Retorna cero si es nulo.
  return scheduler->boatSpeedMetersPerSec; // Retorna velocidad.
} // Fin de ship_scheduler_get_boat_speed.

void ship_scheduler_set_flow_logging(ShipScheduler *scheduler, bool enabled) { // Ajusta trazas de flujo.
  if (!scheduler) return; // Valida puntero.
  scheduler->flowLoggingEnabled = enabled; // Aplica valor.
} // Fin de ship_scheduler_set_flow_logging.

bool ship_scheduler_get_flow_logging(const ShipScheduler *scheduler) { // Lee trazas de flujo.
  if (!scheduler) return false; // Retorna por defecto.
  return scheduler->flowLoggingEnabled; // Retorna estado.
} // Fin de ship_scheduler_get_flow_logging.

void ship_scheduler_set_tico_margin_factor(ShipScheduler *scheduler, BoatType activeType, BoatType candidateType, float factor) {
  if (!scheduler) return;
  if (activeType < 0 || activeType >= 3) return;
  if (candidateType < 0 || candidateType >= 3) return;
  if (factor <= 0.0f) factor = 1.0f;
  scheduler->ticoMarginFactor[activeType][candidateType] = factor;
}

float ship_scheduler_get_tico_margin_factor(const ShipScheduler *scheduler, BoatType activeType, BoatType candidateType) {
  if (!scheduler) return 1.0f;
  if (activeType < 0 || activeType >= 3) return 1.0f;
  if (candidateType < 0 || candidateType >= 3) return 1.0f;
  return scheduler->ticoMarginFactor[activeType][candidateType];
}

void ship_scheduler_set_sensor_enabled(ShipScheduler *scheduler, bool enabled) { // Habilita/deshabilita sensor.
  if (!scheduler) return; // Valida puntero.
  scheduler->sensorActive = enabled; // Aplica valor.
  if (enabled) {
    ship_logln("[SENSOR] Sensor de proximidad activado");
  } else {
    ship_logln("[SENSOR] Sensor de proximidad desactivado");
  }
} // Fin de ship_scheduler_set_sensor_enabled.

bool ship_scheduler_get_sensor_enabled(const ShipScheduler *scheduler) { // Lee estado del sensor.
  if (!scheduler) return false; // Retorna por defecto.
  return scheduler->sensorActive; // Retorna estado.
} // Fin de ship_scheduler_get_sensor_enabled.

void ship_scheduler_set_proximity_threshold(ShipScheduler *scheduler, uint16_t cm) { // Ajusta umbral en cm.
  if (!scheduler) return; // Valida puntero.
  if (cm < 10) cm = 10; // Minimo 10cm.
  if (cm > 500) cm = 500; // Maximo 500cm.
  scheduler->proximityThresholdCm = cm; // Aplica umbral.
  ship_logf("[SENSOR] Umbral de proximidad ajustado a %u cm\n", cm);
} // Fin de ship_scheduler_set_proximity_threshold.

uint16_t ship_scheduler_get_proximity_threshold(const ShipScheduler *scheduler) { // Lee umbral en cm.
  if (!scheduler) return 150; // Retorna por defecto.
  return scheduler->proximityThresholdCm; // Retorna umbral.
} // Fin de ship_scheduler_get_proximity_threshold.

void ship_scheduler_set_proximity_distance(ShipScheduler *scheduler, uint16_t cm) { // Ajusta distancia actual (simulada).
  if (!scheduler) return; // Valida puntero.
  scheduler->proximityCurrentDistanceCm = cm; // Aplica distancia.
  scheduler->proximityDistanceIsSimulated = false; // Marca entrada como real.
  // Verifica si se activa la emergencia por proximidad
  if (scheduler->sensorActive && scheduler->emergencyMode == EMERGENCY_NONE && cm <= scheduler->proximityThresholdCm) {
    ship_logf("[SENSOR] ALERTA: Barco a %u cm (umbral: %u cm)\n", cm, scheduler->proximityThresholdCm);
    ship_scheduler_trigger_emergency(scheduler);
  }
} // Fin de ship_scheduler_set_proximity_distance.

void ship_scheduler_set_proximity_distance_simulated(ShipScheduler *scheduler, uint16_t cm) { // Ajusta distancia usando simulate.
  if (!scheduler) return; // Valida puntero.
  scheduler->proximityCurrentDistanceCm = cm; // Aplica distancia simulada.
  scheduler->proximityDistanceIsSimulated = true; // Marca que viene de simulate.
  // Verifica si se activa la emergencia por proximidad
  if (scheduler->sensorActive && scheduler->emergencyMode == EMERGENCY_NONE && cm <= scheduler->proximityThresholdCm) {
    ship_logf("[SENSOR] ALERTA: Barco a %u cm (umbral: %u cm)\n", cm, scheduler->proximityThresholdCm);
    ship_scheduler_trigger_emergency(scheduler);
  }
} // Fin de ship_scheduler_set_proximity_distance_simulated.

uint16_t ship_scheduler_get_proximity_distance(const ShipScheduler *scheduler) { // Lee distancia actual.
  if (!scheduler) return 999; // Retorna por defecto "lejano".
  return scheduler->proximityCurrentDistanceCm; // Retorna distancia.
} // Fin de ship_scheduler_get_proximity_distance.

ShipEmergencyMode ship_scheduler_get_emergency_mode(const ShipScheduler *scheduler) { // Lee modo de emergencia.
  if (!scheduler) return EMERGENCY_NONE; // Retorna por defecto.
  return scheduler->emergencyMode; // Retorna modo actual.
} // Fin de ship_scheduler_get_emergency_mode.

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

  uint8_t insertAt = scheduler->readyCount; // Posicion de insercion. 
  while (insertAt > 0 && scheduler->readyQueue[insertAt - 1]->arrivalOrder > boat->arrivalOrder) { // Mantiene orden por llegada. 
    scheduler->readyQueue[insertAt] = scheduler->readyQueue[insertAt - 1]; // Desplaza a la derecha. 
    insertAt--; // Decrementa indice. 
  } 

  scheduler->readyQueue[insertAt] = boat; // Inserta el barco. 
  scheduler->readyCount++; // Incrementa la cola. 

  ship_logf("Barco agregado: #%u tipo=%s origen=%s\n", boat->id, boatTypeName(boat->type), boatSideName(boat->origin)); // Log detallado de alta. 

  xTaskCreate(boatTask, "boat", 4096, boat, 1, &boat->taskHandle); // Crea la tarea del barco. 

  if (scheduler->activeCount > 0 && scheduler->activeBoat) { // Si hay activo. 
    if (scheduler->flowMode == FLOW_TICO) { // En TICO no preempta activos.
      return; // Mantiene la ejecucion concurrente.
    }
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
      ship_scheduler_remove_active(scheduler, preempted); // Quita de activos. 
      ship_scheduler_requeue_boat(scheduler, preempted, true); // Reencola el activo. 
      ship_scheduler_start_next_boat(scheduler); // Inicia el siguiente. 
    } 
  } 
} // Fin de ship_scheduler_enqueue. 

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

static bool ship_scheduler_start_next_boat(ShipScheduler *scheduler) { // Selecciona y arranca el siguiente barco. 
  if (!scheduler || scheduler->readyCount == 0) return false; // Valida estado. 
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
      scheduler->collisionDetections++; // Registra intento de colision.
      //ship_logf("Colision evitada entre sentidos opuestos (#%u y #%u).\n", active->id, b->id); // Reporta evento.
      FLOW_LOG(scheduler, "[FLOW][SAFE] Requeue por seguridad: activo #%u (%s), candidato #%u (%s)\n", active->id, boatSideName(active->origin), b->id, boatSideName(b->origin)); // Traza de seguridad.
      ship_scheduler_requeue_boat(scheduler, b, true); // Reencola el barco para reintento.
      return false; // No inicia para evitar choque.
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

  // Antes de añadir como activo, intentar reservar la casilla de entrada.
  if (scheduler->listLength > 0 && scheduler->slotOwner) {
    int entryIndex = (b->origin == SIDE_LEFT) ? 0 : (int)(scheduler->listLength - 1);
    if (!ship_scheduler_try_reserve_range(scheduler, entryIndex, 1, b)) {
      FLOW_LOG(scheduler, "[DISPATCH] No se pudo reservar entrada para barco #%u, reencolando\n", b->id);
      ship_scheduler_requeue_boat(scheduler, b, true); // Reencola el barco para reintento.
      return false; // No inicia ahora.
    }
    // Reserva exitosa: marca currentSlot del barco.
    b->currentSlot = entryIndex;
  }

  if (b->serviceMillis == 0) { // Calcula tiempos reales antes de notificar al hilo.
    b->serviceMillis = ship_scheduler_estimate_service_millis(scheduler, b); // Estimacion unica.
    b->remainingMillis = b->serviceMillis; // Sincroniza restante.
    b->deadlineMillis = b->startedAt + (b->serviceMillis * 2UL); // Recalcula deadline heuristico.
  }

  ship_scheduler_add_active(scheduler, b); // Agrega a la lista de activos.
  b->allowedToMove = true; // Permite avance al despachar.
  ship_logf("Dispatching -> barco #%u (rem=%lu svc=%lu)\n", b->id, b->remainingMillis, b->serviceMillis);
  if (scheduler->flowMode == FLOW_FAIRNESS && b->origin == scheduler->fairnessCurrentSide) { // Cuenta solo despachos reales.
    scheduler->fairnessPassedInWindow++; // Incrementa barcos realmente despachados en ventana W.
    FLOW_LOG(scheduler, "[FLOW][FAIR] Despachado #%u lado=%s ventana=%u/%u\n", b->id, boatSideName(b->origin), scheduler->fairnessPassedInWindow, ship_scheduler_get_fairness_window(scheduler)); // Traza de despacho.
  }
  if (b->taskHandle) {
    FLOW_LOG(scheduler, "[DISPATCH DEBUG] Enviando NOTIF_CMD_RUN a barco #%u (taskHandle=%p)\n", b->id, (void*)b->taskHandle);
    xTaskNotify(b->taskHandle, NOTIF_CMD_RUN, eSetValueWithOverwrite); // Arranca la tarea.
    scheduler->activeQuantumStartedAt = millis();
  } else {
    FLOW_LOG(scheduler, "[DISPATCH DEBUG] ERROR: barco #%u NO TIENE taskHandle!\n", b->id);
  }

  ship_logf("Start -> barco #%u\n", b->id); // Log del inicio. 
  return true; // Indica que se despacho un barco.
} // Fin de ship_scheduler_start_next_boat. 

static void ship_scheduler_preempt_active_for_rr(ShipScheduler *scheduler) { // Preempcion por RR. 
  if (!scheduler || scheduler->activeCount == 0 || !scheduler->activeBoat) return; // Valida activo. 
  if (scheduler->flowMode == FLOW_TICO) return; // En TICO no se preempta.
  if (scheduler->readyCount == 0) return; // Si no hay listos, sale. 
  if (ship_scheduler_get_active_elapsed_millis(scheduler) < scheduler->rrQuantumMillis) return; // Si no consume quantum, sale. 

  Boat *preempted = scheduler->activeBoat; // Activo primario que consumio su quantum.
  if (preempted->taskHandle) {
    FLOW_LOG(scheduler, "[RR] Pausando activo #%u por quantum\n", preempted->id);
    preempted->allowedToMove = false; // Detener movimiento pero conservar la casilla.
    xTaskNotify(preempted->taskHandle, NOTIF_CMD_PAUSE, eSetValueWithOverwrite); // Pausa la tarea.
  }

  // Rotar la lista de activos para que el siguiente activo sea el nuevo primario.
  if (scheduler->activeCount > 1) {
    Boat *first = scheduler->activeBoats[0];
    for (uint8_t i = 1; i < scheduler->activeCount; i++) scheduler->activeBoats[i - 1] = scheduler->activeBoats[i];
    scheduler->activeBoats[scheduler->activeCount - 1] = first; // Pone el antiguo al final.
    ship_scheduler_sync_primary_active(scheduler); // Actualiza activeBoat.
  }

  // Intenta despachar un barco listo (si hay espacio y se puede reservar entrada).
  if (!ship_scheduler_start_next_boat(scheduler)) {
    // Si no se pudo despachar ninguno, reanudar al siguiente activo pausado (si existe).
    if (scheduler->activeCount > 0 && scheduler->activeBoat && !scheduler->activeBoat->allowedToMove) {
      Boat *next = scheduler->activeBoat;
      if (next->taskHandle) {
        next->allowedToMove = true;
        FLOW_LOG(scheduler, "[RR] Reanudando activo #%u para usar su quantum\n", next->id);
        xTaskNotify(next->taskHandle, NOTIF_CMD_RUN, eSetValueWithOverwrite);
        scheduler->activeQuantumStartedAt = millis();
      }
    }
  }
} // Fin de ship_scheduler_preempt_active_for_rr. 

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
    ship_scheduler_preempt_active_for_rr(scheduler); // Aplica preempcion por quantum.
    // Intentar despachar tantos barcos como sea posible (llenar canal hasta limite),
    // respetando las protecciones de colision y reserva de entrada.
    bool started = true;
    while (started && scheduler->readyCount > 0 && scheduler->activeCount < MAX_BOATS) {
      started = ship_scheduler_start_next_boat(scheduler);
    }
    return;
  }

  bool started = true; // Bandera de despachos.
  while (started && scheduler->readyCount > 0 && scheduler->activeCount < MAX_BOATS) { // Mientras haya candidatos.
    started = ship_scheduler_start_next_boat(scheduler); // Intenta despachar.
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
  // Use the quantum start timestamp if available to measure elapsed within the quantum
  if (scheduler->activeQuantumStartedAt > 0) {
    unsigned long now = millis();
    if (now >= scheduler->activeQuantumStartedAt) return now - scheduler->activeQuantumStartedAt;
    return 0;
  }
  return ship_scheduler_boat_elapsed_millis(scheduler->activeBoat); // Fallback: total elapsed.
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
        xTaskNotify(active->taskHandle, NOTIF_CMD_PAUSE, eSetValueWithOverwrite); // Envia pausa.
      }
      ship_logf("Pausado barco #%u\n", active->id); // Log de pausa.
    }
    return; // Sale.
  }

  if (scheduler->activeBoat && scheduler->activeBoat->taskHandle) { // Si hay tarea. 
    scheduler->activeBoat->allowedToMove = false; // Congela movimiento del barco. 
    xTaskNotify(scheduler->activeBoat->taskHandle, NOTIF_CMD_PAUSE, eSetValueWithOverwrite); // Envia pausa. 
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
        xTaskNotify(active->taskHandle, NOTIF_CMD_RUN, eSetValueWithOverwrite); // Envia run.
      }
      ship_logf("Reanudado barco #%u\n", active->id); // Log de reanudacion.
    }
    return; // Sale.
  }

  if (scheduler->activeBoat && scheduler->activeBoat->taskHandle) { // Si hay tarea. 
    scheduler->activeBoat->allowedToMove = true; // Permite movimiento del barco. 
    xTaskNotify(scheduler->activeBoat->taskHandle, NOTIF_CMD_RUN, eSetValueWithOverwrite); // Envia run. 
  } 
  ship_logf("Reanudado barco #%u\n", scheduler->activeBoat ? scheduler->activeBoat->id : 0); // Log de reanudacion. 
} // Fin de ship_scheduler_resume_active. 

void ship_scheduler_trigger_emergency(ShipScheduler *scheduler) { // Activa emergencia por proximidad.
  if (!scheduler) return; // Valida puntero.
  
  scheduler->emergencyMode = EMERGENCY_PROXIMITY_ALERT; // Marca alerta.
  scheduler->emergencyStartedAt = millis(); // Registra timestamp.
  
  ship_logln("[EMERGENCY] ¡¡¡ ALERTA DE PROXIMIDAD !!!"); // Alerta loudly.
  ship_logln("[EMERGENCY] Cerrando compuertas..."); // Aviso de cierre.
  
  // Cierra puertas (simulado)
  scheduler->gateLeftClosed = 2; // Puerta izquierda cerrada (inmediatamente en simulacion).
  scheduler->gateRightClosed = 2; // Puerta derecha cerrada.
  scheduler->emergencyMode = EMERGENCY_GATES_CLOSED; // Marca puertas cerradas.
  
  ship_logln("[EMERGENCY] Compuertas CERRADAS"); // Confirma cierre.
  
  // Congela temporalmente los barcos activos mientras dura la emergencia.
  if (scheduler->activeCount > 0) {
    for (uint8_t i = 0; i < scheduler->activeCount; i++) {
      Boat *activeBoat = scheduler->activeBoats[i]; // Copia referencia.
      if (!activeBoat) continue; // Salta nulos.
      activeBoat->allowedToMove = false; // Bloquea movimiento.
      if (activeBoat->taskHandle) {
        xTaskNotify(activeBoat->taskHandle, NOTIF_CMD_PAUSE, eSetValueWithOverwrite); // Pausa la tarea.
      }
      ship_logf("[EMERGENCY] Barco #%u congelado en el canal\n", activeBoat->id); // Aviso.

      ship_logf("[EMERGENCY] Intentando recolocar barco #%u en cola (readyCount=%u, maxConfigured=%u)\n", activeBoat->id, scheduler->readyCount, scheduler->maxReadyQueueConfigured); // Log de depuracion.
      if (scheduler->readyCount < scheduler->maxReadyQueueConfigured) {
        activeBoat->remainingMillis = activeBoat->serviceMillis; // Reinicia el cruce.
        activeBoat->state = STATE_WAITING; // Lo devuelve a cola.
        activeBoat->allowedToMove = false; // Sigue detenido hasta el siguiente despacho.
        activeBoat->startedAt = 0; // Fuerza que el siguiente despacho sea un viaje nuevo.
        activeBoat->enqueuedAt = millis(); // Registra nuevo ingreso a cola tras la emergencia.
        scheduler->readyQueue[scheduler->readyCount] = activeBoat; // Lo agrega al final.
        scheduler->readyCount++; // Incrementa contador.
        ship_logf("[EMERGENCY] Barco #%u recolocado en cola en posicion %u (rem=%lu svc=%lu)\n", activeBoat->id, scheduler->readyCount - 1, activeBoat->remainingMillis, activeBoat->serviceMillis); // Confirma recolocacion y tiempos.
      } else {
        ship_logf("[EMERGENCY] Cola llena: barco #%u no puede recolocarse, se destruye\n", activeBoat->id); // Error: cola llena.
        destroyBoat(activeBoat); // Destruye si no cabe.
      }
      scheduler->activeBoats[i] = NULL; // Limpia el slot.
    }

    scheduler->activeCount = 0; // Limpia activos.
    scheduler->activeBoat = NULL; // Limpia puntero activo.
    scheduler->hasActiveBoat = false; // Limpia estado activo.
    scheduler->crossingStartedAt = 0; // Reinicia timestamp de cruce.
  }

  scheduler->emergencyMode = EMERGENCY_RECOVERY; // Marca en recuperacion.
  ship_logln("[EMERGENCY] Modo: RECOVERY (esperando apertura de compuertas)"); // Log estado.
} // Fin de ship_scheduler_trigger_emergency.

void ship_scheduler_clear_emergency(ShipScheduler *scheduler) { // Limpia el estado de emergencia.
  if (!scheduler) return; // Valida puntero.
  
  if (scheduler->emergencyMode != EMERGENCY_NONE) {
    ship_logln("[EMERGENCY] Limpiando estado de emergencia..."); // Aviso.
    scheduler->gateLeftClosed = 0; // Abre puerta izquierda.
    scheduler->gateRightClosed = 0; // Abre puerta derecha.
    ship_logln("[EMERGENCY] Compuertas ABIERTAS"); // Confirma apertura.

    if (scheduler->proximityDistanceIsSimulated) { // Si la distancia venia de simulate.
      scheduler->proximityCurrentDistanceCm = 120; // Resetea a distancia segura.
      scheduler->proximityDistanceIsSimulated = false; // Limpia bandera de simulacion.
      ship_logln("[SENSOR] distancia: 120 cm"); // Confirma reseteo para la pantalla.
    }
  }
  
  scheduler->emergencyMode = EMERGENCY_NONE; // Sin emergencia.
  scheduler->emergencyStartedAt = 0; // Limpia timestamp.
  scheduler->emergencyDispatchBlockedLogged = false; // Reinicia bandera para futuros eventos.
  ship_logln("[EMERGENCY] Estado: NORMAL"); // Log retorno a normal.
} // Fin de ship_scheduler_clear_emergency.

void ship_scheduler_update_emergency(ShipScheduler *scheduler) { // Actualiza estado de emergencia (llamar en tick).
  if (!scheduler) return; // Valida puntero.
  
  // Si el sensor esta activo, revisa la distancia actual
  if (scheduler->sensorActive && scheduler->emergencyMode == EMERGENCY_NONE) {
    if (scheduler->proximityCurrentDistanceCm <= scheduler->proximityThresholdCm) {
      ship_scheduler_trigger_emergency(scheduler); // Activa emergencia.
    }
  }
  
  // Si estamos en recuperacion, espera a que pase el tiempo antes de limpiar
  if (scheduler->emergencyMode == EMERGENCY_RECOVERY) {
    unsigned long elapsedMs = millis() - scheduler->emergencyStartedAt;
    if (elapsedMs >= scheduler->gateLockDurationMs) {
      ship_scheduler_clear_emergency(scheduler); // Limpia emergencia tras tiempo de espera.
    }
  }
} // Fin de ship_scheduler_update_emergency.

uint8_t ship_scheduler_get_gate_left_state(const ShipScheduler *scheduler) { // Obtiene estado puerta izquierda.
  if (!scheduler) return 0; // Retorna abierta si es nulo.
  return scheduler->gateLeftClosed; // Retorna estado (0=abierta, 1=cerrando, 2=cerrada).
} // Fin de ship_scheduler_get_gate_left_state.

uint8_t ship_scheduler_get_gate_right_state(const ShipScheduler *scheduler) { // Obtiene estado puerta derecha.
  if (!scheduler) return 0; // Retorna abierta si es nulo.
  return scheduler->gateRightClosed; // Retorna estado (0=abierta, 1=cerrando, 2=cerrada).
} // Fin de ship_scheduler_get_gate_right_state.

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
