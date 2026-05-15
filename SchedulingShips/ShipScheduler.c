#include "ShipScheduler.h" // API del scheduler. 

#include <freertos/FreeRTOS.h> // Tipos base de FreeRTOS. 
#include <freertos/task.h> // API de tareas. 
#include <stdlib.h> // malloc y free. 
#include <stdio.h> // fopen para cargar channel_config.txt
#include <freertos/semphr.h> // Semaphores and mutexes
#include <string.h>
#include <limits.h> // ULONG_MAX para comparaciones de servicio en SJF.

#include "ShipIO.h" // Logging por Serial. 
#include "ShipDisplay.h" // API de pantalla (para que cada barco pueda redibujar). 
#include "ShipCommands.h" // Parser de comandos para leer config.

/*
 * gScheduler: puntero GLOBAL al unico ShipScheduler activo en el sistema.
 * Se almacena en la seccion .data del binario (variable global inicializada a NULL).
 * Su valor es la DIRECCION DE MEMORIA del struct ShipScheduler creado en el main
 * (tipicamente en el stack de app_main o como variable estatica en SchedulingShips.ino).
 * Se usa como acceso rapido desde boatTask (tarea FreeRTOS) que no recibe el
 * scheduler como parametro, solo el puntero al Boat.
 */
ShipScheduler *gScheduler = NULL; // Puntero global para callbacks. 

/*
 * gSlotSemaphores: ARRAY de punteros a semaforos binarios FreeRTOS, uno por casilla.
 * En memoria: es un puntero a un bloque de heap de tamano (listLength * sizeof(SemaphoreHandle_t)).
 * SemaphoreHandle_t es en realidad un 'void*' que apunta a la estructura interna
 * de FreeRTOS Queue_t (las semaphores se implementan sobre colas de longitud 1).
 * Layout en memoria: gSlotSemaphores -> [ptr_sem_0][ptr_sem_1]...[ptr_sem_N-1]
 *                                            |               |               |
 *                                         Queue_t         Queue_t         Queue_t  (en heap FreeRTOS)
 * Cada 'take' de un semaforo bloquea atomicamente el acceso a esa casilla.
 * Cada 'give' desbloquea y senala a otras tareas que estan esperando.
 *
 * gSlotSemaphoreCount: copia de listLength usada al destruir el array para
 * saber cuantos elementos liberar. Se guarda separado porque el scheduler puede
 * cambiar listLength sin invalidar inmediatamente el array antiguo.
 */
static SemaphoreHandle_t *gSlotSemaphores = NULL; // Puntero al array de semaforos de casillas (heap).
static uint16_t gSlotSemaphoreCount = 0; // Cuantos semaforos contiene el array actual.

static void ship_scheduler_requeue_boat(ShipScheduler *scheduler, Boat *boat, bool atFront); // Declara requeue. 
static bool ship_scheduler_start_next_boat(ShipScheduler *scheduler); // Declara start. 
static void ship_scheduler_finish_active_boat(ShipScheduler *scheduler, Boat *boat); // Declara finish. 
static bool ship_scheduler_preempt_active_for_rr(ShipScheduler *scheduler); // Declara preempt RR. 
static void ship_scheduler_yield_active_for_rr(ShipScheduler *scheduler, Boat *boat); // Yield voluntario cuando el barco se bloquea.
static BoatSide opposite_side(BoatSide side); // Declara lado opuesto.
static bool queue_has_side(const ShipScheduler *scheduler, BoatSide side); // Declara busqueda por lado.
static bool candidate_is_better(ShipAlgo algo, const Boat *candidate, const Boat *best); // Declara comparador.
static int findIndexForAlgoAndSide(ShipAlgo algo, Boat *readyQueue[], uint8_t readyCount, bool useSide, BoatSide side); // Declara selector filtrado.
static void ship_scheduler_tick_sign(ShipScheduler *scheduler); // Declara tick de letrero.
static int ship_scheduler_select_next_index(ShipScheduler *scheduler); // Declara selector con flujo.
static unsigned long ship_scheduler_boat_elapsed_millis(const Boat *boat); // Declara elapsed por barco.
static unsigned long ship_scheduler_estimate_service_millis(const ShipScheduler *scheduler, const Boat *boat); // Declara estimador de servicio.
static void ship_scheduler_sync_primary_active(ShipScheduler *scheduler); // Declara sync de activo.
static void ship_scheduler_sync_rr_permissions(ShipScheduler *scheduler); // Declara sync de permisos RR.
static void ship_scheduler_promote_active_to_front(ShipScheduler *scheduler, Boat *boat); // Promueve un activo al frente.
static void ship_scheduler_add_active(ShipScheduler *scheduler, Boat *boat); // Declara alta de activo.
static void ship_scheduler_remove_active(ShipScheduler *scheduler, Boat *boat); // Declara baja de activo.
static bool ship_scheduler_is_tico_safe(const ShipScheduler *scheduler, const Boat *candidate); // Declara seguridad TICO.
static void ship_scheduler_destroy_slot_resources(void); // Libera semaforos de casillas.
static unsigned long ship_scheduler_compute_default_deadline(const ShipScheduler *scheduler, const Boat *boat); // Declara deadline EDF por defecto.
static bool ship_scheduler_init_slot_resources(uint16_t count); // Crea semaforos de casillas.
static SemaphoreHandle_t ship_scheduler_get_slot_semaphore(uint16_t slotIndex); // Obtiene semaforo de casilla.
static bool ship_scheduler_wait_for_slot(ShipScheduler *scheduler, uint16_t slotIndex, Boat *boat); // Espera una casilla libre.
static void ship_scheduler_lock_slot_owner(ShipScheduler *scheduler, uint16_t slotIndex, uint8_t boatId); // Marca casilla ocupada.
static void ship_scheduler_unlock_slot_owner(ShipScheduler *scheduler, uint16_t slotIndex); // Marca casilla libre.
static void ship_scheduler_restore_parked_boats(ShipScheduler *scheduler); // Reubica barcos retirados por emergencia.

/*
 * safe_task_notify — envia una notificacion directa a una tarea FreeRTOS.
 *
 * TaskHandle_t es un 'void*' que apunta al TCB (Task Control Block) de la tarea
 * destino dentro del heap de FreeRTOS. El TCB incluye el estado de notificacion,
 * el valor de notificacion (uint32_t), el stack de la tarea, etc.
 *
 * xTaskNotify(taskHandle, value, eSetValueWithOverwrite):
 *   - Escribe 'value' en el campo ulNotifiedValue del TCB apuntado por taskHandle.
 *   - eSetValueWithOverwrite: si ya habia un valor no leido, lo sobreescribe.
 *     Esto es intencional: solo nos importa la ultima orden (RUN/PAUSE/TERMINATE).
 *   - Si la tarea estaba bloqueada en xTaskNotifyWait(), la desbloquea y la pone
 *     en estado 'Ready' para que el scheduler FreeRTOS la ejecute cuando tenga CPU.
 *   - Esta funcion NO bloquea: simplemente escribe en el TCB y retorna.
 *
 * Guardia 'if (!taskHandle)': evita pasar NULL a xTaskNotify, lo cual causa
 * un hardfault en ESP32 porque intentaria desreferenciar la direccion 0x0.
 */
static void safe_task_notify(TaskHandle_t taskHandle, uint32_t notificationValue) {
  if (!taskHandle) return; // Guardia: NULL == direccion 0x0, dereferenciarla causa hardfault.
  // Escribe notificationValue en el campo ulNotifiedValue del TCB de la tarea destino.
  // Si la tarea estaba bloqueada esperando notificacion, la desbloquea inmediatamente.
  xTaskNotify(taskHandle, notificationValue, eSetValueWithOverwrite);
}

/*
 * findIndexForAlgoAndSide — recorre readyQueue[] y devuelve el indice del mejor
 * candidato segun el algoritmo de planificacion 'algo' y el filtro de lado.
 *
 * readyQueue[]: array de punteros a Boat. Cada elemento es la DIRECCION del
 *   struct Boat en el heap. El array en si puede estar en el stack (local) o
 *   dentro del struct ShipScheduler (campo readyQueue[MAX_BOATS]).
 *
 * readyQueue[i]: acceso al i-esimo puntero. En terminos de direcciones,
 *   equivale a *(readyQueue + i), donde readyQueue es la direccion base del array
 *   y sizeof(Boat*) es el desplazamiento entre elementos (4 u 8 bytes segun arch).
 *
 * Retorna el INDICE (posicion en el array), no el puntero, para que el llamador
 * pueda extraer y desplazar el elemento en O(n) sin buscar de nuevo.
 */
static int findIndexForAlgoAndSide(ShipAlgo algo, Boat *readyQueue[], uint8_t readyCount, bool useSide, BoatSide side) {
  if (!readyQueue || readyCount == 0) return -1; // Sin array o sin elementos no hay candidato.
  int bestIdx = -1;   // Indice del mejor encontrado; -1 = ningun candidato aun.
  Boat *best = NULL;  // Puntero al struct Boat del mejor candidato actual.
  for (uint8_t i = 0; i < readyCount; i++) {
    Boat *c = readyQueue[i]; // Desreferencia: lee el puntero almacenado en la posicion i del array.
    if (!c) continue;        // Posicion vacia (NULL = 0x0): saltar para evitar acceso invalido.
    if (useSide && c->origin != side) continue; // c->origin: accede al campo 'origin' del struct apuntado por c.
    if (!best) {
      // Primera vez: este es el mejor por defecto.
      best = c; bestIdx = (int)i; continue;
    }
    if (candidate_is_better(algo, c, best)) {
      // c gana al mejor actual: actualizar puntero e indice.
      best = c; bestIdx = (int)i;
    }
  }
  return bestIdx; // Indice dentro de readyQueue[] del mejor candidato, o -1 si no hay.
}

/*
 * ship_scheduler_try_reserve_range — intenta reservar 'steps' casillas contiguas.
 *
 * MEMORIA INVOLUCRADA:
 *  - scheduler->channelSlotsGuard: campo de tipo 'void*' en el struct.
 *    Se almacena como void* para no exponer SemaphoreHandle_t en el .h publico.
 *    El cast (SemaphoreHandle_t)scheduler->channelSlotsGuard reinterpreta esos
 *    4/8 bytes como un puntero a Queue_t (la estructura interna de FreeRTOS).
 *    Este es el MUTEX del canal: garantiza que solo una tarea a la vez pueda
 *    inspeccionar o modificar slotOwner[] y tomar semaforos de casillas.
 *
 *  - scheduler->slotOwner[idx]: array de uint8_t en el heap.
 *    slotOwner apunta al byte 0 del bloque; slotOwner[idx] = *(slotOwner + idx).
 *    Un valor 0 significa 'casilla libre'; cualquier otro valor es el ID del barco
 *    propietario. uint8_t = 1 byte por casilla, IDs validos son 1..255.
 *
 *  - gSlotSemaphores[idx]: semaforo binario de la casilla idx.
 *    xSemaphoreTake(sem, 0) intenta tomar el semaforo SIN bloquear (timeout=0).
 *    Si el semaforo ya fue tomado por otro (valor=0), retorna pdFALSE.
 *    Si esta disponible (valor=1), lo toma (pone valor a 0) y retorna pdTRUE.
 *
 *  - taken[]: buffer local en el stack de la funcion. Guarda los semaforos ya
 *    tomados para poder liberarlos en caso de fallo parcial (rollback atomico).
 *
 * PROTOCOLO ATOMICO:
 *  1. Tomar el mutex del canal (channelSlotsGuard) para serializar el acceso.
 *  2. Verificar que slotOwner[i]==0 para cada casilla del rango (sin propietario).
 *  3. Intentar xSemaphoreTake(sem, 0) para cada casilla (no bloquear).
 *     Si alguna falla: liberar TODAS las ya tomadas + liberar el mutex. Retorna false.
 *  4. Si todas fueron tomadas: escribir boat->id en slotOwner[i] para marcar propiedad.
 *  5. Liberar el mutex. El barco ahora es el unico con acceso a esas casillas.
 */
bool ship_scheduler_try_reserve_range(ShipScheduler *scheduler, int startIndex, uint8_t steps, Boat *boat) {
  if (!scheduler || !boat || steps == 0) return false; // Punteros nulos = acceso invalido.
  if (steps > 16) return false; // Guarda: el buffer taken[] tiene capacidad maxima de 16.
  if (scheduler->listLength == 0 || !scheduler->slotOwner) return false; // Canal no inicializado.
  // dir: +1 si el barco viaja de izq a der, -1 si viaja de der a izq.
  // Determina el sentido en que se numera el rango de casillas a reservar.
  int dir = (boat->origin == SIDE_LEFT) ? 1 : -1;
  // endIndex: casilla final del rango. Para un barco L->R que ocupa 2 casillas desde
  // la casilla 3: endIndex = 3 + 1*(2-1) = 4. El rango es {3,4}.
  int endIndex = startIndex + dir * ((int)steps - 1);
  // Validar que ambos extremos del rango caen dentro del canal [0, listLength-1].
  if (startIndex < 0 || endIndex < 0 || startIndex >= (int)scheduler->listLength || endIndex >= (int)scheduler->listLength) return false;

  // channelSlotsGuard almacenado como void*; cast a SemaphoreHandle_t (tambien void*)
  // para pasarlo a la API de FreeRTOS que espera ese tipo.
  if ((SemaphoreHandle_t)scheduler->channelSlotsGuard == NULL) return false; // Mutex no creado aun.
  // pdMS_TO_TICKS(100): convierte 100 ms a ticks FreeRTOS. En ESP32 a 1000 Hz = 100 ticks.
  // Si el mutex esta tomado, espera hasta 100 ms antes de declarar fallo.
  if (xSemaphoreTake((SemaphoreHandle_t)scheduler->channelSlotsGuard, pdMS_TO_TICKS(100)) != pdTRUE) return false;

  // --- DENTRO DE LA SECCION CRITICA (mutex tomado) ---

  // Paso 1: verificar propietarios en slotOwner[].
  // scheduler->slotOwner es uint8_t*; slotOwner[idx] lee el byte en la posicion
  // (scheduler->slotOwner + idx), que ocupa 1 byte en el heap.
  int idx = startIndex;
  for (uint8_t s = 0; s < steps; s++) {
    if (scheduler->slotOwner[idx] != 0) { // 0 = libre; != 0 = ocupado por algun barco.
      // Casilla ocupada: liberar el mutex y retornar fallo.
      xSemaphoreGive((SemaphoreHandle_t)scheduler->channelSlotsGuard);
      return false;
    }
    idx += dir; // Avanzar al siguiente indice en la direccion del barco.
  }

  // Paso 2: intentar tomar los semaforos binarios de cada casilla (sin bloquear).
  idx = startIndex;
  SemaphoreHandle_t taken[16]; // Buffer local en stack: guarda punteros a Queue_t tomados.
  uint8_t takenCount = 0;      // Cuantos se han tomado exitosamente hasta ahora.
  for (uint8_t s = 0; s < steps; s++) {
    // gSlotSemaphores[idx]: lee el puntero SemaphoreHandle_t de la posicion idx
    // en el array global, equivale a *(gSlotSemaphores + idx).
    SemaphoreHandle_t sem = ship_scheduler_get_slot_semaphore((uint16_t)idx);
    if (!sem) { // Semaforo no inicializado para este indice.
      // Rollback: liberar todos los semaforos ya tomados en este intento.
      for (uint8_t t = 0; t < takenCount; t++) xSemaphoreGive(taken[t]);
      xSemaphoreGive((SemaphoreHandle_t)scheduler->channelSlotsGuard);
      return false;
    }
    // xSemaphoreTake(sem, 0): timeout=0 => no bloquear. Atomicamente decrementa
    // el contador del semaforo de 1 a 0 si estaba disponible; retorna pdFALSE si ya era 0.
    if (xSemaphoreTake(sem, 0) != pdTRUE) {
      // Esta casilla ya esta bloqueada por otra tarea.
      for (uint8_t t = 0; t < takenCount; t++) xSemaphoreGive(taken[t]); // Rollback.
      xSemaphoreGive((SemaphoreHandle_t)scheduler->channelSlotsGuard);
      return false;
    }
    taken[takenCount++] = sem; // Guardar puntero al semaforo tomado para posible rollback.
    idx += dir;
  }

  // Paso 3: todos los semaforos tomados => registrar propiedad en slotOwner[].
  // Esto hace que otros hilos que comprueben slotOwner[] (fuera del semaforo)
  // vean la casilla como ocupada incluso si aun no adquirieron el mutex.
  idx = startIndex;
  for (uint8_t s = 0; s < steps; s++) {
    // Escribe boat->id (1..255) en slotOwner[idx]. Lectura posterior: slotOwner[idx] != 0 => ocupado.
    ship_scheduler_lock_slot_owner(scheduler, (uint16_t)idx, boat->id);
    idx += dir;
  }

  // Liberar el mutex: otros hilos pueden ahora inspeccionar el canal.
  // Las casillas quedan 'tomadas' por los semaforos binarios; el propietario
  // los liberara con xSemaphoreGive cuando salga de esas casillas.
  xSemaphoreGive((SemaphoreHandle_t)scheduler->channelSlotsGuard);
  return true; // Rango reservado exitosamente.
}

// Valores por defecto si no existe channel_config.txt
#define DEFAULT_LIST_LENGTH 3U
#define DEFAULT_VISUAL_CHANNEL_LENGTH 6U

#define FLOW_LOG(schedulerPtr, fmt, ...) do { if ((schedulerPtr) && (schedulerPtr)->flowLoggingEnabled) ship_logf(fmt, ##__VA_ARGS__); } while (0) // Macro de trazas de flujo.

/*
 * ship_scheduler_destroy_slot_resources — libera toda la memoria de semaforos.
 *
 * vSemaphoreDelete(gSlotSemaphores[i]):
 *   - Llama a vQueueDelete() internamente; libera la Queue_t del heap de FreeRTOS.
 *   - IMPORTANTE: ninguna tarea debe estar bloqueada en este semaforo al llamarla;
 *     de lo contrario el comportamiento es indefinido (potencial corrupcion del heap).
 *   - Tras la llamada, el puntero guardado en gSlotSemaphores[i] ya no es valido;
 *     se pone a NULL para evitar double-free o acceso a memoria liberada.
 *
 * free(gSlotSemaphores):
 *   - Devuelve al heap de C (heap normal, no el de FreeRTOS) el bloque de
 *     (gSlotSemaphoreCount * sizeof(SemaphoreHandle_t)) bytes.
 *   - gSlotSemaphores pasa a ser un puntero dangling si no se pone a NULL;
 *     por eso se asigna NULL inmediatamente despues.
 */
static void ship_scheduler_destroy_slot_resources(void) {
  if (gSlotSemaphores) { // Solo actuar si el array existe; evitar free(NULL) aunque sea seguro.
    for (uint16_t i = 0; i < gSlotSemaphoreCount; i++) {
      if (gSlotSemaphores[i]) { // Verificar cada elemento: podria ser NULL si init fallo a medias.
        vSemaphoreDelete(gSlotSemaphores[i]); // Libera la Queue_t interna en el heap de FreeRTOS.
        gSlotSemaphores[i] = NULL;            // Evita usar el puntero invalido (dangling pointer).
      }
    }
    free(gSlotSemaphores); // Libera el array de punteros del heap de C.
    gSlotSemaphores = NULL; // Invalida el puntero global para evitar double-free.
  }
  gSlotSemaphoreCount = 0; // Resetea el contador; indica que no hay semaforos activos.
}

/*
 * ship_scheduler_init_slot_resources — crea 'count' semaforos binarios para el canal.
 *
 * calloc(count, sizeof(SemaphoreHandle_t)):
 *   - Reserva count * 4 bytes (o count * 8 en 64-bit) en el heap de C.
 *   - calloc INICIALIZA a cero todos los bytes, equivalente a:
 *       ptr = malloc(count * sizeof(SemaphoreHandle_t));
 *       memset(ptr, 0, count * sizeof(SemaphoreHandle_t));
 *   - Tener los punteros a NULL desde el inicio permite que destroy_slot_resources
 *     detecte correctamente elementos no inicializados si init falla a medias.
 *
 * xSemaphoreCreateBinary():
 *   - Llama a xQueueCreate(1, 0) internamente; asigna una Queue_t en el heap FreeRTOS.
 *   - El semaforo recien creado esta en estado 'TAKEN' (valor=0, no disponible).
 *   - Por eso hay que llamar a xSemaphoreGive() inmediatamente para ponerlo
 *     en estado 'AVAILABLE' (valor=1); de lo contrario el primer barco que
 *     intente tomar la casilla se bloquearia indefinidamente.
 *
 * gSlotSemaphores[i] = xSemaphoreCreateBinary():
 *   - Guarda la direccion de la Queue_t recien creada en la posicion i del array.
 *   - Equivale a: *(gSlotSemaphores + i) = <puntero retornado por FreeRTOS>.
 */
static bool ship_scheduler_init_slot_resources(uint16_t count) {
  ship_scheduler_destroy_slot_resources(); // Garantiza estado limpio antes de re-crear.
  if (count == 0) return false;            // Sin casillas no tiene sentido crear el array.
  // Reserva en el heap de C un array de 'count' punteros a SemaphoreHandle_t, todos en NULL.
  gSlotSemaphores = (SemaphoreHandle_t *)calloc(count, sizeof(SemaphoreHandle_t));
  if (!gSlotSemaphores) return false; // calloc retorna NULL si el heap esta agotado.
  gSlotSemaphoreCount = count; // Guardar antes del loop para que destroy funcione si hay fallo.
  for (uint16_t i = 0; i < count; i++) {
    // Crea un semaforo binario para la casilla i en el heap de FreeRTOS.
    // El resultado es un void* (SemaphoreHandle_t) que identifica la estructura interna.
    gSlotSemaphores[i] = xSemaphoreCreateBinary();
    if (!gSlotSemaphores[i]) {
      // FreeRTOS heap agotado: liberar todo lo creado hasta ahora y retornar fallo.
      ship_scheduler_destroy_slot_resources();
      return false;
    }
    // Da el semaforo (valor 0 -> 1): la casilla queda disponible para ser tomada.
    // Sin este Give, el primer xSemaphoreTake bloquea indefinidamente.
    xSemaphoreGive(gSlotSemaphores[i]);
  }
  return true; // Todos los semaforos creados y disponibles.
}

/*
 * ship_scheduler_get_slot_semaphore — acceso seguro al array global de semaforos.
 *
 * gSlotSemaphores[slotIndex]: equivale a *(gSlotSemaphores + slotIndex).
 * El calificador de rango 'slotIndex < gSlotSemaphoreCount' previene acceso fuera
 * del bloque reservado por calloc (buffer overflow).
 * Retorna NULL si el indice es invalido; todos los llamadores verifican NULL antes
 * de usar el puntero.
 */
static SemaphoreHandle_t ship_scheduler_get_slot_semaphore(uint16_t slotIndex) {
  // Validar puntero y rango: slotIndex fuera de rango causaria leer memoria no asignada.
  if (!gSlotSemaphores || slotIndex >= gSlotSemaphoreCount) return NULL;
  return gSlotSemaphores[slotIndex]; // Lee el puntero void* guardado en la posicion slotIndex.
}

