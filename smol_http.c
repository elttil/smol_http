#include "config.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define COND_PERROR_EXP(condition, function_name, expression)                  \
  if (condition) {                                                             \
    perror(function_name);                                                     \
    expression;                                                                \
  }

#define MIME_HTML (mimes[2].type)

struct timeval timeout = {
    .tv_sec = TIMEOUT_SECOND,
    .tv_usec = TIMEOUT_USECOND,
};

static const struct {
  char *ext;
  char *type;
} mimes[] = {
    {"xml", "application/xml; charset=utf-8"},
    {"xhtml", "application/xhtml+xml; charset=utf-8"},
    {"html", "text/html; charset=utf-8"},
    {"htm", "text/html; charset=utf-8"},
    {"css", "text/css; charset=utf-8"},
    {"txt", "text/plain; charset=utf-8"},
    {"md", "text/plain; charset=utf-8"},
    {"c", "text/plain; charset=utf-8"},
    {"h", "text/plain; charset=utf-8"},
    {"gz", "application/x-gtar"},
    {"tar", "application/tar"},
    {"pdf", "application/x-pdf"},
    {"png", "image/png"},
    {"gif", "image/gif"},
    {"jpeg", "image/jpg"},
    {"jpg", "image/jpg"},
    {"iso", "application/x-iso9660-image"},
    {"webp", "image/webp"},
    {"svg", "image/svg+xml; charset=utf-8"},
    {"flac", "audio/flac"},
    {"mp3", "audio/mpeg"},
    {"ogg", "audio/ogg"},
    {"mp4", "video/mp4"},
    {"ogv", "video/ogg"},
    {"webm", "video/webm"},
};

const char *get_mime(const char *file) {
  const char *ext = file;
  for (; *ext++;) // Move ext to end of string
    ;
  for (; ext != file && *(ext - 1) != '.';
       ext--) // Move ext back until we find a dot
    ;
  if (file == ext)
    goto ret_default; // If there is no dot then there is no file
                      // extension.

  for (size_t i = 0; i < sizeof(mimes) / sizeof(mimes[0]) - 1; i++)
    if (0 == strcmp(mimes[i].ext, ext))
      return mimes[i].type;

ret_default:
  return "application/octet-stream";
}

