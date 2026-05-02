#pragma once // Evita inclusiones duplicadas. 

#ifdef __cplusplus // Habilita linkage C para C++. 
extern "C" { // Inicio del bloque con nombres C. 
#endif // Fin de compatibilidad C++. 

#include "ShipModel.h" // Tipos base de barcos. 

typedef enum ShipAlgo { // Enumeracion de algoritmos. 
  ALG_FCFS = 0, // First Come First Served. 
  ALG_SJF = 1, // Shortest Job First. 
  ALG_STRN = 2, // Shortest Remaining Time Next. 
  ALG_EDF = 3, // Earliest Deadline First. 
  ALG_RR = 4, // Round Robin. 
  ALG_PRIORITY = 5 // Prioridad estatica. 
} ShipAlgo; // Alias del enum de algoritmos. 

typedef enum ShipFlowMode { // Modo de control de flujo del canal.
  FLOW_TICO = 0, // Sin control de turno por lado.
  FLOW_FAIRNESS = 1, // Ventana W alternando lados.
  FLOW_SIGN = 2 // Letrero por tiempo.
} ShipFlowMode; // Alias del enum de flujo.

typedef enum ShipEmergencyMode { // Estados de emergencia del canal.
  EMERGENCY_NONE = 0, // Sin emergencia.
  EMERGENCY_PROXIMITY_ALERT = 1, // Alerta de proximidad activada.
  EMERGENCY_GATES_CLOSED = 2, // Compuertas cerradas.
  EMERGENCY_RECOVERY = 3 // En recuperacion de emergencia.
} ShipEmergencyMode; // Alias del enum de emergencia.

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
  ShipFlowMode flowMode; // Metodo de control de flujo.
  uint8_t fairnessWindowW; // Parametro W para equidad.
  BoatSide fairnessCurrentSide; // Lado actual de la ventana W.
  uint8_t fairnessPassedInWindow; // Cuantos barcos pasaron en la ventana.
  BoatSide signDirection; // Direccion actual del letrero.
  unsigned long signIntervalMillis; // Periodo de cambio del letrero.
  unsigned long signLastSwitchAt; // Ultimo cambio de letrero.
  uint8_t maxReadyQueueConfigured; // Limite de cola configurable.
  uint16_t channelLengthMeters; // Largo del canal configurado.
  uint16_t boatSpeedMetersPerSec; // Velocidad base del barco.
  uint16_t collisionDetections; // Contador de colisiones detectadas.
  bool flowLoggingEnabled; // Habilita trazas de decisiones de flujo.
  // Sensor de proximidad e interrupciones
  bool sensorActive; // Sensor habilitado/deshabilitado.
  uint16_t proximityThresholdCm; // Distancia umbral de alerta en cm.
  uint16_t proximityCurrentDistanceCm; // Distancia actual medida/simulada.
  bool proximityDistanceIsSimulated; // Indica si la distancia viene de sensor simulate.
  ShipEmergencyMode emergencyMode; // Estado actual de emergencia.
  unsigned long emergencyStartedAt; // Marca de inicio de la emergencia.
  uint8_t gateLeftClosed; // Estado puerta izquierda (0=abierta, 1=cerrando, 2=cerrada).
  uint8_t gateRightClosed; // Estado puerta derecha (0=abierta, 1=cerrando, 2=cerrada).
  uint16_t gateLockDurationMs; // Duracion del cierre de puertas en ms.
  bool emergencyDispatchBlockedLogged; // Evita logs repetidos de despacho bloqueado.
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
void ship_scheduler_set_flow_mode(ShipScheduler *scheduler, ShipFlowMode mode); // Ajusta metodo de flujo.
ShipFlowMode ship_scheduler_get_flow_mode(const ShipScheduler *scheduler); // Lee metodo de flujo.
const char *ship_scheduler_get_flow_mode_label(const ShipScheduler *scheduler); // Texto del metodo de flujo.
void ship_scheduler_set_fairness_window(ShipScheduler *scheduler, uint8_t windowW); // Ajusta parametro W.
uint8_t ship_scheduler_get_fairness_window(const ShipScheduler *scheduler); // Lee parametro W.
void ship_scheduler_set_sign_direction(ShipScheduler *scheduler, BoatSide side); // Ajusta direccion del letrero.
BoatSide ship_scheduler_get_sign_direction(const ShipScheduler *scheduler); // Lee direccion del letrero.
void ship_scheduler_set_sign_interval(ShipScheduler *scheduler, unsigned long intervalMillis); // Ajusta periodo del letrero.
unsigned long ship_scheduler_get_sign_interval(const ShipScheduler *scheduler); // Lee periodo del letrero.
void ship_scheduler_set_max_ready_queue(ShipScheduler *scheduler, uint8_t limit); // Ajusta cola maxima.
uint8_t ship_scheduler_get_max_ready_queue(const ShipScheduler *scheduler); // Lee cola maxima.
void ship_scheduler_set_channel_length(ShipScheduler *scheduler, uint16_t meters); // Ajusta largo del canal.
uint16_t ship_scheduler_get_channel_length(const ShipScheduler *scheduler); // Lee largo del canal.
void ship_scheduler_set_boat_speed(ShipScheduler *scheduler, uint16_t metersPerSec); // Ajusta velocidad base.
uint16_t ship_scheduler_get_boat_speed(const ShipScheduler *scheduler); // Lee velocidad base.
void ship_scheduler_set_flow_logging(ShipScheduler *scheduler, bool enabled); // Activa/desactiva trazas de flujo.
bool ship_scheduler_get_flow_logging(const ShipScheduler *scheduler); // Lee estado de trazas de flujo.

const Boat *ship_scheduler_get_active_boat(const ShipScheduler *scheduler); // Devuelve barco activo. 
uint8_t ship_scheduler_get_ready_count(const ShipScheduler *scheduler); // Devuelve listos. 
const Boat *ship_scheduler_get_ready_boat(const ShipScheduler *scheduler, uint8_t index); // Devuelve barco en cola. 
uint8_t ship_scheduler_get_waiting_count(const ShipScheduler *scheduler, BoatSide side); // Devuelve cantidad por lado. 
const Boat *ship_scheduler_get_waiting_boat(const ShipScheduler *scheduler, BoatSide side, uint8_t index); // Devuelve barco por lado. 
uint16_t ship_scheduler_get_completed_left_to_right(const ShipScheduler *scheduler); // Devuelve completados izq-der. 
uint16_t ship_scheduler_get_completed_right_to_left(const ShipScheduler *scheduler); // Devuelve completados der-izq. 
unsigned long ship_scheduler_get_active_elapsed_millis(const ShipScheduler *scheduler); // Devuelve tiempo activo. 
uint16_t ship_scheduler_get_completed_total(const ShipScheduler *scheduler); // Devuelve completados totales. 
uint16_t ship_scheduler_get_collision_detections(const ShipScheduler *scheduler); // Devuelve colisiones detectadas.
unsigned long ship_scheduler_get_total_wait_millis(const ShipScheduler *scheduler); // Devuelve espera acumulada. 
unsigned long ship_scheduler_get_total_turnaround_millis(const ShipScheduler *scheduler); // Devuelve turnaround acumulado. 
unsigned long ship_scheduler_get_total_service_millis(const ShipScheduler *scheduler); // Devuelve servicio acumulado. 
uint8_t ship_scheduler_get_completion_count(const ShipScheduler *scheduler); // Devuelve cantidad en orden final. 
uint8_t ship_scheduler_get_completion_id(const ShipScheduler *scheduler, uint8_t index); // Devuelve id en orden final. 

void ship_scheduler_notify_boat_finished(ShipScheduler *scheduler, Boat *b); // Callback de finalizacion. 
void ship_scheduler_pause_active(ShipScheduler *scheduler); // Pausa el barco activo. 
void ship_scheduler_resume_active(ShipScheduler *scheduler); // Reanuda el barco activo. 
void ship_scheduler_dump_status(const ShipScheduler *scheduler); // Imprime el estado por Serial. 

// Sensor de proximidad e interrupciones
void ship_scheduler_set_sensor_enabled(ShipScheduler *scheduler, bool enabled); // Habilita/deshabilita sensor.
bool ship_scheduler_get_sensor_enabled(const ShipScheduler *scheduler); // Lee estado del sensor.
void ship_scheduler_set_proximity_threshold(ShipScheduler *scheduler, uint16_t cm); // Ajusta umbral en cm.
uint16_t ship_scheduler_get_proximity_threshold(const ShipScheduler *scheduler); // Lee umbral en cm.
void ship_scheduler_set_proximity_distance(ShipScheduler *scheduler, uint16_t cm); // Ajusta distancia actual (simulada).
void ship_scheduler_set_proximity_distance_simulated(ShipScheduler *scheduler, uint16_t cm); // Ajusta distancia usando simulate.
uint16_t ship_scheduler_get_proximity_distance(const ShipScheduler *scheduler); // Lee distancia actual.
ShipEmergencyMode ship_scheduler_get_emergency_mode(const ShipScheduler *scheduler); // Lee modo de emergencia.
void ship_scheduler_trigger_emergency(ShipScheduler *scheduler); // Activa emergencia por proximidad.
void ship_scheduler_clear_emergency(ShipScheduler *scheduler); // Limpia el estado de emergencia.
void ship_scheduler_update_emergency(ShipScheduler *scheduler); // Actualiza estado de emergencia (llamar en tick).
uint8_t ship_scheduler_get_gate_left_state(const ShipScheduler *scheduler); // Obtiene estado puerta izquierda (0=open, 1=closing, 2=closed).
uint8_t ship_scheduler_get_gate_right_state(const ShipScheduler *scheduler); // Obtiene estado puerta derecha (0=open, 1=closing, 2=closed).

extern ShipScheduler *gScheduler; // Puntero global para callbacks de tareas. 

#define NOTIF_CMD_RUN 1UL // Comando RUN por notificacion. 
#define NOTIF_CMD_PAUSE 2UL // Comando PAUSE por notificacion. 
#define NOTIF_CMD_TERMINATE 3UL // Comando TERMINATE por notificacion. 

#ifdef __cplusplus // Cierra el bloque de linkage C. 
} // Fin de extern "C". 
#endif // Fin de compatibilidad C++. 