#include "ShipDisplay.h" // API publica de la pantalla.
#include "ShipIO.h" // Logging por serial.

#include <freertos/FreeRTOS.h> // Tipos base FreeRTOS.
#include <freertos/semphr.h> // SemaphoreHandle_t y API de mutex.
#include <math.h>

// Colores basicos en formato RGB565 (sin depender de encabezados C++).
static const uint16_t COLOR_BLACK = 0x0000; // Negro.
static const uint16_t COLOR_WHITE = 0xFFFF; // Blanco.
static const uint16_t COLOR_BLUE = 0x001F; // Azul.
static const uint16_t COLOR_YELLOW = 0xFFE0; // Amarillo.
static const uint16_t COLOR_GREEN = 0x07E0; // Verde.
static const uint16_t COLOR_RED = 0xF800; // Rojo.
static const uint16_t SHIP_DARK_GREY = 0x7BEF; // Gris oscuro para bordes.
static const uint16_t SHIP_NAVY = 0x000F; // Azul marino para encabezado y pie.
static const uint16_t CANAL_BG = 0x18E3; // Color del fondo del canal.

// Dimensiones y posiciones de la interfaz de la pantalla.
static const uint8_t SCREEN_W = 160; // Ancho en modo horizontal.
static const uint8_t SCREEN_H = 128; // Alto en modo horizontal.
static const uint8_t HEADER_H = 12; // Alto del encabezado.
static const uint8_t FOOTER_H = 14; // Alto del pie.
static const uint8_t SIDE_PANEL_W = 30; // Ancho de panel lateral.
static const uint8_t CANAL_X = SIDE_PANEL_W; // X inicial del canal.
static const uint8_t CANAL_W = SCREEN_W - (SIDE_PANEL_W * 2); // Ancho del canal.
static const uint8_t CANAL_Y = HEADER_H; // Y inicial del canal.
static const uint8_t CANAL_H = SCREEN_H - HEADER_H - FOOTER_H; // Alto del canal.
static const uint8_t BOAT_SIZE = 12; // Tamano del cuadrado de barco.
static const uint8_t LABEL_Y = HEADER_H + 2; // Y del texto de etiquetas.
static const uint8_t FOOTER_Y = SCREEN_H - FOOTER_H; // Y del pie.
static const uint8_t WAIT_AREA_Y = CANAL_Y + 12; // Y de inicio de cola visible.
static const uint8_t WAIT_SLOT_SPACING = 24; // Distancia entre slots.
static const uint8_t WAIT_COUNT_Y = CANAL_Y + CANAL_H - 12; // Y del contador de cola.
static const uint8_t INFO_H = 10; // Alto del texto de info en canal.
static const uint8_t INFO_Y = FOOTER_Y - INFO_H - 1; // Y del texto de info.
static const uint8_t PANEL_INSET_X = 2; // Margen interno de panel.

// Estado interno de renderizado para limitar refrescos y cachear el diseno.
static unsigned long gLastUiRefresh = 0; // Marca del ultimo refresco.
static bool gLayoutDrawn = false; // Indica si el diseno base ya fue dibujado.
static ShipAlgo gLastAlgorithm = ALG_FCFS; // Ultimo algoritmo mostrado.

// Mutex para proteger accesos concurrentes a la pantalla desde tareas.
static SemaphoreHandle_t gDisplayMutex = NULL;

// Cache de la posicion previa del barco activo para evitar parpadeo.
static int16_t gPrevBoatX = -1; // X previa del barco.
static int16_t gPrevBoatY = -1; // Y previa del barco.
static int16_t gPrevBoatW = 0; // Ancho previo del barco.
static int16_t gPrevBoatH = 0; // Alto previo del barco.

// Cache del ultimo ID de barco renderizado.
static uint8_t gPrevRenderedBoatId = 0; // ID del ultimo barco dibujado.

