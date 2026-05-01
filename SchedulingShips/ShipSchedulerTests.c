#include "ShipSchedulerTests.h" // API de pruebas. 

#include <stddef.h> // size_t y NULL. 

#include "ShipIO.h" // Logging por Serial. 
#include "ShipModel.h" // Modelo de barcos. 

typedef struct TestBoatSpec { // Estructura del escenario. 
  BoatSide origin; // Lado de entrada. 
  BoatType type; // Tipo de barco. 
  unsigned long serviceMs; // Tiempo de servicio. 
  unsigned long deadlineMs; // Deadline relativo. 
  uint8_t priority; // Prioridad para algoritmo. 
} TestBoatSpec; // Alias del tipo de prueba. 

static const TestBoatSpec kScenario[] = { // Escenario fijo para todas las pruebas. 
  { SIDE_LEFT, BOAT_NORMAL, 6000, 9000, 1 }, // Caso 1. 
  { SIDE_RIGHT, BOAT_PESQUERA, 3500, 6000, 2 }, // Caso 2. 
  { SIDE_LEFT, BOAT_PATRULLA, 2000, 3000, 3 }, // Caso 3. 
  { SIDE_RIGHT, BOAT_NORMAL, 5000, 10000, 1 }, // Caso 4. 
  { SIDE_LEFT, BOAT_PESQUERA, 3000, 7000, 2 } // Caso 5. 
}; // Fin del escenario. 

static void enqueueScenario(ShipScheduler *scheduler, const TestBoatSpec *specs, size_t count) { // Encola el escenario. 
  if (!scheduler || !specs || count == 0) return; // Valida entradas. 

  // Primera tanda: deja que el scheduler arranque con dos barcos para que luego haya preempcion. 
  size_t firstBatch = count > 2 ? 2 : count; // Calcula la primera tanda. 
  for (size_t i = 0; i < firstBatch; i++) { // Itera la primera tanda. 
    // Crea un barco con los tiempos y prioridad del escenario de prueba. 
    Boat *boat = createBoat(specs[i].origin, specs[i].type); // Crea el barco. 
    if (!boat) continue; // Si falla, continua. 
    boat->serviceMillis = specs[i].serviceMs; // Asigna servicio. 
    boat->remainingMillis = specs[i].serviceMs; // Asigna restante. 
    boat->deadlineMillis = millis() + specs[i].deadlineMs; // Asigna deadline. 
    boat->priority = specs[i].priority; // Asigna prioridad. 
    ship_scheduler_enqueue(scheduler, boat); // Encola el barco. 
  } // Fin del for. 

  // Avanza una vez la simulacion para activar el primer barco. 
  ship_scheduler_update(scheduler); // Ejecuta un tick. 
  delay(50); // Espera un poco. 

  for (size_t i = firstBatch; i < count; i++) { // Itera la segunda tanda. 
    // Encola el resto para probar comportamiento comparativo entre algoritmos. 
    Boat *boat = createBoat(specs[i].origin, specs[i].type); // Crea el barco. 
    if (!boat) continue; // Si falla, continua. 
    boat->serviceMillis = specs[i].serviceMs; // Asigna servicio. 
    boat->remainingMillis = specs[i].serviceMs; // Asigna restante. 
    boat->deadlineMillis = millis() + specs[i].deadlineMs; // Asigna deadline. 
    boat->priority = specs[i].priority; // Asigna prioridad. 
    ship_scheduler_enqueue(scheduler, boat); // Encola el barco. 
    delay(20); // Espera para espaciado. 
  } // Fin del for. 
} // Fin de enqueueScenario. 

static void waitUntilIdle(ShipScheduler *scheduler, unsigned long timeoutMs) { // Espera hasta que termine. 
  if (!scheduler) return; // Valida el scheduler. 
  // Espera activa acotada para dejar terminar la simulacion sin bloquear indefinidamente. 
  unsigned long start = millis(); // Marca el inicio. 
  while ((ship_scheduler_get_ready_count(scheduler) > 0 || ship_scheduler_get_active_boat(scheduler) != NULL) &&
         (millis() - start < timeoutMs)) { // Condicion de espera. 
    ship_scheduler_update(scheduler); // Avanza el scheduler. 
    delay(20); // Pequeña pausa. 
  } // Fin del while. 
} // Fin de waitUntilIdle. 