/*
 * ship_scheduler_lock_slot_owner — escribe el ID del barco en slotOwner[slotIndex].
 *
 * scheduler->slotOwner es un uint8_t* que apunta al byte 0 del bloque de
 * (listLength * 1 byte) reservado con malloc/calloc en ship_scheduler_rebuild_slots.
 *
 * scheduler->slotOwner[slotIndex] = boatId  es equivalente a:
 *   *(scheduler->slotOwner + slotIndex) = boatId;
 *   o bien: *((uint8_t*)scheduler->slotOwner + slotIndex) = boatId;
 *
 * Offset en bytes: slotIndex * sizeof(uint8_t) = slotIndex * 1.
 * El valor boatId es siempre != 0 (IDs empiezan en 1), por lo que cualquier
 * lectura posterior de slotOwner[i] != 0 indica 'casilla ocupada'.
 *
 * La guardia 'slotIndex >= scheduler->listLength' evita escribir fuera del
 * bloque reservado (buffer overflow), lo que corromperia el heap.
 */
static void ship_scheduler_lock_slot_owner(ShipScheduler *scheduler, uint16_t slotIndex, uint8_t boatId) {
  // Validar: scheduler debe existir, slotOwner apuntar a memoria valida, e indice en rango.
  if (!scheduler || !scheduler->slotOwner || slotIndex >= scheduler->listLength) return;
  // Escribe boatId en el byte numero slotIndex del array slotOwner.
  // Despues de esto, cualquier comprobacion slotOwner[slotIndex] != 0 devuelve true.
  scheduler->slotOwner[slotIndex] = boatId;
}

/*
 * ship_scheduler_unlock_slot_owner — libera la casilla escribiendo 0 en slotOwner[slotIndex].
 *
 * Escribir 0 en slotOwner[slotIndex] senala a todos los hilos que lean ese byte
 * que la casilla ya no tiene propietario. El valor 0 es el centinela de 'libre'.
 * Solo se hace DESPUES de dar el semaforo binario de esa casilla (xSemaphoreGive),
 * para que cualquier tarea que espere en el semaforo, al desbloquearse, encuentre
 * slotOwner[slotIndex] == 0 y pueda reclamar la casilla sin conflicto.
 */
static void ship_scheduler_unlock_slot_owner(ShipScheduler *scheduler, uint16_t slotIndex) {
  if (!scheduler || !scheduler->slotOwner || slotIndex >= scheduler->listLength) return;
  scheduler->slotOwner[slotIndex] = 0; // 0 = casilla libre; ninguna tarea la posee.
}

/*
 * ship_scheduler_restore_parked_boats — re-reserva casillas de barcos que fueron
 * estacionados de emergencia (sensor de colision, no preempcion).
 *
 * scheduler->activeBoats[i]: el campo activeBoats es un array de Boat* DENTRO del
 *   struct ShipScheduler (no en el heap aparte). Leer activeBoats[i] es acceder al
 *   i-esimo puntero de ese array embebido; si el puntero != NULL, apunta a un
 *   Boat en el heap creado por createBoat().
 *
 * activeBoat->emergencySavedSlot: int16_t campo del struct Boat. Guarda el indice
 *   de la casilla donde estaba el barco cuando se activo la emergencia.
 *   -1 significa 'sin casilla guardada'.
 *
 * activeBoat->emergencyParked: bool campo del struct Boat. true = barco detenido
 *   en su casilla esperando que la emergencia se resuelva.
 */
static void ship_scheduler_restore_parked_boats(ShipScheduler *scheduler) {
  if (!scheduler) return;
  for (uint8_t i = 0; i < scheduler->activeCount; i++) {
    Boat *activeBoat = scheduler->activeBoats[i]; // Desreferencia: lee el Boat* en la posicion i.
    if (!activeBoat || !activeBoat->emergencyParked) continue; // Saltar si no esta estacionado.
    int16_t savedSlot = activeBoat->emergencySavedSlot; // Casilla guardada al inicio de emergencia.
    activeBoat->emergencyParked = false;  // Limpiar la bandera antes de intentar restaurar.
    activeBoat->emergencySavedSlot = -1; // Limpiar el slot guardado.
    if (savedSlot >= 0 && scheduler->listLength > 0 && scheduler->slotOwner) {
      // Intentar restaurar con el numero de casillas que usa el barco (stepSize).
      uint8_t slotsToRestore = (activeBoat->stepSize > 0) ? activeBoat->stepSize : 1;
      if (slotsToRestore > 16) slotsToRestore = 16; // Cota del buffer interno de try_reserve_range.
      if (ship_scheduler_try_reserve_range(scheduler, (int)savedSlot, slotsToRestore, activeBoat)) {
        activeBoat->currentSlot = savedSlot; // Restaura la posicion del barco en el canal.
        FLOW_LOG(scheduler, "[EMERGENCY] Barco #%u restaurado en casilla %d (steps=%u)\n", activeBoat->id, savedSlot, slotsToRestore);
      } else {
        // Fallback: intentar con solo 1 casilla si el rango completo no esta libre.
        if (ship_scheduler_try_reserve_range(scheduler, (int)savedSlot, 1, activeBoat)) {
          activeBoat->currentSlot = savedSlot;
          FLOW_LOG(scheduler, "[EMERGENCY] Barco #%u restaurado en casilla %d (fallback 1 slot)\n", activeBoat->id, savedSlot);
        } else {
          FLOW_LOG(scheduler, "[EMERGENCY] No se pudo restaurar casilla %d para barco #%u\n", savedSlot, activeBoat->id);
        }
      }
    }
  }
}

/*
 * ship_scheduler_wait_for_slot — bucle de espera activa por un semaforo de casilla.
 *
 * Se usa cuando el barco llego a la entrada del canal pero la primera casilla
 * esta ocupada. En lugar de bloquearse indefinidamente (podria recibir TERMINATE
 * mientras espera), hace intentos de 100ms con timeout para poder reaccionar.
 *
 * xSemaphoreTake(slotSem, pdMS_TO_TICKS(100)):
 *   Espera hasta 100 ms a que el semaforo este disponible (valor pase de 0 a 1).
 *   Si en 100 ms no queda libre, retorna pdFALSE sin haber tomado el semaforo.
 *   Si queda libre antes de 100 ms, lo toma (valor a 0) y retorna pdTRUE.
 *
 * xTaskNotifyWait(0x00, 0xFFFFFFFF, &waitCmd, pdMS_TO_TICKS(0)):
 *   Parametro 1 (0x00): bits a limpiar ANTES de leer (ningun bit; no borra nada previo).
 *   Parametro 2 (0xFFFFFFFF): bits a limpiar DESPUES de leer (todos; limpia la notificacion).
 *   Parametro 3 (&waitCmd): donde escribir el valor de notificacion recibido.
 *   Parametro 4 (0): timeout = 0 ticks (no bloquear; leer si hay algo pendiente).
 *   Si habia una notificacion pendiente (TERMINATE o PAUSE), la lee y retorna pdTRUE.
 *   Asi el barco puede abortar la espera de casilla si el scheduler lo cancela.
 */
static bool ship_scheduler_wait_for_slot(ShipScheduler *scheduler, uint16_t slotIndex, Boat *boat) {
  if (!scheduler || !boat) return false;
  SemaphoreHandle_t slotSem = ship_scheduler_get_slot_semaphore(slotIndex); // Puntero al semaforo de la casilla.
  if (!slotSem) return false; // Casilla fuera de rango o array no inicializado.

  uint32_t waitCmd = 0; // Almacena el valor de notificacion recibido.
  for (;;) { // Loop infinito hasta que se tome el semaforo o se reciba orden de abortar.
    // Intenta tomar el semaforo de la casilla durante maximo 100ms.
    if (xSemaphoreTake(slotSem, pdMS_TO_TICKS(100)) == pdTRUE) return true; // Casilla obtenida.
    // Timeout: revisar si hay notificacion de control pendiente sin bloquear.
    if (xTaskNotifyWait(0x00, 0xFFFFFFFF, &waitCmd, pdMS_TO_TICKS(0)) == pdTRUE) {
      if (waitCmd == NOTIF_CMD_TERMINATE) return false; // Scheduler cancelo la tarea.
      if (waitCmd == NOTIF_CMD_PAUSE)     return false; // Scheduler pauso la tarea.
    }
    // No hay notificacion critica: volver a intentar en el proximo ciclo de 100ms.
  }
}

/*
 * ship_scheduler_wait_for_slot_range — verifica si un rango de casillas esta libre.
 *
 * A diferencia de try_reserve_range, esta funcion SOLO COMPRUEBA; no toma semaforos
 * ni escribe en slotOwner[]. Es una consulta rapida bajo el mutex del canal.
 *
 * scheduler->slotOwner[idx]: accede al byte idx del array para leer su valor actual.
 *   Si slotOwner[idx] == 0, la casilla esta libre.
 *   Si slotOwner[idx] != 0, la casilla tiene un propietario.
 */
static bool ship_scheduler_wait_for_slot_range(ShipScheduler *scheduler, int startIndex, int steps, int dir, Boat *boat) {
  if (!scheduler || !boat || steps <= 0) return false;
  // Tomar el mutex para leer slotOwner[] de forma consistente (otra tarea podria
  // estar escribiendo en slotOwner[] concurrentemente).
  if (xSemaphoreTake((SemaphoreHandle_t)scheduler->channelSlotsGuard, pdMS_TO_TICKS(100)) != pdTRUE) return false;

  bool allFree = true; // Resultado: verdadero si todo el rango esta libre.
  int idx = startIndex;
  for (int s = 0; s < steps; s++) {
    // Comprobar que idx esta dentro de [0, listLength-1] y que la casilla no tiene dueno.
    if (idx < 0 || idx >= scheduler->listLength || scheduler->slotOwner[idx] != 0) {
      allFree = false; // Alguna casilla del rango esta ocupada o fuera de limites.
      break;
    }
    idx += dir; // Avanzar en la direccion del barco.
  }

  xSemaphoreGive((SemaphoreHandle_t)scheduler->channelSlotsGuard); // Liberar el mutex.
  return allFree;
}

static BoatSide opposite_side(BoatSide side) {
  return side == SIDE_LEFT ? SIDE_RIGHT : SIDE_LEFT;
}

static bool queue_has_side(const ShipScheduler *scheduler, BoatSide side) {
  if (!scheduler) return false;
  for (uint8_t i = 0; i < scheduler->readyCount; i++) {
    Boat *candidate = scheduler->readyQueue[i];
    if (candidate && candidate->origin == side) return true;
  }
  return false;
}

/*
 * ship_scheduler_try_move_range — mueve atomicamente un barco 'steps' casillas.
 *
 * Esta es la operacion de mayor complejidad del scheduler porque involucra
 * multiples casillas de forma atomica: si falla a medias, revierte todo.
 *
 * SECUENCIA CRITICA DE MEMORIA:
 *  1. Verificar rango libre bajo el mutex (slotOwner[] read).
 *  2. Adquirir los semaforos de casillas intermedias + destino (toma atomica).
 *  3. Escribir boat->id en slotOwner[idx] para cada casilla del trayecto.
 *  4. Liberar la casilla de ORIGEN:
 *       slotOwner[currentIndex] = 0  (el byte queda disponible)
 *       xSemaphoreGive(currentSem)   (el semaforo sube a 1; otro barco puede tomarlo)
 *  5. Liberar casillas INTERMEDIAS (barco ya paso; no las necesita):
 *       slotOwner[idx] = 0
 *       xSemaphoreGive(sem)
 *  6. La casilla DESTINO permanece bloqueada (semaforo=0, slotOwner[target]=boat->id).
 *  7. *outNewSlot = targetIndex: el llamador actualiza boat->currentSlot.
 *
 * RAZON DEL ORDEN: Si liberaramos el origen antes de tomar el destino, otro barco
 * podria ocupar el origen y avanzar hacia el destino antes que nosotros. Al tomar
 * primero el destino (y las intermedias), garantizamos que el "tunel" es exclusivo.
 *
 * acquiredSems[]: buffer local en el stack de la funcion (maximo 16 elementos).
 *   Contiene los SemaphoreHandle_t (void*) de las casillas que ya se tomaron
 *   con exito en la iteracion actual, para poder hacer rollback si una falla.
 */
static bool ship_scheduler_try_move_range(ShipScheduler *scheduler, int startIndex, uint8_t steps, Boat *boat, int16_t *outNewSlot) {
  if (!scheduler || !boat || steps == 0) return false;
  if (steps > 16) return false; // Guarda: el buffer acquiredSems[] tiene capacidad maxima de 16.
  if (scheduler->listLength == 0 || !scheduler->slotOwner) return false;
  if ((SemaphoreHandle_t)scheduler->channelSlotsGuard == NULL) return false;

  int dir = (boat->origin == SIDE_LEFT) ? 1 : -1; // +1: izq->der; -1: der->izq.
  int currentIndex = boat->currentSlot; // Casilla actual del barco (indice en slotOwner[]).
  if (currentIndex < 0 || currentIndex >= scheduler->listLength) return false; // No esta en el canal.
  if (startIndex < 0 || startIndex >= scheduler->listLength) return false; // Inicio invalido.

  // targetIndex: casilla donde quedara el barco tras moverse 'steps' pasos.
  // Ejemplo: L->R, currentIndex=2, steps=2, dir=+1 -> targetIndex=4.
  int targetIndex = currentIndex + dir * steps;
  if (targetIndex < 0 || targetIndex >= scheduler->listLength) return false; // Fuera del canal.

  // startReserve: primera casilla nueva que el barco necesita (la siguiente a la actual).
  int startReserve = currentIndex + dir; // En el ejemplo: 3.

  // Verificacion previa rapida (sin bloquear todos los semaforos): si alguna casilla
  // del rango esta ocupada, abortar antes de entrar en la seccion critica larga.
  if (!ship_scheduler_wait_for_slot_range(scheduler, startReserve, steps, dir, boat)) return false;

  // Entrar en la seccion critica del canal para la operacion atomica completa.
  if (xSemaphoreTake((SemaphoreHandle_t)scheduler->channelSlotsGuard, pdMS_TO_TICKS(100)) != pdTRUE) {
    return false; // Otro hilo tiene el mutex; abortar en este tick.
  }

  // --- SECCION CRITICA ---

  // Re-verificar slotOwner[] dentro del mutex (la verificacion previa pudo quedar obsoleta).
  int idx = startReserve;
  for (uint8_t s = 0; s < steps; s++) {
    // slotOwner[idx] != 0 && != boat->id: casilla ocupada por OTRO barco.
    // (boat->id permite re-entrancia: el barco ya tenia reservada esa casilla)
    if (scheduler->slotOwner[idx] != 0 && scheduler->slotOwner[idx] != boat->id) {
      xSemaphoreGive((SemaphoreHandle_t)scheduler->channelSlotsGuard);
      return false; // Casilla ocupada: abortar.
    }
    idx += dir;
  }

  // Tomar los semaforos de TODAS las casillas nuevas (intermedias + destino) sin bloquear.
  // Si alguna falla, liberar las ya tomadas (rollback) y abortar.
  SemaphoreHandle_t acquiredSems[16]; // Punteros a Queue_t tomadas; en el stack de esta funcion.
  uint8_t acquiredCount = 0;          // Cuantas se han tomado exitosamente.
  idx = startReserve;
  for (uint8_t s = 0; s < steps; s++) {
    if ((uint16_t)idx != (uint16_t)currentIndex) { // No re-tomar el origen (ya lo tenemos).
      SemaphoreHandle_t sem = ship_scheduler_get_slot_semaphore((uint16_t)idx);
      if (!sem || xSemaphoreTake(sem, 0) != pdTRUE) {
        // No se pudo tomar esta casilla: rollback de todas las ya tomadas.
        for (uint8_t t = 0; t < acquiredCount; t++) xSemaphoreGive(acquiredSems[t]);
        xSemaphoreGive((SemaphoreHandle_t)scheduler->channelSlotsGuard);
        return false;
      }
      acquiredSems[acquiredCount++] = sem; // Guardar para posible rollback futuro.
      // Marcar propiedad: otro hilo leyendo slotOwner[idx] vera que ya esta asignada.
      ship_scheduler_lock_slot_owner(scheduler, (uint16_t)idx, boat->id);
    }
    idx += dir;
  }

  // LIBERAR LA CASILLA ORIGEN: el barco ya no esta ahi.
  if (scheduler->slotOwner[currentIndex] == boat->id) {
    ship_scheduler_unlock_slot_owner(scheduler, (uint16_t)currentIndex); // slotOwner[orig] = 0.
    SemaphoreHandle_t currentSem = ship_scheduler_get_slot_semaphore((uint16_t)currentIndex);
    if (currentSem) xSemaphoreGive(currentSem); // Semaforo del origen: 0 -> 1 (disponible).
  }

  // LIBERAR CASILLAS INTERMEDIAS (todas menos la ultima = destino).
  // El barco 'atravieso' esas casillas pero no se detiene en ellas.
  idx = startReserve;
  for (uint8_t s = 0; s < steps - 1; s++) {
    ship_scheduler_unlock_slot_owner(scheduler, (uint16_t)idx); // slotOwner[interm] = 0.
    SemaphoreHandle_t sem = ship_scheduler_get_slot_semaphore((uint16_t)idx);
    if (sem) xSemaphoreGive(sem); // Semaforo del intermedio: 0 -> 1.
    idx += dir;
  }
  // idx == targetIndex: la casilla destino permanece bloqueada (semaforo=0, slotOwner=boat->id).

  xSemaphoreGive((SemaphoreHandle_t)scheduler->channelSlotsGuard); // Liberar mutex del canal.

  // Escribir la nueva posicion del barco a traves del puntero de salida.
  // *outNewSlot = targetIndex actualiza boat->currentSlot en el llamador.
  if (outNewSlot) *outNewSlot = (int16_t)targetIndex;
  return true; // Movimiento atomico completado.
}

/*
 * candidate_is_better — comparador para el algoritmo de planificacion.
 *
 * Recibe dos punteros a Boat (const Boat*): accede a sus campos sin modificarlos.
 * La funcion no desreferencia punteros de array; compara los campos escalares
 * directamente dentro del struct.
 *
 * candidate->arrivalOrder: uint16_t. Contador monotonico; el primero en llegar
 *   tiene el valor mas bajo. Es el desempatador universal.
 *
 * candidate->remainingMillis: unsigned long. Cuanto tiempo le falta al barco
 *   para terminar el canal. Puede ser 1 (modo visual-only: sigue moviendose pero
 *   no descuenta tiempo). El valor 1 se trata como 'practicamente terminado';
 *   STRN usa 'remainingMillis > 1' para distinguirlo de 0 (finalizado).
 *
 * gScheduler: puntero global al scheduler activo. Se usa en STRN para estimar
 *   serviceMillis si el barco aun no lo tiene calculado.
 */
static bool candidate_is_better(ShipAlgo algo, const Boat *candidate, const Boat *best) { // Compara dos candidatos segun la politica de planificacion.
  if (!candidate) return false; // candidate == NULL: no puede ganar.
  if (!best) return true;       // Sin competidor, cualquier candidato valido gana.

  if (algo == ALG_FCFS || algo == ALG_RR) {
    // FCFS/RR: primero en llegar, primero en ser atendido.
    // arrivalOrder es el orden de encolado global (campo uint16_t en el struct Boat).
    return candidate->arrivalOrder < best->arrivalOrder;
  }

  if (algo == ALG_SJF) {
    // SJF: menor tiempo de servicio TOTAL (no restante). Uso tipico: no-preemptivo.
    // Si serviceMillis == 0 el valor no fue calculado aun: se le asigna ULONG_MAX
    // para que pierda contra cualquier barco que si tenga el valor.
    unsigned long candSvc = candidate->serviceMillis > 0 ? candidate->serviceMillis : ULONG_MAX;
    unsigned long bestSvc = best->serviceMillis > 0 ? best->serviceMillis : ULONG_MAX;
    if (candSvc != bestSvc) return candSvc < bestSvc; // Menor servicio gana.
    return candidate->arrivalOrder < best->arrivalOrder; // Desempata por llegada.
  }

  if (algo == ALG_STRN) {
    // STRN (Shortest Time Remaining Next): gana el barco con MENOS tiempo restante.
    // Jerarquia de fuentes del tiempo restante:
    //   1. remainingMillis > 1: valor real actualizado en tiempo de ejecucion.
    //   2. serviceMillis > 0: estimacion calculada al encolar (si no empezo a correr).
    //   3. Estimacion por parametros del canal (formula de velocidad * distancia).
    unsigned long candRem = 0UL;
    unsigned long bestRem = 0UL;
    if (candidate->remainingMillis > 1) candRem = candidate->remainingMillis;
    else if (candidate->serviceMillis > 0) candRem = candidate->serviceMillis;
    else candRem = ship_scheduler_estimate_service_millis(gScheduler, candidate); // gScheduler = puntero global.

    if (best->remainingMillis > 1) bestRem = best->remainingMillis;
    else if (best->serviceMillis > 0) bestRem = best->serviceMillis;
    else bestRem = ship_scheduler_estimate_service_millis(gScheduler, best);

    if (candRem != bestRem) return candRem < bestRem;
    // Desempate 1: mayor stepSize = avanza mas rapido = termina antes en la practica.
    if (candidate->stepSize != best->stepSize) return candidate->stepSize > best->stepSize;
    return candidate->arrivalOrder < best->arrivalOrder; // Desempate final.
  }

  if (algo == ALG_EDF) {
    // EDF (Earliest Deadline First): gana el barco cuyo deadline esta MAS PROXIMO.
    // Se usa el tiempo RESTANTE hasta el deadline (deadlineMillis - now), no el
    // deadline absoluto, para manejar correctamente el caso de deadlines pasados
    // (se tratan como urgencia maxima = 0).
    unsigned long now = millis(); // Lectura del contador de tiempo del sistema (ms desde boot).
    // Si deadlineMillis < now: el deadline ya paso; tiempo restante = 0 (urgencia maxima).
    unsigned long candRem = candidate->deadlineMillis > now ? candidate->deadlineMillis - now : 0UL;
    unsigned long bestRem = best->deadlineMillis > now ? best->deadlineMillis - now : 0UL;
    if (candRem != bestRem) return candRem < bestRem; // Menos tiempo = mas urgente.
    return candidate->arrivalOrder < best->arrivalOrder;
  }

  if (algo == ALG_PRIORITY) {
    // PRIORITY: mayor valor numerico de 'priority' = mayor prioridad (al reves de UNIX).
    if (candidate->priority != best->priority) return candidate->priority > best->priority;
    return candidate->arrivalOrder < best->arrivalOrder;
  }

  return candidate->arrivalOrder < best->arrivalOrder; // Fallback generico.
} // Fin de candidate_is_better.

static unsigned long ship_scheduler_boat_elapsed_millis(const Boat *boat) { // Calcula elapsed de un barco.
  if (!boat) return 0; // Valida puntero.
  if (boat->serviceMillis <= boat->remainingMillis) return 0; // Evita underflow.
  return boat->serviceMillis - boat->remainingMillis; // Retorna elapsed.
} // Fin de ship_scheduler_boat_elapsed_millis.

/*
 * ship_scheduler_estimate_service_millis — estima cuanto tardara un barco en cruzar.
 *
 * Formula: serviceMs = (channelLengthMeters / boatSpeedMetersPerSec) * 1000.0
 *   - channelLengthMeters: uint32_t en el scheduler, longitud real del canal.
 *   - boatSpeedMetersPerSec: uint32_t en el scheduler, velocidad base.
 *   - La division produce segundos de punto flotante; *1000 convierte a ms.
 *   - Se usa float (32-bit) para la aritmetica intermedia; el resultado se
 *     trunca a unsigned long con redondeo (+ 0.5f).
 *
 * typeFactor: modificador segun el tipo de barco:
 *   - BOAT_NORMAL: 1.0 (velocidad base completa)
 *   - BOAT_PESQUERA: 0.6 (un poco mas lenta; mas tiempo en el canal)
 *   - BOAT_PATRULLA: 0.3 (la mas rapida; menos tiempo en el canal)
 *   El factor se multiplica por baseMs para obtener el tiempo ajustado.
 *
 * Minimo de 50ms: evita que una division con valores muy pequenos produzca
 * un servicio 0 o 1ms que haria que los barcos 'finalizaran' instantaneamente
 * antes de poder moverse siquiera una casilla.
 */
