CC      = gcc
CFLAGS  = -Wall -Wextra -g -Iinclude
LDFLAGS = -lpthread

GTK_CFLAGS  = $(shell pkg-config --cflags gtk+-3.0)
GTK_LDFLAGS = $(shell pkg-config --libs   gtk+-3.0)

SRC_COMMON = src/framing.c src/serializer.c

.PHONY: all server client clean

all: server client

server: $(SRC_COMMON) src/server.c
	$(CC) $(CFLAGS) -o server $(SRC_COMMON) src/server.c $(LDFLAGS)

client: $(SRC_COMMON) src/client.c
	$(CC) $(CFLAGS) $(GTK_CFLAGS) -o client $(SRC_COMMON) src/client.c $(LDFLAGS) $(GTK_LDFLAGS)

clean:
	rm -f server client