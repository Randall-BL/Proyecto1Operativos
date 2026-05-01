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
static BoatSide opposite_side(BoatSide side); // Declara lado opuesto.
static bool queue_has_side(const ShipScheduler *scheduler, BoatSide side); // Declara busqueda por lado.
static bool candidate_is_better(ShipAlgo algo, const Boat *candidate, const Boat *best); // Declara comparador.
static int findIndexForAlgoAndSide(ShipAlgo algo, Boat *readyQueue[], uint8_t readyCount, bool useSide, BoatSide side); // Declara selector filtrado.
static void ship_scheduler_tick_sign(ShipScheduler *scheduler); // Declara tick de letrero.
static int ship_scheduler_select_next_index(ShipScheduler *scheduler); // Declara selector con flujo.

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

  return candidate->arrivalOrder < best->arrivalOrder; // Fallback por llegada.
} // Fin de candidate_is_better.

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
      if (cmd == NOTIF_CMD_RUN) { // Si run, inicia. 
        running = true; // Marca ejecucion activa. 
        b->allowedToMove = true; // Permite avanzar. 
      } 
      continue; // Repite el ciclo. 
    } 

    unsigned long step = 200; // Paso de tiempo en ms. 
    if (step > b->remainingMillis) step = b->remainingMillis; // Ajusta si queda menos. 
    unsigned long slept = 0; // Acumulado de tiempo ejecutado. 
    const unsigned long slice = 50; // Subpaso interno. 
    bool interrupted = false; // Indica si se interrumpio el paso por pausa/termino. 
    while (slept < step) { // Mientras falte ejecutar el paso. 
      xTaskNotifyWait(0x00, 0xFFFFFFFF, &cmd, pdMS_TO_TICKS(slice)); // Espera o recibe comando. 
      if (cmd == NOTIF_CMD_TERMINATE) { // Si terminate. 
        b->remainingMillis = 0; // Fuerza fin. 
        running = false; // Detiene ejecucion. 
        b->allowedToMove = false; // Bloquea movimiento. 
        interrupted = true; // Marca interrupcion. 
        break; // Sale del while interno. 
      } 
      if (cmd == NOTIF_CMD_PAUSE) { // Si pause. 
        running = false; // Detiene ejecucion. 
        b->allowedToMove = false; // Bloquea movimiento. 
        interrupted = true; // Marca interrupcion. 
        break; // Sale del while interno. 
      } 

      unsigned long doSleep = slice; // Tiempo a contar. 
      if (slept + doSleep > step) doSleep = step - slept; // Ajusta si excede. 
      slept += doSleep; // Acumula tiempo. 
    } 

    if (interrupted || !running) { // Si se interrumpio o quedo pausado. 
      continue; // No descuenta tiempo restante. 
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
  if (scheduler->rrQuantumMillis < 100) scheduler->rrQuantumMillis = 1200; // Default RR.
  if (scheduler->fairnessWindowW == 0) scheduler->fairnessWindowW = 2; // Default W.
  if (scheduler->signIntervalMillis < 1000) scheduler->signIntervalMillis = 8000; // Default de letrero.
  if (scheduler->maxReadyQueueConfigured == 0 || scheduler->maxReadyQueueConfigured > MAX_BOATS) scheduler->maxReadyQueueConfigured = MAX_BOATS; // Limite de cola por defecto.
  if (scheduler->channelLengthMeters == 0) scheduler->channelLengthMeters = 120; // Largo default del canal.
  if (scheduler->boatSpeedMetersPerSec == 0) scheduler->boatSpeedMetersPerSec = 18; // Velocidad default.
  scheduler->flowMode = FLOW_TICO; // Default de flujo (sin control).
  scheduler->signDirection = SIDE_LEFT; // Default letrero izquierda.
  scheduler->signLastSwitchAt = millis(); // Marca inicial de letrero.
  scheduler->fairnessCurrentSide = SIDE_LEFT; // Lado inicial de equidad.
  scheduler->fairnessPassedInWindow = 0; // Reinicia ventana de equidad.
  scheduler->collisionDetections = 0; // Reinicia contador de colisiones.
  // Inicializa sensor e interrupciones
  scheduler->sensorActive = false; // Sensor deshabilitado por defecto.
  scheduler->proximityThresholdCm = 150; // Umbral de 150cm por defecto.
  scheduler->proximityCurrentDistanceCm = 999; // Distancia inicial "lejana".
  scheduler->proximityDistanceIsSimulated = false; // Distancia real por defecto.
  scheduler->emergencyMode = EMERGENCY_NONE; // Sin emergencia.
  scheduler->emergencyStartedAt = 0; // Sin timestamp.
  scheduler->gateLeftClosed = 0; // Puerta izquierda abierta.
  scheduler->gateRightClosed = 0; // Puerta derecha abierta.
  scheduler->gateLockDurationMs = 5000; // Cierre de 5 segundos por defecto.
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
  scheduler->fairnessPassedInWindow = 0; // Resetea ventana de equidad.
  scheduler->signLastSwitchAt = millis(); // Reinicia reloj de letrero.
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

void ship_scheduler_set_flow_mode(ShipScheduler *scheduler, ShipFlowMode mode) { // Configura metodo de flujo.
  if (!scheduler) return; // Valida puntero.
  scheduler->flowMode = mode; // Asigna modo.
  scheduler->fairnessPassedInWindow = 0; // Reinicia ventana de equidad.
  scheduler->signLastSwitchAt = millis(); // Reinicia reloj del letrero.
} // Fin de ship_scheduler_set_flow_mode.

ShipFlowMode ship_scheduler_get_flow_mode(const ShipScheduler *scheduler) { // Lee metodo de flujo.
  if (!scheduler) return FLOW_TICO; // Retorna default si es nulo.
  return scheduler->flowMode; // Retorna modo configurado.
} // Fin de ship_scheduler_get_flow_mode.

const char *ship_scheduler_get_flow_mode_label(const ShipScheduler *scheduler) { // Etiqueta de flujo.
  if (!scheduler) return "?"; // Retorna placeholder.
  switch (scheduler->flowMode) { // Selecciona segun modo.
    case FLOW_TICO: return "TICO"; // Sin control de lado.
    case FLOW_FAIRNESS: return "EQUIDAD"; // Ventana W.
    case FLOW_SIGN: return "LETRERO"; // Cambio por tiempo.
  }
  return "?"; // Fallback.
} // Fin de ship_scheduler_get_flow_mode_label.

void ship_scheduler_set_fairness_window(ShipScheduler *scheduler, uint8_t windowW) { // Ajusta W.
  if (!scheduler) return; // Valida puntero.
  if (windowW == 0) windowW = 1; // Normaliza minimo.
  scheduler->fairnessWindowW = windowW; // Asigna W.
  scheduler->fairnessPassedInWindow = 0; // Reinicia conteo de ventana.
} // Fin de ship_scheduler_set_fairness_window.

uint8_t ship_scheduler_get_fairness_window(const ShipScheduler *scheduler) { // Lee W.
  if (!scheduler) return 1; // Retorna default.
  return scheduler->fairnessWindowW == 0 ? 1 : scheduler->fairnessWindowW; // Retorna W normalizado.
} // Fin de ship_scheduler_get_fairness_window.

void ship_scheduler_set_sign_direction(ShipScheduler *scheduler, BoatSide side) { // Ajusta direccion del letrero.
  if (!scheduler) return; // Valida puntero.
  scheduler->signDirection = side; // Asigna lado.
  scheduler->signLastSwitchAt = millis(); // Reinicia reloj del letrero.
} // Fin de ship_scheduler_set_sign_direction.

BoatSide ship_scheduler_get_sign_direction(const ShipScheduler *scheduler) { // Lee direccion del letrero.
  if (!scheduler) return SIDE_LEFT; // Retorna default.
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
  if (!scheduler) return MAX_BOATS; // Retorna default.
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
  if (!scheduler) return false; // Retorna default.
  return scheduler->flowLoggingEnabled; // Retorna estado.
} // Fin de ship_scheduler_get_flow_logging.

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
  if (!scheduler) return false; // Retorna default.
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
  if (!scheduler) return 150; // Retorna default.
  return scheduler->proximityThresholdCm; // Retorna umbral.
} // Fin de ship_scheduler_get_proximity_threshold.

