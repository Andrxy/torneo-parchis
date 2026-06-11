#ifndef PARCHIS_H
#define PARCHIS_H

#include <pthread.h>
#include <semaphore.h>
#include <sys/types.h>

#define NUM_JUGADORES 4
#define FICHAS_POR_JUGADOR 4

typedef enum {
    ROJO = 0,
    VERDE = 1,
    AZUL = 2,
    AMARILLO = 3
} Color;

/* 68 casillas comunes (0-67) y pasillo final de 7 por color (0-6) */
#define CASILLAS_COMUNES 68
#define CASILLAS_PASILLO_FINAL 7

#define POS_EN_CASA -1

#define SALIDA_ROJO 4
#define SALIDA_VERDE 21
#define SALIDA_AZUL 38
#define SALIDA_AMARILLO 55

#define ENTRADA_PASILLO_ROJO 68
#define ENTRADA_PASILLO_VERDE 17
#define ENTRADA_PASILLO_AZUL 34
#define ENTRADA_PASILLO_AMARILLO 51

#define POS_META_EN_PASILLO (CASILLAS_PASILLO_FINAL - 1)

#define AFORO_META 1

#define PUERTO_ARBITRO 5050

#define MAX_RONDAS 500

#define RETARDO_RONDA_US 120000

/* Regla 3: tres 6 seguidos en un turno penalizan a la última ficha movida */
#define MAX_SEISES 3

/* Bonificaciones (movimientos extra) */
#define BONO_CAPTURA 20  /* regla 5: tras capturar, avanzar 20 */
#define BONO_META 10     /* regla 8: tras meter en meta, avanzar 10 (fase futura) */

/* Tope de bonos +20 encadenados en un turno (evita bucles infinitos) */
#define MAX_BONOS_ENCADENADOS 32

typedef enum {
    MSG_TU_TURNO = 1,
    MSG_TURNO_OK = 2,
    MSG_FIN_JUEGO = 3
} MsgControl;

#define NUM_SEGUROS 8
extern const int CASILLAS_SEGURO[NUM_SEGUROS];

typedef enum {
    EN_BASE = 0,
    EN_RECORRIDO = 1,
    EN_META = 2
} EstadoFicha;

typedef struct {
    EstadoFicha estado;
    int posicion;    /* casilla común 0-67; POS_EN_CASA si EN_BASE */
    int en_pasillo;  /* 1 si entró al pasillo final */
    int pos_pasillo; /* 0-6 en el pasillo; -1 si no está en él */
} Ficha;

#define SIN_FICHA -1

/* Una casilla aloja una LISTA de ocupantes (regla 6: en seguros coexisten colores
 * distintos; en casilla normal, hasta 2 del mismo color = barrera). */
#define MAX_OCUPANTES_CASILLA 4

typedef struct {
    int color;
    int id_ficha;
} Ocupante;

typedef struct {
    Ocupante ocupantes[MAX_OCUPANTES_CASILLA];
    int n_ocupantes;
    pthread_mutex_t mutex; /* protege TODA lectura/escritura de la lista (PROCESS_SHARED) */
} Casilla;

typedef struct {
    pid_t pid;
    Color color;
} InfoJugador;

/* Tablero completo — vive en memoria compartida */
typedef struct {
    Casilla casillas[CASILLAS_COMUNES];
    Casilla pasillos[NUM_JUGADORES][CASILLAS_PASILLO_FINAL];
    Ficha fichas[NUM_JUGADORES][FICHAS_POR_JUGADOR];

    int ultimo_dado[NUM_JUGADORES][FICHAS_POR_JUGADOR];

    InfoJugador jugadores[NUM_JUGADORES];

    int capturas_total;
    int capturas_por_color[NUM_JUGADORES];
    int capturas_sufridas[NUM_JUGADORES];
    int movimientos[NUM_JUGADORES];
    int fichas_en_meta[NUM_JUGADORES];

    int turno_actual;
    int partida_activa;

    pthread_mutex_t mutex_impresion; /* protege printf y contadores (PROCESS_SHARED) */

    /* pshared=1: una ficha por pasillo, meta serializada */
    sem_t sem_pasillo[NUM_JUGADORES];
    sem_t sem_meta;
} Tablero;

#define MTYPE_FIN_COLA 99L

typedef struct {
    long mtype; /* victima+1 (1-4), o MTYPE_FIN_COLA al terminar */
    int capturador;
    int victima;
    int id_victima;
    int casilla;
} MsgCaptura;

/* Estadísticas enviadas por pipe al árbitro al terminar (cabe en PIPE_BUF) */
typedef struct {
    int color;
    int fichas_meta;
    int capturas_hechas;
    int capturas_sufridas;
    int movimientos;
} EstadisticasJugador;

#endif
