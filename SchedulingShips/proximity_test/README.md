Proximity test (HC-SR04) for ESP32

Usage:
- Upload `proximity_test.ino` to your ESP32 (ESP32-C6 should work similarly).
- Open Serial monitor at 115200 baudios.
- Commands (send a line and press Enter):
  - `help` -> muestra comandos
  - `pins <trig> <echo>` -> establece pines GPIO para TRIG y ECHO
  - `poll <ms>` -> periodo de sondeo en ms (default 200)
  - `start` / `stop` -> inicia / detiene mediciones
  - `status` -> muestra pines, poll y estado

Wiring (HC-SR04 classic):
- VCC -> 5V (o 3.3V si tu módulo soporta)
- GND -> GND
- TRIG -> pin configurado (ej. GPIO 23)
- ECHO -> pin configurado (ej. GPIO 22) **NO conectar directo si sensor usa 5V**

Nivel lógico: divisor de voltaje para ECHO (si alimentas el sensor a 5V):
- Conectar ECHO a R1 (10k) hacia el pin del ESP
- Entre el pin del ESP y GND pones R2 (20k)
- Resultado aproximado: Vout = 5V * (R2 / (R1+R2)) = 5 * (20k / 30k) = ~3.33V

Ejemplo de valores recomendados:
- R1 = 10kΩ (serie)
- R2 = 20kΩ (a tierra)

Si tu módulo HC-SR04 tiene ECHO en 3.3V, puedes alimentar a 3.3V y conectar ECHO directo al ESP.

Notes:
- pulseIn timeout está en 30ms (aprox 5m) en el sketch; si tu objeto está muy lejos, verás "out of range".
- Para ESP32-C6 algunos GPIOs pueden tener restricciones; verifica que los pines elegidos sean válidos para uso digital.
