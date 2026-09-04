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

#ifndef NETUTILS_H_
#define NETUTILS_H_

#include <stdbool.h>
#include <stddef.h>

/* Big enough for MCC (3 digits) + MNC (up to 3 digits) + NUL */
#define TELEPHONY_NETWORK_ID_SIZE 7

/**
 * Concatenate MCC and MNC into buf ("262" + "02" -> "26202"). Returns false
 * (and an empty buf) when either part is missing or buf cannot hold both.
 */
bool telephony_format_network_id(const char *mcc, const char *mnc,
								 char *buf, size_t size);

/**
 * The numeric form of the same concatenation ("302" + "220" -> 302220).
 * MNCs keep their length: a two digit MNC yields a five digit id, a three
 * digit MNC a six digit one. Returns -1 when either part is missing.
 */
long telephony_network_id_to_number(const char *mcc, const char *mnc);

#endif

// vim:ts=4:sw=4:noexpandtab
