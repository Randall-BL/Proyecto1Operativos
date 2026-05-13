#pragma once // Evita inclusiones duplicadas del header. 

#ifdef __cplusplus // Habilita linkage C para C++. 
extern "C" { // Inicio del bloque con nombres C. 
#endif // Fin de la directiva de compatibilidad C++. 

// Cabecera base para usar el modelo tanto desde C como desde C++. 
#include <stdint.h> // Tipos enteros de ancho fijo. 
// Necesario para el tipo bool en C puro. 
#include <stdbool.h> // Booleanos en C. 
// Tipos y manejo de tareas de FreeRTOS. 
#include <freertos/FreeRTOS.h> // Tipos base FreeRTOS. 
// Declaracion del handle de tarea. 
#include <freertos/task.h> // TaskHandle_t y API de tareas. 

// Declaracion explicita de millis para no depender de Arduino.h en C. 
extern unsigned long millis(void); // Devuelve el tiempo en ms desde el arranque. 
// Declaracion explicita de delay para no depender de Arduino.h en C. 
extern void delay(unsigned long ms); // Espera activa por ms. 

// Tipo de barco; cada valor define un perfil distinto de servicio. 
typedef enum BoatType { // Enumeracion de tipos de barco. 
  BOAT_NORMAL, // Barco normal con tiempo medio. 
  BOAT_PESQUERA, // Barco pesquero con tiempo menor. 
  BOAT_PATRULLA // Barco patrulla con tiempo mas corto. 
} BoatType; // Alias del enum de tipos. 

// Lado del canal desde donde entra el barco. 
typedef enum BoatSide { // Enumeracion del lado de entrada. 
  SIDE_LEFT, // Lado izquierdo del canal. 
  SIDE_RIGHT // Lado derecho del canal. 
} BoatSide; // Alias del enum de lados. 

// Estado de vida del barco dentro del scheduler. 
typedef enum BoatState { // Enumeracion de estados del barco. 
  STATE_WAITING, // En espera en cola. 
  STATE_CROSSING, // Cruzando el canal. 
  STATE_DONE // Cruzado y finalizado. 
} BoatState; // Alias del enum de estados. 

// Estructura principal que representa un barco y su estado de ejecucion. 
typedef struct Boat { // Definicion de la estructura Boat. 
  uint8_t id; // Identificador unico del barco. 
  BoatType type; // Tipo funcional del barco. 
  BoatSide origin; // Lado de origen en el canal. 
  uint8_t priority; // Prioridad usada por el algoritmo de prioridad. 
  unsigned long arrivalOrder; // Orden de llegada para desempates FCFS. 
  unsigned long serviceMillis; // Tiempo total que requiere para cruzar. 
  unsigned long startedAt; // Marca temporal de inicio de cruce. 
  unsigned long enqueuedAt; // Marca temporal de entrada a cola. 
  BoatState state; // Estado actual del barco. 
  TaskHandle_t taskHandle; // Handle de la tarea FreeRTOS asociada. 
  volatile bool allowedToMove; // Bandera de compatibilidad para pausa/reanudacion. 
  unsigned long remainingMillis; // Tiempo restante estimado. 
  unsigned long deadlineMillis; // Deadline absoluto para EDF. 
  bool cancelled; // Marca de cancelacion para limpieza segura. 
  uint8_t stepSize; // Cantidad de casillas de la lista que avanza por movimiento.
  int16_t currentSlot; // Posicion actual en la lista (-1 si fuera del canal)
} Boat; // Alias del tipo Boat. 

// Cantidad maxima de barcos permitidos en memoria. 
#define MAX_BOATS 40 // Limite superior de barcos simultaneos y del manifiesto demo. 
// Cantidad de barcos visibles por lado en la pantalla. 
#define VISIBLE_QUEUE 6 // Maximo visible por lado. 
// Periodo minimo de refresco visual. 
#define UI_REFRESH_MS 200UL // Periodo de refresco en ms. 
// Margen adicional para simular el cruce completo. 
#define CROSSING_MARGIN_MS 250UL // Margen extra en ms.
//Margen para evitar solapamientos visuales en TICO.
#define TICO_INITIAL_MARGIN 2000UL // Margen inicial para TICO
//Margen para evitar choques visuales en TICO.
#define TICO_SAFETY_MARGIN 0.18f // Margen de seguridad para TICO


// Helpers de texto y construccion de barcos. 
const char *boatTypeName(BoatType type); // Nombre largo del tipo. 
const char *boatSideName(BoatSide side); // Nombre corto del lado. 
const char *boatTypeShort(BoatType type); // Etiqueta de un caracter. 
uint16_t boatColor(BoatType type); // Color RGB565 asociado. 
uint8_t defaultPriorityForType(BoatType type); // Prioridad base por tipo. 
void ship_model_set_step_size(BoatType type, uint8_t stepSize); // Ajusta stepSize por tipo.
uint8_t ship_model_get_step_size(BoatType type); // Lee stepSize por tipo.
void resetBoatSequence(void); // Reinicia los contadores. 
Boat *createBoat(BoatSide origin, BoatType type); // Crea un barco con prioridad base. 
Boat *createBoatWithPriority(BoatSide origin, BoatType type, uint8_t priority); // Crea un barco con prioridad explicita. 
void destroyBoat(Boat *b); // Libera memoria de un barco. 

#ifdef __cplusplus // Cierra el bloque de linkage C. 
} // Fin de extern "C". 
#endif // Fin de compatibilidad C++. 