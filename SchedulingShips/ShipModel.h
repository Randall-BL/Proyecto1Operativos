#pragma once

#include <Arduino.h>

// Tipos de barco usados para derivar velocidad y prioridad.
enum BoatType : uint8_t {
  BOAT_NORMAL,
  BOAT_PESQUERA,
  BOAT_PATRULLA
};

// Lado de origen del canal.
enum BoatSide : uint8_t {
  SIDE_LEFT,
  SIDE_RIGHT
};

// Estados del ciclo de vida del barco.
enum BoatState : uint8_t {
  STATE_WAITING,
  STATE_CROSSING,
  STATE_DONE
};

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Datos de ejecucion para cada tarea de barco.
struct Boat {
  uint8_t id;                 // Identificador unico del barco.
  BoatType type;              // Tipo de barco (define velocidad/servicio).
  BoatSide origin;            // Lado de origen del canal.
  uint8_t priority;           // Prioridad para algoritmo de prioridad.
  unsigned long arrivalOrder; // Orden de llegada para desempate FCFS.
  unsigned long serviceMillis; // Tiempo total de servicio/cruce.
  unsigned long startedAt;    // Momento en que inicio a cruzar.
  unsigned long enqueuedAt;   // Momento en que entro a la cola.
  BoatState state;            // Estado actual del barco.

  // Integracion con FreeRTOS.
  TaskHandle_t taskHandle;     // Handle de la tarea que ejecuta al barco.
  volatile bool allowedToMove; // Legado; ahora se usan notificaciones.
  unsigned long remainingMillis; // Tiempo restante de servicio.
  unsigned long deadlineMillis;  // Deadline absoluta en millis.
  bool cancelled;              // Marca de cancelacion para cleanup.
};

// Limites globales y ritmo de refresco de la interfaz.
constexpr uint8_t MAX_BOATS = 16;
constexpr uint8_t VISIBLE_QUEUE = 6;
constexpr unsigned long UI_REFRESH_MS = 200;
constexpr unsigned long CROSSING_MARGIN_MS = 250;

// Helpers de presentacion y constructores de barcos.
const char *boatTypeName(BoatType type);
const char *boatSideName(BoatSide side);
const char *boatTypeShort(BoatType type);
uint16_t boatColor(BoatType type);
unsigned long serviceTimeForType(BoatType type);
uint8_t defaultPriorityForType(BoatType type);
void resetBoatSequence();
Boat *createBoat(BoatSide origin, BoatType type);
Boat *createBoatWithPriority(BoatSide origin, BoatType type, uint8_t priority);
void destroyBoat(Boat *b);