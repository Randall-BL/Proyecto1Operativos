#include "ShipScheduler.h" // API del scheduler.

#include "ShipIO.h" // Logging por Serial.
#include <freertos/task.h> // xTaskNotify.

#define FLOW_LOG(schedulerPtr, fmt, ...) do { if ((schedulerPtr) && (schedulerPtr)->flowLoggingEnabled) ship_logf(fmt, ##__VA_ARGS__); } while (0)
/*
 * ShipSchedulerSensor.c — Sensor de proximidad y gestion de emergencias.
 *
 * PROPOSITO DE ESTE MODULO:
 *   Maneja el ciclo de vida completo de una emergencia provocada por el sensor
 *   de ultrasonido (HC-SR04). Cuando el sensor detecta un barco a menos de
 *   proximityThresholdCm centimetros, se activa la emergencia:
 *     1. Cierre de compuertas del canal (gateLeftClosed / gateRightClosed).
 *     2. Pausa de todas las tareas FreeRTOS de barcos activos.
 *     3. Liberacion de casillas del canal (para evitar deadlocks futuros).
 *     4. Bloqueo de nuevos despachos.
 *     5. Temporizador de recuperacion (gateLockDurationMs ms).
 *     6. Reapertura de compuertas y reanudacion de tareas al expirar el timer.
 *
 * FUNCIONES ESTATICAS (internas al modulo):
 *   Copias locales de funciones auxiliares de ShipScheduler.c usadas solo
 *   durante el manejo de emergencias, para no exponer en la API publica.
 *
 * FLUJO GENERAL DE EMERGENCIA:
 *   trigger_emergency -> EMERGENCY_GATES_CLOSED -> EMERGENCY_RECOVERY
 *   update_emergency  -> elapsed >= gateLockDurationMs -> clear_emergency
 *   clear_emergency   -> EMERGENCY_NONE (estado normal)
 */
/*
 * notify_task — wrapper local de xTaskNotify para el modulo de sensor.
 *
 * xTaskNotify(taskHandle, notificationValue, eSetValueWithOverwrite):
 *   - taskHandle    : puntero al TCB de la tarea en el heap de FreeRTOS.
 *   - notificationValue: valor uint32_t que se escribe en ulNotifiedValue del TCB.
 *   - eSetValueWithOverwrite: sobreescribe el valor anterior sin importar si la
 *     tarea ya tenia una notificacion pendiente (no se pierde si la tarea no la
 *     leyo aun; el nuevo valor reemplaza al anterior).
 *
 * El guard taskHandle != NULL es critico: llamar xTaskNotify con handle NULL
 * causa un hardfault en el Xtensa LX6 (acceso a 0x00000000).
 *
 * Esta funcion es una copia local de safe_task_notify de ShipScheduler.c;
 * se duplica aqui para evitar dependencias entre modulos de implementacion.
 */
static void notify_task(TaskHandle_t taskHandle, uint32_t notificationValue) {
  if (!taskHandle) return;
  xTaskNotify(taskHandle, notificationValue, eSetValueWithOverwrite);
}

/*
 * ship_scheduler_sync_rr_permissions_local — sincroniza allowedToMove en RR.
 *
 * En Round Robin, SOLO el barco primario (activeBoats[0], al que apunta
 * scheduler->activeBoat) debe tener allowedToMove == true en cualquier momento.
 * Todos los demas activos tienen allowedToMove == false y esperan en boatTask
 * a recibir NOTIF_CMD_RUN.
 *
 * Esta funcion es una copia local de ship_scheduler_sync_rr_permissions de
 * ShipScheduler.c. Se usa en clear_emergency para sincronizar los permisos
 * justo despues de restaurar el estado de los barcos.
 *
 * CRITERIO: active == scheduler->activeBoat
 *   Es una comparacion de PUNTEROS (direcciones de heap), no de valores.
 *   Dos Boat* distintos con los mismos campos siguen siendo objetos diferentes.
 *   Solo el barco que vive exactamente en la misma direccion que activeBoat
 *   recibe allowedToMove = true.
 */
static void ship_scheduler_sync_rr_permissions_local(ShipScheduler *scheduler) {
  if (!scheduler || scheduler->algorithm != ALG_RR) return;
  for (uint8_t i = 0; i < scheduler->activeCount; i++) {
    Boat *active = scheduler->activeBoats[i];
    if (!active) continue;
    active->allowedToMove = (active == scheduler->activeBoat);
  }
}