// Resetea el cache de la posicion previa del barco activo.
static void reset_prev_boat_rect_cache(void) {
  gPrevBoatX = -1;
  gPrevBoatY = -1;
  gPrevBoatW = 0;
  gPrevBoatH = 0;
}

// Calcula elapsed de un barco sin underflow.
static unsigned long ship_display_boat_elapsed_millis(const Boat *boat) {
  if (!boat) return 0; // Valida puntero.
  if (boat->serviceMillis <= boat->remainingMillis) return 0; // Evita underflow.
  return boat->serviceMillis - boat->remainingMillis; // Retorna elapsed.
}

// Mapea un indice de casilla de la lista a una coordenada X en pixeles dentro del canal.
static int16_t slot_index_to_x(const ShipScheduler *s, int16_t slotIndex) {
  if (!s) return CANAL_X + 2;
  if (s->listLength == 0) return CANAL_X + 2;
  // Si el barco aun no tiene slot (-1) devolvemos inicio segun lado por seguridad.
  if (slotIndex < 0) return CANAL_X + 2;

  float ratio = ship_scheduler_get_list_to_visual_ratio(s); // visual per list-slot
  int visualIndex = (int)roundf(slotIndex * ratio);
  int visualLen = s->visualChannelLength > 0 ? s->visualChannelLength : s->listLength;
  int availPixels = CANAL_W - BOAT_SIZE - 4; // espacio util
  if (visualLen <= 1) return CANAL_X + 2;
  if (visualIndex < 0) visualIndex = 0;
  if (visualIndex > visualLen - 1) visualIndex = visualLen - 1;
  int16_t x = CANAL_X + 2 + (visualIndex * availPixels) / (visualLen - 1);
  return x;
}

// Dibuja el fondo estatico de la interfaz.
static void draw_static_layout(const char *algoLabel) {
  ship_display_hw_fill_screen(COLOR_BLACK); // Limpia toda la pantalla.
  ship_display_invalidate_boat_cache(); // Invalida caches al redibujar el diseno completo.

  ship_display_hw_fill_rect(0, 0, SCREEN_W, HEADER_H, SHIP_NAVY); // Encabezado.
  ship_display_hw_set_text_wrap(true); // Activa el ajuste de texto.
  ship_display_hw_set_text_size(1); // Tamano de fuente base.
  ship_display_hw_set_text_color(COLOR_WHITE, COLOR_BLACK); // Color del texto.
  ship_display_hw_set_cursor(8, 4); // Posicion del titulo.
  ship_display_hw_print_str("Scheduling Ships - "); // Titulo base.
  ship_display_hw_print_str(algoLabel ? algoLabel : "?"); // Etiqueta del algoritmo.

  ship_display_hw_fill_rect(0, HEADER_H, SIDE_PANEL_W, CANAL_H, COLOR_BLACK); // Panel izquierdo.
  ship_display_hw_fill_rect(CANAL_X, HEADER_H, CANAL_W, CANAL_H, CANAL_BG); // Fondo del canal.
  ship_display_hw_fill_rect(SCREEN_W - SIDE_PANEL_W, HEADER_H, SIDE_PANEL_W, CANAL_H, COLOR_BLACK); // Panel derecho.

  ship_display_hw_draw_rect(0, HEADER_H, SIDE_PANEL_W, CANAL_H, COLOR_BLUE); // Borde izq.
  ship_display_hw_draw_rect(CANAL_X, HEADER_H, CANAL_W, CANAL_H, COLOR_WHITE); // Borde canal.
  ship_display_hw_draw_rect(SCREEN_W - SIDE_PANEL_W, HEADER_H, SIDE_PANEL_W, CANAL_H, COLOR_BLUE); // Borde der.

  ship_display_hw_set_text_color(COLOR_YELLOW, COLOR_BLACK); // Color de etiquetas.
  ship_display_hw_set_cursor(PANEL_INSET_X, LABEL_Y); // Etiqueta izquierda.
  ship_display_hw_print_str("IZQ"); // Texto izquierda.
  ship_display_hw_set_cursor(CANAL_X + 6, LABEL_Y); // Etiqueta canal.
  ship_display_hw_print_str("CANAL"); // Texto canal.
  ship_display_hw_set_cursor(SCREEN_W - SIDE_PANEL_W + PANEL_INSET_X, LABEL_Y); // Etiqueta derecha.
  ship_display_hw_print_str("DER"); // Texto derecha.

  gLayoutDrawn = true; // Marca que el layout ya se dibujo.
}

