#pragma once // Evita inclusiones duplicadas del header. 
// API C para la pantalla; la implementacion usa C++ internamente. 

#ifdef __cplusplus // Habilita linkage C cuando se incluye desde C++. 
extern "C" { // Inicio del bloque con nombres C. 
#endif // Fin de la directiva de compatibilidad C++. 

#include "ShipScheduler.h" // Tipos y funciones del scheduler. 

// Estructura para datos de renderizacion del barco activo.
typedef struct {
  int16_t boatX;
  int16_t boatY;
  uint8_t boatWidth;
  uint8_t boatHeight;
  bool isNewBoat; // True si es diferente al anterior.
} BoatRenderData;

// Inicializa la pantalla TFT y deja la interfaz lista. 
void ship_display_begin(void); // Arranque de la pantalla y del estado interno. 
// Renderiza el cuadro completo de la interfaz. 
void ship_display_render(const ShipScheduler *scheduler); // Dibujo completo con el estado actual. 
// Renderiza solo si se cumple el tiempo de refresco. 
void ship_display_render_if_needed(const ShipScheduler *scheduler); // Dibujo limitado por UI_REFRESH_MS. 
// Renderiza forzadamente sin respetar el rate limit (para eventos críticos como emergencias).
void ship_display_render_forced(const ShipScheduler *scheduler); // Fuerza render inmediato ignorando rate limit.

// Funciones de logica (calculo), no renderizacion.
int16_t ship_display_map_progress(unsigned long elapsed, unsigned long total, int16_t from, int16_t to);
BoatRenderData ship_display_calculate_active_boat_position(const Boat *boat, unsigned long elapsed, int16_t canalX, int16_t canalW, int16_t canalY, int16_t canalH, int16_t boatSize);
uint8_t ship_display_get_visible_waiting_count(uint8_t totalWaiting);
void ship_display_invalidate_boat_cache(void);

#ifdef __cplusplus // Cierra el bloque de linkage C. 
} // Fin de extern "C". 
#endif // Fin de compatibilidad C++. 