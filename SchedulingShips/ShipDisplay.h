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
// Renderiza forzadamente sin respetar el limite de refresco (para eventos criticos como emergencias).
void ship_display_render_forced(const ShipScheduler *scheduler); // Fuerza dibujo inmediato ignorando el limite de refresco.

// Funciones de logica (calculo), no renderizacion.
int16_t ship_display_map_progress(unsigned long elapsed, unsigned long total, int16_t from, int16_t to);
BoatRenderData ship_display_calculate_active_boat_position(const Boat *boat, unsigned long elapsed, int16_t canalX, int16_t canalW, int16_t canalY, int16_t canalH, int16_t boatSize);
uint8_t ship_display_get_visible_waiting_count(uint8_t totalWaiting);
void ship_display_invalidate_boat_cache(void);

// Envoltorios de la parte fisica de la TFT (implementados en ShipDisplay.cpp).
void ship_display_hw_begin(void); // Inicializa SPI y el controlador de la TFT.
void ship_display_hw_fill_screen(uint16_t color); // Limpia la pantalla con un color.
void ship_display_hw_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color); // Rellena un rectangulo.
void ship_display_hw_draw_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color); // Dibuja el borde de un rectangulo.
void ship_display_hw_set_text_wrap(bool wrap); // Habilita o deshabilita el ajuste de texto.
void ship_display_hw_set_text_size(uint8_t size); // Define el tamano de la fuente.
void ship_display_hw_set_text_color(uint16_t color, uint16_t bg); // Define color de texto y fondo.
void ship_display_hw_set_cursor(int16_t x, int16_t y); // Posiciona el cursor de texto.
void ship_display_hw_print_str(const char *text); // Imprime una cadena.
void ship_display_hw_print_char(char value); // Imprime un caracter.
void ship_display_hw_print_uint(unsigned long value); // Imprime un entero sin signo.

#ifdef __cplusplus // Cierra el bloque de linkage C. 
} // Fin de extern "C". 
#endif // Fin de compatibilidad C++. 