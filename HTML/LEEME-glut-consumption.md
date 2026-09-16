# Glut Consumption — el consumo del cuarto descuenta el flushing

`glut-consumption.html` es una animación aparte, hermana de `flushing.html`. No
reemplaza a la original: aquella muestra por qué el flushing detiene la planta
dos horas, y esta añade encima un tercer escenario, **Glut Consumption**, donde
se mide lo que cada cuarto saca por sus Glut Stations y ese consumo se descuenta
del flushing que hay que hacer.

Se abre con doble clic. Es un solo archivo, sin instalación, sin servidor y sin
nada que descargar, y funciona sin conexión. Todo lo que `LEEME.md` explica del
sistema —los dos tanques, el manifold, el cabezal, los ramales, el empuje del
lote B sobre el lote A, la resta V<sub>I</sub> − V<sub>F</sub> y el PT-100— vale
igual aquí; este documento solo cuenta lo que es nuevo.

## De qué va

El volumen mínimo de flushing es un volumen que tiene que **pasar por la línea**
del cuarto. No exige que salga por la manguera de la purga: si el propio cuarto
lo saca por sus Glut Stations, la tubería queda igual de desplazada y el fluido
del lote anterior va bajando solo. Lo que hoy impide acreditarlo es que el único
instrumento está en el tanque y no distingue por cuál boquilla salió el líquido.

Con un flujómetro en cada Glut Station sí se puede. Al cambiar el lote se abre
una **ventana de lectura**: cada estación totaliza lo suyo, la aplicación lo suma
por cuarto y lo descuenta del volumen validado. Al cerrar la ventana solo se
purga lo que faltó.

## Los números

Cada cuarto tiene sus estaciones: **Training 4, CR1 6, CR2 6, CR3 4 y CR4 5, 25
en total**. Con los consumos que usa la animación:

| Cuarto | Glut Stations | Volumen validado | Consumo acreditado | Flushing pendiente |
|---|---|---|---|---|
| Training | 4 | 120 L | 48 L (40 %) | **72 L** |
| CR1 | 6 | 450 L | 450 L | **0 L — no se purga** |
| CR2 | 6 | 252 L | 252 L | **0 L — no se purga** |
| CR3 | 4 | 266 L | 189 L | **77 L** |
| CR4 | 5 | 295 L | 118 L (40 %) | **177 L** |
| **Total** | **25** | **1383 L** | **1057 L** | **326 L** |

CR1 y CR2, con seis estaciones cada uno, cubren su volumen validado ellos solos:
CR1 lo cruza en el minuto 51 y CR2 en el 44. De ahí en adelante quedan liberados
y el cronograma ni siquiera les abre fila de purga.

| | **Hoy** | **Con la aplicación** | **Glut Consumption** |
|---|---|---|---|
| Instrumento | PT-100 en el tanque | Flujómetro en la Glut final de cada cuarto | Flujómetro en **cada una de las 25 Glut Stations** |
| Qué mide | El volumen de todo el tanque | Lo que sale por esa boquilla | Lo que saca el cuarto por todas sus estaciones |
| Estaciones que se purgan | 5 | 5 | **3** |
| Volumen de la campaña | 1383 L | 1383 L | **326 L** |
| Desplazamiento del lote A | 329 L | 329 L | **0 L**: el consumo ya vació la línea |
| Duración del trabajo | ≈ 1 h 47 min | ≈ 1 h 47 min | **≈ 36 min** |
| Producción detenida | **2 h 00 min** | 0 min | 0 min |

## Qué se ve en pantalla

- **Al lado del tanque**, donde en los otros escenarios va la vista de detalle de
  V<sub>I</sub> − V<sub>F</sub>, aparece el **libro de consumo acreditado**: una
  barra por cuarto contra su volumen validado, y abajo el pendiente total, que va
  bajando de 1383 L a 326 L mientras corre la ventana.
- **En cada cuarto**, sus Glut Stations dibujadas una por una, abriendo y
  cerrando con su chorro —en verde salvia las que están abiertas—, la pantalla de
  la AMF con el totalizador, la barra de `ACREDITADO x / meta` y la línea
  `falta purgar N L`, que pasa a `sin purga · volumen cubierto` cuando el cuarto
  queda liberado.
