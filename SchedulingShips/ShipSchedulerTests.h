#pragma once

#include "ShipScheduler.h"

class ShipDisplay;

// Permite adjuntar una pantalla para visualizar pruebas (opcional).
void setTestDisplay(ShipDisplay *display);
// Ejecuta la bateria completa de pruebas para todos los algoritmos.
void runSchedulerTests(ShipScheduler &scheduler);
// Ejecuta una prueba de un algoritmo especifico.
void runSchedulerTest(ShipScheduler &scheduler, ShipScheduler::Algo algo);
