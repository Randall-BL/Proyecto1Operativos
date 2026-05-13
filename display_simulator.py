#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Simulador de pantalla para Scheduling Ships
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
    """Interfaz Tkinter que refleja la pantalla embebida y los logs seriales."""

    def __init__(self, root, port='COM5', baudrate=115200):
        """Inicializa estado de UI, hilo serial y modelos de vista."""
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

        # Pantalla fisica: 128x160 (ancho x alto)
        self.tft_width = 128
        self.tft_height = 160
        self.scale = 3

        # Estado de cruces activos o en espera de inicio
        self.crossings = []

        # Parametros de dibujo para barcos en el canal
        self.boat_width = 8
        self.boat_height = 6
        self.boat_gap_px = 10

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

        # Cache de lado de origen por id para respetar la aparicion real
        self.boat_origin_by_id = {}

        # Cache de algoritmo por id para mostrar el algoritmo que tenia cada barco al entrar a cola
        self.boat_algorithm_by_id = {}

        # Casilla visual actual por barco (índice 0..channel_cells-1)
        self.boat_slot_by_id = {}

        # Buffer de movimientos por slot (evita que el barco "salte" al final si llega backlog)
        self.boat_slot_events_by_id = {}
        self.boat_slot_last_applied_ts_by_id = {}
        self.boat_move_period_ms_by_id = {}

        # Largo real de la lista (slots) reportado por logs de boat task: list_len = totalSlots + 1
        self.list_length = None

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
            'flow_mode': 'TICO',
            'gate_status': 'ABIERTO',
            'emergency_mode': 'NONE',
            'sensor_enabled': False,
            'proximity_distance': 999,
            'collision_count': 0,
        }
        
        # Marco principal con dos columnas
        main_frame = ttk.Frame(root)
        main_frame.pack(fill=tk.BOTH, expand=True, padx=10, pady=10)
        
        # ===== COLUMNA IZQUIERDA: PANTALLA =====
        display_frame = ttk.LabelFrame(main_frame, text="Display TFT (128x160)")
        display_frame.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=5)
        
        # Canvas para la pantalla (escalado 3x para mejor visualizacion)
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
        
        # Monitor serial (scrolledtext)
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
        
        # Inicia conexion serial en un hilo
        self.serial_thread = threading.Thread(target=self.serial_loop, daemon=True)
        self.serial_thread.start()
        
        # Bucle de redibujo
        self.redraw()
    
    def log_serial(self, message):
        """Agrega un mensaje al monitor serial"""
        if message == self.last_serial_log:
            return
        self.last_serial_log = message
        self.serial_output.insert(tk.END, message + '\n')
        self.serial_output.see(tk.END)  # Auto-desplazamiento
    
    def connect_serial(self):
        """Intenta conectar al puerto serial"""
        try:
            # timeout=0 => no bloqueante (evita que una línea incompleta congele el hilo)
            self.ser = serial.Serial(self.serial_port, self.baudrate, timeout=0, write_timeout=1)
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
        rx_buffer = bytearray()
        
        while self.running:
            try:
                if self.ser:
                    n = self.ser.in_waiting
                    if n:
                        chunk = self.ser.read(n)
                        if chunk:
                            rx_buffer.extend(chunk)

                    # Procesa todas las líneas completas disponibles
                    while True:
                        nl = rx_buffer.find(b'\n')
                        if nl == -1:
                            break
                        line_bytes = rx_buffer[:nl]
                        del rx_buffer[:nl + 1]
                        line = line_bytes.decode('utf-8', errors='ignore').strip('\r').strip()
                        if line:
                            buffer.append(line)
                            self.serial_queue.put(("log", line))
                            self.serial_queue.put(("parse", line))
                time.sleep(0.01)
            except Exception as e:
                self.serial_queue.put(("log", f"Serial error: {e}"))
                time.sleep(1)
    
    def clear_display_state(self):
        """Limpia el estado visual de la pantalla (barcos en tránsito y colas)"""
        self.crossings = []
        self.queue_left = []
        self.queue_right = []
        self.boat_algorithm_by_id = {}
        self.boat_type_by_id = {}
        self.boat_origin_by_id = {}
        self.boat_slot_by_id = {}
        self.boat_slot_events_by_id = {}
        self.boat_slot_last_applied_ts_by_id = {}
        self.boat_move_period_ms_by_id = {}
        self.list_length = None
        self.state['completed_total'] = 0
        self.state['completed_lr'] = 0
        self.state['completed_rl'] = 0
        self.log_serial("🔄 Pantalla limpiada - reinicio visual")

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
            
            # Si es comando "demo", limpiar pantalla
            if cmd.strip().lower() == 'demo' or cmd.strip().lower() == 'clear':
                self.clear_display_state()
        except Exception as e:
            self.log_serial(f"✗ Error al enviar: {e}")

    def process_serial_events(self):
        """Procesa eventos de serial en el hilo de UI"""
        processed = 0
        max_events_per_tick = 400
        while processed < max_events_per_tick:
            try:
                event = self.serial_queue.get_nowait()
            except Empty:
                break

            processed += 1

            etype = event[0]
            if etype == "log":
                self.log_serial(event[1])
            elif etype == "parse":
                self.parse_line(event[1])
            elif etype == "status":
                _, text, color = event
                self.status_label.config(text=text, foreground=color)

    def detect_direction(self, line):
        """Infiere la direccion a partir de una linea de log."""
        lower = line.lower()
        if "der->izq" in lower or "r->l" in lower or "right" in lower:
            return 'RL'
        if "izq->der" in lower or "l->r" in lower or "left" in lower:
            return 'LR'
        return 'LR'

    def normalize_boat_type(self, raw_type):
        """Normaliza el tipo de barco a un token canonico."""
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
        """Normaliza el nombre del algoritmo a una etiqueta canonica."""
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
        """Devuelve una etiqueta de un caracter para el algoritmo."""
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
        """Normaliza el origen a tokens izquierda/derecha."""
        if not raw_origin:
            return None
        value = raw_origin.strip().lower()
        aliases = {
            'l': 'left',
            'izq': 'left',
            'izquierda': 'left',
            'left': 'left',
            'r': 'right',
            'der': 'right',
            'derecha': 'right',
            'right': 'right',
        }
        return aliases.get(value, value)

    def _enqueue_slot_event(self, boat_id, slot):
        """Encola eventos de slot para aplicarlos gradualmente en redraw()."""
        if boat_id is None:
            return
        try:
            boat_id = int(boat_id)
        except Exception:
            return

        q = self.boat_slot_events_by_id.get(boat_id)
        if q is None:
            q = deque(maxlen=5000)
            self.boat_slot_events_by_id[boat_id] = q
        q.append(int(slot))

    def _apply_slot_events_for_boat(self, boat_id, now_ts):
        """Consume eventos de slot.

        Nota: Para mantener sincronía con la LCD (que redibuja por eventos),
        aplicamos el estado más reciente inmediatamente. Esto evita que el
        simulador se quede "atrasado" si llega backlog por Serial.
        """
        q = self.boat_slot_events_by_id.get(boat_id)
        if not q:
            return

        # Consume todo y deja el último slot.
        last = None
        while q:
            last = q.popleft()
        if last is not None:
            self.boat_slot_by_id[boat_id] = int(last)
            self.boat_slot_last_applied_ts_by_id[boat_id] = now_ts

    def rgb565_to_hex(self, value):
        """Convierte un valor RGB565 a un color hexadecimal."""
        r = (value >> 11) & 0x1F
        g = (value >> 5) & 0x3F
        b = value & 0x1F
        r8 = int(round(r * 255 / 31))
        g8 = int(round(g * 255 / 63))
        b8 = int(round(b * 255 / 31))
        return f"#{r8:02x}{g8:02x}{b8:02x}"

    def canal_travel_length(self):
        """Calcula la distancia horizontal de cruce en pixeles logicos."""
        side_w = 18
        return max(1, self.tft_width - (2 * side_w) - 8)

    def compute_safe_start(self, now, duration_s, direction):
        """Calcula el inicio mas temprano para evitar colisiones."""
        travel_len = self.canal_travel_length()
        gap_progress = min(0.4, self.boat_gap_px / travel_len)
        start_time = now

        for crossing in self.crossings:
            if not crossing.get('active'):
                continue

            start_a = crossing['start_ts']
            duration_a = crossing['duration_s']

            if crossing['direction'] != direction:
                end_a = start_a + duration_a
                if end_a > start_time:
                    start_time = end_a
                continue

            start_gap = start_a + (gap_progress * duration_a)
            end_gap = (start_a + duration_a) - ((1.0 - gap_progress) * duration_s)
            required = max(start_gap, end_gap)
            if required > start_time:
                start_time = required

        return start_time

    def get_crossing(self, boat_id):
        """Busca un cruce activo por id de barco."""
        for crossing in self.crossings:
            if crossing.get('active') and crossing.get('boat_id') == boat_id:
                return crossing
        return None

    def start_crossing(self, boat_id, boat_type, boat_origin, boat_algorithm):
        """Registra un cruce, aplicando espera si hay riesgo de colision."""
        now = time.time()
        duration_s = self.duration_for_boat_type(boat_type)
        direction = self.origin_to_direction(boat_origin)
        scheduled_start = now
        pending = False

        if self.state.get('flow_mode') == 'TICO':
            scheduled_start = self.compute_safe_start(now, duration_s, direction)
            pending = scheduled_start > now + 0.001

        crossing = {
            'boat_id': boat_id,
            'boat_type': boat_type,
            'boat_origin': boat_origin,
            'boat_algorithm': self.normalize_algorithm(boat_algorithm),
            'direction': direction,
            'start_ts': scheduled_start,
            'duration_s': duration_s,
            'paused': False,
            'pause_progress': 0.0,
            'active': True,
            'pending': pending,
        }

        self.crossings.append(crossing)

        if pending:
            self.add_boat_to_queue(boat_id, boat_origin, boat_type=boat_type, boat_algorithm=boat_algorithm, front=True)
        else:
            self.remove_boat_from_queues(boat_id)

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
        """Selecciona la cola que corresponde al origen."""
        return self.queue_right if boat_origin == 'right' else self.queue_left

    def duration_for_boat_type(self, boat_type):
        """Devuelve la duracion de animacion en segundos para el tipo de barco."""
        if not boat_type:
            boat_type = 'normal'

        duration_ms = self.service_time_by_type_ms.get(boat_type)
        if duration_ms is None:
            # No se cargó desde ShipModel.c: informar y usar alternativa segura (1000ms)
            try:
                self.serial_queue.put(("log", f"Aviso: tiempo para tipo '{boat_type}' no cargado desde ShipModel.c; usando alternativa 1000ms"))
            except Exception:
                pass
            duration_ms = 1000
        return duration_ms / 1000.0

    def origin_to_direction(self, boat_origin):
        """Convierte el origen en una etiqueta de direccion."""
        if boat_origin == 'right':
            return 'RL'
        return 'LR'

    def add_boat_to_queue(self, boat_id, boat_origin, boat_type=None, boat_algorithm=None, front=False):
        """Inserta o mueve un barco dentro de la cola adecuada."""
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
        """Elimina un id de barco de ambas colas."""
        if boat_id in self.queue_left:
            self.queue_left.remove(boat_id)
        if boat_id in self.queue_right:
            self.queue_right.remove(boat_id)

    def current_boat_label(self, boat_id):
        """Compone la etiqueta corta usada en los cuadros de cola."""
        meta = self.boat_algorithm_by_id.get(boat_id, {})
        boat_type = meta.get('type') or '?'
        algo = meta.get('algorithm') or self.state.get('algorithm', 'FCFS')
        return f"#{boat_id} {self.boat_type_short(boat_type)} {self.algorithm_short(algo)}"

    def boat_type_short(self, boat_type):
        """Devuelve la letra corta para el tipo de barco."""
        normalized = self.normalize_boat_type(boat_type)
        return {
            'normal': 'N',
            'pesquera': 'P',
            'patrulla': 'A',
        }.get(normalized, '?')

    def boat_fill_color(self, boat_type):
        """Devuelve el color de relleno en hex para el tipo de barco."""
        normalized = self.normalize_boat_type(boat_type)
        color_map = {
            'normal': self.rgb565_to_hex(0xFFFF),
            'pesquera': self.rgb565_to_hex(0x07FF),
            'patrulla': self.rgb565_to_hex(0xF800),
        }
        fallback = self.rgb565_to_hex(0xFFE0)
        return {
            'normal': color_map['normal'],
            'pesquera': color_map['pesquera'],
            'patrulla': color_map['patrulla'],
        }.get(normalized, fallback)
    
    def parse_line(self, line):
        """Parsea una línea del serial y actualiza estado"""
        try:
            def _remove_from_channel_and_requeue(boat_id, front=True):
                """Quita el barco del canal en el simulador y lo devuelve a la cola.

                Se usa para preempciones (algoritmos apropiativos) y otros casos donde
                el barco deja de estar activo en el canal en la LCD.
                """
                if boat_id is None:
                    return
                try:
                    boat_id = int(boat_id)
                except Exception:
                    return

                # Detiene animación en canal
                crossing = self.get_crossing(boat_id)
                if crossing:
                    crossing['active'] = False
                    self.crossings = [item for item in self.crossings if item.get('active')]

                if self.state.get('active_boat') == boat_id:
                    self.state['active_boat'] = None

                # Limpia estado de slots para evitar que reaparezca en canal por backlog
                self.boat_slot_by_id.pop(boat_id, None)
                self.boat_slot_events_by_id.pop(boat_id, None)
                self.boat_slot_last_applied_ts_by_id.pop(boat_id, None)

                # Devuelve a la cola según origen (como en la LCD)
                boat_origin = self.boat_origin_by_id.get(boat_id, 'left')
                boat_type = self.boat_type_by_id.get(boat_id)
                boat_algorithm = self.boat_algorithm_by_id.get(boat_id, {}).get('algorithm', self.state.get('algorithm', 'FCFS'))
                self.add_boat_to_queue(boat_id, boat_origin, boat_type=boat_type, boat_algorithm=boat_algorithm, front=front)

            # Detecta reinicio/reboot del ESP32
            if any(keyword in line.lower() for keyword in ['starting...', 'boot', 'reboot', 'reset', 'ets jul']):
                self.clear_display_state()
                return

            # Preempción (STRN/EDF/PRIO): el firmware reencola al barco activo y lo saca del canal.
            # El log no incluye el id del preemptado, así que usamos el activo actual.
            if line.startswith("Preemption:"):
                preempted_id = self.state.get('active_boat')
                _remove_from_channel_and_requeue(preempted_id, front=True)
                return

            # Si el firmware destruye un barco (por ejemplo, cola llena en emergencia),
            # el simulador debe removerlo del canal para evitar "fantasmas".
            if "se destruye" in line and "#" in line:
                m = re.search(r'#(?P<id>\d+)', line)
                if m:
                    destroyed_id = int(m.group('id'))
                    crossing = self.get_crossing(destroyed_id)
                    if crossing:
                        crossing['active'] = False
                        self.crossings = [item for item in self.crossings if item.get('active')]
                    if self.state.get('active_boat') == destroyed_id:
                        self.state['active_boat'] = None
                    self.boat_slot_by_id.pop(destroyed_id, None)
                    self.boat_slot_events_by_id.pop(destroyed_id, None)
                    self.boat_slot_last_applied_ts_by_id.pop(destroyed_id, None)
                return

            # Logs de movimiento por slots emitidos por ShipScheduler.c
            if line.startswith("[BOAT TASK #"):
                id_match = re.search(r'\[BOAT TASK #(?P<id>\d+)\]', line)
                if id_match:
                    boat_id = int(id_match.group('id'))

                    # Ej: [BOAT TASK #2] movesCount=99 perMoveMs=40 totalSlots=99 stepSize=1
                    if "movesCount=" in line and "perMoveMs=" in line and "totalSlots=" in line:
                        m = re.search(r'movesCount=(?P<moves>\d+)\s+perMoveMs=(?P<per>\d+)\s+totalSlots=(?P<total>\d+)\s+stepSize=(?P<step>\d+)', line)
                        if m:
                            total_slots = int(m.group('total'))
                            self.list_length = total_slots + 1
                            self.boat_move_period_ms_by_id[boat_id] = int(m.group('per'))
                            # Inicializa slot de entrada si aún no existe
                            if boat_id not in self.boat_slot_by_id and self.list_length and self.list_length > 0:
                                origin = self.boat_origin_by_id.get(boat_id)
                                if origin == 'right':
                                    self.boat_slot_by_id[boat_id] = self.list_length - 1
                                elif origin == 'left':
                                    self.boat_slot_by_id[boat_id] = 0

                    # Ej: [BOAT TASK #2] moved to slot 98
                    if "moved to slot" in line:
                        m = re.search(r'moved to slot\s+(?P<slot>-?\d+)', line)
                        if m:
                            self._enqueue_slot_event(boat_id, int(m.group('slot')))

                    return
            
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

            if line.startswith("Flujo:"):
                flow_value = line.split(":", 1)[1].strip().split()[0]
                self.state['flow_mode'] = flow_value.strip().upper()

            # Detecta barco activo
            if "Start ->" in line and "barco #" in line:
                match = re.search(r'barco #(\d+)', line)
                if match:
                    boat_id = int(match.group(1))
                    boat_type = self.boat_type_by_id.get(boat_id)
                    boat_origin = self.boat_origin_by_id.get(boat_id)
                    boat_algorithm = self.boat_algorithm_by_id.get(boat_id, {}).get('algorithm', self.state.get('algorithm', 'FCFS'))
                    self.state['active_boat'] = boat_id
                    self.start_crossing(boat_id, boat_type, boat_origin, boat_algorithm)
            
            # Detecta finalizacion de barco
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

                    crossing = self.get_crossing(boat_id)
                    if crossing:
                        crossing['active'] = False
                        self.crossings = [item for item in self.crossings if item.get('active')]
                    self.state['active_boat'] = None
                    if 'izq' in line or 'Left' in line:
                        self.state['completed_lr'] += 1
                    else:
                        self.state['completed_rl'] += 1
                    self.state['completed_total'] += 1

            if "Pausado barco #" in line or "congelado en el canal" in line:
                match = re.search(r'#(\d+)', line)
                if match:
                    boat_id = int(match.group(1))
                    crossing = self.get_crossing(boat_id)
                    if crossing:
                        crossing['paused'] = True
                        elapsed = time.time() - crossing['start_ts']
                        crossing['pause_progress'] = max(0.0, min(1.0, elapsed / crossing['duration_s']))

            if "Reanudado barco #" in line:
                match = re.search(r'#(\d+)', line)
                if match:
                    boat_id = int(match.group(1))
                    crossing = self.get_crossing(boat_id)
                    if crossing:
                        crossing['paused'] = False
                        crossing['start_ts'] = time.time() - (crossing['pause_progress'] * crossing['duration_s'])

            if "recolocado en cola" in line or "[EMERGENCY]" in line and "recolocado" in line:
                self.serial_queue.put(("log", f"[SIM DEBUG] Detectado 'recolocado en cola' en línea: {line}"))
                match = re.search(r'Barco #(\d+)', line)
                if match:
                    boat_id = int(match.group(1))
                    self.serial_queue.put(("log", f"[SIM DEBUG] Recolocando barco #{boat_id} en cola"))
                    boat_origin = self.boat_origin_by_id.get(boat_id, 'left')
                    boat_type = self.boat_type_by_id.get(boat_id)
                    boat_algorithm = self.state.get('algorithm', 'FCFS')
                    self.serial_queue.put(("log", f"[SIM DEBUG] Origen={boat_origin}, Tipo={boat_type}, Algo={boat_algorithm}"))
                    self.add_boat_to_queue(boat_id, boat_origin, boat_type=boat_type, boat_algorithm=boat_algorithm)
                    crossing = self.get_crossing(boat_id)
                    if crossing:
                        self.serial_queue.put(("log", f"[SIM DEBUG] Limpiando estado activo para barco #{boat_id}"))
                        crossing['active'] = False
                        self.crossings = [item for item in self.crossings if item.get('active')]
                        self.state['active_boat'] = None
                    else:
                        self.serial_queue.put(("log", f"[SIM DEBUG] Barco #{boat_id} no es el activo"))
                else:
                    self.serial_queue.put(("log", f"[SIM DEBUG] No se encontró 'Barco #' en línea: {line}"))

            if "[EMERGENCY]" in line and "Cola llena" in line:
                match = re.search(r'Barco #(\d+)', line)
                if match:
                    boat_id = int(match.group(1))
                    crossing = self.get_crossing(boat_id)
                    if crossing:
                        crossing['active'] = False
                        self.crossings = [item for item in self.crossings if item.get('active')]
                        self.state['active_boat'] = None

            # Nota: '[FLOW][SAFE] Requeue' contiene pistas internas del scheduler.
            # La pantalla no debe tomar decisiones de planificacion; se ignoran estas lineas
            # para que el simulador solo refleje logs explicitos de encolado/reencolado.
            if "[FLOW][SAFE] Requeue" in line:
                # Se ignora a proposito en el simulador de pantalla
                pass
            
            # Detecta cantidad en cola
            if "Ready count:" in line:
                match = re.search(r'Ready count: (\d+)', line)
                if match:
                    self.state['ready_count'] = int(match.group(1))
            
            # Detecta estado de compuertas
            if "CERRADO" in line or "CLOSED" in line:
                self.state['gate_status'] = 'CERRADO'
            if "ABIERTO" in line or "OPEN" in line:
                self.state['gate_status'] = 'ABIERTO'
            
            # Detecta emergencia
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

        # Animacion de barcos activos
        now = time.time()
        draw_items = []
        for crossing in self.crossings:
            if not crossing.get('active'):
                continue

            if crossing.get('pending'):
                if now < crossing['start_ts']:
                    continue
                crossing['pending'] = False
                self.remove_boat_from_queues(crossing['boat_id'])

            if crossing.get('paused'):
                elapsed = crossing['pause_progress'] * crossing['duration_s']
            else:
                elapsed = now - crossing['start_ts']

            progress = max(0.0, min(1.0, elapsed / max(1e-6, crossing['duration_s'])))

            # Movimiento por casillas discretas
            cells = getattr(self, 'channel_cells', 10) or 10
            denom = max(1, cells - 1)
            # Preferir posición desde logs de scheduler si existe
            bid = crossing.get('boat_id')

            if bid is not None:
                # Aplica movimientos pendientes sin consumirlos de golpe
                self._apply_slot_events_for_boat(bid, now)

            if bid is not None and bid in self.boat_slot_by_id:
                # Usar posición informada por logs de "moved to slot"
                slot = self.boat_slot_by_id[bid]
                list_len = getattr(self, 'list_length', None)
                if list_len and list_len > 0:
                    # Mapeo por grupos: cada (list_len / cells) slots = una casilla visual
                    # Ejemplo: list_len=100, cells=10 → cada 10 slots = 1 casilla
                    step_size = (list_len + cells - 1) // cells if cells > 0 else 1
                    if step_size <= 0:
                        step_size = 1

                    # Para RL invertimos el slot para que el barco arranque a la derecha
                    if crossing.get('boat_origin') == 'right' or crossing.get('direction') == 'RL':
                        effective = (list_len - 1) - slot
                    else:
                        effective = slot
                    if effective < 0:
                        effective = 0
                    cell_index = int(effective) // int(step_size)
                    if cell_index > denom:
                        cell_index = denom
                    effective_for_sort = int(effective)
                else:
                    # Fallback: usar progreso temporal
                    cell_index = int(progress * denom)
                    if cell_index > denom:
                        cell_index = denom
                    effective_for_sort = int(cell_index)
            else:
                # Sin logs de slot: mantener en la entrada hasta recibir eventos.
                if crossing.get('boat_origin') == 'right' or crossing.get('direction') == 'RL':
                    cell_index = denom
                else:
                    cell_index = 0
                effective_for_sort = int(cell_index)

            x_left = side_w + 4
            x_right = w - side_w - 4
            pos_ratio = cell_index / float(denom)
            if crossing['boat_origin'] == 'right' or crossing['direction'] == 'RL':
                x = x_right - (x_right - x_left) * pos_ratio
            else:
                x = x_left + (x_right - x_left) * pos_ratio
            y = (top + bottom) / 2
            fill_color = self.boat_fill_color(crossing.get('boat_type'))

            # Agrupar por casilla y dirección para evitar solapamiento visual
            direction_key = crossing.get('direction') or ('RL' if (crossing.get('boat_origin') == 'right') else 'LR')
            group_key = (cell_index, direction_key)
            draw_items.append({
                'group_key': group_key,
                'x': x,
                'y': y,
                'pos': effective_for_sort,
                'fill_color': fill_color,
                'crossing': crossing,
            })

        # Dibuja grupos con offset horizontal (un solo carril) para que no se solapen.
        if draw_items:
            groups = {}
            for item in draw_items:
                groups.setdefault(item['group_key'], []).append(item)

            x_min = side_w + 4
            x_max = w - side_w - 4
            spacing = self.boat_width + 2

            for _key, items in groups.items():
                # Ordenar por posición efectiva: el que está más adelante se dibuja primero.
                # (pos ya viene invertido para RL).
                items.sort(key=lambda it: (-int(it.get('pos', 0)), int(it['crossing'].get('boat_id', 0))))

                # En un carril: el de atrás se desplaza hacia atrás en X.
                direction = items[0]['group_key'][1]
                sign = 1 if direction == 'RL' else -1

                for i, item in enumerate(items):
                    crossing = item['crossing']
                    x = item['x'] + (i * spacing * sign)
                    if x < x_min:
                        x = x_min
                    if x > x_max:
                        x = x_max
                    y = item['y']

                    self.canvas.create_rectangle(
                        sx(x - (self.boat_width / 2)),
                        sy(y - (self.boat_height / 2)),
                        sx(x + (self.boat_width / 2)),
                        sy(y + (self.boat_height / 2)),
                        fill=item['fill_color'],
                        outline='white',
                        width=1,
                    )
                    self.canvas.create_text(sx(x), sy(y), text=str(crossing['boat_id']), fill='white', font=("Arial", 7, "bold"))
                    if crossing.get('boat_type'):
                        self.canvas.create_text(sx(x), sy(y + 6), text=crossing['boat_type'][:3].upper(), fill='white', font=("Arial", 6, "bold"))
                    if crossing.get('boat_origin'):
                        side_tag = 'L' if crossing['boat_origin'] == 'left' else 'R'
                        self.canvas.create_text(sx(x), sy(y - 7), text=side_tag, fill='white', font=("Arial", 6, "bold"))
                    if crossing.get('boat_algorithm'):
                        self.canvas.create_text(sx(x), sy(y + 12), text=self.algorithm_short(crossing['boat_algorithm']), fill='white', font=("Arial", 6, "bold"))

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

        # Redibuja a 20 FPS para animacion fluida
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
    app = SchedulingShipsDisplay(root, port='/dev/ttyACM0', baudrate=115200)
    root.protocol("WM_DELETE_WINDOW", app.on_closing)
    root.mainloop()
