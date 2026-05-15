# Solución: Flashear channel_config.txt al SPIFFS del ESP32-C6

## Problema diagnosticado
El ESP32-C6 tiene un SPIFFS/LittleFS vacío. El archivo `/channel_config.txt` no existe en la placa.

## Solución: Usar la extensión de LittleFS de Arduino IDE en VSCode

### Opción 1: Usar extensión de LittleFS Uploader en Arduino IDE (Recomendado si tienes Arduino IDE)
1. Abre Arduino IDE 2.0
2. Selecciona: Tools → ESP32 LittleFS Data Upload
3. Espera a que flashee el archivo

### Opción 2: Generar imagen LittleFS y flashear con esptool

#### Paso 1: Descargar mklittlefs.exe
- Ve a: https://github.com/earlephilhower/mklittlefs/releases
- Descarga la versión más reciente para Windows (mklittlefs.exe)
- Guarda en una carpeta accesible, ej: `C:\Tools\mklittlefs.exe`

#### Paso 2: Generar la imagen
```powershell
cd d:\tec\2026\i sem\sistemasoperativos\Proyecto1Operativos\SchedulingShips

# Ejecutar mklittlefs (reemplaza la ruta si descargaste en otra ubicación)
& "C:\Users\YITAN\OneDrive\Escritorio\Proyecto1Operativos\mklittlefs\mklittlefs.exe" -c "data" -p 256 -b 4096 -s 0x160000 "littlefs.bin"
```
& "C:\Users\YITAN\OneDrive\Escritorio\Proyecto1Operativos\mklittlefs\mklittlefs.exe" -c "SchedulingShips\data" -p 256 -b 4096 -s 0x160000 "littlefs.bin"
Esto genera `littlefs.bin` con tamaño de 0x160000 (1.4 MB aprox).

#### Paso 3: Flashear con esptool
```powershell
python -m esptool --chip esp32c6 --port COM6 write_flash 0x290000 littlefs.bin
```
python -m esptool --chip esp32 --port COM5 write-flash 0x290000 littlefs.bin
### Opción 3: Usar script Python (si tienes installed las herramientas)
```powershell
python make_littlefs.py
```

Luego:
```powershell
python -m esptool --chip esp32c6 --port COM6 write_flash 0x290000 littlefs.bin
```

## Después de flashear:
1. Desconecta/reconecta el ESP32-C6 (o presiona RESET)
2. Abre Monitor Serial a 115200 baud
3. Deberías ver:
   ```
   [CONFIG] Cargado channel_config.txt desde SPIFFS
   Demo entries cargados: 2
   Barco agregado: #1 tipo=Normal origen=Izq
   Barco agregado: #2 tipo=Pesquera origen=Der
   ```

## Verificación
Si los logs muestran:
- `[CONFIG] No se encontro channel_config.txt` → El flasheo falló, repite Paso 3
- `Demo entries cargados: 2` → Éxito, ahora verás solo los 2 barcos definidos en el archivo

---

**Recomendación**: La forma más simple es descargar `mklittlefs.exe` del GitHub y seguir Opción 2. 
¿Necesitas ayuda con algún paso específico?
