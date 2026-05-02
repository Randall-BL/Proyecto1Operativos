#include "ShipDisplay.h" // Declara la API C de la pantalla. 
#include "ShipPins.h" // Define los pines de la TFT. 
#include "ShipIO.h" // Logging por serial (para mensajes de debug si hace falta).

#include <SPI.h> // Libreria SPI usada por la TFT. 
#include <Adafruit_GFX.h> // Base grafica de Adafruit. 
#include <Adafruit_ST7735.h> // Driver de la TFT ST7735. 
#include <stdint.h> // Tipos enteros de ancho fijo. 

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Colores y constantes de diseno de la interfaz. 
static const uint16_t SHIP_DARK_GREY = 0x7BEF; // Gris oscuro para bordes. 
static const uint16_t SHIP_NAVY = 0x000F; // Azul marino para encabezado. 
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

// Estado interno de la pantalla (simula un singleton). 
static Adafruit_ST7735 gTft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST); // Driver TFT global. 
static unsigned long gLastUiRefresh = 0; // Marca del ultimo refresco. 
static bool gLayoutDrawn = false; // Marca si el layout base ya se dibujo. 
static ShipAlgo gLastAlgorithm = ALG_FCFS; // Ultimo algoritmo mostrado. 

// Mutex para proteger accesos concurrentes a la pantalla
static SemaphoreHandle_t gDisplayMutex = NULL;

// Estado de dibujo para evitar parpadeo: trackear la posicion previa del barco activo
static int16_t gPrevBoatX = -1;
static int16_t gPrevBoatY = -1;
static int16_t gPrevBoatW = 0;
static int16_t gPrevBoatH = 0;
static uint8_t gPrevBoatId = 0;

// Mapea el progreso temporal a una posicion en pixeles. 
static int16_t mapProgress(unsigned long elapsed, unsigned long total, int16_t from, int16_t to) { // Funcion de mapeo lineal. 
  if (total == 0) { // Evita division por cero. 
    return to; // Devuelve el extremo final si no hay total. 
  } 

  long delta = (long)to - (long)from; // Distancia total en pixeles. 
  return from + (int16_t)((delta * (long)elapsed) / (long)total); // Mapea proporcionalmente. 
} 

// Dibuja el fondo estatico de la interfaz. 
static void drawStaticLayout(const char *algoLabel) { // Renderiza el layout base. 
  gTft.fillScreen(ST77XX_BLACK); // Limpia toda la pantalla. 

  gTft.fillRect(0, 0, SCREEN_W, HEADER_H, SHIP_NAVY); // Encabezado. 
  gTft.setTextWrap(true); // Activa el ajuste de texto. 
  gTft.setTextSize(1); // Tamano de fuente base. 
  gTft.setTextColor(ST77XX_WHITE, ST77XX_BLACK); // Color del texto. 
  gTft.setCursor(8, 4); // Posicion del titulo. 
  gTft.print("Scheduling Ships - "); // Titulo base. 
  gTft.print(algoLabel ? algoLabel : "?"); // Etiqueta del algoritmo. 

  gTft.fillRect(0, HEADER_H, SIDE_PANEL_W, CANAL_H, ST77XX_BLACK); // Panel izquierdo. 
  gTft.fillRect(CANAL_X, HEADER_H, CANAL_W, CANAL_H, 0x18E3); // Fondo del canal. 
  gTft.fillRect(SCREEN_W - SIDE_PANEL_W, HEADER_H, SIDE_PANEL_W, CANAL_H, ST77XX_BLACK); // Panel derecho. 

  gTft.drawRect(0, HEADER_H, SIDE_PANEL_W, CANAL_H, ST77XX_BLUE); // Borde izq. 
  gTft.drawRect(CANAL_X, HEADER_H, CANAL_W, CANAL_H, ST77XX_WHITE); // Borde canal. 
  gTft.drawRect(SCREEN_W - SIDE_PANEL_W, HEADER_H, SIDE_PANEL_W, CANAL_H, ST77XX_BLUE); // Borde der. 

  gTft.setTextColor(ST77XX_YELLOW, ST77XX_BLACK); // Color de etiquetas. 
  gTft.setCursor(PANEL_INSET_X, LABEL_Y); // Etiqueta izquierda. 
  gTft.print("IZQ"); // Texto izquierda. 
  gTft.setCursor(CANAL_X + 6, LABEL_Y); // Etiqueta canal. 
  gTft.print("CANAL"); // Texto canal. 
  gTft.setCursor(SCREEN_W - SIDE_PANEL_W + PANEL_INSET_X, LABEL_Y); // Etiqueta derecha. 
  gTft.print("DER"); // Texto derecha. 

  gLayoutDrawn = true; // Marca que el layout ya se dibujo. 
} 

