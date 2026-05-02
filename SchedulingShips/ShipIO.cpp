#include "ShipIO.h" // Declaraciones de logging. 

#include <Arduino.h> // Acceso a Serial en C++. 
#include <stdarg.h> // Manejo de argumentos variables. 
#include <stdio.h> // vsnprintf para formateo. 

void ship_log(const char *text) { // Imprime texto simple. 
  if (text) { // Solo si el puntero es valido. 
    Serial.print(text); // Envia a Serial sin salto. 
  } // Fin del if. 
} // Fin de ship_log. 

void ship_logln(const char *text) { // Imprime texto con salto. 
  if (text) { // Si hay texto. 
    Serial.println(text); // Imprime con salto. 
  } else { // Si no hay texto. 
    Serial.println(); // Imprime linea vacia. 
  } // Fin del if. 
} // Fin de ship_logln. 

void ship_logf(const char *format, ...) { // Imprime formato con printf. 
  char buffer[192]; // Buffer temporal para el mensaje. 
  va_list args; // Lista de argumentos variables. 
  va_start(args, format); // Inicializa la lista. 
  vsnprintf(buffer, sizeof(buffer), format, args); // Formatea en el buffer. 
  va_end(args); // Cierra la lista. 
  Serial.print(buffer); // Imprime el buffer en Serial. 
} // Fin de ship_logf. 
