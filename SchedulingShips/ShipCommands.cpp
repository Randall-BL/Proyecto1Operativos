#include "ShipCommands.h"

static void handleCommand(ShipScheduler &scheduler, String command) {
  command.trim();
  command.toLowerCase();

  if (command == "help") {
    Serial.println("Comandos: demo | clear | add <l|r> <n|p|u>");
    return;
  }

  if (command == "demo") {
    scheduler.loadDemoManifest();
    return;
  }

  if (command.startsWith("alg ")) {
    String arg = command.substring(4);
    arg.trim();
    if (arg == "fcfs") {
      scheduler.setAlgorithm(ShipScheduler::ALG_FCFS);
      Serial.println("Algoritmo: FCFS");
    } else if (arg == "strn") {
      scheduler.setAlgorithm(ShipScheduler::ALG_STRN);
      Serial.println("Algoritmo: STRN (Shortest Remaining Time)");
    } else if (arg == "edf") {
      scheduler.setAlgorithm(ShipScheduler::ALG_EDF);
      Serial.println("Algoritmo: EDF (Earliest Deadline First)");
    } else {
      Serial.println("Uso: alg <fcfs|strn|edf>");
    }
    return;
  }

  if (command == "clear") {
    scheduler.clear();
    Serial.println("Colas limpiadas.");
    return;
  }

  if (command == "pause") {
    scheduler.pauseActive();
    return;
  }

  if (command == "resume") {
    scheduler.resumeActive();
    return;
  }

  if (command == "status") {
    scheduler.dumpStatus();
    return;
  }

  if (command.startsWith("add ")) {
    int firstSpace = command.indexOf(' ');
    int secondSpace = command.indexOf(' ', firstSpace + 1);
    if (firstSpace < 0 || secondSpace < 0) {
      Serial.println("Formato: add <l|r> <n|p|u>");
      return;
    }

    String sideToken = command.substring(firstSpace + 1, secondSpace);
    String typeToken = command.substring(secondSpace + 1);
    BoatSide side = sideToken.startsWith("l") ? SIDE_LEFT : SIDE_RIGHT;
    BoatType type = BOAT_NORMAL;

    if (typeToken.startsWith("p")) {
      type = BOAT_PESQUERA;
    } else if (typeToken.startsWith("u") || typeToken.startsWith("r")) {
      type = BOAT_PATRULLA;
    }

    scheduler.enqueue(createBoat(side, type));
    Serial.println("Barco agregado a la cola.");
    return;
  }

  Serial.println("Comando no reconocido. Use help.");
}

void processSerialInput(ShipScheduler &scheduler) {
  if (Serial.available() == 0) {
    return;
  }

  String command = Serial.readStringUntil('\n');
  command.replace("\r", "");
  handleCommand(scheduler, command);
}