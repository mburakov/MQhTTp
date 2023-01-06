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

#include "http_utils.h"

#include <errno.h>
#include <string.h>

#include "toolbox/utils.h"

static const char part1[] = {'H', 'T', 'T', 'P', '/', '1', '.', '1', ' '};
static const char part2[] = {'\r', '\n', 'C', 'o', 'n', 't', 'e', 'n', 't',
                             '-',  'L',  'e', 'n', 'g', 't', 'h', ':', ' '};
static const char part3[] = {'\r', '\n', 'C', 'o', 'n', 't', 'e', 'n',
                             't',  '-',  'T', 'y', 'p', 'e', ':', ' '};
static const char part4[] = {'\r', '\n', '\r', '\n'};

static size_t FormatSize(char* buffer, size_t size) {
  if (!size) {
    buffer[0] = '0';
    return 1;
  }
  size_t power = 1, index = 0;
  while (power <= size) power *= 10;
  while (power /= 10) buffer[index++] = '0' + (size / power) % 10;
  return index;
}

static void AppendIov(struct iovec** ptr, const char* base, size_t len) {
  struct iovec* iov = *ptr;
  iov->iov_base = *(void**)(void*)&base;
  iov->iov_len = len;
  *ptr = iov + 1;
}

static bool SendAll(int fd, struct iovec* iov, int iov_count) {
  size_t total_size = 0;
  for (int i = 0; i < iov_count; i++) {
    total_size += iov[i].iov_len;
  }
  while (total_size) {
    ssize_t result = writev(fd, iov, iov_count);
    if (result == -1) {
      LOGW("Failed to send body chunk (%s)", strerror(errno));
      return false;
    }
    for (int i = 0; i < iov_count; i++) {
      size_t delta = MIN(iov[i].iov_len, (size_t)result);
      iov[i].iov_len -= delta;
      total_size -= delta;
      result -= delta;
    }
  }
  return true;
}

bool SendHttpReply(int fd, const char* status, const char* content_type,
                   const struct iovec* chunks, size_t chunks_count) {
  size_t content_length = 0;
  for (size_t i = 0; i < chunks_count; i++) {
    content_length += chunks[i].iov_len;
  }

  char buffer[24];
  struct iovec iov[7 + chunks_count];
  struct iovec* ptr = iov;
  AppendIov(&ptr, part1, sizeof(part1));
  AppendIov(&ptr, status, strlen(status));
  AppendIov(&ptr, part2, sizeof(part2));
  AppendIov(&ptr, buffer, FormatSize(buffer, content_length));
  if (content_length && content_type) {
    AppendIov(&ptr, part3, sizeof(part3));
    AppendIov(&ptr, content_type, strlen(content_type));
  }
  AppendIov(&ptr, part4, sizeof(part4));
  for (size_t i = 0; i < chunks_count; i++) {
    *ptr++ = *chunks++;
  }

  return SendAll(fd, iov, (int)(ptr - iov));
}