// Dibuja un barco como un cuadrado con borde y etiqueta.
static void draw_boat_square(int16_t x, int16_t y, const Boat *boat, bool highlight) {
  if (!boat) return; // Evita acceso nulo.
  uint16_t color = boatColor(boat->type); // Color por tipo.
  ship_display_hw_fill_rect(x, y, BOAT_SIZE, BOAT_SIZE, color); // Cuerpo del barco.
  ship_display_hw_draw_rect(x, y, BOAT_SIZE, BOAT_SIZE, highlight ? COLOR_WHITE : SHIP_DARK_GREY); // Borde.
  ship_display_hw_set_text_color(highlight ? COLOR_WHITE : COLOR_BLACK, color); // Texto invertido si esta activo.
  ship_display_hw_set_cursor(x + 3, y + 3); // Posicion del texto.
  ship_display_hw_print_str(boatTypeShort(boat->type)); // Imprime letra de tipo.
}

// Dibuja los barcos en espera de un lado.
static void draw_waiting_side(const ShipScheduler *scheduler, BoatSide side) {
  int16_t panelX = side == SIDE_LEFT ? PANEL_INSET_X : (SCREEN_W - SIDE_PANEL_W + PANEL_INSET_X); // X del panel.
  ship_display_hw_fill_rect(panelX - 1, WAIT_AREA_Y - 2, BOAT_SIZE + 4, CANAL_H - 18, COLOR_BLACK); // Limpia area.

  uint8_t waitingCount = ship_scheduler_get_waiting_count(scheduler, side); // Total en cola.
  uint8_t visibleCount = ship_display_get_visible_waiting_count(waitingCount); // Limite visible.

  for (uint8_t i = 0; i < visibleCount; i++) { // Itera los visibles.
    const Boat *boat = ship_scheduler_get_waiting_boat(scheduler, side, i); // Barco en posicion i.
    if (!boat) continue; // Salta nulos.
    int16_t slotY = WAIT_AREA_Y + (WAIT_SLOT_SPACING * i); // Calcula Y del slot.
    draw_boat_square(panelX, slotY, boat, false); // Dibuja el barco en espera.
  }

  ship_display_hw_set_text_color(COLOR_WHITE, COLOR_BLACK); // Color del contador.
  ship_display_hw_set_cursor(panelX - 1, WAIT_COUNT_Y); // Posicion del contador.
  ship_display_hw_print_uint(waitingCount); // Muestra el numero.
}

