#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

#include "ShipPins.h"
#include "ShipModel.h"
#include "ShipScheduler.h"
#include "ShipCommands.h"
#include "ShipDisplay.h"

ShipDisplay shipDisplay;
ShipScheduler shipScheduler;

void setup() {
  Serial.begin(115200);
  uint32_t startLog = millis();
  while (!Serial && (millis() - startLog < 3000)) {
  }

  Serial.println("Sistema Scheduling Ships iniciado.");

  shipDisplay.begin();
  shipScheduler.begin();
  shipScheduler.loadDemoManifest();
  shipDisplay.render(shipScheduler);

  Serial.println("FCFS listo. Escribe 'help' para comandos.");
}

void loop() {
  processSerialInput(shipScheduler);
  shipScheduler.update();
  shipDisplay.renderIfNeeded(shipScheduler);
}