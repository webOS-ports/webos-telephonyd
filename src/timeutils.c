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

#include <glib.h>

#include "timeutils.h"

time_t telephony_decode_iso8601_time(const char *str)
{
	GTimeZone *local;
	GDateTime *dt;
	time_t result;

	if (!str)
		return (time_t) -1;

	/* The fallback zone only applies when the string carries no offset of
	 * its own; ofono's LocalSentTime is wall-clock time in the local zone. */
	local = g_time_zone_new_local();
	dt = g_date_time_new_from_iso8601(str, local);
	g_time_zone_unref(local);

	if (!dt)
		return (time_t) -1;

	result = (time_t) g_date_time_to_unix(dt);
	g_date_time_unref(dt);

	return result;
}

// vim:ts=4:sw=4:noexpandtab
