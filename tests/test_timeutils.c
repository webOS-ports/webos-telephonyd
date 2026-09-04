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
#include <stdlib.h>

#include "timeutils.h"

/* 2026-09-04T12:00:00Z */
#define REF_UTC 1788523200

static void test_decode_with_utc_offset(void)
{
	g_assert_cmpint(telephony_decode_iso8601_time("2026-09-04T12:00:00+0000"), ==, REF_UTC);
	g_assert_cmpint(telephony_decode_iso8601_time("2026-09-04T12:00:00Z"), ==, REF_UTC);
}

static void test_decode_with_positive_offset(void)
{
	/* 14:00 at +0200 is 12:00 UTC */
	g_assert_cmpint(telephony_decode_iso8601_time("2026-09-04T14:00:00+0200"), ==, REF_UTC);
	g_assert_cmpint(telephony_decode_iso8601_time("2026-09-04T14:00:00+02:00"), ==, REF_UTC);
}

static void test_decode_with_negative_offset(void)
{
	/* 07:30 at -0430 is 12:00 UTC */
	g_assert_cmpint(telephony_decode_iso8601_time("2026-09-04T07:30:00-0430"), ==, REF_UTC);
}

static void test_decode_seconds_survive(void)
{
	/* The old decoder subtracted one from hour, minute and second; make sure
	 * every field round-trips exactly. */
	g_assert_cmpint(telephony_decode_iso8601_time("2026-09-04T12:34:56Z"), ==,
					REF_UTC + 34 * 60 + 56);
}

static void test_decode_without_offset_is_local(void)
{
	GTimeZone *local = g_time_zone_new_local();
	GDateTime *dt = g_date_time_new(local, 2026, 9, 4, 12, 0, 0);
	time_t expected = (time_t) g_date_time_to_unix(dt);

	g_assert_cmpint(telephony_decode_iso8601_time("2026-09-04T12:00:00"), ==, expected);

	g_date_time_unref(dt);
	g_time_zone_unref(local);
}

static void test_decode_invalid(void)
{
	g_assert_cmpint(telephony_decode_iso8601_time(NULL), ==, (time_t) -1);
	g_assert_cmpint(telephony_decode_iso8601_time(""), ==, (time_t) -1);
	g_assert_cmpint(telephony_decode_iso8601_time("not a timestamp"), ==, (time_t) -1);
	g_assert_cmpint(telephony_decode_iso8601_time("2026-13-40T99:99:99Z"), ==, (time_t) -1);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/timeutils/utc-offset", test_decode_with_utc_offset);
	g_test_add_func("/timeutils/positive-offset", test_decode_with_positive_offset);
	g_test_add_func("/timeutils/negative-offset", test_decode_with_negative_offset);
	g_test_add_func("/timeutils/seconds-survive", test_decode_seconds_survive);
	g_test_add_func("/timeutils/no-offset-is-local", test_decode_without_offset_is_local);
	g_test_add_func("/timeutils/invalid", test_decode_invalid);

	return g_test_run();
}

// vim:ts=4:sw=4:noexpandtab
