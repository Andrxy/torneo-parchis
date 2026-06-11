#ifndef JUGADOR_H
#define JUGADOR_H

#include "parchis.h"

void jugador_ejecutar(int color, Tablero *tablero, int srv_fd, int msq_id, int pipe_wr);

#endif