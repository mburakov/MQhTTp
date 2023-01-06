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

//#include <dirent.h>
#include <errno.h>
//#include <lauxlib.h>
//#include <lualib.h>
//#include <search.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <search.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include "http_utils.h"
#include "message.h"
#include "toolbox/buffer.h"
#include "toolbox/http_parser.h"
#include "toolbox/io_muxer.h"
#include "toolbox/mqtt.h"
#include "toolbox/mqtt_parser.h"
#include "toolbox/utils.h"

struct ClientContext {
  int sock;
  char* method;
  char* target;
  size_t content_length;
  struct Buffer http_buffer;
  struct HttpParserState http_parser;
  struct ClientContext* next;
};

struct ServiceContext {
  int http;
  int mqtt;
  struct IoMuxer io_muxer;
  struct Buffer mqtt_buffer;
  struct ClientContext* clients;
  void* messages;
};

static volatile sig_atomic_t g_signal;
static struct ServiceContext g_service;

static void OnSignal(int signal) { g_signal = signal; }

static struct sockaddr_in GetHttpAddr() {
  const char* sport = getenv("HTTP_PORT");
  int port = sport ? atoi(sport) : 8080;
  if (0 > port || port > UINT16_MAX) {
    LOGE("Invalid http port");
  }
  const char* saddr = getenv("HTTP_ADDR");
  in_addr_t addr = inet_addr(saddr ? saddr : "0.0.0.0");
  if (addr == INADDR_NONE) {
    LOGE("Invalid http addr");
    exit(EXIT_FAILURE);
  }
  struct sockaddr_in result = {
      .sin_family = AF_INET,
      .sin_port = htons((uint16_t)port),
      .sin_addr.s_addr = addr,
  };
  return result;
}

static struct sockaddr_in GetMqttAddr(int argc, char* argv[]) {
  int port = argc > 2 ? atoi(argv[2]) : 1883;
  if (0 > port || port > UINT16_MAX) {
    LOGE("Invalid mqtt port");
    exit(EXIT_FAILURE);
  }
  in_addr_t addr = inet_addr(argc > 1 ? argv[1] : "127.0.0.1");
  if (addr == INADDR_NONE) {
    LOGE("Invalid mqtt addr");
    exit(EXIT_FAILURE);
  }
  struct sockaddr_in result = {
      .sin_family = AF_INET,
      .sin_port = htons((uint16_t)port),
      .sin_addr.s_addr = addr,
  };
  return result;
}

static void DropClientContext(struct ClientContext* client) {
  struct ClientContext** next = &g_service.clients;
  while (*next != client) next = &(*next)->next;
  *next = client->next;

  BufferDestroy(&client->http_buffer);
  free(client->target);
  free(client->method);
  close(client->sock);
  free(client);
}

static void OnHttpRequest(void* user, const char* method, size_t method_size,
                          const char* target, size_t target_size) {
  struct ClientContext* client = user;
  client->method = malloc(method_size + 1);
  if (!client->method) {
    LOGW("Failed to copy http method (%s)", strerror(errno));
    return;
  }
  memcpy(client->method, method, method_size);
  client->method[method_size] = 0;
  client->target = malloc(target_size + 1);
  if (!client->target) {
    LOGW("Failed to copy http target (%s)", strerror(errno));
    return;
  }
  memcpy(client->target, target, target_size);
  client->target[target_size] = 0;
}

static void OnHttpField(void* user, const char* name, size_t name_size,
                        const char* value, size_t value_size) {
  static const char content_length_name[] = {'C', 'o', 'n', 't', 'e', 'n', 't',
                                             '-', 'L', 'e', 'n', 'g', 't', 'h'};
  struct ClientContext* client = user;
  if (name_size != sizeof(content_length_name) ||
      memcmp(name, content_length_name, sizeof(content_length_name))) {
    return;
  }
  size_t content_length = 0;
  if (!value_size) goto bad_content_length;
  for (size_t offset = 0; offset < value_size; offset++) {
    if ('0' > value[offset] || value[offset] > '9') goto bad_content_length;
    content_length = content_length * 10 + (size_t)(value[offset] - '0');
  }
  client->content_length = content_length;
  return;

bad_content_length:
  LOGW("Invalid Content-Length value");
}

static void OnHttpFinished(void* user, size_t offset) {
  struct ClientContext* client = user;
  BufferDiscard(&client->http_buffer, offset);
}

