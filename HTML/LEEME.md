# Animación — Flushing de la línea GLUT

Animación 2D de la campaña de *flushing* de la línea de distribución de GLUT, y
de por qué hoy obliga a detener la planta unas dos horas.

## Cómo se abre

Doble clic en **`flushing.html`**. Es un solo archivo, sin instalación, sin
servidor y sin nada que descargar: corre en cualquier navegador moderno (Edge,
Chrome, Firefox) y **funciona sin conexión**, porque la única tipografía que usa
es Arial, que ya está en el sistema.

## El sistema que muestra

Dos tanques alimentan el mismo cabezal de distribución a través de un manifold:
el **tanque A**, con el lote anterior, y el **tanque B**, con el lote nuevo. Del
cabezal cuelga un ramal por cada cuarto: **Training, CR1, CR2, CR3 y CR4**. Cada
ramal tiene varios puntos de uso, y el flushing se ejecuta siempre en el **último
POU**, el más lejano del tanque, porque es el peor caso de desplazamiento de
volumen en la tubería.

## De esto va el flushing: el lote B empuja al lote A

Al arrancar la campaña se cierra la válvula del tanque A y se abre la del B. A
partir de ahí la solución nueva entra a la tubería y **va empujando a la que
quedó del lote anterior**, como un pistón: mientras un tramo tenga lote A, lo que
sale por su extremo sigue siendo lote A. Por eso los primeros litros de cada
purga no limpian nada —solo desplazan— y por eso el volumen mínimo de flushing es
varias veces el volumen de la tubería.

La animación lo muestra con dos colores, y se ve en cuatro sitios a la vez:

- **En la tubería.** El lote A es ámbar y el lote B es azul. Entre los dos hay un
  filete claro, que es la interfase, y se ve avanzar por el cabezal y meterse en
  el ramal del cuarto que está purgando.
- **En la boquilla.** El chorro sale ámbar mientras todavía esté saliendo lote
  anterior, y cambia a azul cuando el ramal quedó desplazado. Ese cambio de color
  es el momento en que la purga empieza a servir de algo.
- **En cada cuarto.** El recuadro dice `DESPLAZANDO` y «empujando lote A · faltan
  N L» mientras dura, y luego pasa a `FLUSHING` y «ya sale lote B por la
  boquilla». Los cuartos que aún no se purgan avisan «lote A en el ramal».
- **En el cronograma.** La barra de cada purga arranca con un tramo ámbar, que es
  la parte del volumen que se va solo en desplazar. Son 40 L en Training, 132 L
  en CR1, 56 L en CR2, 51 L en CR3 y 50 L en CR4: **329 L de los 1383 L** de la
  campaña.

El cabezal es compartido, así que lo que desplaza una purga le queda desplazado a
la siguiente. Por eso Training es el único que paga los 18 L del primer tramo, y
por eso el tramo ciego que sigue después de CR4 se queda con lote A para siempre:
nunca pasa caudal por él.

La campaña es **secuencial**: se purga una estación, se cierra, y sigue la
siguiente, hasta terminar las cinco. Son 1383 L en total.

El instrumento del tanque **no es un flujómetro: es un sensor de presión**
(PT-100), y está en el tanque que esté en servicio, o sea el B. Mide la presión
hidrostática en el fondo; con las dimensiones del tanque esa presión se convierte
en nivel y en volumen. El volumen de cada purga no se mide, **se deduce restando
V<sub>I</sub> − V<sub>F</sub>**.

### La resta, ampliada

En el tanque esa resta es diminuta: 120 L sobre 6000 L son unos tres píxeles, y
así no se ve nada. Por eso el esquema lleva una **vista de detalle** al lado del
tanque, amarrada a él con una caja punteada y dos líneas guía, que amplía
justamente esa rebanada. Ahí se ven, a escala legible:

- la línea roja de **V<sub>I</sub>**, el volumen que quedó registrado al abrir la
  boquilla;
- la línea negra de **V** —o de **V<sub>F</sub>** cuando la purga ya cerró y el
  nivel se estabilizó—;
- la **banda rayada** entre las dos, que es la resta, con su flecha y su valor;
- y una marca blanca punteada con la **meta** del cuarto, para ver cuánto falta.

La ventana de la vista se reescala sola según el cuarto en turno, de modo que la
rebanada ocupe siempre buena parte de la tira lo mismo con 120 L que con 450 L.

De ahí sale todo lo demás. El tanque solo ve que bajó; no sabe por cuál boquilla
salió el líquido. Cualquier consumo en otro cuarto se confunde con el flushing,
así que la línea entera queda sin consumo desde que arranca la primera estación
hasta que termina la última.

## Los dos escenarios

Se cambian con los dos botones de arriba a la derecha, o con las teclas 1 y 2.

