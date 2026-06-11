# Rúbrica explicada — Torneo de Parchís (auditoría para Claude Code)

Este archivo descompone los 100 puntos de la rúbrica en requisitos **verificables uno por uno**.
Úsalo así: recorre cada criterio, busca en el código dónde se cumple, y marca el estado con evidencia
(`archivo:función` o `archivo:línea`). Si algo no aparece o aparece a medias, queda como pendiente.

Formato de estado para cada sub-ítem:
- `[x]` cumplido, con evidencia.
- `[~]` parcial (explica qué falta).
- `[ ]` ausente.

Al final hay una **tabla de cierre** que debes rellenar antes de dar el proyecto por terminado.

---

## 1. Gestión de Procesos e Hilos — 15 pts

> *Uso correcto de fork() para crear los procesos de los jugadores y pthreads para manejar las 4 fichas
> de forma simultánea.*

Qué debe existir en el código:

- [ ] El árbitro (proceso padre) crea **exactamente 4 procesos** con `fork()`, uno por color (Rojo, Verde, Azul, Amarillo).
- [ ] Tras cada `fork()` se revisa el retorno: `< 0` error, `== 0` hijo, `> 0` padre (patrón de guia-procesos.md).
- [ ] El hijo (jugador) termina con `exit()`, no con un `return` que arrastre código del padre.
- [ ] Cada proceso jugador crea **4 hilos** con `pthread_create()`, uno por ficha, y hace `pthread_join()` de los 4.
- [ ] Los argumentos a cada hilo se pasan con un **struct** (no un puntero a variable de loop reutilizada).
- [ ] El padre recoge a los 4 hijos con `wait()`/`waitpid()` y captura su estado con `WEXITSTATUS`/`WIFEXITED`.
- [ ] No quedan procesos **zombie** ni hilos sin `join`.

Errores que descuentan:
- Un `fork()` dentro de un loop sin controlar el PID acaba creando 2^n procesos.
- Pasar `&i` del loop a `pthread_create` → todas las fichas leen el mismo valor.
- Falta de `-lpthread` al enlazar.

Verificación sugerida: contar 4 procesos hijo y 16 hilos en total; `ps`/`pstree` durante la ejecución;
el padre nunca queda colgado ni deja zombies.

---

## 2. Sincronización Avanzada — 20 pts

> *Implementación efectiva de Mutex para proteger cada casilla del tablero y Semáforos para controlar
> el acceso a la zona de meta y pasillos.*

Mutex por casilla:
- [ ] Cada casilla del tablero tiene su propio `pthread_mutex_t`.
- [ ] Como las casillas las disputan fichas de **procesos distintos**, los mutex viven en **memoria compartida**
      e inicializan con `pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED)`.
      (Un `pthread_mutex_t` normal NO sincroniza entre procesos: esto es lo más fácil de hacer mal.)
- [ ] El movimiento de una ficha (operación no atómica) está protegido: `lock` → leer/mover/capturar → `unlock`.
- [ ] No hay `sleep`/`usleep` largos dentro de una sección crítica.
- [ ] Todos los mutex se destruyen con `pthread_mutex_destroy` al terminar.

Semáforos en meta y pasillos:
- [ ] La zona de Meta y los pasillos estrechos están limitados por **semáforos** (no por mutex).
- [ ] Semáforos POSIX `sem_t` inicializados con `sem_init(&s, 1, CAPACIDAD)` (pshared=1) y colocados en
      memoria compartida, o equivalente System V justificado.
- [ ] Entrada con `sem_wait`, salida con `sem_post`. **Sin busy-wait.**
- [ ] La capacidad del semáforo refleja la regla (p.ej. 1 ficha por pasillo estrecho).
- [ ] `sem_destroy` (o `IPC_RMID`) al terminar.

Errores que descuentan:
- Usar un solo mutex global para todo el tablero (no es "por casilla").
- Mutex normal entre procesos (compila pero no sincroniza).
- Resolver el aforo con un mutex en vez de un semáforo contador.

Verificación: bajo concurrencia, ninguna casilla muestra dos fichas a la vez; nunca hay más fichas que
la capacidad en un pasillo/meta.

