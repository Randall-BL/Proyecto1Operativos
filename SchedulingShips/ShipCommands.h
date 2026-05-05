// API publica para parsear comandos seriales.
#pragma once // Evita inclusiones duplicadas. 

#ifdef __cplusplus // Habilita linkage C para C++. 
extern "C" { // Inicio del bloque con nombres C. 
#endif // Fin de compatibilidad C++. 

#include "ShipScheduler.h" // Necesario para ShipScheduler. 

void process_serial_command(ShipScheduler *scheduler, const char *command); // Procesa una linea de comando. 

#ifdef __cplusplus // Cierra el bloque de linkage C. 
} // Fin de extern "C". 
#endif // Fin de compatibilidad C++. 