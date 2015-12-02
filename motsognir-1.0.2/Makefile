CC = gcc
CFLAGS = -Wall -Wextra -O3 -std=gnu89 -pedantic

all: motsognir

motsognir: motsognir.o
	$(CC) motsognir.o -o motsognir $(CFLAGS)

motsognir.o: motsognir.c
	$(CC) -c motsognir.c -o motsognir.o $(CFLAGS)

clean:
	rm -f motsognir *.o

install:
	mkdir -p $(DESTDIR)/usr/sbin/
	mkdir -p $(DESTDIR)/etc/init.d/
	mkdir -p $(DESTDIR)/usr/share/doc/packages/motsognir/
	cp motsognir $(DESTDIR)/usr/sbin/
	cp motsognir.conf $(DESTDIR)/etc/
	cp initd_motsognir $(DESTDIR)/etc/init.d/motsognir
	cp license.txt changes.txt manual.pdf $(DESTDIR)/usr/share/doc/packages/motsognir/