// Dibuja un barco como cuadrado. 
static void drawBoatSquare(int16_t x, int16_t y, const Boat *boat, bool highlight) { // Render del barco. 
  if (!boat) return; // Evita acceso nulo. 
  uint16_t color = boatColor(boat->type); // Color por tipo. 
  gTft.fillRect(x, y, BOAT_SIZE, BOAT_SIZE, color); // Cuerpo del barco. 
  gTft.drawRect(x, y, BOAT_SIZE, BOAT_SIZE, highlight ? ST77XX_WHITE : SHIP_DARK_GREY); // Borde. 
  gTft.setTextColor(highlight ? ST77XX_WHITE : ST77XX_BLACK, color); // Texto invertido si esta activo. 
  gTft.setCursor(x + 3, y + 3); // Posicion del texto. 
  gTft.print(boatTypeShort(boat->type)); // Imprime letra de tipo. 
} 

// Dibuja los barcos en espera de un lado. 
static void drawWaitingSide(const ShipScheduler *scheduler, BoatSide side) { // Render de cola. 
  int16_t panelX = side == SIDE_LEFT ? PANEL_INSET_X : (SCREEN_W - SIDE_PANEL_W + PANEL_INSET_X); // X del panel. 
  gTft.fillRect(panelX - 1, WAIT_AREA_Y - 2, BOAT_SIZE + 4, CANAL_H - 18, ST77XX_BLACK); // Limpia area. 

  uint8_t waitingCount = ship_scheduler_get_waiting_count(scheduler, side); // Total en cola. 
  uint8_t visibleCount = waitingCount > 3 ? 3 : waitingCount; // Maximo visible. 

  for (uint8_t i = 0; i < visibleCount; i++) { // Itera los visibles. 
    const Boat *boat = ship_scheduler_get_waiting_boat(scheduler, side, i); // Barco en posicion i. 
    if (boat == NULL) { // Si no existe, salta. 
      continue; // Continua con el siguiente. 
    } 

    int16_t slotY = WAIT_AREA_Y + (WAIT_SLOT_SPACING * i); // Calcula Y del slot. 
    drawBoatSquare(panelX, slotY, boat, false); // Dibuja el barco en espera. 
  } 

  gTft.setTextColor(ST77XX_WHITE, ST77XX_BLACK); // Color del contador. 
  gTft.setCursor(panelX - 1, WAIT_COUNT_Y); // Posicion del contador. 
  if (waitingCount == 0) { // Si no hay barcos. 
    gTft.print("0"); // Muestra cero. 
  } else { // Si hay barcos. 
    gTft.print(waitingCount); // Muestra el numero. 
  } 
} 