static unsigned long ship_scheduler_estimate_service_millis(const ShipScheduler *scheduler, const Boat *boat) {
  if (!scheduler || !boat) return 0;
  if (scheduler->channelLengthMeters == 0 || scheduler->boatSpeedMetersPerSec == 0) return 5000UL;

  // Calculo del tiempo base para cruzar TODO el canal a velocidad base.
  // (uint32_t * 1000.0f): conversion explicita a float antes de dividir para no
  // perder precision con division entera (si ambos fueran uint32_t la division
  // seria entera y se perderian los decimales).
  float fullChannelMs = ((float)scheduler->channelLengthMeters * 1000.0f) / (float)scheduler->boatSpeedMetersPerSec;
  unsigned long baseMs = (unsigned long)(fullChannelMs + 0.5f); // +0.5 = redondeo al ms mas cercano.
  if (baseMs == 0) baseMs = 1; // Caso degenerado: canal/velocidad muy pequenos.

  float typeFactor = 1.0f;
  switch (boat->type) {
    case BOAT_NORMAL:   typeFactor = 1.0f; break; // Velocidad estandar.
    case BOAT_PESQUERA: typeFactor = 0.6f; break; // 60% de la velocidad base -> mas tiempo.
    case BOAT_PATRULLA: typeFactor = 0.3f; break; // 30% del tiempo base -> muy rapida.
    default:            typeFactor = 1.0f; break;
  }

  // Aplicar factor: baseMs * 1.0 = normal; baseMs * 0.3 = patrulla (usa menos tiempo).
  unsigned long estimated = (unsigned long)((float)baseMs * typeFactor);
  if (estimated == 0) estimated = baseMs; // Nunca retornar 0.
  return estimated < 50 ? 50 : estimated; // Piso de 50ms para evitar finalizacion instantanea.
} // Fin de ship_scheduler_estimate_service_millis.

/*
 * ship_scheduler_compute_default_deadline — calcula un deadline heuristico para EDF.
 *
 * Heuristica: deadline = now + (serviceMs * 2).
 * El factor 2 da un margen conservador: si el barco usa todo su serviceMs sin
 * interrupciones, le sobra 1x de margen. Es suficiente para que EDF distinga
 * entre barcos urgentes (poco tiempo al deadline) y los que aun tienen tiempo.
 * Se puede ajustar en la configuracion del canal si la heuristica no se adapta.
 */
static unsigned long ship_scheduler_compute_default_deadline(const ShipScheduler *scheduler, const Boat *boat) {
  unsigned long now = millis(); // ms desde el arranque del ESP32.
  unsigned long serviceMs = boat && boat->serviceMillis > 0
                            ? boat->serviceMillis
                            : ship_scheduler_estimate_service_millis(scheduler, boat);
  if (serviceMs == 0) serviceMs = 50UL; // Evita un deadline == now (urgencia maxima incorrecta).
  return now + (serviceMs * 2UL); // Deadline = ahora + 2 veces el tiempo de servicio estimado.
} // Fin de ship_scheduler_compute_default_deadline.

/*
 * ship_scheduler_sync_primary_active — mantiene coherente el puntero de atajos.
 *
 * scheduler->activeBoats[0] es siempre el barco primario (el que tiene permiso
 * de moverse en algoritmos que solo permiten un activo, o el que tiene el quantum
 * en RR). El campo scheduler->activeBoat es un alias al mismo puntero para acceso
 * rapido sin indexar el array.
 *
 * scheduler->activeBoats[0]: desreferencia del puntero almacenado en la posicion 0
 *   del array embebido en el struct. El array esta DENTRO del struct ShipScheduler,
 *   no en el heap separado; su direccion base es &scheduler->activeBoats[0].
 */
static void ship_scheduler_sync_primary_active(ShipScheduler *scheduler) {
  if (!scheduler) return;
  if (scheduler->activeCount > 0) {
    // Apuntar scheduler->activeBoat a la misma direccion que activeBoats[0].
    // No se copia el struct Boat; solo se copia el puntero (4/8 bytes).
    scheduler->activeBoat = scheduler->activeBoats[0];
    scheduler->hasActiveBoat = true;
  } else {
    scheduler->activeBoat = NULL;  // Sin activos, el alias queda en NULL.
    scheduler->hasActiveBoat = false;
  }
} // Fin de ship_scheduler_sync_primary_active.

/*
 * ship_scheduler_sync_rr_permissions — ajusta allowedToMove para todos los activos en RR.
 *
 * En RR solo el barco primario (activeBoats[0] = activeBoat) puede moverse.
 * Todos los demas activos tienen allowedToMove = false y esperan NOTIF_CMD_RUN.
 * Esta funcion se llama cada vez que cambia el primario para garantizar que la
 * bandera sea coherente con el estado real.
 *
 * scheduler->activeBoats[i]: accede al puntero del i-esimo barco activo.
 *   active == scheduler->activeBoat: comparacion de DIRECCIONES (no valores).
 *   Si apuntan al mismo Boat en memoria, es el mismo barco.
 */
static void ship_scheduler_sync_rr_permissions(ShipScheduler *scheduler) {
  if (!scheduler || scheduler->algorithm != ALG_RR) return;
  for (uint8_t i = 0; i < scheduler->activeCount; i++) {
    Boat *active = scheduler->activeBoats[i]; // Lee el Boat* en la posicion i.
    if (!active) continue;
    // Solo el primario (activeBoat) tiene permiso; los demas se bloquean.
    active->allowedToMove = (active == scheduler->activeBoat);
  }
} // Fin de ship_scheduler_sync_rr_permissions.

/*
 * ship_scheduler_promote_active_to_front — mueve un barco a la posicion 0 del array.
 *
 * El array activeBoats[] es un array de Boat* EMBEBIDO en el struct ShipScheduler.
 * Para promover a la posicion 0 se hace un desplazamiento in-place:
 *   activeBoats[foundIndex] -> se guarda en una variable local
 *   for i = foundIndex..1: activeBoats[i] = activeBoats[i-1]  (desplazamiento hacia la derecha)
 *   activeBoats[0] = promoted  (el barco queda al frente)
 *
 * Esto es equivalente a una rotacion parcial del array, manteniendo el orden
 * relativo de los demas elementos (los que estaban antes del promovido quedan
 * en las posiciones 1..foundIndex en el mismo orden original).
 *
 * Despues del desplazamiento, activeBoat se actualiza para apuntar al nuevo
 * primer elemento (mismo struct Boat, direccion sin cambios; solo cambia el indice).
 */
static void ship_scheduler_promote_active_to_front(ShipScheduler *scheduler, Boat *boat) {
  if (!scheduler || !boat || scheduler->activeCount == 0) return;
  int foundIndex = -1;
  // Buscar la posicion del barco en el array por comparacion de punteros.
  for (uint8_t i = 0; i < scheduler->activeCount; i++) {
    if (scheduler->activeBoats[i] == boat) { // Comparar DIRECCIONES (no contenido).
      foundIndex = (int)i;
      break;
    }
  }
  if (foundIndex < 0) return; // El barco no estaba en la lista de activos.
  if (foundIndex > 0) {
    Boat *promoted = scheduler->activeBoats[foundIndex]; // Guardar el puntero.
    // Desplazar todos los elementos anteriores un lugar hacia la derecha.
    // Esto libera la posicion 0 para el promovido.
    for (int i = foundIndex; i > 0; i--) {
      scheduler->activeBoats[i] = scheduler->activeBoats[i - 1]; // Copiar puntero (no struct).
    }
    scheduler->activeBoats[0] = promoted; // Colocar el barco promovido al frente.
  }
  // Actualizar el alias de acceso rapido.
  scheduler->activeBoat = scheduler->activeBoats[0];
  scheduler->hasActiveBoat = (scheduler->activeBoat != NULL);
}

/*
 * ship_scheduler_reset_active_quantum — reinicia el cronometro del quantum activo.
 *
 * Dos campos controlan el quantum en el scheduler:
 *  - activeQuantumAccumulatedMillis: unsigned long. Tiempo total acumulado del
 *    quantum en pausas previas. Se resetea a 0 para dar un quantum completo nuevo.
 *  - activeQuantumStartedAt: unsigned long. Marca de tiempo (ms desde boot) en
 *    que empezo el tramo actual del quantum. 0 = no esta corriendo.
 *
 * Se llama cuando un barco TERMINA o cuando el sucesor NO EXISTE (para dar un
 * quantum completo al barco que sigue en lugar de heredar el tiempo ya consumido).
 */
static void ship_scheduler_reset_active_quantum(ShipScheduler *scheduler) {
  if (!scheduler) return;
  scheduler->activeQuantumAccumulatedMillis = 0; // Borrar tiempo acumulado de pausas anteriores.
  scheduler->activeQuantumStartedAt = 0;          // El quantum no esta en curso.
}

/*
 * ship_scheduler_rr_next_active — selecciona el siguiente activo en turno circular RR.
 *
 * Usa el campo scheduler->rrTurnIndex (uint8_t) como indice del turno actual.
 * La siguiente seleccion empieza en (rrTurnIndex + 1) % activeCount.
 *
 * PREFERENCIA: Si un candidato puede avanzar sus 'need' pasos completos (casillas
 * adelante libres en slotOwner[]), se elige inmediatamente. Esto evita asignar el
 * quantum a un barco que se bloqueara en el primer tick (desperdicio de tiempo).
 *
 * scheduler->slotOwner[checkSlot]: lectura del byte de propiedad de la casilla
 *   checkSlot. Si es 0 o es el propio id del candidato (caso de re-entrancia),
 *   la casilla se considera libre para ese barco.
 *
 * Fallback: si ningun activo puede moverse (canal congestionado), se devuelve el
 *   siguiente en orden circular de todas formas para no freezar el scheduler.
 */
static Boat *ship_scheduler_rr_next_active(ShipScheduler *scheduler) {
  if (!scheduler || scheduler->activeCount == 0) return NULL;
  if (scheduler->activeCount == 1) {
    scheduler->rrTurnIndex = 0; // Solo hay un activo; siempre es el mismo.
    return scheduler->activeBoats[0];
  }
  // Calcular el punto de partida de la busqueda circular.
  // (rrTurnIndex + 1) % activeCount: evita desbordamiento del contador uint8_t.
  uint8_t start = (uint8_t)((scheduler->rrTurnIndex + 1) % scheduler->activeCount);
  Boat *fallback = NULL; // Primer candidato valido como respaldo si ninguno puede moverse.
  for (uint8_t offset = 0; offset < scheduler->activeCount; offset++) {
    uint8_t idx = (uint8_t)((start + offset) % scheduler->activeCount); // Indice circular.
    Boat *candidate = scheduler->activeBoats[idx]; // Lee el puntero del candidato.
    if (!candidate || candidate->currentSlot < 0) continue; // Sin posicion valida en el canal.
    if (!fallback) fallback = candidate; // Primer candidato: guardarlo como respaldo.
    // Calcular cuantas casillas necesita avanzar el candidato.
    int dir = (candidate->origin == SIDE_LEFT) ? 1 : -1;
    int endIndex = (candidate->origin == SIDE_LEFT) ? (int)(scheduler->listLength - 1) : 0;
    int remSlots = (candidate->origin == SIDE_LEFT)
                   ? (endIndex - candidate->currentSlot)
                   : (candidate->currentSlot - endIndex);
    if (remSlots <= 0) {
      // Barco en la casilla final: que reciba el turno para poder finalizar.
      scheduler->rrTurnIndex = idx;
      return candidate;
    }
    // need = minimo entre las casillas restantes y el stepSize del barco.
    uint8_t need = (uint8_t)((remSlots < (int)candidate->stepSize) ? remSlots : candidate->stepSize);
    bool canMove = true;
    int checkSlot = candidate->currentSlot + dir; // Primera casilla a verificar.
    for (uint8_t s = 0; s < need; s++) {
      if (checkSlot < 0 || checkSlot >= (int)scheduler->listLength) { canMove = false; break; }
      // slotOwner[checkSlot] != 0 y != candidate->id: ocupado por otro barco.
      if (scheduler->slotOwner && scheduler->slotOwner[checkSlot] != 0
          && scheduler->slotOwner[checkSlot] != candidate->id) { canMove = false; break; }
      checkSlot += dir;
    }
    if (canMove) {
      scheduler->rrTurnIndex = idx; // Actualizar el indice de turno.
      return candidate; // Elegido: puede avanzar sus steps.
    }
  }
  // Fallback: ninguno puede moverse; rotar al siguiente de todas formas.
  if (fallback) {
    for (uint8_t idx = 0; idx < scheduler->activeCount; idx++) {
      if (scheduler->activeBoats[idx] == fallback) { scheduler->rrTurnIndex = idx; break; }
    }
  }
  return fallback;
}

/*
 * ship_scheduler_freeze_active_quantum — congela el cronometro del quantum.
 *
 * Cuando un barco se pausa (PAUSE) o cede su turno (yield), el tiempo que ya
 * lleva corriendo se acumula en activeQuantumAccumulatedMillis para que al
 * reanudarse se sume correctamente.
 *
 * activeQuantumAccumulatedMillis += now - activeQuantumStartedAt:
 *   - now: lectura del contador de hardware (millis()) en el momento del congelado.
 *   - La resta calcula el tramo que el quantum estuvo corriendo en este intervalo.
 *   - La suma acumula todos los tramos previos + el actual.
 * activeQuantumStartedAt = 0: indica que el quantum ya NO esta en curso.
 *   Un valor 0 evita que get_active_elapsed_millis cuente un tramo de duracion
 *   incorrecta si se llama mientras el quantum esta pausado.
 */
static void ship_scheduler_freeze_active_quantum(ShipScheduler *scheduler) {
  if (!scheduler) return;
  if (scheduler->activeQuantumStartedAt > 0) { // Solo acumular si el quantum estaba activo.
    unsigned long now = millis();
    if (now >= scheduler->activeQuantumStartedAt) {
      // Acumular: tiempo transcurrido desde que empezo el tramo actual.
      scheduler->activeQuantumAccumulatedMillis += now - scheduler->activeQuantumStartedAt;
    }
    scheduler->activeQuantumStartedAt = 0; // Marcar el quantum como detenido.
  }
}

/*
 * ship_scheduler_resume_active_quantum — reanuda el cronometro del quantum.
 *
 * Cuando el barco recibe NOTIF_CMD_RUN y empieza a correr, se registra la
 * marca de tiempo actual. get_active_elapsed_millis() sumara este tramo
 * en tiempo real: accumulated + (now - startedAt).
 *
 * La guardia 'activeQuantumStartedAt == 0' evita registrar una nueva marca
 * si el quantum ya estaba corriendo (lo que producirìa doble conteo).
 */
static void ship_scheduler_resume_active_quantum(ShipScheduler *scheduler) {
  if (!scheduler) return;
  if (scheduler->activeQuantumStartedAt == 0) // Solo arrancar si no esta corriendo ya.
    scheduler->activeQuantumStartedAt = millis(); // Marca de inicio del tramo actual.
}

/*
 * ship_scheduler_add_active — agrega un barco a la lista de activos.
 *
 * scheduler->activeBoats[scheduler->activeCount++] = boat:
 *   - activeCount es el indice de la proxima posicion libre (0-based).
 *   - Se escribe el puntero 'boat' en esa posicion y luego se incrementa el contador.
 *   - El barco NO se copia: solo se guarda su direccion de memoria.
 *   - Limite: MAX_BOATS (definido en ShipScheduler.h); si se supera, se ignora la operacion.
 *
 * sync_primary_active: actualiza el alias scheduler->activeBoat = activeBoats[0].
 *   El nuevo barco se agrega al FINAL, no al frente; el primario sigue siendo el mismo
 *   hasta que promote_active_to_front lo cambie explicitamente.
 */
static void ship_scheduler_add_active(ShipScheduler *scheduler, Boat *boat) {
  if (!scheduler || !boat) return;
  if (scheduler->activeCount >= MAX_BOATS) return; // Evita escribir fuera del array.
  // Guardar la direccion del barco en la siguiente posicion libre del array.
  scheduler->activeBoats[scheduler->activeCount++] = boat;
  ship_scheduler_sync_primary_active(scheduler); // Actualizar el alias activeBoat.
} // Fin de ship_scheduler_add_active.

/*
 * ship_scheduler_remove_active — quita un barco de la lista de activos.
 *
 * Algoritmo de compactacion:
 *   1. Buscar el indice del barco por comparacion de punteros.
 *   2. Desplazar todos los elementos posteriores una posicion hacia atras:
 *      for j = removedIndex+1 .. activeCount-1: activeBoats[j-1] = activeBoats[j]
 *      Esto es O(n); para MAX_BOATS pequeno (tipicamente 8-16) es aceptable.
 *   3. Escribir NULL en la ultima posicion (evita puntero dangling en la posicion liberada).
 *   4. Decrementar activeCount.
 *
 * Ajuste de rrTurnIndex:
 *   - Si el barco eliminado estaba ANTES del indice actual, el array se comprimo
 *     y todos los indices > removedIndex se desplazaron -1. Para que rrTurnIndex
 *     siga apuntando al mismo barco logico, se decrementa en 1.
 *   - Si rrTurnIndex queda fuera de rango (>= activeCount), se ajusta al ultimo.
 */
static void ship_scheduler_remove_active(ShipScheduler *scheduler, Boat *boat) {
  if (!scheduler || !boat || scheduler->activeCount == 0) return;
  int removedIndex = -1;
  for (uint8_t i = 0; i < scheduler->activeCount; i++) {
    if (scheduler->activeBoats[i] == boat) { // Comparar direcciones (punteros).
      removedIndex = (int)i;
      // Compactar: mover cada elemento posterior un lugar hacia atras.
      for (uint8_t j = i + 1; j < scheduler->activeCount; j++) {
        scheduler->activeBoats[j - 1] = scheduler->activeBoats[j]; // Copiar puntero.
      }
      scheduler->activeBoats[scheduler->activeCount - 1] = NULL; // Limpiar la posicion liberada.
      scheduler->activeCount--;
      break;
    }
  }
  ship_scheduler_sync_primary_active(scheduler); // Actualizar el alias activeBoat.
  // Mantener rrTurnIndex coherente con el nuevo tamano del array.
  if (scheduler->activeCount == 0) {
    scheduler->rrTurnIndex = 0; // Sin activos, resetear al inicio.
  } else if (removedIndex >= 0 && (int)scheduler->rrTurnIndex > removedIndex) {
    // El barco eliminado estaba ANTES del turno actual: compensar el desplazamiento.
    scheduler->rrTurnIndex--;
  } else if (scheduler->rrTurnIndex >= scheduler->activeCount) {
    scheduler->rrTurnIndex = (uint8_t)(scheduler->activeCount - 1); // Clamping al ultimo.
  }
} // Fin de ship_scheduler_remove_active.

static bool ship_scheduler_is_tico_safe(const ShipScheduler *scheduler, const Boat *candidate) { // Evalua seguridad TICO.
  if (!scheduler || !candidate) return false; // Valida punteros.
  if (scheduler->activeCount == 0) return true; // Sin activos, no hay riesgo.

  BoatSide side = candidate->origin; // Lado del candidato.
  for (uint8_t i = 0; i < scheduler->activeCount; i++) { // Evalua contra cada activo.
    Boat *active = scheduler->activeBoats[i];
    if (!active) continue; // Salta nulos.
    if (active->origin != side) return false; // Evita sentidos opuestos.
    if (!active->allowedToMove) return false; // Evita iniciar si un activo esta detenido.

    unsigned long elapsed = ship_scheduler_boat_elapsed_millis(active); // Progreso del activo.
    float pairFactor = 1.0f;
    if (scheduler) {
      BoatType at = active->type;
      BoatType ct = candidate->type;
      if (at >= 0 && at < 3 && ct >= 0 && ct < 3) pairFactor = scheduler->ticoMarginFactor[at][ct];
    }
    float serviceScale = 1.0f;
    float speedScale = 1.0f;
    if (scheduler && scheduler->boatSpeedMetersPerSec > 0) speedScale = 18.0f / (float)scheduler->boatSpeedMetersPerSec;
    unsigned long minGapMs = (unsigned long)((float)TICO_INITIAL_MARGIN * pairFactor * serviceScale * speedScale);
    if (elapsed < minGapMs) return false; // Evita solapamiento inicial.

    if (candidate->serviceMillis < active->serviceMillis && active->serviceMillis > 0) { // Riesgo si el candidato es mas rapido.
      float gapFraction = (float)minGapMs / (float)active->serviceMillis; // Margen relativo.
      if (gapFraction > 0.02f) gapFraction = TICO_SAFETY_MARGIN; // Evita margenes excesivos.
      float ratio = (float)candidate->serviceMillis / (float)active->serviceMillis; // Relacion de tiempos.
      float requiredElapsed = (1.0f - (1.0f - gapFraction) * ratio) * (float)active->serviceMillis; // Elapsed minimo.
      if ((float)elapsed < requiredElapsed) return false; // No es seguro iniciar.
    }
  }

  return true; // Cumple condiciones de seguridad.
} // Fin de ship_scheduler_is_tico_safe.

/*
 * ship_scheduler_release_range — libera un rango de casillas del barco y notifica.
 *
 * Operacion inversa a try_reserve_range:
 *   1. Tomar el mutex del canal (channelSlotsGuard) para operar atomicamente.
 *   2. Para cada casilla del rango:
 *      a. Verificar que slotOwner[idx] == boat->id (solo liberar si la posee este barco).
 *      b. unlock_slot_owner: slotOwner[idx] = 0.
 *      c. xSemaphoreGive(slotSem): semaforo 0 -> 1 (disponible para el siguiente barco).
 *   3. Liberar el mutex.
 *   4. NOTIF_CMD_SLOT_UPDATE a todos los activos: les senala que el estado del canal
 *      cambio para que reintenten sus movimientos sin esperar el proximo tick (50ms).
 *      Esto evita un busy-wait: en lugar de que boatTask haga un loop de 100ms
 *      hasta que la casilla este libre, recibe la notificacion y retoma inmediatamente.
 *
 * Por que notificar a TODOS los activos y no solo al que esperaba esa casilla:
 *   En TICO varios barcos pueden estar esperando distintas casillas del mismo rango.
 *   No sabemos cual(es) estaban bloqueados; notificar a todos es O(n) pero correcto.
 */
