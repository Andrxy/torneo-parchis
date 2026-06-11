# Guía: Semáforos

Existen dos APIs de semáforos en Linux/POSIX. Elegir según el caso:

| API          | Cuándo usarla                          | Header           |
|--------------|----------------------------------------|------------------|
| **POSIX**    | Sincronización entre **hilos** (o procesos con memoria compartida) | `<semaphore.h>` |
| **System V** | Sincronización entre **procesos** independientes | `<sys/sem.h>`   |

---

## Parte 1: Semáforos POSIX (`sem_t`)

### Headers

```c
#include <semaphore.h>  /* sem_t, sem_init, sem_wait, sem_post, sem_destroy */
#include <pthread.h>    /* para los hilos que usan el semáforo */
#include <stdio.h>
```

Compilar con: `gcc programa.c -o programa -lpthread`

### Llamadas en orden

```c
sem_t mutex;

sem_init(&mutex, 0, 1);   /* 1. Inicializar: pshared=0 (entre hilos), valor inicial=1 */

/* --- dentro del hilo --- */
sem_wait(&mutex);          /* 2. P() — decrementa; bloquea si valor == 0 */
/* sección crítica */
sem_post(&mutex);          /* 3. V() — incrementa; despierta a quien espera */

sem_destroy(&mutex);       /* 4. Destruir al terminar */
```

### Parámetros de `sem_init`

```c
int sem_init(sem_t *sem, int pshared, unsigned int value);
```

| Parámetro | Significado                                                  |
|-----------|--------------------------------------------------------------|
| `sem`     | Puntero al semáforo                                          |
| `pshared` | `0` = entre hilos del mismo proceso; `1` = entre procesos (requiere memoria compartida) |
| `value`   | Valor inicial (`1` = binario/mutex; `N` = permite N accesos simultáneos) |

### Ejemplo mínimo — proteger variable compartida

```c
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>

int variable_global = 0;
sem_t mutex;

void *hilo(void *arg) {
    sem_wait(&mutex);
    variable_global++;
    sem_post(&mutex);
    return NULL;
}

int main(void) {
    sem_init(&mutex, 0, 1);

    pthread_t h1, h2;
    pthread_create(&h1, NULL, hilo, NULL);
    pthread_create(&h2, NULL, hilo, NULL);
    pthread_join(h1, NULL);
    pthread_join(h2, NULL);

    printf("Valor final: %d\n", variable_global);  /* siempre 2 */
    sem_destroy(&mutex);
    return 0;
}
```

### Patrón productor-consumidor con 3 semáforos

```c
#define BUFFER_SIZE 5

int buffer[BUFFER_SIZE];
sem_t mutex;   /* acceso exclusivo al buffer */
sem_t empty;   /* cuenta los espacios vacíos  */
sem_t full;    /* cuenta los espacios llenos  */

/* Inicialización */
sem_init(&mutex, 0, 1);
sem_init(&empty, 0, BUFFER_SIZE);  /* buffer empieza vacío */
sem_init(&full,  0, 0);

/* Productor */
sem_wait(&empty);    /* espera espacio libre */
sem_wait(&mutex);
buffer[i] = item;   /* insertar en buffer */
sem_post(&mutex);
sem_post(&full);     /* avisa que hay un elemento listo */

/* Consumidor */
sem_wait(&full);     /* espera que haya algo */
sem_wait(&mutex);
item = buffer[i];   /* retirar del buffer */
sem_post(&mutex);
sem_post(&empty);    /* libera un espacio */
```

### Semáforo como señal (valor inicial = 0)

```c
sem_t listo;
sem_init(&listo, 0, 0);   /* empieza en 0 → el consumer se bloqueará */

/* Hilo A produce algo */
sem_post(&listo);          /* señala que terminó */

/* Hilo B espera la señal */
sem_wait(&listo);          /* se desbloquea cuando A hace post */
```

---

## Parte 2: Semáforos System V

### Headers

```c
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <stdio.h>
#include <stdlib.h>
```

### Llamadas en orden

