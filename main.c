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
#include <search.h>
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

struct Message {
  char* topic;
  void* payload;
  size_t payloadlen;
  void* handler;
};

struct Context {
  struct mosquitto* mosq;
  void* messages;
};

static volatile sig_atomic_t g_shutdown;

static void OnSignal(int num) {
  (void)num;
  g_shutdown = 1;
}

static int CompareMessages(const void* a, const void* b) {
  return strcmp(((const struct Message*)a)->topic,
                ((const struct Message*)b)->topic);
}

static bool Flush(int fd, const char* status, const void* body,
                  size_t body_size) {
  char buffer[64];
  int length = snprintf(buffer, sizeof(buffer),
                        "HTTP/1.1 %s\r\n"
                        "Content-Length: %zu\r\n"
                        "\r\n",
                        status, body_size);
  // TODO(mburakov): Verify length is valid at this point.
  struct iovec iov[] = {{.iov_base = buffer, .iov_len = (size_t)length},
                        {.iov_base = UNCONST(body), .iov_len = body_size}};
  ssize_t result = writev(fd, iov, 2);
  if (result != (ssize_t)(iov[0].iov_len + iov[1].iov_len)) {
    Log("Failed to write complete reply (%s)", strerror(errno));
    return false;
  }
  return true;
}

static void CollectMatchingMessages(const void* nodep, VISIT which,
                                    void* closure) {
  struct {
    const char* topic;
    size_t topic_len;
    char* buffer;
    size_t buffer_size;
  }* arg = closure;
  const struct Message* const* it = nodep;
  if (which == preorder || which == endorder ||
      strncmp((*it)->topic, arg->topic, arg->topic_len))
    return;
  size_t topic_len = strlen((*it)->topic);
  size_t buffer_size = arg->buffer_size + topic_len + 1;
  char* buffer = realloc(arg->buffer, buffer_size);
  if (!buffer) {
    Log("Failed to realloc buffer (%s)", strerror(errno));
    return;
  }
  memcpy(buffer + arg->buffer_size, (*it)->topic, topic_len);
  buffer[buffer_size - 1] = '\n';
  arg->buffer = buffer;
  arg->buffer_size = buffer_size;
}

static bool HandleRequest(void* user, int fd, const char* method,
                          const char* target, const void* body,
                          size_t body_size) {
  struct Context* context = user;
  if (!strcmp(method, "GET")) {
    struct {
      const char* topic;
      size_t topic_len;
      char* buffer;
      size_t buffer_size;
    } arg = {.topic = target + 1,
             .topic_len = strlen(target) - 1,
             .buffer = NULL,
             .buffer_size = 0};
    twalk_r(context->messages, CollectMatchingMessages, &arg);
    return Flush(fd, "200 OK", arg.buffer, arg.buffer_size);
  }
  if (strcmp(method, "POST"))
    return Flush(fd, "405 Method Not Allowed", NULL, 0);
  if (!body_size || !target[1]) return Flush(fd, "400 Bad Request", NULL, 0);
  int mosq_errno = mosquitto_publish(context->mosq, NULL, target + 1,
                                     (int)body_size, body, 0, false);
  if (mosq_errno == MOSQ_ERR_SUCCESS) return Flush(fd, "200 OK", NULL, 0);
  const char* error = mosquitto_strerror(mosq_errno);
  return Flush(fd, "500 Internal Server Error", error, strlen(error));
}

static void StoreMessagePayload(void** messages,
                                const struct mosquitto_message* message) {
  struct Message pred = {.topic = message->topic};
  struct Message** it = tsearch(&pred, messages, CompareMessages);
  if (!it) {
    Log("Failed to add message to the map");
    return;
  }
  if (*it != &pred) {
    size_t payloadlen = (size_t)message->payloadlen;
    void* buffer = malloc(payloadlen);
    if (!buffer) {
      Log("Failed to copy payload (%s)", strerror(errno));
      return;
    }
    memcpy(buffer, message->payload, payloadlen);
    free((*it)->payload);
    (*it)->payload = buffer;
    (*it)->payloadlen = payloadlen;
    return;
  }
  struct Message* added = calloc(1, sizeof(struct Message));
  if (!added) {
    Log("Failed to allocate new message (%s)", strerror(errno));
    goto rollback_tsearch;
  }
  added->topic = strdup(message->topic);
  if (!added->topic) {
    Log("Failed to copy topic (%s)", strerror(errno));
    goto rollback_calloc;
  }
  size_t payloadlen = (size_t)message->payloadlen;
  added->payload = malloc(payloadlen);
  if (!added->payload) {
    Log("Failed to copy payload (%s)", strerror(errno));
    goto rollback_strdup;
  }
  memcpy(added->payload, message->payload, payloadlen);
  added->payloadlen = payloadlen;
  *it = added;
  return;
rollback_strdup:
  free(added->topic);
rollback_calloc:
  free(added);
rollback_tsearch:
  tdelete(&pred, messages, CompareMessages);
}

static void HandleMosquitto(struct mosquitto* mosq, void* user,
                            const struct mosquitto_message* message) {
  (void)mosq;
  struct Context* context = user;
  StoreMessagePayload(&context->messages, message);
}

int main(int argc, char* argv[]) {
  if (argc < 3) Terminate("Usage: %s <host> <port>", argv[0]);
  int port = atoi(argv[2]);
  if (0 >= port || port >= 65536)
    Terminate("Invalid mosquitto port \"%s\"", argv[2]);
  int epfd = epoll_create(1);
  if (epfd == -1) Terminate("Failed to create epoll (%s)", strerror(errno));
  int result = EXIT_FAILURE;
  struct Context context = {.mosq = NULL, .messages = NULL};
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
  context.mosq = mosquitto_new(NULL, true, &context);
  if (!context.mosq) {
    Log("Failed to create mosquitto (%s)", strerror(errno));
    goto rollback_mosquitto_lib_init;
  }
  mosquitto_message_callback_set(context.mosq, HandleMosquitto);
  mosq_errno = mosquitto_connect(context.mosq, argv[1], port, 60);
  if (mosq_errno != MOSQ_ERR_SUCCESS) {
    Log("Failed to connect mosquitto (%s)", mosquitto_strerror(mosq_errno));
    goto rollback_mosquitto_new;
  }
  mosq_errno = mosquitto_subscribe(context.mosq, NULL, "+/#", 0);
  if (mosq_errno != MOSQ_ERR_SUCCESS) {
    Log("Failed to subscribe mosquitto (%s)", mosquitto_strerror(mosq_errno));
    goto rollback_mosquitto_connect;
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