/*
 * ship_scheduler_freeze_active_quantum — congela el contador del quantum activo.
 *
 * Campos involucrados (unsigned long en el struct ShipScheduler en heap):
 *   activeQuantumStartedAt      : timestamp millis() del inicio del quantum actual.
 *   activeQuantumAccumulatedMillis: tiempo ya acumulado de iteraciones anteriores.
 *
 * OPERACION:
 *   accumulated += now - startedAt   (suma el tiempo transcurrido desde la ultima
 *                                    vez que el quantum empezo a correr).
 *   startedAt = 0                    (centinela: 0 significa 'quantum detenido').
 *
 * POR QUE ES NECESARIO EN EMERGENCIA:
 *   Al pausar los barcos por emergencia, el reloj de la tarea debe dejar de
 *   correr. Si no se congela, cuando se reanude la tarea get_active_elapsed
 *   devolvera (now - startedAt) con un 'now' muy lejano al startedAt original,
 *   superando el quantum inmediatamente y causando una preempcion espuria.
 *
 * ship_scheduler_resume_active_quantum:
 *   Si startedAt == 0 (quantum detenido), escribe startedAt = millis().
 *   El guard evita reiniciar el timestamp si el quantum ya esta corriendo
 *   (previene doble-conteo si se llama dos veces seguidas).
 */
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

/*
 * ship_scheduler_restore_parked_boats — restaura barcos estacionados por emergencia.
 *
 * Durante una emergencia, cada barco activo fue 'estacionado':
 *   activeBoat->emergencyParked = true      : bandera que marca el estado.
 *   activeBoat->emergencySavedSlot = slot   : casilla donde estaba cuando se paro
 *                                             (int16_t en el struct Boat en heap).
 *   activeBoat->currentSlot = -1            : el barco no ocupa casilla en el canal.
 *
 * RESTAURACION:
 *   Para cada barco con emergencyParked == true:
 *     1. Limpiar emergencyParked = false y emergencySavedSlot = -1.
 *     2. try_reserve_range(scheduler, savedSlot, 1, activeBoat):
 *        Intenta bajo mutex: slotOwner[savedSlot] == 0 (libre) -> escribe boat->id
 *        y toma el semaforo del slot. Si el slot sigue libre (nadie lo ocupo
 *        durante la emergencia), el barco regresa exactamente donde estaba.
 *        Si el slot esta ocupado, el barco se queda sin currentSlot (-1) y
 *        start_next_boat lo reubicara en el proximo despacho.
 *     3. currentSlot = savedSlot: el barco vuelve a 'estar' en el canal.
 *
 * Despues de restore_parked_boats, clear_emergency envia NOTIF_CMD_RUN a cada
 * barco para que continue moviendose desde su casilla restaurada.
 */
static void ship_scheduler_restore_parked_boats(ShipScheduler *scheduler) {
  if (!scheduler) return;
  for (uint8_t i = 0; i < scheduler->activeCount; i++) {
    Boat *activeBoat = scheduler->activeBoats[i];
    if (!activeBoat || !activeBoat->emergencyParked) continue;
    int16_t savedSlot = activeBoat->emergencySavedSlot;
    activeBoat->emergencyParked = false;
    activeBoat->emergencySavedSlot = -1;
    if (savedSlot >= 0 && scheduler->listLength > 0 && scheduler->slotOwner) {
      if (ship_scheduler_try_reserve_range(scheduler, (int)savedSlot, 1, activeBoat)) {
        activeBoat->currentSlot = savedSlot;
        FLOW_LOG(scheduler, "[EMERGENCY] Barco #%u restaurado en casilla %d\n", activeBoat->id, savedSlot);
      } else {
        FLOW_LOG(scheduler, "[EMERGENCY] No se pudo restaurar casilla %d para barco #%u\n", savedSlot, activeBoat->id);
      }
    }
  }
}

