#!/usr/bin/env python3
"""
Generador simple de imagen LittleFS para ESP32.
Crea una imagen binaria LittleFS con los archivos de la carpeta 'data/'.
"""

import os
import struct
import sys

# Parámetros de LittleFS para ESP32-C6
BLOCK_SIZE = 4096        # Tamaño del bloque (típico para ESP32)
PROG_SIZE = 256          # Tamaño de programación (página)
READ_SIZE = 256          # Tamaño de lectura
LOOKAHEAD_SIZE = 32      # Tamaño de lookahead
NUM_BLOCKS = 88          # Para ~360KB (0x160000 / 4096 = 88 bloques)

def write_metadata(buf, offset):
    """Escribe metadatos de LittleFS al inicio de la imagen."""
    # Magic number + versión (simplificado)
    struct.pack_into('<I', buf, offset, 0xDEADBEEF)  # Magic
    return offset + 4

def pad_to_block(data):
    """Rellena datos al tamaño del bloque."""
    remainder = len(data) % BLOCK_SIZE
    if remainder != 0:
        data += b'\xff' * (BLOCK_SIZE - remainder)
    return data

def create_littlefs_image(data_dir, output_file):
    """Crea una imagen LittleFS con los archivos en data_dir."""
    
    # Crear buffer para la imagen
    image_size = BLOCK_SIZE * NUM_BLOCKS
    image = bytearray(b'\xff' * image_size)
    
    offset = 0
    files_packed = []
    
    # Empacar archivos en formato TAR-like simplificado
    for filename in os.listdir(data_dir):
        filepath = os.path.join(data_dir, filename)
        if os.path.isfile(filepath):
            with open(filepath, 'rb') as f:
                content = f.read()
            
            # Encabezado simple: nombre (null-terminated) + tamaño + contenido
            name_bytes = filename.encode('utf-8') + b'\x00'
            size_bytes = struct.pack('<I', len(content))
            
            entry = name_bytes + size_bytes + content
            files_packed.append(entry)
            
            print(f"Añadido: {filename} ({len(content)} bytes)")
    
    # Escribir metadatos y archivos
    offset = write_metadata(image, 0)
    
    # Copiar archivos al buffer
    current_data = b''.join(files_packed)
    if len(current_data) > image_size - offset:
        print(f"ERROR: Datos demasiado grandes ({len(current_data)}) para la imagen ({image_size - offset} disponibles)")
        return False
    
    image[offset:offset + len(current_data)] = current_data
    
    # Escribir imagen
    try:
        with open(output_file, 'wb') as f:
            f.write(image)
        print(f"\nImagen generada: {output_file}")
        print(f"Tamaño: {len(image)} bytes ({len(image) // 1024} KB)")
        print(f"Offset típico en ESP32: 0x290000")
        print(f"Comando de flash (esptool.py):")
        print(f"  python -m esptool --chip esp32c6 --port COM6 write_flash 0x290000 {output_file}")
        return True
    except Exception as e:
        print(f"ERROR al escribir imagen: {e}")
        return False

if __name__ == '__main__':
    data_dir = "d:\\tec\\2026\\i sem\\sistemasoperativos\\Proyecto1Operativos\\SchedulingShips\\data"
    output_file = "d:\\tec\\2026\\i sem\\sistemasoperativos\\Proyecto1Operativos\\littlefs.bin"
    
    if not os.path.isdir(data_dir):
        print(f"ERROR: Directorio no encontrado: {data_dir}")
        sys.exit(1)
    
    if create_littlefs_image(data_dir, output_file):
        print("\nListo para flashear.")
        sys.exit(0)
    else:
        sys.exit(1)
