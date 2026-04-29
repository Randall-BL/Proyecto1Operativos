#pragma once

#include "ShipPins.h"
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

#include "ShipScheduler.h"

// Encargado de renderizar la interfaz en la pantalla TFT.
class ShipDisplay {
public:
  // Inicializa la pantalla TFT y deja el fondo listo.
  void begin();
  // Renderiza un cuadro completo de la interfaz.
  void render(const ShipScheduler &scheduler);
  // Renderiza solo si paso el tiempo de refresco configurado.
  void renderIfNeeded(const ShipScheduler &scheduler);

private:
  // Dibuja el fondo estatico segun el algoritmo actual.
  void drawStaticLayout(const char *algoLabel);
  // Dibuja el icono de un barco con o sin resaltado.
  void drawBoatSquare(int16_t x, int16_t y, const Boat &boat, bool highlight = false);
  // Dibuja la cola de espera para un lado del canal.
  void drawWaitingSide(const ShipScheduler &scheduler, BoatSide side);
  // Dibuja el barco activo dentro del canal.
  void drawActiveBoat(const ShipScheduler &scheduler);
  // Dibuja las estadisticas del pie de pantalla.
  void drawStatistics(const ShipScheduler &scheduler);

  Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);
  unsigned long lastUiRefresh = 0;
  bool layoutDrawn = false;
  ShipScheduler::Algo lastAlgorithm = ShipScheduler::ALG_FCFS;
};