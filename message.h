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

#ifndef MQHTTP_MESSAGE_H_
#define MQHTTP_MESSAGE_H_

#include <stddef.h>

struct Message {
  char* topic;
  size_t topic_size;
  void* payload;
  size_t payload_size;
};

struct Message* MessageCreate(const char* topic, size_t topic_size);
int MessageCompare(const void* a, const void* b);
void MessageDestroy(void* message);

#endif  // MQHTTP_MESSAGE_H_