/*
 * ship_scheduler_set_sensor_enabled / get_sensor_enabled
 *
 * scheduler->sensorActive (bool): habilita o deshabilita la lectura del sensor
 * de ultrasonido. Cuando esta desactivado, set_proximity_distance y
 * set_proximity_distance_simulated NO disparan emergencias aunque la distancia
 * este por debajo del umbral.
 *
 * Cuando se activa (enabled=true), el siguiente llamado a update_emergency
 * evaluara la distancia actual contra el umbral y disparara emergencia si
 * corresponde (sin necesidad de que llegue una nueva lectura).
 */
void ship_scheduler_set_sensor_enabled(ShipScheduler *scheduler, bool enabled) {
  if (!scheduler) return;
  scheduler->sensorActive = enabled;
  if (enabled) {
    ship_logln("[SENSOR] Sensor de proximidad activado");
  } else {
    ship_logln("[SENSOR] Sensor de proximidad desactivado");
  }
}

bool ship_scheduler_get_sensor_enabled(const ShipScheduler *scheduler) {
  if (!scheduler) return false;
  return scheduler->sensorActive;
}

void ship_scheduler_set_proximity_threshold(ShipScheduler *scheduler, uint16_t cm) {
  if (!scheduler) return;
  if (cm < 10) cm = 10;
  if (cm > 500) cm = 500;
  scheduler->proximityThresholdCm = cm;
  ship_logf("[SENSOR] Umbral de proximidad ajustado a %u cm\n", cm);
}

uint16_t ship_scheduler_get_proximity_threshold(const ShipScheduler *scheduler) {
  if (!scheduler) return 150;
  return scheduler->proximityThresholdCm;
}

/*
 * ship_scheduler_set_proximity_distance — distancia real del sensor (hardware).
 * ship_scheduler_set_proximity_distance_simulated — distancia simulada (software).
 *
 * scheduler->proximityCurrentDistanceCm (uint16_t): ultima lectura de distancia
 * del sensor HC-SR04 en centimetros. Cada llamada actualiza este campo y,
 * si el sensor esta activo (sensorActive==true) y la distancia <= umbral Y no
 * hay ya una emergencia activa (EMERGENCY_NONE), dispara trigger_emergency.
 *
 * DIFERENCIA ENTRE LAS DOS FUNCIONES:
 *   set_proximity_distance: lectura real del hardware.
 *     proximityDistanceIsSimulated = false.
 *   set_proximity_distance_simulated: valor inyectado por software (tests/demo).
 *     proximityDistanceIsSimulated = true.
 *     Cuando se limpia una emergencia simulada en clear_emergency, el campo se
 *     resetea a 120cm (valor seguro por encima del umbral tipico de 100cm) y
 *     proximityDistanceIsSimulated = false.
 *     Esto evita que en el proximo tick de update_emergency la distancia simulada
 *     vuelva a disparar otra emergencia inmediatamente.
 *
 * Rango valido: 10cm – 500cm (fijado en set_proximity_threshold).
 * Por debajo de 10cm: ruido del sensor (reflexion de onda ultrasonica).
 * Por encima de 500cm: fuera del rango util del HC-SR04.
 */
void ship_scheduler_set_proximity_distance(ShipScheduler *scheduler, uint16_t cm) {
  if (!scheduler) return;
  scheduler->proximityCurrentDistanceCm = cm;
  scheduler->proximityDistanceIsSimulated = false;
  if (scheduler->sensorActive && scheduler->emergencyMode == EMERGENCY_NONE && cm <= scheduler->proximityThresholdCm) {
    ship_logf("[SENSOR] ALERTA: Barco a %u cm (umbral: %u cm)\n", cm, scheduler->proximityThresholdCm);
    ship_scheduler_trigger_emergency(scheduler);
  }
}

void ship_scheduler_set_proximity_distance_simulated(ShipScheduler *scheduler, uint16_t cm) {
  if (!scheduler) return;
  scheduler->proximityCurrentDistanceCm = cm;
  scheduler->proximityDistanceIsSimulated = true;
  if (scheduler->sensorActive && scheduler->emergencyMode == EMERGENCY_NONE && cm <= scheduler->proximityThresholdCm) {
    ship_logf("[SENSOR] ALERTA: Barco a %u cm (umbral: %u cm)\n", cm, scheduler->proximityThresholdCm);
    ship_scheduler_trigger_emergency(scheduler);
  }
}

uint16_t ship_scheduler_get_proximity_distance(const ShipScheduler *scheduler) {
  if (!scheduler) return 999;
  return scheduler->proximityCurrentDistanceCm;
}

