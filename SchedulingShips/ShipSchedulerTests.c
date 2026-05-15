#include "ShipSchedulerTests.h" // API de pruebas. 

#include <stddef.h> // size_t y NULL. 
#include <string.h> // memset para utilidades locales.

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

static void waitUntilIdle(ShipScheduler *scheduler, unsigned long timeoutMs); // Declaracion adelantada para pruebas de flujo.

static bool gCurrentFlowTestFailed = false; // Resultado acumulado del caso actual.
static uint16_t gCurrentFlowAssertions = 0; // Cantidad de aserciones ejecutadas.

static void flow_test_begin(const char *name) { // Abre un caso de prueba de flujo.
  gCurrentFlowTestFailed = false; // Reinicia estado de fallo.
  gCurrentFlowAssertions = 0; // Reinicia contador.
  ship_logln(""); // Separador visual.
  ship_logf("== Flow Test: %s ==\n", name); // Titulo del caso.
} // Fin de flow_test_begin.

static void flow_test_assert(bool condition, const char *message) { // Evalua una asercion de flujo.
  gCurrentFlowAssertions++; // Cuenta aserciones.
  if (condition) { // Caso exitoso.
    ship_logf("  [OK] %s\n", message); // Reporta exito.
  } else { // Caso fallido.
    gCurrentFlowTestFailed = true; // Marca fallo del caso.
    ship_logf("  [FAIL] %s\n", message); // Reporta fallo.
  }
} // Fin de flow_test_assert.

static bool flow_test_end(void) { // Cierra caso y retorna exito.
  if (gCurrentFlowTestFailed) { // Si hubo fallos.
    ship_logf("Result: FAIL (%u assertions)\n", gCurrentFlowAssertions); // Resultado fallido.
    return false; // Retorna falso.
  }
  ship_logf("Result: PASS (%u assertions)\n", gCurrentFlowAssertions); // Resultado exitoso.
  return true; // Retorna verdadero.
} // Fin de flow_test_end.

static Boat *createBoatForTest(BoatSide side, BoatType type, unsigned long serviceMs) { // Crea un barco con tiempos acotados para test.
  Boat *boat = createBoat(side, type); // Crea barco base.
  if (!boat) return NULL; // Valida creacion.
  boat->serviceMillis = serviceMs; // Ajusta servicio.
  boat->remainingMillis = serviceMs; // Ajusta restante.
  boat->deadlineMillis = millis() + (serviceMs * 2UL); // Ajusta deadline relativo.
  return boat; // Retorna barco listo.
} // Fin de createBoatForTest.

static bool run_flow_test_fairness_window(ShipScheduler *scheduler) { // Verifica alternancia por ventana W.
  if (!scheduler) return false; // Valida puntero.
  flow_test_begin("Equidad W alterna lados"); // Inicia caso.

  ship_scheduler_clear(scheduler); // Limpia estado.
  resetBoatSequence(); // Reinicia IDs.
  ship_scheduler_set_algorithm(scheduler, ALG_FCFS); // Algoritmo estable para orden.
  ship_scheduler_set_flow_mode(scheduler, FLOW_FAIRNESS); // Activa equidad.
  ship_scheduler_set_fairness_window(scheduler, 2); // Configura W=2.

  ship_scheduler_enqueue(scheduler, createBoatForTest(SIDE_LEFT, BOAT_NORMAL, 900)); // #1
  ship_scheduler_enqueue(scheduler, createBoatForTest(SIDE_LEFT, BOAT_PESQUERA, 900)); // #2
  ship_scheduler_enqueue(scheduler, createBoatForTest(SIDE_LEFT, BOAT_PATRULLA, 900)); // #3
  ship_scheduler_enqueue(scheduler, createBoatForTest(SIDE_RIGHT, BOAT_NORMAL, 900)); // #4
  ship_scheduler_enqueue(scheduler, createBoatForTest(SIDE_RIGHT, BOAT_PESQUERA, 900)); // #5
  ship_scheduler_enqueue(scheduler, createBoatForTest(SIDE_RIGHT, BOAT_PATRULLA, 900)); // #6

  waitUntilIdle(scheduler, 20000); // Espera fin de ejecucion.

  flow_test_assert(ship_scheduler_get_completed_total(scheduler) == 6, "Se completan los 6 barcos"); // Verifica total.
  if (ship_scheduler_get_completion_count(scheduler) >= 6) { // Valida que haya orden completo.
    flow_test_assert(ship_scheduler_get_completion_id(scheduler, 0) == 1, "Orden[0] = #1"); // Esperado por W.
    flow_test_assert(ship_scheduler_get_completion_id(scheduler, 1) == 2, "Orden[1] = #2"); // Esperado por W.
    flow_test_assert(ship_scheduler_get_completion_id(scheduler, 2) == 4, "Orden[2] = #4"); // Esperado por W.
    flow_test_assert(ship_scheduler_get_completion_id(scheduler, 3) == 5, "Orden[3] = #5"); // Esperado por W.
  } else { // Si no hay orden completo, falla explicito.
    flow_test_assert(false, "Completion order tiene al menos 6 entradas"); // Falla de estructura.
  }

  return flow_test_end(); // Retorna resultado del caso.
} // Fin de run_flow_test_fairness_window.

