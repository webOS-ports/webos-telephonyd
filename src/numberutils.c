/* @@@LICENSE
*
* Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
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

#include <glib.h>

#include "numberutils.h"

gchar *phone_number_normalize(const char *addr)
{
	GString *out;
	const char *p;
	unsigned int digits = 0;

	if (!addr)
		return NULL;

	out = g_string_new(NULL);

	for (p = addr; *p; p++) {
		if (*p == ' ' || *p == '\t' || *p == '-' || *p == '.' || *p == '(' || *p == ')')
			continue;

		if (*p == '+' && out->len == 0) {
			g_string_append_c(out, '+');
			continue;
		}

		if (*p >= '0' && *p <= '9') {
			if (++digits > PHONE_NUMBER_MAX_DIGITS)
				goto bad;
			g_string_append_c(out, *p);
			continue;
		}

		goto bad;
	}

	if (digits == 0)
		goto bad;

	return g_string_free(out, FALSE);

bad:
	g_string_free(out, TRUE);
	return NULL;
}
