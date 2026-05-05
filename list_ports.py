#!/usr/bin/env python3
"""Lista los puertos seriales disponibles para seleccion rapida."""

import serial.tools.list_ports

print("Puertos seriales disponibles:\n")  # Encabezado del listado.
ports = serial.tools.list_ports.comports()
for port, desc, hwid in ports:
    print(f"  {port}: {desc}")  # Muestra puerto y descripcion.
    
if not ports:
    print("  (ninguno encontrado)")
