CC      = gcc
CFLAGS  = -Wall -Wextra -g -I src/
LDFLAGS = -lpthread -lrt

BIN     = bin/parchis
SRCS    = src/main.c src/tablero.c src/jugador.c src/ficha.c
OBJS    = $(SRCS:src/%.c=bin/%.o)

.PHONY: all clean run

all: $(BIN)


$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)


bin/%.o: src/%.c | bin/
	$(CC) $(CFLAGS) -c -o $@ $<


bin/:
	mkdir -p bin/

run: all
	./$(BIN)

clean:
	rm -rf bin/