const char *status_code_to_error_message(uint16_t status_code) {
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

typedef struct {
  struct pollfd *fds;
  uint16_t num_fds;
  uint16_t size;
} polling_queue;

int polling_queue_init(polling_queue *q, uint16_t s) {
  q->fds = malloc(sizeof(struct pollfd) * s);
  if (!q->fds)
    return 0;
  q->size = s;
  q->num_fds = 0;
  return 1;
}

int polling_queue_add(polling_queue *q, int fd, short flag) {
  if (q->num_fds > q->size) {
    // FIXME: Possibly allocate more size
    return 0;
  }
  q->fds[q->num_fds].fd = fd;
  q->fds[q->num_fds].events = flag;
  q->num_fds++;
  return 1;
}

int polling_queue_remove(polling_queue *q, int index) {
  if (q->num_fds == 0)
    return 0;

  q->num_fds--;

  // Set the removed index with the values of the last element
  q->fds[index].fd = q->fds[q->num_fds].fd;
  q->fds[index].events = q->fds[q->num_fds].events;

  // Unset the last element
  q->fds[q->num_fds].fd = 0;
  q->fds[q->num_fds].events = 0;
  return 1;
}

int polling_queue_poll(polling_queue *q) {
  return poll(q->fds, q->num_fds, -1);
}

void polling_queue_unset(polling_queue *q) {
  for (int i = 0; i < q->num_fds; i++)
    q->fds[i].revents = 0;
}

int drop_root_privleges(void) {
  COND_PERROR_EXP(0 != seteuid(getuid()), "seteuid", return 0);
  COND_PERROR_EXP(0 != setegid(getgid()), "setegid", return 0);
  if (0 == geteuid()) {
    fprintf(stderr, "Error: Program can not be ran by a root user.\n");
    return 0;
  }
  return 1;
}

const char *parse_path(char *buffer, int *status_code) {
  char *path = buffer;
  // Get to the second argument in the buffer.
  for (; *path && *path++ != ' ';)
    ;

  // If we had only one argument then just provide 400.html.
  if (!(*path)) {
    path = "/400.html";
    *status_code = 400;
    return path;
  }

  uint16_t i;
  for (i = 0; path[i] && ' ' != path[i] && '\n' != path[i] && '\r' != path[i];
       i++)
    ;

  path[i] = 0;
  return path;
}

void write_constant_content(int fd, int status_code) {
  char *content;
  switch (status_code) {
  case 400:
    content = DEFAULT_400_SITE;
    break;
  case 404:
    content = DEFAULT_404_SITE;
    break;
  case 500:
  default:
    content = DEFAULT_500_SITE;
    break;
  }
  write(fd, content, strlen(content));
}

int http_read_dir(int socket) {
  DIR *d;
  COND_PERROR_EXP(NULL == (d = opendir(".")), "opendir", return 0);

  char current_path[PATH_MAX];
  char back_path[PATH_MAX];
  COND_PERROR_EXP(!realpath(".", current_path), "realpath",
                  goto directory_cleanup)
  COND_PERROR_EXP(!realpath("..", back_path), "realpath",
                  goto directory_cleanup)

  if (0 > dprintf(socket,
                  "Index of %s/<br><a href='%s'>./</a><br><a "
                  "href='%s'>../</a><br>",
                  current_path, current_path, back_path)) {
    puts("dprintf error");
    goto directory_cleanup;
  }

  for (struct dirent *dir; (dir = readdir(d));) {
    if (0 == strcmp(dir->d_name, ".") || 0 == strcmp(dir->d_name, ".."))
      continue;

    char tmp_path[PATH_MAX];
    COND_PERROR_EXP(!realpath(dir->d_name, tmp_path), "realpath", break);
    if (0 > dprintf(socket, "<a href='%s'>%s%s</a><br>", tmp_path, dir->d_name,
                    (DT_DIR == dir->d_type) ? "/" : "")) {
      puts("dprintf error");
      goto directory_cleanup;
    }
  }

  closedir(d);
  return 1;
directory_cleanup:
  closedir(d);
  return 0;
}

int http_read_file(int socket, const char *path, int *status_code, int *fd,
                   const char **mime) {
  *mime = MIME_HTML;
  struct stat statbuf;
  if (-1 == stat(path, &statbuf)) {
    if (ENOENT == errno) {
      if (404 == *status_code) {
        return 2;
      }
      *status_code = 404;
      return http_read_file(socket, "/404.html", status_code, fd, mime);
    }
    return 0;
  }

  if (S_ISDIR(statbuf.st_mode)) {
    chdir(path);
    // Check if we can read /index.html in the directory. If not we give
    // a directory listing.
    int rc = http_read_file(socket, "index.html", status_code, fd, mime);
    if (1 == rc)
      return 1;
    return 3;
  }

  *fd = open(path, O_RDONLY);
  if (-1 == *fd) {
    if (200 == *status_code)
      *status_code = 500;
    return 2;
  }
  *mime = get_mime(path);
  return 1;
}

void outfile(int socket, int fd) {
  char buffer[4096];
  for (int rc;;) {
    COND_PERROR_EXP(-1 == (rc = read(fd, buffer, 4096)), "read", break);
    if (rc == 0)
      break;
    COND_PERROR_EXP((-1 == write(socket, buffer, rc)), "write", break);
  }
}

void handle_connection(int socket) {
  char buffer[4096];
  int status_code = 200;
  int rc;

  // We can ignore SIGPIPE as we already have checks
  // that would deal with this.
  COND_PERROR_EXP(SIG_ERR == signal(SIGPIPE, SIG_IGN), "signal", return );

  rc = read(socket, buffer, 4096);
  if (-1 == rc)
    return;

  // Ensure that we timeout should the send take too long.
  COND_PERROR_EXP(-1 == setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                                   sizeof(timeout)),
                  "setsockopt", return );

  buffer[rc] = '\0';
  const char *path = parse_path(buffer, &status_code);
  int fd;
  const char *mime;
  rc = http_read_file(socket, path, &status_code, &fd, &mime);
  if (0 == rc)
    return;

  // Write header
  if (0 > dprintf(socket,
                  "HTTP/1.1 %s\r\nContent-Type: %s\r\nServer: smol_http\r\n\r\n",
                  status_code_to_error_message(status_code), mime)) {
    puts("dprintf error");
    return;
  }

  switch (rc) {
  case 3:
    http_read_dir(socket);
    break;
  case 2:
    // http_read_file could not find a file to handle the
    // error(404.html 400.html etc). Therefore we provide a error
    // that is compiled in the program.
    write_constant_content(socket, status_code);
    break;
  default:
    // Read from the file that http_read_file opened
    outfile(socket, fd);
    close(fd);
    break;
  }
}

