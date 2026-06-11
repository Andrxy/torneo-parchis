# Reglas del juego — Torneo de Parchís (especificación canónica)

Este archivo es la **única fuente de verdad** de las reglas. Toda la lógica del juego debe ajustarse a
esto. Las ambigüedades del enunciado están resueltas aquí con un valor por defecto; donde diga
**(confirmar con profesora)** es una decisión que puede cambiarse en un solo punto del código.

> Importante: este refactor cambia SOLO la lógica del juego. NO se tocan los mecanismos de SO
> (procesos, hilos, memoria compartida, mutex, semáforos, pipes, colas, sockets, scheduler Round Robin,
> ni la liberación de IPC). El mutex por casilla y los semáforos de pasillo/meta se conservan.

---

## Estados de una ficha

- `EN_NIDO`: en la base, fuera del tablero.
- `EN_RECORRIDO`: en el circuito común.
- `EN_PASILLO`: en el pasillo final del propio color.
- `EN_META`: llegó a meta (no se mueve más).

"En juego" / "fuera del nido" = cualquier estado distinto de `EN_NIDO`.

---

## Modelo de ocupación de casilla (cambio estructural)

Una casilla ya **no** tiene un único ocupante. Pasa a tener una **lista de ocupantes** (color + id de
ficha), con una constante `MAX_OCUPANTES_CASILLA` (sugerido: 4). Reglas de ocupación:

- Casilla **normal**: vacía, 1 ficha, o 2 fichas del **mismo color** (barrera). Si llega una ficha de
  otro color a una casilla con 1 rival → captura (regla 5). No coexisten colores distintos en casilla normal.
- Casilla **seguro**: pueden coexistir fichas de **colores distintos** (no hay captura, regla 6). También
  puede formarse barrera (2 del mismo color).
- El `pthread_mutex_t` de la casilla (PROCESS_SHARED) sigue protegiendo TODA modificación de su lista de
  ocupantes. Una ficha sostiene a lo sumo un mutex de casilla a la vez (regla anti-deadlock vigente).

---

## Las 9 reglas

### 1. Salida con 5
Una ficha sale del nido solo con un **5**. Si al sacar un 5 el jugador tiene fichas en el nido, saca una a
su casilla de salida. Si la salida está ocupada por una barrera propia, no puede sacar (mueve 5 con otra
ficha, ver regla 2).

### 2. Un dado por turno, mover una ficha
Se lanza **un** dado por turno (salvo lo que añaden las reglas 3 y 4). El jugador avanza ese número con
**una** ficha disponible. "Disponible" = ficha fuera del nido que pueda hacer el movimiento legalmente
(o sacar una del nido si el dado es 5).

**Política de selección automática (decisión de simulación, determinista):** entre los movimientos
legales, elegir en este orden:
1. Un movimiento que **capture** una ficha rival.
2. Un movimiento que **meta una ficha en meta** (cuenta exacta).
3. Sacar una ficha del nido si el dado es 5 y hay fichas en el nido.
4. La ficha **más avanzada** (más cerca de meta) que pueda moverse.
Si ninguna ficha puede moverse legalmente, el turno pasa sin movimiento.

### 3. El 6 repite; tres seises → al nido
Si el dado es **6** (y cuenta como 6, ver regla 4), el jugador **vuelve a tirar** tras mover. Se llevan
los 6 consecutivos del turno: al **tercer 6 seguido**, la **última ficha movida** vuelve al nido y el
turno termina (ese tercer 6 no se juega).

### 4. Con 4 fichas fuera, el 6 vale 7
Cuando el jugador **no tiene fichas en el nido** (las 4 están fuera), un 6 se juega como **7**.
**(confirmar con profesora)** En ese caso el 6-convertido-en-7 **no** otorga repetición de tirada ni
cuenta para los "tres seises" (se trata como un 7 normal).

### 5. Captura → nido + avanzar 20
Si una ficha cae en una casilla ocupada por **una** ficha rival (en casilla NO seguro y que no sea
barrera), la **captura**: la rival vuelve a su nido. El jugador que capturó **avanza 20** con cualquier
ficha en juego (aplicando la política de selección de la regla 2). El avance de 20 es un movimiento
normal: puede a su vez capturar (otra vez +20) o entrar a meta (+10, regla 8). Si ninguna ficha puede
avanzar 20 legalmente, la bonificación se pierde.

### 6. Seguros: no hay captura
En las casillas **de seguro** no se captura: una ficha que cae en un seguro ocupado por rivales coexiste
con ellas (sin enviar a nadie al nido). Define el conjunto de seguros como una constante documentada
`SEGUROS[]` (sugerido: la casilla de salida de cada color y un conjunto fijo de casillas equiespaciadas).

### 7. Barreras
Dos fichas del **mismo color** en una misma casilla forman una **barrera**.
- Una barrera **bloquea**: ninguna ficha (propia o rival) puede **caer en** ni **pasar por encima** de la
  casilla con barrera. **(confirmar con profesora)** El movimiento que cruzaría una barrera es ilegal.
- La barrera **se deshace** cuando el jugador que la formó saca un **6**: ese turno está **obligado** a
  mover una de las fichas de la barrera, si el movimiento es legal. **(confirmar con profesora)**

### 8. Entrada a meta exacta + avanzar 10
Para entrar en meta se necesita el **número exacto**; si el dado pasa de meta, esa ficha no puede usar
ese valor (se mueve otra ficha según la regla 2). Cada ficha que **entra en meta** da derecho a avanzar
**10** con la ficha que el jugador elija (política regla 2). Ese avance de 10 es un movimiento normal
(puede capturar o meter otra ficha en meta, encadenando bonificaciones).

### 9. Victoria
Gana el **primer** jugador que coloca sus **4 fichas en meta**. El árbitro detecta la condición y termina
la partida (mensaje `FIN_JUEGO` por socket; liberación ordenada de IPC).

---

## Resumen de bonificaciones (movimientos extra)

| Evento | Bonificación | Encadena |
|---|---|---|
| Sacar 6 (cuenta como 6) | repetir tirada | sí, hasta 2 veces (3.er 6 penaliza) |
| Capturar una ficha | avanzar 20 | sí (puede capturar / meta) |
| Meter una ficha en meta | avanzar 10 | sí (puede capturar / meta) |

Todas las bonificaciones se aplican como movimientos protegidos normales (cada uno toma y suelta el mutex
de su casilla destino; nunca se sostienen dos mutex a la vez). Protege los encadenamientos con un límite
de seguridad para no entrar en bucles infinitos.

---

## Constantes nuevas a definir (documentadas)

- `MAX_OCUPANTES_CASILLA` (p.ej. 4)
- `SEGUROS[]` y su tamaño
- `BONO_CAPTURA = 20`, `BONO_META = 10`
- `MAX_SEISES = 3`
- `VALOR_SALIDA = 5`