void ship_scheduler_release_range(ShipScheduler *scheduler, int startIndex, uint8_t steps, Boat *boat) {
  if (!scheduler || !boat || steps == 0) return;
  if (scheduler->listLength == 0 || !scheduler->slotOwner) return;
  if ((SemaphoreHandle_t)scheduler->channelSlotsGuard == NULL) return;

  // Tomar el mutex del canal para la liberacion atomica.
  if (xSemaphoreTake((SemaphoreHandle_t)scheduler->channelSlotsGuard, pdMS_TO_TICKS(100)) != pdTRUE) return;
  int dir = (boat->origin == SIDE_LEFT) ? 1 : -1;
  int endIndex = startIndex + dir * (steps - 1); // Ultimo indice del rango.
  if (startIndex < 0 || startIndex >= scheduler->listLength || endIndex < 0 || endIndex >= scheduler->listLength) {
    xSemaphoreGive((SemaphoreHandle_t)scheduler->channelSlotsGuard); // Liberar mutex aunque el rango sea invalido.
    // Notificar de todas formas para que los barcos bloqueados reintenten.
    for (uint8_t i = 0; i < scheduler->activeCount; i++) {
      Boat *active = scheduler->activeBoats[i];
      if (!active) continue;
      // NOTIF_CMD_SLOT_UPDATE: valor especial que boatTask interpreta como
      // 'el canal cambio, intenta moverte de nuevo sin esperar el timeout de 100ms'.
      if (active->taskHandle) safe_task_notify(active->taskHandle, NOTIF_CMD_SLOT_UPDATE);
    }
    return;
  }
  int idx = startIndex;
  for (uint8_t s = 0; s < steps; s++) {
    // Solo liberar si el barco 'boat' es realmente el propietario de esta casilla.
    // Evita que un barco libere casillas que no le pertenecen (por concurrencia).
    if (scheduler->slotOwner[idx] == boat->id) {
      ship_scheduler_unlock_slot_owner(scheduler, (uint16_t)idx); // slotOwner[idx] = 0.
      SemaphoreHandle_t slotSem = ship_scheduler_get_slot_semaphore((uint16_t)idx);
      if (slotSem) xSemaphoreGive(slotSem); // Semaforo de la casilla: 0 -> 1 (libre).
    }
    idx += dir;
  }
  xSemaphoreGive((SemaphoreHandle_t)scheduler->channelSlotsGuard); // Liberar el mutex.
  // No notificar aqui: el llamador es responsable si necesita notificar a otros barcos.
}

void ship_scheduler_load_channel_config(ShipScheduler *scheduler, const char *path) { // Lee channel_config.txt con formato de comandos
  if (!scheduler) return;
  if (!path) path = "channel_config.txt"; // Ruta por defecto relativa al ejecutable

  FILE *f = fopen(path, "r");
  if (!f) {
    // Usa valores por defecto
    scheduler->listLength = DEFAULT_LIST_LENGTH;
    scheduler->visualChannelLength = DEFAULT_VISUAL_CHANNEL_LENGTH;
    ship_logf("[CONFIG] No se encontro %s, usando defaults list=%u visual=%u\n", path, scheduler->listLength, scheduler->visualChannelLength);
    return;
  }

  char line[160];
  uint16_t applied = 0;
  while (fgets(line, sizeof(line), f)) {
    // Trim basico y salto de comentarios
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t')) {
      line[len - 1] = '\0';
      len--;
    }
    char *cursor = line;
    while (*cursor == ' ' || *cursor == '\t') cursor++;
    if (*cursor == '\0' || *cursor == '#') continue;
    process_serial_command(scheduler, cursor);
    applied++;
  }
  fclose(f);
  ship_logf("[CONFIG] Cargado %s (%u lineas)\n", path, applied);
}

void ship_scheduler_demo_clear(ShipScheduler *scheduler) {
  if (!scheduler) return;
  scheduler->demoCount = 0;
}

bool ship_scheduler_demo_add(ShipScheduler *scheduler, BoatSide side, BoatType type, uint8_t priority, uint8_t stepSize) {
  if (!scheduler) return false;
  if (scheduler->demoCount >= MAX_BOATS) return false;
  uint8_t idx = scheduler->demoCount++;
  scheduler->demoSide[idx] = side;
  scheduler->demoType[idx] = type;
  scheduler->demoPrio[idx] = priority;
  scheduler->demoStep[idx] = stepSize;
  return true;
}

/*
 * ship_scheduler_rebuild_slots — reconstruye el array slotOwner[] y los semaforos.
 *
 * Se llama cuando cambia listLength (al cargar channel_config.txt o reset).
 *
 * SECUENCIA DE MEMORIA:
 *  1. free(scheduler->slotOwner):
 *     Devuelve al heap de C el bloque de (listLength_anterior * sizeof(uint8_t)) bytes.
 *     slotOwner queda apuntando a memoria liberada (puntero dangling).
 *  2. scheduler->slotOwner = NULL: invalida el puntero dangling.
 *  3. malloc(scheduler->listLength * sizeof(uint8_t)):
 *     Reserva un NUEVO bloque contiguo de listLength bytes en el heap de C.
 *     Retorna la direccion base del bloque (o NULL si el heap esta agotado).
 *     El resultado se guarda en scheduler->slotOwner (campo uint8_t* del struct).
 *  4. for i = 0..listLength-1: slotOwner[i] = 0:
 *     Inicializa cada byte a 0 (casilla libre). malloc NO inicializa; si no se
 *     hace explicitamente, los bytes contienen basura del heap (valores arbitrarios).
 *  5. ship_scheduler_init_slot_resources(listLength):
 *     Recrea el array global gSlotSemaphores[] con listLength semaforos.
 *  6. xSemaphoreCreateMutex():
 *     Crea el mutex del canal (channelSlotsGuard) si no existe aun.
 *     Se almacena como void* en scheduler->channelSlotsGuard para no requerir
 *     que el .h incluya FreeRTOS headers. Se castea a SemaphoreHandle_t en cada uso.
 */
void ship_scheduler_rebuild_slots(ShipScheduler *scheduler) {
  if (!scheduler) return;
  if (scheduler->listLength == 0) scheduler->listLength = DEFAULT_LIST_LENGTH; // Valor minimo.
  if (scheduler->visualChannelLength == 0) scheduler->visualChannelLength = scheduler->listLength;

  // Destruir los semaforos del canal anterior (si existian).
  ship_scheduler_destroy_slot_resources();

  // Liberar el array slotOwner anterior.
  if (scheduler->slotOwner) {
    free(scheduler->slotOwner); // Devuelve al heap el bloque de bytes.
    scheduler->slotOwner = NULL; // Invalida el puntero para evitar uso accidental.
  }

  // Reservar un nuevo bloque de (listLength * 1 byte) para slotOwner.
  // malloc retorna void*; el cast (uint8_t*) es necesario en C++ (Arduino).
  scheduler->slotOwner = (uint8_t *)malloc(scheduler->listLength * sizeof(uint8_t));
  if (scheduler->slotOwner) {
    // Inicializar a 0: todas las casillas libres (sin propietario).
    for (uint16_t i = 0; i < scheduler->listLength; i++) scheduler->slotOwner[i] = 0;
  }
  // Crear los semaforos binarios del canal (uno por casilla).
  ship_scheduler_init_slot_resources(scheduler->listLength);
  // Crear el mutex del canal si no existe aun.
  // channelSlotsGuard se almacena como void* para no exponer SemaphoreHandle_t en el .h.
  if (!scheduler->channelSlotsGuard)
    scheduler->channelSlotsGuard = (void *)xSemaphoreCreateMutex();
}

void ship_scheduler_set_list_length(ShipScheduler *scheduler, uint16_t listLength) {
  if (!scheduler) return;
  scheduler->listLength = listLength > 0 ? listLength : DEFAULT_LIST_LENGTH;
}

uint16_t ship_scheduler_get_list_length(const ShipScheduler *scheduler) {
  return scheduler ? scheduler->listLength : DEFAULT_LIST_LENGTH;
}

void ship_scheduler_set_visual_channel_length(ShipScheduler *scheduler, uint16_t visualLength) {
  if (!scheduler) return;
  scheduler->visualChannelLength = visualLength > 0 ? visualLength : scheduler->listLength;
}

uint16_t ship_scheduler_get_visual_channel_length(const ShipScheduler *scheduler) {
  return scheduler ? scheduler->visualChannelLength : DEFAULT_VISUAL_CHANNEL_LENGTH;
}

float ship_scheduler_get_list_to_visual_ratio(const ShipScheduler *scheduler) {
  if (!scheduler) return 1.0f;
  if (scheduler->listLength == 0) return 1.0f;
  return (float)scheduler->visualChannelLength / (float)scheduler->listLength; // ratio visual per list-slot
}

/*
 * boatTask — tarea FreeRTOS que controla el ciclo de vida de un barco en el canal.
 *
 * PARAMETRO pv (void*):
 *   FreeRTOS pasa los parametros de las tareas como void* para ser generico.
 *   La tarea recibe la DIRECCION del struct Boat en el heap (creado por createBoat()).
 *   El cast (Boat *)pv reinterpreta esos 4/8 bytes como un puntero a Boat.
 *   Es seguro porque xTaskCreate fue llamado con el Boat* exacto como cuarto parametro.
 *
 * ESTADO DE LA TAREA:
 *   La tarea empieza bloqueada en xTaskNotifyWait(portMAX_DELAY) esperando
 *   NOTIF_CMD_RUN. El scheduler la desbloquea llamando a safe_task_notify(handle, RUN).
 *   Una vez corriendo, hace slices de 50ms verificando comandos y moviendose.
 *
 * CAMPOS DE BOAT ACCEDIDOS:
 *   b->remainingMillis: unsigned long. Tiempo restante de servicio. Decrece con elapsed.
 *   b->currentSlot: int16_t. Casilla actual en el canal (-1 = fuera del canal).
 *   b->allowedToMove: bool. Solo el primario en RR tiene este flag en true.
 *   b->stepSize: uint8_t. Casillas que avanza por movimiento.
 *   b->serviceMillis: unsigned long. Tiempo total de servicio del barco.
 *
 * FLUJO DE MEMORIA AL FINALIZAR:
 *   1. ship_scheduler_release_range: libera la casilla en slotOwner[] y da el semaforo.
 *   2. ship_scheduler_notify_boat_finished: el scheduler actualiza estadisticas y
 *      llama a ship_scheduler_finish_active_boat (que llama a remove_active).
 *   3. destroyBoat(b): free(b) - devuelve el struct Boat al heap de C.
 *      Despues de esta linea el puntero b es invalido (dangling); no se puede usar.
 *   4. vTaskDelete(NULL): elimina el TCB de esta tarea del heap de FreeRTOS.
 *      NULL = 'eliminar la tarea que llama a esta funcion'.
 *      Despues de esta linea la tarea deja de existir; el contexto es destruido.
 */
static void boatTask(void *pv) {
  /*
   * Cast de void* a Boat*:
   * pv contiene la DIRECCION del Boat en el heap (pasada como cuarto argumento a xTaskCreate).
   * (Boat *)pv reinterpreta esos bytes como un puntero tipado a struct Boat.
   * b apunta al mismo bloque de memoria que la variable Boat* que paso xTaskCreate.
   */
  Boat *b = (Boat *)pv;
  if (!b) { // pv era NULL: no hay barco; la tarea no puede operar.
    vTaskDelete(NULL); // Elimina la tarea actual del heap de FreeRTOS.
    return;
  }

  bool running = false;          // Si la tarea esta ejecutando activamente (no esperando RUN).
  unsigned long lastTickAt = millis(); // Ms desde boot cuando se realizo el ultimo descuento.
  // Variables de movimiento en el canal:
  int dir = 0;               // +1 = izquierda a derecha; -1 = derecha a izquierda.
  int16_t currentSlot = -1;  // Casilla local del barco (espejo de b->currentSlot).
  unsigned long perMoveMs = 0; // Cuantos ms deben acumularse antes de avanzar una casilla.
  unsigned long moveAccum = 0; // Ms acumulados desde el ultimo movimiento.
  int totalSlotsToTravel = 0;  // Casillas totales del canal (listLength - 1).
  int movesCount = 0;          // Total de movimientos que hara el barco (totalSlots / stepSize).
  /*
   * visualOnly: cuando remainingMillis se agota pero el barco no llego al final
   * del canal, se activa este modo para seguir moviendose sin descontar tiempo.
   * remainingMillis se fija en 1 (no 0) para que el scheduler no lo elimine.
   */
  bool visualOnly = false;
  ship_logf("[BOAT TASK] Barco #%u iniciada. serviceMillis=%lu\n", b->id, b->serviceMillis);

  // BUCLE PRINCIPAL: corre mientras remainingMillis > 0.
  // El scheduler pone remainingMillis = 0 para ordenar la finalizacion,
  // o la tarea lo hace sola cuando llega al final del canal.
  while (b->remainingMillis > 0) {
    uint32_t cmd = 0; // Valor de notificacion recibido (NOTIF_CMD_*).

    if (!running) {
      // FASE DE ESPERA: bloqueada hasta recibir NOTIF_CMD_RUN del scheduler.
      ship_logf("[BOAT TASK #%u] Esperando NOTIF_CMD_RUN (remainingMillis=%lu)...\n", b->id, b->remainingMillis);
      /*
       * xTaskNotifyWait(ulBitsToClearOnEntry, ulBitsToClearOnExit, pulValue, timeout):
       *   ulBitsToClearOnEntry=0x00: no borrar bits al ENTRAR (conservar notificacion pendiente).
       *   ulBitsToClearOnExit=0xFFFFFFFF: borrar TODOS los bits al SALIR (limpiar la notificacion).
       *   pulValue=&cmd: donde escribir el valor de notificacion leido del TCB.
       *   timeout=portMAX_DELAY: bloquear indefinidamente hasta que llegue una notificacion.
       * La tarea queda en estado 'Blocked' en el TCB; el scheduler FreeRTOS no le da CPU
       * hasta que otra tarea llame a xTaskNotify(handle, value, ...) con este handle.
       */
      xTaskNotifyWait(0x00, 0xFFFFFFFF, &cmd, portMAX_DELAY);
      ship_logf("[BOAT TASK #%u] Recibido comando: %u\n", b->id, cmd);
      if (cmd == NOTIF_CMD_TERMINATE) break; // Scheduler cancela: salir del loop y finalizar.
      if (cmd == NOTIF_CMD_RUN) {
        running = true;        // Activar ejecucion.
        b->allowedToMove = true;
        // FIX #3: Resetear lastTickAt en CADA RUN para no contabilizar el tiempo de pausa.
        lastTickAt = millis();
        if (visualOnly && b->remainingMillis == 0) b->remainingMillis = 1; // Mantener vivo en visual.
        ship_logf("[BOAT TASK #%u] RUN recibido. running=true, lastTickAt=%lu, remainingMillis=%lu\n",
                  b->id, lastTickAt, b->remainingMillis);

        // Calcular parametros de movimiento al recibir el primer RUN (o al reanudar).
        if (gScheduler) { // gScheduler: puntero global al ShipScheduler activo.
          ShipScheduler *s = gScheduler; // Alias local para legibilidad.
          if (s->listLength > 1) {
            totalSlotsToTravel = s->listLength - 1; // Casillas a recorrer desde entrada hasta salida.
            // movesCount = techo(totalSlotsToTravel / stepSize):
            //   Si el barco avanza de a 2 casillas y el canal tiene 6 casillas -> 3 movimientos.
            movesCount = (totalSlotsToTravel + b->stepSize - 1) / b->stepSize;
            if (movesCount <= 0) movesCount = 1;
            // perMoveMs = tiempo total de servicio dividido entre el numero de movimientos.
            // Cada vez que moveAccum >= perMoveMs se ejecuta un movimiento.
            perMoveMs = b->serviceMillis / (unsigned long)movesCount;
            if (perMoveMs == 0) perMoveMs = 1;
          } else {
            totalSlotsToTravel = 0; movesCount = 1; perMoveMs = b->serviceMillis;
          }
          dir = (b->origin == SIDE_LEFT) ? 1 : -1; // Direccion de movimiento.
          // Casilla de entrada: 0 para L->R, (listLength-1) para R->L.
          int entryIndex = (b->origin == SIDE_LEFT) ? 0 : (int)(s->listLength - 1);
          ship_logf("[BOAT TASK #%u] movesCount=%d perMoveMs=%lu totalSlots=%d stepSize=%u currentSlot=%d\n",
                    b->id, movesCount, perMoveMs, totalSlotsToTravel, b->stepSize, b->currentSlot);

          if (b->currentSlot < 0) {
            // Barco nuevo: reservar la casilla de entrada del canal.
            // Si la casilla de entrada esta ocupada, esperar SLOT_UPDATE o PAUSE/TERMINATE.
            while (b->remainingMillis > 0 && b->currentSlot < 0) {
              if (ship_scheduler_try_reserve_range(s, entryIndex, 1, b)) {
                // Reserva exitosa: slotOwner[entryIndex] = b->id, semaforo tomado.
                b->currentSlot = entryIndex;
                currentSlot = entryIndex; // Sincronizar variable local.
                break;
              }
              // No pudo reservar: esperar notificacion (SLOT_UPDATE, PAUSE o TERMINATE).
              uint32_t innerCmd = 0;
              xTaskNotifyWait(0x00, 0xFFFFFFFF, &innerCmd, portMAX_DELAY);
              if (innerCmd == NOTIF_CMD_TERMINATE) break;
              if (innerCmd == NOTIF_CMD_PAUSE) {
                running = false; b->allowedToMove = false; break;
              }
              // SLOT_UPDATE o cualquier otro: reintentar en el proximo ciclo.
            }
          } else {
            /*
             * FIX #2: Barco que regresa de preempcion o emergencia.
             * b->currentSlot ya tiene un valor >= 0 (la casilla donde fue restaurado).
             * Sincronizar la variable local 'currentSlot' y recalcular perMoveMs
             * proporcional a las casillas restantes para mantener la velocidad visual correcta.
             *
             * EXCEPCION RR: en RR perMoveMs se calcula siempre como serviceMillis / originalMoves
             * (basado en el canal COMPLETO), NO desde remainingMillis. Esto evita dos bugs:
             *   - rushBug: si remainingMillis es pequeno, perMoveMs seria minusculo y el barco
             *     se moveria varias casillas en cada tick (muy rapido visualmente).
             *   - freezeBug: si remainingMillis==1 (visualOnly), perMoveMs >> quantum y el barco
             *     nunca acumula suficiente para moverse, quedandose congelado para siempre.
             */
            currentSlot = b->currentSlot; // Sincronizar la variable local de la tarea.
            if (s->listLength > 1 && b->stepSize > 0) {
              int endIdx = (b->origin == SIDE_LEFT) ? (int)(s->listLength - 1) : 0;
              int remSlots = (b->origin == SIDE_LEFT)
                             ? (endIdx - b->currentSlot)
                             : (b->currentSlot - endIdx);
              if (remSlots > 0) {
                movesCount = (remSlots + (int)b->stepSize - 1) / (int)b->stepSize;
                if (movesCount <= 0) movesCount = 1;
                if (s->algorithm == ALG_RR && totalSlotsToTravel > 0 && b->stepSize > 0) {
                  // RR: velocidad constante basada en el canal completo.
                  int originalMoves = ((int)totalSlotsToTravel + (int)b->stepSize - 1) / (int)b->stepSize;
                  if (originalMoves > 0) {
                    perMoveMs = b->serviceMillis / (unsigned long)originalMoves;
                    if (perMoveMs == 0) perMoveMs = 1;
                  } else {
                    perMoveMs = b->serviceMillis;
                    if (perMoveMs == 0) perMoveMs = 1;
                  }
                } else {
                  // Algoritmos no-RR: recalculo proporcional al tiempo restante.
                  unsigned long baseMs = (b->remainingMillis > 1) ? b->remainingMillis : b->serviceMillis;
                  if (baseMs == 0) baseMs = ship_scheduler_estimate_service_millis(s, b);
                  perMoveMs = baseMs / (unsigned long)movesCount;
                  if (perMoveMs == 0) perMoveMs = 1;
                }
                ship_logf("[BOAT TASK #%u] Recalculo tras preempcion: remSlots=%d movesCount=%d perMoveMs=%lu remainingMillis=%lu\n",
                          b->id, remSlots, movesCount, perMoveMs, b->remainingMillis);
              }
            }
          }
        }
      }
      continue; // Volver al inicio del while para re-evaluar el estado.
    }

    // --- FASE DE EJECUCION: la tarea esta corriendo ---

    const unsigned long slice = 50; // Ms por slice: esperar hasta 50ms o hasta una notificacion.
    bool interrupted = false;
    /*
     * xTaskNotifyWait con timeout=50ms:
     * Si llega un comando (PAUSE, TERMINATE) en los proximos 50ms, se procesa.
     * Si no llega nada en 50ms, cmd=0 y el barco descuenta 50ms y avanza si aplica.
     * Esto da al barco una resolucion temporal de ~50ms para su movimiento.
     */
    xTaskNotifyWait(0x00, 0xFFFFFFFF, &cmd, pdMS_TO_TICKS(slice));

    if (cmd == NOTIF_CMD_TERMINATE) {
      b->remainingMillis = 0; // Marcar fin para salir del while.
      running = false;
      b->allowedToMove = false;
      if (gScheduler) ship_display_render_forced(gScheduler);
      break;
    }
    if (cmd == NOTIF_CMD_PAUSE) {
      ship_logf("[BOAT TASK #%u] PAUSE recibido. running=false, remainingMillis=%lu\n", b->id, b->remainingMillis);
      running = false;
      b->allowedToMove = false;
      interrupted = true;
      moveAccum = 0; // FIX #11: Resetear acumulador para no moverse de inmediato al reanudar.
      if (gScheduler) ship_display_render_forced(gScheduler);
      continue; // Volver al inicio del while; esperara NOTIF_CMD_RUN en la fase de espera.
    }

    // Calcular tiempo real transcurrido desde el ultimo descuento.
    unsigned long now = millis();
    unsigned long elapsed = now - lastTickAt; // Diferencia en ms.
    lastTickAt = now; // Actualizar base temporal.

    // Descontar remainingMillis si aun no se agoto el tiempo de servicio.
    if (!visualOnly && elapsed > 0) {
      if (elapsed >= b->remainingMillis) {
        // El tiempo se agoto: ver si el barco necesita seguir moviendose visualmente.
        b->remainingMillis = 0;
        if (b->currentSlot >= 0 && gScheduler) {
          ShipScheduler *s = gScheduler;
          int endIndex = (b->origin == SIDE_LEFT) ? (int)(s->listLength - 1) : 0;
          // needsVisual: el barco aun no llego a la casilla final.
          bool needsVisual = (b->origin == SIDE_LEFT)
                             ? (b->currentSlot < endIndex)
                             : (b->currentSlot > endIndex);
          if (needsVisual) {
            visualOnly = true;
            b->remainingMillis = 1; // Mantener vivo; el scheduler no lo elimina si remainingMillis != 0.
          }
          // Si ya esta en la casilla final: remainingMillis = 0, el scheduler lo finalizara.
        }
      } else {
        b->remainingMillis -= elapsed; // Restar el tiempo real del slice.
      }
    }
    // En modo visualOnly: remainingMillis permanece en 1; no se toca aqui.

    if (interrupted || !running) { continue; } // Pausado: no acumular ni moverse.

    // Acumular tiempo para decidir cuando mover el barco en el canal.
    if (b->allowedToMove && perMoveMs > 0 && b->currentSlot >= 0) {
      moveAccum += elapsed; // Sumar el slice al acumulador de movimiento.
    }

    // Mover el barco cuando se acumulo suficiente tiempo.
    if (b->allowedToMove && b->currentSlot >= 0 && moveAccum >= perMoveMs) {
      ShipScheduler *s = gScheduler;
      // En RR: si el barco no es el primario actual, no moverse (ya fue preemptado).
      if (s && s->algorithm == ALG_RR && s->activeBoat != b) {
        moveAccum = 0; continue;
      }
      int16_t slot = b->currentSlot; // Casilla actual del barco en el canal.
      int endIndex = (b->origin == SIDE_LEFT) ? (s->listLength - 1) : 0;
      // remainingSlots: cuantas casillas le quedan hasta el final.
      int remainingSlots = (b->origin == SIDE_LEFT) ? (s->listLength - 1 - slot) : (slot - 0);
      if (remainingSlots <= 0) {
        // El barco llego al final del canal: liberar su casilla.
        // ship_scheduler_release_range: slotOwner[slot] = 0, xSemaphoreGive(sem).
        ship_scheduler_release_range(s, slot, 1, b);
        b->currentSlot = -1; // Ya no esta en el canal.
        b->remainingMillis = 0; // Senal de finalizacion para el scheduler.
        break; // Salir del while para terminar la tarea.
      }

      // Calcular cuantas casillas mover: el minimo entre stepSize y las que quedan.
      uint8_t desiredSteps = (uint8_t)((remainingSlots < b->stepSize) ? remainingSlots : b->stepSize);
      int startReserve = slot + dir; // Primera casilla nueva a reservar.
      ship_logf("[BOAT TASK #%u] desiredSteps=%u remainingSlots=%d startReserve=%d\n",
                b->id, desiredSteps, remainingSlots, startReserve);
      if (desiredSteps == 0) { continue; }

      int16_t newSlot = -1; // Se llenara con la nueva casilla si el movimiento tiene exito.

      if (ship_scheduler_try_move_range(s, startReserve, desiredSteps, b, &newSlot)) {
        // Movimiento atomico exitoso:
        //   - slotOwner del origen = 0 (liberado).
        //   - slotOwner del destino = b->id (reservado).
        //   - *outNewSlot = newSlot (direccion del int16_t en esta llamada).
        b->currentSlot = newSlot;  // Actualizar la casilla en el struct Boat (heap).
        currentSlot = newSlot;     // Sincronizar variable local de la tarea.
        moveAccum = 0;             // Reiniciar acumulador para el proximo movimiento.
        ship_logf("[BOAT TASK #%u] moved to slot %d\n", b->id, newSlot);
        ship_display_render_forced(s); // Actualizar la pantalla con la nueva posicion.
      } else {
        // El movimiento fallo: la(s) casilla(s) destino estan ocupadas.
        ship_logf("[BOAT TASK #%u] blocked waiting for slot at startReserve=%d desiredSteps=%u\n",
                  b->id, startReserve, desiredSteps);
        moveAccum = 0; // No cobrar el tiempo: esperar antes de reintentar.
        if (gScheduler && gScheduler->algorithm == ALG_RR) {
          // En RR: ceder voluntariamente el turno para no desperdiciar el quantum
          // de otros barcos bloqueandose aqui.
          ship_scheduler_yield_active_for_rr(gScheduler, b);
          running = false;
          b->allowedToMove = false;
          continue;
        }
        // En otros algoritmos: quedarse esperando SLOT_UPDATE que enviara release_range.
      }
    }
  } // Fin del bucle principal (while remainingMillis > 0).

  // LIMPIEZA FINAL: liberar la casilla si el barco la tenia al salir.
  if (gScheduler && b->currentSlot >= 0) {
    // Liberar slotOwner[currentSlot] = 0 y dar el semaforo de esa casilla.
    ship_scheduler_release_range(gScheduler, b->currentSlot, 1, b);
    b->currentSlot = -1;
  }

  if (gScheduler) {
    // Notificar al scheduler que este barco termino: actualiza estadisticas y
    // llama a remove_active, que compacta el array activeBoats[].
    ship_scheduler_notify_boat_finished(gScheduler, b);
    ship_display_render_forced(gScheduler); // Ultima actualizacion de pantalla.
  }

  /*
   * destroyBoat(b): llama a free(b) internamente.
   *   - Devuelve al heap de C el bloque de sizeof(Boat) bytes que fue asignado
   *     por createBoat() o createBoatWithPriority().
   *   - Despues de esta linea, b es un puntero dangling y NO debe usarse.
   */
  destroyBoat(b);

  /*
   * vTaskDelete(NULL): elimina la tarea que llama a esta funcion.
   *   - NULL = referirse a si misma (equivale a pasar el propio handle).
   *   - Libera el TCB y el stack de la tarea del heap de FreeRTOS.
   *   - La ejecucion NO retorna despues de esta llamada.
   */
  vTaskDelete(NULL);
} // Fin de boatTask. 

