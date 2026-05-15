#include "ShipScheduler.h" // API del scheduler.
#include "ShipIO.h" // Logging por Serial.
/*
 * ShipSchedulerConfig.c — Getters y setters de configuracion del planificador.
 *
 * PROPOSITO DE ESTE MODULO:
 *   Contiene EXCLUSIVAMENTE funciones de acceso (getters y setters) a los campos
 *   de configuracion del struct ShipScheduler. El struct vive en el HEAP de C:
 *   fue creado con malloc() por el codigo cliente y su puntero se pasa a cada
 *   funcion como ShipScheduler *scheduler.
 *
 * POR QUE VALIDAR scheduler != NULL EN CADA FUNCION:
 *   El struct esta en el heap. Si el puntero es NULL (malloc fallo o fue liberado),
 *   cualquier acceso a scheduler->campo provocaria un HARDFAULT en el ESP32
 *   (acceso a direccion 0x00000000 en la memoria de datos). El guard
 *     if (!scheduler) return;
 *   cortocircuita la ejecucion antes de desreferenciar el puntero.
 *
 * LAYOUT EN MEMORIA:
 *   Cada campo de ShipScheduler es accesible como:
 *     *(scheduler + offsetof(ShipScheduler, campo))
 *   o equivalentemente scheduler->campo. El compilador calcula el offset de cada
 *   campo automaticamente segun el orden de declaracion en ShipScheduler.h y el
 *   alineamiento del procesador (Xtensa LX6, 32 bits, alinea uint32_t a 4 bytes).
 *
 * CONVENCION DE NOMBRES:
 *   ship_scheduler_set_X : escribe scheduler->X con validacion de rango.
 *   ship_scheduler_get_X : lee scheduler->X; retorna valor por defecto si NULL.
 */
/*
 * ship_scheduler_set_algorithm / get_algorithm / get_algorithm_label
 *
 * scheduler->algorithm es un campo de tipo ShipAlgo (enum, 1 byte alineado a 4).
 * Determina el criterio de seleccion en findIndexForAlgoAndSide:
 *   ALG_FCFS    : menor arrivalOrder (uint32_t, orden de llegada).
 *   ALG_SJF     : menor serviceMillis (unsigned long, tiempo estimado total).
 *   ALG_STRN    : menor remainingMillis (tiempo restante en el canal).
 *   ALG_EDF     : menor (deadlineMillis - now); urgencia del plazo absoluto.
 *   ALG_RR      : quantum de tiempo fijo; rotacion circular entre activos.
 *   ALG_PRIORITY: mayor campo 'priority' (uint8_t; 255 = mayor prioridad).
 *
 * get_algorithm_label retorna un literal string en memoria flash (ROM del ESP32),
 * no en el heap; el puntero es valido para siempre y no debe ser liberado.
 */
void ship_scheduler_set_algorithm(ShipScheduler *scheduler, ShipAlgo algo) {
  if (!scheduler) return;
  scheduler->algorithm = algo;
}

ShipAlgo ship_scheduler_get_algorithm(const ShipScheduler *scheduler) {
  if (!scheduler) return ALG_FCFS;
  return scheduler->algorithm;
}

const char *ship_scheduler_get_algorithm_label(const ShipScheduler *scheduler) {
  if (!scheduler) return "?";
  switch (scheduler->algorithm) {
    case ALG_FCFS: return "FCFS";
    case ALG_SJF: return "SJF";
    case ALG_STRN: return "STRN";
    case ALG_EDF: return "EDF";
    case ALG_RR: return "RR";
    case ALG_PRIORITY: return "PRIO";
  }
  return "?";
}

/*
 * ship_scheduler_set_round_robin_quantum / get_round_robin_quantum
 *
 * scheduler->rrQuantumMillis (unsigned long): duracion en milisegundos del
 * quantum de tiempo asignado a cada barco en Round Robin.
 *
 * El quantum minimo es 100ms para evitar que los barcos se preempten tan
 * frecuentemente que nunca avancen en el canal (el tick del loop principal es
 * ~50ms; un quantum de 50ms causaria preempcion en cada tick).
 *
 * Este valor es consultado en:
 *   - preempt_active_for_rr: elapsed >= rrQuantumMillis -> preemptar.
 *   - start_next_boat (RR): elapsed < rrQuantumMillis -> no despachar todavia.
 * Donde elapsed = accumulated + (now - startedAt).
 */