static void printCompletionOrder(const ShipScheduler *scheduler) { // Imprime orden de finalizacion. 
  uint8_t count = ship_scheduler_get_completion_count(scheduler); // Cantidad finalizada. 
  ship_log("Order: "); // Encabezado de orden. 
  for (uint8_t i = 0; i < count; i++) { // Itera IDs. 
    if (i > 0) ship_log(" -> "); // Separador. 
    ship_logf("#%u", ship_scheduler_get_completion_id(scheduler, i)); // Imprime ID. 
  } // Fin del for. 
  ship_logln(""); // Cierra la linea. 
} // Fin de printCompletionOrder. 

static void printSummary(const ShipScheduler *scheduler) { // Imprime resumen de metricas. 
  // Resume las metricas principales para comparar algoritmos en la defensa. 
  uint16_t completed = ship_scheduler_get_completed_total(scheduler); // Total completados. 
  unsigned long avgWait = completed ? (ship_scheduler_get_total_wait_millis(scheduler) / completed) : 0; // Promedio espera. 
  unsigned long avgTurn = completed ? (ship_scheduler_get_total_turnaround_millis(scheduler) / completed) : 0; // Promedio turnaround. 

  ship_logf("Completed: %u\n", completed); // Imprime completados. 
  ship_logf("Avg wait: %lu ms\n", avgWait); // Imprime espera promedio. 
  ship_logf("Avg turnaround: %lu ms\n", avgTurn); // Imprime turnaround promedio. 
  printCompletionOrder(scheduler); // Imprime el orden. 
} // Fin de printSummary. 

void run_scheduler_test(ShipScheduler *scheduler, ShipAlgo algo) { // Ejecuta una prueba puntual. 
  if (!scheduler) return; // Valida el scheduler. 

  // Reinicia el estado para que la corrida sea repetible. 
  ship_scheduler_clear(scheduler); // Limpia el scheduler. 
  resetBoatSequence(); // Reinicia secuencias. 
  ship_scheduler_set_algorithm(scheduler, algo); // Configura algoritmo. 
  if (algo == ALG_RR) { // Si es RR. 
    ship_scheduler_set_round_robin_quantum(scheduler, 1200); // Ajusta quantum. 
  } // Fin del if. 

  ship_logln(""); // Separador en el log. 
  ship_logf("== Test %s", ship_scheduler_get_algorithm_label(scheduler)); // Encabezado de prueba. 
  if (algo == ALG_RR) { // Si es RR. 
    ship_logf(" q=%lums", ship_scheduler_get_round_robin_quantum(scheduler)); // Muestra quantum. 
  } // Fin del if. 
  ship_logln(" =="); // Cierra el encabezado. 

  enqueueScenario(scheduler, kScenario, sizeof(kScenario) / sizeof(kScenario[0])); // Encola el escenario. 
  waitUntilIdle(scheduler, 30000); // Espera a que termine. 
  printSummary(scheduler); // Imprime resumen. 
} // Fin de run_scheduler_test. 

void run_scheduler_tests(ShipScheduler *scheduler) { // Ejecuta todas las pruebas. 
  if (!scheduler) return; // Valida el scheduler. 
  // Ejecuta todos los algoritmos soportados por el harness de pruebas. 
  run_scheduler_test(scheduler, ALG_FCFS); // Prueba FCFS. 
  run_scheduler_test(scheduler, ALG_SJF); // Prueba SJF. 
  run_scheduler_test(scheduler, ALG_RR); // Prueba RR. 
  run_scheduler_test(scheduler, ALG_PRIORITY); // Prueba prioridad. 
  run_scheduler_test(scheduler, ALG_STRN); // Prueba STRN. 
  run_scheduler_test(scheduler, ALG_EDF); // Prueba EDF. 
} // Fin de run_scheduler_tests. 
