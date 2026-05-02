#include "ShipDisplay.h" // API publica.
#include "ShipScheduler.h" // Tipos de barco.

// Variable global privada: almacena ID del barco renderizado anteriormente.
static uint8_t gPrevRenderedBoatId = 0;

// Mapea progreso temporal a posicion en pixeles.
int16_t ship_display_map_progress(unsigned long elapsed, unsigned long total, int16_t from, int16_t to) {
  if (total == 0) {
    return to; // Evita division por cero.
  }
  long delta = (long)to - (long)from; // Distancia en pixeles.
  return from + (int16_t)((delta * (long)elapsed) / (long)total); // Mapeo lineal.
}

// Calcula la posicion y datos del barco activo para renderizado.
BoatRenderData ship_display_calculate_active_boat_position(const Boat *boat, unsigned long elapsed, int16_t canalX, int16_t canalW, int16_t canalY, int16_t canalH, int16_t boatSize) {
  BoatRenderData data = {0};
  
  if (!boat) {
    data.isNewBoat = (gPrevRenderedBoatId != 0); // Indica que habia barco antes.
    gPrevRenderedBoatId = 0;
    return data;
  }

  // Calcula X segun el lado de origen.
  int16_t travelStart = boat->origin == SIDE_RIGHT ? canalX + canalW - boatSize - 2 : canalX + 2;
  int16_t travelEnd = boat->origin == SIDE_RIGHT ? canalX + 2 : canalX + canalW - boatSize - 2;
  int16_t boatX = ship_display_map_progress(elapsed, boat->serviceMillis, travelStart, travelEnd);

  // Aplica limites.
  int16_t minX = canalX + 2;
  int16_t maxX = canalX + canalW - boatSize - 2;
  if (boatX < minX) boatX = minX;
  if (boatX > maxX) boatX = maxX;

  // Calcula Y centrado.
  int16_t boatY = canalY + (canalH / 2) - (boatSize / 2);

  data.boatX = boatX;
  data.boatY = boatY;
  data.boatWidth = boatSize;
  data.boatHeight = boatSize;
  data.isNewBoat = (gPrevRenderedBoatId != boat->id); // True si es un barco distinto.

  gPrevRenderedBoatId = boat->id; // Actualiza el ID anterior.
  return data;
}

// Retorna la cantidad visible de barcos en la cola (maximo 3).
uint8_t ship_display_get_visible_waiting_count(uint8_t totalWaiting) {
  return totalWaiting > 3 ? 3 : totalWaiting;
}

// Invalida el cache de barco renderizado (se llama al limpiar la pantalla).
void ship_display_invalidate_boat_cache(void) {
  gPrevRenderedBoatId = 0;
}
