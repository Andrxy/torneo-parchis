# Guía: Procesos con fork()

## ¿Qué es un proceso?

Un proceso es un programa en ejecución con su propio espacio de memoria. En POSIX, `fork()` crea un proceso hijo que es una copia exacta del padre. Ambos continúan ejecutando desde el punto del `fork()`.

## Headers necesarios

```c
#include <unistd.h>    /* fork(), getpid(), getppid(), sleep() */
#include <sys/types.h> /* pid_t */
#include <sys/wait.h>  /* wait(), waitpid(), WEXITSTATUS() */
#include <stdlib.h>    /* exit() */
#include <signal.h>    /* kill(), SIGKILL, SIGTERM */
#include <stdio.h>     /* printf(), perror() */
```

## Llamadas en orden — patrón mínimo

```c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

int main(void) {
    pid_t pid = fork();   /* 1. Crear proceso hijo */

    if (pid < 0) {        /* 2. Revisar error */
        perror("fork");
        exit(1);
    }

    if (pid == 0) {
        /* --- CÓDIGO DEL HIJO --- */
        printf("Hijo: mi PID es %d, padre es %d\n", getpid(), getppid());
        exit(0);          /* 3. Hijo termina con exit() */
    }

    /* --- CÓDIGO DEL PADRE --- */
    int status;
    wait(&status);        /* 4. Padre espera al hijo */
    printf("Hijo terminó con código: %d\n", WEXITSTATUS(status));

    return 0;
}
```

## ¿Qué retorna fork()?

| Valor de `pid` | Quién soy  | Significado                  |
|----------------|------------|------------------------------|
| `< 0`          | padre      | Error, no se creó el hijo    |
| `== 0`         | hijo       | Soy el proceso hijo          |
| `> 0`          | padre      | `pid` es el PID del hijo     |

## Esperar al hijo: wait() vs waitpid()

```c
/* Espera a cualquier hijo; NULL = no capturar código de salida */
wait(NULL);

/* Espera al hijo específico con PID dado */
int status;
waitpid(pid, &status, 0);

/* Extraer el código de salida del hijo */
if (WIFEXITED(status)) {
    int codigo = WEXITSTATUS(status);   /* lo que pasó a exit() */
}
```

> **Nota:** el valor de `status` que devuelve `wait()` es un entero compuesto; NO es directamente el código de exit. Usar siempre `WEXITSTATUS()`.

## Enviar señales a un proceso

```c
/* El padre mata al hijo después de 5 segundos */
pid_t pid = fork();
if (pid == 0) {
    while (1) { printf("hijo vivo\n"); sleep(1); }
} else {
    sleep(5);
    kill(pid, SIGKILL);   /* termina el proceso inmediatamente */
}
```

Señales comunes:

| Señal     | Número | Efecto                          |
|-----------|--------|---------------------------------|
| `SIGKILL` | 9      | Termina el proceso sin aviso    |
| `SIGTERM` | 15     | Solicita terminación (atrapaple)|
| `SIGINT`  | 2      | Ctrl+C                          |

## Árbol de procesos con múltiples forks

Cada `fork()` duplica el proceso actual. Si el padre hace dos `fork()` sin guardar el PID del primero, el primer hijo también hace el segundo fork:

```c
fork();   /* ahora hay 2 procesos */
fork();   /* cada uno hace fork → ahora hay 4 procesos */
```

Para controlar quién sigue creando hijos, guardar el retorno y usar `exit()` en el hijo:

```c
if (fork() == 0) { /* trabajo del hijo */ exit(0); }
if (fork() == 0) { /* otro hijo independiente */ exit(0); }
wait(NULL);
wait(NULL);
```

## Errores típicos

| Error                  | Causa probable                                      |
|------------------------|-----------------------------------------------------|
| `fork: Resource temporarily unavailable` | Se alcanzó el límite de procesos del sistema (`ulimit -u`) |
| Hijo queda zombie      | El padre nunca llama `wait()` o `waitpid()`         |
| Padre termina antes que el hijo | El hijo queda huérfano y lo adopta `init` (PID 1) |
| `WEXITSTATUS` devuelve basura | No se verificó `WIFEXITED(status)` antes    |

## Cómo compilar

```bash
gcc proceso.c -o proceso
./proceso
```

No requiere flags especiales (a diferencia de pthreads).

## Cierre de recursos

- El hijo debe llamar **`exit()`** (no `return` desde `main` si hay código posterior que no debe ejecutar).
- El padre debe llamar **`wait()`** o **`waitpid()`** por cada hijo para evitar zombies.
- Si el hijo abre archivos o sockets, debe cerrarlos con `close()` antes de `exit()`.
