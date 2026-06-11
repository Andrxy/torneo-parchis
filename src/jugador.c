/* jugador.c — Proceso jugador: cliente TCP que dirige sus 4 fichas.
 * En cada TU_TURNO despierta las fichas, espera y responde TURNO_OK. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "jugador.h"
#include "ficha.h"
#include "tablero.h"

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

void jugador_ejecutar(int color, Tablero *tablero, int srv_fd,
                      int msq_id, int pipe_wr)
{
    int f;

    close(srv_fd);  /* el hijo no es servidor */

    tablero->jugadores[color].pid = getpid();

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) { perror("socket (jugador)"); exit(1); }

    struct sockaddr_in serv;
    memset(&serv, 0, sizeof(serv));
    serv.sin_family = AF_INET;
    serv.sin_port = htons(PUERTO_ARBITRO);
    inet_pton(AF_INET, "127.0.0.1", &serv.sin_addr);

    if (connect(sockfd, (struct sockaddr *)&serv, sizeof(serv)) == -1) {
        perror("connect (jugador)"); close(sockfd); exit(1);
    }

    int32_t mi_color = (int32_t)color;
    send(sockfd, &mi_color, sizeof(mi_color), 0);

    pthread_mutex_lock(&tablero->mutex_impresion);
    printf("[jugador %-8s pid=%d] conectado al árbitro\n",
           color_nombre(color), getpid());
    pthread_mutex_unlock(&tablero->mutex_impresion);

    /* Un semáforo propio por ficha: solo se postea el de la ficha elegida.
     * sem_listo es compartido: la ficha que mueve avisa que terminó. */
    sem_t sem_turno[FICHAS_POR_JUGADOR];
    sem_t sem_listo;
    for (f = 0; f < FICHAS_POR_JUGADOR; f++)
        sem_init(&sem_turno[f], 0, 0);
    sem_init(&sem_listo, 0, 0);

    pthread_t hilos[FICHAS_POR_JUGADOR];
    ArgsFicha args[FICHAS_POR_JUGADOR];

    for (f = 0; f < FICHAS_POR_JUGADOR; f++) {
        args[f].color = color;
        args[f].id_ficha = f;
        args[f].tablero = tablero;
        args[f].sem_turno = &sem_turno[f];
        args[f].sem_listo = &sem_listo;
        args[f].accion = ACCION_MOVER;
        args[f].msq_id = msq_id;
        pthread_create(&hilos[f], NULL, ficha_hilo, &args[f]);
    }

    /* Cada jugador es un proceso: getpid() basta para sembrar su dado */
    unsigned int semilla = (unsigned int)getpid();

    int32_t msg;
    while (1) {
        if (leer_exacto(sockfd, &msg, sizeof(msg)) <= 0 || msg == MSG_FIN_JUEGO) {
            tablero->partida_activa = 0;
            for (f = 0; f < FICHAS_POR_JUGADOR; f++) sem_post(&sem_turno[f]);
            for (f = 0; f < FICHAS_POR_JUGADOR; f++) sem_wait(&sem_listo);
            break;
        }

        /* Reinicia los dados con las fichas dormidas (sin carrera) */
        for (f = 0; f < FICHAS_POR_JUGADOR; f++)
            tablero->ultimo_dado[color][f] = 0;

        /* Un turno puede encadenar varias tiradas por los 6 (reglas 3 y 4).
         * El bucle siempre termina: solo un 6-contado-como-6 repite, y a lo
         * sumo se permiten MAX_SEISES seguidos. */
        int seises = 0;
        int ult_movida = SIN_FICHA;  /* última ficha que efectivamente movió */

        while (1) {
            int dado = (rand_r(&semilla) % 6) + 1;

            /* Regla 4: con las 4 fichas fuera del nido, el 6 vale 7 y se juega
             * como un 7 normal (no repite tirada ni cuenta como seis). */
            int hay_en_nido = 0;
            for (f = 0; f < FICHAS_POR_JUGADOR; f++)
                if (tablero->fichas[color][f].estado == EN_BASE) { hay_en_nido = 1; break; }

            int valor = dado;
            int es_seis = 0;
            if (dado == 6) {
                if (hay_en_nido) es_seis = 1;
                else valor = 7;  /* regla 4 */
            }

            /* Regla 3: al tercer 6 seguido, la última ficha movida vuelve al
             * nido y el turno termina sin jugar ese 6. */
            if (es_seis) {
                seises++;
                if (seises == MAX_SEISES) {
                    if (ult_movida != SIN_FICHA) {
                        args[ult_movida].accion = ACCION_AL_NIDO;
                        sem_post(&sem_turno[ult_movida]);
                        sem_wait(&sem_listo);
                    }
                    break;
                }
            }

            /* Regla 7: con un 6 (contado como 6), si hay barrera propia con una
             * ficha que pueda mover, el jugador está OBLIGADO a moverla; si no,
             * rige la política normal de la regla 2. */
            int elegida = SIN_FICHA;
            if (es_seis)
                elegida = elegir_ficha_rompe_barrera(tablero, color, valor);
            if (elegida == SIN_FICHA)
                elegida = elegir_ficha(tablero, color, valor);
            if (elegida != SIN_FICHA) {
                tablero->ultimo_dado[color][elegida] = valor;
                args[elegida].accion = ACCION_MOVER;
                sem_post(&sem_turno[elegida]);  /* solo despierta a la elegida */
                sem_wait(&sem_listo);           /* espera a que termine su jugada */
                ult_movida = elegida;

                /* Bonificaciones: capturar da +20 (regla 5), meter en meta da +10
                 * (regla 8), ambas con la política de la regla 2. Cada bono puede
                 * encadenar otro (captura o meta); se itera con límite de seguridad. */
                int bono = args[elegida].capturo ? BONO_CAPTURA
                         : args[elegida].metio   ? BONO_META : 0;
                int bonos = 0;
                while (bono > 0 && bonos < MAX_BONOS_ENCADENADOS) {
                    int b = elegir_ficha(tablero, color, bono);
                    if (b == SIN_FICHA) break;  /* nadie puede usar el bono: se pierde */
                    tablero->ultimo_dado[color][b] = bono;
                    args[b].accion = ACCION_MOVER;
                    sem_post(&sem_turno[b]);
                    sem_wait(&sem_listo);
                    ult_movida = b;
                    bono = args[b].capturo ? BONO_CAPTURA
                         : args[b].metio   ? BONO_META : 0;
                    bonos++;
                }
            }
            /* Si ninguna ficha puede moverse, esa tirada pasa sin movimiento. */

            /* Regla 3: solo un 6 contado como 6 da derecho a repetir tirada. */
            if (!es_seis) break;
        }

        int32_t ok = MSG_TURNO_OK;
        send(sockfd, &ok, sizeof(ok), 0);
    }

    for (f = 0; f < FICHAS_POR_JUGADOR; f++)
        pthread_join(hilos[f], NULL);

    for (f = 0; f < FICHAS_POR_JUGADOR; f++)
        sem_destroy(&sem_turno[f]);
    sem_destroy(&sem_listo);
    close(sockfd);

    int metas = 0;
    for (f = 0; f < FICHAS_POR_JUGADOR; f++)
        if (tablero->fichas[color][f].estado == EN_META)
            metas++;

    pthread_mutex_lock(&tablero->mutex_impresion);
    printf("[jugador %-8s pid=%d] fichas en meta: %d/4%s\n",
           color_nombre(color), getpid(), metas,
           metas == FICHAS_POR_JUGADOR ? "  *** GANADOR ***" : "");
    pthread_mutex_unlock(&tablero->mutex_impresion);

    /* Envía stats por pipe (write atómico, cabe en PIPE_BUF) */
    EstadisticasJugador est;
    est.color = color;
    est.fichas_meta = metas;
    est.capturas_hechas = tablero->capturas_por_color[color];
    est.capturas_sufridas = tablero->capturas_sufridas[color];
    est.movimientos = tablero->movimientos[color];
    if (write(pipe_wr, &est, sizeof(est)) == -1)
        perror("write (estadisticas)");
    close(pipe_wr);

    exit(0);
}
