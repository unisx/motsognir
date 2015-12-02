CC ?= gcc
CFLAGS += -Wall -Wextra -O3 -std=gnu89 -pedantic

all: motsognir

motsognir: motsognir.o
	$(CC) motsognir.o -o motsognir $(CFLAGS)

motsognir.o: motsognir.c
	$(CC) -c motsognir.c -o motsognir.o $(CFLAGS)

clean:
	rm -f motsognir *.o

install:
	mkdir -p $(PREFIX)/$(DESTDIR)/usr/sbin/
	mkdir -p $(PREFIX)/$(DESTDIR)/etc/init.d/
	mkdir -p $(PREFIX)/$(DESTDIR)/usr/share/doc/motsognir/
	cp motsognir $(PREFIX)/$(DESTDIR)/usr/sbin/
	cp motsognir.conf $(PREFIX)/$(DESTDIR)/etc/
	@if [ -d $(PREFIX)/$(DESTDIR)/etc/init.d ] ; then cp initd_motsognir $(PREFIX)/$(DESTDIR)/etc/init.d/motsognir ; fi
	@if [ -d $(PREFIX)/$(DESTDIR)/etc/rc.d ] ; then cp rc_d_motsognir $(PREFIX)/$(DESTDIR)/etc/rc.d/motsognir ; fi
	cp changes.txt manual.pdf $(PREFIX)/$(DESTDIR)/usr/share/doc/motsognir/
