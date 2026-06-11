/* ficha.c — Hilo de ficha y selección de turno (reglas 1-2-5-6 de docs/REGLAS.md).
 *
 *   - Un turno = UNA tirada; el jugador la lanza y elige UNA ficha (elegir_ficha).
 *   - Solo la ficha elegida despierta y mueve; las demás siguen bloqueadas en su
 *     propio semáforo (sin busy-wait).
 *   - Regla 1: una ficha sale del nido solo con un 5, a la casilla de salida.
 *   - Política regla 2 (esta fase): capturar > sacar-con-5 > más-avanzada.
 *   - Regla 5: caer sobre UNA ficha rival en casilla normal la captura (rival al
 *     nido) y publica el evento por la cola; el +20 lo encadena el jugador.
 *   - Regla 6: en seguros no hay captura; los colores coexisten en la lista.
 *
 * La casilla aloja una lista de ocupantes; su mutex protege toda modificación.
 * Aún NO: repetición/penalización ya hecha (reglas 3-4); barreras ni meta.
 * Cada movimiento sostiene un solo mutex de casilla a la vez (anti-deadlock). */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include "ficha.h"

#define PASOS_AL_PASILLO 64

static const int SALIDAS[NUM_JUGADORES] = {
    SALIDA_ROJO, SALIDA_VERDE, SALIDA_AZUL, SALIDA_AMARILLO
};

static int es_seguro(int pos)
{
    int i;
    for (i = 0; i < NUM_SEGUROS; i++)
        if (CASILLAS_SEGURO[i] == pos) return 1;
    return 0;
}

/* ---- Helpers de la lista de ocupantes (el llamador sostiene el mutex al mutar) ---- */

static int contar_color(const Casilla *c, int color)
{
    int k, n = 0;
    for (k = 0; k < c->n_ocupantes; k++)
        if (c->ocupantes[k].color == color) n++;
    return n;
}

/* Regla 7: hay barrera si algún color aparece dos o más veces en la casilla
 * (sirve tanto en casilla normal como en seguro). */
static int casilla_tiene_barrera(const Casilla *c)
{
    int k, j;
    for (k = 0; k < c->n_ocupantes; k++)
        for (j = k + 1; j < c->n_ocupantes; j++)
            if (c->ocupantes[k].color == c->ocupantes[j].color)
                return 1;
    return 0;
}

static void casilla_agregar(Casilla *c, int color, int id)
{
    if (c->n_ocupantes < MAX_OCUPANTES_CASILLA) {
        c->ocupantes[c->n_ocupantes].color = color;
        c->ocupantes[c->n_ocupantes].id_ficha = id;
        c->n_ocupantes++;
    }
}

static void casilla_quitar(Casilla *c, int color, int id)
{
    int k;
    for (k = 0; k < c->n_ocupantes; k++) {
        if (c->ocupantes[k].color == color && c->ocupantes[k].id_ficha == id) {
            c->ocupantes[k] = c->ocupantes[c->n_ocupantes - 1];
            c->n_ocupantes--;
            return;
        }
    }
}

/* ¿Se puede aterrizar en pos con mi color? (sin pasar/capturar barreras)
 *   - seguro: hay cupo (coexisten colores);
 *   - normal vacía: sí;
 *   - normal solo con míos: coexisto si hay cupo (prepara barrera);
 *   - normal con rivales: solo si es exactamente 1 (captura). */
static int landing_legal(Tablero *t, int color, int pos)
{
    Casilla *c = &t->casillas[pos];
    int total = c->n_ocupantes;

    if (casilla_tiene_barrera(c))
        return 0;  /* regla 7: nadie (propio o rival) cae en una barrera */
    if (es_seguro(pos))
        return total < MAX_OCUPANTES_CASILLA;
    if (total == 0)
        return 1;
    if (contar_color(c, color) == total)
        return total < MAX_OCUPANTES_CASILLA;  /* formar barrera (ser la 2.ª) es legal */
    return total == 1;  /* un único rival → capturable */
}

