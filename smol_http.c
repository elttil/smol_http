/*
Copyright (C) 2022 by Anton Kling <anton@kling.gg>

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
#include <arpa/inet.h>
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netdb.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

// Should xxx.html not exist these will be supplied instead.
#define DEFAULT_404_SITE "404 - Not Found\0"
#define DEFAULT_400_SITE "400 - Bad Request\0"

// Default port should -p not be supplied.
#define DEFAULT_PORT 1337

// Default directory should -d not be supplied.
#define WEBSITE_ROOT "./site/"

#define TIMEOUT_SECOND 3
#define TIMEOUT_USECOND 0

#define MAX_BUFFER 4096 // Size of the read buffer

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define COND_PERROR_EXP(condition, function_name, expression)                  \
	if (condition) {                                                       \
		perror(function_name);                                         \
		expression;                                                    \
	}

#define HAS_PLEDGE (__OpenBSD__ | __SerenityOS__)
#define HAS_UNVEIL (__OpenBSD__ | __SerenityOS__)

#if HAS_PLEDGE
#define PLEDGE(promise, exec)                                                  \
	COND_PERROR_EXP(-1 == pledge(promise, exec), "pledge", exit(1))
#else
#define PLEDGE(promise, exec) ;
#endif

#if HAS_UNVEIL
#define UNVEIL(path, permissions)                                              \
	COND_PERROR_EXP(-1 == unveil(path, permissions), "unveil", exit(1))
#else
#define UNVEIL(path, permissions) ;
#endif

#define ASSERT_NOT_REACHED assert(0)

#define COPYRIGHT_STATEMENT                                                    \
	"\
This program is licensed under Affero GNU Public License Version 3.\n\
See https://www.gnu.org/licenses/ for more information.\n\
THIS PROGRAM COMES WITHOUT ANY WARRANTY; without even the implied\n\
warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE."

static const struct {
	char *ext;
	char *type;
} mimes[] = {
	{ "xml", "application/xml; charset=utf-8" },
	{ "xhtml", "application/xhtml+xml; charset=utf-8" },
	{ "html", "text/html; charset=utf-8" },
	{ "htm", "text/html; charset=utf-8" },
	{ "css", "text/css; charset=utf-8" },
	{ "txt", "text/plain; charset=utf-8" },
	{ "md", "text/plain; charset=utf-8" },
	{ "c", "text/plain; charset=utf-8" },
	{ "h", "text/plain; charset=utf-8" },
	{ "gz", "application/x-gtar" },
	{ "tar", "application/tar" },
	{ "pdf", "application/x-pdf" },
	{ "png", "image/png" },
	{ "gif", "image/gif" },
	{ "jpeg", "image/jpg" },
	{ "jpg", "image/jpg" },
	{ "iso", "application/x-iso9660-image" },
	{ "webp", "image/webp" },
	{ "svg", "image/svg+xml; charset=utf-8" },
	{ "flac", "audio/flac" },
	{ "mp3", "audio/mpeg" },
	{ "ogg", "audio/ogg" },
	{ "mp4", "video/mp4" },
	{ "ogv", "video/ogg" },
	{ "webm", "video/webm" },
};

const char *const get_mime(const char *file)
{
	for (; *file && *file++ != '.';)
		;
	for (size_t i = 0; i < sizeof(mimes) / sizeof(mimes[0]) - 1; i++)
		if (0 == strcmp(mimes[i].ext, file))
			return mimes[i].type;

	return "application/octet-stream";
}

const char *const status_code_to_error_message(uint16_t status_code)
{
	switch (status_code) {
	case 400:
		return "400 Bad Request";
	case 404:
		return "404 File Not Found";
	case 200:
	default:
		return "200 OK";
	}
}

void connection_handler(int socket_desc)
{
	PLEDGE("stdio rpath", "");
	char recv_buffer[MAX_BUFFER];
	int status_code = 200;

	// We can ignore SIGPIPE as we already have checks
	// that would deal with this.
	COND_PERROR_EXP(SIG_ERR == signal(SIGPIPE, SIG_IGN), "signal",
			goto cleanup);

	// Ensure that we timeout should the send/recv take too long.
	struct timeval timeout;
	timeout.tv_sec = TIMEOUT_SECOND;
	timeout.tv_usec = TIMEOUT_USECOND;
	COND_PERROR_EXP(-1 == setsockopt(socket_desc, SOL_SOCKET, SO_RCVTIMEO,
					 &timeout, sizeof(timeout)),
			"setsockopt", goto cleanup);
	COND_PERROR_EXP(-1 == setsockopt(socket_desc, SOL_SOCKET, SO_SNDTIMEO,
					 &timeout, sizeof(timeout)),
			"setsockopt", goto cleanup);

	ssize_t recv_size;
	COND_PERROR_EXP(-1 == (recv_size = recv(socket_desc, recv_buffer,
						MAX_BUFFER - 1, 0)),
			"recv", goto cleanup);
	// Null terminate the request.
	recv_buffer[recv_size] = 0;

	char *filename = recv_buffer;
	// Get to the second argument in the buffer.
	for (; *filename && *filename++ != ' ';)
		;

	// If we had only one argument then just provide 400.html.
	if (!(*filename)) {
		filename = "/400.html";
		status_code = 400;
		goto skip_filename_parse;
	}

	int enter_directory = 0;
	uint16_t i;
	for (i = 0; filename[i] && ' ' != filename[i] && '\n' != filename[i] &&
		    '\r' != filename[i];
	     i++)
		;

	filename[i] = 0;

	struct stat statbuf;
	if (-1 == stat(filename, &statbuf)) {
		if (ENOENT == errno)
			goto not_found;
		goto cleanup;
	}
	if (S_ISDIR(statbuf.st_mode)) {
		enter_directory = 1;
		chdir(filename);
		filename = "index.html";
	}

	int fd;
	char *const_site_content;
skip_filename_parse:
redo:
	const_site_content = NULL;
	if (-1 == (fd = open(filename, O_RDONLY))) {
		if (1 == enter_directory) {
			enter_directory = 2;
			goto write;
		}

		if (400 == status_code) {
			const_site_content = DEFAULT_400_SITE;
			goto write;
		}

		if (0 == strcmp(filename, "/404.html") && 404 == status_code) {
			const_site_content = DEFAULT_404_SITE;
			goto write;
		}

	not_found:
		filename = "/404.html";
		status_code = 404;
		goto redo;
	}

write:
	if (0 >
	    dprintf(socket_desc,
		    "HTTP/1.0 %s\r\nContent-Type: %s\r\nServer: smol_http\r\n\r\n",
		    status_code_to_error_message(status_code),
		    get_mime(filename))) {
		puts("dprintf error");
		goto cleanup;
	}

	if (const_site_content) {
		PLEDGE("stdio", NULL);
		COND_PERROR_EXP(-1 == write(socket_desc, const_site_content,
					    strlen(const_site_content)),
				"write",
				/*NOP*/);
		goto cleanup;
	}

	// Should ./index.html be unable to be read we create a
	// directory listing.
	if (2 == enter_directory) {
		// Get the directory contents and provide that to the client.
		DIR *d;
		COND_PERROR_EXP(NULL == (d = opendir(".")), "opendir",
				goto cleanup);

		char current_path[PATH_MAX];
		char back_path[PATH_MAX];
		COND_PERROR_EXP(!realpath(".", current_path), "realpath",
				goto directory_cleanup)
		COND_PERROR_EXP(!realpath("..", back_path), "realpath",
				goto directory_cleanup)
		if (0 >
		    dprintf(socket_desc,
			    "Index of %s/<br><a href='%s'>./</a><br><a href='%s'>../</a><br>",
			    current_path, current_path, back_path)) {
			puts("dprintf error");
			goto directory_cleanup;
		}
		for (struct dirent *dir; (dir = readdir(d));) {
			if (0 == strcmp(dir->d_name, ".") ||
			    0 == strcmp(dir->d_name, ".."))
				continue;

			char tmp_path[PATH_MAX];
			COND_PERROR_EXP(!realpath(dir->d_name, tmp_path),
					"realpath", break);
			if (0 > dprintf(socket_desc,
					"<a href='%s'>%s%s</a><br>", tmp_path,
					dir->d_name,
					(DT_DIR == dir->d_type) ? "/" : "")) {
				puts("dprintf error");
				break;
			}
		}
	directory_cleanup:
		closedir(d);
		goto cleanup;
	}
	PLEDGE("stdio", NULL);

	char rwbuf[4096];
	for (int l; 0 != (l = read(fd, rwbuf, sizeof(rwbuf)));) {
		COND_PERROR_EXP(-1 == l, "read", break);
		COND_PERROR_EXP(-1 == write(socket_desc, rwbuf, l), "write",
				break);
	}

	close(fd);