// Dibuja el barco activo en el canal. 
static void drawActiveBoat(const ShipScheduler *scheduler) { // Render del activo. 
  // Solo limpiamos la banda de info; el resto lo hacemos por regiones para evitar parpadeo
  gTft.fillRect(CANAL_X + 2, INFO_Y - 1, CANAL_W - 4, INFO_H + 2, 0x18E3); // Limpia banda de info.

  gTft.setTextSize(1); // Fuente base. 
  gTft.setTextColor(ST77XX_WHITE, ST77XX_BLACK); // Color del texto. 
  gTft.setCursor(CANAL_X + 6, CANAL_Y + 3); // Posicion del texto. 
  gTft.print("Canal"); // Titulo del canal. 

  const Boat *activeBoat = ship_scheduler_get_active_boat(scheduler); // Obtiene el barco activo. 
  if (activeBoat != NULL) { // Si hay barco activo. 
    unsigned long elapsed = ship_scheduler_get_active_elapsed_millis(scheduler); // Progreso real del barco ya resuelto por el scheduler.
    unsigned long remaining = activeBoat->remainingMillis; // Tiempo restante mostrado en la banda de info. 

    int16_t travelStart = activeBoat->origin == SIDE_RIGHT ? CANAL_X + CANAL_W - BOAT_SIZE - 2 : CANAL_X + 2; // X inicial. 
    int16_t travelEnd = activeBoat->origin == SIDE_RIGHT ? CANAL_X + 2 : CANAL_X + CANAL_W - BOAT_SIZE - 2; // X final. 
    int16_t boatX = mapProgress(elapsed, activeBoat->serviceMillis, travelStart, travelEnd); // Mapea X. 
    int16_t minX = CANAL_X + 2; // Limite minimo. 
    int16_t maxX = CANAL_X + CANAL_W - BOAT_SIZE - 2; // Limite maximo. 
    if (boatX < minX) boatX = minX; // Corrige si es menor. 
    if (boatX > maxX) boatX = maxX; // Corrige si es mayor. 
    int16_t boatY = CANAL_Y + (CANAL_H / 2) - (BOAT_SIZE / 2); // Y centrado. 

    // Si es un barco distinto al anterior, borra la region completa del canal una vez
    if (gPrevBoatId != activeBoat->id) {
      gTft.fillRect(CANAL_X + 1, CANAL_Y + 1, CANAL_W - 2, CANAL_H - 2, 0x18E3); // Limpia canal completo al cambiar de barco.
    } else {
      // Borra solo la posicion anterior del barco para evitar redibujado completo
      if (gPrevBoatX >= 0 && gPrevBoatY >= 0) {
        gTft.fillRect(gPrevBoatX, gPrevBoatY, gPrevBoatW, gPrevBoatH, 0x18E3);
      }
    }

    drawBoatSquare(boatX, boatY, activeBoat, true); // Dibuja el barco activo. 

    // Actualiza estado previo
    gPrevBoatId = activeBoat->id;
    gPrevBoatX = boatX;
    gPrevBoatY = boatY;
    gPrevBoatW = BOAT_SIZE;
    gPrevBoatH = BOAT_SIZE;

    gTft.fillRect(CANAL_X + 2, INFO_Y - 1, CANAL_W - 4, INFO_H + 2, 0x18E3); // Limpia banda de info. 
    gTft.setTextColor(ST77XX_WHITE, ST77XX_BLACK); // Color de info. 
    gTft.setTextWrap(false); // Evita salto de linea. 
    gTft.setCursor(CANAL_X + 4, INFO_Y); // Posicion de info. 
    gTft.print(boatTypeShort(activeBoat->type)); // Tipo corto. 
    gTft.print('#'); // Separador. 
    gTft.print(activeBoat->id); // ID del barco. 
    gTft.print(' '); // Espacio. 
    gTft.print(boatSideName(activeBoat->origin)); // Lado de origen. 
    gTft.print(' '); // Espacio. 
    gTft.print(remaining / 1000); // Segundos restantes. 
    gTft.print('s'); // Sufijo segundos. 
    gTft.setTextWrap(true); // Restaura ajuste de texto. 
  } else { // Si no hay barco activo. 
    gTft.fillRect(CANAL_X + 2, INFO_Y - 1, CANAL_W - 4, INFO_H + 2, 0x18E3); // Limpia info. 
  } 
} 

// Dibuja estadisticas de completados. 
static void drawStatistics(const ShipScheduler *scheduler) { // Render del pie. 
  gTft.fillRect(0, FOOTER_Y, SCREEN_W, FOOTER_H, SHIP_NAVY); // Fondo del pie. 
  gTft.setTextColor(ST77XX_GREEN, ST77XX_BLACK); // Color del texto. 
  gTft.setCursor(4, FOOTER_Y + 3); // Posicion izq. 
  gTft.print("I->D:"); // Etiqueta izq. 
  gTft.print(ship_scheduler_get_completed_left_to_right(scheduler)); // Contador izq. 
  gTft.setCursor((SCREEN_W / 2) + 4, FOOTER_Y + 3); // Posicion der. 
  gTft.print("D->I:"); // Etiqueta der. 
  gTft.print(ship_scheduler_get_completed_right_to_left(scheduler)); // Contador der. 
} 

