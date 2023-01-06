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

#ifndef MQHTTP_HTTP_UTILS_H_
#define MQHTTP_HTTP_UTILS_H_

#include <stdbool.h>
#include <stddef.h>
#include <sys/uio.h>

bool SendHttpReply(int fd, const char* status, const char* content_type,
                   const struct iovec* chunks, size_t chunks_count);

#endif  // MQHTTP_HTTP_UTILS_H_
