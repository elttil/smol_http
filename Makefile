CFLAGS=-Wall -pedantic -Werror -Wimplicit-fallthrough

all: config.h smol_http

config.h:
	cp config.def.h $@

smol_http: smol_http.c
	$(CC) $(CFLAGS) $^ -o $@
	chown root:root $@
	chmod u+s $@

install: smol_http config.h
	mkdir -p /usr/bin
	cp -f smol_http /usr/bin
	chmod 755 /usr/bin/smol_http

clean:
	rm smol_http