void ship_scheduler_set_round_robin_quantum(ShipScheduler *scheduler, unsigned long quantumMillis) {
  if (!scheduler) return;
  if (quantumMillis < 100) quantumMillis = 100;
  scheduler->rrQuantumMillis = quantumMillis;
}

unsigned long ship_scheduler_get_round_robin_quantum(const ShipScheduler *scheduler) {
  if (!scheduler) return 0;
  return scheduler->rrQuantumMillis;
}

/*
 * ship_scheduler_set_flow_mode / get_flow_mode / get_flow_mode_label
 *
 * scheduler->flowMode (ShipFlowMode enum) controla QUIEN puede entrar al canal:
 *   FLOW_TICO    : ambos lados compiten libremente por el canal; el algoritmo
 *                  de scheduling elige al mejor sin restriccion de sentido.
 *   FLOW_FAIRNESS: ventana W — los primeros W barcos del lado actual pasan;
 *                  al completarse la ventana, se alterna al lado opuesto.
 *                  Se alterna antes si el lado actual queda vacio.
 *   FLOW_SIGN    : solo el lado que indica scheduler->signDirection puede pasar;
 *                  el letrero cambia de direccion cada signIntervalMillis.
 *
 * Al cambiar el modo se reinicia fairnessPassedInWindow (contador de barcos
 * despachados en la ventana actual) y signLastSwitchAt (timestamp del ultimo
 * cambio del letrero), para evitar que el estado de la configuracion anterior
 * contamine el comportamiento del nuevo modo.
 */
void ship_scheduler_set_flow_mode(ShipScheduler *scheduler, ShipFlowMode mode) {
  if (!scheduler) return;
  scheduler->flowMode = mode;
  scheduler->fairnessPassedInWindow = 0;
  scheduler->signLastSwitchAt = millis();
}

ShipFlowMode ship_scheduler_get_flow_mode(const ShipScheduler *scheduler) {
  if (!scheduler) return FLOW_TICO;
  return scheduler->flowMode;
}

const char *ship_scheduler_get_flow_mode_label(const ShipScheduler *scheduler) {
  if (!scheduler) return "?";
  switch (scheduler->flowMode) {
    case FLOW_TICO: return "TICO";
    case FLOW_FAIRNESS: return "EQUIDAD";
    case FLOW_SIGN: return "LETRERO";
  }
  return "?";
}

/*
 * ship_scheduler_set_fairness_window / get_fairness_window
 *
 * scheduler->fairnessWindowW (uint8_t): numero de barcos del lado actual que
 * pueden cruzar consecutivamente antes de ceder el turno al lado opuesto.
 *
 * Ejemplo con W=3:
 *   - Lado izquierdo despacha 3 barcos -> cambio a lado derecho.
 *   - Lado derecho despacha 3 barcos   -> cambio a lado izquierdo.
 *   (Si un lado se queda vacio antes de W, se alterna de inmediato.)
 *
 * El minimo es 1 (nunca 0) para garantizar que al menos un barco cruce antes
 * de alternar; de lo contrario la ventana nunca avanzaria.
 * get_fairness_window tambien normaliza a 1 si el campo interno es 0 (estado
 * inicial del struct antes de que el usuario configure el valor).
 *
 * fairnessPassedInWindow se resetea a 0 al cambiar W para reiniciar el conteo
 * con la nueva configuracion.
 */
void ship_scheduler_set_fairness_window(ShipScheduler *scheduler, uint8_t windowW) {
  if (!scheduler) return;
  if (windowW == 0) windowW = 1;
  scheduler->fairnessWindowW = windowW;
  scheduler->fairnessPassedInWindow = 0;
}

uint8_t ship_scheduler_get_fairness_window(const ShipScheduler *scheduler) {
  if (!scheduler) return 1;
  return scheduler->fairnessWindowW == 0 ? 1 : scheduler->fairnessWindowW;
}