static bool run_flow_test_sign_switch_timer(ShipScheduler *scheduler) { // Verifica cambio automatico de letrero por tiempo.
  if (!scheduler) return false; // Valida puntero.
  flow_test_begin("Letrero cambia por tiempo"); // Inicia caso.

  ship_scheduler_clear(scheduler); // Limpia estado.
  ship_scheduler_set_flow_mode(scheduler, FLOW_SIGN); // Activa letrero.
  ship_scheduler_set_sign_direction(scheduler, SIDE_LEFT); // Define direccion inicial.
  ship_scheduler_set_sign_interval(scheduler, 1000); // Usa minimo permitido.

  flow_test_assert(ship_scheduler_get_sign_direction(scheduler) == SIDE_LEFT, "Direccion inicial izquierda"); // Verifica estado inicial.
  delay(1100); // Espera para forzar cambio.
  ship_scheduler_update(scheduler); // Avanza tick de letrero.
  flow_test_assert(ship_scheduler_get_sign_direction(scheduler) == SIDE_RIGHT, "Cambia a derecha tras un intervalo"); // Verifica primer cambio.

  delay(1100); // Espera segundo cambio.
  ship_scheduler_update(scheduler); // Avanza tick.
  flow_test_assert(ship_scheduler_get_sign_direction(scheduler) == SIDE_LEFT, "Regresa a izquierda tras segundo intervalo"); // Verifica segundo cambio.

  return flow_test_end(); // Retorna resultado del caso.
} // Fin de run_flow_test_sign_switch_timer.

static bool run_flow_test_sign_fallback_and_tico(ShipScheduler *scheduler) { // Verifica alternativa del letrero y comportamiento tico.
  if (!scheduler) return false; // Valida puntero.
  flow_test_begin("Alternativa letrero y Tico"); // Inicia caso.

  ship_scheduler_clear(scheduler); // Limpia estado.
  resetBoatSequence(); // Reinicia IDs para validacion.
  ship_scheduler_set_algorithm(scheduler, ALG_FCFS); // Algoritmo simple.
  ship_scheduler_set_flow_mode(scheduler, FLOW_SIGN); // Modo letrero.
  ship_scheduler_set_sign_direction(scheduler, SIDE_LEFT); // Letrero apunta a izquierda.

  ship_scheduler_enqueue(scheduler, createBoatForTest(SIDE_RIGHT, BOAT_NORMAL, 700)); // Solo hay barco derecha.
  ship_scheduler_update(scheduler); // Debe despachar por alternativa.
  const Boat *activeSign = ship_scheduler_get_active_boat(scheduler); // Lee activo.
  flow_test_assert(activeSign != NULL, "Con letrero y un solo lado disponible se despacha barco"); // Verifica que no se bloquee.
  if (activeSign) flow_test_assert(activeSign->origin == SIDE_RIGHT, "Alternativa permite lado opuesto cuando el del letrero esta vacio"); // Verifica origen.
  waitUntilIdle(scheduler, 10000); // Deja terminar.

  ship_scheduler_clear(scheduler); // Reinicia para tico.
  resetBoatSequence(); // Reinicia IDs.
  ship_scheduler_set_algorithm(scheduler, ALG_SJF); // SJF para comprobar no restriccion por lado.
  ship_scheduler_set_flow_mode(scheduler, FLOW_TICO); // Modo sin control por lado.

  ship_scheduler_enqueue(scheduler, createBoatForTest(SIDE_LEFT, BOAT_NORMAL, 1800)); // #1 largo.
  ship_scheduler_enqueue(scheduler, createBoatForTest(SIDE_RIGHT, BOAT_PATRULLA, 700)); // #2 corto.
  waitUntilIdle(scheduler, 10000); // Espera fin.

  flow_test_assert(ship_scheduler_get_completed_total(scheduler) == 2, "En Tico se completan ambos barcos"); // Verifica finalizacion.
  if (ship_scheduler_get_completion_count(scheduler) >= 2) { // Valida orden disponible.
    flow_test_assert(ship_scheduler_get_completion_id(scheduler, 0) == 2, "En Tico+SJF pasa primero el trabajo mas corto (#2)"); // Verifica no restriccion de lado.
  } else { // Si no hay orden completo.
    flow_test_assert(false, "Completion order contiene 2 entradas"); // Marca falla.
  }

  return flow_test_end(); // Retorna resultado del caso.
} // Fin de run_flow_test_sign_fallback_and_tico.

