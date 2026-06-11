# Guía: Colas de mensajes System V

## ¿Qué es una cola de mensajes?

Una cola de mensajes es un canal IPC (Inter-Process Communication) gestionado por el kernel. A diferencia de los pipes, los mensajes tienen un **tipo** (`mtype`), lo que permite que un proceso lector filtre qué mensajes quiere recibir. No requiere que los procesos compartan parentesco (fork).

## Headers necesarios

```c
#include <sys/msg.h>   /* msgget, msgsnd, msgrcv, msgctl */
#include <sys/types.h> /* key_t, pid_t */
#include <sys/ipc.h>   /* IPC_CREAT, IPC_RMID, ftok */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>    /* strcpy */
#include <unistd.h>    /* fork */
```

## Definir la estructura del mensaje

El primer campo **debe** ser `long mtype` con valor > 0. El resto es el contenido:

```c
typedef struct {
    long mtype;       /* tipo del mensaje — debe ser > 0 */
    char mtext[100];  /* cuerpo del mensaje */
} mensaje_t;
```

## Llamadas en orden — patrón mínimo

```c
/* 1. Generar clave IPC */
key_t key = ftok(".", 'a');       /* deriva clave de un archivo existente */

/* 2. Crear o abrir la cola */
int msg_id = msgget(key, IPC_CREAT | 0666);
if (msg_id == -1) { perror("msgget"); exit(1); }

/* 3. Enviar mensaje */
mensaje_t m;
m.mtype = 1;
strcpy(m.mtext, "Hola");
if (msgsnd(msg_id, &m, sizeof(m.mtext), 0) == -1) {
    perror("msgsnd");
}

/* 4. Recibir mensaje (type=1 → recibe el primer mensaje de tipo 1) */
mensaje_t recibido;
if (msgrcv(msg_id, &recibido, sizeof(recibido.mtext), 1, 0) == -1) {
    perror("msgrcv");
}
printf("Recibido: %s\n", recibido.mtext);

/* 5. Eliminar la cola */
msgctl(msg_id, IPC_RMID, NULL);
```

## Ejemplo completo con padre e hijo

```c
#include <sys/msg.h>
#include <sys/ipc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

typedef struct { long mtype; char mtext[100]; } mensaje_t;

int main(void) {
    key_t key = ftok(".", 'a');
    int qid = msgget(key, IPC_CREAT | 0666);

    pid_t pid = fork();

    if (pid == 0) {
        /* hijo: envía */
        mensaje_t m = { .mtype = 1 };
        strcpy(m.mtext, "Hola del proceso hijo!");
        msgsnd(qid, &m, sizeof(m.mtext), 0);
        return 0;
    }

    /* padre: recibe */
    mensaje_t recibido;
    msgrcv(qid, &recibido, sizeof(recibido.mtext), 1, 0);
    printf("Padre recibió: %s\n", recibido.mtext);

    wait(NULL);
    msgctl(qid, IPC_RMID, NULL);   /* solo el padre elimina la cola */
    return 0;
}
```

## Generar la clave: ftok

```c
key_t key = ftok("/ruta/existente", 'X');
```

`ftok` combina el inode del archivo con el carácter dado para producir una clave reproducible. Ambos procesos deben usar **el mismo archivo y el mismo carácter** para obtener la misma clave.

- El archivo referenciado **debe existir**.
- Usar `"."` (directorio actual) es común, pero si los procesos se ejecutan desde directorios distintos, la clave diferirá.

Alternativa: usar una clave fija numérica:

```c
int qid = msgget(1234, IPC_CREAT | 0666);
```

## Parámetros de msgsnd y msgrcv

```c
/* msgsnd */
int msgsnd(int msqid, const void *msgp, size_t msgsz, int msgflg);
/*         id cola     puntero struct   tamaño del cuerpo  flags */
/* msgsz = sizeof(m.mtext), NO sizeof(m) — no incluye mtype */

/* msgrcv */
ssize_t msgrcv(int msqid, void *msgp, size_t msgsz, long msgtyp, int msgflg);
/*             id cola     destino    tamaño cuerpo  tipo        flags */
```

### Valores de msgtyp en msgrcv

| `msgtyp` | Comportamiento                                  |
|----------|-------------------------------------------------|
| `> 0`    | Lee el primer mensaje del tipo exacto           |
| `== 0`   | Lee el primer mensaje sin importar el tipo      |
| `< 0`    | Lee el mensaje con el tipo más pequeño ≤ |msgtyp| |

### Flags comunes

| Flag      | Efecto                                                     |
|-----------|------------------------------------------------------------|
| `0`       | Bloquea hasta que haya mensaje o haya espacio              |
| `IPC_NOWAIT` | Retorna inmediatamente con error `ENOMSG` si no hay nada |

## Errores típicos

| Error                   | Causa probable                                                   |
|-------------------------|------------------------------------------------------------------|
| `msgget: No space left` | Límite de colas del sistema alcanzado; ver `ipcs -q`             |
| `msgsnd: Message too long` | `msgsz` excede el límite de la cola (`MSGMAX`)                |
| `msgrcv: No message`    | `IPC_NOWAIT` activado y no hay mensajes del tipo pedido          |
| Cola persiste en el SO  | No se llamó `msgctl(IPC_RMID)`; limpiar con `ipcrm -q <id>`     |
| `ftok` devuelve -1      | El archivo no existe o no es accesible                           |

## Comandos de diagnóstico

```bash
ipcs -q               # listar colas de mensajes activas
ipcrm -q <msqid>      # eliminar una cola manualmente
ipcs -l               # ver límites del sistema (MSGMAX, MSGMNB, etc.)
```

## Diferencias con pipes y sockets

| Característica        | Cola de mensajes       | Pipe            | Socket TCP        |
|-----------------------|------------------------|-----------------|-------------------|
| Persistencia          | Hasta `IPC_RMID` o reboot | Proceso activo | Proceso activo    |
| Tipos de mensaje      | Sí (`mtype`)           | No              | No                |
| Procesos no emparentados | Sí (con misma clave) | No             | Sí                |
| Red (máquinas distintas) | No                  | No              | Sí                |

## Cierre de recursos

La cola de mensajes **persiste en el kernel** aunque todos los procesos terminen. Es obligatorio llamar:

```c
msgctl(msg_id, IPC_RMID, NULL);
```

Solo un proceso debe eliminarlo (típicamente el que la creó o el padre). Verificar con `ipcs -q` que no queden colas huérfanas.
