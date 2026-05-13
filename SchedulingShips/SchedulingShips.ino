// Programa principal que orquesta la simulacion en el ESP32. // Comentario general del archivo. 
#include <SPI.h> // Libreria SPI para la TFT. 
#include <Adafruit_GFX.h> // Libreria base de graficos. 
#include <Adafruit_ST7735.h> // Controlador de la pantalla ST7735. 
#include <FS.h> // API base de sistema de archivos.
#include <LittleFS.h> // LittleFS para cargar archivos de configuracion.

#include "ShipPins.h" // Mapeo de pines para la TFT. 
#include "ShipModel.h" // Modelo de barcos en C. 
#include "ShipScheduler.h" // Scheduler en C. 
#include "ShipCommands.h" // Parser de comandos en C. 
#include "ShipDisplay.h" // API C de la pantalla. 
#include "ShipIO.h" // Logging C con puente configurable.

// Instancia global del scheduler en C. 
ShipScheduler shipScheduler = {}; // Estado del scheduler inicializado a cero. 

// Estado global del sensor de proximidad fisico.
static uint8_t gProxTrigPin = PROX_TRIG_PIN; // Pin TRIG configurable.
static uint8_t gProxEchoPin = PROX_ECHO_PIN; // Pin ECHO configurable.
static unsigned long gProxPollIntervalMs = 120; // Periodo de sondeo configurable.
static unsigned long gProxLastPollAt = 0; // Ultimo instante de lectura.
static bool gProxPinsConfigured = false; // Evita reconfigurar pines en cada ciclo.

// Puentea logs en C hacia Serial sin salto de linea.
static void serial_write_bridge(const char *text) { // Puente C -> Serial sin salto.
  if (text) Serial.print(text);
}

// Puentea logs en C hacia Serial con salto de linea.
static void serial_writeln_bridge(const char *text) { // Puente C -> Serial con salto.
  if (text) {
    Serial.println(text);
  } else {
    Serial.println();
  }
}

// Configura los pines GPIO del sensor de proximidad.
static void configure_proximity_sensor_pins() { // Configura pines del sensor.
  pinMode(gProxTrigPin, OUTPUT); // TRIG como salida.
  pinMode(gProxEchoPin, INPUT); // ECHO como entrada.
  digitalWrite(gProxTrigPin, LOW); // Mantiene TRIG en bajo por defecto.
  gProxPinsConfigured = true; // Marca configuracion aplicada.
  Serial.print("[SENSOR] Pines configurados TRIG=");
  Serial.print(gProxTrigPin);
  Serial.print(" ECHO=");
  Serial.println(gProxEchoPin);
} // Fin de configure_proximity_sensor_pins.

// Mide distancia en cm usando el sensor ultrasonico.
static uint16_t measure_proximity_distance_cm() { // Mide distancia en cm con ultrasonido.
  digitalWrite(gProxTrigPin, LOW); // Estabiliza el pulso.
  delayMicroseconds(2);
  digitalWrite(gProxTrigPin, HIGH); // Pulso de disparo de 10us.
  delayMicroseconds(10);
  digitalWrite(gProxTrigPin, LOW);

  unsigned long durationUs = pulseIn(gProxEchoPin, HIGH, 30000UL); // Tiempo de espera ~30ms.
  if (durationUs == 0) return 999; // Sin eco: distancia fuera de rango.

  float distanceCm = (durationUs * 0.0343f) * 0.5f; // Convierte tiempo de vuelo a cm.
  if (distanceCm < 0.0f) distanceCm = 0.0f;
  if (distanceCm > 999.0f) distanceCm = 999.0f;
  return (uint16_t)(distanceCm + 0.5f); // Redondea al entero mas cercano.
} // Fin de measure_proximity_distance_cm.

// Consulta el sensor solo si esta activo y dentro del intervalo.
static void poll_proximity_sensor_if_needed(ShipScheduler *scheduler) { // Lee sensor en tiempo real si esta activo.
  if (!scheduler) return; // Valida scheduler.
  if (!ship_scheduler_get_sensor_enabled(scheduler)) return; // Solo cuando sensor esta activo.
  if (!gProxPinsConfigured) configure_proximity_sensor_pins(); // Configura una vez.

  unsigned long now = millis(); // Reloj actual.
  if (now - gProxLastPollAt < gProxPollIntervalMs) return; // Respeta periodo de sondeo.
  gProxLastPollAt = now; // Registra lectura.

  uint16_t measuredCm = measure_proximity_distance_cm(); // Mide distancia real.
  // Depuracion: imprime la medicion y el umbral para verificar por qué no actua la emergencia
  ship_scheduler_set_proximity_distance(scheduler, measuredCm); // Actualiza distancia en el scheduler.
} // Fin de poll_proximity_sensor_if_needed.