/*
 * ship_scheduler_get_emergency_mode
 *
 * Retorna el estado actual del ciclo de emergencia:
 *   EMERGENCY_NONE            : operacion normal.
 *   EMERGENCY_PROXIMITY_ALERT : sensor disparo; procesando (transitorio).
 *   EMERGENCY_GATES_CLOSED    : compuertas cerradas; barcos congelados.
 *   EMERGENCY_RECOVERY        : esperando que expire gateLockDurationMs.
 *
 * El enum avanza linealmente en trigger_emergency:
 *   NONE -> PROXIMITY_ALERT -> GATES_CLOSED -> RECOVERY
 * y regresa a NONE en clear_emergency.
 */
ShipEmergencyMode ship_scheduler_get_emergency_mode(const ShipScheduler *scheduler) {
  if (!scheduler) return EMERGENCY_NONE;
  return scheduler->emergencyMode;
}

/*
 * ship_scheduler_trigger_emergency — activa la emergencia de proximidad.
 *
 * SECUENCIA COMPLETA:
 *
 * 1. emergencyMode = EMERGENCY_PROXIMITY_ALERT
 *    emergencyStartedAt = millis()  <- timestamp para el temporizador de recovery.
 *
 * 2. Cerrar compuertas:
 *    gateLeftClosed  = 2   (2 = cerrado por emergencia; 0=abierto, 1=cerrando).
 *    gateRightClosed = 2
 *    emergencyMode   = EMERGENCY_GATES_CLOSED
 *
 * 3. Para cada barco en activeBoats[] (i < activeCount):
 *    a. emergencySavedSlot = activeBoat->currentSlot
 *       Guarda la posicion actual (int16_t en el struct Boat en heap) para
 *       poder restaurar al barco en la misma casilla al limpiar la emergencia.
 *    b. allowedToMove = false
 *       La bandera que boatTask verifica en cada iteracion; al bajarla, el
 *       barco deja de intentar moverse en el proximo ciclo de su loop.
 *    c. notify_task(taskHandle, NOTIF_CMD_PAUSE):
 *       Escribe NOTIF_CMD_PAUSE (valor 2) en ulNotifiedValue del TCB de FreeRTOS.
 *       boatTask lee esto en xTaskNotifyWait y entra al bloque 'NOTIF_CMD_PAUSE':
 *       running=false; espera el proximo NOTIF_CMD_RUN para continuar.
 *    d. ship_scheduler_release_range(scheduler, currentSlot, 1, activeBoat):
 *       Bajo mutex channelSlotsGuard:
 *         slotOwner[currentSlot] = 0      <- slot libre.
 *         xSemaphoreGive(gSlotSemaphores[currentSlot]) <- slot disponible.
 *       Broadcast NOTIF_CMD_SLOT_UPDATE a todas las tareas en espera.
 *       RAZON: Si no se libera la casilla, cuando la emergencia termina y los
 *       barcos se restauran via try_reserve_range, el slotOwner sigue siendo
 *       el id del barco -> try_reserve_range falla (el slot 'ya esta ocupado
 *       por mi mismo') y el barco no puede restaurarse.
 *    e. currentSlot = -1    <- el barco ya no ocupa casilla en el canal.
 *    f. emergencyParked = true  <- marcado para restore_parked_boats.
 *
 * 4. crossingStartedAt = 0  (sin cruce activo durante la emergencia).
 * 5. freeze_active_quantum  (congelar contador para evitar preempcion espuria).
 * 6. emergencyMode = EMERGENCY_RECOVERY
 *    A partir de aqui, update_emergency cuenta el tiempo hasta gateLockDurationMs.
 */