// Dibuja el barco activo en el canal y su banda de informacion.
static void draw_active_boat(const ShipScheduler *scheduler) {
  ship_display_hw_fill_rect(CANAL_X + 2, INFO_Y - 1, CANAL_W - 4, INFO_H + 2, CANAL_BG); // Limpia banda de info.

  ship_display_hw_set_text_size(1); // Fuente base.
  ship_display_hw_set_text_color(COLOR_WHITE, COLOR_BLACK); // Color del texto.
  ship_display_hw_set_cursor(CANAL_X + 6, CANAL_Y + 3); // Posicion del texto.
  ship_display_hw_print_str("Canal"); // Titulo del canal.

  uint8_t activeCount = ship_scheduler_get_active_count(scheduler); // Cantidad de activos.
  const Boat *activeBoat = ship_scheduler_get_active_boat(scheduler); // Barco activo actual.

  if (activeCount > 1) { // Si hay varios activos.
    ship_display_hw_fill_rect(CANAL_X + 1, CANAL_Y + 1, CANAL_W - 2, CANAL_H - 2, CANAL_BG); // Limpia todo el canal.
    reset_prev_boat_rect_cache(); // Reinicia el cache de la posicion previa.
    gPrevRenderedBoatId = 0; // Reinicia cache de ID.

    for (uint8_t i = 0; i < activeCount; i++) { // Dibuja cada activo.
      const Boat *boat = ship_scheduler_get_active_boat_at(scheduler, i); // Barco activo.
      if (!boat) continue; // Salta nulos.
      if (boat->emergencyParked) continue; // No dibuja barcos retirados temporalmente del canal.
      // Preferimos posicion por casilla si el barco indica currentSlot
      int16_t boatX;
      if (boat->currentSlot >= 0 && scheduler->listLength > 0) {
        boatX = slot_index_to_x(scheduler, boat->currentSlot);
      } else {
        unsigned long elapsed = ship_display_boat_elapsed_millis(boat);
        int16_t travelStart = boat->origin == SIDE_RIGHT ? CANAL_X + CANAL_W - BOAT_SIZE - 2 : CANAL_X + 2; // Inicio segun origen.
        int16_t travelEnd = boat->origin == SIDE_RIGHT ? CANAL_X + 2 : CANAL_X + CANAL_W - BOAT_SIZE - 2; // Fin segun origen.
        boatX = ship_display_map_progress(elapsed, boat->serviceMillis, travelStart, travelEnd);
        int16_t minX = CANAL_X + 2; // Limite izquierdo.
        int16_t maxX = CANAL_X + CANAL_W - BOAT_SIZE - 2; // Limite derecho.
        if (boatX < minX) boatX = minX; // Aplica limite izq.
        if (boatX > maxX) boatX = maxX; // Aplica limite der.
      }
      int16_t boatY = CANAL_Y + (CANAL_H / 2) - (BOAT_SIZE / 2); // Y centrado.
      draw_boat_square(boatX, boatY, boat, true); // Dibuja el barco activo.
    }

    ship_display_hw_fill_rect(CANAL_X + 2, INFO_Y - 1, CANAL_W - 4, INFO_H + 2, CANAL_BG); // Limpia banda de info.
    ship_display_hw_set_text_color(COLOR_WHITE, COLOR_BLACK); // Color de info.
    ship_display_hw_set_text_wrap(false); // Evita salto de linea.
    ship_display_hw_set_cursor(CANAL_X + 4, INFO_Y); // Posicion de info.
    while (activeBoat && activeBoat->emergencyParked) { // Omitir barcos retirados por emergencia.
      activeBoat = NULL;
      for (uint8_t i = 0; i < activeCount; i++) {
        const Boat *candidate = ship_scheduler_get_active_boat_at(scheduler, i);
        if (candidate && !candidate->emergencyParked) {
          activeBoat = candidate;
          break;
        }
      }
    }
    if (activeBoat) { // Muestra datos del primer activo visible.
      ship_display_hw_print_str(boatTypeShort(activeBoat->type)); // Tipo corto.
      ship_display_hw_print_char('#'); // Separador.
      ship_display_hw_print_uint(activeBoat->id); // ID del barco.
      ship_display_hw_print_char(' '); // Espacio.
      ship_display_hw_print_str(boatSideName(activeBoat->origin)); // Lado de origen.
      ship_display_hw_print_char(' '); // Espacio.
      ship_display_hw_print_char('+'); // Prefijo para conteo.
      ship_display_hw_print_uint(activeCount - 1); // Cantidad extra.
    }
    ship_display_hw_set_text_wrap(true); // Restaura ajuste de texto.
    return; // Ya termino.
  }

  unsigned long elapsed = ship_scheduler_get_active_elapsed_millis(scheduler); // Tiempo transcurrido.
  BoatRenderData boatData = ship_display_calculate_active_boat_position(activeBoat, elapsed, CANAL_X, CANAL_W, CANAL_Y, CANAL_H, BOAT_SIZE); // Posicion calculada.

  if (activeBoat) { // Si hay barco activo.
    if (activeBoat->emergencyParked) { // Si esta retirado temporalmente del canal.
      ship_display_hw_fill_rect(CANAL_X + 1, CANAL_Y + 1, CANAL_W - 2, CANAL_H - 2, CANAL_BG); // Limpia el canal.
      reset_prev_boat_rect_cache(); // Reinicia cache de posicion.
      ship_display_hw_fill_rect(CANAL_X + 2, INFO_Y - 1, CANAL_W - 4, INFO_H + 2, CANAL_BG); // Limpia info.
      return; // No dibuja barcos estacionados por emergencia.
    }
    unsigned long remaining = activeBoat->remainingMillis; // Tiempo restante mostrado en la banda de info.

    if (boatData.isNewBoat) { // Si cambia el barco activo.
      ship_display_hw_fill_rect(CANAL_X + 1, CANAL_Y + 1, CANAL_W - 2, CANAL_H - 2, CANAL_BG); // Limpia todo el canal.
      reset_prev_boat_rect_cache(); // Reinicia el cache de la posicion previa.
    } else if (gPrevBoatX >= 0 && gPrevBoatY >= 0) { // Si hay posicion previa valida.
      ship_display_hw_fill_rect(gPrevBoatX, gPrevBoatY, gPrevBoatW, gPrevBoatH, CANAL_BG); // Limpia la posicion anterior.
    }

    // Si el barco tiene currentSlot, dibujamos en la casilla; en caso contrario usamos el cálculo por tiempo.
    int16_t drawX = boatData.boatX;
    int16_t drawY = boatData.boatY;
    if (activeBoat->currentSlot >= 0 && scheduler->listLength > 0) {
      drawX = slot_index_to_x(scheduler, activeBoat->currentSlot);
      drawY = CANAL_Y + (CANAL_H / 2) - (BOAT_SIZE / 2);
    }
    draw_boat_square(drawX, drawY, activeBoat, true); // Dibuja el barco activo.

    gPrevBoatX = drawX; // Actualiza X previa con la posicion realmente dibujada.
    gPrevBoatY = drawY; // Actualiza Y previa con la posicion realmente dibujada.
    gPrevBoatW = BOAT_SIZE; // Actualiza ancho previo real.
    gPrevBoatH = BOAT_SIZE; // Actualiza alto previo real.

    ship_display_hw_fill_rect(CANAL_X + 2, INFO_Y - 1, CANAL_W - 4, INFO_H + 2, CANAL_BG); // Limpia banda de info.
    ship_display_hw_set_text_color(COLOR_WHITE, COLOR_BLACK); // Color de info.
    ship_display_hw_set_text_wrap(false); // Evita salto de linea.
    ship_display_hw_set_cursor(CANAL_X + 4, INFO_Y); // Posicion de info.
    ship_display_hw_print_str(boatTypeShort(activeBoat->type)); // Tipo corto.
    ship_display_hw_print_char('#'); // Separador.
    ship_display_hw_print_uint(activeBoat->id); // ID del barco.
    ship_display_hw_print_char(' '); // Espacio.
    ship_display_hw_print_str(boatSideName(activeBoat->origin)); // Lado de origen.
    ship_display_hw_print_char(' '); // Espacio.
    ship_display_hw_print_uint(remaining / 1000); // Segundos restantes.
    ship_display_hw_print_char('s'); // Sufijo segundos.
    ship_display_hw_set_text_wrap(true); // Restaura ajuste de texto.
  } else { // Si no hay barco activo.
    if (boatData.isNewBoat) { // Si se elimino el barco anterior.
      ship_display_hw_fill_rect(CANAL_X + 1, CANAL_Y + 1, CANAL_W - 2, CANAL_H - 2, CANAL_BG); // Limpia todo el canal.
      reset_prev_boat_rect_cache(); // Reinicia cache de posicion.
    }
    ship_display_hw_fill_rect(CANAL_X + 2, INFO_Y - 1, CANAL_W - 4, INFO_H + 2, CANAL_BG); // Limpia info.
  }
}