static bool run_flow_test_no_collision(ShipScheduler *scheduler) { // Verifica ausencia de colisiones durante ejecucion normal.
  if (!scheduler) return false; // Valida puntero.
  flow_test_begin("No colisiones en ejecucion"); // Inicia caso.

  ship_scheduler_clear(scheduler); // Limpia estado.
  resetBoatSequence(); // Reinicia IDs.
  ship_scheduler_set_algorithm(scheduler, ALG_RR); // Algoritmo con preempcion por quantum.
  ship_scheduler_set_round_robin_quantum(scheduler, 1000); // Quantum estable.
  ship_scheduler_set_flow_mode(scheduler, FLOW_FAIRNESS); // Modo de flujo alternante.
  ship_scheduler_set_fairness_window(scheduler, 1); // Alterna cada barco.

  ship_scheduler_enqueue(scheduler, createBoatForTest(SIDE_LEFT, BOAT_NORMAL, 1200)); // #1
  ship_scheduler_enqueue(scheduler, createBoatForTest(SIDE_RIGHT, BOAT_NORMAL, 1200)); // #2
  ship_scheduler_enqueue(scheduler, createBoatForTest(SIDE_LEFT, BOAT_PESQUERA, 1200)); // #3
  ship_scheduler_enqueue(scheduler, createBoatForTest(SIDE_RIGHT, BOAT_PATRULLA, 1200)); // #4
  waitUntilIdle(scheduler, 20000); // Espera fin.

  flow_test_assert(ship_scheduler_get_completed_total(scheduler) == 4, "Se completan los barcos de ambos sentidos"); // Verifica progreso.
  flow_test_assert(ship_scheduler_get_collision_detections(scheduler) == 0, "No se detectan colisiones"); // Verifica seguridad.

  return flow_test_end(); // Retorna resultado del caso.
} // Fin de run_flow_test_no_collision.

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
  run_flow_control_tests(scheduler); // Prueba controles de flujo. 
} // Fin de run_scheduler_tests. 

static bool run_emergency_proximity_test(ShipScheduler *scheduler) { // Verifica disparo de emergencia por proximidad.
  if (!scheduler) return false; // Valida scheduler.
  
  ship_logln("[TEST] === Prueba de Emergencia por Proximidad ==="); // Encabezado.
  
  // Limpia y reinicia
  ship_scheduler_clear(scheduler); // Limpia estado previo.
  ship_scheduler_set_sensor_enabled(scheduler, true); // Activa sensor.
  ship_scheduler_set_proximity_threshold(scheduler, 100); // Umbral 100cm.
  ship_scheduler_set_proximity_distance(scheduler, 999); // Distancia lejana inicial.
  
  // Carga barcos demo
  ship_scheduler_load_demo_manifest(scheduler); // Carga barcos.
  if (scheduler->readyCount == 0) {
    ship_logln("[TEST] FAIL: No boats in ready queue"); // Error sin barcos.
    return false; // Falla.
  }
  
  // Verifica que no hay emergencia inicialmente
  if (ship_scheduler_get_emergency_mode(scheduler) != EMERGENCY_NONE) {
    ship_logln("[TEST] FAIL: Emergency should be NONE initially"); // Error de estado.
    return false; // Falla.
  }
  
  // Simula un barco acercandose por debajo del umbral
  ship_logln("[TEST] Simulando barco acercandose..."); // Aviso.
  ship_scheduler_set_proximity_distance(scheduler, 80); // 80cm < umbral 100cm.
  
  // Verifica que se activo la emergencia
  if (ship_scheduler_get_emergency_mode(scheduler) == EMERGENCY_NONE) {
    ship_logln("[TEST] FAIL: Emergency should be activated"); // Error: no se activo.
    return false; // Falla.
  }
  
  ship_logf("[TEST] Emergency mode: %u\n", ship_scheduler_get_emergency_mode(scheduler)); // Log modo.
  
  // Verifica que los barcos siguen en la cola lista (no removidos aun si no habia activo)
  // O que el barco activo fue removido si habia uno
  ship_logf("[TEST] Ready queue count: %u\n", scheduler->readyCount); // Log cantidad.
  
  // Limpia la emergencia
  ship_scheduler_clear_emergency(scheduler); // Limpia estado.
  
  // Verifica que volvio a normal
  if (ship_scheduler_get_emergency_mode(scheduler) != EMERGENCY_NONE) {
    ship_logln("[TEST] FAIL: Emergency should be cleared"); // Error no se limpio.
    return false; // Falla.
  }
  
  ship_logln("[TEST] PASS: Emergency proximity test passed"); // Exito.
  return true; // Exito.
} // Fin de run_emergency_proximity_test.

void run_flow_control_tests(ShipScheduler *scheduler) { // Ejecuta bateria de control de flujo.
  if (!scheduler) return; // Valida scheduler.
  uint8_t passed = 0; // Conteo de casos exitosos.
  uint8_t total = 4; // Total de casos de flujo.

  if (run_flow_test_fairness_window(scheduler)) passed++; // Ejecuta equidad.
  if (run_flow_test_sign_switch_timer(scheduler)) passed++; // Ejecuta letrero por tiempo.
  if (run_flow_test_sign_fallback_and_tico(scheduler)) passed++; // Ejecuta alternativa y tico.
  if (run_flow_test_no_collision(scheduler)) passed++; // Ejecuta no-colision.

  ship_logln(""); // Separador final.
  ship_logf("== Flow Tests Summary: %u/%u passed ==\n", passed, total); // Resumen final.
} // Fin de run_flow_control_tests.
