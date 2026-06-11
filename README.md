# Torneo de Parchís - GRUPO 5

Este README cubre **únicamente cómo correr el proyecto con el `Makefile`**.

---

## Targets del Makefile

| Comando | Qué hace |
|---------|----------|
| `make` (= `make all`) | Compila el binario en **`bin/parchis`**. |
| `make run` | Compila (si hace falta) y ejecuta el juego. |
| `make clean` | Borra el directorio `bin/` con el binario y los `.o`. |

---

## Compilar

```bash
make
```

Genera el binario en **`bin/parchis`**.

---

## Ejecutar

```bash
make run        # compila si hace falta y lanza el juego
```

o, si ya está compilado:

```bash
./bin/parchis
```

No recibe argumentos y se ejecuta en **una sola terminal**: el árbitro hace `fork()` de los 4
jugadores, no hay que abrir procesos cliente por separado. El tablero se redibuja tras cada
turno y, al acabar la partida, se imprime la tabla de estadísticas finales.

Para detener a mitad de partida: **Ctrl + C**. El árbitro captura la señal, libera toda la IPC
(cola, semáforos, memoria compartida, sockets, pipes) y espera a los hijos antes de salir.

---

## Limpiar

```bash
make clean
```

Elimina el directorio `bin/`. La IPC se libera automáticamente al terminar el juego.