/* Avance total para ordenar "más avanzada": común 0..63, pasillo 64..(64+5);
 * SIN_FICHA si la ficha no está en juego en el circuito. */
static int ficha_avance(const Ficha *f, int salida)
{
    if (f->estado != EN_RECORRIDO)
        return SIN_FICHA;
    if (f->en_pasillo)
        return PASOS_AL_PASILLO + f->pos_pasillo;
    return (f->posicion - salida + CASILLAS_COMUNES) % CASILLAS_COMUNES;
}

/* Casilla destino en el circuito común si la ficha avanza 'dado' sin entrar al
 * pasillo; SIN_FICHA si no aplica o entraría al pasillo. */
static int destino_comun(const Ficha *f, int salida, int dado)
{
    if (f->estado != EN_RECORRIDO || f->en_pasillo)
        return SIN_FICHA;
    int pasos = (f->posicion - salida + CASILLAS_COMUNES) % CASILLAS_COMUNES;
    int nuevos = pasos + dado;
    if (nuevos >= PASOS_AL_PASILLO)
        return SIN_FICHA;
    return (salida + nuevos) % CASILLAS_COMUNES;
}

/* Tipo de destino al avanzar 'dado' (regla 8: entrada a meta exacta). */
enum { DEST_ILEGAL = -1, DEST_COMUN = 0, DEST_PASILLO = 1, DEST_META = 2 };

/* Calcula el destino: devuelve el tipo y, por punteros, el índice (casilla común
 * o casilla de pasillo) y cuántas casillas COMUNES cruza (para verificar barreras).
 * DEST_ILEGAL si se pasa de meta (cuenta no exacta). */
static int calc_destino(const Ficha *f, int salida, int dado, int *idx, int *n_comun)
{
    *idx = SIN_FICHA;
    *n_comun = 0;

    if (f->estado == EN_RECORRIDO && !f->en_pasillo) {
        int pasos = (f->posicion - salida + CASILLAS_COMUNES) % CASILLAS_COMUNES;
        int nuevos = pasos + dado;
        if (nuevos < PASOS_AL_PASILLO) {
            *idx = (salida + nuevos) % CASILLAS_COMUNES;
            *n_comun = dado;
            return DEST_COMUN;
        }
        int pp = nuevos - PASOS_AL_PASILLO;          /* posición dentro del pasillo */
        *n_comun = (PASOS_AL_PASILLO - 1) - pasos;   /* comunes cruzadas antes del pasillo */
        if (pp < POS_META_EN_PASILLO) { *idx = pp; return DEST_PASILLO; }
        if (pp == POS_META_EN_PASILLO) return DEST_META;
        return DEST_ILEGAL;                          /* se pasa de meta */
    }

    if (f->estado == EN_RECORRIDO && f->en_pasillo) {
        int np = f->pos_pasillo + dado;
        if (np < POS_META_EN_PASILLO) { *idx = np; return DEST_PASILLO; }
        if (np == POS_META_EN_PASILLO) return DEST_META;
        return DEST_ILEGAL;
    }

    return DEST_ILEGAL;
}

/* ¿Hay ya una ficha de este color en su pasillo? (sem_pasillo admite solo una) */
static int pasillo_ocupado(Tablero *t, int color)
{
    int i;
    for (i = 0; i < FICHAS_POR_JUGADOR; i++)
        if (t->fichas[color][i].estado == EN_RECORRIDO && t->fichas[color][i].en_pasillo)
            return 1;
    return 0;
}

/* Regla 7: ¿hay una barrera en las primeras 'n' casillas COMUNES del camino
 * (posicion, posicion+n]? Lee cada casilla con un bloqueo corto e independiente,
 * sin sostener nunca dos mutex a la vez. El origen no cuenta. */
