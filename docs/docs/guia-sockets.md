# Guía: Sockets TCP

## ¿Qué es un socket?

Un socket es un punto de comunicación bidireccional entre procesos, ya sea en la misma máquina o en máquinas distintas a través de la red. El tipo `SOCK_STREAM` (TCP) garantiza entrega ordenada y sin pérdida de datos.

## Headers necesarios

```c
#include <sys/types.h>   /* tipos base */
#include <sys/socket.h>  /* socket, bind, listen, accept, connect, send, recv */
#include <netinet/in.h>  /* struct sockaddr_in, INADDR_ANY */
#include <arpa/inet.h>   /* htons, htonl, inet_pton, inet_ntop */
#include <unistd.h>      /* close, read, write */
#include <stdio.h>
#include <string.h>      /* memset, strcpy */
#include <stdlib.h>      /* exit */
```

## Compilar

```bash
gcc servidor.c -o servidor
gcc cliente.c  -o cliente
```

No se necesitan flags especiales para sockets básicos.

---

## Lado servidor — llamadas en orden

```c
/* 1. Crear el socket */
int sockfd = socket(AF_INET, SOCK_STREAM, 0);
if (sockfd == -1) { perror("socket"); exit(1); }

/* 2. Configurar dirección y puerto */
struct sockaddr_in addr;
memset(&addr, 0, sizeof(addr));
addr.sin_family      = AF_INET;
addr.sin_addr.s_addr = INADDR_ANY;   /* escucha en todas las interfaces */
addr.sin_port        = htons(5001);  /* puerto en orden de red */

/* 3. Asociar el socket a la dirección (bind) */
if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
    perror("bind"); exit(1);
}

/* 4. Marcar el socket como pasivo — acepta conexiones entrantes */
if (listen(sockfd, 5) == -1) {   /* 5 = tamaño de la cola de espera */
    perror("listen"); exit(1);
}

/* 5. Aceptar una conexión — bloquea hasta que llega un cliente */
struct sockaddr_in client_addr;
socklen_t client_len = sizeof(client_addr);
int connfd = accept(sockfd, (struct sockaddr *)&client_addr, &client_len);
if (connfd == -1) { perror("accept"); exit(1); }

/* 6. Comunicarse con el cliente a través de connfd */
char buf[256];
recv(connfd, buf, sizeof(buf), 0);
printf("Cliente dijo: %s\n", buf);
send(connfd, "OK", 3, 0);

/* 7. Cerrar conexiones */
close(connfd);   /* cierra la conexión con este cliente */
close(sockfd);   /* cierra el socket de escucha */
```

## Lado cliente — llamadas en orden

```c
/* 1. Crear el socket */
int sockfd = socket(AF_INET, SOCK_STREAM, 0);
if (sockfd == -1) { perror("socket"); exit(1); }

/* 2. Configurar dirección del servidor */
struct sockaddr_in servaddr;
memset(&servaddr, 0, sizeof(servaddr));
servaddr.sin_family = AF_INET;
servaddr.sin_port   = htons(5001);
/* IP del servidor: INADDR_ANY conecta a 0.0.0.0 — poco útil para cliente */
/* Para localhost: inet_pton(AF_INET, "127.0.0.1", &servaddr.sin_addr) */
servaddr.sin_addr.s_addr = INADDR_ANY;   /* como en el ejemplo del curso */

/* 3. Conectar al servidor */
if (connect(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) == -1) {
    perror("connect"); exit(1);
}

/* 4. Enviar y recibir */
send(sockfd, "Hola del cliente!", 18, 0);

char buf[256];
recv(sockfd, buf, sizeof(buf), 0);
printf("Servidor respondió: %s\n", buf);

/* 5. Cerrar */
close(sockfd);
```

## Diagrama del flujo

```
SERVIDOR                          CLIENTE
socket()                          socket()
bind()
listen()
accept() ←── espera ──────────── connect()
   ↕ (conexión establecida)          ↕
recv() / send()                   send() / recv()
close(connfd)                     close(sockfd)
close(sockfd)
```

## La función htons y las conversiones de byte order

La red usa big-endian ("network byte order"). El host puede ser little-endian. Usar siempre las funciones de conversión:

| Función   | Conversión                              |
|-----------|-----------------------------------------|
| `htons`   | host → network, 16 bits (puerto)        |
| `ntohs`   | network → host, 16 bits                 |
| `htonl`   | host → network, 32 bits (dirección IP)  |
| `ntohl`   | network → host, 32 bits                 |

```c
addr.sin_port = htons(5001);    /* siempre usar htons para el puerto */
```

## Convertir IP de texto a binario

```c
/* texto → binario (para conectar a una IP específica) */
inet_pton(AF_INET, "192.168.1.10", &servaddr.sin_addr);

/* binario → texto (para mostrar la IP del cliente que se conectó) */
char ip_str[INET_ADDRSTRLEN];
inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
printf("Conectó: %s\n", ip_str);
```

## Reusar dirección rápidamente (SO_REUSEADDR)

Si el servidor se reinicia, el puerto puede quedar en estado `TIME_WAIT` por ~60 segundos. Evitarlo:

```c
int opt = 1;
setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
/* llamar antes de bind() */
```

## Servidor que atiende múltiples clientes

Patrón con `fork` por cada cliente:

```c
while (1) {
    int connfd = accept(sockfd, NULL, NULL);
    if (fork() == 0) {
        close(sockfd);        /* hijo no necesita el socket de escucha */
        atender(connfd);
        close(connfd);
        exit(0);
    }
    close(connfd);            /* padre cierra su copia del socket del cliente */
}
```

## Errores típicos

| Error                    | Causa probable                                                      |
|--------------------------|---------------------------------------------------------------------|
| `bind: Address already in use` | Puerto en uso o en `TIME_WAIT`; usar `SO_REUSEADDR`            |
| `connect: Connection refused`  | El servidor no está corriendo o el puerto no coincide           |
| `recv` retorna 0         | El otro extremo cerró la conexión (EOF)                             |
| `send` retorna -1 con `EPIPE` | El receptor ya cerró su socket; manejar `SIGPIPE` o usar `MSG_NOSIGNAL` |
| `accept` bloquea para siempre | Ningún cliente intenta conectarse                               |
| Datos truncados          | `recv` puede devolver menos bytes que los enviados; iterar hasta leer todo |

## Leer exactamente N bytes

`recv` puede devolver menos de lo pedido. Para mensajes de longitud conocida:

```c
ssize_t leer_exacto(int fd, void *buf, size_t n) {
    size_t total = 0;
    while (total < n) {
        ssize_t r = recv(fd, (char *)buf + total, n - total, 0);
        if (r <= 0) return r;
        total += r;
    }
    return total;
}
```

## Cierre de recursos

```c
close(connfd);   /* cerrar la conexión individual con el cliente */
close(sockfd);   /* cerrar el socket de escucha del servidor */
```

En el servidor con fork: el padre debe cerrar `connfd` y el hijo debe cerrar `sockfd` para que los descriptores se liberen correctamente.
