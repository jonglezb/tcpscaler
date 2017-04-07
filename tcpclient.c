#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
#include <event2/event.h>
#include <event2/bufferevent.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>


int main(int argc, char** argv)
{
  struct event_base *base;
  struct bufferevent **bufevents;
  struct addrinfo hints;
  struct addrinfo *res_list, *res;
  struct sockaddr_storage *server;
  int server_len;
  int sock;
  int ret;
  unsigned long int nb_conn, conn;
  char host_s[24];
  char port_s[6];

  if (argc < 4) {
    fprintf(stderr, "usage: %s <host> <port> <nb_conn>\n", argv[0]);
    fprintf(stderr, "Connects to the specified host and port, with the chosen number of TCP connections.\n");
    return 1;
  }
  nb_conn = strtoul(argv[3], NULL, 10);

  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = 0;
  hints.ai_protocol = 0;

  ret = getaddrinfo(argv[1], argv[2], &hints, &res_list);
  if (ret != 0) {
    fprintf(stderr, "Error in getaddrinfo: %s\n", gai_strerror(ret));
    return 1;
  }

  for (res = res_list; res != NULL; res = res->ai_next) {
    sock = socket(res->ai_family, res->ai_socktype,
		  res->ai_protocol);
    if (sock == -1)
      continue;

    getnameinfo(res->ai_addr, res->ai_addrlen, host_s, 24,
		port_s, 6, NI_NUMERICHOST | NI_NUMERICSERV);
    printf("Trying to connect to %s port %s...\n", host_s, port_s);
    if (connect(sock, res->ai_addr, res->ai_addrlen) != -1) {
      printf("Success!\n");
      close(sock);
      break;
    } else {
      perror("Failed to connect");
      close(sock);
    }
  }

  /* No address succeeded */
  if (res == NULL) {
    fprintf(stderr, "Could not connect to host\n");
    return 1;
  }

  /* Copy working server */
  server = malloc(sizeof(struct sockaddr_storage));
  memcpy(server, res->ai_addr, sizeof(struct sockaddr_storage));
  server_len = res->ai_addrlen;
  freeaddrinfo(res_list);

  base = event_base_new();
  if (!base) {
    fprintf(stderr, "Couldn't open event base\n");
    return 1;
  }

  /* Connect again, but using libevent, and multiple times. */
  bufevents = malloc(nb_conn * sizeof(struct bufferevent*));
  for (conn = 0; conn < nb_conn; conn++) {
    bufevents[conn] = bufferevent_socket_new(base, -1, 0);
    if (bufevents[conn] == NULL) {
      fprintf(stderr, "Failed to create socket-based bufferevent.\n");
      break;
    }
    ret = bufferevent_socket_connect(bufevents[conn], (struct sockaddr*)server, server_len);
    if (ret != 0) {
      fprintf(stderr, "Failed to connect to host with bufferevent.\n");
      bufferevent_free(bufevents[conn]);
      bufevents[conn] = NULL;
      break;
    }
  }
  printf("Opened %d connections to host %s port %s\n", conn, host_s, port_s);

  printf("Starting event loop");
  event_base_dispatch(base);

done:
  for (conn = 0; conn < nb_conn; conn++) {
    if (bufevents[conn] == NULL)
      break;
    bufferevent_free(bufevents[conn]);
  }
  free(bufevents);
  event_base_free(base);
  return 0;
}