// Dibuja estadisticas de completados en el pie.
static void draw_statistics(const ShipScheduler *scheduler) {
  ship_display_hw_fill_rect(0, FOOTER_Y, SCREEN_W, FOOTER_H, SHIP_NAVY); // Fondo del pie.
  ship_display_hw_set_text_color(COLOR_GREEN, COLOR_BLACK); // Color del texto.
  ship_display_hw_set_cursor(4, FOOTER_Y + 3); // Posicion izq.
  ship_display_hw_print_str("I->D:"); // Etiqueta izq.
  ship_display_hw_print_uint(ship_scheduler_get_completed_left_to_right(scheduler)); // Contador izq.
  ship_display_hw_set_cursor((SCREEN_W / 2) + 30, FOOTER_Y + 3); // Posicion der.
  ship_display_hw_print_str("D->I:"); // Etiqueta der.
  ship_display_hw_print_uint(ship_scheduler_get_completed_right_to_left(scheduler)); // Contador der.
}

// Dibuja el estado de puertas (ABIERTO/CERRADO) en el encabezado.
static void draw_gate_status(const ShipScheduler *scheduler) {
  uint8_t leftGate = ship_scheduler_get_gate_left_state(scheduler); // 0=abierta, 2=cerrada.
  uint8_t rightGate = ship_scheduler_get_gate_right_state(scheduler); // 0=abierta, 2=cerrada.
  bool allOpen = (leftGate == 0 && rightGate == 0); // Verdadero si ambas abiertas.
  uint16_t color = allOpen ? COLOR_GREEN : COLOR_RED; // Verde si abierto, rojo si cerrado.

  ship_display_hw_set_text_color(color, COLOR_BLACK); // Color segun estado.
  ship_display_hw_set_cursor(50, 117); // Posicion en encabezado (derecha).
  ship_display_hw_set_text_size(1); // Tamano de fuente base.
  ship_display_hw_print_str(allOpen ? "ABIERTO" : "CERRADO"); // Texto de estado.
}