- **En el cronograma**, una barra de crédito por fila dentro de la ventana de
  lectura, la raya del cierre, y después solo tres bloques de purga —72 L, 77 L y
  177 L—. CR1 y CR2 llevan en su lugar el rótulo `SIN PURGA · VOLUMEN CUBIERTO
  POR EL CONSUMO`.
- **En la tabla del pie**, que deja de ser un cronograma y pasa a ser la orden de
  trabajo: estaciones, volumen validado, consumo acreditado y flushing pendiente.

El consumo posterior al cierre de la ventana no se descuenta: queda de margen.
Por eso el totalizador de cada cuarto se congela en ese punto aunque las
estaciones sigan abiertas.

## Controles

Los mismos que la animación original, con un escenario más.

| | |
|---|---|
| Botones **Hoy / Con la aplicación / Glut Consumption** | Cambian de escenario y reinician la corrida |
| **Pausar / Reproducir**, **Reiniciar** | Control de la corrida |
| **×30 / ×120 / ×300** | Aceleración del reloj respecto al tiempo real |
| Barra espaciadora | Pausar o reproducir |
| Tecla **R** | Reiniciar |
| Teclas **1**, **2** y **3** | Escenario 1 (hoy), 2 (con la aplicación) y 3 (Glut Consumption) |
| Puntero sobre el cronograma | Qué está haciendo cada cuarto en ese momento |

### Enlaces directos

- `glut-consumption.html#C` — abre directamente en Glut Consumption
- `glut-consumption.html#C,3400` — con CR1 y CR2 ya cubiertos, antes del cierre
  de la ventana
- `glut-consumption.html#C,4300` — justo después del cierre, con la orden de
  trabajo de 326 L ya emitida

## Supuestos nuevos, que conviene revisar antes de presentarla

Los de `LEEME.md` siguen valiendo. Estos son los que añade este escenario:

| Supuesto | Valor usado |
|---|---|
| Glut Stations por cuarto | Training 4, CR1 6, CR2 6, CR3 4, CR4 5 |
| Ventana de lectura del consumo | 60 min |
| Caudal de una Glut Station abierta | ≈ 4,5 L/min, en tramos de 30 s |
| Consumo acreditado en la ventana | 48 L Training, 540 L CR1, 360 L CR2, 189 L CR3, 118 L CR4 |

Los consumos están declarados en la tabla `USED` al inicio del `<script>`, en
litros por cuarto dentro de la ventana. Todo lo demás —el crédito, el pendiente
de cada cuarto, qué cuartos se purgan, el cronograma, la tabla y los totales— se
recalcula solo a partir de ahí, así que para probar otros consumos basta cambiar
esos cinco números. El patrón de apertura de cada estación es determinista y está
calibrado contra la ventana, de modo que el acumulado al cierre cae exactamente
en el valor declarado sin importar a qué velocidad se corra la animación ni en
qué instante se pause.

**Los consumos por cuarto son el dato más importante que falta confirmar.** Los
de CR1, CR2 y CR3 salen de lo que se tiene medido; los de Training y CR4 están
puestos en el 40 % de su volumen validado, a falta del dato real.

## Qué falta / próximos pasos

Además de los de `LEEME.md`:

- **Confirmar los consumos por cuarto** contra la operación real, y sobre todo
  los de Training y CR4. De esos cinco números salen los 326 L y los 36 min.
- **Confirmar el número de Glut Stations por cuarto** (4, 6, 6, 4 y 5) contra los
  isométricos, y si todas van a llevar flujómetro o solo algunas.
- **Acordar la ventana de lectura**: cuánto tiempo se deja acumular consumo antes
  de emitir la orden de trabajo, y si el crédito se congela en ese punto —como lo
  hace la animación— o sigue descontándose hasta que arranca cada purga.
- **Validar el criterio con calidad**: que el volumen consumido por las Glut
  Stations del cuarto cuente como volumen de flushing es el supuesto de fondo de
  todo este escenario, y es el que hay que llevar a revisión.
