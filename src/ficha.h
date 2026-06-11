#ifndef FICHA_H
#define FICHA_H

#include <semaphore.h>
#include "parchis.h"

/* Acción que el jugador encarga a la ficha al despertarla en su turno. */
typedef enum {
    ACCION_MOVER = 0,    /* mover con el valor dejado en ultimo_dado */
    ACCION_AL_NIDO = 1   /* regla 3: penalización, la ficha vuelve al nido */
} AccionFicha;

typedef struct {
    int color;
    int id_ficha;
    Tablero *tablero;
    sem_t *sem_turno;  /* propio de la ficha: solo se postea si fue la elegida */
    sem_t *sem_listo;  /* compartido: la ficha avisa al jugador que terminó */
    AccionFicha accion; /* fijada por el jugador antes de postear sem_turno */
    int capturo;        /* resultado: 1 si el movimiento capturó (para el +20) */
    int metio;          /* resultado: 1 si el movimiento entró en meta (para el +10) */
    int msq_id;
} ArgsFicha;

void *ficha_hilo(void *arg);

/* Política determinista de la regla 2 (capturar > sacar-con-5 > más-avanzada).
 * Devuelve el id de la ficha que debe mover con ese dado, o SIN_FICHA si ninguna. */
int elegir_ficha(Tablero *t, int color, int dado);

/* Regla 7: con un 6, ficha de barrera propia obligada a moverse (o SIN_FICHA). */
int elegir_ficha_rompe_barrera(Tablero *t, int color, int dado);

#endif
