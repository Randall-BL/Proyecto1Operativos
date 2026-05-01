#include "ShipCommands.h"
#include "ShipSchedulerTests.h"

// Parsea una linea de comando y aplica la accion al scheduler.
static void handleCommand(ShipScheduler &scheduler, String command) {
  command.trim();
  command.toLowerCase();

  if (command == "help") {
    // Imprime ayuda con la lista de comandos soportados.
    Serial.println("Comandos: demo | clear | add <l|r> <n|p|u> [prio]");
    Serial.println("          alg <fcfs|sjf|strn|edf|rr|prio> [ms]");
    Serial.println("          pause | resume | status | test [all|rr|prio|fcfs|sjf|strn|edf]");
    return;
  }

  if (command == "demo") {
    // Limpia colas y encola un conjunto de barcos de demostracion.
    scheduler.loadDemoManifest();
    return;
  }

  if (command.startsWith("alg ")) {
    // Cambia el algoritmo de planificacion (RR puede llevar quantum opcional).
    String arg = command.substring(4);
    arg.trim();
    int spaceAt = arg.indexOf(' ');
    String name = spaceAt >= 0 ? arg.substring(0, spaceAt) : arg;
    String value = spaceAt >= 0 ? arg.substring(spaceAt + 1) : "";
    name.trim();
    value.trim();

    if (name == "fcfs") {
      scheduler.setAlgorithm(ShipScheduler::ALG_FCFS);
      Serial.println("Algoritmo: FCFS");
    } else if (name == "strn") {
      scheduler.setAlgorithm(ShipScheduler::ALG_STRN);
      Serial.println("Algoritmo: STRN (Shortest Remaining Time)");
    } else if (name == "edf") {
      scheduler.setAlgorithm(ShipScheduler::ALG_EDF);
      Serial.println("Algoritmo: EDF (Earliest Deadline First)");
    } else if (name == "rr") {
      scheduler.setAlgorithm(ShipScheduler::ALG_RR);
      if (value.length() > 0) {
        unsigned long quantum = (unsigned long)value.toInt();
        if (quantum > 0) scheduler.setRoundRobinQuantum(quantum);
      }
      Serial.print("Algoritmo: RR q=");
      Serial.print(scheduler.getRoundRobinQuantum());
      Serial.println("ms");
    } else if (name == "sjf") {
      scheduler.setAlgorithm(ShipScheduler::ALG_SJF);
      Serial.println("Algoritmo: SJF (Shortest Job First)");
    } else if (name == "prio" || name == "prioridad" || name == "priority") {
      scheduler.setAlgorithm(ShipScheduler::ALG_PRIORITY);
      Serial.println("Algoritmo: Prioridad");
    } else {
      Serial.println("Uso: alg <fcfs|sjf|strn|edf|rr|prio> [ms]");
    }
    return;
  }

  if (command == "clear") {
    // Limpia colas y detiene el barco activo si existe.
    scheduler.clear();
    Serial.println("Colas limpiadas.");
    return;
  }

  if (command == "pause") {
    // Pausa el barco activo actual.
    scheduler.pauseActive();
    return;
  }

  if (command == "resume") {
    // Reanuda el barco activo actual.
    scheduler.resumeActive();
    return;
  }

  if (command == "status") {
    // Imprime el estado interno del scheduler.
    scheduler.dumpStatus();
    return;
  }

  if (command.startsWith("test")) {
    // Ejecuta pruebas del scheduler para comparar algoritmos.
    String arg = command.substring(4);
    arg.trim();
    if (arg.length() == 0 || arg == "all") {
      runSchedulerTests(scheduler);
    } else if (arg == "rr") {
      runSchedulerTest(scheduler, ShipScheduler::ALG_RR);
    } else if (arg == "prio" || arg == "prioridad" || arg == "priority") {
      runSchedulerTest(scheduler, ShipScheduler::ALG_PRIORITY);
    } else if (arg == "fcfs") {
      runSchedulerTest(scheduler, ShipScheduler::ALG_FCFS);
    } else if (arg == "sjf") {
      runSchedulerTest(scheduler, ShipScheduler::ALG_SJF);
    } else if (arg == "strn") {
      runSchedulerTest(scheduler, ShipScheduler::ALG_STRN);
    } else if (arg == "edf") {
      runSchedulerTest(scheduler, ShipScheduler::ALG_EDF);
    } else {
      Serial.println("Uso: test [all|rr|prio|fcfs|sjf|strn|edf]");
    }
    return;
  }

  if (command.startsWith("add ")) {
    // Encola un barco nuevo con prioridad opcional.
    int firstSpace = command.indexOf(' ');
    int secondSpace = command.indexOf(' ', firstSpace + 1);
    int thirdSpace = command.indexOf(' ', secondSpace + 1);
    if (firstSpace < 0 || secondSpace < 0) {
      Serial.println("Formato: add <l|r> <n|p|u> [prio]");
      return;
    }

    String sideToken = command.substring(firstSpace + 1, secondSpace);
    String typeToken = thirdSpace < 0
                         ? command.substring(secondSpace + 1)
                         : command.substring(secondSpace + 1, thirdSpace);
    String prioToken = thirdSpace < 0 ? "" : command.substring(thirdSpace + 1);
    prioToken.trim();
    BoatSide side = sideToken.startsWith("l") ? SIDE_LEFT : SIDE_RIGHT;
    BoatType type = BOAT_NORMAL;

    if (typeToken.startsWith("p")) {
      type = BOAT_PESQUERA;
    } else if (typeToken.startsWith("u") || typeToken.startsWith("r")) {
      type = BOAT_PATRULLA;
    }

    if (prioToken.length() > 0) {
      long prioValue = prioToken.toInt();
      if (prioValue < 1) prioValue = 1;
      if (prioValue > 9) prioValue = 9;
      scheduler.enqueue(createBoatWithPriority(side, type, (uint8_t)prioValue));
    } else {
      scheduler.enqueue(createBoat(side, type));
    }
    Serial.println("Barco agregado a la cola.");
    return;
  }

  Serial.println("Comando no reconocido. Use help.");
}

void processSerialInput(ShipScheduler &scheduler) {
  // Lee una linea de Serial si hay datos y la procesa.
  if (Serial.available() == 0) {
    return;
  }

  String command = Serial.readStringUntil('\n');
  command.replace("\r", "");
  handleCommand(scheduler, command);
}