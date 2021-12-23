CFLAGS=-Wall -pedantic -Werror -Wimplicit-fallthrough
CC=musl-clang

http: http.c
	$(CC) $(CFLAGS) $^ -o $@
	sudo chown root:root $@
	sudo chmod u+s $@

clean:
	rm http
