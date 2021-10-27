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

#ifndef MQHTTP_UHTTP_H_
#define MQHTTP_UHTTP_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

enum UhttpResult {
  kUhttpResultTerminate,
  kUhttpResultFailure,
  kUhttpResultWantMore,
  kUhttpResultFinished
};

struct Uhttp* UhttpCreate(void);
void UhttpDestroy(struct Uhttp* uhttp);

void UhttpReset(struct Uhttp* uhttp);
void* UhttpAllocate(struct Uhttp* uhttp, size_t size);
enum UhttpResult UhttpConsume(struct Uhttp* uhttp, size_t size);

const char* UhttpGetMethod(const struct Uhttp* uhttp);
const char* UhttpGetTarget(const struct Uhttp* uhttp);
const char* UhttpGetBody(const struct Uhttp* uhttp);
size_t UhttpGetBodySize(const struct Uhttp* uhttp);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // MQHTTP_UHTTP_H_
