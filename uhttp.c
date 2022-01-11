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

#include "uhttp.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct Uhttp {
  struct {
    char* data;
    size_t alloc;
    size_t size;
  } buffer;
  struct {
    const char* method;
    const char* target;
    const char* body;
    size_t body_size;
  } public;
  struct {
    _Bool (*state)(struct Uhttp*, char*);
    const char* field_name;
    const char* field_value;
    char* maybe_ows;
    _Bool maybe_eol;
  } parser;
};

static _Bool StrEq(const char* a, const char* b) {
  for (; *a && *b; a++, b++) {
    if (tolower(*a) != tolower(*b)) return 0;
  }
  return *a == *b;
}

static _Bool UhttpOnMethod(struct Uhttp* uhttp, char* glyph);
static _Bool UhttpOnTarget(struct Uhttp* uhttp, char* glyph);
static _Bool UhttpOnVersion(struct Uhttp* uhttp, char* glyph);
static _Bool UhttpOnFieldName(struct Uhttp* uhttp, char* glyph);
static _Bool UhttpOnFieldValue(struct Uhttp* uhttp, char* glyph);
static _Bool UhttpOnBody(struct Uhttp* uhttp, char* glyph);
static _Bool UhttpOnStopped(struct Uhttp* uhttp, char* glyph);

static _Bool UhttpOnMethod(struct Uhttp* uhttp, char* glyph) {
  switch (*glyph) {
    case ' ':
      if (!uhttp->public.method) return 0;
      uhttp->parser.state = UhttpOnTarget;
      *glyph = 0;
      return 1;
    default:
      if (!isgraph(*glyph)) return 0;
      if (!uhttp->public.method) uhttp->public.method = glyph;
      return 1;
  }
}

static _Bool UhttpOnTarget(struct Uhttp* uhttp, char* glyph) {
  switch (*glyph) {
    case ' ':
      if (!uhttp->public.target) return 0;
      uhttp->parser.state = UhttpOnVersion;
      *glyph = 0;
      return 1;
    default:
      if (!isgraph(*glyph)) return 0;
      if (!uhttp->public.target) uhttp->public.target = glyph;
      return 1;
  }
}

static _Bool UhttpOnVersion(struct Uhttp* uhttp, char* glyph) {
  switch (*glyph) {
    case '\r':
      if (uhttp->parser.maybe_eol) return 0;
      uhttp->parser.maybe_eol = 1;
      return 1;
    case '\n':
      if (!uhttp->parser.maybe_eol) return 0;
      uhttp->parser.state = UhttpOnFieldName;
      uhttp->parser.maybe_eol = 0;
      return 1;
    default:
      if (uhttp->parser.maybe_eol) return 0;
      if (!isprint(*glyph)) return 0;
      return 1;
  }
}

static _Bool UhttpOnFieldName(struct Uhttp* uhttp, char* glyph) {
  switch (*glyph) {
    case '\r':
      if (uhttp->parser.maybe_eol) return 0;
      uhttp->parser.maybe_eol = 1;
      return 1;
    case '\n':
      if (!uhttp->parser.maybe_eol) return 0;
      uhttp->parser.state =
          uhttp->public.body_size ? UhttpOnBody : UhttpOnStopped;
      return 1;
    case ' ':
      return 0;
    case ':':
      if (uhttp->parser.maybe_eol) return 0;
      if (!uhttp->parser.field_name) return 0;
      uhttp->parser.state = UhttpOnFieldValue;
      uhttp->parser.field_value = NULL;
      *glyph = 0;
      return 1;
    default:
      if (uhttp->parser.maybe_eol) return 0;
      if (!isgraph(*glyph)) return 0;
      if (!uhttp->parser.field_name) uhttp->parser.field_name = glyph;
      return 1;
  }
}

