/*
 * Copyright (C) 2023 Mikhail Burakov. This file is part of MQhTTp.
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

#include "message.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "toolbox/utils.h"

struct Message* MessageCreate(const char* topic, size_t topic_size) {
  struct Message* message = malloc(sizeof(struct Message));
  if (!message) {
    LOGW("Failed to allocate message (%s)", strerror(errno));
    return NULL;
  }
  message->topic = malloc(topic_size);
  if (!message->topic) {
    LOGW("Failed to copy topic (%s)", strerror(errno));
    goto free_message;
  }
  memcpy(message->topic, topic, topic_size);
  message->topic_size = topic_size;
  message->payload = NULL;
  message->payload_size = 0;
  return message;

free_message:
  free(message);
  return NULL;
}

int MessageCompare(const void* a, const void* b) {
  const struct Message* message_a = a;
  const struct Message* message_b = b;
  if (message_a->topic_size > message_b->topic_size) return 1;
  if (message_a->topic_size < message_b->topic_size) return -1;
  return memcmp(message_a->topic, message_b->topic, message_a->topic_size);
}

void MessageDestroy(void* message) {
  struct Message* instance = message;
  free(instance->payload);
  free(instance->topic);
  free(instance);
}