// Maneja comandos en tiempo de ejecucion que afectan el sensor fisico.
static bool process_runtime_sensor_command(String command) { // Procesa comandos de hardware del sensor.
  String normalized = command; // Copia local para parseo.
  normalized.trim(); // Limpia extremos.

  if (normalized.startsWith("proxpin ")) { // Configura pines TRIG/ECHO.
    int firstSpace = normalized.indexOf(' ');
    int secondSpace = normalized.indexOf(' ', firstSpace + 1);
    if (secondSpace <= firstSpace) {
      Serial.println("Uso: proxpin <trig> <echo>");
      return true;
    }

    String trigToken = normalized.substring(firstSpace + 1, secondSpace);
    String echoToken = normalized.substring(secondSpace + 1);
    trigToken.trim();
    echoToken.trim();
    int trig = trigToken.toInt();
    int echo = echoToken.toInt();
    if (trig < 0 || trig > 39 || echo < 0 || echo > 39 || trig == echo) {
      Serial.println("Error: pines invalidos. Usa GPIO 0..39 y diferentes.");
      return true;
    }

    gProxTrigPin = (uint8_t)trig;
    gProxEchoPin = (uint8_t)echo;
    gProxPinsConfigured = false; // Fuerza reconfiguracion en la siguiente lectura.
    configure_proximity_sensor_pins(); // Aplica de inmediato.
    return true;
  }

  if (normalized.startsWith("proxpollms ")) { // Configura periodo de sondeo.
    int firstSpace = normalized.indexOf(' ');
    String valueToken = normalized.substring(firstSpace + 1);
    valueToken.trim();
    unsigned long valueMs = (unsigned long)valueToken.toInt();
    if (valueMs < 50UL) valueMs = 50UL; // Evita sondeo excesivo.
    if (valueMs > 2000UL) valueMs = 2000UL; // Evita latencias muy altas.
    gProxPollIntervalMs = valueMs;
    Serial.print("[SENSOR] proxpollms=");
    Serial.println(gProxPollIntervalMs);
    return true;
  }

  return false; // No era comando de tiempo de ejecucion del sensor.
} // Fin de process_runtime_sensor_command.

// Carga la configuracion del canal desde SPIFFS si existe.
static void load_channel_config_from_spiffs(ShipScheduler *scheduler) { // Carga configuracion de canal desde archivo.
  if (!scheduler) return; // Valida scheduler.
  // Fuerza un estado base para que la configuracion aplicada coincida exactamente con el archivo.
  ship_scheduler_clear(scheduler); // Limpia ejecucion/colas actuales.
  ship_scheduler_demo_clear(scheduler); // Reinicia manifiesto de demo configurado.
  ship_model_set_step_size(BOAT_NORMAL, 1); // Default tipo normal.
  ship_model_set_step_size(BOAT_PESQUERA, 1); // Default tipo pesquera.
  ship_model_set_step_size(BOAT_PATRULLA, 2); // Default tipo patrulla.

  if (!LittleFS.begin(true)) { // Monta LittleFS.
    Serial.println("No se pudo montar LittleFS; usando configuracion por defecto."); // Informa fallo.
    return; // Sale con configuracion por defecto.
  }

  File cfg = LittleFS.open("/channel_config.txt", "r"); // Abre archivo de configuracion.
  if (!cfg) { // Si no existe archivo.
    Serial.println("No existe /channel_config.txt en LittleFS; usando configuracion por defecto."); // Informa ausencia.
    return; // Sale con configuracion por defecto.
  }

  uint16_t applied = 0; // Cuenta lineas aplicadas.
  while (cfg.available()) { // Lee linea por linea.
    String line = cfg.readStringUntil('\n'); // Lee una linea.
    line.trim(); // Elimina espacios y fin de linea.
    if (line.length() == 0) continue; // Omite lineas vacias.
    if (line.startsWith("#")) continue; // Omite comentarios.
    if (!process_runtime_sensor_command(line)) {
      process_serial_command(scheduler, line.c_str()); // Reutiliza parser de comandos.
    }
    applied++; // Cuenta comando aplicado.
  }

  cfg.close(); // Cierra archivo.
  Serial.print("Configuracion de canal cargada desde SPIFFS: "); // Log de resultado.
  Serial.print(applied); // Imprime cantidad de lineas aplicadas.
  Serial.println(" lineas."); // Cierra mensaje.
  Serial.print("Demo entries cargadas desde archivo: "); // Diagnostico de demo.
  Serial.println(scheduler->demoCount); // Cantidad de demoadd procesados.
  Serial.print("[CONFIG] Llamando ship_scheduler_load_demo_manifest con demoCount=");
  Serial.println(scheduler->demoCount);
  ship_scheduler_load_demo_manifest(scheduler); // Materializa la demo definida en el archivo.
  ship_scheduler_rebuild_slots(scheduler); // Reconstruye slots tras recrear la demo.
  Serial.print("[CONFIG] readyCount despues de load_demo_manifest=");
  Serial.println(scheduler->readyCount);
} // Fin de load_channel_config_from_spiffs.

