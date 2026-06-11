# Guía: Algoritmo de Peterson

## ¿Qué es el algoritmo de Peterson?

El algoritmo de Peterson es una solución de software para el problema de la exclusión mutua entre **dos procesos o hilos**. No requiere instrucciones especiales del hardware ni llamadas al SO. Usa solo dos variables compartidas: `flag[]` (intención de entrar) y `turn` (a quién le toca en caso de empate).

Es un ejemplo histórico y educativo — en hardware moderno, el reordenamiento de instrucciones del compilador y la CPU puede romperlo sin barreras de memoria (`volatile` o `memory fence`).

## Variables compartidas necesarias

```c
#define N 2           /* número de procesos — solo funciona con 2 */

volatile int flag[N]; /* flag[i] = 1: proceso i quiere entrar */
volatile int turn;    /* a quién le cede el paso en caso de conflicto */
```

`volatile` indica al compilador que no cache estas variables en registros, porque otro hilo/proceso las modifica.

## Funciones del patrón

```c
/* Protocolo de entrada a la sección crítica */
void enter_region(int process) {
    int other = 1 - process;   /* el otro proceso (0→1, 1→0) */

    flag[process] = 1;         /* 1. Anunciar intención de entrar */
    turn = other;              /* 2. Ceder el turno al otro */

    /* 3. Esperar si el otro quiere entrar Y es su turno */
    while (flag[other] && turn == other)
        ;   /* busy-wait */
}

/* Protocolo de salida de la sección crítica */
void leave_region(int process) {
    flag[process] = 0;         /* anunciar que ya no se quiere entrar */
}
```

## Cómo usarlo

```c
/* Proceso 0 */
enter_region(0);
/* --- sección crítica --- */
leave_region(0);

/* Proceso 1 (en paralelo) */
enter_region(1);
/* --- sección crítica --- */
leave_region(1);
```

## Ejemplo completo con hilos

```c
#include <stdio.h>
#include <pthread.h>

#define N 2

volatile int flag[N] = {0, 0};
volatile int turn = 0;

void enter_region(int process) {
    int other = 1 - process;
    flag[process] = 1;
    turn = other;
    while (flag[other] && turn == other)
        ;
}

void leave_region(int process) {
    flag[process] = 0;
}

int contador = 0;

void *trabajo(void *arg) {
    int id = *(int *)arg;
    for (int i = 0; i < 100000; i++) {
        enter_region(id);
        contador++;           /* sección crítica */
        leave_region(id);
    }
    return NULL;
}

int main(void) {
    pthread_t hilos[N];
    int ids[N] = {0, 1};

    for (int i = 0; i < N; i++)
        pthread_create(&hilos[i], NULL, trabajo, &ids[i]);
    for (int i = 0; i < N; i++)
        pthread_join(hilos[i], NULL);

    printf("Contador: %d (esperado: 200000)\n", contador);
    return 0;
}
```

## Por qué funciona — las tres propiedades

| Propiedad              | Garantía                                                      |
|------------------------|---------------------------------------------------------------|
| **Exclusión mutua**    | El while asegura que solo uno esté dentro si ambos compiten   |
| **Progreso**           | Si uno no quiere entrar (`flag=0`), el otro pasa sin esperar  |
| **Espera acotada**     | El que cedió el turno (`turn = other`) entra en la siguiente ronda |

## Análisis del bucle de espera

```c
while (flag[other] && turn == other)
    ;
```

- `flag[other] == 0`: el otro no quiere entrar → paso libre.
- `turn == process`: fui yo quien cedió, así que es mi turno → paso libre.
- Ambas condiciones verdaderas: el otro quiere entrar y es su turno → espero.

## Limitaciones

| Limitación                       | Explicación                                                    |
|----------------------------------|----------------------------------------------------------------|
| Solo funciona con 2 procesos     | Para N procesos se necesita el algoritmo de la panadería (Bakery algorithm) |
| Busy-wait (espera activa)        | Consume CPU mientras espera; los semáforos y mutexes bloquean el hilo sin gastar CPU |
| Frágil en hardware moderno       | CPUs y compiladores reordenan instrucciones; necesita `volatile` y barreras de memoria (`__sync_synchronize()`) |
| No recomendado en producción     | Usar `pthread_mutex_t` o semáforos POSIX para código real      |

## Diferencia con busy-wait de Peterson y semáforos

```
Peterson (busy-wait):
  while (condicion) ;   ← la CPU sigue ejecutando instrucciones vacías

Semáforo / mutex:
  sem_wait() o pthread_mutex_lock() → el SO suspende el hilo
                                      y lo despierta cuando puede continuar
```

El busy-wait desperdicia ciclos de CPU. Solo es aceptable si la espera es muy corta (nanosegundos), como en spinlocks de kernel.

## Uso con OpenMP (como en el ejemplo del curso)

```c
#pragma omp parallel for num_threads(N)
for (int i = 0; i < N; i++) {
    enter_region(i);
    critical_section(i);
    leave_region(i);
}
```

Compilar con: `gcc peterson.c -o peterson -fopenmp`

## Resumen del patrón

```
Inicialización: flag[0] = flag[1] = 0; turn = 0;

Proceso i quiere entrar:
  1. flag[i] = 1          ← "quiero entrar"
  2. turn = j (el otro)   ← "te cedo el paso"
  3. while (flag[j] && turn == j) ;   ← esperar si el otro tiene prioridad

Proceso i sale:
  1. flag[i] = 0          ← "ya no quiero entrar"
```