void ship_scheduler_set_proximity_distance(ShipScheduler *scheduler, uint16_t cm) { // Ajusta distancia actual (simulada).
  if (!scheduler) return; // Valida puntero.
  scheduler->proximityCurrentDistanceCm = cm; // Aplica distancia.
  scheduler->proximityDistanceIsSimulated = false; // Marca entrada como real.
  // Verifica si se activa la emergencia por proximidad
  if (scheduler->sensorActive && scheduler->emergencyMode == EMERGENCY_NONE && cm < scheduler->proximityThresholdCm) {
    ship_logf("[SENSOR] ALERTA: Barco a %u cm (umbral: %u cm)\n", cm, scheduler->proximityThresholdCm);
    ship_scheduler_trigger_emergency(scheduler);
  }
} // Fin de ship_scheduler_set_proximity_distance.

void ship_scheduler_set_proximity_distance_simulated(ShipScheduler *scheduler, uint16_t cm) { // Ajusta distancia usando simulate.
  if (!scheduler) return; // Valida puntero.
  scheduler->proximityCurrentDistanceCm = cm; // Aplica distancia simulada.
  scheduler->proximityDistanceIsSimulated = true; // Marca que viene de simulate.
  // Verifica si se activa la emergencia por proximidad
  if (scheduler->sensorActive && scheduler->emergencyMode == EMERGENCY_NONE && cm < scheduler->proximityThresholdCm) {
    ship_logf("[SENSOR] ALERTA: Barco a %u cm (umbral: %u cm)\n", cm, scheduler->proximityThresholdCm);
    ship_scheduler_trigger_emergency(scheduler);
  }
} // Fin de ship_scheduler_set_proximity_distance_simulated.