/*
 * ship_scheduler_set_sign_direction / get_sign_direction
 * ship_scheduler_set_sign_interval / get_sign_interval
 *
 * MODO LETRERO (FLOW_SIGN):
 *   scheduler->signDirection (BoatSide): lado que el letrero permite en este
 *   momento (SIDE_LEFT o SIDE_RIGHT). Solo barcos de ese lado pueden entrar al
 *   canal; el otro lado espera. Si no hay barcos del lado permitido, se usa el
 *   lado opuesto como fallback para no paralizar el flujo.
 *
 *   scheduler->signIntervalMillis (unsigned long): cada cuantos milisegundos
 *   cambia automaticamente signDirection. El minimo es 1000ms (1 segundo) para
 *   dar tiempo a que al menos un barco comience a cruzar antes del cambio.
 *   El cambio lo efectua ship_scheduler_tick_sign() llamado desde update().
 *
 *   set_sign_direction y set_sign_interval actualizan signLastSwitchAt = millis()
 *   para reiniciar el temporizador desde el momento del cambio manual, evitando
 *   que el letrero cambie inmediatamente en el proximo tick si ya expiro el intervalo.
 */
void ship_scheduler_set_sign_direction(ShipScheduler *scheduler, BoatSide side) {
  if (!scheduler) return;
  scheduler->signDirection = side;
  scheduler->signLastSwitchAt = millis();
}

BoatSide ship_scheduler_get_sign_direction(const ShipScheduler *scheduler) {
  if (!scheduler) return SIDE_LEFT;
  return scheduler->signDirection;
}

void ship_scheduler_set_sign_interval(ShipScheduler *scheduler, unsigned long intervalMillis) {
  if (!scheduler) return;
  if (intervalMillis < 1000) intervalMillis = 1000;
  scheduler->signIntervalMillis = intervalMillis;
  scheduler->signLastSwitchAt = millis();
}

unsigned long ship_scheduler_get_sign_interval(const ShipScheduler *scheduler) {
  if (!scheduler) return 0;
  return scheduler->signIntervalMillis;
}

/*
 * ship_scheduler_set_max_ready_queue / get_max_ready_queue
 *
 * scheduler->maxReadyQueueConfigured (uint8_t): limite de barcos que pueden
 * estar en readyQueue[] simultáneamente. Si la cola está llena al intentar
 * encolar un barco nuevo, se descarta (y se libera su struct con destroyBoat).
 *
 * Restricciones:
 *   - Minimo: 1 (nunca 0; la cola debe poder recibir al menos 1 barco).
 *   - Maximo: MAX_BOATS (techo absoluto definido en ShipScheduler.h).
 *     Aunque el usuario configure un valor mayor, se recorta a MAX_BOATS
 *     para no exceder el tamano del array readyQueue[MAX_BOATS].
 *
 * get_max_ready_queue normaliza: si maxReadyQueueConfigured==0 (campo no
 * inicializado), retorna MAX_BOATS en lugar de 0 para no bloquear todos los
 * encolamientos por defecto.
 */
void ship_scheduler_set_max_ready_queue(ShipScheduler *scheduler, uint8_t limit) {
  if (!scheduler) return;
  if (limit == 0) limit = 1;
  if (limit > MAX_BOATS) limit = MAX_BOATS;
  scheduler->maxReadyQueueConfigured = limit;
}

uint8_t ship_scheduler_get_max_ready_queue(const ShipScheduler *scheduler) {
  if (!scheduler) return MAX_BOATS;
  if (scheduler->maxReadyQueueConfigured == 0 || scheduler->maxReadyQueueConfigured > MAX_BOATS) return MAX_BOATS;
  return scheduler->maxReadyQueueConfigured;
}

/*
 * ship_scheduler_set_channel_length / get_channel_length
 * ship_scheduler_set_boat_speed   / get_boat_speed
 *
 * Parametros fisicos del canal usados para calcular serviceMillis:
 *
 *   scheduler->channelLengthMeters (uint16_t): longitud total del canal en
 *   metros. Determina cuantas casillas (listLength) tiene el canal fisico.
 *   El minimo es 1 metro para evitar division por cero en estimaciones.
 *
 *   scheduler->boatSpeedMetersPerSec (uint16_t): velocidad base de los barcos
 *   en metros por segundo. Se multiplica por un typeFactor segun el tipo de
 *   barco (BOAT_CARGO es mas lento que BOAT_FAST) para obtener la velocidad
 *   efectiva. El minimo es 1 m/s para evitar division por cero.
 *
 * Formula de serviceMillis:
 *   baseSpeed = boatSpeedMetersPerSec * typeFactor
 *   serviceMs = (channelLengthMeters / baseSpeed) * 1000
 *   Esto representa el tiempo en ms que tardaria un barco en cruzar el canal
 *   completo a su velocidad efectiva.
 */
