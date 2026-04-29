#pragma once

#include <Arduino.h>

#include "ShipScheduler.h"

// Lee una linea desde Serial y la despacha al scheduler como comando.
void processSerialInput(ShipScheduler &scheduler);