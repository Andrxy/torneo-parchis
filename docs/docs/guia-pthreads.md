# Guía: Hilos POSIX (pthreads)

## ¿Qué es un hilo?

Un hilo (thread) es una unidad de ejecución dentro de un proceso. Todos los hilos de un proceso **comparten la misma memoria** (variables globales, heap), pero cada uno tiene su propio stack. Son más livianos que los procesos porque no duplican el espacio de memoria.

## Headers necesarios

```c
#include <pthread.h>   /* toda la API de pthreads */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>    /* sleep(), usleep() */
```

## Compilar — flag obligatorio

```bash
gcc programa.c -o programa -lpthread
```

El flag `-lpthread` enlaza la biblioteca de hilos. Sin él, el compilador no encontrará las funciones.

## Llamadas en orden — patrón mínimo

```c
#include <pthread.h>
#include <stdio.h>

/* 1. La función que ejecutará el hilo — firma fija */
void *funcion_hilo(void *arg) {
    printf("Hilo ejecutando\n");
    return NULL;   /* o pthread_exit(NULL) */
}

int main(void) {
    pthread_t id;                              /* 2. Identificador del hilo */

    pthread_create(&id, NULL, funcion_hilo, NULL);  /* 3. Crear hilo */
    pthread_join(id, NULL);                    /* 4. Esperar que termine */

    return 0;
}
```

## Pasar argumentos al hilo

La función del hilo recibe un único `void *`. Para pasar varios datos, usar un `struct`:

```c
typedef struct {
    char *cadena;
    int numero;
} Parametro;

void *funcion_hilo(void *arg) {
    Parametro *p = (Parametro *)arg;   /* cast obligatorio */
    printf("%s: %d\n", p->cadena, p->numero);
    return NULL;
}

int main(void) {
    pthread_t id;
    Parametro datos = { .cadena = "hola", .numero = 42 };

    pthread_create(&id, NULL, funcion_hilo, (void *)&datos);
    pthread_join(id, NULL);
    return 0;
}
```

> **Advertencia:** no pasar un puntero a una variable local del loop; cuando el loop avanza, la variable ya cambió.

## Múltiples hilos con array

```c
#define N 3
pthread_t ids[N];

for (int i = 0; i < N; i++)
    pthread_create(&ids[i], NULL, funcion_hilo, NULL);

for (int i = 0; i < N; i++)
    pthread_join(ids[i], NULL);
```

## Retornar valores desde el hilo

```c
void *hilo(void *arg) {
    int *resultado = malloc(sizeof(int));
    *resultado = 99;
    return resultado;   /* se recibe en pthread_join */
}

int main(void) {
    pthread_t id;
    void *retval;

    pthread_create(&id, NULL, hilo, NULL);
    pthread_join(id, &retval);

    int r = *(int *)retval;
    free(retval);       /* liberar lo que el hilo asignó con malloc */
    printf("resultado: %d\n", r);
    return 0;
}
```

## Atributos del hilo (pthread_attr_t)

```c
pthread_attr_t attr;
pthread_attr_init(&attr);                              /* inicializar con defaults */
/* opcional: cambiar tamaño de stack, política de scheduling, etc. */
pthread_create(&id, &attr, funcion_hilo, NULL);
pthread_attr_destroy(&attr);                           /* liberar la estructura */
```

Para uso básico basta `NULL` como segundo argumento de `pthread_create`.

## Terminar un hilo

| Forma                  | Cuándo usarla                                      |
|------------------------|----------------------------------------------------|
| `return NULL`          | Al final de la función — forma preferida           |
| `pthread_exit(NULL)`   | Para salir antes del final, desde cualquier punto  |
| `pthread_cancel(id)`   | El padre solicita cancelar el hilo (asíncrono)     |

## Variables globales compartidas

Los hilos ven las mismas variables globales. Sin sincronización, esto genera condiciones de carrera:

```c
int contador = 0;  /* variable compartida — peligrosa sin mutex */

void *incrementar(void *arg) {
    contador++;    /* NO es atómica: lectura + suma + escritura */
    return NULL;
}
```

Para protegerla, usar mutex (ver `guia-mutex.md`) o semáforos POSIX (ver `guia-semaforos.md`).

## Errores típicos

| Error                          | Causa probable                                            |
|--------------------------------|-----------------------------------------------------------|
| Segfault en el hilo            | El `arg` apunta a memoria que ya no existe (variable local de `main` que se fue del stack) |
| Resultado incorrecto           | Condición de carrera sobre variable compartida sin mutex  |
| `pthread_join` cuelga          | El hilo nunca retorna ni llama `pthread_exit`             |
| `undefined reference to pthread_create` | Falta `-lpthread` al compilar                  |
| Hilo "zombie"                  | No se llamó `pthread_join` ni se configuró `DETACHED`     |

## Diagrama del ciclo de vida

```
pthread_create → [hilo ejecuta] → return / pthread_exit
                                         ↑
main →  ........  pthread_join (bloquea aquí hasta que el hilo termine)
```

## Cierre de recursos

1. Llamar `pthread_join()` por cada hilo creado (o `pthread_detach()` si no se necesita esperar).
2. Si el hilo asignó memoria con `malloc`, liberarla antes de `return` o en el padre después del `join`.
3. Si se usó `pthread_attr_init`, llamar `pthread_attr_destroy` al terminar.