static int camino_bloqueado(Tablero *t, int posicion, int n)
{
    int s;
    for (s = 1; s <= n; s++) {
        int p = (posicion + s) % CASILLAS_COMUNES;
        Casilla *c = &t->casillas[p];
        pthread_mutex_lock(&c->mutex);
        int barrera = casilla_tiene_barrera(c);
        pthread_mutex_unlock(&c->mutex);
        if (barrera)
            return 1;
    }
    return 0;
}

/* ¿Puede esta ficha mover legalmente con ese dado? (regla 8: meta exacta) */
static int puede_mover(Tablero *t, int color, int id, int dado)
{
    Ficha *f = &t->fichas[color][id];
    int salida = SALIDAS[color];

    if (f->estado == EN_BASE)  /* regla 1: solo un 5, y la salida (seguro) con cupo */
        return dado == 5 && landing_legal(t, color, salida);
    if (f->estado != EN_RECORRIDO)
        return 0;

    int idx, n_comun;
    int tipo = calc_destino(f, salida, dado, &idx, &n_comun);
    if (tipo == DEST_ILEGAL)
        return 0;
    if (camino_bloqueado(t, f->posicion, n_comun))  /* regla 7 */
        return 0;

    if (tipo == DEST_COMUN)
        return landing_legal(t, color, idx);

    /* Entrar al pasillo desde el común exige el pasillo libre (sem_pasillo). */
    if (!f->en_pasillo && pasillo_ocupado(t, color))
        return 0;
    if (tipo == DEST_PASILLO)
        return t->pasillos[color][idx].n_ocupantes == 0;
    return 1;  /* DEST_META con cuenta exacta */
}

/* ¿El movimiento de esta ficha con ese dado caería sobre un único rival (captura)? */
static int mov_captura(Tablero *t, int color, int id, int dado)
{
    Ficha *f = &t->fichas[color][id];
    int dest = destino_comun(f, SALIDAS[color], dado);
    if (dest == SIN_FICHA || es_seguro(dest))
        return 0;
    if (camino_bloqueado(t, f->posicion, dado))  /* regla 7: no se cruza una barrera */
        return 0;
    Casilla *c = &t->casillas[dest];
    return c->n_ocupantes == 1 && c->ocupantes[0].color != color;
}

/* ¿El movimiento mete la ficha en meta con cuenta exacta? (regla 8) */
static int mov_meta(Tablero *t, int color, int id, int dado)
{
    Ficha *f = &t->fichas[color][id];
    if (f->estado != EN_RECORRIDO)
        return 0;
    int idx, n_comun;
    if (calc_destino(f, SALIDAS[color], dado, &idx, &n_comun) != DEST_META)
        return 0;
    return puede_mover(t, color, id, dado);
}

/* Política determinista de la regla 2:
 *   1) capturar (la más avanzada que capture);
 *   2) meter una ficha en meta con cuenta exacta;
 *   3) sacar del nido si el dado es 5;
 *   4) la ficha más avanzada que pueda moverse (común o pasillo).
 * Devuelve el id elegido o SIN_FICHA si ninguna puede. */
