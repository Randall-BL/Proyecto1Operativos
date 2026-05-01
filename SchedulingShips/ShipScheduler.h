#pragma once // Evita inclusiones duplicadas. 

#ifdef __cplusplus // Habilita linkage C para C++. 
extern "C" { // Inicio del bloque con nombres C. 
#endif // Fin de compatibilidad C++. 

#include "ShipModel.h" // Tipos base de barcos. 

typedef enum ShipAlgo { // Enumeracion de algoritmos. 
  ALG_FCFS = 0, // First Come First Served. 
  ALG_STRN = 1, // Shortest Remaining Time Next. 
  ALG_EDF = 2, // Earliest Deadline First. 
  ALG_RR = 3, // Round Robin. 
  ALG_PRIORITY = 4 // Prioridad estatica. 
} ShipAlgo; // Alias del enum de algoritmos. 

typedef struct ShipScheduler { // Estructura con el estado del scheduler. 
  Boat *readyQueue[MAX_BOATS]; // Cola de listos (punteros). 
  uint8_t readyCount; // Cantidad de barcos listos. 
  Boat *activeBoat; // Barco activo actual. 
  bool hasActiveBoat; // Flag de barco activo. 
  unsigned long crossingStartedAt; // Marca de inicio del cruce. 
  uint16_t completedLeftToRight; // Completados izq a der. 
  uint16_t completedRightToLeft; // Completados der a izq. 
  uint16_t completedTotal; // Total completados. 
  unsigned long totalWaitMillis; // Acumulado de espera. 
  unsigned long totalTurnaroundMillis; // Acumulado de turnaround. 
  unsigned long totalServiceMillis; // Acumulado de servicio. 
  uint8_t completionOrder[MAX_BOATS]; // Orden de finalizacion. 
  uint8_t completionCount; // Cantidad en orden de finalizacion. 
  bool ignoreCompletions; // Ignora callbacks tardios. 
  ShipAlgo algorithm; // Algoritmo activo. 
  unsigned long rrQuantumMillis; // Quantum de RR. 
} ShipScheduler; // Alias del tipo scheduler. 

void ship_scheduler_begin(ShipScheduler *scheduler); // Inicializa el scheduler. 
void ship_scheduler_load_demo_manifest(ShipScheduler *scheduler); // Carga manifiesto demo. 
void ship_scheduler_clear(ShipScheduler *scheduler); // Limpia colas y tareas. 
void ship_scheduler_enqueue(ShipScheduler *scheduler, Boat *boat); // Encola un barco. 
void ship_scheduler_update(ShipScheduler *scheduler); // Ejecuta un paso de planificacion. 
void ship_scheduler_set_algorithm(ShipScheduler *scheduler, ShipAlgo algo); // Cambia algoritmo. 
ShipAlgo ship_scheduler_get_algorithm(const ShipScheduler *scheduler); // Obtiene algoritmo actual. 
const char *ship_scheduler_get_algorithm_label(const ShipScheduler *scheduler); // Texto corto del algoritmo. 
void ship_scheduler_set_round_robin_quantum(ShipScheduler *scheduler, unsigned long quantumMillis); // Ajusta quantum. 
unsigned long ship_scheduler_get_round_robin_quantum(const ShipScheduler *scheduler); // Lee quantum. 

const Boat *ship_scheduler_get_active_boat(const ShipScheduler *scheduler); // Devuelve barco activo. 
uint8_t ship_scheduler_get_ready_count(const ShipScheduler *scheduler); // Devuelve listos. 
const Boat *ship_scheduler_get_ready_boat(const ShipScheduler *scheduler, uint8_t index); // Devuelve barco en cola. 
uint8_t ship_scheduler_get_waiting_count(const ShipScheduler *scheduler, BoatSide side); // Devuelve cantidad por lado. 
const Boat *ship_scheduler_get_waiting_boat(const ShipScheduler *scheduler, BoatSide side, uint8_t index); // Devuelve barco por lado. 
uint16_t ship_scheduler_get_completed_left_to_right(const ShipScheduler *scheduler); // Devuelve completados izq-der. 
uint16_t ship_scheduler_get_completed_right_to_left(const ShipScheduler *scheduler); // Devuelve completados der-izq. 
unsigned long ship_scheduler_get_active_elapsed_millis(const ShipScheduler *scheduler); // Devuelve tiempo activo. 
uint16_t ship_scheduler_get_completed_total(const ShipScheduler *scheduler); // Devuelve completados totales. 
unsigned long ship_scheduler_get_total_wait_millis(const ShipScheduler *scheduler); // Devuelve espera acumulada. 
unsigned long ship_scheduler_get_total_turnaround_millis(const ShipScheduler *scheduler); // Devuelve turnaround acumulado. 
unsigned long ship_scheduler_get_total_service_millis(const ShipScheduler *scheduler); // Devuelve servicio acumulado. 
uint8_t ship_scheduler_get_completion_count(const ShipScheduler *scheduler); // Devuelve cantidad en orden final. 
uint8_t ship_scheduler_get_completion_id(const ShipScheduler *scheduler, uint8_t index); // Devuelve id en orden final. 

void ship_scheduler_notify_boat_finished(ShipScheduler *scheduler, Boat *b); // Callback de finalizacion. 
void ship_scheduler_pause_active(ShipScheduler *scheduler); // Pausa el barco activo. 
void ship_scheduler_resume_active(ShipScheduler *scheduler); // Reanuda el barco activo. 
void ship_scheduler_dump_status(const ShipScheduler *scheduler); // Imprime el estado por Serial. 

extern ShipScheduler *gScheduler; // Puntero global para callbacks de tareas. 

#define NOTIF_CMD_RUN 1UL // Comando RUN por notificacion. 
#define NOTIF_CMD_PAUSE 2UL // Comando PAUSE por notificacion. 
#define NOTIF_CMD_TERMINATE 3UL // Comando TERMINATE por notificacion. 

#ifdef __cplusplus // Cierra el bloque de linkage C. 
} // Fin de extern "C". 
#endif // Fin de compatibilidad C++. 