uint16_t ship_scheduler_get_proximity_distance(const ShipScheduler *scheduler) { // Lee distancia actual.
  if (!scheduler) return 999; // Retorna default "lejano".
  return scheduler->proximityCurrentDistanceCm; // Retorna distancia.
} // Fin de ship_scheduler_get_proximity_distance.

ShipEmergencyMode ship_scheduler_get_emergency_mode(const ShipScheduler *scheduler) { // Lee modo de emergencia.
  if (!scheduler) return EMERGENCY_NONE; // Retorna default.
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
    return findIndexForAlgo(scheduler->algorithm, scheduler->readyQueue, scheduler->readyCount); // Elige por algoritmo.
  }

  if (scheduler->flowMode == FLOW_SIGN) { // Modo letrero.
    BoatSide allowed = scheduler->signDirection; // Lado permitido por letrero.
    BoatSide fallback = opposite_side(allowed); // Lado alterno para no bloquear flujo.
    if (queue_has_side(scheduler, allowed)) { // Si hay barcos del lado permitido.
      FLOW_LOG(scheduler, "[FLOW][SIGN] Letrero=%s, seleccionando ese lado\n", boatSideName(allowed)); // Traza lado permitido.
      return findIndexForAlgoAndSide(scheduler->algorithm, scheduler->readyQueue, scheduler->readyCount, true, allowed); // Elige permitido.
    }
    FLOW_LOG(scheduler, "[FLOW][SIGN] Letrero=%s sin barcos; fallback a %s\n", boatSideName(allowed), boatSideName(fallback)); // Traza fallback.
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

  return findIndexForAlgo(scheduler->algorithm, scheduler->readyQueue, scheduler->readyCount); // Fallback.
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

  // Bloquea despacho si hay emergencia (puertas cerradas).
  if (scheduler->emergencyMode != EMERGENCY_NONE) {
    ship_logln("[EMERGENCY] Despacho bloqueado: puertas cerradas por emergencia");
    return; // No inicia barco mientras hay emergencia.
  }

  int idx = ship_scheduler_select_next_index(scheduler); // Busca el mejor segun politica completa. 
  if (idx < 0) return; // Sale si no hay indice. 

  Boat *b = scheduler->readyQueue[idx]; // Selecciona el barco. 
  for (uint8_t i = idx + 1; i < scheduler->readyCount; i++) scheduler->readyQueue[i - 1] = scheduler->readyQueue[i]; // Compacta cola. 
  scheduler->readyCount--; // Reduce contador. 

  if (scheduler->hasActiveBoat && scheduler->activeBoat && scheduler->activeBoat->origin != b->origin) { // Verifica colision por sentidos opuestos.
    scheduler->collisionDetections++; // Registra intento de colision.
    ship_logf("Colision evitada entre sentidos opuestos (#%u y #%u).\n", scheduler->activeBoat->id, b->id); // Reporta evento.
    FLOW_LOG(scheduler, "[FLOW][SAFE] Requeue por seguridad: activo #%u (%s), candidato #%u (%s)\n", scheduler->activeBoat->id, boatSideName(scheduler->activeBoat->origin), b->id, boatSideName(b->origin)); // Traza de seguridad.
    ship_scheduler_requeue_boat(scheduler, b, true); // Reencola el barco para reintento.
    return; // No inicia para evitar choque.
  }

  if (scheduler->flowMode == FLOW_FAIRNESS && b->origin == scheduler->fairnessCurrentSide) { // Acumula ventana solo al despachar lado vigente.
    scheduler->fairnessPassedInWindow++; // Incrementa barcos despachados en ventana W.
    FLOW_LOG(scheduler, "[FLOW][FAIR] Despachado #%u lado=%s ventana=%u/%u\n", b->id, boatSideName(b->origin), scheduler->fairnessPassedInWindow, ship_scheduler_get_fairness_window(scheduler)); // Traza de despacho.
  }

  b->state = STATE_CROSSING; // Cambia estado a cruzando. 
  if (b->startedAt == 0) b->startedAt = millis(); // Marca inicio si aplica. 
  scheduler->crossingStartedAt = millis(); // Marca el inicio del cruce. 
  scheduler->activeBoat = b; // Asigna activo. 
  scheduler->hasActiveBoat = true; // Marca flag. 
  b->allowedToMove = true; // Permite avance al despachar.
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

  ship_scheduler_update_emergency(scheduler); // Actualiza estado de emergencia.
  ship_scheduler_tick_sign(scheduler); // Actualiza cambio de direccion para modo letrero.

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
      scheduler->activeBoat->allowedToMove = false; // Congela movimiento del barco. 
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
      scheduler->activeBoat->allowedToMove = true; // Permite movimiento del barco. 
      xTaskNotify(scheduler->activeBoat->taskHandle, NOTIF_CMD_RUN, eSetValueWithOverwrite); // Envia run. 
    } 
    ship_logf("Reanudado barco #%u\n", scheduler->activeBoat->id); // Log de reanudacion. 
  } else { // Si no hay activo. 
    ship_logln("No hay barco activo para reanudar."); // Mensaje de error. 
  } 
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
  
  // Congela temporalmente el barco activo mientras dura la emergencia.
  if (scheduler->hasActiveBoat && scheduler->activeBoat) {
    Boat *activeBoat = scheduler->activeBoat; // Copia referencia.
    activeBoat->allowedToMove = false; // Bloquea movimiento.
    if (activeBoat->taskHandle) {
      xTaskNotify(activeBoat->taskHandle, NOTIF_CMD_PAUSE, eSetValueWithOverwrite); // Pausa la tarea.
    }
    ship_logf("[EMERGENCY] Barco #%u congelado en el canal\n", activeBoat->id); // Aviso.
    
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

    // Si habia un barco congelado en el canal, lo retira y lo vuelve a encolar.
    if (scheduler->hasActiveBoat && scheduler->activeBoat) {
      Boat *activeBoat = scheduler->activeBoat; // Copia referencia.
      ship_logf("[EMERGENCY] Retirando barco #%u del canal para recolocarlo en cola\n", activeBoat->id); // Aviso.

      scheduler->hasActiveBoat = false; // Limpia estado activo.
      scheduler->activeBoat = NULL; // Limpia puntero.
      scheduler->crossingStartedAt = 0; // Reinicia timestamp.

      if (scheduler->readyCount < scheduler->maxReadyQueueConfigured) {
        activeBoat->remainingMillis = activeBoat->serviceMillis; // Reinicia el cruce.
        activeBoat->state = STATE_WAITING; // Lo devuelve a cola.
        activeBoat->allowedToMove = false; // Se mantiene detenido hasta el siguiente despacho.
        scheduler->readyQueue[scheduler->readyCount] = activeBoat; // Lo agrega al final.
        scheduler->readyCount++; // Incrementa contador.
        ship_logf("[EMERGENCY] Barco #%u recolocado en cola en posicion %u\n", activeBoat->id, scheduler->readyCount - 1); // Confirma recolocacion.
      } else {
        ship_logf("[EMERGENCY] Cola llena: barco #%u no puede recolocarse, se destruye\n", activeBoat->id); // Error: cola llena.
        destroyBoat(activeBoat); // Destruye si no cabe.
      }
    }

    if (scheduler->proximityDistanceIsSimulated) { // Si la distancia venia de simulate.
      scheduler->proximityCurrentDistanceCm = 120; // Resetea a distancia segura.
      scheduler->proximityDistanceIsSimulated = false; // Limpia bandera de simulacion.
      ship_logln("[SENSOR] distancia: 120 cm"); // Confirma reseteo para el display.
    }
  }
  
  scheduler->emergencyMode = EMERGENCY_NONE; // Sin emergencia.
  scheduler->emergencyStartedAt = 0; // Limpia timestamp.
  ship_logln("[EMERGENCY] Estado: NORMAL"); // Log retorno a normal.
} // Fin de ship_scheduler_clear_emergency.