int elegir_ficha(Tablero *t, int color, int dado)
{
    int id, elegida, mejor;

    /* 1) Captura, prefiriendo la ficha más avanzada que capture. */
    elegida = SIN_FICHA; mejor = -1;
    for (id = 0; id < FICHAS_POR_JUGADOR; id++) {
        if (!mov_captura(t, color, id, dado))
            continue;
        int avance = ficha_avance(&t->fichas[color][id], SALIDAS[color]);
        if (avance > mejor) { mejor = avance; elegida = id; }
    }
    if (elegida != SIN_FICHA)
        return elegida;

    /* 2) Meter una ficha en meta (cuenta exacta). */
    elegida = SIN_FICHA; mejor = -1;
    for (id = 0; id < FICHAS_POR_JUGADOR; id++) {
        if (!mov_meta(t, color, id, dado))
            continue;
        int avance = ficha_avance(&t->fichas[color][id], SALIDAS[color]);
        if (avance > mejor) { mejor = avance; elegida = id; }
    }
    if (elegida != SIN_FICHA)
        return elegida;

    /* 3) Sacar una ficha del nido con un 5. */
    if (dado == 5) {
        for (id = 0; id < FICHAS_POR_JUGADOR; id++) {
            Ficha *f = &t->fichas[color][id];
            if (f->estado == EN_BASE && puede_mover(t, color, id, dado))
                return id;
        }
    }

    /* 4) La ficha más avanzada que pueda moverse (común o pasillo). */
    elegida = SIN_FICHA; mejor = -1;
    for (id = 0; id < FICHAS_POR_JUGADOR; id++) {
        if (t->fichas[color][id].estado != EN_RECORRIDO)
            continue;
        if (!puede_mover(t, color, id, dado))
            continue;
        int avance = ficha_avance(&t->fichas[color][id], SALIDAS[color]);
        if (avance > mejor) { mejor = avance; elegida = id; }
    }
    return elegida;
}

/* ¿La ficha forma parte de una barrera propia? (su casilla tiene ≥2 de su color) */
static int ficha_en_barrera(Tablero *t, int color, int id)
{
    Ficha *f = &t->fichas[color][id];
    if (f->estado != EN_RECORRIDO || f->en_pasillo)
        return 0;
    return contar_color(&t->casillas[f->posicion], color) >= 2;
}

/* Regla 7: con un 6, si el jugador tiene una barrera propia, está OBLIGADO a mover
 * una de sus fichas si el movimiento es legal. Elige entre las fichas de barrera
 * con el orden de la regla 2 (capturar > más avanzada); SIN_FICHA si ninguna de
 * ellas puede mover legalmente (la obligación se anula y vuelve la política normal). */
int elegir_ficha_rompe_barrera(Tablero *t, int color, int dado)
{
    int id, elegida, mejor;

    elegida = SIN_FICHA; mejor = -1;
    for (id = 0; id < FICHAS_POR_JUGADOR; id++) {
        if (!ficha_en_barrera(t, color, id))
            continue;
        if (!mov_captura(t, color, id, dado))
            continue;
        int avance = ficha_avance(&t->fichas[color][id], SALIDAS[color]);
        if (avance > mejor) { mejor = avance; elegida = id; }
    }
    if (elegida != SIN_FICHA)
        return elegida;

    elegida = SIN_FICHA; mejor = -1;
    for (id = 0; id < FICHAS_POR_JUGADOR; id++) {
        if (!ficha_en_barrera(t, color, id))
            continue;
        if (!puede_mover(t, color, id, dado))
            continue;
        int avance = ficha_avance(&t->fichas[color][id], SALIDAS[color]);
        if (avance > mejor) { mejor = avance; elegida = id; }
    }
    return elegida;
}

/* Ejecuta el movimiento de la ficha ya elegida con el dado del turno. El jugador
 * garantizó legalidad, pero el mutex de la casilla destino protege la escritura
 * igual. Devuelve 1 si movió; *capturo=1 si capturó (regla 5); *metio=1 si entró
 * en meta (regla 8). Un solo mutex de casilla a la vez. */