/*
 * ship_scheduler_begin — inicializa el ShipScheduler y arranca todos los recursos.
 *
 * Es el UNICO punto de entrada que se debe llamar antes de encolar cualquier barco.
 * Aplica valores por defecto para los campos no configurados, construye la matriz
 * ticoMarginFactor, carga la configuracion del canal y reserva los recursos.
 *
 * gScheduler = scheduler:
 *   Escribe la DIRECCION del struct ShipScheduler (variable del llamador, en stack
 *   o en area global) en el puntero global gScheduler (void* en el archivo .c).
 *   Todas las tareas boatTask usan gScheduler para acceder al canal sin necesidad
 *   de recibir el puntero explicitamente (xTaskCreate solo pasa un void* de usuario).
 *
 * ticoMarginFactor[at][ct]:
 *   Matriz 3x3 de float embebida en el struct ShipScheduler.
 *   at = BoatType del barco ACTIVO (fila); ct = BoatType del CANDIDATO (columna).
 *   Valores < 1.0: el candidato puede entrar con poco margen (patrulla sobre patrulla: 0.30).
 *   Valores > 1.0: el candidato necesita mas distancia por seguridad (normal -> patrulla: 1.35).
 */
void ship_scheduler_begin(ShipScheduler *scheduler) {
  if (!scheduler) return;
  // Aplicar valores por defecto para campos no configurados por el llamador.
  if (scheduler->rrQuantumMillis < 100) scheduler->rrQuantumMillis = 1200; // 1.2s de quantum para RR.
  if (scheduler->fairnessWindowW == 0) scheduler->fairnessWindowW = 2;     // W=2: alternar cada 2 barcos.
  if (scheduler->signIntervalMillis < 1000) scheduler->signIntervalMillis = 8000; // 8s por lado en SIGN.
  if (scheduler->maxReadyQueueConfigured == 0 || scheduler->maxReadyQueueConfigured > MAX_BOATS)
    scheduler->maxReadyQueueConfigured = MAX_BOATS;
  if (scheduler->channelLengthMeters == 0) scheduler->channelLengthMeters = 120; // 120m por defecto.
  if (scheduler->boatSpeedMetersPerSec == 0) scheduler->boatSpeedMetersPerSec = 18; // 18 m/s por defecto.
  scheduler->activeQuantumStartedAt = 0; // El quantum no esta corriendo al inicio.
  scheduler->flowMode = FLOW_TICO;        // Modo libre por defecto.

  /*
   * Inicializar la matriz ticoMarginFactor[3][3].
   * Es un array bidimensional de float EMBEBIDO en el struct ShipScheduler.
   * Cada elemento se accede como scheduler->ticoMarginFactor[fila][columna].
   * En memoria, el compilador lo almacena como 9 floats contiguos (row-major):
   *   offset 0: [NORMAL][NORMAL], offset 4: [NORMAL][PESQUERA], ...
   * Leer ticoMarginFactor[at][ct] = *(scheduler->ticoMarginFactor[0] + at*3 + ct).
   */
  scheduler->ticoMarginFactor[BOAT_NORMAL][BOAT_NORMAL] = 1.00f;   // Normal vs normal: margen estandar.
  scheduler->ticoMarginFactor[BOAT_NORMAL][BOAT_PESQUERA] = 1.20f; // Normal vs pesquera: mas margen.
  scheduler->ticoMarginFactor[BOAT_NORMAL][BOAT_PATRULLA] = 1.35f; // Normal vs patrulla: mayor urgencia.

  scheduler->ticoMarginFactor[BOAT_PESQUERA][BOAT_NORMAL] = 0.40f;   // Pesquera lenta: margen reducido.
  scheduler->ticoMarginFactor[BOAT_PESQUERA][BOAT_PESQUERA] = 0.50f; // Dos pesqueras: paso estrecho.
  scheduler->ticoMarginFactor[BOAT_PESQUERA][BOAT_PATRULLA] = 1.20f; // Patrulla prioritaria sobre pesquera.

  scheduler->ticoMarginFactor[BOAT_PATRULLA][BOAT_NORMAL] = 0.35f;   // Patrulla activa: muy pequeño.
  scheduler->ticoMarginFactor[BOAT_PATRULLA][BOAT_PESQUERA] = 0.30f; // Patrulla+pesquera: minimo.
  scheduler->ticoMarginFactor[BOAT_PATRULLA][BOAT_PATRULLA] = 0.30f; // Dos patrullas: margen minimo.

  scheduler->signDirection = SIDE_LEFT;     // Letrero empieza apuntando a la izquierda.
  scheduler->signLastSwitchAt = millis();   // Guarda el timestamp actual como referencia del letrero.
  scheduler->fairnessCurrentSide = SIDE_LEFT; // Ventana de equidad empieza por la izquierda.
  scheduler->fairnessPassedInWindow = 0;    // Ningun barco ha pasado en la ventana actual.
  scheduler->collisionDetections = 0;       // Contador de colisiones potenciales.

  // Sensor de proximidad (ultrasonico).
  scheduler->sensorActive = false;
  scheduler->proximityThresholdCm = 10;
  scheduler->proximityCurrentDistanceCm = 999; // 999 = "lejos" (sin obstaculo).
  scheduler->proximityDistanceIsSimulated = false;

  // Estado de emergencia (puertas del canal).
  scheduler->emergencyMode = EMERGENCY_NONE;
  scheduler->emergencyStartedAt = 0;
  scheduler->emergencyDispatchBlockedLogged = false;
  scheduler->gateLeftClosed = 0;
  scheduler->gateRightClosed = 0;
  scheduler->gateLockDurationMs = 5000; // 5s de cierre por defecto.
  scheduler->demoCount = 0; // Manifesto demo vacio.

  ship_scheduler_clear(scheduler); // Eliminar tareas y recursos del ciclo anterior.

  // Cargar la configuracion del canal (listLength y visualChannelLength)
  // solo en plataformas no-Arduino (PC/simulador) donde existe FILE/fopen.
#ifndef ARDUINO
  ship_scheduler_load_channel_config(scheduler, "channel_config.txt");
#endif

  // Construir slotOwner[] y semaforos segun la listLength cargada.
  ship_scheduler_rebuild_slots(scheduler);

  /*
   * Registrar en la variable global gScheduler.
   * gScheduler es static ShipScheduler * en este archivo .c.
   * Desde este momento, boatTask puede acceder al canal sin recibir el puntero:
   *   ShipScheduler *s = gScheduler;
   */
  gScheduler = scheduler;
} // Fin de ship_scheduler_begin. 

/*
 * ship_scheduler_clear — detiene y libera todos los barcos activos y en cola.
 *
 * Se llama al reiniciar el scheduler o al comenzar una nueva demo.
 * CRITICO: se deben enviar NOTIF_CMD_TERMINATE ANTES de llamar a free() sobre
 * el Boat o el slotOwner[], porque las tareas boatTask estan corriendo en sus
 * propias pilas de FreeRTOS y pueden estar a punto de desreferenciar esos punteros.
 * Al enviar TERMINATE, la tarea hace: b->remainingMillis = 0 -> break -> destroyBoat
 * -> vTaskDelete(NULL), liberando su propia memoria de forma ordenada.
 *
 * Si la tarea YA termino (taskHandle == NULL): el barco aun pudo quedar en la cola
 * readyQueue[] porque la tarea no habia empezado. En ese caso se llama destroyBoat
 * directamente (no hay TCB que notificar).
 *
 * Limpieza de recursos al final:
 *  - ship_scheduler_destroy_slot_resources(): libera gSlotSemaphores[] y cada Queue_t.
 *  - free(scheduler->slotOwner): devuelve el array de bytes al heap de C.
 *  - vSemaphoreDelete(channelSlotsGuard): libera el mutex del canal del heap FreeRTOS.
 *  - ship_scheduler_rebuild_slots: reconstruye los recursos para que el scheduler
 *    pueda aceptar barcos nuevos inmediatamente despues del clear.
 */
void ship_scheduler_clear(ShipScheduler *scheduler) {
  if (!scheduler) return;
  scheduler->ignoreCompletions = true; // Ignorar callbacks de finalizacion durante la limpieza.

  // Terminar todos los barcos en la COLA de espera.
  for (uint8_t i = 0; i < scheduler->readyCount; i++) {
    Boat *b = scheduler->readyQueue[i];
    if (!b) continue;
    b->cancelled = true; // Marcar como cancelado para que la tarea lo detecte.
    if (b->taskHandle) {
      // La tarea existe: enviarle TERMINATE para que se limpie a si misma.
      safe_task_notify(b->taskHandle, NOTIF_CMD_TERMINATE);
    } else {
      // No hay tarea (barco aun no despachado): liberar el struct directamente.
      destroyBoat(b);
    }
    scheduler->readyQueue[i] = NULL; // Invalidar el puntero en la cola.
  }
  scheduler->readyCount = 0; // La cola de espera queda vacia.

  // Terminar todos los barcos ACTIVOS.
  for (uint8_t i = 0; i < scheduler->activeCount; i++) {
    Boat *active = scheduler->activeBoats[i];
    if (!active) continue;
    active->cancelled = true;
    if (active->taskHandle) {
      safe_task_notify(active->taskHandle, NOTIF_CMD_TERMINATE);
    } else {
      destroyBoat(active);
    }
    scheduler->activeBoats[i] = NULL;
  }
  scheduler->activeCount = 0;
  scheduler->activeBoat = NULL;      // Alias primario: invalido.
  scheduler->hasActiveBoat = false;

  // Resetear todos los contadores estadisticos.
  scheduler->completedLeftToRight = 0;
  scheduler->completedRightToLeft = 0;
  scheduler->completedTotal = 0;
  scheduler->totalWaitMillis = 0;       // Suma de (startedAt - enqueuedAt) de cada barco.
  scheduler->totalTurnaroundMillis = 0; // Suma de (finishedAt - enqueuedAt) de cada barco.
  scheduler->totalServiceMillis = 0;    // Suma de serviceMillis de cada barco.
  scheduler->completionCount = 0;       // Ordinal de finalizacion (para ordenar resultados).
  scheduler->crossingStartedAt = 0;
  scheduler->activeQuantumAccumulatedMillis = 0;
  scheduler->rrTurnIndex = 0;
  scheduler->fairnessPassedInWindow = 0;
  scheduler->signLastSwitchAt = millis();

  // Liberar los recursos de slots (semaforos del canal y el array slotOwner).
  ship_scheduler_destroy_slot_resources(); // Libera gSlotSemaphores[] del heap FreeRTOS.
  if (scheduler->slotOwner) {
    free(scheduler->slotOwner); // Devuelve el array uint8_t[] al heap de C.
    scheduler->slotOwner = NULL;
  }
  if (scheduler->channelSlotsGuard) {
    // vSemaphoreDelete: libera el mutex (Queue_t) del heap de FreeRTOS.
    vSemaphoreDelete((SemaphoreHandle_t)scheduler->channelSlotsGuard);
    scheduler->channelSlotsGuard = NULL; // Invalidar el void* para evitar doble-free.
  }
  // Reconstruir los recursos de slots para que el scheduler este listo
  // para aceptar barcos nuevos en la proxima enqueue.
  ship_scheduler_rebuild_slots(scheduler);
} // Fin de ship_scheduler_clear. 

static int findIndexForAlgo(ShipAlgo algo, Boat *readyQueue[], uint8_t readyCount) { // Selecciona indice segun algoritmo. 
  return findIndexForAlgoAndSide(algo, readyQueue, readyCount, false, SIDE_LEFT); // Delega selector sin filtro de lado.
} // Fin de findIndexForAlgo. 

static void ship_scheduler_tick_sign(ShipScheduler *scheduler) { // Actualiza letrero por tiempo.
  if (!scheduler || scheduler->flowMode != FLOW_SIGN) return; // Aplica solo en modo letrero.
  if (scheduler->signIntervalMillis == 0) return; // Evita intervalos invalidos.
  unsigned long now = millis(); // Lee reloj actual.
  if (now - scheduler->signLastSwitchAt >= scheduler->signIntervalMillis) { // Verifica vencimiento.
    scheduler->signDirection = opposite_side(scheduler->signDirection); // Alterna lado del letrero.
    scheduler->signLastSwitchAt = now; // Guarda el instante del cambio.
    ship_logf("Letrero -> %s\n", boatSideName(scheduler->signDirection)); // Reporta cambio.
    FLOW_LOG(scheduler, "[FLOW][SIGN] Cambio de direccion por tiempo. Nueva=%s\n", boatSideName(scheduler->signDirection)); // Traza de cambio.
  }
} // Fin de ship_scheduler_tick_sign.

/*
 * ship_scheduler_select_next_index — selecciona el indice del proximo barco a despachar.
 *
 * Delega la decision segun el flowMode activo:
 *
 * FLOW_TICO: no restringe por lado; elige al mejor candidato segun el algoritmo
 *   de scheduling (FCFS, SJF, STRN, EDF, PRIORITY, RR). Los barcos de ambos lados
 *   compiten libremente.
 *
 * FLOW_SIGN: solo el lado del letrero puede entrar. Si no hay barcos de ese lado,
 *   se usa el lado opuesto como fallback para no paralizar el canal.
 *
 * FLOW_FAIRNESS: ventana W. Los primeros W barcos del lado actual reciben turno;
 *   al completarse la ventana, se alterna al lado opuesto. Si un lado se queda
 *   vacio antes de completar W, se alterna inmediatamente.
 *
 * Todos los modos llaman a findIndexForAlgoAndSide, que es el selector de O(n)
 * que recorre readyQueue[] buscando al mejor candidato segun el criterio del
 * algoritmo activo (minimiza arrivalOrder, serviceMillis, remainingMillis, etc.).
 */
static int ship_scheduler_select_next_index(ShipScheduler *scheduler) {
  if (!scheduler || scheduler->readyCount == 0) return -1;

  if (scheduler->flowMode == FLOW_TICO) {
    FLOW_LOG(scheduler, "[FLOW][TICO] Seleccion libre por algoritmo %s\n", ship_scheduler_get_algorithm_label(scheduler));
    return findIndexForAlgo(scheduler->algorithm, scheduler->readyQueue, scheduler->readyCount);
  }

  if (scheduler->flowMode == FLOW_SIGN) {
    BoatSide allowed = scheduler->signDirection;  // Lado que el letrero permite.
    BoatSide fallback = opposite_side(allowed);   // Lado alternativo si el permitido esta vacio.
    if (queue_has_side(scheduler, allowed)) {
      FLOW_LOG(scheduler, "[FLOW][SIGN] Letrero=%s, seleccionando ese lado\n", boatSideName(allowed));
      return findIndexForAlgoAndSide(scheduler->algorithm, scheduler->readyQueue, scheduler->readyCount, true, allowed);
    }
    FLOW_LOG(scheduler, "[FLOW][SIGN] Letrero=%s sin barcos; fallback a %s\n", boatSideName(allowed), boatSideName(fallback));
    return findIndexForAlgoAndSide(scheduler->algorithm, scheduler->readyQueue, scheduler->readyCount, true, fallback);
  }

  if (scheduler->flowMode == FLOW_FAIRNESS) {
    uint8_t w = scheduler->fairnessWindowW == 0 ? 1 : scheduler->fairnessWindowW; // W no puede ser 0.
    BoatSide current = scheduler->fairnessCurrentSide;  // Lado activo de la ventana.
    BoatSide opposite = opposite_side(current);          // Lado que espera su turno.
    bool hasCurrent = queue_has_side(scheduler, current);
    bool hasOpposite = queue_has_side(scheduler, opposite);

    if (hasCurrent && !hasOpposite) { // Solo un lado disponible: seguir sin alternar.
      FLOW_LOG(scheduler, "[FLOW][FAIR] Solo hay barcos en %s; continuo sin alternar\n", boatSideName(current));
      return findIndexForAlgoAndSide(scheduler->algorithm, scheduler->readyQueue, scheduler->readyCount, true, current);
    }
    if (!hasCurrent && hasOpposite) { // El lado actual se agoto: alternar ya.
      scheduler->fairnessCurrentSide = opposite;
      scheduler->fairnessPassedInWindow = 0;
      FLOW_LOG(scheduler, "[FLOW][FAIR] Lado %s vacio; cambio inmediato a %s\n", boatSideName(current), boatSideName(opposite));
      return findIndexForAlgoAndSide(scheduler->algorithm, scheduler->readyQueue, scheduler->readyCount, true, opposite);
    }
    if (!hasCurrent && !hasOpposite) return -1; // Cola vacia.

    if (scheduler->fairnessPassedInWindow >= w) { // Ventana completada: alternar.
      scheduler->fairnessCurrentSide = opposite;
      scheduler->fairnessPassedInWindow = 0;
      FLOW_LOG(scheduler, "[FLOW][FAIR] Se cumplio W=%u; alterno a %s\n", w, boatSideName(scheduler->fairnessCurrentSide));
    }
    FLOW_LOG(scheduler, "[FLOW][FAIR] Ventana actual lado=%s usados=%u/%u\n",
             boatSideName(scheduler->fairnessCurrentSide), scheduler->fairnessPassedInWindow, w);
    return findIndexForAlgoAndSide(scheduler->algorithm, scheduler->readyQueue, scheduler->readyCount, true, scheduler->fairnessCurrentSide);
  }

  return findIndexForAlgo(scheduler->algorithm, scheduler->readyQueue, scheduler->readyCount); // Alternativa.
} // Fin de ship_scheduler_select_next_index.

void ship_scheduler_enqueue(ShipScheduler *scheduler, Boat *boat) { // Encola un barco nuevo. 
  ship_scheduler_enqueue_with_deadline(scheduler, boat, 0UL); // Usa deadline por defecto.
} // Fin de ship_scheduler_enqueue.

/*
 * ship_scheduler_enqueue_with_deadline — encola un barco con deadline absoluto.
 *
 * ORDEN DE INSERCION:
 *   readyQueue[] se mantiene ordenado por arrivalOrder (uint32_t, monotonicamente
 *   creciente). La insercion es O(n) con corrimiento a la derecha:
 *     while (readyQueue[insertAt-1]->arrivalOrder > boat->arrivalOrder) {
 *       readyQueue[insertAt] = readyQueue[insertAt-1]; insertAt--;
 *     }
 *   Esto garantiza FCFS cuando arrivalOrder es el unico criterio.
 *   Para SJF/STRN/EDF/PRIORITY, findIndexForAlgo recorre la cola completa en O(n)
 *   eligiendo al mejor candidato independientemente de la posicion.
 *
 * xTaskCreate(boatTask, "boat", 4096, boat, 1, &boat->taskHandle):
 *   - boatTask: puntero a la funcion que ejecutara la tarea.
 *   - "boat": nombre de la tarea en el kernel FreeRTOS (solo para depuracion).
 *   - 4096: tamano del stack en BYTES asignado del heap FreeRTOS para esta tarea.
 *   - boat: void* pasado como parametro a boatTask(void *pv).
 *   - 1: prioridad de la tarea (0=idle, mayor numero=mayor prioridad).
 *   - &boat->taskHandle: DIRECCION del campo TaskHandle_t dentro del struct Boat
 *     (que esta en el heap). FreeRTOS escribe ahi el puntero al TCB recien creado.
 *     Gracias a esto, el scheduler puede notificar la tarea: safe_task_notify(boat->taskHandle, ...).
 *   La tarea queda en estado BLOQUEADA en xTaskNotifyWait(portMAX_DELAY) esperando
 *   NOTIF_CMD_RUN; no consume CPU hasta que el scheduler la despache.
 *
 * PREEMPCION (STRN / EDF / PRIORITY):
 *   Si el candidato tiene menor remainingMillis / menor urgencia de deadline /
 *   mayor prioridad que el activo, se desaloja al activo:
 *     - PAUSE al activo (safe_task_notify(preempted->taskHandle, NOTIF_CMD_PAUSE)).
 *     - Guardar emergencySavedSlot = preempted->currentSlot (int16_t en el struct).
 *     - Liberar la casilla del preemptado (release_range) para que el sucesor pase.
 *     - remove_active: compacta activeBoats[].
 *     - requeue_boat al frente (atFront=true): lo pone en readyQueue[0] con shift.
 *     - start_next_boat: despacha al nuevo barco.
 */
