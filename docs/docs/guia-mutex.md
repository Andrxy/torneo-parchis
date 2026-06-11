# Guía: Mutex (exclusión mutua con pthreads)

## ¿Qué es un mutex?

Un mutex (mutual exclusion) es un candado que solo un hilo puede tomar a la vez. El hilo que lo toma puede entrar a la sección crítica; los demás se bloquean hasta que el candado se libere.

## Headers necesarios

```c
#include <pthread.h>   /* pthread_mutex_t y todas las funciones */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
```

Compilar con:
```bash
gcc programa.c -o programa -lpthread
```

## Llamadas en orden — patrón mínimo

```c
#include <pthread.h>
#include <stdio.h>

int contador = 0;
pthread_mutex_t llave;   /* 1. Declarar el mutex como variable global */

void *incrementar(void *arg) {
    pthread_mutex_lock(&llave);     /* 2. Tomar el candado — bloquea si está ocupado */
    contador++;                     /* sección crítica */
    pthread_mutex_unlock(&llave);   /* 3. Soltar el candado */
    return NULL;
}

int main(void) {
    pthread_t h1, h2;

    pthread_mutex_init(&llave, NULL);    /* 4. Inicializar el mutex */

    pthread_create(&h1, NULL, incrementar, NULL);
    pthread_create(&h2, NULL, incrementar, NULL);

    pthread_join(h1, NULL);
    pthread_join(h2, NULL);

    printf("Contador: %d\n", contador);  /* garantizado: 2 */

    pthread_mutex_destroy(&llave);       /* 5. Destruir al terminar */
    return 0;
}
```

## Orden de las llamadas

```
Inicialización (una vez, antes de crear hilos):
  pthread_mutex_init(&llave, NULL)

Por cada hilo, al entrar a la sección crítica:
  pthread_mutex_lock(&llave)     ← bloquea si otro hilo ya lo tiene
    [sección crítica]
  pthread_mutex_unlock(&llave)   ← libera, despierta al próximo en espera

Al terminar el programa:
  pthread_mutex_destroy(&llave)
```

## Mutex estático (sin init)

Si el mutex es global y se inicializa en tiempo de compilación:

```c
pthread_mutex_t llave = PTHREAD_MUTEX_INITIALIZER;
/* No es necesario llamar pthread_mutex_init ni pthread_mutex_destroy */
```

## Intentar tomar el mutex sin bloquear

```c
int ret = pthread_mutex_trylock(&llave);
if (ret == 0) {
    /* tomó el candado */
    pthread_mutex_unlock(&llave);
} else {
    /* no pudo tomarlo, está ocupado */
}
```

## Ejemplo real: dos hilos leen un arreglo compartido

Tomado de `mutex.c` — un hilo recorre el arreglo hacia adelante y otro hacia atrás. El mutex garantiza que no se mezclen sus salidas:

```c
int arreglo[] = {1, 2, 3, 4, 5, 6};
pthread_mutex_t llave;

void *recorrer(void *args) {
    pthread_mutex_lock(&llave);
    for (int i = 0; i < 6; i++)
        printf("%d ", arreglo[i]);
    printf("\n");
    pthread_mutex_unlock(&llave);
    return NULL;
}

void *recorrerInverso(void *args) {
    pthread_mutex_lock(&llave);
    for (int i = 5; i >= 0; i--)
        printf("%d ", arreglo[i]);
    printf("\n");
    pthread_mutex_unlock(&llave);
    return NULL;
}

int main(void) {
    pthread_t h1, h2;
    pthread_mutex_init(&llave, NULL);
    pthread_create(&h1, NULL, recorrer, NULL);
    pthread_create(&h2, NULL, recorrerInverso, NULL);
    pthread_join(h1, NULL);
    pthread_join(h2, NULL);
    pthread_mutex_destroy(&llave);
    return 0;
}
```

## Errores típicos

| Error                     | Causa probable                                                   |
|---------------------------|------------------------------------------------------------------|
| Deadlock (programa cuelga) | Un hilo hace `lock` dos veces sin `unlock` intermedio, o dos hilos se esperan mutuamente |
| Condición de carrera sigue ocurriendo | El mutex no protege **todas** las escrituras a esa variable |
| `pthread_mutex_destroy` falla | Otro hilo todavía tiene el mutex bloqueado                |
| Crash al destruir         | Se llamó `destroy` antes de que todos los hilos hicieran `unlock` |

## Buenas prácticas

- El mutex debe ser visible para todos los hilos que lo usan → declararlo **global** o pasarlo como argumento.
- La sección crítica debe ser lo más corta posible para no bloquear a los demás hilos innecesariamente.
- Nunca hacer `sleep()` dentro de la sección crítica.
- Si se necesitan múltiples mutexes, establecer un orden fijo de adquisición para evitar deadlocks.

## Cierre de recursos

```c
pthread_mutex_destroy(&llave);  /* siempre al terminar, después de join */
```
