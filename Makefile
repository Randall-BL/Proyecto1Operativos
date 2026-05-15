# ============================================================
#  Makefile — Proyecto1Operativos / SchedulingShips
#  Compatible con: GNU Make (Windows via MinGW/Git Bash/Scoop)
# ============================================================

PYTHON      := python
PIP         := $(PYTHON) -m pip
PORT        ?= COM6
BAUD        ?= 921600
DATA_DIR    := SchedulingShips/data
LFS_IMAGE   := littlefs.bin

# ── Ayuda ────────────────────────────────────────────────────
.PHONY: help
help:
	@echo.
	@echo  Uso: make [objetivo] [PORT=COMx]
	@echo.
	@echo  Objetivos disponibles:
	@echo    install      Instala dependencias Python (requirements.txt)
	@echo    check        Verifica que las dependencias esten instaladas
	@echo    ports        Lista puertos seriales disponibles
	@echo    simulator    Lanza el simulador grafico (display_simulator.py)
	@echo    littlefs     Genera la imagen LittleFS con make_littlefs.py
	@echo    flash        Flashea littlefs.bin al ESP32 en $(PORT)
	@echo    flash-ps1    Flashea usando el script PowerShell completo
	@echo    clean        Elimina la imagen LittleFS generada
	@echo    all          install + littlefs
	@echo.
	@echo  Ejemplo: make flash PORT=COM3
	@echo.

# ── Instalacion de dependencias ──────────────────────────────
.PHONY: install
install:
	@echo [*] Actualizando pip...
	$(PIP) install --upgrade pip
	@echo [*] Instalando dependencias desde requirements.txt...
	$(PIP) install -r requirements.txt
	@echo [OK] Dependencias instaladas.

# ── Verificacion ─────────────────────────────────────────────
.PHONY: check
check:
	@echo [*] Verificando dependencias...
	$(PIP) show pyserial  >nul 2>&1 && echo   [OK] pyserial   || echo   [!!] pyserial NO encontrado
	$(PIP) show esptool   >nul 2>&1 && echo   [OK] esptool    || echo   [!!] esptool  NO encontrado
	@echo [*] Version de Python:
	$(PYTHON) --version

# ── Puertos seriales ─────────────────────────────────────────
.PHONY: ports
ports:
	$(PYTHON) list_ports.py

# ── Simulador grafico ────────────────────────────────────────
.PHONY: simulator
simulator:
	$(PYTHON) display_simulator.py

# ── Generar imagen LittleFS ──────────────────────────────────
.PHONY: littlefs
littlefs:
	@echo [*] Generando imagen LittleFS desde $(DATA_DIR)...
	$(PYTHON) make_littlefs.py
	@echo [OK] Imagen generada: $(LFS_IMAGE)

# ── Flashear al ESP32 (via esptool) ──────────────────────────
.PHONY: flash
flash: $(LFS_IMAGE)
	@echo [*] Flasheando $(LFS_IMAGE) en $(PORT) a $(BAUD) baud...
	$(PYTHON) -m esptool --chip esp32c6 --port $(PORT) --baud $(BAUD) \
		write_flash 0x290000 $(LFS_IMAGE)
	@echo [OK] Flash completado.

# ── Flashear usando script PowerShell ────────────────────────
.PHONY: flash-ps1
flash-ps1:
	powershell -ExecutionPolicy Bypass -File flash_littlefs.ps1 -Port $(PORT)

# ── Limpieza ─────────────────────────────────────────────────
.PHONY: clean
clean:
	@echo [*] Eliminando imagen generada...
	if exist $(LFS_IMAGE) del /f /q $(LFS_IMAGE)
	@echo [OK] Limpieza completada.

# ── Todo ─────────────────────────────────────────────────────
.PHONY: all
all: install littlefs
