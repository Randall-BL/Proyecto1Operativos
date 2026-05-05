// Helpers de logging serial con callbacks opcionales.
#include "ShipIO.h" // Declaraciones de logging.

#include <stdio.h> // puts, fputs.
#include <stdarg.h> // Manejo de argumentos variables.

static ship_io_writer_fn gWriteFn = NULL; // Callback para salida sin salto.
static ship_io_writer_fn gWriteLnFn = NULL; // Callback para salida con salto.

// Registra callbacks de escritura para enrutar la salida de logs.
void ship_io_set_writers(ship_io_writer_fn writeFn, ship_io_writer_fn writelnFn) { // Configura callbacks de salida.
  gWriteFn = writeFn;
  gWriteLnFn = writelnFn;
}

// Escribe una cadena sin forzar salto de linea.
void ship_log(const char *text) { // Imprime texto simple.
  if (!text) return;

  if (gWriteFn) {
    gWriteFn(text);
    return;
  }

  fputs(text, stdout); // Alternativa cuando no hay puente registrado.
}

// Escribe una cadena y agrega salto de linea.
void ship_logln(const char *text) { // Imprime texto con salto.
  if (gWriteLnFn) {
    gWriteLnFn(text);
    return;
  }

  if (text) {
    puts(text);
  } else {
    putchar('\n');
  }
}

// Formatea y escribe un mensaje con argumentos tipo printf.
void ship_logf(const char *format, ...) { // Imprime con formato.
  char buffer[192];
  va_list args;

  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);

  ship_log(buffer);
}
