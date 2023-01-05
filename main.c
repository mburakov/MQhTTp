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
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

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
  struct ServiceContext* service;
  struct ClientContext* next;
};

struct ServiceContext {
  int http;
  int mqtt;
  struct IoMuxer io_muxer;
  struct Buffer mqtt_buffer;
  struct ClientContext* clients;
};

static volatile sig_atomic_t g_signal;

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
  struct ClientContext** next = &client->service->clients;
  while (*next != client) next = &(*next)->next;
  *next = client->next;

  BufferDestroy(&client->http_buffer);
  if (client->target) free(client->target);
  if (client->method) free(client->method);
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
  switch (HttpParserParse(&client->http_parser, client->http_buffer.data,
                          client->http_buffer.size, &http_callbacks, user)) {
    case kHttpParserResultFinished:
      break;
    case kHttpParserResultWantMore:
      if (!IoMuxerOnRead(&client->service->io_muxer, client->sock, OnClientRead,
                         user)) {
        LOGW("Failed to reschedule http client read (%s)", strerror(errno));
        goto drop_client;
      }
      return;
    case kHttpParserResultError:
      LOGW("Failed to parse http request");
      goto drop_client;
    default:
      __builtin_unreachable();
  }

  HttpParserReset(&client->http_parser);
  if (strcmp(client->method, "GET")) {
    // TODO(mburakov): Unsupported method
    goto drop_client;
  }
  if (client->content_length) {
    // TODO(mburakov): Unsupported body
    goto drop_client;
  }

  static const char hurr_durr[] =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/html; charset=UTF-8\r\n"
      "Content-Length: 51\r\n"
      "\r\n"
      "<!DOCTYPE html>\n<html><body>"
      "hurr-durr"
      "</body></html>";
  write(client->sock, hurr_durr, sizeof(hurr_durr) - 1);

  free(client->method);
  client->method = NULL;
  free(client->target);
  client->target = NULL;
  client->content_length = 0;

  if (!IoMuxerOnRead(&client->service->io_muxer, client->sock, OnClientRead,
                     user)) {
    LOGW("Failed to reschedule http client read (%s)", strerror(errno));
    goto drop_client;
  }
  return;

drop_client:
  DropClientContext(client);
}

static void OnHttpRead(void* user) {
  struct ServiceContext* context = user;
  int sock = accept(context->http, NULL, NULL);
  if (sock == -1) {
    LOGW("Failed to accept http client (%s)", strerror(errno));
    return;
  }
  struct ClientContext* client = malloc(sizeof(struct ClientContext));
  if (!client) {
    LOGW("Failed to allocate http client context (%s)", strerror(errno));
    goto close_sock;
  }
  if (!IoMuxerOnRead(&context->io_muxer, sock, OnClientRead, client)) {
    LOGW("Failed to schedule http client read (%s)", strerror(errno));
    goto free_client;
  }

  client->sock = sock;
  client->method = NULL;
  client->target = NULL;
  client->content_length = 0;
  BufferCreate(&client->http_buffer);
  HttpParserReset(&client->http_parser);
  client->service = context;
  client->next = NULL;

  struct ClientContext** next = &context->clients;
  while (*next) next = &(*next)->next;
  *next = client;

  if (!IoMuxerOnRead(&context->io_muxer, context->http, OnHttpRead, user)) {
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
  struct ServiceContext* context = user;
  if (!success) {
    LOGW("Mqtt broker rejected connection");
    OnSignal(SIGTERM);
    return;
  }
  if (!MqttSubscribe(context->mqtt, 1, "+/#", 3)) {
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
  // TODO(mburakov): Implement me!
}

static void OnMqttFinished(void* user, size_t offset) {
  struct ServiceContext* context = user;
  BufferDiscard(&context->mqtt_buffer, offset);
}

static void OnMqttRead(void* user) {
  struct ServiceContext* context = user;
  switch (BufferAppendFrom(&context->mqtt_buffer, context->mqtt)) {
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
        MqttParserParse(context->mqtt_buffer.data, context->mqtt_buffer.size,
                        &mqtt_callbacks, context);
    switch (result) {
      case kMqttParserResultFinished:
        continue;
      case kMqttParserResultWantMore:
        if (!IoMuxerOnRead(&context->io_muxer, context->mqtt, OnMqttRead,
                           context)) {
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
  struct sockaddr_in http_addr = GetHttpAddr();
  struct sockaddr_in mqtt_addr = GetMqttAddr(argc, argv);

  struct ServiceContext context;
  IoMuxerCreate(&context.io_muxer);
  context.http = socket(AF_INET, SOCK_STREAM, 0);
  if (context.http == -1) {
    LOGE("Failed to create http socket (%s)", strerror(errno));
    return EXIT_FAILURE;
  }

  int one = 1;
  if (setsockopt(context.http, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one))) {
    LOGE("Failed to reuse http socket (%s)", strerror(errno));
    goto close_http;
  }
  if (bind(context.http, (struct sockaddr*)&http_addr, sizeof(http_addr))) {
    LOGE("Failed to bind http socket (%s)", strerror(errno));
    goto close_http;
  }
  if (listen(context.http, SOMAXCONN)) {
    LOGE("Failed to listen http socket (%s)", strerror(errno));
    goto close_http;
  }
  context.mqtt = socket(AF_INET, SOCK_STREAM, 0);
  if (context.mqtt == -1) {
    LOGE("Failed to create mqtt socket (%s)", strerror(errno));
    goto close_http;
  }

  if (connect(context.mqtt, (struct sockaddr*)&mqtt_addr, sizeof(mqtt_addr))) {
    LOGE("Failed to connect mqtt socket (%s)", strerror(errno));
    goto close_mqtt;
  }
  // TODO(mburakov): Implement keepalive
  if (!MqttConnect(context.mqtt, 65535)) {
    LOGE("Failed to connect mqtt (%s)", strerror(errno));
    goto close_mqtt;
  }

  if (signal(SIGINT, OnSignal) == SIG_ERR ||
      signal(SIGTERM, OnSignal) == SIG_ERR) {
    LOGE("Failed to set signal handlers (%s)", strerror(errno));
    goto disconnect_mqtt;
  }

  IoMuxerCreate(&context.io_muxer);
  if (!IoMuxerOnRead(&context.io_muxer, context.http, OnHttpRead, &context) ||
      !IoMuxerOnRead(&context.io_muxer, context.mqtt, OnMqttRead, &context)) {
    LOGE("Failed to init iomuxer (%s)", strerror(errno));
    goto destroy_iomuxer;
  }

  BufferCreate(&context.mqtt_buffer);
  while (!g_signal) {
    enum IoMuxerResult result = IoMuxerIterate(&context.io_muxer, -1);
    if (result == kIoMuxerResultError && errno != EINTR) {
      LOGE("Failed to iterate iomuxer (%s)", strerror(errno));
      OnSignal(SIGABRT);
    }
  }

  while (context.clients) {
    DropClientContext(context.clients);
  }

destroy_iomuxer:
  IoMuxerDestroy(&context.io_muxer);
disconnect_mqtt:
  MqttDisconnect(context.mqtt);
close_mqtt:
  close(context.mqtt);
close_http:
  close(context.http);
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