void ship_scheduler_enqueue_with_deadline(ShipScheduler *scheduler, Boat *boat, unsigned long deadlineMillis) {
  if (!scheduler || !boat) return;
  scheduler->ignoreCompletions = false; // Habilitar callbacks de finalizacion.
  if (scheduler->readyCount >= MAX_BOATS ||
      scheduler->readyCount >= ship_scheduler_get_max_ready_queue(scheduler)) {
    ship_logln("Cola llena; no se agrego el barco.");
    destroyBoat(boat); // Liberar el struct si no puede encolarse.
    return;
  }

  boat->cancelled = false;
  if (boat->enqueuedAt == 0) boat->enqueuedAt = millis(); // Timestamp de encolado.
  boat->state = STATE_WAITING;
  // Si es un barco preemptado que se reencola, preservar remainingMillis;
  // si es nuevo, usar placeholder para que el scheduler lo estime en start_next_boat.
  if (boat->serviceMillis > 0 && boat->remainingMillis == 0) {
    boat->remainingMillis = 1; // Placeholder: no es 0 para que el scheduler no lo descarte.
  }

  if (deadlineMillis > 0) {
    boat->deadlineMillis = deadlineMillis; // Deadline absoluto explicito.
  } else if (boat->deadlineMillis == 0) {
    // Calcular deadline heuristico: now + (serviceMs * 2).
    boat->deadlineMillis = ship_scheduler_compute_default_deadline(scheduler, boat);
  }

  // Insercion con orden por arrivalOrder (FCFS base).
  uint8_t insertAt = scheduler->readyCount;
  while (insertAt > 0 && scheduler->readyQueue[insertAt - 1]->arrivalOrder > boat->arrivalOrder) {
    scheduler->readyQueue[insertAt] = scheduler->readyQueue[insertAt - 1]; // Copiar puntero a la derecha.
    insertAt--;
  }
  scheduler->readyQueue[insertAt] = boat; // Insertar en la posicion correcta.
  scheduler->readyCount++;

  ship_logf("Barco agregado: #%u tipo=%s origen=%s\n", boat->id, boatTypeName(boat->type), boatSideName(boat->origin));

  /*
   * FIX #9: Crear la tarea FreeRTOS ANTES de decidir si preemptar.
   * xTaskCreate escribe el TCB en boat->taskHandle.
   * La tarea queda bloqueada en xTaskNotifyWait(portMAX_DELAY) hasta recibir
   * NOTIF_CMD_RUN; en ese estado no consume CPU y no tiene efectos secundarios.
   * Si la tarea se creara DESPUES de la decision de preempcion, existiria una
   * condicion de carrera: start_next_boat enviaria NOTIF_CMD_RUN a un handle
   * NULL (o invalido) si la tarea aun no fue creada.
   */
  xTaskCreate(boatTask, "boat", 4096, boat, 1, &boat->taskHandle);
  // boat->taskHandle apunta ahora al TCB en el heap de FreeRTOS.

  if (scheduler->activeCount == 0) {
    // No hay activos: despachar directamente sin evaluar preempcion.
    ship_scheduler_start_next_boat(scheduler);
    return;
  }

  if (scheduler->activeCount > 0 && scheduler->activeBoat) {
    if (scheduler->flowMode == FLOW_TICO && scheduler->algorithm != ALG_RR) {
      // En TICO no-RR: multiples barcos pueden estar en el canal sin preempcion.
      return;
    }
    bool shouldPreempt = false;
    if (scheduler->algorithm == ALG_STRN) {
      // STRN: preemptar si el candidato tiene MENOS remainingMillis que el activo.
      unsigned long candRem = 0UL;
      if (boat->remainingMillis > 1) candRem = boat->remainingMillis;
      else if (boat->serviceMillis > 0) candRem = boat->serviceMillis;
      else candRem = ship_scheduler_estimate_service_millis(scheduler, boat);
      unsigned long activeRem = 0UL;
      if (scheduler->activeBoat->remainingMillis > 1) activeRem = scheduler->activeBoat->remainingMillis;
      else if (scheduler->activeBoat->serviceMillis > 0) activeRem = scheduler->activeBoat->serviceMillis;
      else activeRem = ship_scheduler_estimate_service_millis(scheduler, scheduler->activeBoat);
      if (candRem < activeRem) shouldPreempt = true;
    } else if (scheduler->algorithm == ALG_EDF) {
      // EDF: preemptar si el candidato tiene deadline ANTES que el activo.
      unsigned long now = millis();
      unsigned long candRem  = boat->deadlineMillis > now ? boat->deadlineMillis - now : 0UL;
      unsigned long activeRem = scheduler->activeBoat->deadlineMillis > now
                                 ? scheduler->activeBoat->deadlineMillis - now : 0UL;
      if (candRem < activeRem) shouldPreempt = true;
    } else if (scheduler->algorithm == ALG_PRIORITY) {
      // PRIORITY: preemptar si el candidato tiene MAYOR prioridad numerica.
      if (boat->priority > scheduler->activeBoat->priority) shouldPreempt = true;
    }

    if (shouldPreempt) {
      Boat *preempted = scheduler->activeBoat; // Barco que sera desalojado.
      ship_logf("Preemption: barco #%u desaloja a #%u\n", boat->id, preempted->id);

      // 1. Pausar la tarea del desalojado.
      preempted->allowedToMove = false;
      if (preempted->taskHandle) safe_task_notify(preempted->taskHandle, NOTIF_CMD_PAUSE);
      ship_scheduler_freeze_active_quantum(scheduler);

      // 2. Recalcular remainingMillis proporcional a las casillas restantes.
      //    Critico para STRN: el tiempo restante debe reflejar cuanto canal le queda
      //    al barco, no el reloj de pared consumido.
      if (preempted->currentSlot >= 0 && scheduler->listLength > 0 && preempted->serviceMillis > 0) {
        int endIndex = (preempted->origin == SIDE_LEFT) ? (int)(scheduler->listLength - 1) : 0;
        int slotsRemaining = (preempted->origin == SIDE_LEFT)
                             ? (endIndex - preempted->currentSlot)
                             : (preempted->currentSlot - endIndex);
        int totalSlots = (int)(scheduler->listLength - 1);
        if (totalSlots > 0 && slotsRemaining >= 0) {
          preempted->remainingMillis = (unsigned long)(
            ((float)slotsRemaining / (float)totalSlots) * (float)preempted->serviceMillis + 0.5f);
          if (preempted->remainingMillis == 0) preempted->remainingMillis = 1;
        }
      }

      // 3. Guardar la posicion actual y LIBERAR la casilla del desalojado.
      //    Si no se libera, el sucesor se bloqueara en esa casilla indefinidamente.
      preempted->emergencySavedSlot = preempted->currentSlot; // Guardar para restauracion.
      preempted->emergencyParked = true; // Marcar como aparcado por preempcion.
      if (preempted->currentSlot >= 0) {
        ship_scheduler_release_range(scheduler, preempted->currentSlot, 1, preempted);
        preempted->currentSlot = -1; // El barco ya no ocupa casilla en el canal.
      }

      // 4. Quitar de la lista de activos para que start_next_boat pueda despachar.
      ship_scheduler_remove_active(scheduler, preempted);

      // 5. Reencolarlo al frente para que regrese en cuanto el sucesor termine.
      //    atFront=true: for(i=readyCount; i>0; i--) readyQueue[i] = readyQueue[i-1] -> readyQueue[0]=preempted.
      ship_scheduler_requeue_boat(scheduler, preempted, true);

      // 6. Despachar el barco que gano la preempcion.
      ship_scheduler_start_next_boat(scheduler);
    }
  }
} // Fin de ship_scheduler_enqueue_with_deadline.

void ship_scheduler_load_demo_manifest(ShipScheduler *scheduler) { // Carga manifiesto de demo. 
  if (!scheduler) return; // Valida puntero. 
  ship_scheduler_clear(scheduler); // Limpia estado. 
  resetBoatSequence(); // Reinicia secuencias. 

  if (scheduler->demoCount > 0) {
    for (uint8_t i = 0; i < scheduler->demoCount; i++) {
      Boat *b = createBoatWithPriority(scheduler->demoSide[i], scheduler->demoType[i], scheduler->demoPrio[i]);
      if (!b) continue;
      if (scheduler->demoStep[i] > 0) b->stepSize = scheduler->demoStep[i];
      ship_scheduler_enqueue(scheduler, b);
    }
    ship_logf("Manifesto cargado desde config (%u barcos).\n", scheduler->demoCount);
    return;
  }

  ship_scheduler_enqueue(scheduler, createBoat(SIDE_LEFT, BOAT_NORMAL)); // Encola normal izq. 
  ship_scheduler_enqueue(scheduler, createBoat(SIDE_RIGHT, BOAT_PESQUERA)); // Encola pesquera der. 
  ship_scheduler_enqueue(scheduler, createBoat(SIDE_LEFT, BOAT_PATRULLA)); // Encola patrulla izq. 
  ship_scheduler_enqueue(scheduler, createBoat(SIDE_RIGHT, BOAT_NORMAL)); // Encola normal der. 
  ship_scheduler_enqueue(scheduler, createBoat(SIDE_LEFT, BOAT_PESQUERA)); // Encola pesquera izq. 

  ship_logln("Manifesto cargado (default)." ); // Log de carga. 
} // Fin de ship_scheduler_load_demo_manifest. 

/*
 * ship_scheduler_requeue_boat — reencola un barco previamente activo.
 *
 * atFront = true: insercion al FRENTE de readyQueue[].
 *   for (i = readyCount; i > 0; i--) readyQueue[i] = readyQueue[i-1]:
 *     Desplaza todos los punteros una posicion a la derecha (corrimiento O(n)).
 *   readyQueue[0] = boat: el barco desalojado queda en el frente.
 *   Garantiza que el proximo dispatch (en FCFS/SJF/STRN/EDF) lo elija antes
 *   que cualquier barco que llegue posteriormente.
 *
 * atFront = false: insercion al FINAL (cola normal).
 *   readyQueue[readyCount] = boat: simplemente lo pone al final.
 *   Usado para barcos que voluntariamente ceden (yield_rr).
 *
 * Gestion de overflow: si readyCount >= MAX_BOATS, no hay espacio.
 *   El barco se cancela y su tarea recibe TERMINATE para evitar leaks.
 */
static void ship_scheduler_requeue_boat(ShipScheduler *scheduler, Boat *boat, bool atFront) {
  if (!scheduler || !boat) return;
  boat->state = STATE_WAITING; // Volver al estado de espera.
  if (scheduler->readyCount >= MAX_BOATS) {
    // No hay espacio: cancelar el barco en lugar de perder memoria.
    boat->cancelled = true;
    if (boat->taskHandle) {
      safe_task_notify(boat->taskHandle, NOTIF_CMD_TERMINATE);
    } else {
      destroyBoat(boat);
    }
    return;
  }
  if (atFront) {
    // Corrimiento a la derecha para dejar readyQueue[0] libre.
    for (int i = scheduler->readyCount; i > 0; i--)
      scheduler->readyQueue[i] = scheduler->readyQueue[i - 1]; // Copiar puntero un lugar hacia adelante.
    scheduler->readyQueue[0] = boat; // El barco desalojado ocupa el frente.
  } else {
    scheduler->readyQueue[scheduler->readyCount] = boat; // Insertar al final.
  }
  scheduler->readyCount++; // Incrementar el contador de la cola.
} // Fin de ship_scheduler_requeue_boat. 

/*
 * ship_scheduler_start_next_boat — selecciona y despacha el proximo barco al canal.
 *
 * FLUJO PRINCIPAL:
 *  1. select_next_index: elige el indice segun algoritmo y modo de flujo.
 *  2. Extraer el barco de readyQueue[]: corrimiento de la cola para llenar el hueco.
 *  3. Verificar seguridad de colision: compara con cada activo (sentido y distancia).
 *  4. Reservar casilla de entrada (try_reserve_range) o restaurar desde savedSlot.
 *  5. Calcular serviceMillis (si es el primer despacho del barco).
 *  6. Recalcular remainingMillis proporcional si el barco regresa de preempcion.
 *  7. add_active: agrega el barco a activeBoats[].
 *  8. safe_task_notify(handle, NOTIF_CMD_RUN): desbloquea la tarea FreeRTOS del barco.
 *
 * RESTAURACION DE BARCO PREEMPTADO:
 *   b->emergencyParked == true: el barco fue desalojado y su casilla fue liberada.
 *   Se intenta reservar emergencySavedSlot exactamente; si ya esta ocupado por el
 *   sucesor, se busca hacia la entrada del barco (reverseDir = -dir) en las casillas
 *   adyacentes hasta encontrar una libre.
 *   Razon: al preemptar se libero la casilla para que el sucesor avanzara; si el
 *   sucesor aun no paso, la casilla exacta estara ocupada. El barco restaurado
 *   ocupa la primera libre hacia atras, garantizando que no choque con el sucesor.
 *
 * NOTA sobre b->taskHandle y NOTIF_CMD_RUN:
 *   La tarea ya fue creada por enqueue_with_deadline (FIX #9).
 *   NOTIF_CMD_RUN escribe el valor 1 en el campo ulNotifiedValue del TCB usando
 *   xTaskNotify(handle, NOTIF_CMD_RUN, eSetValueWithOverwrite), que despierta la
 *   tarea de xTaskNotifyWait. La tarea lee el valor en &cmd y verifica == NOTIF_CMD_RUN.
 */
static bool ship_scheduler_start_next_boat(ShipScheduler *scheduler) {
  if (!scheduler || scheduler->readyCount == 0) return false;
  // Solo RR permite multiples activos simultaneos.
  if (scheduler->algorithm != ALG_RR && scheduler->activeCount > 0) return false;
  if (scheduler->activeCount >= MAX_BOATS) return false;

  // Bloquear despacho durante emergencia (puertas cerradas).
  if (scheduler->emergencyMode != EMERGENCY_NONE) {
    if (!scheduler->emergencyDispatchBlockedLogged) {
      ship_logln("[EMERGENCY] Despacho bloqueado: puertas cerradas por emergencia");
      scheduler->emergencyDispatchBlockedLogged = true;
    }
    return false;
  }

  int idx = ship_scheduler_select_next_index(scheduler); // Seleccionar el mejor barco.
  if (idx < 0) return false;

  Boat *b = scheduler->readyQueue[idx]; // Extraer el barco seleccionado.
  // Compactar la cola: mover todos los posteriores una posicion hacia atras.
  for (uint8_t i = idx + 1; i < scheduler->readyCount; i++)
    scheduler->readyQueue[i - 1] = scheduler->readyQueue[i];
  scheduler->readyCount--;

  // Verificar seguridad de colision: comparar con cada activo.
  for (uint8_t i = 0; i < scheduler->activeCount; i++) {
    Boat *active = scheduler->activeBoats[i];
    if (!active) continue;
    if (active->origin != b->origin) {
      // Sentidos opuestos: requieren distancia minima para no chocarse de frente.
      if (active->currentSlot < 0 || scheduler->listLength == 0) {
        scheduler->collisionDetections++;
        FLOW_LOG(scheduler, "[FLOW][SAFE] Requeue por seguridad: activo #%u (%s), candidato #%u (%s)\n",
                 active->id, boatSideName(active->origin), b->id, boatSideName(b->origin));
        ship_scheduler_requeue_boat(scheduler, b, true);
        return false;
      }
      int entryIndex = (b->origin == SIDE_LEFT) ? 0 : (int)(scheduler->listLength - 1);
      int dist = active->currentSlot - entryIndex;
      if (dist < 0) dist = -dist;
      int safeDist = (int)active->stepSize + (int)b->stepSize; // Distancia minima conservadora.
      if (dist <= safeDist) {
        scheduler->collisionDetections++;
        FLOW_LOG(scheduler, "[FLOW][SAFE] Requeue por seguridad: activo #%u (%s), candidato #%u (%s) dist=%d safe=%d\n",
                 active->id, boatSideName(active->origin), b->id, boatSideName(b->origin), dist, safeDist);
        ship_scheduler_requeue_boat(scheduler, b, true);
        return false;
      }
    }
  }

  // En RR: no despachar si el quantum del primario aun no expiro.
  if (scheduler->algorithm == ALG_RR && scheduler->activeCount > 0 &&
      scheduler->activeBoat && scheduler->activeBoat->allowedToMove) {
    unsigned long elapsed = ship_scheduler_get_active_elapsed_millis(scheduler);
    if (elapsed < scheduler->rrQuantumMillis) {
      FLOW_LOG(scheduler, "[DISPATCH] RR: quantum activo no terminado (%lu/%lu), reencolando #%u\n",
               elapsed, scheduler->rrQuantumMillis, b->id);
      ship_scheduler_requeue_boat(scheduler, b, true);
      return false;
    }
  }

  // En RR: no mezclar sentidos en el canal.
  if (scheduler->algorithm == ALG_RR && scheduler->activeCount > 0 && scheduler->activeBoat) {
    if (b->origin != scheduler->activeBoat->origin) {
      FLOW_LOG(scheduler, "[DISPATCH] RR: candidato #%u sentido %s opuesto al activo %s, reencolando\n",
               b->id, boatSideName(b->origin), boatSideName(scheduler->activeBoat->origin));
      ship_scheduler_requeue_boat(scheduler, b, true);
      return false;
    }
  }

  // Distancia minima del mismo sentido: evitar barcos demasiado juntos al entrar.
  for (uint8_t i = 0; i < scheduler->activeCount; i++) {
    Boat *active = scheduler->activeBoats[i];
    if (!active || active->origin != b->origin || active->currentSlot < 0) continue;
    int dist = (b->origin == SIDE_LEFT) ? (active->currentSlot - 0)
                                        : ((int)(scheduler->listLength - 1) - active->currentSlot);
    if (dist < (int)b->stepSize) {
      FLOW_LOG(scheduler, "[DISPATCH] Demasiado cerca del activo #%u (dist=%d < step=%u), reencolando #%u\n",
               active->id, dist, b->stepSize, b->id);
      ship_scheduler_requeue_boat(scheduler, b, true);
      return false;
    }
  }

  b->state = STATE_CROSSING; // El barco entra al canal.
  if (b->startedAt == 0) b->startedAt = millis(); // Timestamp de inicio de servicio.
  scheduler->crossingStartedAt = millis();

  // Intentar colocar el barco en el canal (reservar casilla).
  if (scheduler->listLength > 0 && scheduler->slotOwner) {
    if (b->emergencyParked && b->emergencySavedSlot >= 0) {
      // Barco restaurado desde preempcion: intentar reservar la casilla guardada.
      int dir = (b->origin == SIDE_LEFT) ? 1 : -1;
      int endIndex = (b->origin == SIDE_LEFT) ? (int)(scheduler->listLength - 1) : 0;
      int savedSlot = (int)b->emergencySavedSlot;
      bool restored = false;

      if (ship_scheduler_try_reserve_range(scheduler, savedSlot, 1, b)) {
        b->currentSlot = (int16_t)savedSlot; // Casilla exacta disponible.
        restored = true;
        FLOW_LOG(scheduler, "[DISPATCH] Barco #%u restaurado en casilla exacta %d\n", b->id, savedSlot);
      } else {
        // Casilla exacta ocupada por el sucesor: buscar hacia la entrada.
        int reverseDir = -dir; // Direccion contraria al viaje (hacia el origen).
        int searchSlot = savedSlot + reverseDir;
        for (uint16_t tries = 0; tries < scheduler->listLength; tries++, searchSlot += reverseDir) {
          if (searchSlot < 0 || searchSlot >= (int)scheduler->listLength) break;
          if (ship_scheduler_try_reserve_range(scheduler, searchSlot, 1, b)) {
            b->currentSlot = (int16_t)searchSlot;
            restored = true;
            FLOW_LOG(scheduler, "[DISPATCH] Barco #%u restaurado en casilla fallback %d (saved=%d ocupado)\n",
                     b->id, searchSlot, savedSlot);
            break;
          }
        }
      }
      b->emergencySavedSlot = -1;
      b->emergencyParked = false;
      if (!restored) {
        FLOW_LOG(scheduler, "[DISPATCH] No se pudo restaurar barco #%u, reencolando\n", b->id);
        b->state = STATE_WAITING;
        ship_scheduler_requeue_boat(scheduler, b, true);
        return false;
      }
    } else {
      // Barco nuevo: reservar la casilla de entrada del canal.
      int entryIndex = (b->origin == SIDE_LEFT) ? 0 : (int)(scheduler->listLength - 1);
      if (!ship_scheduler_try_reserve_range(scheduler, entryIndex, 1, b)) {
        FLOW_LOG(scheduler, "[DISPATCH] No se pudo reservar entrada para barco #%u, reencolando\n", b->id);
        ship_scheduler_requeue_boat(scheduler, b, true);
        return false;
      }
      b->currentSlot = entryIndex; // Primera casilla del barco en el canal.
      FLOW_LOG(scheduler, "[DISPATCH] Entrada reservada: barco #%u en casilla %d\n", b->id, entryIndex);
    }
  }

  if (b->serviceMillis == 0) {
    // Primer despacho: calcular el tiempo total de servicio.
    b->serviceMillis = ship_scheduler_estimate_service_millis(scheduler, b);
    b->remainingMillis = b->serviceMillis; // Sincronizar.
    if (b->deadlineMillis == 0) {
      b->deadlineMillis = b->startedAt + (b->serviceMillis * 2UL); // Deadline heuristico.
    }
  }

  // Recalcular remainingMillis si el barco fue restaurado en una casilla distinta
  // a la entrada (viene de preempcion: currentSlot > 0 = no esta desde la entrada).
  if (b->currentSlot >= 0 && b->serviceMillis > 0 && scheduler->listLength > 1) {
    int endIdx = (b->origin == SIDE_LEFT) ? (int)(scheduler->listLength - 1) : 0;
    int remSlots = (b->origin == SIDE_LEFT)
                   ? (endIdx - (int)b->currentSlot)
                   : ((int)b->currentSlot - endIdx);
    int totalSlots = (int)(scheduler->listLength - 1);
    if (remSlots > 0 && remSlots < totalSlots) {
      // remSlots < totalSlots: el barco no esta en la casilla de entrada (es preemptado).
      unsigned long newRemaining = (unsigned long)(
        ((float)remSlots / (float)totalSlots) * (float)b->serviceMillis + 0.5f);
      if (newRemaining == 0) newRemaining = 1;
      b->remainingMillis = newRemaining;
      FLOW_LOG(scheduler, "[DISPATCH] Barco #%u remainingMillis recalculado=%lu desde slot=%d (remSlots=%d)\n",
               b->id, b->remainingMillis, (int)b->currentSlot, remSlots);
    }
  }

  ship_scheduler_add_active(scheduler, b); // Agregar a activeBoats[].
  ship_logf("Dispatching -> barco #%u (rem=%lu svc=%lu)\n", b->id, b->remainingMillis, b->serviceMillis);

  // Contabilizar para la ventana de equidad FAIRNESS.
  if (scheduler->flowMode == FLOW_FAIRNESS && b->origin == scheduler->fairnessCurrentSide) {
    scheduler->fairnessPassedInWindow++; // Un barco mas del lado actual despacho.
    FLOW_LOG(scheduler, "[FLOW][FAIR] Despachado #%u lado=%s ventana=%u/%u\n",
             b->id, boatSideName(b->origin), scheduler->fairnessPassedInWindow,
             ship_scheduler_get_fairness_window(scheduler));
  }

  // Despachar segun algoritmo.
  if (scheduler->algorithm == ALG_RR) {
    // En RR el nuevo barco se convierte en primario y recibe el quantum completo.
    ship_scheduler_promote_active_to_front(scheduler, b); // Mover a activeBoats[0].
    b->allowedToMove = true;
    if (b->taskHandle) {
      FLOW_LOG(scheduler, "[DISPATCH DEBUG] Enviando NOTIF_CMD_RUN (RR primary) a barco #%u (taskHandle=%p)\n",
               b->id, (void*)b->taskHandle);
      // NOTIF_CMD_RUN desbloquea xTaskNotifyWait en boatTask; la tarea pasa de BLOCKED a READY.
      safe_task_notify(b->taskHandle, NOTIF_CMD_RUN);
      scheduler->activeQuantumAccumulatedMillis = 0; // Nuevo quantum: contador en 0.
      scheduler->activeQuantumStartedAt = millis();  // Marcar inicio del quantum.
    } else {
      FLOW_LOG(scheduler, "[DISPATCH DEBUG] ERROR: barco #%u NO TIENE taskHandle!\n", b->id);
    }
  } else {
    b->allowedToMove = true;
    if (b->taskHandle) {
      FLOW_LOG(scheduler, "[DISPATCH DEBUG] Enviando NOTIF_CMD_RUN a barco #%u (taskHandle=%p)\n",
               b->id, (void*)b->taskHandle);
      safe_task_notify(b->taskHandle, NOTIF_CMD_RUN); // Despertar la tarea del barco.
    } else {
      FLOW_LOG(scheduler, "[DISPATCH DEBUG] ERROR: barco #%u NO TIENE taskHandle!\n", b->id);
    }
  }

  ship_logf("Start -> barco #%u\n", b->id);
  return true;
} // Fin de ship_scheduler_start_next_boat. 

