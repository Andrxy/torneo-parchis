/* main.c — Árbitro: tablero compartido, servidor TCP, fork de jugadores, Round Robin */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include "parchis.h"
#include "tablero.h"
#include "jugador.h"

/* Casillas de seguro: salidas + 4 fijas */
const int CASILLAS_SEGURO[NUM_SEGUROS] = {
    SALIDA_ROJO, SALIDA_VERDE, SALIDA_AZUL, SALIDA_AMARILLO,
    11, 28, 45, 62
};

static ssize_t leer_exacto(int fd, void *buf, size_t n)
{
    size_t total = 0;
    while (total < n) {
        ssize_t r = recv(fd, (char *)buf + total, n - total, 0);
        if (r <= 0) return r;
        total += (size_t)r;
    }
    return (ssize_t)total;
}

static size_t leer_pipe(int fd, void *buf, size_t n)
{
    size_t total = 0;
    while (total < n) {
        ssize_t r = read(fd, (char *)buf + total, n - total);
        if (r < 0) { perror("read (pipe)"); break; }
        if (r == 0) break;
        total += (size_t)r;
    }
    return total;
}

#define MAX_EVENTOS 5
static pthread_mutex_t g_lock_eventos = PTHREAD_MUTEX_INITIALIZER;
static char g_eventos[MAX_EVENTOS][96];
static int g_num_eventos = 0;

/* Agrega evento al panel; descarta el más viejo si está lleno */
static void registrar_evento(const char *linea)
{
    pthread_mutex_lock(&g_lock_eventos);
    if (g_num_eventos == MAX_EVENTOS) {
        for (int k = 1; k < MAX_EVENTOS; k++)
            strcpy(g_eventos[k - 1], g_eventos[k]);
        g_num_eventos--;
    }
    strcpy(g_eventos[g_num_eventos], linea);
    g_num_eventos++;
    pthread_mutex_unlock(&g_lock_eventos);
}

/* Copia eventos sin retener el lock durante el dibujo */
static int copiar_eventos(char destino[][96])
{
    pthread_mutex_lock(&g_lock_eventos);
    int n = g_num_eventos;
    for (int k = 0; k < n; k++)
        strcpy(destino[k], g_eventos[k]);
    pthread_mutex_unlock(&g_lock_eventos);
    return n;
}

typedef struct {
    int msq_id;
} ArgsConsumidor;

/* Hilo consumidor: lee capturas de la cola y las registra; sale con MTYPE_FIN_COLA */
static void *consumidor_capturas(void *arg)
{
    ArgsConsumidor *ac = (ArgsConsumidor *)arg;
    MsgCaptura m;

    while (1) {
        ssize_t r = msgrcv(ac->msq_id, &m,
                           sizeof(MsgCaptura) - sizeof(long), 0, 0);
        if (r == -1) {
            if (errno == EIDRM || errno == EINTR) break;
            perror("msgrcv");
            break;
        }
        if (m.mtype == MTYPE_FIN_COLA) break;

        char linea[96];
        snprintf(linea, sizeof(linea),
                 "El %s comió una ficha del %s (ficha%d) en la casilla %d",
                 color_nombre(m.capturador), color_nombre(m.victima),
                 m.id_victima, m.casilla);
        registrar_evento(linea);
    }
    return NULL;
}

/* g_terminar: el handler solo la escribe; la limpieza la hace el flujo principal */
static volatile sig_atomic_t g_terminar = 0;

static void manejador_senal(int sig)
{
    (void)sig;
    g_terminar = 1;
}

/* Inventario de recursos; campos en -1/NULL hasta que se crean */
typedef struct {
    Tablero *tablero;
    int msq_id;
    int srv_fd;
    int pipes_rd[NUM_JUGADORES];
    int fd_jugadores[NUM_JUGADORES];
    pid_t pids[NUM_JUGADORES];
    pthread_t consumidor;
    int consumidor_activo;
} RecursosArbitro;

/* Libera toda la IPC: primero hijos (usan mutex del tablero), luego el tablero */
static void liberar_recursos(RecursosArbitro *r)
{
    int i;

    for (i = 0; i < NUM_JUGADORES; i++)
        if (r->fd_jugadores[i] != -1) { close(r->fd_jugadores[i]); r->fd_jugadores[i] = -1; }

    for (i = 0; i < NUM_JUGADORES; i++)
        if (r->pipes_rd[i] != -1) { close(r->pipes_rd[i]); r->pipes_rd[i] = -1; }

    if (r->srv_fd != -1) { close(r->srv_fd); r->srv_fd = -1; }

    /* Borra la cola para desbloquear msgrcv del consumidor con EIDRM */
    if (r->consumidor_activo) {
        if (r->msq_id != -1) { msgctl(r->msq_id, IPC_RMID, NULL); r->msq_id = -1; }
        pthread_join(r->consumidor, NULL);
        r->consumidor_activo = 0;
    }
    if (r->msq_id != -1) { msgctl(r->msq_id, IPC_RMID, NULL); r->msq_id = -1; }

    for (i = 0; i < NUM_JUGADORES; i++)
        if (r->pids[i] > 0) kill(r->pids[i], SIGTERM);
    for (i = 0; i < NUM_JUGADORES; i++)
        if (r->pids[i] > 0) { waitpid(r->pids[i], NULL, 0); r->pids[i] = -1; }

    if (r->tablero != NULL) { tablero_destruir(r->tablero); r->tablero = NULL; }
}

