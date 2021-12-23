CFLAGS=-Wall -pedantic -Werror -Wimplicit-fallthrough

smol_http: smol_http.c
	$(CC) $(CFLAGS) $^ -o $@
	sudo chown root:root $@
	sudo chmod u+s $@

clean:
	rm smol_http