// API C: inicializa la pantalla y el estado interno del renderizado.
void ship_display_begin(void) {
  ship_display_hw_begin(); // Inicializa la parte fisica de la pantalla.

  if (!gDisplayMutex) { // Crea el mutex si aun no existe.
    gDisplayMutex = xSemaphoreCreateMutex(); // Mutex para dibujo concurrente.
    if (!gDisplayMutex) {
      ship_logln("[DISPLAY] No se pudo crear mutex de pantalla"); // Log de error.
    }
  }

  gLastUiRefresh = 0; // Fuerza primer redibujo.
}

void ship_display_acquire(uint32_t waitMs) {
  if (!gDisplayMutex) return;
  TickType_t ticks = pdMS_TO_TICKS(waitMs);
  xSemaphoreTake(gDisplayMutex, ticks);
}

void ship_display_release(void) {
  if (!gDisplayMutex) return;
  xSemaphoreGive(gDisplayMutex);
}

// API C: dibujo completo (adquiere mutex internamente).
void ship_display_render(const ShipScheduler *scheduler) {
  if (gDisplayMutex) { // Intenta tomar mutex con tiempo de espera.
    xSemaphoreTake(gDisplayMutex, portMAX_DELAY); // Espera hasta obtener la pantalla.
  }

  unsigned long now = millis(); // Reloj actual.
  if (now - gLastUiRefresh < UI_REFRESH_MS) { // Respeta limite de refresco.
    if (gDisplayMutex) xSemaphoreGive(gDisplayMutex);
    return;
  }
  gLastUiRefresh = now; // Actualiza marca de refresco.

  ShipAlgo currentAlgo = ship_scheduler_get_algorithm(scheduler); // Algoritmo actual.
  if (!gLayoutDrawn || currentAlgo != gLastAlgorithm) { // Si el diseno cambia.
    draw_static_layout(ship_scheduler_get_algorithm_label(scheduler)); // Dibuja diseno base.
    gLastAlgorithm = currentAlgo; // Guarda algoritmo mostrado.
  }

  draw_active_boat(scheduler); // Dibuja barco activo.
  draw_waiting_side(scheduler, SIDE_LEFT); // Dibuja cola izquierda.
  draw_waiting_side(scheduler, SIDE_RIGHT); // Dibuja cola derecha.
  
  draw_statistics(scheduler); // Dibuja estadisticas.
  draw_gate_status(scheduler); // Dibuja estado de puertas.
  if (gDisplayMutex) xSemaphoreGive(gDisplayMutex); // Libera mutex al terminar.
}

