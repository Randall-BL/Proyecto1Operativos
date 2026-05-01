#include "ShipCommands.h" // Declaraciones del parser. 

#include <stdbool.h> // Booleanos en C. 
#include <stddef.h> // size_t y NULL. 
#include <stdio.h> // strtoul/strtol y utilidades. 
#include <string.h> // strlen, strcmp, strncpy. 

#include "ShipIO.h" // Logging por Serial. 
#include "ShipSchedulerTests.h" // Pruebas del scheduler. 

static bool starts_with(const char *text, const char *prefix) { // Verifica prefijo. 
  return text && prefix && strncmp(text, prefix, strlen(prefix)) == 0; // Retorna si coincide. 
} // Fin de starts_with. 

static void trim_left(char **text) { // Elimina espacios al inicio. 
  if (!text || !*text) return; // Valida el puntero. 
  while (**text == ' ' || **text == '\t') (*text)++; // Avanza mientras haya espacios. 
} // Fin de trim_left. 

static void trim_right(char *text) { // Elimina espacios al final. 
  if (!text) return; // Valida el puntero. 
  size_t len = strlen(text); // Longitud actual. 
  while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t' || text[len - 1] == '\r' || text[len - 1] == '\n')) { // Mientras haya espacios. 
    text[len - 1] = '\0'; // Corta el caracter final. 
    len--; // Reduce la longitud. 
  } // Fin del while. 
} // Fin de trim_right. 

