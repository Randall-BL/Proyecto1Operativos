# Scheduling Ships - To-Do List

## Calendarizadores
- [x] FCFS
- [x] Round Robin
- [x] Prioridad
- [x] SJF
- [x] STRN
- [x] EDF


## Canal y Control de Flujo
- [x] Equidad (W parameter)
- [x] Letrero (dirección alternante)
- [x] Tico (sin control)
- [x] Detección de colisiones
- [x] Movimiento de barcos

## Barcos
- [x] Tipos: Normal, Pesquera, Patrulla
- [x] Generación por carga predefinida

## Interrupciones
- [x] Sensor de proximidad
- [x] Cerrado de compuertas
- [x] Re-queueing en emergencia

## ESP32 + LCD
- [ ] Setup I2C y LCD
- [x] Pantalla estado canal
- [ ] Pantalla detalles barcos
- [ ] Pantalla estadísticas
- [ ] Pantalla configuración
- [ ] Botones de entrada
- [x] Pantalla en horizontal

## FreeRTOS
- [x] Task scheduler
- [ ] Task física del canal
- [ ] Task UI update
- [x] Task input handler
- [ ] Mutexes y sincronización
- [ ] Integración tareas↔scheduler

## Configuración
- [x] Parsear config.txt
- [ ] Parsear ships.txt
- [ ] Validación de parámetros

## Testing
- [x] Test de cada scheduler
- [x] Test de algoritmos flujo
- [x] Test de colisiones
- [ ] Test de sensor
- [ ] Sin segfault/crashes

## Documentación
- [ ] IEEE-Trans (5 páginas max)
- [ ] README.md

## Entrega
- [ ] Código compilable
- [ ] ESP32 funcionando
- [ ] Archivos config ejemplo
- [ ] Backup listo