| | **Hoy** | **Con la aplicación** |
|---|---|---|
| Instrumento | PT-100, sensor de presión en el tanque | Un flujómetro en la Glut Station del último POU de cada cuarto |
| Qué mide | El volumen de todo el tanque | Lo que sale por esa boquilla |
| Cómo se obtiene el volumen | Restando V<sub>I</sub> − V<sub>F</sub> | Se lee el totalizador |
| Orden de la campaña | Secuencial, una estación a la vez | **Igual**: secuencial, una estación a la vez |
| Desplazamiento del lote A | **Igual**: 329 L de los 1383 L | **Igual**: 329 L de los 1383 L |
| Duración del trabajo | ≈ 1 h 47 min | ≈ 1 h 47 min |
| Producción detenida | **2 h 00 min** | **0 min** |

**El proyecto no busca purgar varios cuartos a la vez.** La campaña sigue siendo
la misma y dura lo mismo. Lo único que cambia es que la planta no tiene que
parar, porque unos litros consumidos en otro cuarto ya no entran en la cuenta del
flushing.

La animación lo demuestra con un aviso que se calcula solo durante la corrida:

> Durante la purga de TRAINING, en el minuto 09:40, la resta del tanque ya
> marcaba los 120 L, pero por la boquilla solo habían salido **102 L**. Los 18 L
> de diferencia son el consumo de los otros cuartos, y para el tanque son
> indistinguibles del flushing.

Es decir: hoy no basta con dejar la línea abierta y confiar en la resta. Con el
flujómetro en la boquilla, sí.

## Qué hay en pantalla

- **El recuadro de la animación**, que lo tiene todo junto: a la izquierda los
  dos tanques con su manifold, la vista de detalle de V<sub>I</sub> −
  V<sub>F</sub> y la carátula del PT-100; arriba a la derecha el cronograma; y
  abajo el cabezal con los cinco cuartos. La marca roja punteada dentro del
  tanque B es el V<sub>I</sub> de la estación que está en turno.
- **El cronograma**, dentro de ese mismo recuadro, es donde se ve el asunto
  completo: una fila por cuarto con su preparación, su purga y su lectura, y
  arriba una banda que dice si la planta puede consumir. Esa banda es lo único
  que cambia entre los dos escenarios.
- **Cuatro instrumentos** debajo: la carátula del PT-100, la resta
  V<sub>I</sub> − V<sub>F</sub> de la estación en turno —con cuántos de esos
  litros solo empujaron el lote A—, el reloj de producción detenida y el avance
  de la campaña, cuya barra viene partida en el tramo de desplazamiento y el de
  purga.
- **Dos tarjetas al pie**: la explicación del escenario y el cronograma en
  números.

## Controles

| | |
|---|---|
| Botones **Hoy / Con la aplicación** | Cambian de escenario y reinician la corrida |
| **Pausar / Reproducir**, **Reiniciar** | Control de la corrida |
| **×30 / ×120 / ×300** | Aceleración del reloj respecto al tiempo real |
| Barra espaciadora | Pausar o reproducir |
| Tecla **R** | Reiniciar |
| Teclas **1** y **2** | Escenario 1 (hoy) y escenario 2 (con la aplicación) |
| Puntero sobre el cronograma | Qué está haciendo cada cuarto en ese momento |

Una corrida completa son 125 minutos simulados: a ×120 tarda poco más de un
minuto real, a ×300 unos veinticinco segundos.

### Enlaces directos

El escenario y el instante se pueden fijar en la URL, lo cual sirve para saltar
directo a un momento concreto durante una presentación:

- `flushing.html#B` — abre el segundo escenario
- `flushing.html#B,4200` — segundo escenario en el minuto 70, con el aviso ya
  visible
- `flushing.html#A,7300` — primer escenario al final, con las 2 h de paro
  acumuladas

## Colores y tipografía

Todo sale del template corporativo que está en la raíz del proyecto,
`1121 - Template 6.06 - Presentation Template (1).pptx` (tema «Edwards»):

| Uso en la animación | Color | Origen en el tema |
|---|---|---|
| Títulos, producción detenida, marca de V<sub>I</sub>, aviso, viñetas | `#C8102E` | `accent1` — es el color que el patrón de diapositivas asigna a los títulos |
| El lote B —la solución nueva—, los caudales, las barras de purga | `#346583` | `accent3` |
| El lote A —la solución anterior— en la tubería y en el tanque A | `#B99B5E` y `#D5BF92` | `accent4` `#CEB888`, un paso oscuro |
| Rótulos del lote A sobre blanco | `#7A6326` | `accent4` bajado hasta 5,8:1 |
| Texto secundario, botones | `#505759` | `accent6` |
| Rótulos menores, filetes | `#898D8D` oscurecido a `#6E7373` | `accent2`, bajado un paso para que pase contraste a 11 px |
| Glut Station en uso, consumo normal | `#94A596` oscurecido a `#5E7263` | `dk2`, mismo ajuste |
| Fondo y tarjetas | `#F2F3F3` / `#FFFFFF` | tinte de `accent2` y `bg1` |

Dos colores del tema (`accent2` gris y `dk2` salvia) son demasiado claros para
texto pequeño sobre blanco —quedan en 3,2:1 y 2,6:1—, así que se usan un paso
más oscuros dentro de la misma familia. Los demás van tal cual.

La tipografía es **Arial** en todo, que es la única que define el tema
(`fontScheme` «Edwards 2015», major y minor). La jerarquía se hace con peso,
tamaño, caja y espaciado, no con familias distintas.