int main(void)
{
    int c;
    int codigo_salida = 0;

    RecursosArbitro rec;
    rec.tablero = NULL;
    rec.msq_id = -1;
    rec.srv_fd = -1;
    rec.consumidor_activo = 0;
    for (c = 0; c < NUM_JUGADORES; c++) {
        rec.pipes_rd[c] = -1;
        rec.fd_jugadores[c] = -1;
        rec.pids[c] = -1;
    }

    /* 1. Tablero en memoria compartida antes del fork */
    Tablero *tablero = tablero_crear();
    if (tablero == NULL) return 1;
    rec.tablero = tablero;
    tablero_init(tablero);
    tablero_imprimir_posiciones(tablero);
    fflush(stdout);

    /* 1b. Cola System V antes del fork (los hijos heredan msq_id) */
    key_t clave_cola = ftok(".", 'P');
    if (clave_cola == (key_t)-1) {
        perror("ftok"); codigo_salida = 1; goto limpieza;
    }
    rec.msq_id = msgget(clave_cola, IPC_CREAT | 0666);
    if (rec.msq_id == -1) {
        perror("msgget"); codigo_salida = 1; goto limpieza;
    }
    int msq_id = rec.msq_id;

    /* 2. Servidor TCP antes del fork; los hijos heredan srv_fd pero lo cierran */
    rec.srv_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (rec.srv_fd == -1) {
        perror("socket"); codigo_salida = 1; goto limpieza;
    }
    int srv_fd = rec.srv_fd;

    int opt = 1;
    setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PUERTO_ARBITRO);

    if (bind(srv_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind"); codigo_salida = 1; goto limpieza;
    }
    if (listen(srv_fd, NUM_JUGADORES + 1) == -1) {
        perror("listen"); codigo_salida = 1; goto limpieza;
    }
    printf("[arbitro] servidor TCP listo en puerto %d\n", PUERTO_ARBITRO);
    fflush(stdout);

    /* 2b. Un pipe por jugador (hijo escribe stats, padre lee), antes del fork */
    int pipes[NUM_JUGADORES][2];
    for (c = 0; c < NUM_JUGADORES; c++) {
        if (pipe(pipes[c]) == -1) {
            perror("pipe");
            for (int j = 0; j < c; j++) { close(pipes[j][0]); close(pipes[j][1]); }
            codigo_salida = 1; goto limpieza;
        }
    }

    /* 3. Fork de 4 procesos jugador */
    for (c = 0; c < NUM_JUGADORES; c++) {
        fflush(stdout);
        rec.pids[c] = fork();
        if (rec.pids[c] < 0) {
            perror("fork");
            for (int j = 0; j < NUM_JUGADORES; j++) close(pipes[j][1]);
            codigo_salida = 1; goto limpieza;
        }
        if (rec.pids[c] == 0) {
            /* Hijo: cierra todos los [0] y las copias de escritura ajenas */
            for (int j = 0; j < NUM_JUGADORES; j++) {
                close(pipes[j][0]);
                if (j != c) close(pipes[j][1]);
            }
            jugador_ejecutar(c, tablero, srv_fd, msq_id, pipes[c][1]);
        }
    }

    /* Padre cierra extremos de escritura y guarda los de lectura */
    for (c = 0; c < NUM_JUGADORES; c++) {
        close(pipes[c][1]);
        rec.pipes_rd[c] = pipes[c][0];
    }

    /* Instala manejador solo en el árbitro; sin SA_RESTART para que EINTR corte accept/recv */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = manejador_senal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* 4. Acepta las 4 conexiones e identifica cada jugador por su color */
    for (c = 0; c < NUM_JUGADORES; c++) {
        int connfd = accept(srv_fd, NULL, NULL);
        if (connfd == -1) {
            if (g_terminar) { codigo_salida = 130; goto limpieza; }
            if (errno == EINTR) { c--; continue; }
            perror("accept"); codigo_salida = 1; goto limpieza;
        }

        int32_t color = -1;
        leer_exacto(connfd, &color, sizeof(color));

        if (color >= 0 && color < NUM_JUGADORES) {
            rec.fd_jugadores[color] = connfd;
            printf("[arbitro] jugador %-8s conectado (fd=%d)\n",
                   color_nombre(color), connfd);
        } else {
            printf("[arbitro] color inválido recibido: %d\n", color);
            close(connfd);
            c--;
        }
    }
    fflush(stdout);
    int *fd_jugadores = rec.fd_jugadores;

    /* 4b. Lanza el hilo consumidor de eventos de captura */
    ArgsConsumidor args_cons;
    args_cons.msq_id = msq_id;
    pthread_create(&rec.consumidor, NULL, consumidor_capturas, &args_cons);
    rec.consumidor_activo = 1;
    pthread_t hilo_consumidor = rec.consumidor;

    /* 5. Round Robin: un turno por jugador en orden R→V→A→Am */
    int siguiente = 0;
    int ronda = 0;
    int ganador = -1;

    while (ronda < MAX_RONDAS && ganador == -1) {
        if (g_terminar) { codigo_salida = 130; goto limpieza; }

        int32_t msg = MSG_TU_TURNO;
        if (send(fd_jugadores[siguiente], &msg, sizeof(msg), 0) <= 0) break;

        int32_t resp = 0;
        if (leer_exacto(fd_jugadores[siguiente], &resp, sizeof(resp)) <= 0) break;

        if (resp == MSG_TURNO_OK &&
            tablero->fichas_en_meta[siguiente] == FICHAS_POR_JUGADOR) {
            ganador = siguiente;
        }

        /* Redibuja con el tablero quieto para evitar lecturas en carrera */
        char ev_local[MAX_EVENTOS][96];
        int n_ev = copiar_eventos(ev_local);
        tablero_dibujar(tablero, siguiente, ronda, ev_local, n_ev);
        usleep(RETARDO_RONDA_US);

        siguiente = (siguiente + 1) % NUM_JUGADORES;
        if (siguiente == 0) ronda++;
    }

    if (g_terminar) { codigo_salida = 130; goto limpieza; }

    if (ganador != -1)
        printf("[arbitro] *** %s GANA en ronda %d ***\n",
               color_nombre(ganador), ronda);
    else
        printf("[arbitro] límite de rondas alcanzado (%d)\n", MAX_RONDAS);

    /* 6. Anuncia FIN_JUEGO; MSG_NOSIGNAL evita SIGPIPE */
    int32_t fin = MSG_FIN_JUEGO;
    for (c = 0; c < NUM_JUGADORES; c++)
        send(fd_jugadores[c], &fin, sizeof(fin), MSG_NOSIGNAL);

    /* 6b. Lee estadísticas de cada pipe antes de waitpid */
    EstadisticasJugador stats[NUM_JUGADORES];
    for (c = 0; c < NUM_JUGADORES; c++) {
        size_t leidos = leer_pipe(rec.pipes_rd[c], &stats[c], sizeof(stats[c]));
        if (leidos != sizeof(stats[c]))
            fprintf(stderr, "[arbitro] estadísticas incompletas del jugador %d\n", c);
        close(rec.pipes_rd[c]);
        rec.pipes_rd[c] = -1;
    }

    /* 7. Espera a los 4 procesos jugador */
    for (c = 0; c < NUM_JUGADORES; c++) {
        int status;
        waitpid(rec.pids[c], &status, 0);
        if (WIFEXITED(status))
            printf("[arbitro] jugador %-8s (pid=%d) terminó con código %d\n",
                   color_nombre(c), rec.pids[c], WEXITSTATUS(status));
        rec.pids[c] = -1;
    }
    printf("[arbitro] todos los jugadores terminaron\n");

    /* 7b. Detiene el consumidor enviando el centinela */
    MsgCaptura fin_cola;
    memset(&fin_cola, 0, sizeof(fin_cola));
    fin_cola.mtype = MTYPE_FIN_COLA;
    if (msgsnd(msq_id, &fin_cola, sizeof(MsgCaptura) - sizeof(long), 0) == -1)
        perror("msgsnd (fin cola)");
    pthread_join(hilo_consumidor, NULL);
    rec.consumidor_activo = 0;

    /* 8. Tabla resumen con estadísticas de los pipes */
    int tot_meta = 0, tot_hechas = 0, tot_sufridas = 0, tot_movs = 0;
    printf("\n=========== Estadísticas finales (vía pipes) ===========\n");
    printf("  %-9s %5s %9s %9s %6s\n",
           "COLOR", "META", "CAP.HECH", "CAP.SUFR", "MOVS");
    printf("  -----------------------------------------------------\n");
    for (c = 0; c < NUM_JUGADORES; c++) {
        printf("  %-9s %4d/4 %9d %9d %6d%s\n",
               color_nombre(stats[c].color), stats[c].fichas_meta,
               stats[c].capturas_hechas, stats[c].capturas_sufridas,
               stats[c].movimientos,
               stats[c].fichas_meta == FICHAS_POR_JUGADOR ? "  *** GANADOR ***" : "");
        tot_meta     += stats[c].fichas_meta;
        tot_hechas   += stats[c].capturas_hechas;
        tot_sufridas += stats[c].capturas_sufridas;
        tot_movs     += stats[c].movimientos;
    }
    printf("  -----------------------------------------------------\n");
    printf("  %-9s %4d   %9d %9d %6d\n",
           "TOTAL", tot_meta, tot_hechas, tot_sufridas, tot_movs);
    printf("========================================================\n");

limpieza:
    if (codigo_salida == 130)
        printf("[arbitro] señal recibida: liberando recursos y saliendo...\n");
    liberar_recursos(&rec);
    return codigo_salida;
}
