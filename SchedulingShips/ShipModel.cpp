#include <Adafruit_ST7735.h>

#include "ShipModel.h"

// Contadores monotonicos para asignar IDs y orden de llegada.
static uint8_t nextBoatId = 1;
static unsigned long nextArrivalOrder = 1;

void resetBoatSequence() {
  // Reinicia contadores para pruebas deterministas.
  nextBoatId = 1;
  nextArrivalOrder = 1;
}

const char *boatTypeName(BoatType type) {
  // Nombre completo para interfaz y logs.
  switch (type) {
    case BOAT_NORMAL: return "Normal";
    case BOAT_PESQUERA: return "Pesquera";
    case BOAT_PATRULLA: return "Patrulla";
  }
  return "Desconocido";
}

const char *boatSideName(BoatSide side) {
  // Etiqueta corta para interfaz.
  return side == SIDE_LEFT ? "Izq" : "Der";
}

const char *boatTypeShort(BoatType type) {
  // Etiqueta de una letra para interfaz compacta.
  switch (type) {
    case BOAT_NORMAL: return "N";
    case BOAT_PESQUERA: return "P";
    case BOAT_PATRULLA: return "U";
  }
  return "?";
}

uint16_t boatColor(BoatType type) {
  // Asigna un color por tipo para la interfaz.
  switch (type) {
    case BOAT_NORMAL: return ST77XX_WHITE;
    case BOAT_PESQUERA: return ST77XX_CYAN;
    case BOAT_PATRULLA: return ST77XX_RED;
  }
  return ST77XX_YELLOW;
}

unsigned long serviceTimeForType(BoatType type) {
  // Duracion base de cruce segun tipo.
  switch (type) {
    case BOAT_NORMAL: return 6500;
    case BOAT_PESQUERA: return 4500;
    case BOAT_PATRULLA: return 3000;
  }
  return 5000;
}

uint8_t defaultPriorityForType(BoatType type) {
  // Prioridad por defecto derivada del tipo.
  switch (type) {
    case BOAT_NORMAL: return 1;
    case BOAT_PESQUERA: return 2;
    case BOAT_PATRULLA: return 3;
  }
  return 1;
}

Boat *createBoat(BoatSide origin, BoatType type) {
  // Reserva memoria y crea un barco con valores por defecto.
  Boat *boat = new Boat();
  boat->id = nextBoatId++;
  boat->type = type;
  boat->origin = origin;
  boat->priority = defaultPriorityForType(type);
  boat->arrivalOrder = nextArrivalOrder++;
  boat->serviceMillis = serviceTimeForType(type);
  boat->startedAt = 0;
  boat->enqueuedAt = 0;
  boat->state = STATE_WAITING;
  boat->taskHandle = NULL;
  boat->remainingMillis = boat->serviceMillis;
  // Deadline por defecto: ahora + 2 * tiempo de servicio (se puede sobrescribir).
  boat->deadlineMillis = millis() + (boat->serviceMillis * 2UL);
  boat->cancelled = false;
  return boat;
}

Boat *createBoatWithPriority(BoatSide origin, BoatType type, uint8_t priority) {
  // Helper para sobrescribir la prioridad por defecto.
  Boat *boat = createBoat(origin, type);
  if (boat) boat->priority = priority;
  return boat;
}

void destroyBoat(Boat *b) {
  // Libera un barco de forma segura.
  if (!b) return;
  delete b;
}