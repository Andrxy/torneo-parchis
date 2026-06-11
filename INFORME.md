# Informe técnico — Torneo de Parchís (GRUPO 5)

Este informe describe **lo que está implementado en el código actual** tras el refactor de reglas.
Cada afirmación apunta al archivo y función donde se realiza.

---

## 1. Diagrama de procesos e hilos

El árbitro (proceso padre) crea los 4 jugadores con `fork()` (`main.c:main`). Cada jugador crea sus
4 hilos-ficha con `pthread_create` (`jugador.c:jugador_ejecutar`). El árbitro además levanta un hilo
consumidor de eventos.

```
Árbitro  (proceso padre)                         main.c
│
├─ hilo principal ......... scheduler Round Robin (sockets), render ASCII, liberación de IPC
├─ hilo consumidor_capturas ... msgrcv de la cola → panel de eventos          main.c:consumidor_capturas
│
├─ fork → Jugador ROJO     (proceso)                                          main.c:main
│         ├─ hilo principal ... cliente TCP, bucle de turno, política, bonos  jugador.c:jugador_ejecutar
│         ├─ hilo ficha 0 ..... espera su turno, mueve, avisa                 ficha.c:ficha_hilo
│         ├─ hilo ficha 1
│         ├─ hilo ficha 2
│         └─ hilo ficha 3
├─ fork → Jugador VERDE    (idéntico: 1 hilo principal + 4 hilos-ficha)
├─ fork → Jugador AZUL     (idéntico)
└─ fork → Jugador AMARILLO (idéntico)
```

Recuento: **5 procesos** (1 árbitro + 4 jugadores) y **22 hilos** (árbitro: principal + consumidor;
cada jugador: principal + 4 fichas = 5). El padre recoge a los hijos con `waitpid` y une todos los
hilos con `pthread_join` (`jugador.c:jugador_ejecutar`, `main.c:liberar_recursos`); no quedan zombies
ni hilos sin `join`.

Dentro de cada jugador, **solo la ficha elegida en el turno se despierta**: hay un semáforo POSIX por
ficha (`sem_turno[FICHAS_POR_JUGADOR]`) y uno compartido de fin de jugada (`sem_listo`), inicializados
en `jugador.c:jugador_ejecutar`. El jugador postea únicamente el semáforo de la ficha seleccionada y
espera `sem_listo`; las otras tres siguen bloqueadas (sin busy-wait).

---

## 2. Reglas del juego implementadas

Las 9 reglas de `docs/docs/REGLAS.md`, tal como están en el código:

| # | Regla | Dónde |
|---|-------|-------|
| 1 | **Salida con 5.** Una ficha sale del nido a su casilla de salida solo con un 5. | `ficha.c:puede_mover` (`dado==5`) y `ficha.c:realizar_movimiento` (bloque `EN_BASE`) |
| 2 | **Un dado por turno + selección de ficha.** Se tira un dado y se mueve una sola ficha según la política determinista: capturar > meta > sacar-con-5 > más avanzada. | Tirada única en `jugador.c:jugador_ejecutar`; política en `ficha.c:elegir_ficha` |
| 3 | **El 6 repite; tres seises → al nido.** Tras mover con 6 se vuelve a tirar; al tercer 6 seguido la última ficha movida vuelve al nido y el turno acaba sin jugar ese 6. | `jugador.c:jugador_ejecutar` (`seises`, `MAX_SEISES`, `ACCION_AL_NIDO`); vuelta al nido en `ficha.c:ficha_al_nido` |
| 4 | **Con 4 fichas fuera, el 6 vale 7.** Si no hay fichas en el nido, el 6 se juega como 7 y no repite ni cuenta como seis. | `jugador.c:jugador_ejecutar` (`hay_en_nido`, `valor = 7`, `es_seis = 0`) |
| 5 | **Captura → nido + avanzar 20.** Caer sobre una única ficha rival (casilla normal, no seguro, no barrera) la manda al nido y publica el evento por la cola; luego se avanza 20 con la política. | Captura y evento en `ficha.c:realizar_movimiento` (rama `DEST_COMUN`, `msgsnd`); +20 en `jugador.c:jugador_ejecutar` (`BONO_CAPTURA`) |
| 6 | **Seguros: sin captura, coexisten colores.** En las casillas de `CASILLAS_SEGURO[]` no se captura; fichas de distinto color coexisten en la lista de ocupantes. | `ficha.c:landing_legal` / `ficha.c:es_seguro`; captura vetada con `!seguro` en `ficha.c:realizar_movimiento` |
| 7 | **Barreras.** Dos fichas del mismo color forman barrera; bloquea caer en y **pasar por encima** de esa casilla; se rompe cuando el jugador saca un 6 (obligado a mover una ficha de la barrera si es legal). | Detección `ficha.c:casilla_tiene_barrera`; verificación de camino `ficha.c:camino_bloqueado`; obligación `ficha.c:elegir_ficha_rompe_barrera` + `jugador.c:jugador_ejecutar` |
| 8 | **Entrada a meta exacta + avanzar 10.** Se entra a meta solo con el número exacto; si se pasa, la ficha no usa ese valor. Cada ficha que entra da derecho a avanzar 10. | Cálculo exacto/overshoot `ficha.c:calc_destino` (`DEST_META` / `DEST_ILEGAL`); ejecución `ficha.c:realizar_movimiento`; +10 en `jugador.c:jugador_ejecutar` (`BONO_META`) |
| 9 | **Victoria.** Gana el primero que mete sus 4 fichas en meta; el árbitro lo detecta, anuncia `FIN_JUEGO` y libera la IPC. | `main.c:main` (`fichas_en_meta == FICHAS_POR_JUGADOR`, `MSG_FIN_JUEGO`, `liberar_recursos`) |