static void handle_command(ShipScheduler *scheduler, char *command) { // Procesa una linea normalizada. 
  if (!scheduler || !command) return; // Valida punteros. 

  trim_right(command); // Limpia espacios al final. 
  char *cursor = command; // Puntero de trabajo. 
  trim_left(&cursor); // Limpia espacios al inicio. 

  if (strcmp(cursor, "help") == 0) { // Comando help. 
    ship_logln("Comandos: demo | clear | add <l|r> <n|p|u> [prio]"); // Lista comandos base. 
    ship_logln("          alg <fcfs|sjf|strn|edf|rr|prio> [ms]"); // Lista alg. 
    ship_logln("          pause | resume | status | test [all|rr|prio|fcfs|sjf|strn|edf]"); // Lista tests. 
    return; // Termina. 
  } 

  if (strcmp(cursor, "demo") == 0) { // Comando demo. 
    ship_scheduler_load_demo_manifest(scheduler); // Carga manifest demo. 
    return; // Termina. 
  } 

  if (starts_with(cursor, "alg ")) { // Comando alg. 
    char *arg = cursor + 4; // Salta el prefijo. 
    trim_left(&arg); // Limpia espacios. 
    char *spaceAt = strchr(arg, ' '); // Busca separador. 
    char *name = arg; // Nombre del algoritmo. 
    char *value = ""; // Valor opcional. 
    if (spaceAt) { // Si hay valor. 
      *spaceAt = '\0'; // Corta el nombre. 
      value = spaceAt + 1; // Apunta al valor. 
      trim_left(&value); // Limpia el valor. 
    } 

    if (strcmp(name, "fcfs") == 0) { // Algoritmo FCFS. 
      ship_scheduler_set_algorithm(scheduler, ALG_FCFS); // Cambia algoritmo. 
      ship_logln("Algoritmo: FCFS"); // Confirma. 
    } else if (strcmp(name, "sjf") == 0) { // Algoritmo SJF. 
      ship_scheduler_set_algorithm(scheduler, ALG_SJF); // Cambia algoritmo. 
      ship_logln("Algoritmo: SJF (Shortest Job First)"); // Confirma. 
    } else if (strcmp(name, "strn") == 0) { // Algoritmo STRN. 
      ship_scheduler_set_algorithm(scheduler, ALG_STRN); // Cambia algoritmo. 
      ship_logln("Algoritmo: STRN (Shortest Remaining Time)"); // Confirma. 
    } else if (strcmp(name, "edf") == 0) { // Algoritmo EDF. 
      ship_scheduler_set_algorithm(scheduler, ALG_EDF); // Cambia algoritmo. 
      ship_logln("Algoritmo: EDF (Earliest Deadline First)"); // Confirma. 
    } else if (strcmp(name, "rr") == 0) { // Algoritmo RR. 
      ship_scheduler_set_algorithm(scheduler, ALG_RR); // Cambia algoritmo. 
      if (value[0] != '\0') { // Si hay quantum. 
        unsigned long quantum = strtoul(value, NULL, 10); // Convierte a numero. 
        if (quantum > 0) ship_scheduler_set_round_robin_quantum(scheduler, quantum); // Aplica quantum. 
      } 
      ship_logf("Algoritmo: RR q=%lums\n", ship_scheduler_get_round_robin_quantum(scheduler)); // Imprime quantum. 
    } else if (strcmp(name, "prio") == 0 || strcmp(name, "prioridad") == 0 || strcmp(name, "priority") == 0) { // Algoritmo prioridad. 
      ship_scheduler_set_algorithm(scheduler, ALG_PRIORITY); // Cambia algoritmo. 
      ship_logln("Algoritmo: Prioridad"); // Confirma. 
    } else { // Algoritmo desconocido. 
      ship_logln("Uso: alg <fcfs|sjf|strn|edf|rr|prio> [ms]"); // Muestra ayuda. 
    } 
    return; // Termina. 
  } 

  if (strcmp(cursor, "clear") == 0) { // Comando clear. 
    ship_scheduler_clear(scheduler); // Limpia el scheduler. 
    ship_logln("Colas limpiadas."); // Confirma. 
    return; // Termina. 
  } 

  if (strcmp(cursor, "pause") == 0) { // Comando pause. 
    ship_scheduler_pause_active(scheduler); // Pausa el barco activo. 
    return; // Termina. 
  } 

  if (strcmp(cursor, "resume") == 0) { // Comando resume. 
    ship_scheduler_resume_active(scheduler); // Reanuda el barco activo. 
    return; // Termina. 
  } 

  if (strcmp(cursor, "status") == 0) { // Comando status. 
    ship_scheduler_dump_status(scheduler); // Imprime el estado. 
    return; // Termina. 
  } 

  if (starts_with(cursor, "test")) { // Comando test. 
    char *arg = cursor + 4; // Apunta al argumento. 
    trim_left(&arg); // Limpia espacios. 
    if (arg[0] == '\0' || strcmp(arg, "all") == 0) { // Sin argumento o all. 
      run_scheduler_tests(scheduler); // Ejecuta todas las pruebas. 
    } else if (strcmp(arg, "rr") == 0) { // Prueba RR. 
      run_scheduler_test(scheduler, ALG_RR); // Ejecuta RR. 
    } else if (strcmp(arg, "prio") == 0 || strcmp(arg, "prioridad") == 0 || strcmp(arg, "priority") == 0) { // Prueba prioridad. 
      run_scheduler_test(scheduler, ALG_PRIORITY); // Ejecuta prioridad. 
    } else if (strcmp(arg, "fcfs") == 0) { // Prueba FCFS. 
      run_scheduler_test(scheduler, ALG_FCFS); // Ejecuta FCFS. 
    } else if (strcmp(arg, "sjf") == 0) { // Prueba SJF. 
      run_scheduler_test(scheduler, ALG_SJF); // Ejecuta SJF. 
    } else if (strcmp(arg, "strn") == 0) { // Prueba STRN. 
      run_scheduler_test(scheduler, ALG_STRN); // Ejecuta STRN. 
    } else if (strcmp(arg, "edf") == 0) { // Prueba EDF. 
      run_scheduler_test(scheduler, ALG_EDF); // Ejecuta EDF. 
    } else { // Caso desconocido. 
      ship_logln("Uso: test [all|rr|prio|fcfs|sjf|strn|edf]"); // Muestra ayuda. 
    } 
    return; // Termina. 
  } 

  if (starts_with(cursor, "add ")) { // Comando add. 
    char *first = strchr(cursor, ' '); // Busca el primer espacio. 
    if (!first) { // Si falta. 
      ship_logln("Formato: add <l|r> <n|p|u> [prio]"); // Ayuda. 
      return; // Termina. 
    } 
    first++; // Avanza al token. 
    trim_left(&first); // Limpia espacios. 
    char *second = strchr(first, ' '); // Busca el segundo espacio. 
    if (!second) { // Si falta. 
      ship_logln("Formato: add <l|r> <n|p|u> [prio]"); // Ayuda. 
      return; // Termina. 
    } 
    *second = '\0'; // Corta el primer token. 
    char *third = second + 1; // Apunta al tercer token. 
    trim_left(&third); // Limpia espacios. 
    char *fourth = strchr(third, ' '); // Busca el cuarto token. 
    char *prioToken = NULL; // Token de prioridad opcional. 
    if (fourth) { // Si hay prioridad. 
      *fourth = '\0'; // Corta el token de tipo. 
      prioToken = fourth + 1; // Apunta a prioridad. 
      trim_left(&prioToken); // Limpia prioridad. 
    } 

    BoatSide side = (first[0] == 'l' || first[0] == 'L') ? SIDE_LEFT : SIDE_RIGHT; // Determina el lado. 
    BoatType type = BOAT_NORMAL; // Tipo por defecto. 
    if (third[0] == 'p' || third[0] == 'P') { // Si es pesquera. 
      type = BOAT_PESQUERA; // Asigna pesquera. 
    } else if (third[0] == 'u' || third[0] == 'U' || third[0] == 'r' || third[0] == 'R') { // Si es patrulla. 
      type = BOAT_PATRULLA; // Asigna patrulla. 
    } 

    if (prioToken && prioToken[0] != '\0') { // Si hay prioridad. 
      long prioValue = strtol(prioToken, NULL, 10); // Convierte a entero. 
      if (prioValue < 1) prioValue = 1; // Limita minimo. 
      if (prioValue > 9) prioValue = 9; // Limita maximo. 
      ship_scheduler_enqueue(scheduler, createBoatWithPriority(side, type, (uint8_t)prioValue)); // Encola con prioridad. 
    } else { // Si no hay prioridad. 
      ship_scheduler_enqueue(scheduler, createBoat(side, type)); // Encola con prioridad base. 
    } 
    // El detalle del alta (id, tipo y origen) se imprime dentro del scheduler.
    return; // Termina. 
  } 

  ship_logln("Comando no reconocido. Use help."); // Mensaje por defecto. 
} // Fin de handle_command. 

void process_serial_command(ShipScheduler *scheduler, const char *command) { // Entrada publica del parser. 
  if (!scheduler || !command) return; // Valida punteros. 
  char buffer[160]; // Buffer local para la linea. 
  strncpy(buffer, command, sizeof(buffer) - 1); // Copia segura. 
  buffer[sizeof(buffer) - 1] = '\0'; // Asegura fin de cadena. 
  handle_command(scheduler, buffer); // Procesa la linea. 
} // Fin de process_serial_command. 