// Inicializacion de Arduino: inicializa perifericos, scheduler y pantalla.
void setup() { // Funcion de inicializacion Arduino. 
  // Inicializa Serial, pantalla y scheduler con un manifiesto de prueba. 
  Serial.begin(115200); // Configura velocidad del puerto serie. 
  uint32_t startLog = millis(); // Guarda el tiempo de inicio. 
  while (!Serial && (millis() - startLog < 3000)) { // Espera Serial por un maximo. 
    delay(10); // Evita espera activa; cede CPU.
  } // Fin del bucle de espera. 

  Serial.println("Sistema Scheduling Ships iniciado."); // Mensaje de arranque. 

  ship_io_set_writers(serial_write_bridge, serial_writeln_bridge); // Conecta logs C a Serial.

  ship_display_begin(); // Inicializa la pantalla TFT. 
  ship_scheduler_begin(&shipScheduler); // Inicializa el scheduler. 
  configure_proximity_sensor_pins(); // Inicializa pines del sensor de proximidad.
  load_channel_config_from_spiffs(&shipScheduler); // Carga parametros del canal si existen.
  ship_display_render(&shipScheduler); // Dibujo inicial de la pantalla. 

  Serial.println("Scheduler listo. Escribe 'help' para comandos."); // Mensaje de listo. 
} // Fin de setup. 

// Bucle de Arduino: procesa comandos, sondea sensor y ejecuta ticks del scheduler.
void loop() { // Bucle principal Arduino. 
  // En cada ciclo: lee comandos, actualiza scheduler y refresca la interfaz si aplica. 
  // Parser no-bloqueante: acumulamos bytes hasta '\n'.
  static char cmdBuf[192];
  static size_t cmdLen = 0;
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\r') continue;

    if (c == '\n') {
      cmdBuf[cmdLen] = '\0';
      cmdLen = 0;

      String command = String(cmdBuf);
      command.trim();
      if (command.length() == 0) continue;

      if (command.equalsIgnoreCase("cfgload")) { // Permite recargar archivo de configuracion.
        load_channel_config_from_spiffs(&shipScheduler); // Recarga parametros desde SPIFFS.
        ship_display_render_forced(&shipScheduler); // Refresca pantalla con la nueva config.
      } else if (command.equalsIgnoreCase("demo")) { // Demo siempre sincronizada con archivo.
        load_channel_config_from_spiffs(&shipScheduler); // Relee config desde SPIFFS.
        ship_display_render_forced(&shipScheduler); // Refresca UI.
      } else if (process_runtime_sensor_command(command)) { // Procesa comandos de hardware de sensor.
        // Comando manejado en tiempo de ejecucion.
      } else {
        process_serial_command(&shipScheduler, command.c_str()); // Parser en C.
      }
    } else {
      if (cmdLen < sizeof(cmdBuf) - 1) {
        cmdBuf[cmdLen++] = c;
      } else {
        // Overflow: descartamos la línea para no corromper el buffer.
        cmdLen = 0;
      }
    }
  }

  poll_proximity_sensor_if_needed(&shipScheduler); // Lee sensor real y actualiza distancia/emergencia.
  ship_scheduler_update(&shipScheduler); // Avanza el scheduler.
  delay(0); // Cede CPU a otras tareas sin dormir un tiempo fijo.
} // Fin del loop. 