Los bonos de captura (20) y de meta (10) se aplican **de forma encadenada** con un tope de seguridad
(`MAX_BONOS_ENCADENADOS`): un +20 o +10 puede a su vez capturar o entrar a meta y volver a bonificar
(`jugador.c:jugador_ejecutar`).

---

## 3. Modelo de ocupación del tablero

Una casilla ya **no** tiene un único ocupante: aloja una **lista de ocupantes**
(`Casilla { Ocupante ocupantes[MAX_OCUPANTES_CASILLA]; int n_ocupantes; pthread_mutex_t mutex; }`,
`parchis.h`). Reglas de ocupación implementadas:

- **Casilla normal:** vacía, 1 ficha, o 2 del **mismo color** (barrera). No coexisten colores distintos:
  caer sobre un único rival lo captura (regla 5).
- **Casilla de seguro:** coexisten fichas de **colores distintos** sin captura (regla 6); también puede
  formarse barrera del mismo color.

**Toda** modificación de la lista (agregar, quitar, capturar) ocurre bajo el `mutex` de la casilla. Las
operaciones sobre la lista están en `ficha.c`: `casilla_agregar`, `casilla_quitar`, `contar_color` y
`casilla_tiene_barrera`. El tablero completo (casillas, pasillos, las 16 fichas, contadores, semáforos
y mutex) vive en memoria compartida `mmap(MAP_SHARED | MAP_ANONYMOUS)` creada **antes** del `fork`
(`tablero.c:tablero_crear` / `tablero.c:tablero_init`), por lo que todos los procesos ven el mismo
estado.

---

## 4. Concurrencia y cómo se evita el interbloqueo

Con **un dado por turno** se mueve una sola ficha, así que el paralelismo entre las 4 fichas de un
jugador es bajo (solo una despierta por jugada). Aun así la sincronización es necesaria por el acceso
concurrente **lector/escritor** sobre el tablero compartido:

- El **árbitro** lee el tablero: dibuja el ASCII tras cada turno (`tablero.c:tablero_dibujar`) y corre
  el hilo `consumidor_capturas` que recibe eventos de la cola.
- El **proceso jugador activo** escribe el tablero a través de su hilo-ficha
  (`ficha.c:realizar_movimiento`), y un mismo turno puede encadenar varias jugadas por las
  **bonificaciones (20 y 10)**, cada una tomando de nuevo el mutex de la casilla destino.

Como la mutación de la lista de ocupantes (y la captura, que toca dos fichas) **no es atómica**, se
protege con el mutex `PROCESS_SHARED` de la casilla; los contadores y la impresión se protegen con
`mutex_impresion`.

**Estrategia anti-deadlock — un solo mutex de casilla a la vez:**

- Una ficha nunca sostiene dos mutex de casilla simultáneamente. El movimiento toma el mutex del
  **destino**, y la casilla de **origen** (que queda quieta durante el turno serializado) se actualiza
  bajo ese mismo mutex (`ficha.c:realizar_movimiento`).
- La verificación de barreras en el camino lee cada casilla con un **bloqueo corto e independiente**
  (lock → leer → unlock), sin anidar (`ficha.c:camino_bloqueado`).
- Las operaciones de semáforo (pasillo/meta) se hacen **fuera** de cualquier sección con mutex de
  casilla. La entrada al pasillo usa `sem_trywait` (no bloqueante) para no quedar bloqueado mientras se
  decide la jugada (`ficha.c:realizar_movimiento`).
- No hay busy-wait: las fichas bloquean en `sem_wait`, el jugador en `recv`/`sem_wait`, el árbitro en
  `accept`/`recv`.

Los semáforos POSIX del tablero reflejan los aforos: `sem_pasillo[color]` con capacidad 1 (una ficha
por pasillo) y `sem_meta` con `AFORO_META` (`tablero.c:tablero_init`), inicializados con `pshared=1`.

---

## 5. Algoritmo de planificación (scheduler) y su justificación