## De dónde salen los números

Volúmenes mínimos de flushing y etiquetas del POU más lejano, tomados de la
validación de cada cuarto:

| Cuarto | Volumen mínimo | POU más lejano | Fuente |
|---|---|---|---|
| Training | 120 L | `GLUT-TRN-POU #4` | DOC-0138892A |
| CR1 | 450 L | `CRCW-GLUT-CR1-POU-GST-06` | DOC-0185706A, DOC-0132700A |
| CR2 | 252 L | `CRCW-GLUT-CR2-POU-GST-06` | DOC-0185706A, DOC-0132700A |
| CR3 | 266 L | `CRCW-GLUT-CR3-POU-GST-04` | DOC-0216863A |
| CR4 | 295 L | `CRCW-GLUT-CR4-POU-GST-05` | DOC-0250796B, DOC-0565322A |
| **Total** | **1383 L** | | |

Son los mismos que recoge la tabla de volúmenes validados de
`Assessment_IEEE/assessment.tex`.

### Supuestos, que conviene revisar antes de presentarla

Estos valores **no son reales**: los escogí para que la campaña dé las dos horas
conocidas y para que la carátula del PT-100 sea coherente consigo misma. Están
declarados en el pie de la animación.

| Supuesto | Valor usado |
|---|---|
| Geometría de cada tanque | Cilindro vertical, 6000 L, Ø 1,80 m (área 2,545 m²) |
| Contenido al arrancar | Tanque B 4500 L de lote nuevo; tanque A 1850 L de lote anterior, aislado |
| Volumen de tubería hasta el último POU | Un tercio del volumen de flushing del cuarto: 40 L Training, 150 L CR1, 84 L CR2, 89 L CR3, 98 L CR4 |
| Reparto de ese volumen | 58 L de cabezal compartido (18 + 10 + 10 + 10 + 10) y el resto en cada ramal |
| Densidad de la solución | 1010 kg/m³ |
| Caudal de purga | 18 L/min |
| Preparación por estación | 4 min |
| Cierre y estabilización por estación | 2 min |
| Cierre inicial y reapertura de la línea | 10 min + 3 min |
| Consumo de una Glut Station en uso | Ráfagas de 3 a 6 L/min |

Con esos números: 77 min de purga efectiva + 30 min de preparación y lectura +
13 min de cierre y reapertura = **exactamente 2 h de línea cerrada**. Si los
tiempos reales son otros, se ajustan las constantes `SETUP`, `SETTLE`, `PREP` y
`RESTORE` al inicio del `<script>` y todo lo demás se recalcula solo.

El volumen de tubería sale de la regla habitual de purgar **tres volúmenes de
línea**, aplicada al revés: si a CR1 se le exigen 450 L, se supone que su línea
son 150 L. Es el supuesto más endeble de todos, porque el número real está en los
isométricos y no en la validación. Se cambia en el campo `line` de la tabla
`ROOMS` y en la constante `HSEG`, que es el reparto del cabezal; el desplazamiento
de cada purga se recalcula solo a partir de ahí.

En el escenario de hoy la línea está cerrada, así que el desplazamiento de cada
purga es exactamente el nominal. En el escenario con la aplicación los cuartos
siguen consumiendo, y ese consumo desplaza por su cuenta una parte del ramal
antes de que le toque el turno: por eso ahí puede salir lote B un poco antes de
lo que marca la columna del cronograma, que es siempre el valor nominal.

El transmisor tiene un ruido pequeño en la lectura, a propósito: un sensor de
presión sobre un tanque ancho no da un número perfectamente quieto, y eso ayuda a
ver que el volumen del tanque es una lectura indirecta.

## Para grabarla en video

No hace falta ninguna herramienta extra:

1. Poner el escenario y la velocidad que se quieran.
2. `Win + G` abre la Barra de juegos de Windows → *Capturar* → *Iniciar
   grabación*. El video queda en `Vídeos\Capturas`.

Si se necesita una versión sin la interfaz alrededor (solo el esquema), se puede
grabar la ventana en pantalla completa con `F11`.

## Qué falta / próximos pasos

- **Confirmar los supuestos de la tabla de arriba** con Gustavo, sobre todo las
  dimensiones de los tanques, el caudal de purga y los tiempos por estación.
- **Confirmar el volumen de tubería de cada ramal** contra los isométricos de la
  línea. Hoy está estimado como un tercio del volumen de flushing, y de ese
  número dependen los 329 L de desplazamiento que la animación muestra en el
  cronograma y en la tabla.
- **Confirmar el arreglo de los dos tanques**: si en planta el cambio de lote se
  hace con dos recipientes y un manifold, como está dibujado, o de otra forma.
- **Cuantificar el costo del paro**: dos horas de planta detenida cada dos días
  se traduce en un número de unidades o de horas-persona. Ese número es
  probablemente el argumento más fuerte del proyecto y hoy no aparece en la
  animación.
- **Mostrar la pantalla de la AMF dentro de la estación**: cronómetro, gráfica y
  sesión de medición, tal como quedaron especificados en el documento de
  requerimientos.
