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

#ifndef MQHTTP_LOGGING_H_
#define MQHTTP_LOGGING_H_

#define STR_IMPL(op) #op
#define STR(op) STR_IMPL(op)
#define LOGGING_WRAP(impl, fmt, ...) \
  impl(__FILE__ ":" STR(__LINE__) " " fmt "\n", ##__VA_ARGS__)
#define Terminate(fmt, ...) LOGGING_WRAP(TerminateImpl, fmt, ##__VA_ARGS__)
#define Log(fmt, ...) LOGGING_WRAP(LogImpl, fmt, ##__VA_ARGS__)

_Noreturn void TerminateImpl(const char* fmt, ...)
    __attribute__((__format__(printf, 1, 2)));

void LogImpl(const char* fmt, ...) __attribute__((__format__(printf, 1, 2)));

#endif  // MQHTTP_LOGGING_H_