static int realizar_movimiento(Tablero *t, int color, int id, int dado,
                               Ficha *f, int *capturo, int *metio, int msq_id)
{
    int salida = SALIDAS[color];
    *capturo = 0;
    *metio = 0;

    /* EN NIDO: solo un 5 saca la ficha a su casilla de salida (seguro). */
    if (f->estado == EN_BASE) {
        if (dado != 5) return 0;

        int movio = 0;
        Casilla *dest = &t->casillas[salida];
        pthread_mutex_lock(&dest->mutex);
        if (dest->n_ocupantes < MAX_OCUPANTES_CASILLA) {
            casilla_agregar(dest, color, id);
            f->estado = EN_RECORRIDO;
            f->posicion = salida;
            f->en_pasillo = 0;
            f->pos_pasillo = -1;
            movio = 1;
        }
        pthread_mutex_unlock(&dest->mutex);
        return movio;
    }
    if (f->estado != EN_RECORRIDO)
        return 0;

    int idx, n_comun;
    int tipo = calc_destino(f, salida, dado, &idx, &n_comun);
    if (tipo == DEST_ILEGAL)
        return 0;
    /* Regla 7: barrera en el tramo común, comprobada antes de tomar mutex alguno. */
    if (camino_bloqueado(t, f->posicion, n_comun))
        return 0;

    /* --- Avance dentro del circuito común (incluye captura, regla 5) --- */
    if (tipo == DEST_COMUN) {
        int nueva_pos = idx;
        int orig = f->posicion;
        int seguro = es_seguro(nueva_pos);
        int movio = 0;
        int cap_color = SIN_FICHA, cap_id = SIN_FICHA;

        Casilla *dest = &t->casillas[nueva_pos];
        pthread_mutex_lock(&dest->mutex);

        int total = dest->n_ocupantes;
        int permitido;
        if (casilla_tiene_barrera(dest))
            permitido = 0;  /* regla 7: no se cae en una barrera */
        else if (seguro)
            permitido = (total < MAX_OCUPANTES_CASILLA);
        else if (total == 0)
            permitido = 1;
        else if (contar_color(dest, color) == total)
            permitido = (total < MAX_OCUPANTES_CASILLA);
        else
            permitido = (total == 1);  /* un único rival → captura */

        if (permitido) {
            /* Regla 5: captura solo en casilla normal con exactamente un rival. */
            if (!seguro && total == 1 && dest->ocupantes[0].color != color) {
                cap_color = dest->ocupantes[0].color;
                cap_id = dest->ocupantes[0].id_ficha;

                Ficha *rival = &t->fichas[cap_color][cap_id];
                rival->estado = EN_BASE;
                rival->posicion = POS_EN_CASA;
                rival->en_pasillo = 0;
                rival->pos_pasillo = -1;

                casilla_quitar(dest, cap_color, cap_id);
                *capturo = 1;
            }
            casilla_agregar(dest, color, id);
            /* Origen quieto durante mi turno: lo actualizo sosteniendo solo el
             * mutex del destino (un único mutex a la vez). */
            casilla_quitar(&t->casillas[orig], color, id);
            f->posicion = nueva_pos;
            movio = 1;
        }
        pthread_mutex_unlock(&dest->mutex);

        if (*capturo) {
            pthread_mutex_lock(&t->mutex_impresion);
            t->capturas_total++;
            t->capturas_por_color[color]++;
            t->capturas_sufridas[cap_color]++;
            pthread_mutex_unlock(&t->mutex_impresion);

            /* Publica el evento por la cola (ya existente). */
            MsgCaptura ev;
            ev.mtype = (long)(cap_color + 1);
            ev.capturador = color;
            ev.victima = cap_color;
            ev.id_victima = cap_id;
            ev.casilla = nueva_pos;
            if (msgsnd(msq_id, &ev, sizeof(MsgCaptura) - sizeof(long), 0) == -1)
                perror("msgsnd (captura)");
        }
        return movio;
    }

    /* --- Pasillo y meta (regla 8). Respeta sem_pasillo (una ficha por pasillo)
     * y sem_meta (serializa llegadas). Si la ficha viene del común, toma el
     * pasillo con sem_trywait; lo libera al llegar a meta. --- */
    int desde_comun = !f->en_pasillo;
    if (desde_comun) {
        if (sem_trywait(&t->sem_pasillo[color]) != 0)
            return 0;  /* el pasillo ya está ocupado: no puede entrar */
    }

    if (tipo == DEST_PASILLO) {
        int movio = 0;
        Casilla *dest = &t->pasillos[color][idx];
        pthread_mutex_lock(&dest->mutex);
        if (dest->n_ocupantes == 0) {
            if (desde_comun)
                casilla_quitar(&t->casillas[f->posicion], color, id);
            else
                casilla_quitar(&t->pasillos[color][f->pos_pasillo], color, id);
            casilla_agregar(dest, color, id);
            f->en_pasillo = 1;
            f->posicion = POS_EN_CASA;   /* fuera del circuito común */
            f->pos_pasillo = idx;
            movio = 1;
        }
        pthread_mutex_unlock(&dest->mutex);
        if (!movio && desde_comun)
            sem_post(&t->sem_pasillo[color]);  /* no avanzó: devuelve el pasillo */
        return movio;
    }

    /* tipo == DEST_META: llegada exacta a meta. */
    sem_wait(&t->sem_meta);

    Casilla *orig_c = desde_comun ? &t->casillas[f->posicion]
                                  : &t->pasillos[color][f->pos_pasillo];
    pthread_mutex_lock(&orig_c->mutex);
    casilla_quitar(orig_c, color, id);
    pthread_mutex_unlock(&orig_c->mutex);

    f->estado = EN_META;
    f->en_pasillo = 0;
    f->posicion = POS_EN_CASA;
    f->pos_pasillo = -1;

    sem_post(&t->sem_meta);
    sem_post(&t->sem_pasillo[color]);  /* la ficha abandona el pasillo: queda libre */

    pthread_mutex_lock(&t->mutex_impresion);
    t->fichas_en_meta[color]++;
    pthread_mutex_unlock(&t->mutex_impresion);

    *metio = 1;
    return 1;
}

