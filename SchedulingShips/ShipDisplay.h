#pragma once // Evita inclusiones duplicadas del header. 
// API C para la pantalla; la implementacion usa C++ internamente. 

#ifdef __cplusplus // Habilita linkage C cuando se incluye desde C++. 
extern "C" { // Inicio del bloque con nombres C. 
#endif // Fin de la directiva de compatibilidad C++. 

#include "ShipScheduler.h" // Tipos y funciones del scheduler. 

// Inicializa la pantalla TFT y deja la interfaz lista. 
void ship_display_begin(void); // Arranque de la pantalla y del estado interno. 
// Renderiza el cuadro completo de la interfaz. 
void ship_display_render(const ShipScheduler *scheduler); // Dibujo completo con el estado actual. 
// Renderiza solo si se cumple el tiempo de refresco. 
void ship_display_render_if_needed(const ShipScheduler *scheduler); // Dibujo limitado por UI_REFRESH_MS. 

#ifdef __cplusplus // Cierra el bloque de linkage C. 
} // Fin de extern "C". 
#endif // Fin de compatibilidad C++. 