---

## 3. Comunicación IPC (Pipes y Colas) — 20 pts

> *Uso de Pipes para la comunicación directa padre-hijo y Colas de Mensajes para la notificación de
> eventos (como capturas de fichas) entre jugadores.*

Pipes (padre↔hijo):
- [ ] Se crea al menos un `pipe()` por jugador **antes** del `fork()`.
- [ ] Cada proceso cierra el extremo que no usa **inmediatamente después** del fork.
- [ ] Se usan para algo real: recolección de estadísticas/resultados finales de cada hijo.
- [ ] El lector lee hasta EOF (`read` devuelve 0) y el escritor cierra su extremo para señalarlo.
- [ ] Todos los extremos se cierran con `close()` en ambos lados.

Colas de mensajes (eventos):
- [ ] Cola System V creada con `msgget` (clave con `ftok` o fija), antes del fork si los hijos la usan.
- [ ] Struct de mensaje con `long mtype` como **primer campo** y `> 0`.
- [ ] Se envían eventos reales del juego (capturas/movimientos especiales) con `msgsnd`,
      usando `sizeof(cuerpo)` sin contar `mtype`.
- [ ] Alguien recibe con `msgrcv` y muestra el evento.
- [ ] La cola se elimina con `msgctl(IPC_RMID)` al terminar (verificable con `ipcs -q`).

Errores que descuentan:
- Pipes "decorativos" que no transportan nada útil.
- Olvidar cerrar `fds[1]` → el lector se cuelga esperando EOF.
- Dejar la cola huérfana en el kernel (no llamar `IPC_RMID`).

Verificación: las estadísticas finales llegan por pipe y cuadran; las capturas generan mensajes que se
ven en pantalla/log; `ipcs -q` queda limpio tras salir.

---

## 4. Comunicación por Red (Sockets) — 15 pts

> *Implementación funcional de Sockets para el envío de mensajes de control y turnos desde el árbitro
> hacia los procesos jugadores.*

- [ ] El árbitro levanta un servidor TCP: `socket` → `setsockopt(SO_REUSEADDR)` → `bind` → `listen` → `accept`.
- [ ] Cada jugador se conecta como cliente con `socket` → `connect`.
- [ ] El árbitro envía por socket mensajes de **control/turno** (p.ej. "TU_TURNO", "FIN_JUEGO").
- [ ] El jugador responde por el mismo canal (p.ej. "TURNO_OK").
- [ ] Se manejan lecturas parciales de `recv` (leer exactamente lo esperado).
- [ ] Todos los sockets se cierran con `close()` (conexión y socket de escucha).

Errores que descuentan:
- "Sockets" que en realidad no transmiten control de turnos (solo un saludo inicial).
- No usar `SO_REUSEADDR` → `bind: Address already in use` al reiniciar.
- Asumir que un `recv` trae el mensaje completo.

Verificación: los turnos viajan por socket; si se apaga el árbitro los jugadores lo detectan; el puerto
se reutiliza sin error entre corridas.

---

## 5. Memoria Compartida — 15 pts

> *Gestión del estado global del tablero mediante un segmento de memoria compartida (shmget o mmap)
> accesible para todos los procesos.*

- [ ] El estado del tablero (casillas, posiciones de las 16 fichas, contadores) vive en memoria compartida.
- [ ] Se usa `mmap(MAP_SHARED | MAP_ANONYMOUS)` antes del fork, o `shmget`/`shmat`.
- [ ] **Todos** los procesos ven y modifican el mismo tablero (no copias privadas).
- [ ] El acceso concurrente al tablero está protegido por los mutex/semáforos del criterio 2.
- [ ] La memoria se libera: `munmap` (mmap) o `shmdt` + `shmctl(IPC_RMID)` (System V).

Errores que descuentan:
- Declarar el tablero como global "normal" creyendo que el fork lo comparte (cada proceso obtiene su copia).
- Crear la memoria compartida **después** del fork (los hijos no la heredan).
- Dejar el segmento System V huérfano (`ipcs -m`).

