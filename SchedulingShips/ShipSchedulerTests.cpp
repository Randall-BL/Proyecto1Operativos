#include "ShipSchedulerTests.h"

#include <Arduino.h>

#include "ShipDisplay.h"
#include "ShipModel.h"

// Definicion del escenario usado para pruebas comparativas.
struct TestBoatSpec {
  BoatSide origin;
  BoatType type;
  unsigned long serviceMs;
  unsigned long deadlineMs;
  uint8_t priority;
};

// Escenario fijo para todos los algoritmos.
static const TestBoatSpec kScenario[] = {
  { SIDE_LEFT, BOAT_NORMAL, 6000, 9000, 1 },
  { SIDE_RIGHT, BOAT_PESQUERA, 3500, 6000, 2 },
  { SIDE_LEFT, BOAT_PATRULLA, 2000, 3000, 3 },
  { SIDE_RIGHT, BOAT_NORMAL, 5000, 10000, 1 },
  { SIDE_LEFT, BOAT_PESQUERA, 3000, 7000, 2 }
};

// Puntero opcional para renderizar progreso durante las pruebas.
static ShipDisplay *gTestDisplay = nullptr;

void setTestDisplay(ShipDisplay *display) {
  // Registra la pantalla que se usara para visualizar las pruebas.
  gTestDisplay = display;
}

static void renderTestFrame(ShipScheduler &scheduler, bool force) {
  // Renderiza un cuadro de prueba si hay pantalla disponible.
  if (!gTestDisplay) return;
  if (force) gTestDisplay->render(scheduler);
  else gTestDisplay->renderIfNeeded(scheduler);
}

// Encola barcos en dos tandas para forzar preempcion.
static void enqueueScenario(ShipScheduler &scheduler, const TestBoatSpec *specs, size_t count) {
  if (count == 0) return;

  size_t firstBatch = count > 2 ? 2 : count;
  for (size_t i = 0; i < firstBatch; i++) {
    Boat *boat = createBoat(specs[i].origin, specs[i].type);
    if (!boat) continue;
    boat->serviceMillis = specs[i].serviceMs;
    boat->remainingMillis = specs[i].serviceMs;
    boat->deadlineMillis = millis() + specs[i].deadlineMs;
    boat->priority = specs[i].priority;
    scheduler.enqueue(boat);
    renderTestFrame(scheduler, false);
  }

  scheduler.update();
  renderTestFrame(scheduler, true);
  delay(50);

  for (size_t i = firstBatch; i < count; i++) {
    Boat *boat = createBoat(specs[i].origin, specs[i].type);
    if (!boat) continue;
    boat->serviceMillis = specs[i].serviceMs;
    boat->remainingMillis = specs[i].serviceMs;
    boat->deadlineMillis = millis() + specs[i].deadlineMs;
    boat->priority = specs[i].priority;
    scheduler.enqueue(boat);
    renderTestFrame(scheduler, false);
    delay(20);
  }
}

// Hace polling al scheduler hasta que terminen o se cumpla el timeout.
static void waitUntilIdle(ShipScheduler &scheduler, unsigned long timeoutMs) {
  unsigned long start = millis();
  while ((scheduler.getReadyCount() > 0 || scheduler.getActiveBoat() != nullptr) &&
         (millis() - start < timeoutMs)) {
    scheduler.update();
    renderTestFrame(scheduler, false);
    delay(20);
  }
}

// Imprime el orden de finalizacion para comparacion visual.
static void printCompletionOrder(const ShipScheduler &scheduler) {
  uint8_t count = scheduler.getCompletionCount();
  Serial.print("Order: ");
  for (uint8_t i = 0; i < count; i++) {
    if (i > 0) Serial.print(" -> ");
    Serial.print('#');
    Serial.print(scheduler.getCompletionId(i));
  }
  Serial.println();
}

// Imprime metricas agregadas de la corrida.
static void printSummary(const ShipScheduler &scheduler) {
  uint16_t completed = scheduler.getCompletedTotal();
  unsigned long avgWait = completed ? (scheduler.getTotalWaitMillis() / completed) : 0;
  unsigned long avgTurn = completed ? (scheduler.getTotalTurnaroundMillis() / completed) : 0;

  Serial.print("Completed: ");
  Serial.println(completed);
  Serial.print("Avg wait: ");
  Serial.print(avgWait);
  Serial.println(" ms");
  Serial.print("Avg turnaround: ");
  Serial.print(avgTurn);
  Serial.println(" ms");
  printCompletionOrder(scheduler);
}

void runSchedulerTest(ShipScheduler &scheduler, ShipScheduler::Algo algo) {
  // Ejecuta el escenario con el algoritmo solicitado.
  scheduler.clear();
  resetBoatSequence();
  scheduler.setAlgorithm(algo);
  if (algo == ShipScheduler::ALG_RR) {
    scheduler.setRoundRobinQuantum(1200);
  }
  renderTestFrame(scheduler, true);

  Serial.println();
  Serial.print("== Test ");
  Serial.print(scheduler.getAlgorithmLabel());
  if (algo == ShipScheduler::ALG_RR) {
    Serial.print(" q=");
    Serial.print(scheduler.getRoundRobinQuantum());
    Serial.print("ms");
  }
  Serial.println(" ==");

  enqueueScenario(scheduler, kScenario, sizeof(kScenario) / sizeof(kScenario[0]));
  waitUntilIdle(scheduler, 30000);
  printSummary(scheduler);
}

void runSchedulerTests(ShipScheduler &scheduler) {
  // Ejecuta todas las variantes de algoritmos en secuencia.
  runSchedulerTest(scheduler, ShipScheduler::ALG_FCFS);
  runSchedulerTest(scheduler, ShipScheduler::ALG_RR);
  runSchedulerTest(scheduler, ShipScheduler::ALG_PRIORITY);
  runSchedulerTest(scheduler, ShipScheduler::ALG_STRN);
  runSchedulerTest(scheduler, ShipScheduler::ALG_EDF);
}