static bool ServeHttpGet(int fd, const char* target) {
  if (!strcmp(target, "/favicon.ico"))
    return SendHttpReply(fd, "404 Not Found", NULL, NULL, 0);
  struct Message key = {
      .topic = *(char**)(void*)&target + 1,
      .topic_size = strlen(target) - 1,
  };
  struct Message** message = tfind(&key, &g_service.messages, MessageCompare);
  if (!message) return SendHttpReply(fd, "404 NotFound", NULL, NULL, 0);
  struct iovec body = {
      .iov_base = (*message)->payload,
      .iov_len = (*message)->payload_size,
  };
  return SendHttpReply(fd, "200 OK", "application/json", &body, 1);
}

static bool ServeHttpPost(int fd, const char* target, const void* content,
                          size_t content_length) {
  return false;
}

static bool DispatchHttpRequest(int fd, const char* method, const char* target,
                                const void* content, size_t content_length) {
  LOGD("%s %s %.*s", method, target, (int)content_length, (const char*)content);
  if (!strcmp(method, "GET")) return ServeHttpGet(fd, target);
  if (!strcmp(method, "POST"))
    return ServeHttpPost(fd, target, content, content_length);
  return SendHttpReply(fd, "405 Method Not Allowed", NULL, NULL, 0);
}

static void OnClientRead(void* user) {
  struct ClientContext* client = user;
  switch (BufferAppendFrom(&client->http_buffer, client->sock)) {
    case -1:
      LOGW("Failed to read http client (%s)", strerror(errno));
      __attribute__((fallthrough));
    case 0:
      goto drop_client;
    default:
      break;
  }
  static const struct HttpParserCallbacks http_callbacks = {
      .on_request = OnHttpRequest,
      .on_field = OnHttpField,
      .on_finished = OnHttpFinished,
  };
  for (;;) {
    if (!client->http_parser.stage) {
      if (client->content_length > client->http_buffer.size)
        goto want_more_data;
      if (!DispatchHttpRequest(client->sock, client->method, client->target,
                               client->http_buffer.data,
                               client->content_length)) {
        goto drop_client;
      }
      HttpParserReset(&client->http_parser);
      BufferDiscard(&client->http_buffer, client->content_length);
      client->content_length = 0;
      free(client->target);
      client->target = NULL;
      free(client->method);
      client->method = NULL;
    }
    switch (HttpParserParse(&client->http_parser, client->http_buffer.data,
                            client->http_buffer.size, &http_callbacks, user)) {
      case kHttpParserResultFinished:
        continue;
      case kHttpParserResultWantMore:
        goto want_more_data;
      case kHttpParserResultError:
        LOGW("Failed to parse http request");
        goto drop_client;
      default:
        __builtin_unreachable();
    }
  }

want_more_data:
  if (IoMuxerOnRead(&g_service.io_muxer, client->sock, OnClientRead, client))
    return;
  LOGW("Failed to reschedule http client read (%s)", strerror(errno));
drop_client:
  DropClientContext(client);
}

static void OnHttpRead(void* user) {
  (void)user;
  int sock = accept(g_service.http, NULL, NULL);
  if (sock == -1) {
    LOGW("Failed to accept http client (%s)", strerror(errno));
    return;
  }
  struct ClientContext* client = malloc(sizeof(struct ClientContext));
  if (!client) {
    LOGW("Failed to allocate http client context (%s)", strerror(errno));
    goto close_sock;
  }
  if (!IoMuxerOnRead(&g_service.io_muxer, sock, OnClientRead, client)) {
    LOGW("Failed to schedule http client read (%s)", strerror(errno));
    goto free_client;
  }

  client->sock = sock;
  client->method = NULL;
  client->target = NULL;
  client->content_length = 0;
  BufferCreate(&client->http_buffer);
  HttpParserReset(&client->http_parser);
  client->next = NULL;

  struct ClientContext** next = &g_service.clients;
  while (*next) next = &(*next)->next;
  *next = client;

  if (!IoMuxerOnRead(&g_service.io_muxer, g_service.http, OnHttpRead, NULL)) {
    LOGE("Failed to reschedule http client accept (%s)", strerror(errno));
    OnSignal(SIGABRT);
  }
  return;

free_client:
  free(client);
close_sock:
  close(sock);
}

static void OnMqttConnectAck(void* user, bool success) {
  (void)user;
  if (!success) {
    LOGW("Mqtt broker rejected connection");
    OnSignal(SIGTERM);
    return;
  }
  if (!MqttSubscribe(g_service.mqtt, 1, "+/#", 3)) {
    LOGE("Failed to subscribe to mqtt topic (%s)", strerror(errno));
    OnSignal(SIGABRT);
    return;
  }
}