void ship_scheduler_set_channel_length(ShipScheduler *scheduler, uint16_t meters) {
  if (!scheduler) return;
  if (meters == 0) meters = 1;
  scheduler->channelLengthMeters = meters;
}

uint16_t ship_scheduler_get_channel_length(const ShipScheduler *scheduler) {
  if (!scheduler) return 0;
  return scheduler->channelLengthMeters;
}

void ship_scheduler_set_boat_speed(ShipScheduler *scheduler, uint16_t metersPerSec) {
  if (!scheduler) return;
  if (metersPerSec == 0) metersPerSec = 1;
  scheduler->boatSpeedMetersPerSec = metersPerSec;
}

uint16_t ship_scheduler_get_boat_speed(const ShipScheduler *scheduler) {
  if (!scheduler) return 0;
  return scheduler->boatSpeedMetersPerSec;
}

/*
 * ship_scheduler_set_flow_logging / get_flow_logging
 *
 * scheduler->flowLoggingEnabled (bool): habilita o deshabilita los mensajes
 * de trazado del control de flujo (macro FLOW_LOG).
 *
 * FLOW_LOG se expande a:
 *   if (scheduler->flowLoggingEnabled) ship_logf(fmt, ...);
 * Es decir: cuando flowLoggingEnabled==false, los mensajes de debug del
 * schedulador NO se envian por Serial, reduciendo la carga de I/O.
 *
 * IMPORTANTE: Los mensajes de preempcion ("Preemption: barco #N desaloja a #M")
 * usan ship_logf DIRECTAMENTE (incondicional), NO FLOW_LOG. Esto es intencional:
 * el simulador Python los parsea para actualizar el canvas y necesita verlos
 * independientemente de si el flow logging esta activo.
 */
void ship_scheduler_set_flow_logging(ShipScheduler *scheduler, bool enabled) {
  if (!scheduler) return;
  scheduler->flowLoggingEnabled = enabled;
}

bool ship_scheduler_get_flow_logging(const ShipScheduler *scheduler) {
  if (!scheduler) return false;
  return scheduler->flowLoggingEnabled;
}

/*
 * ship_scheduler_set_tico_margin_factor / get_tico_margin_factor
 *
 * scheduler->ticoMarginFactor[activeType][candidateType] (float, matriz 3x3):
 * Factor multiplicador del gap minimo entre barcos en modo FLOW_TICO.
 * Permite ajustar la distancia de seguridad segun la combinacion de tipos:
 *   - BOAT_CARGO  (0): barco de carga, mas lento y largo.
 *   - BOAT_NORMAL (1): barco estandar.
 *   - BOAT_FAST   (2): barco rapido.
 *
 * LAYOUT EN MEMORIA (row-major, C estandar):
 *   ticoMarginFactor[0][0], [0][1], [0][2]  <- activo es BOAT_CARGO
 *   ticoMarginFactor[1][0], [1][1], [1][2]  <- activo es BOAT_NORMAL
 *   ticoMarginFactor[2][0], [2][1], [2][2]  <- activo es BOAT_FAST
 *   Total: 9 floats contiguos = 36 bytes en el struct.
 *
 * Acceso: scheduler->ticoMarginFactor[at][ct]
 *   = *(*(scheduler->ticoMarginFactor + at) + ct)
 *   = *(scheduler->ticoMarginFactor[0] + at*3 + ct)
 *
 * Validaciones:
 *   - activeType y candidateType deben estar en [0, 2].
 *   - factor <= 0.0f se normaliza a 1.0f (sin factor de margen).
 */
void ship_scheduler_set_tico_margin_factor(ShipScheduler *scheduler, BoatType activeType, BoatType candidateType, float factor) {
  if (!scheduler) return;
  if (activeType < 0 || activeType >= 3) return;
  if (candidateType < 0 || candidateType >= 3) return;
  if (factor <= 0.0f) factor = 1.0f;
  scheduler->ticoMarginFactor[activeType][candidateType] = factor;
}

float ship_scheduler_get_tico_margin_factor(const ShipScheduler *scheduler, BoatType activeType, BoatType candidateType) {
  if (!scheduler) return 1.0f;
  if (activeType < 0 || activeType >= 3) return 1.0f;
  if (candidateType < 0 || candidateType >= 3) return 1.0f;
  return scheduler->ticoMarginFactor[activeType][candidateType];
}