void ship_scheduler_trigger_emergency(ShipScheduler *scheduler) {
  if (!scheduler) return;

  scheduler->emergencyMode = EMERGENCY_PROXIMITY_ALERT;
  scheduler->emergencyStartedAt = millis();

  ship_logln("[EMERGENCY] ¡¡¡ ALERTA DE PROXIMIDAD !!!");
  ship_logln("[EMERGENCY] Cerrando compuertas...");

  scheduler->gateLeftClosed = 2;
  scheduler->gateRightClosed = 2;
  scheduler->emergencyMode = EMERGENCY_GATES_CLOSED;

  ship_logln("[EMERGENCY] Compuertas CERRADAS");

  if (scheduler->activeCount > 0) {
    for (uint8_t i = 0; i < scheduler->activeCount; i++) {
      Boat *activeBoat = scheduler->activeBoats[i];
      if (!activeBoat) continue;
      if (activeBoat->emergencyParked) continue;
      activeBoat->emergencySavedSlot = activeBoat->currentSlot;
      activeBoat->allowedToMove = false;
      if (activeBoat->taskHandle) {
        notify_task(activeBoat->taskHandle, NOTIF_CMD_PAUSE);
      }
      ship_logf("[EMERGENCY] Barco #%u congelado en el canal\n", activeBoat->id);
      if (activeBoat->currentSlot >= 0) {
        ship_scheduler_release_range(scheduler, activeBoat->currentSlot, 1, activeBoat);
        activeBoat->currentSlot = -1;
      }
      activeBoat->emergencyParked = true;
      activeBoat->state = STATE_CROSSING;
    }
    scheduler->crossingStartedAt = 0;
    ship_scheduler_freeze_active_quantum(scheduler);
  }

  scheduler->emergencyMode = EMERGENCY_RECOVERY;
  ship_logln("[EMERGENCY] Modo: RECOVERY (esperando apertura de compuertas)");
}

/*
 * ship_scheduler_clear_emergency — finaliza la emergencia y reanuda operacion.
 *
 * SECUENCIA DE RESTAURACION:
 *
 * 1. Abrir compuertas:
 *    gateLeftClosed = 0; gateRightClosed = 0.
 *
 * 2. restore_parked_boats(scheduler):
 *    Para cada barco con emergencyParked==true:
 *    - try_reserve_range(savedSlot): intenta re-adquirir la casilla original.
 *    - Si tiene exito: currentSlot = savedSlot (el barco regresa al canal).
 *    - emergencyParked = false; emergencySavedSlot = -1.
 *
 * 3. Reanudar tareas segun algoritmo:
 *
 *    Rama RR:
 *    - activeBoat = activeBoats[0]   <- alias del primario.
 *    - hasActiveBoat = (activeBoat != NULL).
 *    - Para i=0: allowedToMove=true; para i>0: allowedToMove=false
 *      (RR: solo el primario mueve).
 *    - sync_rr_permissions_local: asegura coherencia (comparacion de punteros).
 *    - notify_task(activeBoat->taskHandle, NOTIF_CMD_RUN): desbloquea la tarea
 *      del primario en xTaskNotifyWait(portMAX_DELAY).
 *    - resume_active_quantum: activeQuantumStartedAt = millis() (nuevo quantum).
 *
 *    Rama no-RR:
 *    - Todos los activos reciben allowedToMove=true + NOTIF_CMD_RUN.
 *    - activeQuantumStartedAt = millis() (reinicio del contador de tiempo).
 *
 * 4. Limpieza de distancia simulada:
 *    Si proximityDistanceIsSimulated==true: resetear a 120cm para evitar que
 *    update_emergency dispare otra emergencia en el proximo tick.
 *
 * 5. emergencyMode = EMERGENCY_NONE; emergencyStartedAt = 0;
 *    emergencyDispatchBlockedLogged = false (permite volver a loggear el bloqueo
 *    si hay otra emergencia futura).
 */
