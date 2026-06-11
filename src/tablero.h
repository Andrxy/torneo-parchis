#ifndef TABLERO_H
#define TABLERO_H

#include "parchis.h"

Tablero *tablero_crear(void);      /* reserva tablero en memoria compartida */
void tablero_init(Tablero *t);     /* inicializa casillas, fichas e IPC PROCESS_SHARED */
void tablero_destruir(Tablero *t); /* destruye IPC y libera memoria; solo el padre */
const char *color_nombre(int color);
void tablero_imprimir_posiciones(Tablero *t);
void tablero_dibujar(Tablero *t, int turno, int ronda, char eventos[][96], int n_eventos);

#endif