cleanup:
	PLEDGE("", NULL);
	close(socket_desc);
}

int drop_root_privleges(void)
{
	COND_PERROR_EXP(0 != seteuid(getuid()), "seteuid", return 0);
	COND_PERROR_EXP(0 != setegid(getgid()), "setegid", return 0);
	if (0 == geteuid()) {
		fprintf(stderr,
			"Error: Program can not be ran by a root user.\n");
		return 0;
	}
	return 1;
}

int init_server(short port, const char *website_root)
{
	int socket_desc, new_socket;
	struct sockaddr_in server;
	struct sockaddr client;
	socklen_t c;

	UNVEIL(website_root, "r");
	// Disable usage of unveil()(this will also be
	// done by our pledge() call)
	UNVEIL(NULL, NULL);
	COND_PERROR_EXP(0 != chroot(website_root), "chroot", return 1);
	PLEDGE("stdio inet rpath exec id proc", "");

	// I am unsure if chdir("/") even can fail.
	// But I will keep this check here just in case.
	COND_PERROR_EXP(0 != chdir("/"), "chdir", return 1);

	COND_PERROR_EXP(-1 == (socket_desc = socket(AF_INET, SOCK_STREAM, 0)),
			"socket", return 1);

	server.sin_family = AF_INET;
	server.sin_addr.s_addr = INADDR_ANY;
	server.sin_port = htons(port);

	COND_PERROR_EXP(0 > bind(socket_desc, (struct sockaddr *)&server,
				 sizeof(server)),
			"bind", return 1);

	// Everything that requires root privleges is done,
	// we can now drop privleges.
	if (!drop_root_privleges()) {
		fprintf(stderr, "Unable to drop privleges.\n");
		return 1;
	}

	PLEDGE("stdio inet rpath exec proc", NULL);

	COND_PERROR_EXP(0 != listen(socket_desc, 3), "listen", return 1);

	c = sizeof(struct sockaddr_in);
	for (; (new_socket = accept(socket_desc, &client, &c));) {
		COND_PERROR_EXP(-1 == new_socket, "accept", continue);

		// Create a child and handle the connection
		pid_t pid;
		COND_PERROR_EXP(-1 == (pid = fork()), "fork", continue);
		if (0 != pid) // We are the parent.
		{
			close(new_socket);
			continue;
		}
		// We are the child.
		connection_handler(new_socket);
		close(socket_desc);
		_exit(0);
	}
	close(socket_desc);
	return 0;
}

void usage(const char *const str)
{
	fprintf(stderr,
		"Usage: %s [-p PORT] [-d Website root directory] -h(Print this message)\n",
		str);
	puts("---");
	puts(COPYRIGHT_STATEMENT);
}

int main(int argc, char **argv)
{
	if (0 != geteuid()) {
		fprintf(stderr, "Error: Program does not have root privleges.");
		return 1;
	}

	short port = DEFAULT_PORT;
	char *website_root = WEBSITE_ROOT;
	for (int ch; - 1 != (ch = getopt(argc, argv, "p:d:h"));)
		switch ((char)ch) {
		case 'p':
			if (0 == (port = atoi(optarg))) {
				usage(argv[0]);
				return 0;
			}
			break;
		case 'd':
			website_root = optarg;
			break;
		case '?':
		case ':':
		case 'h':
			usage(argv[0]);
			return 0;
		}

	return init_server(port, website_root);
}
