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
- [x] Setup I2C y LCD
- [x] Pantalla estado canal
- [x] Pantalla detalles barcos
- [x] Pantalla estadísticas
- [x] Pantalla en horizontal
- [ ] Evitar parpadeo de la pantalla

## FreeRTOS
- [x] Task scheduler
- [x] Task física del canal
- [x] Task UI update
- [x] Task input handler
- [x] Mutexes y sincronización
- [x] Integración tareas↔scheduler

## Configuración
- [x] Parsear config.txt
- [x] Validación de parámetros

## Testing
- [x] Test de cada scheduler
- [x] Test de algoritmos flujo
- [x] Test de colisiones
- [x] Test de sensor
- [ ] Sin segfault/crashes

## Documentación
- [ ] IEEE-Trans (5 páginas max)
- [ ] README.md

## Entrega
- [ ] Código compilable
- [ ] ESP32 funcionando
- [ ] Archivos config ejemplo