Verificación: un cambio hecho por un hijo es visible por el padre/otros hijos; `ipcs -m` limpio tras salir
(si se usó System V).

---

## 6. Planificación (Scheduler) — 10 pts

> *Implementación lógica del algoritmo Round Robin (o sistema de prioridades) por parte del proceso
> árbitro para la asignación de turnos.*

- [ ] El árbitro actúa como scheduler y reparte los turnos.
- [ ] Hay un **Round Robin** real: los 4 jugadores reciben el turno en orden cíclico y equitativo.
- [ ] El turno se entrega por el canal de control (socket) y el jugador confirma su fin de turno.
- [ ] Existe una condición de fin del juego clara (un color mete sus 4 fichas en meta, o máximo de rondas).
- [ ] (Opcional, mencionado en el enunciado) prioridades dinámicas: quantum más largo para quien está
      cerca de ganar. Si no se implementa, al menos dejar comentado dónde encajaría.

Errores que descuentan:
- "Turnos" que en realidad corren todos los jugadores a la vez sin control del árbitro.
- Orden no cíclico o que se salta jugadores.

Verificación: la traza muestra rotación R→V→A→Am→R…; ningún jugador acapara turnos.

---

## 7. Robustez y Limpieza — 5 pts

> *Ausencia de interbloqueos (Deadlocks), manejo de condiciones de carrera y liberación correcta de
> todos los recursos IPC al finalizar el programa.*

- [ ] Estrategia anti-deadlock explícita y documentada (p.ej. sostener un solo mutex a la vez, o un orden
      fijo de adquisición si se toman varios).
- [ ] No hay condiciones de carrera: toda escritura compartida está protegida.
- [ ] Manejador de señales (SIGINT/SIGTERM) que usa `volatile sig_atomic_t` y hace la limpieza en el flujo
      principal — **no** llama funciones async-signal-unsafe (mutex, printf) dentro del handler.
- [ ] Una rutina de limpieza centralizada libera TODO: sockets, pipes, semáforos (`sem_destroy`),
      mutex (`pthread_mutex_destroy`), cola (`msgctl IPC_RMID`), memoria (`munmap`/`shmctl`), e hijos.
- [ ] La limpieza corre tanto en salida normal como por señal.

Verificación: `Ctrl+C` deja el sistema limpio; `ipcs` sin colas/semáforos/segmentos huérfanos; varias
corridas seguidas sin colgarse; idealmente sin fugas en valgrind.

---

## Entregables (no son puntos de rúbrica pero son obligatorios)

- [ ] **Makefile** con indentación TAB, `-Wall -Wextra` sin warnings, targets útiles (`all`, `clean`, `run`).
- [ ] **Código C estándar** organizado.
- [ ] **Informe técnico** con: diagrama de procesos e hilos, explicación anti-deadlock, justificación del
      scheduler, descripción de los mecanismos de comunicación.
- [ ] **Salida ASCII art** del tablero mostrando el avance de las fichas en tiempo real.

---

## Tabla de cierre — rellenar antes de entregar

| # | Criterio | Pts | Estado | Evidencia (archivo:función) | Pendiente |
|---|----------|-----|--------|------------------------------|-----------|
| 1 | Procesos e Hilos | 15 | | | |
| 2 | Sincronización (mutex + semáforos) | 20 | | | |
| 3 | IPC Pipes + Colas | 20 | | | |
| 4 | Sockets | 15 | | | |
| 5 | Memoria compartida | 15 | | | |
| 6 | Scheduler Round Robin | 10 | | | |
| 7 | Robustez y limpieza | 5 | | | |
| — | Makefile / Informe / ASCII art | — | | | |
| | **TOTAL** | **100** | | | |

### Cómo usar esta tabla en Claude Code

```
Audita el proyecto actual contra docs/RUBRICA.md. Para CADA sub-ítem de cada criterio, indica si está
cumplido, parcial o ausente, con la evidencia exacta (archivo:función o archivo:línea). No asumas que algo
existe: búscalo en el código. Al final completa la "Tabla de cierre" con el estado de cada criterio y la
lista concreta de lo que falta. No cambies código todavía, solo reporta.
```
