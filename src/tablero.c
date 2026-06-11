#include <sys/mman.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <string.h>
#include "tablero.h"

/* Inicializa una casilla vacía con mutex PROCESS_SHARED */
static void init_casilla(Casilla *c)
{
    c->n_ocupantes = 0;

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&c->mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}

Tablero *tablero_crear(void)
{
    /* MAP_SHARED|MAP_ANONYMOUS: segmento heredado por hijos tras fork */
    Tablero *t = mmap(NULL, sizeof(Tablero),
                      PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS,
                      -1, 0);
    if (t == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }
    return t;
}

void tablero_init(Tablero *t)
{
    int c, f, p;

    for (c = 0; c < CASILLAS_COMUNES; c++)
        init_casilla(&t->casillas[c]);

    for (c = 0; c < NUM_JUGADORES; c++)
        for (p = 0; p < CASILLAS_PASILLO_FINAL; p++)
            init_casilla(&t->pasillos[c][p]);

    for (c = 0; c < NUM_JUGADORES; c++) {
        for (f = 0; f < FICHAS_POR_JUGADOR; f++) {
            t->fichas[c][f].estado = EN_BASE;
            t->fichas[c][f].posicion = POS_EN_CASA;
            t->fichas[c][f].en_pasillo = 0;
            t->fichas[c][f].pos_pasillo = -1;
            t->ultimo_dado[c][f] = 0;
        }
        t->fichas_en_meta[c] = 0;
        t->capturas_por_color[c] = 0;
        t->capturas_sufridas[c] = 0;
        t->movimientos[c] = 0;
        t->jugadores[c].pid = -1;
        t->jugadores[c].color = (Color)c;
    }

    t->capturas_total = 0;
    t->turno_actual = 0;
    t->partida_activa = 1;

    pthread_mutexattr_t attr_imp;
    pthread_mutexattr_init(&attr_imp);
    pthread_mutexattr_setpshared(&attr_imp, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&t->mutex_impresion, &attr_imp);
    pthread_mutexattr_destroy(&attr_imp);

    /* sem_pasillo: una ficha por pasillo; sem_meta: serializa llegadas a meta */
    for (c = 0; c < NUM_JUGADORES; c++)
        sem_init(&t->sem_pasillo[c], 1, 1);
    sem_init(&t->sem_meta, 1, AFORO_META);
}

void tablero_destruir(Tablero *t)
{
    int c, p;

    for (c = 0; c < CASILLAS_COMUNES; c++)
        pthread_mutex_destroy(&t->casillas[c].mutex);

    for (c = 0; c < NUM_JUGADORES; c++)
        for (p = 0; p < CASILLAS_PASILLO_FINAL; p++)
            pthread_mutex_destroy(&t->pasillos[c][p].mutex);

    pthread_mutex_destroy(&t->mutex_impresion);

    for (c = 0; c < NUM_JUGADORES; c++)
        sem_destroy(&t->sem_pasillo[c]);
    sem_destroy(&t->sem_meta);

    munmap(t, sizeof(Tablero));
}

const char *color_nombre(int color)
{
    static const char *nombres[] = {"ROJO", "VERDE", "AZUL", "AMARILLO"};
    if (color < 0 || color >= NUM_JUGADORES)
        return "?";
    return nombres[color];
}

#define LADO 19

static const int ARM0[17][2] = {
    {0,8},{0,9},{0,10},{1,10},{2,10},{3,10},{4,10},{5,10},{6,10},
    {7,10},{8,11},{8,12},{8,13},{8,14},{8,15},{8,16},{8,17}
};

static void rotar_horario(int *fila, int *col)
{
    int nf = *col;
    int nc = LADO - 1 - *fila;
    *fila = nf;
    *col = nc;
}

static void coord_casilla(int idx, int *fila, int *col)
{
    int brazo = idx / 17;
    int off = idx % 17;
    *fila = ARM0[off][0];
    *col = ARM0[off][1];
    for (int k = 0; k < brazo; k++)
        rotar_horario(fila, col);
}

static void coord_pasillo(int color, int p, int *fila, int *col)
{
    *fila = 1 + p;
    *col = 9;
    for (int k = 0; k < color; k++)
        rotar_horario(fila, col);
}

static char inicial_color(int color)
{
    const char *iniciales = "RVAM";
    if (color < 0 || color >= NUM_JUGADORES)
        return '?';
    return iniciales[color];
}

static const char *ansi_color(int color)
{
    switch (color) {
        case ROJO:     return "\033[1;31m";
        case VERDE:    return "\033[1;32m";
        case AZUL:     return "\033[1;34m";
        case AMARILLO: return "\033[1;33m";
        default:       return "\033[0m";
    }
}

