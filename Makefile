CC = gcc
CFLAGS = -Wall -Wextra -O3 -std=gnu89 -pedantic

all: motsognir

motsognir: motsognir.o
	$(CC) motsognir.o -o motsognir $(CFLAGS)

motsognir.o: motsognir.c
	$(CC) -c motsognir.c -o motsognir.o $(CFLAGS)

clean:
	rm -f motsognir *.o
