/* @@@LICENSE
*
* Copyright (c) 2026 LuneOS project
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
* LICENSE@@@ */

#ifndef TIMEUTILS_H_
#define TIMEUTILS_H_

#include <time.h>

/**
 * Parse an ISO 8601 timestamp as ofono delivers it for messages, e.g.
 * "2026-09-04T14:03:12+0200". A timezone suffix (+hh, +hhmm, +hh:mm, Z)
 * is honored; a timestamp without one is interpreted as local time.
 *
 * Returns the Unix time, or (time_t) -1 if the string cannot be parsed.
 */
time_t telephony_decode_iso8601_time(const char *str);

#endif

// vim:ts=4:sw=4:noexpandtab