// API C: alias mantenido por compatibilidad.
void ship_display_render_if_needed(const ShipScheduler *scheduler) {
  ship_display_render(scheduler); // Delega al dibujo principal.
}

// API C: dibuja forzadamente sin respetar el limite de refresco.
void ship_display_render_forced(const ShipScheduler *scheduler) {
  gLastUiRefresh = 0; // Reinicia el limite de refresco.
  ship_display_render(scheduler); // Dibujo inmediato.
}

// Mapea progreso temporal a posicion en pixeles.
int16_t ship_display_map_progress(unsigned long elapsed, unsigned long total, int16_t from, int16_t to) {
  if (total == 0) return to; // Evita division por cero.
  long delta = (long)to - (long)from; // Distancia en pixeles.
  return from + (int16_t)((delta * (long)elapsed) / (long)total); // Mapeo lineal.
}

// Calcula la posicion y datos del barco activo para renderizado.
BoatRenderData ship_display_calculate_active_boat_position(const Boat *boat, unsigned long elapsed, int16_t canalX, int16_t canalW, int16_t canalY, int16_t canalH, int16_t boatSize) {
  BoatRenderData data = (BoatRenderData){0}; // Inicializa estructura.

  if (!boat) { // Si no hay barco.
    data.isNewBoat = (gPrevRenderedBoatId != 0); // Indica que habia barco antes.
    gPrevRenderedBoatId = 0; // Limpia cache de ID.
    return data;
  }

  int16_t travelStart = boat->origin == SIDE_RIGHT ? canalX + canalW - boatSize - 2 : canalX + 2; // Inicio segun origen.
  int16_t travelEnd = boat->origin == SIDE_RIGHT ? canalX + 2 : canalX + canalW - boatSize - 2; // Fin segun origen.
  int16_t boatX = ship_display_map_progress(elapsed, boat->serviceMillis, travelStart, travelEnd); // Posicion X.

  int16_t minX = canalX + 2; // Limite izquierdo.
  int16_t maxX = canalX + canalW - boatSize - 2; // Limite derecho.
  if (boatX < minX) boatX = minX; // Aplica limite izq.
  if (boatX > maxX) boatX = maxX; // Aplica limite der.

  int16_t boatY = canalY + (canalH / 2) - (boatSize / 2); // Calcula Y centrado.

  data.boatX = boatX; // X final.
  data.boatY = boatY; // Y final.
  data.boatWidth = boatSize; // Ancho del barco.
  data.boatHeight = boatSize; // Alto del barco.
  data.isNewBoat = (gPrevRenderedBoatId != boat->id); // Verdadero si cambia el barco.

  gPrevRenderedBoatId = boat->id; // Actualiza el ID anterior.
  return data;
}

// Retorna la cantidad visible de barcos en la cola (maximo 3).
uint8_t ship_display_get_visible_waiting_count(uint8_t totalWaiting) {
  return totalWaiting > 3 ? 3 : totalWaiting; // Limite fijo.
}

// Invalida los caches usados para renderizar barcos.
void ship_display_invalidate_boat_cache(void) {
  gPrevRenderedBoatId = 0; // Limpia cache de ID.
  reset_prev_boat_rect_cache(); // Limpia cache de posicion previa.
}
