#!/usr/bin/env python3
import serial.tools.list_ports

print("Puertos seriales disponibles:\n")
ports = serial.tools.list_ports.comports()
for port, desc, hwid in ports:
    print(f"  {port}: {desc}")
    
if not ports:
    print("  (ninguno encontrado)")