El árbitro actúa como scheduler con un **Round Robin** puro: reparte el turno en orden cíclico
R → V → A → Am con `siguiente = (siguiente + 1) % NUM_JUGADORES` (`main.c:main`). El turno se entrega
por socket (`MSG_TU_TURNO`) y el jugador confirma su fin con `MSG_TURNO_OK` antes de pasar al
siguiente. La partida termina cuando un color mete sus 4 fichas en meta o se alcanza `MAX_RONDAS`.

**Por qué Round Robin:** el parchís es un juego por turnos donde todos los jugadores deben tener la
misma oportunidad; el Round Robin es **equitativo y sin inanición** por construcción, encaja de forma
natural con la rotación de turnos del juego y es el más simple de razonar y depurar. Un esquema de
prioridades habría añadido complejidad sin beneficio para la equidad buscada.

---

## 6. Mecanismos de comunicación utilizados

| Mecanismo | Sentido | Qué transporta | Implementación |
|-----------|---------|----------------|----------------|
| **Sockets TCP** | árbitro ↔ jugador | control de turno: `TU_TURNO`, `TURNO_OK`, `FIN_JUEGO` (+ color inicial) | servidor en `main.c:main` (`socket`/`SO_REUSEADDR`/`bind`/`listen`/`accept`); cliente en `jugador.c:jugador_ejecutar` (`connect`); lecturas exactas con `leer_exacto` |
| **Memoria compartida (mmap)** | todos los procesos | tablero: casillas, pasillos, 16 fichas, contadores, semáforos y mutex | `tablero.c:tablero_crear` (`MAP_SHARED \| MAP_ANONYMOUS`), `tablero.c:tablero_init` |
| **Cola de mensajes System V** | hilo-ficha → árbitro | eventos de captura | `msgget`/`ftok` antes del fork (`main.c:main`); `msgsnd` en `ficha.c:realizar_movimiento`; `msgrcv` en `main.c:consumidor_capturas` |
| **Pipes** | jugador → árbitro | estadísticas finales de cada jugador (`EstadisticasJugador`) | `pipe` por jugador antes del fork (`main.c:main`); `write` en `jugador.c:jugador_ejecutar`; lectura hasta EOF en `main.c:leer_pipe` |
| **Semáforos POSIX (tablero)** | entre procesos | aforo de pasillo (1) y meta | `tablero.c:tablero_init`, uso en `ficha.c:realizar_movimiento` |
| **Semáforos POSIX (jugador)** | jugador ↔ sus hilos | despertar la ficha elegida / avisar fin de jugada | `jugador.c:jugador_ejecutar` (`sem_turno[]`, `sem_listo`) |
| **Mutex PROCESS_SHARED** | entre procesos | exclusión por casilla y de impresión/contadores | `tablero.c:init_casilla`, `tablero.c:tablero_init` |

La cola se crea antes del `fork` (los hijos heredan el `msq_id`), el struct de mensaje lleva
`long mtype` como primer campo (`parchis.h:MsgCaptura`) y se elimina con `msgctl(IPC_RMID)` al terminar
(`main.c:liberar_recursos`). Los pipes cierran sus extremos no usados justo tras el fork.

---

## 7. Salida por terminal: ASCII Art en tiempo real

Tras cada turno el árbitro redibuja el tablero en la misma terminal (`tablero.c:tablero_dibujar`):
limpia la pantalla, dibuja el aspa con las casillas comunes y los cuatro pasillos, colorea cada ficha
por su color, marca las casillas de seguro con `+`, muestra el contador de fichas en meta y un panel de
**capturas recientes** alimentado por el hilo consumidor. Al acabar la partida imprime la tabla de
estadísticas finales (metas, capturas hechas/sufridas y movimientos) recibidas por los pipes.

---

## 8. Robustez y liberación de recursos

El manejador de señales (SIGINT/SIGTERM) solo fija una bandera `volatile sig_atomic_t`
(`main.c:manejador_senal`); la limpieza la hace el flujo principal, no el handler. La rutina
`main.c:liberar_recursos` libera **todo** —sockets, pipes, cola (`IPC_RMID`), hilos, y vía
`tablero.c:tablero_destruir` los mutex (`pthread_mutex_destroy`), semáforos (`sem_destroy`) y la memoria
(`munmap`)— y corre tanto en salida normal como por señal. Tras ejecutar, `ipcs` queda limpio (sin
colas, semáforos ni segmentos huérfanos).

---

## 9. Compilación y ejecución

```bash
make        # compila bin/parchis con -Wall -Wextra (sin warnings)
make run    # compila si hace falta y lanza la partida
make clean  # borra bin/
```

Se ejecuta en **una sola terminal**: el árbitro hace `fork` de los 4 jugadores; no hay que lanzar
clientes aparte. El tablero se redibuja tras cada turno y, al finalizar, se imprime la tabla de
estadísticas. **Ctrl + C** detiene la partida liberando toda la IPC antes de salir.