/* Regla 3: la ficha vuelve al nido. Libera bajo mutex la casilla que ocupa. */
static void ficha_al_nido(Tablero *t, int color, int id, Ficha *f)
{
    if (f->estado == EN_RECORRIDO && !f->en_pasillo) {
        Casilla *c = &t->casillas[f->posicion];
        pthread_mutex_lock(&c->mutex);
        casilla_quitar(c, color, id);
        pthread_mutex_unlock(&c->mutex);
    }
    f->estado = EN_BASE;
    f->posicion = POS_EN_CASA;
    f->en_pasillo = 0;
    f->pos_pasillo = -1;
}

/* Bucle del hilo: bloquea en su propio sem_turno (sin busy-wait); solo lo postea
 * el jugador si esta ficha fue la elegida del turno. Avisa con sem_listo al acabar
 * y deja en a->capturo si su movimiento capturó (para el +20 del jugador). */
void *ficha_hilo(void *arg)
{
    ArgsFicha *a = (ArgsFicha *)arg;
    Tablero *t = a->tablero;
    int color = a->color;
    int id = a->id_ficha;
    Ficha *f = &t->fichas[color][id];

    while (1) {
        sem_wait(a->sem_turno);

        if (t->partida_activa == 0) {
            sem_post(a->sem_listo);
            break;
        }

        a->capturo = 0;
        a->metio = 0;
        if (a->accion == ACCION_AL_NIDO) {
            ficha_al_nido(t, color, id, f);
        } else {
            /* El jugador ya dejó el dado del turno en ultimo_dado para esta ficha. */
            int dado = t->ultimo_dado[color][id];
            int capturo = 0, metio = 0;
            int movio = realizar_movimiento(t, color, id, dado, f,
                                            &capturo, &metio, a->msq_id);
            a->capturo = capturo;
            a->metio = metio;
            if (movio) {
                pthread_mutex_lock(&t->mutex_impresion);
                t->movimientos[color]++;
                pthread_mutex_unlock(&t->mutex_impresion);
            }
        }

        sem_post(a->sem_listo);
    }

    return NULL;
}
