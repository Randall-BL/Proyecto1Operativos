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
    ship_logln("          flow <tico|fair|sign> | w <n> | sign <l|r> | signms <ms>"); // Lista flujo.
    ship_logln("          flowlog <on|off>"); // Trazas de flujo.
    ship_logln("          sensor <activate|deactivate|threshold|simulate> [param]"); // Sensor.
    ship_logln("          emergency <clear>"); // Emergencias.
    ship_logln("          chanlen <m> | boatspeed <mps> | readymax <n>"); // Lista canal.
    ship_logln("          pause | resume | status | test [all|rr|prio|fcfs|sjf|strn|edf|flow]"); // Lista tests. 
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

  if (starts_with(cursor, "flow ")) { // Comando flow.
    char *arg = cursor + 5; // Salta el prefijo.
    trim_left(&arg); // Limpia espacios.
    if (strcmp(arg, "tico") == 0) { // Modo tico.
      ship_scheduler_set_flow_mode(scheduler, FLOW_TICO); // Configura tico.
      ship_logln("Flujo: TICO (sin control de turno)"); // Confirma.
    } else if (strcmp(arg, "fair") == 0 || strcmp(arg, "equidad") == 0) { // Modo equidad.
      ship_scheduler_set_flow_mode(scheduler, FLOW_FAIRNESS); // Configura equidad.
      ship_logf("Flujo: EQUIDAD W=%u\n", ship_scheduler_get_fairness_window(scheduler)); // Confirma.
    } else if (strcmp(arg, "sign") == 0 || strcmp(arg, "letrero") == 0) { // Modo letrero.
      ship_scheduler_set_flow_mode(scheduler, FLOW_SIGN); // Configura letrero.
      ship_logf("Flujo: LETRERO (%s cada %lums)\n", boatSideName(ship_scheduler_get_sign_direction(scheduler)), ship_scheduler_get_sign_interval(scheduler)); // Confirma.
    } else { // Opcion invalida.
      ship_logln("Uso: flow <tico|fair|sign>"); // Ayuda.
    }
    return; // Termina.
  }

  if (starts_with(cursor, "flowlog ")) { // Comando flowlog.
    char *arg = cursor + 8; // Salta prefijo.
    trim_left(&arg); // Limpia espacios.
    bool enabled = !(strcmp(arg, "off") == 0 || strcmp(arg, "0") == 0); // Interpreta valor.
    ship_scheduler_set_flow_logging(scheduler, enabled); // Aplica trazas.
    ship_logf("FlowLog: %s\n", ship_scheduler_get_flow_logging(scheduler) ? "ON" : "OFF"); // Confirma estado.
    return; // Termina.
  }

  if (starts_with(cursor, "sensor ")) { // Comando sensor de proximidad.
    char *arg = cursor + 7; // Salta prefijo.
    trim_left(&arg); // Limpia espacios.
    char *spaceAt = strchr(arg, ' '); // Busca separador.
    char *subcommand = arg; // Subcomando.
    char *param = ""; // Parametro opcional.
    if (spaceAt) { // Si hay parametro.
      *spaceAt = '\0'; // Corta el subcomando.
      param = spaceAt + 1; // Apunta al parametro.
      trim_left(&param); // Limpia el parametro.
    }
    
    if (strcmp(subcommand, "activate") == 0) { // Activa sensor.
      ship_scheduler_set_sensor_enabled(scheduler, true); // Habilita sensor.
      ship_logln("[SENSOR] Sensor ACTIVADO"); // Confirma.
    } else if (strcmp(subcommand, "deactivate") == 0) { // Desactiva sensor.
      ship_scheduler_set_sensor_enabled(scheduler, false); // Deshabilita sensor.
      ship_logln("[SENSOR] Sensor DESACTIVADO"); // Confirma.
    } else if (strcmp(subcommand, "threshold") == 0) { // Configura umbral.
      unsigned long thresholdCm = strtoul(param, NULL, 10); // Convierte a numero.
      if (thresholdCm == 0) { // Si es cero.
        ship_logln("Uso: sensor threshold <cm>"); // Muestra ayuda.
      } else {
        ship_scheduler_set_proximity_threshold(scheduler, (uint16_t)thresholdCm); // Aplica umbral.
      }
    } else if (strcmp(subcommand, "simulate") == 0) { // Simula distancia.
      unsigned long distanceCm = strtoul(param, NULL, 10); // Convierte a numero.
      if (param[0] == '\0') { // Si no hay parametro.
        ship_logln("Uso: sensor simulate <cm>"); // Muestra ayuda.
      } else {
        ship_logf("[SENSOR] Simulando distancia: %lu cm\n", distanceCm); // Aviso.
        ship_scheduler_set_proximity_distance_simulated(scheduler, (uint16_t)distanceCm); // Simula distancia.
      }
    } else { // Subcomando desconocido.
      ship_logln("Uso: sensor <activate|deactivate|threshold|simulate> [param]"); // Ayuda.
    }
    return; // Termina.
  }

  if (starts_with(cursor, "emergency ")) { // Comando control de emergencia.
    char *arg = cursor + 10; // Salta prefijo.
    trim_left(&arg); // Limpia espacios.
    if (strcmp(arg, "clear") == 0) { // Limpia emergencia.
      ship_scheduler_clear_emergency(scheduler); // Limpia estado.
      ship_logln("[EMERGENCY] Emergencia CANCELADA"); // Confirma.
    } else { // Opcion invalida.
      ship_logln("Uso: emergency <clear>"); // Ayuda.
    }
    return; // Termina.
  }

  if (starts_with(cursor, "w ")) { // Comando parametro W.
    char *arg = cursor + 2; // Valor de W.
    trim_left(&arg); // Limpia espacios.
    unsigned long value = strtoul(arg, NULL, 10); // Convierte a entero.
    if (value == 0) value = 1; // Fuerza minimo.
    if (value > 255) value = 255; // Limita maximo.
    ship_scheduler_set_fairness_window(scheduler, (uint8_t)value); // Aplica W.
    ship_logf("Equidad W=%u\n", ship_scheduler_get_fairness_window(scheduler)); // Confirma.
    return; // Termina.
  }

  if (starts_with(cursor, "sign ")) { // Comando direccion de letrero.
    char *arg = cursor + 5; // Valor de direccion.
    trim_left(&arg); // Limpia espacios.
    BoatSide side = (arg[0] == 'r' || arg[0] == 'R' || arg[0] == 'd' || arg[0] == 'D') ? SIDE_RIGHT : SIDE_LEFT; // Parsea lado.
    ship_scheduler_set_sign_direction(scheduler, side); // Aplica lado.
    ship_logf("Letrero en %s\n", boatSideName(ship_scheduler_get_sign_direction(scheduler))); // Confirma.
    return; // Termina.
  }

  if (starts_with(cursor, "signms ")) { // Comando intervalo de letrero.
    char *arg = cursor + 7; // Valor de milisegundos.
    trim_left(&arg); // Limpia espacios.
    unsigned long value = strtoul(arg, NULL, 10); // Convierte a entero.
    ship_scheduler_set_sign_interval(scheduler, value); // Aplica intervalo.
    ship_logf("Letrero cada %lums\n", ship_scheduler_get_sign_interval(scheduler)); // Confirma.
    return; // Termina.
  }

  if (starts_with(cursor, "chanlen ")) { // Comando largo de canal.
    char *arg = cursor + 8; // Valor de metros.
    trim_left(&arg); // Limpia espacios.
    unsigned long value = strtoul(arg, NULL, 10); // Convierte a entero.
    if (value == 0) value = 1; // Fuerza minimo.
    if (value > 65535UL) value = 65535UL; // Limita maximo uint16_t.
    ship_scheduler_set_channel_length(scheduler, (uint16_t)value); // Aplica largo.
    ship_logf("Canal=%um\n", ship_scheduler_get_channel_length(scheduler)); // Confirma.
    return; // Termina.
  }

  if (starts_with(cursor, "boatspeed ")) { // Comando velocidad de barco.
    char *arg = cursor + 10; // Valor de m/s.
    trim_left(&arg); // Limpia espacios.
    unsigned long value = strtoul(arg, NULL, 10); // Convierte a entero.
    if (value == 0) value = 1; // Fuerza minimo.
    if (value > 65535UL) value = 65535UL; // Limita maximo uint16_t.
    ship_scheduler_set_boat_speed(scheduler, (uint16_t)value); // Aplica velocidad.
    ship_logf("Velocidad base=%um/s\n", ship_scheduler_get_boat_speed(scheduler)); // Confirma.
    return; // Termina.
  }

  if (starts_with(cursor, "readymax ")) { // Comando limite de cola.
    char *arg = cursor + 9; // Valor de limite.
    trim_left(&arg); // Limpia espacios.
    unsigned long value = strtoul(arg, NULL, 10); // Convierte a entero.
    if (value == 0) value = 1; // Fuerza minimo.
    if (value > MAX_BOATS) value = MAX_BOATS; // Limita maximo permitido.
    ship_scheduler_set_max_ready_queue(scheduler, (uint8_t)value); // Aplica limite.
    ship_logf("Cola maxima=%u\n", ship_scheduler_get_max_ready_queue(scheduler)); // Confirma.
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
    } else if (strcmp(arg, "flow") == 0) { // Pruebas de control de flujo.
      run_flow_control_tests(scheduler); // Ejecuta bateria de flujo.
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
      ship_logln("Uso: test [all|rr|prio|fcfs|sjf|strn|edf|flow]"); // Muestra ayuda. 
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