static void OnMqttSubscribeAck(void* user, bool success) {
  (void)user;
  if (!success) {
    LOGW("Mqtt broker rejected subscription");
    OnSignal(SIGTERM);
    return;
  }
}

static void OnMqttPublish(void* user, const char* topic, size_t topic_size,
                          const void* payload, size_t payload_size) {
  (void)user;
  LOGD("%.*s <- %.*s", (int)topic_size, topic, (int)payload_size,
       (const char*)payload);
  void* payload_copy = malloc(payload_size);
  if (!payload_copy) {
    LOGW("Failed to copy payload (%s)", strerror(errno));
    return;
  }
  memcpy(payload_copy, payload, payload_size);
  struct Message key = {
      .topic = *(void**)(void*)&topic,
      .topic_size = topic_size,
  };
  struct Message** node = tsearch(&key, &g_service.messages, MessageCompare);
  if (!node) {
    LOGW("Failed to create message node (%s)", strerror(errno));
    goto free_payload;
  }
  if (*node == &key) {
    struct Message* message = MessageCreate(topic, topic_size);
    if (!message) {
      LOGW("Failed to create message (%s)", strerror(errno));
      goto delete_node;
    }
    *node = message;
  }
  free((*node)->payload);
  (*node)->payload = payload_copy;
  (*node)->payload_size = payload_size;
  return;

delete_node:
  tdelete(&key, &g_service.messages, MessageCompare);
free_payload:
  free(payload_copy);
}

static void OnMqttFinished(void* user, size_t offset) {
  (void)user;
  BufferDiscard(&g_service.mqtt_buffer, offset);
}

static void OnMqttRead(void* user) {
  (void)user;
  switch (BufferAppendFrom(&g_service.mqtt_buffer, g_service.mqtt)) {
    case -1:
      LOGE("Failed to read mqtt socket (%s)", strerror(errno));
      OnSignal(SIGABRT);
      return;
    case 0:
      LOGW("Mqtt broker closed connection");
      OnSignal(SIGTERM);
      return;
    default:
      break;
  }
  static const struct MqttParserCallbacks mqtt_callbacks = {
      .on_connect_ack = OnMqttConnectAck,
      .on_subscribe_ack = OnMqttSubscribeAck,
      .on_publish = OnMqttPublish,
      .on_finished = OnMqttFinished,
  };
  for (;;) {
    enum MqttParserResult result =
        MqttParserParse(g_service.mqtt_buffer.data, g_service.mqtt_buffer.size,
                        &mqtt_callbacks, NULL);
    switch (result) {
      case kMqttParserResultFinished:
        continue;
      case kMqttParserResultWantMore:
        if (!IoMuxerOnRead(&g_service.io_muxer, g_service.mqtt, OnMqttRead,
                           NULL)) {
          LOGE("Failed to schedule mqtt read (%s)", strerror(errno));
          OnSignal(SIGABRT);
        }
        return;
      case kMqttParserResultError:
        LOGE("Failed to parse mqtt message");
        OnSignal(SIGABRT);
        return;
      default:
        __builtin_unreachable();
    }
  }
}

