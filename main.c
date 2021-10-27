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

#include <errno.h>
#include <luajit.h>
#include <mosquitto.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/uio.h>
#include <unistd.h>

#include "logging.h"
#include "server.h"

#define UNCONST(op) ((void*)(uintptr_t)(op))
#define STRIOVEC(op) \
  { .iov_base = UNCONST(op), .iov_len = sizeof(op) - 1 }
#define FLUSHARG(op) \
  { STRIOVEC(op), {.iov_base = NULL, .iov_len = 0}, }

struct Context {
  struct mosquitto* mosq;
};

static volatile sig_atomic_t g_shutdown;

static void OnSignal(int num) {
  (void)num;
  g_shutdown = 1;
}

static bool Flush(int fd, const struct iovec iov[2]) {
  // TODO(mburakov): Change to iterative writing.
  ssize_t result = writev(fd, iov, 2);
  if (result != (ssize_t)(iov[0].iov_len + iov[1].iov_len)) {
    Log("Failed to write complete reply (%s)", strerror(errno));
    return false;
  }
  return true;
}

static bool HandleRequest(void* user, int fd, const char* method,
                          const char* target, const void* body,
                          size_t body_size) {
  struct Context* context = user;
  if (strcmp(method, "POST")) {
    static const struct iovec kStatus405[2] = FLUSHARG(
        "HTTP/1.1 405 Method Not Allowed\r\n"
        "Content-Length: 0\r\n"
        "\r\n");
    return Flush(fd, kStatus405);
  }
  if (!body_size || !target[1]) {
    static const struct iovec kStatus400[2] = FLUSHARG(
        "HTTP/1.1 400 Bad Request\r\n"
        "Content-Length: 0\r\n"
        "\r\n");
    return Flush(fd, kStatus400);
  }
  int mosq_errno = mosquitto_publish(context->mosq, NULL, target + 1,
                                     (int)body_size, body, 0, false);
  if (mosq_errno == MOSQ_ERR_SUCCESS) {
    static const struct iovec kStatus200[2] = FLUSHARG(
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 0\r\n"
        "\r\n");
    return Flush(fd, kStatus200);
  }
  char buffer[64];
  const char* error = mosquitto_strerror(mosq_errno);
  snprintf(buffer, sizeof(buffer),
           "HTTP/1.1 500 Internal Server Error\r\n"
           "Content-Length: %zu\r\n\r\n",
           strlen(error));
  const struct iovec iov[2] = {
      {.iov_base = buffer, .iov_len = strlen(buffer)},
      {.iov_base = UNCONST(error), .iov_len = strlen(error)}};
  return Flush(fd, iov);
}

int main(int argc, char* argv[]) {
  if (argc < 3) Terminate("Usage: %s <host> <port>", argv[0]);
  int port = atoi(argv[2]);
  if (0 >= port || port >= 65536)
    Terminate("Invalid mosquitto port \"%s\"", argv[2]);
  int epfd = epoll_create(1);
  if (epfd == -1) Terminate("Failed to create epoll (%s)", strerror(errno));
  int result = EXIT_FAILURE;
  struct Context context;
  struct Server* server = ServerCreate(epfd, HandleRequest, &context);
  if (!server) {
    Log("Failed to create server");
    goto rollback_epoll_create;
  }
  int mosq_errno = mosquitto_lib_init();
  if (mosq_errno != MOSQ_ERR_SUCCESS) {
    Log("Failed to initialize mosquitto lib (%s)",
        mosquitto_strerror(mosq_errno));
    goto rollback_server_create;
  }
  context.mosq = mosquitto_new(NULL, true, NULL);
  if (!context.mosq) {
    Log("Failed to create mosquitto (%s)", strerror(errno));
    goto rollback_mosquitto_lib_init;
  }
  mosq_errno = mosquitto_connect(context.mosq, argv[1], port, 60);
  if (mosq_errno != MOSQ_ERR_SUCCESS) {
    Log("Failed to connect mosquitto (%s)", mosquitto_strerror(mosq_errno));
    goto rollback_mosquitto_new;
  }
  int mosq_sock = mosquitto_socket(context.mosq);
  if (mosq_sock == -1) {
    Log("Failed to get mosquitto socket");
    goto rollback_mosquitto_connect;
  }
  struct epoll_event ev = {.events = EPOLLIN, .data.fd = mosq_sock};
  if (epoll_ctl(epfd, EPOLL_CTL_ADD, mosq_sock, &ev)) {
    Log("Failed to add mosquitto to epoll (%s)", strerror(errno));
    goto rollback_mosquitto_connect;
  }
  static const struct sigaction sa = {.sa_handler = OnSignal};
  if (sigaction(SIGINT, &sa, NULL) || sigaction(SIGTERM, &sa, NULL)) {
    Log("Failed to set up signal handlers (%s)", strerror(errno));
    goto rollback_mosquitto_connect;
  }
  while (!g_shutdown) {
    switch (epoll_wait(epfd, &ev, 1, 1000)) {
      case -1:
        Log("Failed to wait epoll (%s)", strerror(errno));
        if (errno != EINTR) goto rollback_mosquitto_connect;
        continue;
      case 0:
        mosq_errno = mosquitto_loop_misc(context.mosq);
        if (mosq_errno != MOSQ_ERR_SUCCESS) {
          Log("Failed to loop mosquitto (%s)", mosquitto_strerror(mosq_errno));
          goto rollback_mosquitto_connect;
        }
        continue;
      default:
        break;
    }
    if (ev.data.fd == mosq_sock) {
      mosq_errno = mosquitto_loop_read(context.mosq, 1);
      if (mosq_errno != MOSQ_ERR_SUCCESS) {
        Log("Failed to read mosquitto (%s)", mosquitto_strerror(mosq_errno));
        goto rollback_mosquitto_connect;
      }
      continue;
    }
    if (!ServerMaybeHandle(server, ev.data.fd))
      Terminate("Stray socket in epoll");
  }
  result = EXIT_SUCCESS;
rollback_mosquitto_connect:
  mosquitto_disconnect(context.mosq);
rollback_mosquitto_new:
  mosquitto_destroy(context.mosq);
rollback_mosquitto_lib_init:
  mosquitto_lib_cleanup();
rollback_server_create:
  ServerDestroy(server);
rollback_epoll_create:
  close(epfd);
  return result;
}
