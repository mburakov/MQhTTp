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

#include <dirent.h>
#include <errno.h>
#include <lauxlib.h>
#include <lualib.h>
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
  size_t payload_size;
  void* handler;
};

struct Context {
  struct mosquitto* mosq;
  void* messages;
  char* buffer;
  size_t size;
  size_t alloc;
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

static void FreeMessage(void* nodep) {
  struct Message* message = nodep;
  free(message->topic);
  free(message->payload);
}

static bool Flush(int fd, const char* status, const char* type,
                  const void* body, size_t body_size) {
  char buffer[256];
  int length;
  if (type) {
    length = snprintf(buffer, sizeof(buffer),
                      "HTTP/1.1 %s\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %zu\r\n"
                      "\r\n",
                      status, type, body_size);
  } else {
    length = snprintf(buffer, sizeof(buffer),
                      "HTTP/1.1 %s\r\n"
                      "Content-Length: %zu\r\n"
                      "\r\n",
                      status, body_size);
  }
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

static bool BufferAppend(struct Context* context, const char* data,
                         size_t size) {
  size_t alloc = context->size + size;
  if (context->alloc < alloc) {
    char* buffer = realloc(context->buffer, alloc);
    if (!buffer) {
      Log("Failed to reallocate buffer (%s)", strerror(errno));
      return false;
    }
    context->buffer = buffer;
    context->alloc = alloc;
  }
  memcpy(context->buffer + context->size, data, size);
  context->size += size;
  return true;
}

static void CollectMatchingMessages(const void* nodep, VISIT which,
                                    void* closure) {
  struct {
    struct Context* context;
    const char* topic;
    size_t topic_len;
  }* arg = closure;
  const struct Message* const* it = nodep;
  if (which == preorder || which == endorder ||
      strncmp((*it)->topic, arg->topic, arg->topic_len))
    return;
  static const char kPre[] = "<a href=\"/";
  BufferAppend(arg->context, kPre, strlen(kPre));
  BufferAppend(arg->context, (*it)->topic, strlen((*it)->topic));
  static const char kInt[] = "\">/";
  BufferAppend(arg->context, kInt, strlen(kInt));
  BufferAppend(arg->context, (*it)->topic, strlen((*it)->topic));
  static const char kPost[] = "<br>";
  BufferAppend(arg->context, kPost, strlen(kPost));
}

static bool HandleGetRequest(struct Context* context, int fd,
                             const char* target) {
  if (!strcmp(target, "/favicon.ico"))
    return Flush(fd, "404 Not Found", NULL, NULL, 0);
  struct Message pred = {.topic = UNCONST(target + 1)};
  struct Message** it = tfind(&pred, &context->messages, CompareMessages);
  if (it) {
    return Flush(fd, "200 OK", "application/json", (*it)->payload,
                 (*it)->payload_size);
  }
  static const char kHeader[] =
      "<!DOCTYPE html>"
      "<html>"
      "<head>"
      "<title>MQhTTp</title>"
      "</head>"
      "<body>";
  static const char kFooter[] =
      "</body>"
      "</html>";
  struct {
    struct Context* context;
    const char* topic;
    size_t topic_len;
  } arg = {context, target + 1, strlen(target) - 1};
  context->size = 0;
  if (!BufferAppend(context, kHeader, sizeof(kHeader) - 1)) return false;
  twalk_r(context->messages, CollectMatchingMessages, &arg);
  if (!BufferAppend(context, kFooter, sizeof(kFooter) - 1)) return false;
  return Flush(fd, "200 OK", "text/html", context->buffer, context->size);
}

static bool HandleRequest(void* user, int fd, const char* method,
                          const char* target, const void* body,
                          size_t body_size) {
  struct Context* context = user;
  if (!strcmp(method, "GET")) return HandleGetRequest(context, fd, target);
  if (strcmp(method, "POST"))
    return Flush(fd, "405 Method Not Allowed", NULL, NULL, 0);
  if (!body_size || !target[1])
    return Flush(fd, "400 Bad Request", NULL, NULL, 0);
  int mosq_errno = mosquitto_publish(context->mosq, NULL, target + 1,
                                     (int)body_size, body, 0, false);
  if (mosq_errno == MOSQ_ERR_SUCCESS) return Flush(fd, "200 OK", NULL, NULL, 0);
  const char* error = mosquitto_strerror(mosq_errno);
  return Flush(fd, "500 Internal Server Error", "text/plain", error,
               strlen(error));
}

static struct Message* GetMessage(void** messages, const char* topic) {
  struct Message pred = {.topic = UNCONST(topic)};
  struct Message** it = tsearch(&pred, messages, CompareMessages);
  if (!it) {
    Log("Failed to add message to the map");
    return NULL;
  }
  if (*it != &pred) return *it;
  struct Message* message = calloc(1, sizeof(struct Message));
  if (!message) {
    Log("Failed to allocate new message (%s)", strerror(errno));
    goto rollback_tsearch;
  }
  message->topic = strdup(topic);
  if (!message->topic) {
    Log("Failed to copy topic (%s)", strerror(errno));
    goto rollback_calloc;
  }
  *it = message;
  return message;
rollback_calloc:
  free(message);
rollback_tsearch:
  tdelete(&pred, messages, CompareMessages);
  return NULL;
}

static void StoreMessagePayload(void** messages, const char* topic,
                                const void* payload, size_t payload_size) {
  struct Message* message = GetMessage(messages, topic);
  if (!message) {
    Log("Failed to get message");
    return;
  }
  void* buffer = malloc(payload_size);
  if (!buffer) {
    Log("Failed to copy payload (%s)", strerror(errno));
    return;
  }
  memcpy(buffer, payload, payload_size);
  free(message->payload);
  message->payload = buffer;
  message->payload_size = payload_size;
  return;
}

static void HandleMosquitto(struct mosquitto* mosq, void* user,
                            const struct mosquitto_message* message) {
  (void)mosq;
  struct Context* context = user;
  // TODO(mburakov): Call registered lua callback if any.
  StoreMessagePayload(&context->messages, message->topic, message->payload,
                      (size_t)message->payloadlen);
}

static void SourceCurrentDir(lua_State* lua_state) {
  DIR* current_dir = opendir(".");
  if (!current_dir) Terminate("Failed to open current dir");
  for (struct dirent* item; (item = readdir(current_dir));) {
    if (item->d_type != DT_REG) continue;
    size_t length = strlen(item->d_name);
    static const char kLuaExt[] = {'.', 'l', 'u', 'a'};
    if (length < sizeof(kLuaExt)) continue;
    const char* ext = item->d_name + length - sizeof(kLuaExt);
    if (memcmp(ext, kLuaExt, sizeof(kLuaExt))) continue;
    Log("Sourcing %s...", item->d_name);
    if (luaL_dofile(lua_state, item->d_name))
      Log("%s", lua_tostring(lua_state, -1));
  }
  closedir(current_dir);
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
  memset(&context, 0, sizeof(struct Context));
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
  lua_State* lua_state = luaL_newstate();
  if (!lua_state) {
    Log("Failed to create lua state");
    goto rollback_mosquitto_connect;
  }
  luaL_openlibs(lua_state);
  SourceCurrentDir(lua_state);
  static const struct sigaction sa = {.sa_handler = OnSignal};
  if (sigaction(SIGINT, &sa, NULL) || sigaction(SIGTERM, &sa, NULL)) {
    Log("Failed to set up signal handlers (%s)", strerror(errno));
    goto rollback_lual_newstate;
  }
  while (!g_shutdown) {
    switch (epoll_wait(epfd, &ev, 1, 1000)) {
      case -1:
        Log("Failed to wait epoll (%s)", strerror(errno));
        if (errno != EINTR) goto rollback_lual_newstate;
        continue;
      case 0:
        mosq_errno = mosquitto_loop_misc(context.mosq);
        if (mosq_errno != MOSQ_ERR_SUCCESS) {
          Log("Failed to loop mosquitto (%s)", mosquitto_strerror(mosq_errno));
          goto rollback_lual_newstate;
        }
        continue;
      default:
        break;
    }
    if (ev.data.fd == mosq_sock) {
      mosq_errno = mosquitto_loop_read(context.mosq, 1);
      if (mosq_errno != MOSQ_ERR_SUCCESS) {
        Log("Failed to read mosquitto (%s)", mosquitto_strerror(mosq_errno));
        goto rollback_lual_newstate;
      }
      continue;
    }
    if (!ServerMaybeHandle(server, ev.data.fd))
      Terminate("Stray socket in epoll");
  }
  result = EXIT_SUCCESS;
rollback_lual_newstate:
  lua_close(lua_state);
rollback_mosquitto_connect:
  mosquitto_disconnect(context.mosq);
rollback_mosquitto_new:
  mosquitto_destroy(context.mosq);
rollback_mosquitto_lib_init:
  mosquitto_lib_cleanup();
rollback_server_create:
  ServerDestroy(server);
rollback_epoll_create:
  tdestroy(context.messages, FreeMessage);
  free(context.buffer);
  close(epfd);
  return result;
}
