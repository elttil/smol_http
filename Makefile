CFLAGS=-Wall -pedantic -Werror -Wimplicit-fallthrough

smol_http: smol_http.c
	$(CC) $(CFLAGS) $^ -o $@
	chown root:root $@
	chmod u+s $@

install: all
	mkdir -p /usr/bin
	cp -f smol_http /usr/bin
	chmod 755 /usr/bin/smol_http

clean:
	rm smol_http