int server_loop(const char *website_root, uint16_t port) {
  int socket_desc;
  struct sockaddr_in server;
  struct sockaddr client;
  socklen_t c;

  COND_PERROR_EXP(0 != chroot(website_root), "chroot", return 1);

  // I am unsure if chdir("/") even can fail.
  // But I will keep this check here just in case.
  COND_PERROR_EXP(0 != chdir("/"), "chdir", return 1);

  COND_PERROR_EXP(-1 == (socket_desc = socket(AF_INET, SOCK_STREAM, 0)),
                  "socket", return 1);

  server.sin_family = AF_INET;
  server.sin_addr.s_addr = INADDR_ANY;
  server.sin_port = htons(port);

  COND_PERROR_EXP(
      0 > bind(socket_desc, (struct sockaddr *)&server, sizeof(server)), "bind",
      return 1);

  // Everything that requires root privleges is done,
  // we can now drop privleges.
  if (!drop_root_privleges()) {
    fprintf(stderr, "Unable to drop privleges.\n");
    return 1;
  }

  c = sizeof(struct sockaddr_in);
  polling_queue q;
  polling_queue_init(&q, 100);

  polling_queue_add(&q, socket_desc, POLLIN);

  COND_PERROR_EXP(0 != listen(socket_desc, 3), "listen", return 1);

  for (;; polling_queue_unset(&q)) {
    int rc;
    COND_PERROR_EXP(-1 == (rc = polling_queue_poll(&q)), "ppoll", continue);
    if (0 == rc)
      continue;

    if (q.fds[0].revents == POLLIN) {
      // Add a client
      int new_socket = accept(socket_desc, &client, &c);
      COND_PERROR_EXP(-1 == new_socket, "accept", continue);
      if (!polling_queue_add(&q, new_socket, POLLIN)) {
        printf("[ERROR]: Unable to add fd.");
        continue;
      }
    }
    for (int i = 1; i < q.num_fds; i++) {
      if (!(q.fds[i].revents & POLLIN))
        continue;

      int fd = q.fds[i].fd;
      pid_t pid = fork();
      COND_PERROR_EXP(-1 == pid, "fork", continue);
      if (0 != pid) {
        polling_queue_remove(&q, i);
        close(fd);
        continue;
      }
      close(socket_desc);
      handle_connection(fd);
      close(fd);
      exit(0);
    }
  }
  close(socket_desc);
  return 0;
}

void usage(const char *const str) {
  fprintf(stderr,
          "Usage: %s [-p PORT] [-d Website root directory] -h(Print this "
          "message)\n",
          str);
}

int main(int argc, char **argv) {
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

  return server_loop(website_root, port);
}
