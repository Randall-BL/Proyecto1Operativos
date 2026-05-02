#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Display Simulator para Scheduling Ships
Conecta por serial al ESP32 y visualiza el estado del canal en tiempo real
Con interfaz de comandos como Arduino IDE
"""

import tkinter as tk
from tkinter import ttk, scrolledtext
import serial
import threading
import time
from collections import deque
from queue import Queue, Empty
import re
import os

class SchedulingShipsDisplay:
    def __init__(self, root, port='COM6', baudrate=115200):
        self.root = root
        self.root.title("Scheduling Ships - Display + Serial Monitor")
        self.root.geometry("1200x700")
        self.root.configure(bg='black')
        
        self.serial_port = port
        self.baudrate = baudrate
        self.ser = None
        self.running = True
        self.serial_queue = Queue()
        self.last_update_ts = time.time()
        self.app_start_ts = time.time()
        self.last_serial_log = None

        # Display físico: 128x160 (ancho x alto)
        self.tft_width = 128
        self.tft_height = 160
        self.scale = 3

        # Estado de cruce para animación
        self.crossing = {
            'boat_id': None,
            'boat_type': None,
            'boat_origin': None,
            'boat_algorithm': None,
            'direction': 'LR',
            'start_ts': 0.0,
            'duration_s': 6.5,
            'paused': False,
            'pause_progress': 0.0,
            'active': False,
        }

        # Tiempos por tipo cargados directamente desde ShipModel.c (sin valores por defecto aquí)
        self.service_time_by_type_ms = {}

        # Intenta cargar los tiempos desde el fichero C para mantener sincronía
        try:
            self.load_service_times_from_c()
        except Exception:
            # Si falla, mantenemos los valores por defecto
            pass

        # No cargamos el paso de movimiento; la animación será continua basada en duraciones del .c
        # Cache de tipo por id para enlazar "Start -> barco #N" con el tipo real
        self.boat_type_by_id = {}

        # Cache de lado de origen por id para respetar el spawn real
        self.boat_origin_by_id = {}

        # Cache de algoritmo por id para mostrar el algoritmo que tenia cada barco al entrar a cola
        self.boat_algorithm_by_id = {}

        # Colas visuales separadas por lado
        self.queue_left = []
        self.queue_right = []
        
        # Estado del scheduler
        self.state = {
            'active_boat': None,
            'ready_count': 0,
            'boats_left': [],
            'boats_right': [],
            'completed_lr': 0,
            'completed_rl': 0,
            'completed_total': 0,
            'algorithm': 'FCFS',
            'gate_status': 'ABIERTO',
            'emergency_mode': 'NONE',
            'sensor_enabled': False,
            'proximity_distance': 999,
            'collision_count': 0,
        }
        
        # Main frame con dos columnas
        main_frame = ttk.Frame(root)
        main_frame.pack(fill=tk.BOTH, expand=True, padx=10, pady=10)
        
        # ===== COLUMNA IZQUIERDA: DISPLAY =====
        display_frame = ttk.LabelFrame(main_frame, text="Display TFT (128x160)")
        display_frame.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=5)
        
        # Canvas para el display (escalado 3x para mejor visualización)
        self.canvas = tk.Canvas(
            display_frame,
            bg='black',
            width=self.tft_width * self.scale,
            height=self.tft_height * self.scale,
            bd=2,
            relief=tk.SUNKEN,
        )
        self.canvas.pack(padx=5, pady=5)
        
        # Etiqueta de estado
        self.status_label = ttk.Label(display_frame, text="Conectando...", foreground="yellow")
        self.status_label.pack(pady=5)

        # Cronometro de ejecucion (fuente agrandada 4x, texto negro)
        self.timer_label = ttk.Label(display_frame, text="Tiempo: 00:00:00", foreground="black", font=("Courier", 36, "bold"))
        self.timer_label.pack(pady=(0, 5))
        
        # ===== COLUMNA DERECHA: SERIAL MONITOR + COMANDOS =====
        serial_frame = ttk.LabelFrame(main_frame, text="Serial Monitor & Comandos")
        serial_frame.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=5)
        
        # Serial monitor (scrolledtext)
        ttk.Label(serial_frame, text="Salida Serial:", font=("Arial", 9, "bold")).pack(anchor='w', padx=5, pady=(5, 0))
        self.serial_output = scrolledtext.ScrolledText(
            serial_frame, 
            height=20, 
            width=50, 
            bg='black', 
            fg='white',
            font=("Courier", 9),
            wrap=tk.WORD
        )
        self.serial_output.pack(fill=tk.BOTH, expand=True, padx=5, pady=5)
        
        # Frame de comandos
        cmd_frame = ttk.LabelFrame(serial_frame, text="Enviar Comando")
        cmd_frame.pack(fill=tk.X, padx=5, pady=5)
        
        self.cmd_entry = ttk.Entry(cmd_frame, font=("Courier", 10))
        self.cmd_entry.pack(fill=tk.X, padx=5, pady=5)
        self.cmd_entry.bind("<Return>", lambda e: self.send_command())
        
        ttk.Button(cmd_frame, text="Enviar (Enter)", command=self.send_command).pack(fill=tk.X, padx=5, pady=5)
        
        # Ejemplos de comandos
        examples = ttk.LabelFrame(serial_frame, text="Ejemplos de Comandos")
        examples_text = ttk.Label(
            examples, 
            text="sensor activate\nsensor threshold 100\nsensor simulate 80\nemergency clear\nflowlog on\ndemo",
            justify=tk.LEFT,
            font=("Courier", 8),
            foreground="gray"
        )
        examples_text.pack(anchor='w', padx=5, pady=5)
        examples.pack(fill=tk.X, padx=5, pady=5)
        
        # Inicia conexión serial en thread
        self.serial_thread = threading.Thread(target=self.serial_loop, daemon=True)
        self.serial_thread.start()
        
        # Redraw loop
        self.redraw()
    
    def log_serial(self, message):
        """Agrega un mensaje al monitor serial"""
        if message == self.last_serial_log:
            return
        self.last_serial_log = message
        self.serial_output.insert(tk.END, message + '\n')
        self.serial_output.see(tk.END)  # Auto-scroll
    
    def connect_serial(self):
        """Intenta conectar al puerto serial"""
        try:
            self.ser = serial.Serial(self.serial_port, self.baudrate, timeout=1)
            time.sleep(2)  # Espera a que el ESP32 se reinicie
            msg = f"✓ Conectado a {self.serial_port} @ {self.baudrate} baud"
            self.serial_queue.put(("status", msg, "lightgreen"))
            self.serial_queue.put(("log", msg))
            return True
        except Exception as e:
            msg = f"✗ Error al conectar: {e}"
            self.serial_queue.put(("status", msg, "red"))
            self.serial_queue.put(("log", msg))
            return False
    
    def serial_loop(self):
        """Lee datos del puerto serial y parsea el estado"""
        if not self.connect_serial():
            self.serial_queue.put(("log", "No se pudo conectar al puerto serial. Verifica que esté disponible."))
            return
        
        buffer = deque(maxlen=500)
        
        while self.running:
            try:
                if self.ser and self.ser.in_waiting:
                    line = self.ser.readline().decode('utf-8', errors='ignore').strip()
                    if line:
                        buffer.append(line)
                        self.serial_queue.put(("log", line))
                        self.serial_queue.put(("parse", line))
                time.sleep(0.01)
            except Exception as e:
                self.serial_queue.put(("log", f"Serial error: {e}"))
                time.sleep(1)
    
    def send_command(self):
        """Envía comando al ESP32 por puerto serial"""
        cmd = self.cmd_entry.get().strip()
        self.cmd_entry.delete(0, tk.END)
        
        if not cmd:
            return
        
        if not self.ser or not self.ser.is_open:
            self.log_serial("✗ Error: Puerto serial no conectado")
            return
        
        try:
            # Agrega salto de línea y envía
            self.ser.write((cmd + '\n').encode('utf-8'))
            self.log_serial(f"> {cmd}")
        except Exception as e:
            self.log_serial(f"✗ Error al enviar: {e}")

    def process_serial_events(self):
        """Procesa eventos de serial en el hilo de UI"""
        while True:
            try:
                event = self.serial_queue.get_nowait()
            except Empty:
                break

            etype = event[0]
            if etype == "log":
                self.log_serial(event[1])
            elif etype == "parse":
                self.parse_line(event[1])
            elif etype == "status":
                _, text, color = event
                self.status_label.config(text=text, foreground=color)

    def detect_direction(self, line):
        lower = line.lower()
        if "der->izq" in lower or "r->l" in lower or "right" in lower:
            return 'RL'
        if "izq->der" in lower or "l->r" in lower or "left" in lower:
            return 'LR'
        return 'LR'

    def normalize_boat_type(self, raw_type):
        if not raw_type:
            return None
        value = raw_type.strip().lower()
        # Acepta variantes comunes que puedan salir por log
        aliases = {
            'normal': 'normal',
            'pesquera': 'pesquera',
            'pesquero': 'pesquera',
            'patrulla': 'patrulla',
        }
        return aliases.get(value, value)

    def normalize_algorithm(self, raw_algorithm):
        if not raw_algorithm:
            return 'FCFS'
        value = raw_algorithm.strip().lower()
        aliases = {
            'fcfs': 'FCFS',
            'sjf': 'SJF',
            'strn': 'STRN',
            'edf': 'EDF',
            'rr': 'RR',
            'prio': 'PRIO',
            'priority': 'PRIO',
        }
        return aliases.get(value, raw_algorithm.strip().upper())

    def algorithm_short(self, algorithm_name):
        normalized = self.normalize_algorithm(algorithm_name)
        return {
            'FCFS': 'F',
            'SJF': 'S',
            'STRN': 'T',
            'EDF': 'E',
            'RR': 'R',
            'PRIO': 'P',
        }.get(normalized, normalized[:1])

    def normalize_boat_origin(self, raw_origin):
        if not raw_origin:
            return None
        value = raw_origin.strip().lower()
        aliases = {
            'izq': 'left',
            'izquierda': 'left',
            'left': 'left',
            'der': 'right',
            'derecha': 'right',
            'right': 'right',
        }
        return aliases.get(value, value)

    def load_service_times_from_c(self):
        """Lee SchedulingShips/ShipModel.c y extrae los valores de serviceTimeForType.
        Si no se encuentran, no lanza excepción y deja los valores por defecto.
        """
        # Ruta relativa al script/raíz del repo
        base_dir = os.path.dirname(os.path.abspath(__file__))
        candidate = os.path.join(base_dir, 'SchedulingShips', 'ShipModel.c')
        if not os.path.exists(candidate):
            # intenta ruta un nivel arriba por si el script se ejecuta desde otro cwd
            candidate = os.path.join(base_dir, '..', 'SchedulingShips', 'ShipModel.c')
            candidate = os.path.normpath(candidate)
            if not os.path.exists(candidate):
                self.serial_queue.put(("log", f"Aviso: ShipModel.c no encontrado en {candidate}. Usando valores por defecto."))
                return

        try:
            with open(candidate, 'r', encoding='utf-8') as f:
                src = f.read()

            # Busca patrones de retorno para cada case
            patterns = {
                'normal': r'case\s+BOAT_NORMAL\s*:\s*return\s+(\d+)\s*;',
                'pesquera': r'case\s+BOAT_PESQUERA\s*:\s*return\s+(\d+)\s*;',
                'patrulla': r'case\s+BOAT_PATRULLA\s*:\s*return\s+(\d+)\s*;',
            }
            found = {}
            for k, pat in patterns.items():
                m = re.search(pat, src)
                if m:
                    found[k] = int(m.group(1))

            if found:
                # Actualiza solamente las entradas encontradas
                for k, v in found.items():
                    self.service_time_by_type_ms[k] = v
                self.serial_queue.put(("log", f"Tiempos de servicio cargados desde ShipModel.c: {found}"))
            else:
                self.serial_queue.put(("log", "No se encontraron tiempos en ShipModel.c; usando valores por defecto."))

        except Exception as e:
            self.serial_queue.put(("log", f"Error leyendo ShipModel.c: {e}. Usando valores por defecto."))

    

    def queue_for_origin(self, boat_origin):
        return self.queue_right if boat_origin == 'right' else self.queue_left

    def duration_for_boat_type(self, boat_type):
        if not boat_type:
            boat_type = 'normal'

        duration_ms = self.service_time_by_type_ms.get(boat_type)
        if duration_ms is None:
            # No se cargó desde ShipModel.c: informar y usar fallback seguro (1000ms)
            try:
                self.serial_queue.put(("log", f"Aviso: tiempo para tipo '{boat_type}' no cargado desde ShipModel.c; usando fallback 1000ms"))
            except Exception:
                pass
            duration_ms = 1000
        return duration_ms / 1000.0

    def origin_to_direction(self, boat_origin):
        if boat_origin == 'right':
            return 'RL'
        return 'LR'

    def add_boat_to_queue(self, boat_id, boat_origin, boat_type=None, boat_algorithm=None, front=False):
        boat_origin = self.normalize_boat_origin(boat_origin) or 'left'
        boat_algorithm = self.normalize_algorithm(boat_algorithm or self.state.get('algorithm', 'FCFS'))
        meta = self.boat_algorithm_by_id.get(boat_id)
        if meta is None:
            meta = {
                'id': boat_id,
                'type': boat_type,
                'origin': boat_origin,
                'algorithm': boat_algorithm,
            }
            self.boat_algorithm_by_id[boat_id] = meta
        else:
            meta['type'] = boat_type or meta.get('type')
            meta['origin'] = boat_origin
            meta['algorithm'] = boat_algorithm

        queue = self.queue_for_origin(boat_origin)
        if boat_id in queue:
            queue.remove(boat_id)
        if front:
            queue.insert(0, boat_id)
        else:
            queue.append(boat_id)

    def remove_boat_from_queues(self, boat_id):
        if boat_id in self.queue_left:
            self.queue_left.remove(boat_id)
        if boat_id in self.queue_right:
            self.queue_right.remove(boat_id)

    def current_boat_label(self, boat_id):
        meta = self.boat_algorithm_by_id.get(boat_id, {})
        boat_type = meta.get('type') or '?'
        algo = meta.get('algorithm') or self.state.get('algorithm', 'FCFS')
        return f"#{boat_id} {self.boat_type_short(boat_type)} {self.algorithm_short(algo)}"

    def boat_type_short(self, boat_type):
        normalized = self.normalize_boat_type(boat_type)
        return {
            'normal': 'N',
            'pesquera': 'P',
            'patrulla': 'A',
        }.get(normalized, '?')

    def boat_fill_color(self, boat_type):
        normalized = self.normalize_boat_type(boat_type)
        return {
            'normal': '#ffd166',
            'pesquera': '#06d6a0',
            'patrulla': '#ef476f',
        }.get(normalized, '#9aa0a6')
    
    def parse_line(self, line):
        """Parsea una línea del serial y actualiza estado"""
        try:
            # Cachea tipo al momento de alta: "Barco agregado: #7 tipo=Pesquera origen=..."
            if "Barco agregado:" in line and "tipo=" in line:
                match = re.search(r'#(\d+).*tipo=([^\s]+)', line)
                if match:
                    boat_id = int(match.group(1))
                    boat_type = self.normalize_boat_type(match.group(2))
                    self.boat_type_by_id[boat_id] = boat_type

                origin_match = re.search(r'origen=([^\s]+)', line)
                if origin_match and match:
                    boat_id = int(match.group(1))
                    boat_origin = self.normalize_boat_origin(origin_match.group(1))
                    self.boat_origin_by_id[boat_id] = boat_origin
                    self.add_boat_to_queue(boat_id, boat_origin, boat_type=self.boat_type_by_id.get(boat_id))

            if line.startswith("Algoritmo:"):
                algo_value = line.split(":", 1)[1].strip().split()[0]
                self.state['algorithm'] = self.normalize_algorithm(algo_value)

            # Detect active boat
            if "Start ->" in line and "barco #" in line:
                match = re.search(r'barco #(\d+)', line)
                if match:
                    boat_id = int(match.group(1))
                    boat_type = self.boat_type_by_id.get(boat_id)
                    boat_origin = self.boat_origin_by_id.get(boat_id)
                    boat_algorithm = self.boat_algorithm_by_id.get(boat_id, {}).get('algorithm', self.state.get('algorithm', 'FCFS'))
                    self.state['active_boat'] = boat_id
                    self.crossing['boat_id'] = boat_id
                    self.crossing['boat_type'] = boat_type
                    self.crossing['boat_origin'] = boat_origin
                    self.crossing['boat_algorithm'] = self.normalize_algorithm(boat_algorithm)
                    self.crossing['direction'] = self.origin_to_direction(boat_origin)
                    self.crossing['start_ts'] = time.time()
                    self.crossing['duration_s'] = self.duration_for_boat_type(boat_type)
                    self.crossing['paused'] = False
                    self.crossing['pause_progress'] = 0.0
                    self.crossing['active'] = True
                    self.remove_boat_from_queues(boat_id)
            
            # Detect boat completion
            if "Barco finalizado" in line and "#" in line:
                match = re.search(r'#(\d+).*origen=([LR\w]+)', line)
                if match:
                    boat_id = int(match.group(1))
                    # Refresca tipo por si viene en log de finalización
                    type_match = re.search(r'tipo=([^\s]+)', line)
                    if type_match:
                        self.boat_type_by_id[boat_id] = self.normalize_boat_type(type_match.group(1))

                    origin_match = re.search(r'origen=([^\s]+)', line)
                    if origin_match:
                        self.boat_origin_by_id[boat_id] = self.normalize_boat_origin(origin_match.group(1))

                    self.state['active_boat'] = None
                    self.crossing['active'] = False
                    self.crossing['boat_type'] = None
                    self.crossing['boat_origin'] = None
                    self.crossing['boat_algorithm'] = None
                    self.crossing['paused'] = False
                    self.crossing['pause_progress'] = 0.0
                    if 'izq' in line or 'Left' in line:
                        self.state['completed_lr'] += 1
                    else:
                        self.state['completed_rl'] += 1
                    self.state['completed_total'] += 1

            if "Pausado barco #" in line or "congelado en el canal" in line:
                match = re.search(r'#(\d+)', line)
                if match and self.crossing['boat_id'] == int(match.group(1)):
                    self.crossing['paused'] = True
                    elapsed = time.time() - self.crossing['start_ts']
                    self.crossing['pause_progress'] = max(0.0, min(1.0, elapsed / self.crossing['duration_s']))

            if "Reanudado barco #" in line:
                match = re.search(r'#(\d+)', line)
                if match and self.crossing['boat_id'] == int(match.group(1)):
                    self.crossing['paused'] = False
                    self.crossing['start_ts'] = time.time() - (self.crossing['pause_progress'] * self.crossing['duration_s'])

            if "recolocado en cola" in line:
                match = re.search(r'Barco #(\d+)', line)
                if match:
                    boat_id = int(match.group(1))
                    boat_origin = self.boat_origin_by_id.get(boat_id, 'left')
                    boat_type = self.boat_type_by_id.get(boat_id)
                    boat_algorithm = self.state.get('algorithm', 'FCFS')
                    self.add_boat_to_queue(boat_id, boat_origin, boat_type=boat_type, boat_algorithm=boat_algorithm)
                    if self.crossing['boat_id'] == boat_id:
                        self.state['active_boat'] = None
                        self.crossing['active'] = False
                        self.crossing['boat_type'] = None
                        self.crossing['boat_origin'] = None
                        self.crossing['boat_algorithm'] = None

            if "[EMERGENCY] Barco #" in line and "reencola" in line:
                match = re.search(r'Barco #(\d+)', line)
                if match:
                    boat_id = int(match.group(1))
                    boat_origin = self.boat_origin_by_id.get(boat_id, 'left')
                    boat_type = self.boat_type_by_id.get(boat_id)
                    boat_algorithm = self.state.get('algorithm', 'FCFS')
                    self.add_boat_to_queue(boat_id, boat_origin, boat_type=boat_type, boat_algorithm=boat_algorithm)
                    if self.state.get('active_boat') == boat_id:
                        self.state['active_boat'] = None
                        self.crossing['active'] = False
                        self.crossing['boat_type'] = None
                        self.crossing['boat_origin'] = None
                        self.crossing['boat_algorithm'] = None

            # NOTE: '[FLOW][SAFE] Requeue' contains scheduler-internal hints.
            # The display must not implement scheduling decisions; ignore these lines
            # so the simulator only mirrors explicit enqueue/requeue logs emitted by firmware.
            if "[FLOW][SAFE] Requeue" in line:
                # Intentionally ignored by display-only simulator
                pass
            
            # Detect ready count
            if "Ready count:" in line:
                match = re.search(r'Ready count: (\d+)', line)
                if match:
                    self.state['ready_count'] = int(match.group(1))
            
            # Detect gate status
            if "CERRADO" in line or "CLOSED" in line:
                self.state['gate_status'] = 'CERRADO'
            if "ABIERTO" in line or "OPEN" in line:
                self.state['gate_status'] = 'ABIERTO'
            
            # Detect emergency
            if "ALERTA DE PROXIMIDAD" in line or "PROXIMITY_ALERT" in line:
                self.state['emergency_mode'] = 'ALERTA'
            if "GATES_CLOSED" in line:
                self.state['emergency_mode'] = 'CERRADO'
            if "Limpiando estado de emergencia" in line or "emergency clear" in line:
                self.state['emergency_mode'] = 'NONE'
            
            # Sensor
            if "Sensor ACTIVADO" in line or "activated" in line:
                self.state['sensor_enabled'] = True
            if "Sensor DESACTIVADO" in line or "deactivated" in line:
                self.state['sensor_enabled'] = False
            
            # Proximidad
            if "distancia:" in line:
                match = re.search(r'distancia: (\d+)', line)
                if match:
                    self.state['proximity_distance'] = int(match.group(1))
            
            # Colisiones
            if "Colision evitada" in line:
                self.state['collision_count'] += 1
                
        except Exception as e:
            pass
    
    def redraw(self):
        """Redibuja el canvas con escala basada en 128x160"""
        self.process_serial_events()
        elapsed = int(time.time() - self.app_start_ts)
        hours = elapsed // 3600
        minutes = (elapsed % 3600) // 60
        seconds = elapsed % 60
        self.timer_label.config(text=f"Tiempo: {hours:02d}:{minutes:02d}:{seconds:02d}")
        self.canvas.delete("all")

        s = self.scale
        w = self.tft_width
        h = self.tft_height

        def sx(x):
            return int(x * s)

        def sy(y):
            return int(y * s)

        # Layout lógico del TFT (128x160)
        header_h = 14
        footer_h = 16
        side_w = 18
        top = header_h
        bottom = h - footer_h

        # Encabezado
        self.canvas.create_rectangle(sx(0), sy(0), sx(w), sy(header_h), fill='navy', outline='white', width=1)
        algo = self.state.get('algorithm', 'FCFS')
        self.canvas.create_text(sx(2), sy(7), text=algo, fill='white', font=("Arial", 8, "bold"), anchor='w')

        gate_color = 'lime' if self.state['gate_status'] == 'ABIERTO' else 'red'
        gate_text = self.state['gate_status']

        self.canvas.create_text(sx(w - 2), sy(7), text=gate_text, fill=gate_color, font=("Arial", 8, "bold"), anchor='e')

        # Paneles laterales y canal
        self.canvas.create_rectangle(sx(0), sy(top), sx(side_w), sy(bottom), fill='#0b2a6f', outline='cyan', width=1)
        self.canvas.create_text(sx(side_w // 2), sy(top + 4), text="IZQ", fill='yellow', font=("Arial", 7, "bold"))
        self.draw_queue_items(self.queue_left, sx, sy, 0, side_w, top + 12, bottom)

        self.canvas.create_rectangle(sx(side_w), sy(top), sx(w - side_w), sy(bottom), fill='#20B2AA', outline='white', width=1)
        self.canvas.create_text(sx(w // 2), sy(top + 4), text=f"CANAL {self.normalize_algorithm(self.state.get('algorithm', 'FCFS'))}", fill='black', font=("Arial", 7, "bold"))

        self.canvas.create_rectangle(sx(w - side_w), sy(top), sx(w), sy(bottom), fill='#0b2a6f', outline='cyan', width=1)
        self.canvas.create_text(sx(w - side_w // 2), sy(top + 4), text="DER", fill='yellow', font=("Arial", 7, "bold"))
        self.draw_queue_items(self.queue_right, sx, sy, w - side_w, w, top + 12, bottom)

        # Animación de barco activo
        if self.crossing['active'] and self.crossing['boat_id'] is not None:
            now = time.time()
            if self.crossing['paused']:
                elapsed = self.crossing['pause_progress'] * self.crossing['duration_s']
            else:
                elapsed = now - self.crossing['start_ts']

            progress = max(0.0, min(1.0, elapsed / max(1e-6, self.crossing['duration_s'])))

            x_left = side_w + 4
            x_right = w - side_w - 4
            if self.crossing['boat_origin'] == 'right' or self.crossing['direction'] == 'RL':
                x = x_right - (x_right - x_left) * progress
            else:
                x = x_left + (x_right - x_left) * progress
            y = (top + bottom) / 2

            self.canvas.create_rectangle(sx(x - 4), sy(y - 3), sx(x + 4), sy(y + 3), fill='red', outline='white', width=1)
            self.canvas.create_text(sx(x), sy(y), text=str(self.crossing['boat_id']), fill='white', font=("Arial", 7, "bold"))
            if self.crossing['boat_type']:
                self.canvas.create_text(sx(x), sy(y + 6), text=self.crossing['boat_type'][:3].upper(), fill='white', font=("Arial", 6, "bold"))
            if self.crossing['boat_origin']:
                side_tag = 'L' if self.crossing['boat_origin'] == 'left' else 'R'
                self.canvas.create_text(sx(x), sy(y - 7), text=side_tag, fill='white', font=("Arial", 6, "bold"))
            if self.crossing['boat_algorithm']:
                self.canvas.create_text(sx(x), sy(y + 12), text=self.algorithm_short(self.crossing['boat_algorithm']), fill='white', font=("Arial", 6, "bold"))

            if progress >= 1.0:
                # Mantiene el barco visible en el extremo hasta recibir el log real de finalización.
                progress = 1.0

        # Información de cola visible
        # Pie con estadísticas
        self.canvas.create_rectangle(sx(0), sy(bottom), sx(w), sy(h), fill='#000033', outline='white', width=1)
        self.canvas.create_text(sx(2), sy(bottom + 5), text=f"QL:{len(self.queue_left)}", fill='lightgreen', font=("Arial", 7), anchor='w')
        self.canvas.create_text(sx(34), sy(bottom + 5), text=f"QR:{len(self.queue_right)}", fill='lightgreen', font=("Arial", 7), anchor='w')
        self.canvas.create_text(sx(68), sy(bottom + 5), text=f"OK:{self.state['completed_total']}", fill='lightgreen', font=("Arial", 7), anchor='w')
        self.canvas.create_text(sx(100), sy(bottom + 5), text=f"C:{self.state['collision_count']}", fill='orange', font=("Arial", 7), anchor='w')

        # Estado de emergencia si aplica
        if self.state['emergency_mode'] != 'NONE':
            self.canvas.create_rectangle(sx(20), sy(top + 16), sx(w - 20), sy(top + 34), fill='red', outline='white', width=1)
            self.canvas.create_text(sx(w // 2), sy(top + 25), text="EMERGENCIA", fill='white', font=("Arial", 8, "bold"))

        # Sensor info
        if self.state['sensor_enabled']:
            sensor_text = f"S:{self.state['proximity_distance']}cm"
            self.canvas.create_text(sx(w // 2), sy(bottom - 6), text=sensor_text, fill='yellow', font=("Arial", 7))

        # Redraw a 20 FPS para animación fluida
        self.root.after(50, self.redraw)

    def draw_queue_items(self, queue_ids, sx, sy, panel_left, panel_right, top_y, bottom_y):
        """Dibuja barcos como cajas compactas dentro de la barra lateral."""
        max_items = 6
        visible = queue_ids[:max_items]
        slot_h = 16
        box_w = 11
        box_h = 10

        # Calcula el centro real de la barra azul lateral y deja un margen interno
        panel_w = panel_right - panel_left
        inner_margin = 2
        if panel_left == 0:
            x0 = panel_left + inner_margin
            x1 = x0 + box_w
            text_x = x0 + box_w / 2
        else:
            x1 = panel_right - inner_margin
            x0 = x1 - box_w
            text_x = x0 + box_w / 2

        y = top_y + 2
        font_id = ("Arial", 5, "bold")
        font_meta = ("Arial", 4, "bold")

        for boat_id in visible:
            meta = self.boat_algorithm_by_id.get(boat_id, {})
            boat_type = meta.get('type')
            boat_algo = meta.get('algorithm') or self.state.get('algorithm', 'FCFS')
            fill = self.boat_fill_color(boat_type)
            outline = 'white'
            self.canvas.create_rectangle(sx(x0), sy(y), sx(x1), sy(y + box_h), fill=fill, outline=outline, width=1)
            self.canvas.create_text(sx(text_x), sy(y + 3), text=f"#{boat_id}", fill='black', font=font_id, anchor='n')
            self.canvas.create_text(sx(text_x), sy(y + 6), text=f"{self.boat_type_short(boat_type)}{self.algorithm_short(boat_algo)}", fill='black', font=font_meta, anchor='n')
            y += slot_h

        if len(queue_ids) > max_items:
            self.canvas.create_text(sx(text_x), sy(bottom_y - 8), text=f"+{len(queue_ids) - max_items}", fill='white', font=("Arial", 5, "bold"), anchor='center')
    
    def on_closing(self):
        """Cierra la aplicación"""
        self.running = False
        if self.ser:
            self.ser.close()
        self.root.destroy()


if __name__ == '__main__':
    root = tk.Tk()
    # Cambiar COM5 al puerto que uses (COM4, COM5, etc. en Windows, /dev/ttyUSB0 en Linux)
    app = SchedulingShipsDisplay(root, port='COM6', baudrate=115200)
    root.protocol("WM_DELETE_WINDOW", app.on_closing)
    root.mainloop()
