# Scheduling Ships - To-Do List

## Calendarizadores
- [x] FCFS
- [x] Round Robin
- [x] Prioridad
- [x] SJF
- [x] STRN
- [x] EDF


## Canal y Control de Flujo
- [ ] Equidad (W parameter)
- [ ] Letrero (dirección alternante)
- [ ] Tico (sin control)
- [ ] Detección de colisiones
- [x] Movimiento de barcos

## Barcos
- [x] Tipos: Normal, Pesquera, Patrulla
- [x] Generación por carga predefinida
- [ ] Generación por botones

## Interrupciones
- [ ] Sensor de proximidad
- [ ] Cerrado de compuertas
- [ ] Re-queueing en emergencia

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
- [ ] Parsear config.txt
- [ ] Parsear ships.txt
- [ ] Validación de parámetros

## Testing
- [x] Test de cada scheduler
- [ ] Test de algoritmos flujo
- [ ] Test de colisiones
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