/*
 * ship_scheduler_preempt_active_for_rr — preempta al barco primario cuando su quantum expira.
 *
 * PROPOSITO:
 *   En Round Robin, cada barco recibe un quantum de tiempo fijo (rrQuantumMillis).
 *   Esta funcion verifica si el barco activo primario (activeBoat = activeBoats[0])
 *   ha consumido su quantum y, de ser asi, lo pausa para dar paso al siguiente.
 *   Es llamada cada tick (~50ms) desde ship_scheduler_update.
 *
 * CALCULO DEL QUANTUM CONSUMIDO:
 *   ship_scheduler_get_active_elapsed_millis(scheduler) retorna:
 *     accumulated + (now - startedAt)
 *   Donde:
 *     - accumulated: tiempo ya congelado de iteraciones previas del barco.
 *     - (now - startedAt): tiempo transcurrido desde el inicio del quantum actual.
 *   Si elapsed < rrQuantumMillis: el quantum NO ha expirado; retornar false.
 *
 * GUARD hasWorkable:
 *   Preemptar cuando no hay nadie que pueda continuar no tiene sentido: el barco
 *   pausado nunca recibiria NOTIF_CMD_RUN y se bloquearia permanentemente.
 *   hasWorkable = true si:
 *     a) activeCount > 1: ya hay otro activo en el canal que puede recibir el turno.
 *     b) readyQueue tiene algun candidato de mismo sentido con distancia minima segura.
 *   Si ninguno de los dos: retornar false (no preemptar).
 *
 * MECANISMO DE PAUSA:
 *   preempted->allowedToMove = false:
 *     La bandera que boatTask comprueba en cada iteracion del loop de movimiento.
 *     Cuando el barco ve allowedToMove==false despues de cada move, se detiene
 *     voluntariamente en su posicion actual SIN liberar la casilla.
 *     La casilla se conserva porque en RR los barcos co-existen en el canal;
 *     liberar la casilla permitiria que otro barco la pisara (colision).
 *   safe_task_notify(preempted->taskHandle, NOTIF_CMD_PAUSE):
 *     Escribe NOTIF_CMD_PAUSE en el ulNotifiedValue del TCB. boatTask lee esto en
 *     xTaskNotifyWait y entra al bloque 'cmd == NOTIF_CMD_PAUSE', poniendo
 *     running=false y esperando el proximo NOTIF_CMD_RUN.
 *   ship_scheduler_freeze_active_quantum(scheduler):
 *     accumulated += now - startedAt; startedAt = 0.
 *     Esto congela el contador para que el proximo barco empiece con slate limpio.
 *
 * RETORNO:
 *   true  -> se preempto exitosamente; ship_scheduler_update rota al siguiente.
 *   false -> quantum no expirado, emergencia activa, o nadie con quien rotar.
 */
static bool ship_scheduler_preempt_active_for_rr(ShipScheduler *scheduler) { // Preempcion por RR. 
  if (!scheduler || scheduler->activeCount == 0 || !scheduler->activeBoat) return false; // Valida activo. 
  if (scheduler->emergencyMode != EMERGENCY_NONE) return false; // Durante emergencia no rotamos ni despachamos.
  // TICO controla seguridad de casillas; RR controla quantum de tiempo. Son ortogonales:
  // no bloquear la preempcion por tiempo aunque TICO este activo.
  // Si no hay listos y solo un activo, nada que rotar.
  if (scheduler->readyCount == 0 && scheduler->activeCount <= 1) return false;
  // Solo preemptar si hay otro activo para promover o existe algun candidato listo
  // que pueda ser despachado sin riesgo de colision o cercania.
  bool hasWorkable = false;
  if (scheduler->activeCount > 1) hasWorkable = true; // ya hay otro activo en canal
  if (!hasWorkable && scheduler->readyCount > 0) {
    for (uint8_t ri = 0; ri < scheduler->readyCount; ri++) {
      Boat *cand = scheduler->readyQueue[ri];
      if (!cand) continue;
      bool safe = true;
      for (uint8_t ai = 0; ai < scheduler->activeCount; ai++) {
        Boat *active = scheduler->activeBoats[ai];
        if (!active) continue;
        // no permitir despacho de sentido opuesto si hay activo en ese sentido
        if (active->origin != cand->origin) { safe = false; break; }
        // si hay activo del mismo sentido, comprobar distancia minima
        if (active->currentSlot >= 0) {
          int dist = (cand->origin == SIDE_LEFT) ? (active->currentSlot - 0) : ((int)(scheduler->listLength - 1) - active->currentSlot);
          if (dist < (int)cand->stepSize) { safe = false; break; }
        }
      }
      if (safe) { hasWorkable = true; break; }
    }
  }
  if (!hasWorkable) return false; // No hay quien pueda correr ahora, no preemptar.
  if (!scheduler->activeBoat->allowedToMove) return false; // Ya esta pausado; no repetir la preempcion.
  // En RR, el modo visual-only (remainingMillis==1) NO exime al barco de preempcion:
  // el barco debe ceder su quantum como cualquier otro para no bloquear a los demas.
  // En otros algoritmos (STRN, EDF) la preempcion por desalojo se maneja aparte y
  // visualOnly si necesita proteccion; pero la funcion actual solo aplica a RR.
  if (ship_scheduler_get_active_elapsed_millis(scheduler) < scheduler->rrQuantumMillis) return false; // Si no consume quantum, sale. 

  Boat *preempted = scheduler->activeBoat; // Activo primario que consumio su quantum.
  if (preempted->taskHandle) {
    FLOW_LOG(scheduler, "[RR] Pausando activo #%u por quantum\n", preempted->id);
    preempted->allowedToMove = false; // Detener movimiento pero conservar la casilla.
    safe_task_notify(preempted->taskHandle, NOTIF_CMD_PAUSE); // Pausa la tarea.
  }

  ship_scheduler_freeze_active_quantum(scheduler); // Congela el quantum agotado.
  return true;
} // Fin de ship_scheduler_preempt_active_for_rr. 

/*
 * ship_scheduler_yield_active_for_rr — ceder voluntario del barco primario en RR.
 *
 * CUANDO SE LLAMA:
 *   Desde boatTask cuando el barco esta bloqueado esperando una casilla:
 *   el barco intento avanzar pero las casillas destino estan todas ocupadas.
 *   En lugar de quedarse girando en espera activa (busy-wait), el barco le avisa
 *   al scheduler que ceda el turno a otro barco que SI pueda moverse.
 *
 * GUARD 'activeBoat != boat':
 *   Solo el primario (activeBoats[0], al que se compara con scheduler->activeBoat)
 *   tiene permitido ceder su turno. Si un activo secundario llama a esta funcion,
 *   se ignora: el primario todavia puede estar moviendose y no hay conflicto de quantum.
 *
 * BUSQUEDA CIRCULAR DE SUCESOR:
 *   Recorre activeBoats[] desde currentIdx+1 (modulo activeCount) buscando al
 *   primer barco que pueda avanzar sus pasos completos:
 *     for offset in [1..activeCount-1]:
 *       idx = (currentIdx + offset) % activeCount
 *       candidate = activeBoats[idx]
 *       Simula los 'need' pasos del candidate verificando slotOwner[checkSlot]:
 *         slotOwner[checkSlot] == 0 (libre) o == candidate->id (ya es del barco)
 *       Si todos los pasos son libres: candidate es el sucesor.
 *
 * GUARD queueHasWorkable:
 *   Si ningun activo puede moverse, revisa readyQueue[] buscando un candidato
 *   de mismo sentido con distancia minima segura. Si existe, start_next_boat
 *   lo despachara al canal.
 *
 * CASO DE CANAL BLOQUEADO (ningun activo ni candidato puede avanzar):
 *   Reanudar al primario actual con allowedToMove=true + NOTIF_CMD_RUN.
 *   CRITICO: resetear activeQuantumAccumulatedMillis=0 antes de resume para que
 *   preempt_active_for_rr no lo preempte inmediatamente de nuevo (evita loop).
 *
 * PAUSA + ROTACION EQUITATIVA:
 *   1. boat->allowedToMove = false: bandera para detener el movimiento.
 *   2. safe_task_notify(boat->taskHandle, NOTIF_CMD_PAUSE): avisa a la tarea.
 *   3. freeze_active_quantum: congela el tiempo del barco saliente.
 *   4. rr_next_active: avanza rrTurnIndex al siguiente activo equitativamente.
 *   5. promote_active_to_front: sube al elegido a activeBoats[0].
 *   6. sync_rr_permissions: baja allowedToMove de TODOS los activos primero.
 *   7. chosen->allowedToMove = true: sube el permiso SOLO del elegido.
 *   8. safe_task_notify(chosen->taskHandle, NOTIF_CMD_RUN): desbloquea la tarea.
 */
static void ship_scheduler_yield_active_for_rr(ShipScheduler *scheduler, Boat *boat) {
  if (!scheduler || scheduler->algorithm != ALG_RR || !boat) return;
  if (scheduler->emergencyMode != EMERGENCY_NONE) return;
  if (scheduler->activeBoat != boat) return; // Solo el primario puede ceder su turno.

  // Encontrar la posicion actual del primario en la lista de activos.
  int currentIdx = -1;
  for (uint8_t i = 0; i < scheduler->activeCount; i++) {
    if (scheduler->activeBoats[i] == boat) { currentIdx = (int)i; break; }
  }

  // Buscar el siguiente activo en rotacion circular que pueda avanzar sus steps.
  Boat *successor = NULL;
  if (scheduler->activeCount > 1) {
    for (uint8_t offset = 1; offset < scheduler->activeCount; offset++) {
      uint8_t idx = (uint8_t)((currentIdx + offset) % scheduler->activeCount);
      Boat *candidate = scheduler->activeBoats[idx];
      if (!candidate || candidate == boat || candidate->currentSlot < 0) continue;
      // Verificar si el candidato puede avanzar al menos stepSize casillas.
      int dir = (candidate->origin == SIDE_LEFT) ? 1 : -1;
      int endIndex = (candidate->origin == SIDE_LEFT) ? (int)(scheduler->listLength - 1) : 0;
      int remSlots = (candidate->origin == SIDE_LEFT)
                     ? (endIndex - candidate->currentSlot)
                     : (candidate->currentSlot - endIndex);
      if (remSlots <= 0) continue; // Ya en el final, no necesita moverse.
      uint8_t need = (uint8_t)((remSlots < (int)candidate->stepSize) ? remSlots : candidate->stepSize);
      bool canMove = true;
      int checkSlot = candidate->currentSlot + dir;
      for (uint8_t s = 0; s < need; s++) {
        if (checkSlot < 0 || checkSlot >= (int)scheduler->listLength) { canMove = false; break; }
        if (scheduler->slotOwner && scheduler->slotOwner[checkSlot] != 0
            && scheduler->slotOwner[checkSlot] != candidate->id) { canMove = false; break; }
        checkSlot += dir;
      }
      if (canMove) { successor = candidate; break; }
    }
  }

  // Si ningun activo puede moverse, revisar si hay candidato seguro en la cola.
  bool queueHasWorkable = false;
  if (!successor && scheduler->readyCount > 0) {
    for (uint8_t ri = 0; ri < scheduler->readyCount; ri++) {
      Boat *cand = scheduler->readyQueue[ri];
      if (!cand) continue;
      bool safe = true;
      for (uint8_t ai = 0; ai < scheduler->activeCount; ai++) {
        Boat *active = scheduler->activeBoats[ai];
        if (!active) continue;
        if (active->origin != cand->origin) { safe = false; break; }
        if (active->currentSlot >= 0) {
          int dist = (cand->origin == SIDE_LEFT)
                     ? (active->currentSlot - 0)
                     : ((int)(scheduler->listLength - 1) - active->currentSlot);
          if (dist < (int)cand->stepSize) { safe = false; break; }
        }
      }
      if (safe) { queueHasWorkable = true; break; }
    }
  }

  if (!successor && !queueHasWorkable) {
    // Canal bloqueado: ningun activo ni candidato puede avanzar ahora.
    // Reanudar al primario actual para que siga intentando en el proximo tick.
    // CRITICO: resetear accumulated para que el barco tenga un quantum completo
    // antes de ser preemptado de nuevo (evita loop PAUSE/RUN/PAUSE/RUN).
    FLOW_LOG(scheduler, "[RR] Canal bloqueado; no hay sucesor viable para barco #%u\n", boat->id);
    boat->allowedToMove = true;
    if (boat->taskHandle) safe_task_notify(boat->taskHandle, NOTIF_CMD_RUN);
    scheduler->activeQuantumAccumulatedMillis = 0;
    ship_scheduler_resume_active_quantum(scheduler);
    return;
  }

  // Pausar este barco y congelar su quantum.
  boat->allowedToMove = false;
  if (boat->taskHandle) {
    FLOW_LOG(scheduler, "[RR] Barco #%u cede voluntariamente por bloqueo\n", boat->id);
    safe_task_notify(boat->taskHandle, NOTIF_CMD_PAUSE);
  }
  ship_scheduler_freeze_active_quantum(scheduler);

  // Usar el indice circular para elegir al sucesor activo de forma equitativa.
  // ship_scheduler_rr_next_active ya actualiza rrTurnIndex correctamente.
  Boat *chosen = ship_scheduler_rr_next_active(scheduler);
  if (!chosen || chosen == boat) chosen = successor; // Fallback al que encontramos antes.
  if (chosen && chosen != boat) {
    ship_scheduler_promote_active_to_front(scheduler, chosen);
    // FIX #1: sync_rr_permissions baja allowedToMove de todos los activos ANTES
    // de que el elegido lo vuelva a subir. Esto garantiza que el barco saliente
    // (boat, ya pausado arriba) no tenga allowedToMove=true residual cuando la
    // tarea del entrante se despierta.
    ship_scheduler_sync_rr_permissions(scheduler); // Primero bajar permisos de todos.
    chosen->allowedToMove = true;                  // Luego subir solo al elegido.
    if (chosen->taskHandle) {
      FLOW_LOG(scheduler, "[RR] Rotando a activo #%u tras yield de #%u\n", chosen->id, boat->id);
      safe_task_notify(chosen->taskHandle, NOTIF_CMD_RUN);
      scheduler->activeQuantumAccumulatedMillis = 0;
      ship_scheduler_resume_active_quantum(scheduler);
    }
    return;
  }

  // No hay activo viable pero si candidato en cola: intentar despacharlo.
  if (queueHasWorkable && ship_scheduler_start_next_boat(scheduler)) {
    ship_scheduler_sync_rr_permissions(scheduler);
    return;
  }

  // Ultimo recurso: reanudar al primario pausado para evitar bloqueo total.
  if (scheduler->activeCount >= 1 && scheduler->activeBoat) {
    Boat *preemptedBoat = scheduler->activeBoat;
    preemptedBoat->allowedToMove = true;
    if (preemptedBoat->taskHandle) {
      FLOW_LOG(scheduler, "[RR] No hay sucesor tras yield; reanudando activo #%u\n", preemptedBoat->id);
      safe_task_notify(preemptedBoat->taskHandle, NOTIF_CMD_RUN);
      ship_scheduler_resume_active_quantum(scheduler);
    }
    ship_scheduler_sync_rr_permissions(scheduler);
  }
}

/*
 * ship_scheduler_finish_active_boat — registra estadisticas y libera el barco finalizado.
 *
 * ESTADISTICAS ACUMULADAS (campos unsigned long en el struct ShipScheduler en heap):
 *
 *   totalWaitMillis += b->startedAt - b->enqueuedAt
 *     - enqueuedAt (unsigned long): timestamp millis() cuando el barco entro a readyQueue.
 *     - startedAt  (unsigned long): timestamp millis() cuando el barco recibio NOTIF_CMD_RUN
 *       por primera vez (primer despacho al canal).
 *     - La diferencia es el TIEMPO DE ESPERA en cola (wait time).
 *     - Guard: enqueuedAt>0 && startedAt>=enqueuedAt evita underflow en unsigned.
 *
 *   totalTurnaroundMillis += finishedAt - b->enqueuedAt
 *     - finishedAt: millis() en el momento que el barco llama a esta funcion.
 *     - turnaround = wait + service = tiempo total desde encolado hasta finalizacion.
 *     - Formulas estadisticas: promedio_wait = totalWaitMillis / completedTotal;
 *                              promedio_turnaround = totalTurnaroundMillis / completedTotal.
 *
 *   totalServiceMillis += b->serviceMillis
 *     - serviceMillis: tiempo estimado total del cruce (calculado en start_next_boat).
 *     - Permite calcular promedios de tiempo de CPU.
 *
 *   completionOrder[completionCount++] = b->id
 *     - Array de uint8_t en el struct (MAX_BOATS entradas) que guarda el orden
 *       de finalizacion. Util para comparar con el FCFS/SJF/EDF esperado.
 *
 * LIBERACION:
 *   b->state = STATE_DONE: marca el struct como finalizado (boatTask ya lo leyo).
 *   remove_active(scheduler, b): compacta activeBoats[], decrementa activeCount,
 *     ajusta rrTurnIndex si es necesario, actualiza el alias activeBoat.
 *   reset_active_quantum: accumulated=0, startedAt=0 para el proximo barco.
 *
 * NOTA:
 *   Esta funcion NO libera el struct Boat (b) del heap. La liberacion ocurre en
 *   boatTask despues de que el barco llega a su destino:
 *     destroyBoat(b)     -> free(b) en heap de C (libera el struct Boat).
 *     vTaskDelete(NULL)  -> FreeRTOS libera TCB+stack del heap de FreeRTOS.
 *   Si se llamara free(b) aqui, boatTask usaria memoria liberada (use-after-free).
 */
static void ship_scheduler_finish_active_boat(ShipScheduler *scheduler, Boat *b) { // Finaliza el barco activo. 
  if (!scheduler || !b) return; // Valida activo. 

  if (b->origin == SIDE_LEFT) scheduler->completedLeftToRight++; // Incrementa izq-der. 
  else scheduler->completedRightToLeft++; // Incrementa der-izq. 
  scheduler->completedTotal++; // Incrementa total. 
  if (scheduler->completionCount < MAX_BOATS) scheduler->completionOrder[scheduler->completionCount++] = b->id; // Guarda orden. 
  if (b->enqueuedAt > 0 && b->startedAt >= b->enqueuedAt) { // Si hay tiempos validos. 
    scheduler->totalWaitMillis += (b->startedAt - b->enqueuedAt); // Acumula espera. 
  } 
  if (b->enqueuedAt > 0) { // Si hay encolado. 
    unsigned long finishedAt = millis(); // Tiempo de fin. 
    if (finishedAt >= b->enqueuedAt) scheduler->totalTurnaroundMillis += (finishedAt - b->enqueuedAt); // Acumula turnaround. 
  } 
  scheduler->totalServiceMillis += b->serviceMillis; // Acumula servicio. 
  b->state = STATE_DONE; // Marca estado final. 
  ship_scheduler_remove_active(scheduler, b); // Quita de activos. 
  ship_logf("Barco finalizado: #%u tipo=%s origen=%s\n", b->id, boatTypeName(b->type), boatSideName(b->origin)); // Log de finalizacion con tipo. 
  // Reiniciar contabilidad de quantum activo para que el siguiente despacho
  // no quede bloqueado por el tiempo restante del barco terminado.
  ship_scheduler_reset_active_quantum(scheduler);

  // En RR, si quedan activos, reanudar inmediatamente al nuevo primario
  // (evita que se quede esperando NOTIF_CMD_RUN tras una finalizacion).
  if (scheduler->algorithm == ALG_RR && scheduler->activeCount > 0) {
    Boat *next = scheduler->activeBoat; // ya actualizado por remove_active
    if (next) {
      next->allowedToMove = true;
      if (next->taskHandle) {
        FLOW_LOG(scheduler, "[RR] Reanudando activo #%u tras finalizacion\n", next->id);
        safe_task_notify(next->taskHandle, NOTIF_CMD_RUN);
        ship_scheduler_resume_active_quantum(scheduler);
      }
    }
    ship_scheduler_sync_rr_permissions(scheduler); // Evita que otros activos sigan moviendose.
  }
} // Fin de ship_scheduler_finish_active_boat. 

