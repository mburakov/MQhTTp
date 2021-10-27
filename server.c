/*
 * Copyright (C) 2021 Mikhail Burakov. This file is part of MQhTTp.
 *
 * MQhTTp is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * MQhTTp is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with MQhTTp.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <search.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "logging.h"
#include "uhttp.h"

struct Client {
  int fd;
  struct Uhttp* uhttp;
};

struct Server {
  int epfd;
  ServerHandler handler;
  void* user;
  int fd;
  void* clients;
};

static int CompareClients(const void* a, const void* b) {
  int fda = ((const struct Client*)a)->fd;
  int fdb = ((const struct Client*)b)->fd;
  return (fda > fdb) - (fda < fdb);
}

static void FreeClient(void* nodep) {
  struct Client* client = nodep;
  if (client->uhttp) UhttpDestroy(client->uhttp);
  close(client->fd);
  free(client);
}

static in_port_t GetPort() {
  static const in_port_t kDefaultPort = 8080;
  const char* http_port = getenv("HTTP_PORT");
  if (!http_port) return kDefaultPort;
  int port = atoi(http_port);
  if (0 >= port || port >= 65536) {
    Log("Invalid http port value \"%s\", using %u", http_port, kDefaultPort);
    return kDefaultPort;
  }
  return (in_port_t)port;
}

static in_addr_t GetAddr() {
  static const in_addr_t kDefaultAddr = INADDR_LOOPBACK;
  const char* http_addr = getenv("HTTP_ADDR");
  if (!http_addr) return kDefaultAddr;
  in_addr_t addr = inet_addr(http_addr);
  if (addr == INADDR_NONE) {
    Log("Invalid http addr value \"%s\", using loopback", http_addr);
    return kDefaultAddr;
  }
  return ntohl(addr);
}

static int MakeServerSocket() {
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock == -1) {
    Log("Failed to create socket (%s)", strerror(errno));
    return -1;
  }
  int one = 1;
  if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one))) {
    Log("Failed to reuse address (%s)", strerror(errno));
    goto rollback_socket;
  }
  struct sockaddr_in addr = {.sin_family = AF_INET,
                             .sin_port = htons(GetPort()),
                             .sin_addr.s_addr = htonl(GetAddr())};
  if (bind(sock, (struct sockaddr*)&addr, sizeof(addr))) {
    Log("Failed to bind socket (%s)", strerror(errno));
    goto rollback_socket;
  }
  if (listen(sock, SOMAXCONN)) {
    Log("Failed to listen socket (%s)", strerror(errno));
    goto rollback_socket;
  }
  return sock;
rollback_socket:
  close(sock);
  return -1;
}

static void AcceptClient(struct Server* server) {
  struct Client* client = calloc(1, sizeof(struct Client));
  if (!client) {
    Log("Failed to allocate client (%s)", strerror(errno));
    return;
  }
  client->fd = accept(server->fd, NULL, NULL);
  if (client->fd == -1) {
    Log("Failed to accept client (%s)", strerror(errno));
    goto rollback_calloc;
  }
  struct epoll_event ev = {.events = EPOLLIN, .data.fd = client->fd};
  if (epoll_ctl(server->epfd, EPOLL_CTL_ADD, client->fd, &ev)) {
    Log("Failed to add client to epoll (%s)", strerror(errno));
    goto rollback_accept;
  }
  struct Client** it = tsearch(client, &server->clients, CompareClients);
  if (!it || *it != client) {
    Log("Failed to add client to the map");
    goto rollback_accept;
  }
  return;
rollback_accept:
  close(client->fd);
rollback_calloc:
  free(client);
}

static void HandleClient(struct Server* server, struct Client* client) {
  int nbytes;
  if (ioctl(client->fd, FIONREAD, &nbytes) == -1) {
    Log("Failed to get pending byte count (%s)", strerror(errno));
    goto drop_client;
  }
  if (nbytes <= 0) goto drop_client;
  if (!client->uhttp) {
    client->uhttp = UhttpCreate();
    if (!client->uhttp) {
      Log("Failed to create uhttp");
      goto drop_client;
    }
  }
  void* buffer = UhttpAllocate(client->uhttp, (size_t)nbytes);
  if (!buffer) {
    Log("Failed to allocate uhttp buffer");
    goto drop_client;
  }
  ssize_t result = read(client->fd, buffer, (size_t)nbytes);
  switch (result) {
    case -1:
      Log("Failed to read client (%s)", strerror(errno));
      __attribute__((fallthrough));
    case 0:
      goto drop_client;
    default:
      break;
  }
  switch (UhttpConsume(client->uhttp, (size_t)result)) {
    case kUhttpResultTerminate:
      Terminate("Heap corruption possible");
    case kUhttpResultFailure:
      Log("Failed to parse request");
      goto drop_client;
    case kUhttpResultWantMore:
      return;
    case kUhttpResultFinished:
      break;
  }
  if (!server->handler(server->user, client->fd, UhttpGetMethod(client->uhttp),
                       UhttpGetTarget(client->uhttp),
                       UhttpGetBody(client->uhttp),
                       UhttpGetBodySize(client->uhttp))) {
    Log("Failed to handle client request");
    goto drop_client;
  }
  UhttpReset(client->uhttp);
  return;
drop_client:
  tdelete(client, &server->clients, CompareClients);
  FreeClient(client);
}

struct Server* ServerCreate(int epfd, ServerHandler handler, void* user) {
  struct Server* result = calloc(1, sizeof(struct Server));
  if (!result) {
    Log("Failed to allocate server (%s)", strerror(errno));
    return NULL;
  }
  result->epfd = epfd;
  result->handler = handler;
  result->user = user;
  result->fd = MakeServerSocket();
  if (result->fd == -1) {
    Log("Failed to create server socket");
    goto rollback_calloc;
  }
  struct epoll_event ev = {.events = EPOLLIN, .data.fd = result->fd};
  if (epoll_ctl(epfd, EPOLL_CTL_ADD, result->fd, &ev)) {
    Log("Failed to add server to epoll (%s)", strerror(errno));
    goto rollback_make_server_socket;
  }
  return result;
rollback_make_server_socket:
  close(result->fd);
rollback_calloc:
  free(result);
  return NULL;
}

void ServerDestroy(struct Server* server) {
  tdestroy(server->clients, FreeClient);
  close(server->fd);
  free(server);
}

bool ServerMaybeHandle(struct Server* server, int fd) {
  if (fd == server->fd) {
    AcceptClient(server);
    return true;
  }
  struct Client pred = {.fd = fd};
  struct Client** it = tfind(&pred, &server->clients, CompareClients);
  if (!it) return false;
  HandleClient(server, *it);
  return true;
}
