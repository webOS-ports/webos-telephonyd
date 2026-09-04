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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netutils.h"

bool telephony_format_network_id(const char *mcc, const char *mnc,
								 char *buf, size_t size)
{
	int needed;

	if (size > 0)
		buf[0] = '\0';

	if (!mcc || !mnc || !mcc[0] || !mnc[0])
		return false;

	needed = snprintf(buf, size, "%s%s", mcc, mnc);
	if (needed < 0 || (size_t) needed >= size) {
		if (size > 0)
			buf[0] = '\0';
		return false;
	}

	return true;
}

long telephony_network_id_to_number(const char *mcc, const char *mnc)
{
	char buf[TELEPHONY_NETWORK_ID_SIZE];

	if (!telephony_format_network_id(mcc, mnc, buf, sizeof(buf)))
		return -1;

	return strtol(buf, NULL, 10);
}

// vim:ts=4:sw=4:noexpandtab
