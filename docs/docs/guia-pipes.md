# Guía: Pipes (tuberías sin nombre)

## ¿Qué es un pipe?

Un pipe es un canal de comunicación unidireccional entre procesos relacionados (padre e hijo). Los datos se escriben por un extremo y se leen por el otro, en orden FIFO. El kernel mantiene un buffer interno (típicamente 64 KB en Linux).

## Headers necesarios

```c
#include <unistd.h>    /* pipe(), read(), write(), close(), fork() */
#include <sys/types.h> /* pid_t */
#include <sys/wait.h>  /* wait() */
#include <stdio.h>     /* perror(), printf() */
#include <string.h>    /* strlen(), strcpy() */
#include <stdlib.h>    /* exit() */
```

## Llamadas en orden — patrón mínimo

```c
#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    int fds[2];

    /* 1. Crear el pipe ANTES de fork */
    if (pipe(fds) == -1) {
        perror("pipe");
        return 1;
    }
    /* fds[0] = extremo de lectura  */
    /* fds[1] = extremo de escritura */

    pid_t pid = fork();   /* 2. Crear proceso hijo */

    if (pid == 0) {
        /* --- HIJO: solo lee --- */
        close(fds[1]);               /* 3a. Cerrar extremo que no usa */
        char buf[100];
        read(fds[0], buf, sizeof(buf));   /* 4. Leer del pipe */
        printf("Hijo recibió: %s\n", buf);
        close(fds[0]);               /* 5. Cerrar extremo de lectura */
        return 0;
    }

    /* --- PADRE: solo escribe --- */
    close(fds[0]);                   /* 3b. Cerrar extremo que no usa */
    char msg[] = "Hola del padre";
    write(fds[1], msg, sizeof(msg)); /* 4. Escribir en el pipe */
    close(fds[1]);                   /* 5. Cerrar — señala EOF al hijo */

    wait(NULL);   /* 6. Esperar al hijo */
    return 0;
}
```

## Anatomía del array fds

```
pipe(fds)
         ┌──────────────────────────┐
PADRE    │  fds[1] → [buffer del   │ → fds[0]  HIJO
escribe ──►   kernel ~64KB]        ├──────────► lee
         └──────────────────────────┘
```

| Índice  | Extremo    | Operación permitida |
|---------|------------|---------------------|
| `fds[0]`| lectura    | `read()`            |
| `fds[1]`| escritura  | `write()`           |

## ¿Por qué cerrar el extremo que no se usa?

- Si el padre no cierra `fds[0]`, hay dos lectores potenciales y el EOF nunca llega correctamente.
- Si el padre no cierra `fds[1]` después de escribir, el hijo se queda bloqueado en `read()` esperando más datos porque el pipe sigue "abierto".

**Regla:** cada proceso debe cerrar el extremo que no va a usar **inmediatamente después del fork**.

## Comunicación bidireccional

Un solo pipe es unidireccional. Para bidireccional, usar dos pipes:

```c
int padre_a_hijo[2];   /* padre escribe, hijo lee */
int hijo_a_padre[2];   /* hijo escribe, padre lee */

pipe(padre_a_hijo);
pipe(hijo_a_padre);

if (fork() == 0) {
    close(padre_a_hijo[1]);
    close(hijo_a_padre[0]);
    /* hijo: lee de padre_a_hijo[0], escribe en hijo_a_padre[1] */
} else {
    close(padre_a_hijo[0]);
    close(hijo_a_padre[1]);
    /* padre: escribe en padre_a_hijo[1], lee de hijo_a_padre[0] */
}
```

## Leer hasta EOF

`read()` retorna `0` cuando todos los escritores han cerrado su extremo:

```c
char buf[256];
ssize_t n;
while ((n = read(fds[0], buf, sizeof(buf))) > 0) {
    fwrite(buf, 1, n, stdout);
}
/* n == 0 → EOF; n == -1 → error */
```

## Tamaño del buffer y escrituras atómicas

- Escrituras de hasta `PIPE_BUF` bytes (≥ 512 bytes, típicamente 4096) son **atómicas**: no se entremezclan con escrituras de otros procesos.
- Escrituras más grandes pueden fragmentarse.

```c
#include <limits.h>
printf("PIPE_BUF = %ld\n", (long)PIPE_BUF);
```

## Redireccionar stdin/stdout hacia un pipe

Técnica usada para conectar la salida de un proceso con la entrada de otro (como el shell con `|`):

```c
/* Redirigir stdout del hijo al extremo de escritura del pipe */
dup2(fds[1], STDOUT_FILENO);
close(fds[1]);
execl("/bin/ls", "ls", NULL);   /* ls escribirá al pipe */
```

## Errores típicos

| Error                  | Causa probable                                               |
|------------------------|--------------------------------------------------------------|
| `read` bloquea para siempre | No se cerró `fds[1]` en el proceso lector; el pipe nunca alcanza EOF |
| `write: Broken pipe`   | El proceso lector cerró su extremo sin leer; el SO envía `SIGPIPE` al escritor |
| Datos mezclados        | Varios procesos escriben sin sincronización y los datos se entrelazan |
| Datos perdidos         | Se leyó menos de lo escrito; `read` puede devolver menos bytes de lo esperado en escrituras grandes |

## Compilar

```bash
gcc mensajepipe.c -o mensajepipe
./mensajepipe
```

No requiere flags especiales.

## Cierre de recursos

```
Cada extremo (fds[0] y fds[1]) debe cerrarse con close() en cada proceso
que lo tenga abierto, incluyendo las copias creadas por fork().
```

No existe una llamada de "destrucción" del pipe: se destruye automáticamente cuando todos los descriptores que lo referencian están cerrados.