void ship_scheduler_clear_emergency(ShipScheduler *scheduler) {
  if (!scheduler) return;

  if (scheduler->emergencyMode != EMERGENCY_NONE) {
    ship_logln("[EMERGENCY] Limpiando estado de emergencia...");
    scheduler->gateLeftClosed = 0;
    scheduler->gateRightClosed = 0;
    ship_logln("[EMERGENCY] Compuertas ABIERTAS");

    ship_scheduler_restore_parked_boats(scheduler);

    if (scheduler->algorithm == ALG_RR) {
      if (scheduler->activeCount > 0) {
        scheduler->activeBoat = scheduler->activeBoats[0];
        scheduler->hasActiveBoat = (scheduler->activeBoat != NULL);
        for (uint8_t i = 0; i < scheduler->activeCount; i++) {
          Boat *active = scheduler->activeBoats[i];
          if (!active) continue;
          active->allowedToMove = (i == 0);
        }
      }
      ship_scheduler_sync_rr_permissions_local(scheduler);
      if (scheduler->activeBoat && scheduler->activeBoat->taskHandle) {
        notify_task(scheduler->activeBoat->taskHandle, NOTIF_CMD_RUN);
        ship_scheduler_resume_active_quantum(scheduler);
      }
    } else {
      for (uint8_t i = 0; i < scheduler->activeCount; i++) {
        Boat *activeBoat = scheduler->activeBoats[i];
        if (!activeBoat || !activeBoat->taskHandle) continue;
        activeBoat->allowedToMove = true;
        notify_task(activeBoat->taskHandle, NOTIF_CMD_RUN);
      }
      scheduler->activeQuantumStartedAt = millis();
    }

    if (scheduler->proximityDistanceIsSimulated) {
      scheduler->proximityCurrentDistanceCm = 120;
      scheduler->proximityDistanceIsSimulated = false;
      ship_logln("[SENSOR] distancia: 120 cm");
    }
  }

  scheduler->emergencyMode = EMERGENCY_NONE;
  scheduler->emergencyStartedAt = 0;
  scheduler->emergencyDispatchBlockedLogged = false;
  ship_logln("[EMERGENCY] Estado: NORMAL");
}

/*
 * ship_scheduler_update_emergency — tick periodico de la maquina de estados de emergencia.
 *
 * Llamada desde ship_scheduler_update() cada ~50ms.
 * Implementa dos responsabilidades:
 *
 * 1. DETECCION REACTIVA:
 *    Si sensorActive==true Y emergencyMode==EMERGENCY_NONE Y
 *    proximityCurrentDistanceCm <= proximityThresholdCm:
 *    La distancia actual (actualizada por set_proximity_distance o
 *    set_proximity_distance_simulated) sigue siendo peligrosa.
 *    -> trigger_emergency(scheduler)
 *    Esto cubre el caso en que el sensor fue activado (sensorActive=false->true)
 *    cuando ya habia una distancia peligrosa almacenada, sin que llegara una
 *    nueva lectura.
 *
 * 2. TEMPORIZADOR DE RECOVERY:
 *    Si emergencyMode == EMERGENCY_RECOVERY:
 *      elapsedMs = millis() - emergencyStartedAt
 *      Si elapsedMs >= gateLockDurationMs -> clear_emergency(scheduler)
 *    gateLockDurationMs (unsigned long en el struct): duracion en ms que las
 *    compuertas permanecen cerradas antes de reanudar el trafico.
 *    emergencyStartedAt fue fijado en trigger_emergency al inicio de la emergencia.
 */
void ship_scheduler_update_emergency(ShipScheduler *scheduler) {
  if (!scheduler) return;

  if (scheduler->sensorActive && scheduler->emergencyMode == EMERGENCY_NONE) {
    if (scheduler->proximityCurrentDistanceCm <= scheduler->proximityThresholdCm) {
      ship_scheduler_trigger_emergency(scheduler);
    }
  }

  if (scheduler->emergencyMode == EMERGENCY_RECOVERY) {
    unsigned long elapsedMs = millis() - scheduler->emergencyStartedAt;
    if (elapsedMs >= scheduler->gateLockDurationMs) {
      ship_scheduler_clear_emergency(scheduler);
    }
  }
}

/*
 * ship_scheduler_get_gate_left_state / get_gate_right_state
 *
 * Retorna el estado de la compuerta izquierda / derecha del canal:
 *   0 = ABIERTA  (operacion normal; barcos pueden entrar y salir).
 *   1 = CERRANDO (transitorio; animacion de cierre en display).
 *   2 = CERRADA  (emergencia activa; barcos bloqueados).
 *
 * Estos valores son escritos en trigger_emergency (0->2) y
 * limpiados en clear_emergency (2->0).
 * El display los usa para mostrar el estado de las compuertas en tiempo real.
 */
uint8_t ship_scheduler_get_gate_left_state(const ShipScheduler *scheduler) {
  if (!scheduler) return 0;
  return scheduler->gateLeftClosed;
}

uint8_t ship_scheduler_get_gate_right_state(const ShipScheduler *scheduler) {
  if (!scheduler) return 0;
  return scheduler->gateRightClosed;
}
