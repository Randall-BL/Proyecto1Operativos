#include "ShipScheduler.h" // API del scheduler.

#include "ShipIO.h" // Logging por Serial.
#include <freertos/task.h> // xTaskNotify.

#define FLOW_LOG(schedulerPtr, fmt, ...) do { if ((schedulerPtr) && (schedulerPtr)->flowLoggingEnabled) ship_logf(fmt, ##__VA_ARGS__); } while (0)

static void notify_task(TaskHandle_t taskHandle, uint32_t notificationValue) {
  if (!taskHandle) return;
  xTaskNotify(taskHandle, notificationValue, eSetValueWithOverwrite);
}

static void ship_scheduler_sync_rr_permissions_local(ShipScheduler *scheduler) {
  if (!scheduler || scheduler->algorithm != ALG_RR) return;
  for (uint8_t i = 0; i < scheduler->activeCount; i++) {
    Boat *active = scheduler->activeBoats[i];
    if (!active) continue;
    active->allowedToMove = (active == scheduler->activeBoat);
  }
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

ShipEmergencyMode ship_scheduler_get_emergency_mode(const ShipScheduler *scheduler) {
  if (!scheduler) return EMERGENCY_NONE;
  return scheduler->emergencyMode;
}

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

uint8_t ship_scheduler_get_gate_left_state(const ShipScheduler *scheduler) {
  if (!scheduler) return 0;
  return scheduler->gateLeftClosed;
}

uint8_t ship_scheduler_get_gate_right_state(const ShipScheduler *scheduler) {
  if (!scheduler) return 0;
  return scheduler->gateRightClosed;
}
