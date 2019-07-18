
CC=gcc
CFLAGS=-c
LIBS =-levent

all: server

server: main.o
	$(CC) main.o  $(LIBS) -o server

main.o: main.c
	$(CC) $(CFLAGS) main.c

clean:
	rm -rf *.o server