void ship_scheduler_update_emergency(ShipScheduler *scheduler) { // Actualiza estado de emergencia (llamar en tick).
  if (!scheduler) return; // Valida puntero.
  
  // Si el sensor esta activo, revisa la distancia actual
  if (scheduler->sensorActive && scheduler->emergencyMode == EMERGENCY_NONE) {
    if (scheduler->proximityCurrentDistanceCm < scheduler->proximityThresholdCm) {
      ship_scheduler_trigger_emergency(scheduler); // Activa emergencia.
    }
  }
  
  // Si estamos en recuperacion, espera a que pase el tiempo antes de limpiar
  if (scheduler->emergencyMode == EMERGENCY_RECOVERY) {
    unsigned long elapsedMs = millis() - scheduler->emergencyStartedAt;
    if (elapsedMs >= scheduler->gateLockDurationMs) {
      ship_scheduler_clear_emergency(scheduler); // Limpia emergencia tras timeout.
    }
  }
} // Fin de ship_scheduler_update_emergency.

uint8_t ship_scheduler_get_gate_left_state(const ShipScheduler *scheduler) { // Obtiene estado puerta izquierda.
  if (!scheduler) return 0; // Retorna open si es nulo.
  return scheduler->gateLeftClosed; // Retorna estado (0=open, 1=closing, 2=closed).
} // Fin de ship_scheduler_get_gate_left_state.

uint8_t ship_scheduler_get_gate_right_state(const ShipScheduler *scheduler) { // Obtiene estado puerta derecha.
  if (!scheduler) return 0; // Retorna open si es nulo.
  return scheduler->gateRightClosed; // Retorna estado (0=open, 1=closing, 2=closed).
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
  if (scheduler->hasActiveBoat && scheduler->activeBoat) { // Si hay activo. 
    ship_logf("Active: #%u rem=%lu\n", scheduler->activeBoat->id, scheduler->activeBoat->remainingMillis); // Imprime activo. 
  } else { // Si no hay activo. 
    ship_logln("Active: none"); // Informa sin activo. 
  } 
  ship_logln("------------------------"); // Cierra el bloque. 
} // Fin de ship_scheduler_dump_status. 
