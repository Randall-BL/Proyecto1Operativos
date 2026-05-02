// Implementacion del modelo de barcos en C puro. // Comentario de encabezado del archivo. 
#include "ShipModel.h" // Incluye la definicion de Boat y helpers. 

// Colores RGB565 usados por la interfaz sin depender de la libreria TFT. // Nota sobre los colores locales. 
#define SHIP_COLOR_WHITE 0xFFFF // Blanco RGB565. 
#define SHIP_COLOR_CYAN  0x07FF // Cian RGB565. 
#define SHIP_COLOR_RED   0xF800 // Rojo RGB565. 
#define SHIP_COLOR_YELLOW 0xFFE0 // Amarillo RGB565. 

// Secuencias globales para generar identificadores y orden de llegada. // Contadores globales. 
static uint8_t nextBoatId = 1; // Siguiente ID de barco. 
static unsigned long nextArrivalOrder = 1; // Siguiente orden de llegada. 

void resetBoatSequence(void) { // Reinicia los contadores globales. 
  // Reinicia las secuencias para que las pruebas sean reproducibles. // Comentario de intencion. 
  nextBoatId = 1; // Reinicia el ID. 
  nextArrivalOrder = 1; // Reinicia el orden de llegada. 
} // Fin de resetBoatSequence. 

const char *boatTypeName(BoatType type) { // Devuelve el nombre largo del tipo. 
  // Devuelve un nombre legible para mostrar en pantalla o por Serial. // Comentario funcional. 
  switch (type) { // Selecciona segun el tipo. 
    case BOAT_NORMAL: return "Normal"; // Nombre para normal. 
    case BOAT_PESQUERA: return "Pesquera"; // Nombre para pesquera. 
    case BOAT_PATRULLA: return "Patrulla"; // Nombre para patrulla. 
  } // Fin del switch. 
  return "Desconocido"; // Fallback por defecto. 
} // Fin de boatTypeName. 

const char *boatSideName(BoatSide side) { // Devuelve etiqueta del lado. 
  // Convierte el lado de entrada a una etiqueta compacta. // Comentario funcional. 
  return side == SIDE_LEFT ? "Izq" : "Der"; // Retorna segun el lado. 
} // Fin de boatSideName. 

const char *boatTypeShort(BoatType type) { // Devuelve etiqueta corta del tipo. 
  // Reduce el tipo a una sola letra para dibujarlo dentro del cuadrado. // Comentario funcional. 
  switch (type) { // Selecciona segun el tipo. 
    case BOAT_NORMAL: return "N"; // Letra para normal. 
    case BOAT_PESQUERA: return "P"; // Letra para pesquera. 
    case BOAT_PATRULLA: return "U"; // Letra para patrulla. 
  } // Fin del switch. 
  return "?"; // Fallback por defecto. 
} // Fin de boatTypeShort. 

uint16_t boatColor(BoatType type) { // Devuelve color RGB565 por tipo. 
  // Asigna un color RGB565 estable a cada tipo de barco. // Comentario funcional. 
  switch (type) { // Selecciona segun el tipo. 
    case BOAT_NORMAL: return SHIP_COLOR_WHITE; // Color para normal. 
    case BOAT_PESQUERA: return SHIP_COLOR_CYAN; // Color para pesquera. 
    case BOAT_PATRULLA: return SHIP_COLOR_RED; // Color para patrulla. 
  } // Fin del switch. 
  return SHIP_COLOR_YELLOW; // Color por defecto. 
} // Fin de boatColor. 

unsigned long serviceTimeForType(BoatType type) { // Devuelve el tiempo base por tipo. 
  // Define el tiempo base de servicio segun el tipo de barco. // Comentario funcional. 
  switch (type) { // Selecciona segun el tipo. 
    case BOAT_NORMAL: return 8500; // Tiempo para normal. 
    case BOAT_PESQUERA: return 6500; // Tiempo para pesquera. 
    case BOAT_PATRULLA: return 4000; // Tiempo para patrulla. 
  } // Fin del switch. 
  return 5000; // Tiempo por defecto. 
} // Fin de serviceTimeForType. 

uint8_t defaultPriorityForType(BoatType type) { // Devuelve prioridad base por tipo. 
  // Asigna una prioridad inicial coherente con el tipo. // Comentario funcional. 
  switch (type) { // Selecciona segun el tipo. 
    case BOAT_NORMAL: return 1; // Prioridad para normal. 
    case BOAT_PESQUERA: return 2; // Prioridad para pesquera. 
    case BOAT_PATRULLA: return 3; // Prioridad para patrulla. 
  } // Fin del switch. 
  return 1; // Prioridad por defecto. 
} // Fin de defaultPriorityForType. 

Boat *createBoat(BoatSide origin, BoatType type) { // Crea un barco con prioridad base. 
  // Crea un barco con la prioridad por defecto derivada del tipo. // Comentario funcional. 
  return createBoatWithPriority(origin, type, defaultPriorityForType(type)); // Delegacion a la funcion principal. 
} // Fin de createBoat. 

Boat *createBoatWithPriority(BoatSide origin, BoatType type, uint8_t priority) { // Crea un barco con prioridad explicitada. 
  // Reserva memoria para un barco individual. // Comentario de memoria. 
  Boat *boat = (Boat *)malloc(sizeof(Boat)); // Reserva memoria dinamica. 
  if (!boat) { // Si falla la reserva. 
    return NULL; // Retorna nulo. 
  } // Fin del if. 

  // Inicializa el identificador y la informacion de planificacion. // Comentario de init. 
  boat->id = nextBoatId++; // Asigna el ID. 
  boat->type = type; // Asigna el tipo. 
  boat->origin = origin; // Asigna el origen. 
  boat->priority = priority; // Asigna la prioridad. 
  boat->arrivalOrder = nextArrivalOrder++; // Asigna el orden de llegada. 
  boat->serviceMillis = serviceTimeForType(type); // Asigna el tiempo de servicio. 
  boat->startedAt = 0; // Inicializa el tiempo de inicio. 
  boat->enqueuedAt = 0; // Inicializa el tiempo de encolado. 
  boat->state = STATE_WAITING; // Estado inicial en espera. 
  boat->taskHandle = NULL; // Aun no hay task. 
  boat->allowedToMove = false; // Bandera en falso al iniciar. 
  boat->remainingMillis = boat->serviceMillis; // Tiempo restante inicial. 
  // El deadline inicial es una heuristica simple para EDF. // Comentario sobre EDF. 
  boat->deadlineMillis = millis() + (boat->serviceMillis * 2UL); // Deadline basado en servicio. 
  boat->cancelled = false; // Marca de cancelacion en falso. 
  return boat; // Retorna el barco creado. 
} // Fin de createBoatWithPriority. 

void destroyBoat(Boat *b) { // Libera un barco dinamico. 
  // Libera la memoria reservada para un barco dinamico. // Comentario funcional. 
  if (!b) return; // Si es nulo, no hace nada. 
  free(b); // Libera memoria. 
} // Fin de destroyBoat. 
