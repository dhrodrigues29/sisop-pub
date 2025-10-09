# Makefile para gerar os executáveis do projeto

CC = gcc
CFLAGS = -O2 -std=c11 -Wall -Wextra
LDFLAGS = -lpthread

# Lista de fontes e executáveis
SOURCES = servidor.c cliente.c clienteStart.c serve2.c
TARGETS = servidor cliente clienteStart serve2

.PHONY: all clean

all: $(TARGETS)

servidor: servidor.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

cliente: cliente.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

clienteStart: clienteStart.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

serve2: serve2.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -f $(TARGETS) *.o
