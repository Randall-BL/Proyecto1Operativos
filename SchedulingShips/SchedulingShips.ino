// Programa principal que orquesta la simulacion en el ESP32. // Comentario general del archivo. 
#include <SPI.h> // Libreria SPI para la TFT. 
#include <Adafruit_GFX.h> // Libreria base de graficos. 
#include <Adafruit_ST7735.h> // Driver de la pantalla ST7735. 
#include <FS.h> // API base de sistema de archivos.
#include <SPIFFS.h> // SPIFFS para cargar archivos de configuracion.

#include "ShipPins.h" // Mapeo de pines para la TFT. 
#include "ShipModel.h" // Modelo de barcos en C. 
#include "ShipScheduler.h" // Scheduler en C. 
#include "ShipCommands.h" // Parser de comandos en C. 
#include "ShipDisplay.h" // API C de la pantalla. 

// Instancia global del scheduler en C. 
ShipScheduler shipScheduler = {}; // Estado del scheduler inicializado a cero. 

static void load_channel_config_from_spiffs(ShipScheduler *scheduler) { // Carga configuracion de canal desde archivo.
  if (!scheduler) return; // Valida scheduler.
  if (!SPIFFS.begin(true)) { // Monta SPIFFS.
    Serial.println("No se pudo montar SPIFFS; usando configuracion por defecto."); // Informa fallo.
    return; // Sale con defaults.
  }

  File cfg = SPIFFS.open("/channel_config.txt", "r"); // Abre archivo de configuracion.
  if (!cfg) { // Si no existe archivo.
    Serial.println("No existe /channel_config.txt; usando configuracion por defecto."); // Informa ausencia.
    return; // Sale con defaults.
  }

  uint16_t applied = 0; // Cuenta lineas aplicadas.
  while (cfg.available()) { // Lee linea por linea.
    String line = cfg.readStringUntil('\n'); // Lee una linea.
    line.trim(); // Elimina espacios y fin de linea.
    if (line.length() == 0) continue; // Omite lineas vacias.
    if (line.startsWith("#")) continue; // Omite comentarios.
    process_serial_command(scheduler, line.c_str()); // Reutiliza parser de comandos.
    applied++; // Cuenta comando aplicado.
  }

  cfg.close(); // Cierra archivo.
  Serial.print("Configuracion de canal cargada desde SPIFFS: "); // Log de resultado.
  Serial.print(applied); // Imprime cantidad de lineas aplicadas.
  Serial.println(" lineas."); // Cierra mensaje.
} // Fin de load_channel_config_from_spiffs.

void setup() { // Funcion de inicializacion Arduino. 
  // Inicializa Serial, pantalla y scheduler con un manifiesto de prueba. 
  Serial.begin(115200); // Configura velocidad del puerto serie. 
  uint32_t startLog = millis(); // Guarda el tiempo de inicio. 
  while (!Serial && (millis() - startLog < 3000)) { // Espera Serial por un maximo. 
  } // Fin del bucle de espera. 

  Serial.println("Sistema Scheduling Ships iniciado."); // Mensaje de arranque. 

  ship_display_begin(); // Inicializa la pantalla TFT. 
  ship_scheduler_begin(&shipScheduler); // Inicializa el scheduler. 
  load_channel_config_from_spiffs(&shipScheduler); // Carga parametros del canal si existen.
  ship_scheduler_load_demo_manifest(&shipScheduler); // Carga un manifiesto demo. 
  ship_display_render(&shipScheduler); // Render inicial de la pantalla. 

  Serial.println("Scheduler listo. Escribe 'help' para comandos."); // Mensaje de listo. 
} // Fin de setup. 

void loop() { // Bucle principal Arduino. 
  // En cada ciclo: lee comandos, actualiza scheduler y refresca la interfaz si aplica. 
  if (Serial.available() > 0) { // Si hay datos en Serial. 
    String command = Serial.readStringUntil('\n'); // Lee una linea completa. 
    command.replace("\r", ""); // Elimina retorno de carro. 
    command.trim(); // Normaliza espacios de extremos.
    if (command.equalsIgnoreCase("cfgload")) { // Permite recargar archivo de configuracion.
      load_channel_config_from_spiffs(&shipScheduler); // Recarga parametros desde SPIFFS.
    } else { // Cualquier otro comando.
      process_serial_command(&shipScheduler, command.c_str()); // Pasa el comando al parser en C.
    }
  } // Fin de lectura de comandos. 

  ship_scheduler_update(&shipScheduler); // Avanza el scheduler. 
  ship_display_render_if_needed(&shipScheduler); // Redibuja si toca por tiempo. 
} // Fin del loop. 