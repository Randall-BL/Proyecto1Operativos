#pragma once // Evita inclusiones duplicadas. 

#include <stdarg.h> // Necesario para listas de argumentos. 

#ifdef __cplusplus // Habilita linkage C para C++. 
extern "C" { // Inicio del bloque con nombres C. 
#endif // Fin de compatibilidad C++. 

void ship_log(const char *text); // Imprime texto sin salto de linea. 
void ship_logln(const char *text); // Imprime texto con salto de linea. 
void ship_logf(const char *format, ...); // Imprime texto formateado. 

#ifdef __cplusplus // Cierra el bloque de linkage C. 
} // Fin de extern "C". 
#endif // Fin de compatibilidad C++. 
