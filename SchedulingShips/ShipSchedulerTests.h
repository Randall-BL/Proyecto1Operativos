#pragma once // Evita inclusiones duplicadas. 

#ifdef __cplusplus // Habilita linkage C para C++. 
extern "C" { // Inicio del bloque con nombres C. 
#endif // Fin de compatibilidad C++. 

#include "ShipScheduler.h" // Tipos del scheduler para pruebas. 

void run_scheduler_tests(ShipScheduler *scheduler); // Ejecuta la bateria completa. 
void run_scheduler_test(ShipScheduler *scheduler, ShipAlgo algo); // Ejecuta una prueba puntual. 

#ifdef __cplusplus // Cierra el bloque de linkage C. 
} // Fin de extern "C". 
#endif // Fin de compatibilidad C++. 