// Dibuja estado de puertas (Abierto/Cerrado) en encabezado.
static void drawGateStatus(const ShipScheduler *scheduler) {
  uint8_t leftGate = ship_scheduler_get_gate_left_state(scheduler); // 0=open, 2=closed.
  uint8_t rightGate = ship_scheduler_get_gate_right_state(scheduler);
  bool allOpen = (leftGate == 0 && rightGate == 0); // True si ambas abiertas.
  
  uint16_t color = allOpen ? ST77XX_GREEN : ST77XX_RED; // Verde si abierto, rojo si cerrado.
  gTft.setTextColor(color, ST77XX_BLACK);
  gTft.setCursor(100, 4); // Posicion en encabezado (derecha).
  gTft.setTextSize(1);
  gTft.print(allOpen ? "ABIERTO" : "CERRADO");
}


// API C: inicializa la pantalla. 
void ship_display_begin(void) { // Entrada publica de inicializacion. 
  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS); // Configura SPI. 
  pinMode(TFT_RST, OUTPUT); // Configura el pin de reset. 
  digitalWrite(TFT_RST, HIGH); // Libera reset. 
  delay(10); // Pequeña espera para estabilizar. 

  gTft.initR(INITR_BLACKTAB); // Inicializa el driver ST7735. 
  gTft.setRotation(1); // Rota para modo horizontal. 
  gTft.fillScreen(ST77XX_BLACK); // Limpia la pantalla. 

  // Crear mutex para acceso concurrente desde tareas
  if (gDisplayMutex == NULL) {
    gDisplayMutex = xSemaphoreCreateMutex();
    if (gDisplayMutex == NULL) {
      ship_logln("[DISPLAY] No se pudo crear mutex de pantalla");
    }
  }

  gLastUiRefresh = 0; // Fuerza primer redibujo.
} 

// API C: render completo (adquirir mutex internamente). 
void ship_display_render(const ShipScheduler *scheduler) { // Entrada publica de render. 
  // Intentamos tomar el mutex; si falla por timeout, abortamos el render
  if (gDisplayMutex) {
    if (xSemaphoreTake(gDisplayMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
      ship_logln("[DISPLAY] mutex ocupado, omitiendo render");
      return;
    }
  }

  unsigned long now = millis();
  if (now - gLastUiRefresh < UI_REFRESH_MS) {
    if (gDisplayMutex) xSemaphoreGive(gDisplayMutex);
    return;
  }
  gLastUiRefresh = now;

  ShipAlgo currentAlgo = ship_scheduler_get_algorithm(scheduler); // Algoritmo actual. 
  if (!gLayoutDrawn || currentAlgo != gLastAlgorithm) { // Si el layout cambia. 
    drawStaticLayout(ship_scheduler_get_algorithm_label(scheduler)); // Dibuja layout base. 
    gLastAlgorithm = currentAlgo; // Guarda el algoritmo mostrado. 
  } 

  drawActiveBoat(scheduler); // Dibuja el barco activo. 
  drawWaitingSide(scheduler, SIDE_LEFT); // Dibuja cola izquierda. 
  drawWaitingSide(scheduler, SIDE_RIGHT); // Dibuja cola derecha. 
  drawGateStatus(scheduler); // Dibuja estado de puertas (Abierto/Cerrado).
  drawStatistics(scheduler); // Dibuja estadisticas. 

  if (gDisplayMutex) xSemaphoreGive(gDisplayMutex);
} 

// API C: alias mantenido por compatibilidad; ahora delega a ship_display_render
void ship_display_render_if_needed(const ShipScheduler *scheduler) { // Entrada publica con limitador. 
  ship_display_render(scheduler);
} 
