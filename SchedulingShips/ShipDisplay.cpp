#include "ShipDisplay.h" // API publica y envoltorios de la parte fisica.
#include "ShipPins.h" // Mapa de pines para la TFT.

#include <SPI.h> // Controlador SPI para la TFT.
#include <Adafruit_GFX.h> // Libreria grafica base.
#include <Adafruit_ST7735.h> // Controlador TFT para ST7735.

// Instancia unica del controlador TFT para el envoltorio de la parte fisica.
static Adafruit_ST7735 gTft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);

// Inicializa SPI y el controlador TFT con la rotacion esperada.
extern "C" void ship_display_hw_begin(void) {
  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  pinMode(TFT_RST, OUTPUT);
  digitalWrite(TFT_RST, HIGH);
  delay(10);
  gTft.initR(INITR_BLACKTAB);
  gTft.setRotation(1);
  gTft.fillScreen(0x0000);
}

// Rellena toda la pantalla usando el controlador TFT.
extern "C" void ship_display_hw_fill_screen(uint16_t color) {
  gTft.fillScreen(color);
}

// Rellena un rectangulo usando el controlador TFT.
extern "C" void ship_display_hw_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  gTft.fillRect(x, y, w, h, color);
}

// Dibuja el contorno de un rectangulo usando el controlador TFT.
extern "C" void ship_display_hw_draw_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  gTft.drawRect(x, y, w, h, color);
}

// Configura el ajuste de linea del texto.
extern "C" void ship_display_hw_set_text_wrap(bool wrap) {
  gTft.setTextWrap(wrap);
}

// Configura el tamano del texto.
extern "C" void ship_display_hw_set_text_size(uint8_t size) {
  gTft.setTextSize(size);
}

// Configura el color del texto y su fondo.
extern "C" void ship_display_hw_set_text_color(uint16_t color, uint16_t bg) {
  gTft.setTextColor(color, bg);
}

// Configura la posicion del cursor para texto.
extern "C" void ship_display_hw_set_cursor(int16_t x, int16_t y) {
  gTft.setCursor(x, y);
}

// Imprime una cadena terminada en nulo usando el controlador TFT.
extern "C" void ship_display_hw_print_str(const char *text) {
  gTft.print(text);
}

// Imprime un caracter usando el controlador TFT.
extern "C" void ship_display_hw_print_char(char value) {
  gTft.print(value);
}

// Imprime un entero sin signo usando el controlador TFT.
extern "C" void ship_display_hw_print_uint(unsigned long value) {
  gTft.print(value);
}