int main(int argc, char* argv[]) {
  const struct sockaddr_in http_addr = GetHttpAddr();
  const struct sockaddr_in mqtt_addr = GetMqttAddr(argc, argv);

  IoMuxerCreate(&g_service.io_muxer);
  g_service.http = socket(AF_INET, SOCK_STREAM, 0);
  if (g_service.http == -1) {
    LOGE("Failed to create http socket (%s)", strerror(errno));
    return EXIT_FAILURE;
  }

  static const int one = 1;
  if (setsockopt(g_service.http, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one))) {
    LOGE("Failed to reuse http socket (%s)", strerror(errno));
    goto close_http;
  }
  if (bind(g_service.http, (const struct sockaddr*)&http_addr,
           sizeof(http_addr))) {
    LOGE("Failed to bind http socket (%s)", strerror(errno));
    goto close_http;
  }
  if (listen(g_service.http, SOMAXCONN)) {
    LOGE("Failed to listen http socket (%s)", strerror(errno));
    goto close_http;
  }
  g_service.mqtt = socket(AF_INET, SOCK_STREAM, 0);
  if (g_service.mqtt == -1) {
    LOGE("Failed to create mqtt socket (%s)", strerror(errno));
    goto close_http;
  }

  if (connect(g_service.mqtt, (const struct sockaddr*)&mqtt_addr,
              sizeof(mqtt_addr))) {
    LOGE("Failed to connect mqtt socket (%s)", strerror(errno));
    goto close_mqtt;
  }
  // TODO(mburakov): Implement keepalive
  if (!MqttConnect(g_service.mqtt, 65535)) {
    LOGE("Failed to connect mqtt (%s)", strerror(errno));
    goto close_mqtt;
  }

  if (signal(SIGINT, OnSignal) == SIG_ERR ||
      signal(SIGTERM, OnSignal) == SIG_ERR) {
    LOGE("Failed to set signal handlers (%s)", strerror(errno));
    goto disconnect_mqtt;
  }

  IoMuxerCreate(&g_service.io_muxer);
  if (!IoMuxerOnRead(&g_service.io_muxer, g_service.http, OnHttpRead, NULL) ||
      !IoMuxerOnRead(&g_service.io_muxer, g_service.mqtt, OnMqttRead, NULL)) {
    LOGE("Failed to init iomuxer (%s)", strerror(errno));
    goto destroy_iomuxer;
  }

  BufferCreate(&g_service.mqtt_buffer);
  while (!g_signal) {
    enum IoMuxerResult result = IoMuxerIterate(&g_service.io_muxer, -1);
    if (result == kIoMuxerResultError && errno != EINTR) {
      LOGE("Failed to iterate iomuxer (%s)", strerror(errno));
      OnSignal(SIGABRT);
    }
  }
  tdestroy(g_service.messages, MessageDestroy);
  while (g_service.clients) DropClientContext(g_service.clients);
  BufferDestroy(&g_service.mqtt_buffer);

destroy_iomuxer:
  IoMuxerDestroy(&g_service.io_muxer);
disconnect_mqtt:
  MqttDisconnect(g_service.mqtt);
close_mqtt:
  close(g_service.mqtt);
close_http:
  close(g_service.http);
  bool success = g_signal == SIGINT || g_signal == SIGTERM;
  return success ? EXIT_SUCCESS : EXIT_FAILURE;

#if 0
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
  context.lua_state = luaL_newstate();
  if (!context.lua_state) {
    Log("Failed to create lua state");
    goto rollback_mosquitto_connect;
  }
  luaL_openlibs(context.lua_state);
  // TODO(mburakov): Handle lua errors.
#if 0
  // mburakov: Userdata is broken on AArch64
  lua_pushlightuserdata(context.lua_state, &context);
#else
  void* ctx = &context;
  lua_pushlstring(context.lua_state, (const char*)&ctx, sizeof(void*));
#endif
  lua_pushcclosure(context.lua_state, LuaSubscribe, 1);
  lua_setglobal(context.lua_state, "subscribe");
#if 0
  // mburakov: Userdata is broken on AArch64
  lua_pushlightuserdata(context.lua_state, &context);
#else
  lua_pushlstring(context.lua_state, (const char*)&ctx, sizeof(void*));
#endif
  lua_pushcclosure(context.lua_state, LuaPublish, 1);
  lua_setglobal(context.lua_state, "publish");
  SourceCurrentDir(context.lua_state);
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
      context.size = 0;
      mosq_errno = mosquitto_loop_read(context.mosq, 1);
      if (mosq_errno != MOSQ_ERR_SUCCESS) {
        Log("Failed to read mosquitto (%s)", mosquitto_strerror(mosq_errno));
        goto rollback_lual_newstate;
      }
      // mburakov: Execute scheduled publishes if any.
      for (const char* ptr = context.buffer;
           ptr < context.buffer + context.size;) {
        const char* topic = ptr;
        ptr += strlen(ptr) + 1;
        const char* payload = ptr;
        size_t payload_size = strlen(payload);
        ptr += payload_size + 1;
        mosq_errno = mosquitto_publish(context.mosq, NULL, topic,
                                       (int)payload_size, payload, 0, false);
        if (mosq_errno != MOSQ_ERR_SUCCESS) {
          Log("Failed to publish mosquitto (%s)",
              mosquitto_strerror(mosq_errno));
        }
      }
      continue;
    }
    if (!ServerMaybeHandle(server, ev.data.fd))
      Terminate("Stray socket in epoll");
  }
  result = EXIT_SUCCESS;
rollback_lual_newstate:
  lua_close(context.lua_state);
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
#endif
}