```c
/* 1. Obtener/crear el conjunto de semáforos */
int sem_id = semget(KEY, 1, IPC_CREAT | 0666);

/* 2. Inicializar el valor del semáforo */
semctl(sem_id, 0, SETVAL, 1);

/* 3. Operación P (wait/down) — entra a sección crítica */
struct sembuf wait_op = { .sem_num = 0, .sem_op = -1, .sem_flg = 0 };
semop(sem_id, &wait_op, 1);

/* --- sección crítica --- */

/* 4. Operación V (signal/up) — sale de sección crítica */
struct sembuf signal_op = { .sem_num = 0, .sem_op = 1, .sem_flg = 0 };
semop(sem_id, &signal_op, 1);

/* 5. Eliminar el semáforo del sistema al terminar */
semctl(sem_id, 0, IPC_RMID, 0);
```

### Generar la clave con ftok vs IPC_PRIVATE

```c
/* clave derivada de un archivo existente — reproducible entre procesos */
key_t key = ftok("/ruta/archivo", 'X');
int sem_id = semget(key, 1, IPC_CREAT | 0666);

/* clave privada — solo útil si se hereda el sem_id por fork */
int sem_id = semget(IPC_PRIVATE, 1, 0666);
```

### La estructura sembuf

```c
struct sembuf {
    short sem_num;  /* índice del semáforo en el conjunto (0 si hay 1 solo) */
    short sem_op;   /* -1 = wait (P), +1 = signal (V) */
    short sem_flg;  /* 0 normal; SEM_UNDO = deshacer si el proceso muere */
};
```

`SEM_UNDO` es útil para que el SO libere el semáforo automáticamente si el proceso falla abruptamente.

### Wrapper conveniente (como en semaphores.h)

```c
void semwait(int semid) {
    struct sembuf s = { 0, -1, SEM_UNDO };
    semop(semid, &s, 1);
}

void semsignal(int semid) {
    struct sembuf s = { 0, +1, SEM_UNDO };
    semop(semid, &s, 1);
}

int createsem(int key, int value) {
    int semid = semget(key, 1, 0666 | IPC_CREAT);
    semctl(semid, 0, SETVAL, value);
    return semid;
}

void erasesem(int semid) {
    semctl(semid, 0, IPC_RMID, 0);
}
```

### Ejemplo con fork y sección crítica

```c
int semexmut = createsem(0x1234, 1);

pid_t p = fork();
if (p == 0) {
    semwait(semexmut);
    printf("Hijo en sección crítica\n");
    semsignal(semexmut);
    exit(0);
} else {
    semwait(semexmut);
    printf("Padre en sección crítica\n");
    semsignal(semexmut);
    wait(NULL);
    erasesem(semexmut);   /* solo el padre elimina el semáforo */
}
```

---

## Errores típicos

| Error                    | Causa probable                                                        |
|--------------------------|-----------------------------------------------------------------------|
| `semget: No space left`  | Límite de semáforos del sistema alcanzado; ver `ipcs -s` y `ipcrm`  |
| Semáforo queda en el SO  | No se llamó `semctl(id, 0, IPC_RMID)` al terminar; limpiar con `ipcrm -s <semid>` |
| Deadlock                 | Dos procesos se esperan mutuamente con `semwait`                      |
| `semop: Interrupted system call` | Una señal interrumpió la espera; reintentar o manejar `EINTR` |
| Valor inicial incorrecto | Olvidar `SETVAL` después de `semget`; el valor inicial es indefinido  |

## Comandos de diagnóstico

```bash
ipcs -s          # listar semáforos activos en el sistema
ipcrm -s <semid> # eliminar un semáforo manualmente
```

## Resumen de diferencias

| Característica     | POSIX (`sem_t`)          | System V (`semget`)           |
|--------------------|--------------------------|-------------------------------|
| Entre hilos        | Sí (pshared=0)           | No recomendado                |
| Entre procesos     | Con memoria compartida   | Sí (clave IPC)                |
| Persistencia       | Se destruye con proceso  | Persiste hasta `IPC_RMID`     |
| Inicialización     | `sem_init`               | `semget` + `semctl(SETVAL)`   |
| Operaciones        | `sem_wait` / `sem_post`  | `semop` con `sembuf`          |
| Limpieza           | `sem_destroy`            | `semctl(IPC_RMID)`            |
