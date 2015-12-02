CC ?= gcc
CFLAGS += -Wall -Wextra -O3 -std=gnu89 -pedantic

all: motsognir motsognir.8.gz

motsognir: motsognir.o
	$(CC) motsognir.o -o motsognir $(CFLAGS)

motsognir.8.gz: motsognir.8
	cat motsognir.8 | gzip > motsognir.8.gz

motsognir.o: motsognir.c
	$(CC) -c motsognir.c -o motsognir.o $(CFLAGS)

clean:
	rm -f motsognir *.o *.gz

install:
	mkdir -p $(PREFIX)/$(DESTDIR)/usr/sbin/
	mkdir -p $(PREFIX)/$(DESTDIR)/etc/init.d/
	mkdir -p $(PREFIX)/$(DESTDIR)/usr/share/doc/motsognir/
	mkdir -p $(PREFIX)/$(DESTDIR)/usr/share/man/man8/
	cp motsognir $(PREFIX)/$(DESTDIR)/usr/sbin/
	cp motsognir.conf $(PREFIX)/$(DESTDIR)/etc/
	cp motsognir.8.gz $(PREFIX)/$(DESTDIR)/usr/share/man/man8/
	@if [ -d $(PREFIX)/$(DESTDIR)/etc/init.d ] ; then cp initd_motsognir $(PREFIX)/$(DESTDIR)/etc/init.d/motsognir ; fi
	@if [ -d $(PREFIX)/$(DESTDIR)/etc/rc.d ] ; then cp rc_d_motsognir $(PREFIX)/$(DESTDIR)/etc/rc.d/motsognir ; fi
	cp changes.txt manual.pdf $(PREFIX)/$(DESTDIR)/usr/share/doc/motsognir/
