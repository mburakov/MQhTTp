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

#ifndef MQHTTP_SERVER_H_
#define MQHTTP_SERVER_H_

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

typedef bool (*ServerHandler)(void* user, int fd, const char* method,
                              const char* target, const void* body,
                              size_t body_size);

struct Server* ServerCreate(int epfd, ServerHandler handler, void* user);
void ServerDestroy(struct Server* server);

bool ServerMaybeHandle(struct Server* server, int fd);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // MQHTTP_SERVER_H_
