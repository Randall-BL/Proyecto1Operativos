// Programa principal que orquesta la simulacion en el ESP32.
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

#include "ShipPins.h"
#include "ShipModel.h"
#include "ShipScheduler.h"
#include "ShipCommands.h"
#include "ShipDisplay.h"
#include "ShipSchedulerTests.h"

// Instancias globales usadas por el loop principal.
ShipDisplay shipDisplay;
ShipScheduler shipScheduler;

void setup() {
  // Inicializa Serial, pantalla y scheduler con un manifiesto de prueba.
  Serial.begin(115200);
  uint32_t startLog = millis();
  while (!Serial && (millis() - startLog < 3000)) {
  }

  Serial.println("Sistema Scheduling Ships iniciado.");

  shipDisplay.begin();
  setTestDisplay(&shipDisplay);
  shipScheduler.begin();
  shipScheduler.loadDemoManifest();
  shipDisplay.render(shipScheduler);

  Serial.println("Scheduler listo. Escribe 'help' para comandos.");
}

void loop() {
  // En cada ciclo: lee comandos, actualiza scheduler y refresca la interfaz si aplica.
  processSerialInput(shipScheduler);
  shipScheduler.update();
  shipDisplay.renderIfNeeded(shipScheduler);
}