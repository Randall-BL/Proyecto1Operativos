# Display Simulator - Scheduling Ships

Interfaz gráfica en Python que se conecta por puerto serial al ESP32 y visualiza en tiempo real el estado del canal, barcos, puertas y emergencias.

## Instalación

1. Instala Python 3.7+
2. Instala las dependencias:
```bash
pip install -r requirements.txt
```

## Uso

1. **Conecta el ESP32** por USB a tu computadora
2. **Identifica el puerto** (en Windows: COM3, COM4, etc.; en Linux: /dev/ttyUSB0, etc.)
3. **Edita `display_simulator.py`** línea 176 y reemplaza `'COM3'` con tu puerto:
```python
app = SchedulingShipsDisplay(root, port='COM3', baudrate=115200)
```
4. **Ejecuta el simulador**:
```bash
python display_simulator.py
```

## Qué visualiza

- **Encabezado**: Algoritmo actual y estado de puertas (ABIERTO/CERRADO)
- **Panel Izquierdo (IZQ)**: Cola de espera lado izquierdo
- **Canal (Centro)**: Barco activo cruzando (si hay)
- **Panel Derecho (DER)**: Cola de espera lado derecho
- **Pie**: Barcos completados (I->D, D->I) y colisiones detectadas
- **Emergencia**: Banner rojo si se dispara proximidad
- **Sensor**: Distancia actual simulada

## Comandos para probar

### Test básico de interrupciones:

```
flowlog on
demo
sensor activate
sensor threshold 100
pause
```

Espera a que un barco empiece a cruzar (mira el display), luego:

```
resume
sensor simulate 80
```

Deberías ver:
- Panel encabezado: **CERRADO** (rojo)
- Banner: **¡EMERGENCIA!**
- Barco desaparece del canal
- Reaparece en la cola

Después de ~5 segundos:
- Panel: vuelve a **ABIERTO** (verde)
- Barcos pueden cruzar de nuevo

Para limpiar emergencia manualmente:
```
emergency clear
```

## Notas

- El puerto serial debe estar a **115200 baudios**
- Si no conecta, verifica que el ESP32 esté en el puerto correcto
- Los datos se leen en tiempo real desde el monitor serial
- La visualización parsea logs automáticamente
