#!/usr/bin/env bash
set -euo pipefail

# Script para generar y flashear littlefs en Ubuntu
# Uso: flash_littlefs.sh [-p PORT] [-d DATA_DIR] [-o OUTPUT] [-m MKLITTLEFS]

MKLITTLEFS_DEFAULT="/usr/x86_64-linux-gnu-mklittlefs-42acb97/mklittlefs/mklittlefs"
DATA_DIR_DEFAULT="SchedulingShips/data"
PAGE_SIZE=256
BLOCK_SIZE=4096
SIZE=0x160000
OUTPUT_DEFAULT="littlefs.bin"
PORT_DEFAULT="/dev/ttyACM0"

ensure_esptool_python() {
  if python3 -m esptool --help >/dev/null 2>&1; then
    echo "python3"
    return 0
  fi

  if command -v pip3 >/dev/null 2>&1; then
    echo "Instalando esptool con pip3 en el espacio de usuario..." >&2
    pip3 install --user --break-system-packages esptool >&2
    if python3 -m esptool --help >/dev/null 2>&1; then
      echo "python3"
      return 0
    fi
  fi

  echo "Error: no se pudo habilitar esptool. Instala python3-pip o ejecuta: python3 -m pip install --user --break-system-packages esptool" >&2
  exit 4
}

resolve_port() {
  local requested_port="$1"

  if [ -e "$requested_port" ]; then
    echo "$requested_port"
    return 0
  fi

  if [ -e "/dev/ttyACM0" ]; then
    echo "/dev/ttyACM0"
    return 0
  fi

  if [ -e "/dev/ttyUSB0" ]; then
    echo "/dev/ttyUSB0"
    return 0
  fi

  local candidate
  for candidate in /dev/ttyACM* /dev/ttyUSB*; do
    if [ -e "$candidate" ]; then
      echo "$candidate"
      return 0
    fi
  done

  echo "$requested_port"
}

usage(){
  cat <<EOF
Usage: $0 [-p PORT] [-d DATA_DIR] [-o OUTPUT] [-m MKLITTLEFS]

Defaults:
  PORT: $PORT_DEFAULT
  DATA_DIR: $DATA_DIR_DEFAULT
  OUTPUT: $OUTPUT_DEFAULT
  MKLITTLEFS: $MKLITTLEFS_DEFAULT

Example:
  $0 -p /dev/ttyACM0 -d SchedulingShips/data
EOF
  exit 1
}

PORT="$PORT_DEFAULT"
DATA_DIR="$DATA_DIR_DEFAULT"
OUTPUT="$OUTPUT_DEFAULT"
MKLITTLEFS="$MKLITTLEFS_DEFAULT"

while getopts ":p:d:o:m:h" opt; do
  case $opt in
    p) PORT="$OPTARG" ;;
    d) DATA_DIR="$OPTARG" ;;
    o) OUTPUT="$OPTARG" ;;
    m) MKLITTLEFS="$OPTARG" ;;
    h) usage ;;
    \?) echo "Invalid option: -$OPTARG" >&2; usage ;;
  esac
done

if [ ! -x "$MKLITTLEFS" ]; then
  echo "Error: mklittlefs no encontrado o no ejecutable en: $MKLITTLEFS" >&2
  exit 2
fi

if [ ! -d "$DATA_DIR" ]; then
  echo "Error: directorio de datos no encontrado: $DATA_DIR" >&2
  exit 3
fi

echo "Generando $OUTPUT desde $DATA_DIR usando $MKLITTLEFS..."
"$MKLITTLEFS" -c "$DATA_DIR" -p $PAGE_SIZE -b $BLOCK_SIZE -s $SIZE "$OUTPUT"

PORT="$(resolve_port "$PORT")"

echo "Generado $OUTPUT. Iniciando flasheo en el puerto $PORT..."
ESPTOOL_PYTHON="$(ensure_esptool_python)"
"$ESPTOOL_PYTHON" -m esptool --chip esp32c6 --port "$PORT" write-flash 0x290000 "$OUTPUT"

echo "Listo."