void tablero_dibujar(Tablero *t, int turno, int ronda,
                     char eventos[][96], int n_eventos)
{
    char etiqueta[LADO][LADO][4];
    int color[LADO][LADO];
    int f, c, i, p;

    for (f = 0; f < LADO; f++)
        for (c = 0; c < LADO; c++) {
            etiqueta[f][c][0] = '\0';
            color[f][c] = -1;
        }

    for (i = 0; i < CASILLAS_COMUNES; i++) {
        int es_seg = 0, s;
        for (s = 0; s < NUM_SEGUROS; s++)
            if (CASILLAS_SEGURO[s] == i) { es_seg = 1; break; }
        coord_casilla(i, &f, &c);
        strcpy(etiqueta[f][c], es_seg ? "+" : ".");  /* '+' marca casilla de seguro */
        color[f][c] = -2;
    }

    for (i = 0; i < NUM_JUGADORES; i++)
        for (p = 0; p < CASILLAS_PASILLO_FINAL; p++) {
            coord_pasillo(i, p, &f, &c);
            strcpy(etiqueta[f][c], ".");
            color[f][c] = -2;
        }

    static const int base_ancla[NUM_JUGADORES][2] = {
        {2, 2}, {2, 15}, {15, 15}, {15, 2}
    };

    int meta_pos[NUM_JUGADORES][2] = { {8,9}, {9,10}, {10,9}, {9,8} };
    for (i = 0; i < NUM_JUGADORES; i++) {
        snprintf(etiqueta[meta_pos[i][0]][meta_pos[i][1]], 4, "%c%d",
                 inicial_color(i), t->fichas_en_meta[i]);
        color[meta_pos[i][0]][meta_pos[i][1]] = i;
    }
    strcpy(etiqueta[9][9], "*");
    color[9][9] = -2;

    for (i = 0; i < NUM_JUGADORES; i++) {
        int en_base = 0;
        for (p = 0; p < FICHAS_POR_JUGADOR; p++) {
            Ficha *fi = &t->fichas[i][p];
            if (fi->estado == EN_RECORRIDO && !fi->en_pasillo) {
                coord_casilla(fi->posicion, &f, &c);
            } else if (fi->estado == EN_RECORRIDO && fi->en_pasillo) {
                coord_pasillo(i, fi->pos_pasillo, &f, &c);
            } else if (fi->estado == EN_BASE) {
                f = base_ancla[i][0] + (en_base / 2);
                c = base_ancla[i][1] + (en_base % 2);
                en_base++;
            } else {
                continue;
            }
            snprintf(etiqueta[f][c], 4, "%c%d", inicial_color(i), p + 1);
            color[f][c] = i;
        }
    }

    printf("\033[2J\033[H");
    printf("  ╔═══════════════ TORNEO DE PARCHÍS ═══════════════╗\n");
    printf("  Ronda %-4d   Turno: %s%s\033[0m   Meta: ", ronda,
           ansi_color(turno), color_nombre(turno));
    for (i = 0; i < NUM_JUGADORES; i++)
        printf("%s%c:%d\033[0m ", ansi_color(i), inicial_color(i),
               t->fichas_en_meta[i]);
    printf("\n");

    printf("  %sDados:\033[0m ", ansi_color(turno));
    for (i = 0; i < FICHAS_POR_JUGADOR; i++) {
        int d = t->ultimo_dado[turno][i];
        if (d == 0)
            printf("  %s%c%d:-\033[0m", ansi_color(turno), inicial_color(turno), i + 1);
        else
            printf("  %s%c%d:%d\033[0m", ansi_color(turno), inicial_color(turno), i + 1, d);
    }
    printf("\n\n");

    for (f = 0; f < LADO; f++) {
        printf("  ");
        for (c = 0; c < LADO; c++) {
            if (etiqueta[f][c][0] == '\0') {
                printf("   ");
            } else if (color[f][c] == -2) {
                printf("\033[2m%3s\033[0m", etiqueta[f][c]);
            } else {
                printf("%s%3s\033[0m", ansi_color(color[f][c]), etiqueta[f][c]);
            }
        }
        printf("\n");
    }

    printf("\n  Leyenda: %sR\033[0m=ROJO %sV\033[0m=VERDE %sA\033[0m=AZUL "
           "%sM\033[0m=AMARILLO   +=seguro\n",
           ansi_color(ROJO), ansi_color(VERDE), ansi_color(AZUL),
           ansi_color(AMARILLO));

    printf("  Capturas recientes:\n");
    if (n_eventos == 0) {
        printf("    (ninguna aún)\n");
    } else {
        for (i = 0; i < n_eventos; i++)
            printf("    • %s\n", eventos[i]);
    }
    fflush(stdout);
}

void tablero_imprimir_posiciones(Tablero *t)
{
    static const char *estado_str[] = {"EN_BASE", "EN_RECORRIDO", "EN_META"};
    int c, f;

    pthread_mutex_lock(&t->mutex_impresion);
    printf("=== Posiciones iniciales ===\n");
    for (c = 0; c < NUM_JUGADORES; c++) {
        printf("  %-8s:", color_nombre(c));
        for (f = 0; f < FICHAS_POR_JUGADOR; f++)
            printf(" ficha%d=%-12s", f,
                   estado_str[(int)t->fichas[c][f].estado]);
        printf("\n");
    }
    printf("============================\n");
    pthread_mutex_unlock(&t->mutex_impresion);
}