static _Bool UhttpOnFieldValue(struct Uhttp* uhttp, char* glyph) {
  switch (*glyph) {
    case '\r':
      if (uhttp->parser.maybe_eol) return 0;
      uhttp->parser.maybe_ows = glyph;
      uhttp->parser.maybe_eol = 1;
      return 1;
    case '\n':
      if (!uhttp->parser.maybe_eol) return 0;
      if (!uhttp->parser.field_value) return 0;
      *uhttp->parser.maybe_ows = 0;
      if (!uhttp->public.body_size &&
          StrEq(uhttp->parser.field_name, "Content-Length"))
        uhttp->public.body_size = (size_t)atoi(uhttp->parser.field_value);
      uhttp->parser.state = UhttpOnFieldName;
      uhttp->parser.field_name = NULL;
      uhttp->parser.maybe_eol = 0;
      return 1;
    case '\t':
    case ' ':
      if (uhttp->parser.maybe_eol) return 0;
      if (!uhttp->parser.field_value) return 1;
      if (!uhttp->parser.maybe_ows) uhttp->parser.maybe_ows = glyph;
      return 1;
    default:
      if (uhttp->parser.maybe_eol) return 0;
      if (!isgraph(*glyph)) return 0;
      if (!uhttp->parser.field_value) uhttp->parser.field_value = glyph;
      uhttp->parser.maybe_ows = NULL;
      return 1;
  }
}

static _Bool UhttpOnBody(struct Uhttp* uhttp, char* glyph) {
  if (!uhttp->public.body) uhttp->public.body = glyph;
  ptrdiff_t body_size = glyph - uhttp->public.body + 1;
  if ((size_t)body_size == uhttp->public.body_size)
    uhttp->parser.state = UhttpOnStopped;
  return 1;
}

static _Bool UhttpOnStopped(struct Uhttp* uhttp, char* glyph) {
  (void)uhttp;
  (void)glyph;
  return 0;
}

static void UhttpRelocateBuffer(struct Uhttp* uhttp, char* data) {
  if (uhttp->public.method)
    uhttp->public.method = data + (uhttp->public.method - uhttp->buffer.data);
  if (uhttp->public.target)
    uhttp->public.target = data + (uhttp->public.target - uhttp->buffer.data);
  if (uhttp->public.body)
    uhttp->public.body = data + (uhttp->public.body - uhttp->buffer.data);
  if (uhttp->parser.field_name) {
    uhttp->parser.field_name =
        data + (uhttp->parser.field_name - uhttp->buffer.data);
  }
  if (uhttp->parser.field_value) {
    uhttp->parser.field_value =
        data + (uhttp->parser.field_value - uhttp->buffer.data);
  }
  if (uhttp->parser.maybe_ows) {
    uhttp->parser.maybe_ows =
        data + (uhttp->parser.maybe_ows - uhttp->buffer.data);
  }
  uhttp->buffer.data = data;
}

struct Uhttp* UhttpCreate() {
  struct Uhttp* result = calloc(1, sizeof(struct Uhttp));
  UhttpReset(result);
  return result;
}

void UhttpDestroy(struct Uhttp* uhttp) {
  free(uhttp->buffer.data);
  free(uhttp);
}

void UhttpReset(struct Uhttp* uhttp) {
  const struct Uhttp reset = {.buffer.data = uhttp->buffer.data,
                              .buffer.alloc = uhttp->buffer.alloc,
                              .parser.state = UhttpOnMethod};
  *uhttp = reset;
}

void* UhttpAllocate(struct Uhttp* uhttp, size_t size) {
  size_t alloc = uhttp->buffer.size + size;
  if (alloc > uhttp->buffer.alloc) {
    char* data = realloc(uhttp->buffer.data, alloc);
    if (!data) return NULL;
    if (data != uhttp->buffer.data) UhttpRelocateBuffer(uhttp, data);
    uhttp->buffer.alloc = alloc;
  }
  return uhttp->buffer.data + uhttp->buffer.size;
}

enum UhttpResult UhttpConsume(struct Uhttp* uhttp, size_t size) {
  if (uhttp->buffer.size + size > uhttp->buffer.alloc)
    return kUhttpResultTerminate;
  size_t end = uhttp->buffer.size + size;
  for (; uhttp->buffer.size < end; uhttp->buffer.size++) {
    if (!uhttp->parser.state(uhttp, &uhttp->buffer.data[uhttp->buffer.size])) {
      uhttp->parser.state = UhttpOnStopped;
      return kUhttpResultFailure;
    }
    if (uhttp->parser.state == UhttpOnStopped) {
      return kUhttpResultFinished;
    }
  }
  return kUhttpResultWantMore;
}

const char* UhttpGetMethod(const struct Uhttp* uhttp) {
  return uhttp->public.method;
}

const char* UhttpGetTarget(const struct Uhttp* uhttp) {
  return uhttp->public.target;
}

const char* UhttpGetBody(const struct Uhttp* uhttp) {
  return uhttp->public.body;
}

size_t UhttpGetBodySize(const struct Uhttp* uhttp) {
  return uhttp->public.body_size;
}
