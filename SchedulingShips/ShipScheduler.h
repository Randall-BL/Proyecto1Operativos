#pragma once

#include "ShipModel.h"

// Administra la cola de listos, el barco activo y los algoritmos de planificacion.
class ShipScheduler {
public:
  // Inicializa el estado interno del scheduler.
  void begin();
  // Carga un manifiesto de prueba predefinido.
  void loadDemoManifest();
  // Limpia colas y detiene tareas activas.
  void clear();
  // Agrega un barco nuevo a la cola de listos.
  void enqueue(Boat *boat);
  // Avanza las decisiones de planificacion.
  void update();

  // Algoritmos de planificacion soportados por el simulador.
  enum Algo { ALG_FCFS = 0, ALG_STRN = 1, ALG_EDF = 2, ALG_RR = 3, ALG_PRIORITY = 4 };
  void setAlgorithm(Algo a) { algorithm = a; }
  Algo getAlgorithm() const { return algorithm; }
  // Etiqueta corta usada por la interfaz.
  const char *getAlgorithmLabel() const;
  // Configura el quantum de Round Robin.
  void setRoundRobinQuantum(unsigned long quantumMillis);
  unsigned long getRoundRobinQuantum() const { return rrQuantumMillis; }

  // Accesores de solo lectura para la interfaz y pruebas.
  const Boat *getActiveBoat() const;
  uint8_t getReadyCount() const;
  const Boat *getReadyBoat(uint8_t index) const;
  uint8_t getWaitingCount(BoatSide side) const;
  const Boat *getWaitingBoat(BoatSide side, uint8_t index) const;
  uint16_t getCompletedLeftToRight() const;
  uint16_t getCompletedRightToLeft() const;
  unsigned long getActiveElapsedMillis() const;
  uint16_t getCompletedTotal() const { return completedTotal; }
  unsigned long getTotalWaitMillis() const { return totalWaitMillis; }
  unsigned long getTotalTurnaroundMillis() const { return totalTurnaroundMillis; }
  unsigned long getTotalServiceMillis() const { return totalServiceMillis; }
  uint8_t getCompletionCount() const { return completionCount; }
  uint8_t getCompletionId(uint8_t index) const;

  // Llamado por tareas de barco cuando terminan.
  void notifyBoatFinished(Boat *b);

  // API de control para tareas.
  void pauseActive();
  void resumeActive();
  void dumpStatus();

private:
  // Helpers internos usados por update() y enqueue().
  void startNextBoat();
  void finishActiveBoat();
  void requeueBoat(Boat *boat, bool atFront);
  void preemptActiveForRR();

  Boat *readyQueue[MAX_BOATS];
  uint8_t readyCount = 0;
  Boat *activeBoat = nullptr;
  bool hasActiveBoat = false;
  unsigned long crossingStartedAt = 0;
  uint16_t completedLeftToRight = 0;
  uint16_t completedRightToLeft = 0;
  uint16_t completedTotal = 0;
  unsigned long totalWaitMillis = 0;
  unsigned long totalTurnaroundMillis = 0;
  unsigned long totalServiceMillis = 0;
  uint8_t completionOrder[MAX_BOATS];
  uint8_t completionCount = 0;
  bool ignoreCompletions = false;
  Algo algorithm = ALG_FCFS;
  unsigned long rrQuantumMillis = 1000;
};

// Puntero global para que las tareas reporten al scheduler.
extern ShipScheduler *gScheduler;

// Valores de comandos para xTaskNotify en modo value.
constexpr uint32_t NOTIF_CMD_RUN = 1;
constexpr uint32_t NOTIF_CMD_PAUSE = 2;
constexpr uint32_t NOTIF_CMD_TERMINATE = 3;