#pragma once // Evita inclusiones duplicadas. 

// Mapeo de pines de la TFT en el ESP32. 
#define TFT_MOSI 19 // Pin MOSI para SPI. 
#define TFT_SCLK 21 // Pin SCLK para SPI. 
#define TFT_CS 18 // Pin Chip Select. 
#define TFT_DC 4 // Pin Data/Command. 
#define TFT_RST 5 // Pin Reset. 

// Pines sugeridos para sensor de proximidad ultrasónico (HC-SR04 o similar).
#define PROX_TRIG_PIN 23 // Pin TRIG del sensor.
#define PROX_ECHO_PIN 22 // Pin ECHO del sensor (usar divisor de voltaje si el sensor opera a 5V).