/*
 * ship_scheduler_update — tick periodico del planificador (~50ms desde el loop principal).
 *
 * ESTA ES LA FUNCION MAS IMPORTANTE DEL PLANIFICADOR.
 * Es llamada desde SchedulingShips.ino cada ~50ms en el loop de Arduino/FreeRTOS.
 * Coordina TODOS los eventos de planificacion en un solo punto central:
 *
 * PASO 1 — update_emergency:
 *   Verifica el estado de la emergencia (sensor activado, temporizador de puertas).
 *   Si hay emergencia activa, bloquea nuevos despachos y notifica a barcos activos.
 *
 * PASO 2 — tick_sign:
 *   Si el flowMode es FLOW_SIGN, verifica si el timer del letrero expiro para
 *   cambiar signDirection (SIDE_LEFT <-> SIDE_RIGHT). El cambio habilita barcos
 *   del lado opuesto a entrar al canal en el proximo dispatch.
 *
 * PASO 3 — Recorrer activos buscando remainingMillis == 0:
 *   boatTask decrementa remainingMillis en cada paso; cuando llega a 0, el barco
 *   llego a su destino. finish_active_boat acumula estadisticas y llama
 *   remove_active (compacta activeBoats[]). El indice 'i' NO se incrementa
 *   despues de una finalizacion porque el array se compacto: el elemento que
 *   estaba en i+1 ahora esta en i.
 *
 * PASO 4a — Rama RR:
 *   a) preempt_active_for_rr: si el quantum del primario expiro (elapsed >= rrQuantumMillis)
 *      y hay al menos otro activo o candidato seguro, pausa al primario.
 *   b) Si se preempto:
 *      PRIORIDAD 1: rr_next_active → promote_active_to_front → sync_rr_permissions
 *        → chosen->allowedToMove=true → NOTIF_CMD_RUN al elegido.
 *      PRIORIDAD 2: si no hay otros activos, start_next_boat de readyQueue.
 *      PRIORIDAD 3: si tampoco hay candidatos seguros, reanudar al preemptado
 *        (accumulated=0 para evitar loop PAUSE/RUN).
 *   c) Si no hay activos y hay listos: start_next_boat (arranque inicial o reinicio).
 *
 * PASO 4b — Rama no-RR (FCFS, SJF, STRN, EDF, PRIORITY):
 *   Si no hay activos: start_next_boat.
 *
 * PASO 5 — FIX #6: Preempcion periodica para STRN y EDF:
 *   La preempcion al encolar (enqueue_with_deadline) solo detecta barcos NUEVOS.
 *   Pero en STRN, el activo consume remainingMillis y puede volverse 'mas largo'
 *   que alguien en cola. En EDF, el tiempo a deadline cambia con el reloj de pared.
 *   Este bloque re-evalua periodicamente si algun barco en readyQueue debe desalojar
 *   al activo actual:
 *
 *   STRN: candRem < activeRem → el candidato terminara antes que el activo.
 *   EDF:  candTimeLeft < activeTimeLeft → el candidato tiene deadline mas urgente.
 *
 *   Si shouldPreempt:
 *     1. ship_logf INCONDICIONAL (no FLOW_LOG) del mensaje de preempcion:
 *        "Preemption: barco #N desaloja a #M\n"
 *        CRITICO: el simulador Python parsea EXACTAMENTE esta cadena con
 *        re.search(r'desaloja a #(\d+)', line) para eliminar el cruce del canvas.
 *        Si se usa FLOW_LOG (condicional), el simulador no ve el mensaje cuando
 *        los logs de flujo estan desactivados → el barco desalojado se queda
 *        como 'ghost boat' estacionario en el display (bug resuelto en FIX #6).
 *     2. PAUSE al activo + freeze_active_quantum.
 *     3. Recalcular remainingMillis proporcional a casillas restantes.
 *     4. Liberar casilla (release_range) + emergencyParked=true + remove_active.
 *     5. requeue_boat(active, false): al FINAL de la cola (el candidato que lo derroto
 *        ya estaba esperando mas; el activo va al fondo, no al frente).
 *     6. start_next_boat: despacha al candidato mas urgente.
 */
void ship_scheduler_update(ShipScheduler *scheduler) { // Ejecuta un tick de planificacion. 
  if (!scheduler) return; // Valida puntero. 

  ship_scheduler_update_emergency(scheduler); // Actualiza estado de emergencia.
  ship_scheduler_tick_sign(scheduler); // Actualiza cambio de direccion para modo letrero.

  for (uint8_t i = 0; i < scheduler->activeCount; ) { // Recorre activos.
    Boat *active = scheduler->activeBoats[i]; // Toma el activo.
    if (!active) { // Si es nulo.
      i++; // Avanza.
      continue; // Sigue.
    }
    if (active->remainingMillis == 0) { // Si ya termino.
      ship_scheduler_finish_active_boat(scheduler, active); // Finaliza.
      continue; // Reevalua el indice por cambios.
    }
    i++; // Avanza.
  }

  if (scheduler->algorithm == ALG_RR) { // Si es RR.
    if (scheduler->emergencyMode != EMERGENCY_NONE) return; // Mientras dura la emergencia no avanzamos RR.
    bool preempted = ship_scheduler_preempt_active_for_rr(scheduler); // Detecta si el quantum se agotó.
    if (preempted) { // Si ya se pausa el actual, intenta mover otro barco.
      // PRIORIDAD 1: Rotar entre los activos ya en el canal.
      // Si se despacha un barco nuevo de readyQueue ANTES de rotar, sync_rr_permissions
      // pausa a todos los activos existentes y solo el recien despachado corre. Esto hace
      // que los barcos ya en el canal nunca vuelvan a recibir su quantum (se quedan quietos).
      // La correccion: rotar primero entre activos; solo despachar de la cola si no hay
      // nadie mas con quien rotar.
      if (scheduler->activeCount > 1) {
        Boat *nextActive = ship_scheduler_rr_next_active(scheduler);
        if (nextActive && nextActive != scheduler->activeBoat) {
          ship_scheduler_promote_active_to_front(scheduler, nextActive);
          ship_scheduler_sync_rr_permissions(scheduler); // Primero bajar permisos de todos.
          nextActive->allowedToMove = true;             // Luego subir solo al nuevo primario.
          if (nextActive->taskHandle) {
            FLOW_LOG(scheduler, "[RR] Rotando a activo #%u tras quantum\n", nextActive->id);
            safe_task_notify(nextActive->taskHandle, NOTIF_CMD_RUN);
            scheduler->activeQuantumAccumulatedMillis = 0;
            ship_scheduler_resume_active_quantum(scheduler);
          }
          return;
        }
      }

      // PRIORIDAD 2: Solo un activo (o ninguno rotable); intentar despachar de la cola.
      bool started = false;
      if (scheduler->readyCount > 0) started = ship_scheduler_start_next_boat(scheduler);
      if (started) {
        ship_scheduler_sync_rr_permissions(scheduler); // Garantiza que solo el nuevo primario avance.
        return; // Se despacho un sucesor válido.
      }

      // Ningun sucesor valido ni otro activo: reanudar al preempted (evita quedarse bloqueado).
      // CRITICO: resetear accumulated a 0 antes de resume. Si no, get_active_elapsed devuelve
      // el quantum anterior (>=1000ms) y preempt_active_for_rr vuelve a disparar en el
      // proximo tick (50ms despues), creando un loop PAUSE/RUN/PAUSE/RUN que congela al barco.
      if (scheduler->activeCount >= 1 && scheduler->activeBoat) {
        Boat *preemptedBoat = scheduler->activeBoat;
        preemptedBoat->allowedToMove = true;
        if (preemptedBoat->taskHandle) {
          FLOW_LOG(scheduler, "[RR] No hay sucesor valido; reanudando activo #%u\n", preemptedBoat->id);
          safe_task_notify(preemptedBoat->taskHandle, NOTIF_CMD_RUN);
          scheduler->activeQuantumAccumulatedMillis = 0;
          ship_scheduler_resume_active_quantum(scheduler);
        }
        ship_scheduler_sync_rr_permissions(scheduler);
        return;
      }
    }
    if (scheduler->activeCount == 0 && scheduler->readyCount > 0) { // Arranque inicial o despues de terminar todo.
      ship_scheduler_start_next_boat(scheduler);
    }
    return;
  }

  // Algoritmos no RR: ejecucion secuencial (un solo barco activo).
  if (scheduler->activeCount == 0 && scheduler->readyCount > 0) {
    ship_scheduler_start_next_boat(scheduler);
    return;
  }

  // FIX #6: Re-evaluar preempcion periodica para STRN y EDF.
  // La preempcion al encolar un barco nuevo solo cubre el caso en que un barco
  // mas urgente LLEGA. Pero en STRN el activo va consumiendo remainingMillis y
  // puede volverse mas largo que alguien ya en cola. En EDF, el tiempo que falta
  // para el deadline de barcos en cola cambia con el reloj: un barco puede
  // volverse mas urgente que el activo sin que haya llegado nadie nuevo.
  if (scheduler->activeCount > 0 && scheduler->activeBoat && scheduler->readyCount > 0) {
    if (scheduler->algorithm == ALG_STRN || scheduler->algorithm == ALG_EDF) {
      bool shouldPreempt = false;
      Boat *active = scheduler->activeBoat;
      Boat *periodicPreemptor = NULL; // Candidato que provoca la preempcion periodica.

      for (uint8_t ri = 0; ri < scheduler->readyCount; ri++) {
        Boat *cand = scheduler->readyQueue[ri];
        if (!cand) continue;
        // No preemptar si el candidato tiene sentido opuesto al activo (colision).
        if (cand->origin != active->origin) continue;

        if (scheduler->algorithm == ALG_STRN) {
          unsigned long candRem = (cand->remainingMillis > 1) ? cand->remainingMillis
                                  : (cand->serviceMillis > 0 ? cand->serviceMillis
                                  : ship_scheduler_estimate_service_millis(scheduler, cand));
          unsigned long activeRem = (active->remainingMillis > 1) ? active->remainingMillis
                                    : (active->serviceMillis > 0 ? active->serviceMillis
                                    : ship_scheduler_estimate_service_millis(scheduler, active));
          if (candRem < activeRem) { shouldPreempt = true; periodicPreemptor = cand; break; }
        } else { // ALG_EDF
          unsigned long now = millis();
          unsigned long candTimeLeft = cand->deadlineMillis > now ? cand->deadlineMillis - now : 0UL;
          unsigned long activeTimeLeft = active->deadlineMillis > now ? active->deadlineMillis - now : 0UL;
          if (candTimeLeft < activeTimeLeft) { shouldPreempt = true; periodicPreemptor = cand; break; }
        }
      }

      if (shouldPreempt && active->allowedToMove) {
        // Log incondicional (no FLOW_LOG) para que el simulador Python siempre
        // detecte la preempcion periodica y elimine el cruce del barco desalojado.
        ship_logf("Preemption: barco #%u desaloja a #%u\n",
                  periodicPreemptor ? periodicPreemptor->id : 0, active->id);
        FLOW_LOG(scheduler, "[PREEMPT PERIODIC] Desalojando activo #%u por candidato mas urgente en cola\n", active->id);
        // Detener el activo.
        active->allowedToMove = false;
        if (active->taskHandle) safe_task_notify(active->taskHandle, NOTIF_CMD_PAUSE);
        ship_scheduler_freeze_active_quantum(scheduler);
        // Recalcular remainingMillis del desalojado por posicion en canal.
        if (active->currentSlot >= 0 && scheduler->listLength > 0 && active->serviceMillis > 0) {
          int endIndex = (active->origin == SIDE_LEFT) ? (int)(scheduler->listLength - 1) : 0;
          int slotsRemaining = (active->origin == SIDE_LEFT)
                               ? (endIndex - active->currentSlot)
                               : (active->currentSlot - endIndex);
          int totalSlots = (int)(scheduler->listLength - 1);
          if (totalSlots > 0 && slotsRemaining >= 0) {
            active->remainingMillis = (unsigned long)(
              ((float)slotsRemaining / (float)totalSlots) * (float)active->serviceMillis + 0.5f);
            if (active->remainingMillis == 0) active->remainingMillis = 1;
          }
        }
        // Guardar posicion, liberar casilla y estacionar.
        // Igual que en la preempcion por enqueue: hay que liberar la casilla para
        // que el sucesor no se bloquee al llegar a ella.
        active->emergencySavedSlot = active->currentSlot;
        active->emergencyParked = true;
        if (active->currentSlot >= 0) {
          ship_scheduler_release_range(scheduler, active->currentSlot, 1, active);
          active->currentSlot = -1;
        }
        ship_scheduler_remove_active(scheduler, active);
        ship_scheduler_requeue_boat(scheduler, active, false); // Al final, no al frente (otro ya era mas urgente).
        ship_scheduler_start_next_boat(scheduler);
      }
    }
  }
} // Fin de ship_scheduler_update. 

const Boat *ship_scheduler_get_active_boat(const ShipScheduler *scheduler) { // Devuelve barco activo. 
  if (!scheduler || scheduler->activeCount == 0) return NULL; // Valida estado. 
  return scheduler->activeBoat; // Retorna activo. 
} // Fin de ship_scheduler_get_active_boat. 

uint8_t ship_scheduler_get_active_count(const ShipScheduler *scheduler) { // Devuelve cantidad de activos.
  return scheduler ? scheduler->activeCount : 0; // Retorna count o cero.
} // Fin de ship_scheduler_get_active_count.

const Boat *ship_scheduler_get_active_boat_at(const ShipScheduler *scheduler, uint8_t index) { // Devuelve activo por indice.
  if (!scheduler || index >= scheduler->activeCount) return NULL; // Valida rango.
  return scheduler->activeBoats[index]; // Retorna activo.
} // Fin de ship_scheduler_get_active_boat_at.

uint8_t ship_scheduler_get_ready_count(const ShipScheduler *scheduler) { // Devuelve cantidad en cola. 
  return scheduler ? scheduler->readyCount : 0; // Retorna count o cero. 
} // Fin de ship_scheduler_get_ready_count. 

const Boat *ship_scheduler_get_ready_boat(const ShipScheduler *scheduler, uint8_t index) { // Devuelve barco en cola. 
  if (!scheduler || index >= scheduler->readyCount) return NULL; // Valida rango. 
  return scheduler->readyQueue[index]; // Retorna el barco. 
} // Fin de ship_scheduler_get_ready_boat. 

uint8_t ship_scheduler_get_completion_id(const ShipScheduler *scheduler, uint8_t index) { // Devuelve ID de finalizacion. 
  if (!scheduler || index >= scheduler->completionCount) return 0; // Valida rango. 
  return scheduler->completionOrder[index]; // Retorna el ID. 
} // Fin de ship_scheduler_get_completion_id. 

uint8_t ship_scheduler_get_waiting_count(const ShipScheduler *scheduler, BoatSide side) { // Cuenta barcos por lado. 
  if (!scheduler) return 0; // Valida puntero. 
  uint8_t count = 0; // Contador local. 
  for (uint8_t i = 0; i < scheduler->readyCount; i++) { // Recorre cola. 
    if (scheduler->readyQueue[i]->origin == side) count++; // Cuenta si coincide el lado. 
  } 
  return count; // Retorna el conteo. 
} // Fin de ship_scheduler_get_waiting_count. 

const Boat *ship_scheduler_get_waiting_boat(const ShipScheduler *scheduler, BoatSide side, uint8_t index) { // Devuelve barco por lado e indice. 
  if (!scheduler) return NULL; // Valida puntero. 
  uint8_t seen = 0; // Contador de vistos. 
  for (uint8_t i = 0; i < scheduler->readyCount; i++) { // Recorre la cola. 
    if (scheduler->readyQueue[i]->origin != side) continue; // Salta si no coincide. 
    if (seen == index) return scheduler->readyQueue[i]; // Retorna si coincide el indice. 
    seen++; // Incrementa vistos. 
  } 
  return NULL; // Retorna nulo si no existe. 
} // Fin de ship_scheduler_get_waiting_boat. 

uint16_t ship_scheduler_get_completed_left_to_right(const ShipScheduler *scheduler) { // Devuelve completados izq-der. 
  return scheduler ? scheduler->completedLeftToRight : 0; // Retorna contador o cero. 
} // Fin de ship_scheduler_get_completed_left_to_right. 

uint16_t ship_scheduler_get_completed_right_to_left(const ShipScheduler *scheduler) { // Devuelve completados der-izq. 
  return scheduler ? scheduler->completedRightToLeft : 0; // Retorna contador o cero. 
} // Fin de ship_scheduler_get_completed_right_to_left. 

unsigned long ship_scheduler_get_active_elapsed_millis(const ShipScheduler *scheduler) { // Devuelve tiempo transcurrido. 
  if (!scheduler || scheduler->activeCount == 0 || !scheduler->activeBoat) return 0; // Valida estado. 
  // Usa el quantum acumulado más el tramo en curso.
  if (scheduler->activeQuantumStartedAt > 0) {
    unsigned long now = millis();
    if (now >= scheduler->activeQuantumStartedAt) return scheduler->activeQuantumAccumulatedMillis + (now - scheduler->activeQuantumStartedAt);
    return scheduler->activeQuantumAccumulatedMillis;
  }
  return scheduler->activeQuantumAccumulatedMillis; // Quantum congelado o pausado.
} // Fin de ship_scheduler_get_active_elapsed_millis. 

uint16_t ship_scheduler_get_completed_total(const ShipScheduler *scheduler) { // Devuelve total completados. 
  return scheduler ? scheduler->completedTotal : 0; // Retorna contador o cero. 
} // Fin de ship_scheduler_get_completed_total. 

uint16_t ship_scheduler_get_collision_detections(const ShipScheduler *scheduler) { // Devuelve colisiones detectadas.
  return scheduler ? scheduler->collisionDetections : 0; // Retorna contador o cero.
} // Fin de ship_scheduler_get_collision_detections.

unsigned long ship_scheduler_get_total_wait_millis(const ShipScheduler *scheduler) { // Devuelve espera total. 
  return scheduler ? scheduler->totalWaitMillis : 0; // Retorna acumulado o cero. 
} // Fin de ship_scheduler_get_total_wait_millis. 

unsigned long ship_scheduler_get_total_turnaround_millis(const ShipScheduler *scheduler) { // Devuelve turnaround total. 
  return scheduler ? scheduler->totalTurnaroundMillis : 0; // Retorna acumulado o cero. 
} // Fin de ship_scheduler_get_total_turnaround_millis. 

unsigned long ship_scheduler_get_total_service_millis(const ShipScheduler *scheduler) { // Devuelve servicio total. 
  return scheduler ? scheduler->totalServiceMillis : 0; // Retorna acumulado o cero. 
} // Fin de ship_scheduler_get_total_service_millis. 

uint8_t ship_scheduler_get_completion_count(const ShipScheduler *scheduler) { // Devuelve cantidad en orden final. 
  return scheduler ? scheduler->completionCount : 0; // Retorna count o cero. 
} // Fin de ship_scheduler_get_completion_count. 

void ship_scheduler_notify_boat_finished(ShipScheduler *scheduler, Boat *b) { // Callback de finalizacion. 
  if (!scheduler || !b) return; // Valida punteros. 
  if (scheduler->ignoreCompletions || b->cancelled) return; // Ignora si corresponde. 

  for (uint8_t i = 0; i < scheduler->activeCount; i++) { // Recorre activos.
    if (scheduler->activeBoats[i] == b) { // Si era activo.
      ship_scheduler_finish_active_boat(scheduler, b); // Finaliza normalmente.
      return; // Sale.
    }
  }

  for (uint8_t i = 0; i < scheduler->readyCount; i++) { // Recorre cola. 
    if (scheduler->readyQueue[i] == b) { // Si encuentra el barco. 
      for (uint8_t j = i + 1; j < scheduler->readyCount; j++) scheduler->readyQueue[j - 1] = scheduler->readyQueue[j]; // Compacta cola. 
      scheduler->readyCount--; // Reduce contador. 
      return; // Sale. 
    } 
  } 
} // Fin de ship_scheduler_notify_boat_finished. 

void ship_scheduler_pause_active(ShipScheduler *scheduler) { // Pausa el barco activo. 
  if (!scheduler || scheduler->activeCount == 0) { // Valida activo.
    ship_logln("No hay barco activo para pausar."); // Mensaje de error. 
    return; // Sale.
  }

  if (scheduler->flowMode == FLOW_TICO) { // Pausa todos los activos.
    for (uint8_t i = 0; i < scheduler->activeCount; i++) {
      Boat *active = scheduler->activeBoats[i];
      if (!active) continue;
      if (active->taskHandle) {
        active->allowedToMove = false; // Congela movimiento del barco.
        safe_task_notify(active->taskHandle, NOTIF_CMD_PAUSE); // Envia pausa.
      }
      ship_logf("Pausado barco #%u\n", active->id); // Log de pausa.
    }
    return; // Sale.
  }

  if (scheduler->activeBoat && scheduler->activeBoat->taskHandle) { // Si hay tarea. 
    scheduler->activeBoat->allowedToMove = false; // Congela movimiento del barco. 
    safe_task_notify(scheduler->activeBoat->taskHandle, NOTIF_CMD_PAUSE); // Envia pausa. 
  } 
  ship_logf("Pausado barco #%u\n", scheduler->activeBoat ? scheduler->activeBoat->id : 0); // Log de pausa. 
} // Fin de ship_scheduler_pause_active. 

void ship_scheduler_resume_active(ShipScheduler *scheduler) { // Reanuda el barco activo. 
  if (!scheduler || scheduler->activeCount == 0) { // Valida activo.
    ship_logln("No hay barco activo para reanudar."); // Mensaje de error. 
    return; // Sale.
  }

  if (scheduler->flowMode == FLOW_TICO) { // Reanuda todos los activos.
    for (uint8_t i = 0; i < scheduler->activeCount; i++) {
      Boat *active = scheduler->activeBoats[i];
      if (!active) continue;
      if (active->taskHandle) {
        active->allowedToMove = true; // Permite movimiento del barco.
        safe_task_notify(active->taskHandle, NOTIF_CMD_RUN); // Envia run.
      }
      ship_logf("Reanudado barco #%u\n", active->id); // Log de reanudacion.
    }
    return; // Sale.
  }

  if (scheduler->activeBoat && scheduler->activeBoat->taskHandle) { // Si hay tarea. 
    scheduler->activeBoat->allowedToMove = true; // Permite movimiento del barco. 
    safe_task_notify(scheduler->activeBoat->taskHandle, NOTIF_CMD_RUN); // Envia run. 
  } 
  ship_logf("Reanudado barco #%u\n", scheduler->activeBoat ? scheduler->activeBoat->id : 0); // Log de reanudacion. 
} // Fin de ship_scheduler_resume_active. 


void ship_scheduler_dump_status(const ShipScheduler *scheduler) { // Imprime estado del scheduler. 
  if (!scheduler) return; // Valida puntero. 
  ship_logln("--- Scheduler Status ---"); // Encabezado. 
  ship_logf("Algorithm: %s\n", ship_scheduler_get_algorithm_label(scheduler)); // Algoritmo actual. 
  ship_logf("Flow: %s\n", ship_scheduler_get_flow_mode_label(scheduler)); // Metodo de flujo.
  ship_logf("W=%u Sign=%s/%lums QueueMax=%u\n", ship_scheduler_get_fairness_window(scheduler), boatSideName(ship_scheduler_get_sign_direction(scheduler)), ship_scheduler_get_sign_interval(scheduler), ship_scheduler_get_max_ready_queue(scheduler)); // Parametros de flujo.
  ship_logf("Canal=%um Vel=%um/s Collisions=%u\n", ship_scheduler_get_channel_length(scheduler), ship_scheduler_get_boat_speed(scheduler), ship_scheduler_get_collision_detections(scheduler)); // Parametros de canal y colisiones.
  ship_logf("FlowLog: %s\n", ship_scheduler_get_flow_logging(scheduler) ? "ON" : "OFF"); // Estado de trazas de flujo.
  ship_logf("Ready count: %u\n", scheduler->readyCount); // Cantidad en cola. 
  for (uint8_t i = 0; i < scheduler->readyCount; i++) { // Recorre cola. 
    Boat *b = scheduler->readyQueue[i]; // Obtiene barco. 
    ship_logf("%u: #%u %s from %s rem=%lu\n", i, b->id, boatTypeShort(b->type), boatSideName(b->origin), b->remainingMillis); // Imprime linea. 
  } 
  ship_logf("Active count: %u\n", scheduler->activeCount); // Cantidad de activos.
  if (scheduler->activeCount > 0) { // Si hay activos.
    for (uint8_t i = 0; i < scheduler->activeCount; i++) { // Recorre activos.
      Boat *b = scheduler->activeBoats[i]; // Obtiene activo.
      if (!b) continue; // Salta nulos.
      ship_logf("Active %u: #%u rem=%lu\n", i, b->id, b->remainingMillis); // Imprime activo.
    }
  } else { // Si no hay activo.
    ship_logln("Active: none"); // Informa sin activo.
  } 
  ship_logln("------------------------"); // Cierra el bloque. 
} // Fin de ship_scheduler_dump_status.