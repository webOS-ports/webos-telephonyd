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

#include "telephonydriver.h"

static void test_from_string_known_modes(void)
{
	g_assert_cmpint(telephony_radio_access_mode_from_string("any"), ==,
					TELEPHONY_RADIO_ACCESS_MODE_ANY);
	g_assert_cmpint(telephony_radio_access_mode_from_string("gsm"), ==,
					TELEPHONY_RADIO_ACCESS_MODE_GSM);
	g_assert_cmpint(telephony_radio_access_mode_from_string("umts"), ==,
					TELEPHONY_RADIO_ACCESS_MODE_UMTS);
	g_assert_cmpint(telephony_radio_access_mode_from_string("lte"), ==,
					TELEPHONY_RADIO_ACCESS_MODE_LTE);
}

static void test_from_string_invalid(void)
{
	/* The enum used to be unsigned, so the -1 "invalid" return survived no
	 * < 0 check and 0xFFFFFFFF reached the driver */
	g_assert_cmpint(telephony_radio_access_mode_from_string("foo"), ==,
					TELEPHONY_RADIO_ACCESS_MODE_INVALID);
	g_assert_true(telephony_radio_access_mode_from_string("foo") < 0);

	/* A non-string luna payload like {"mode": 1} hands us NULL; this used to
	 * crash in g_str_equal */
	g_assert_cmpint(telephony_radio_access_mode_from_string(NULL), ==,
					TELEPHONY_RADIO_ACCESS_MODE_INVALID);
}

static void test_to_string_round_trip(void)
{
	const char *names[] = { "any", "gsm", "umts", "lte" };
	unsigned int n;

	for (n = 0; n < G_N_ELEMENTS(names); n++) {
		enum telephony_radio_access_mode mode = telephony_radio_access_mode_from_string(names[n]);
		g_assert_cmpstr(telephony_radio_access_mode_to_string(mode), ==, names[n]);
	}
}

static void test_to_string_out_of_range(void)
{
	g_assert_cmpstr(telephony_radio_access_mode_to_string(TELEPHONY_RADIO_ACCESS_MODE_UNKNOWN),
					==, "unknown");
	g_assert_cmpstr(telephony_radio_access_mode_to_string(TELEPHONY_RADIO_ACCESS_MODE_INVALID),
					==, "unknown");
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ratmode/from-string-known", test_from_string_known_modes);
	g_test_add_func("/ratmode/from-string-invalid", test_from_string_invalid);
	g_test_add_func("/ratmode/round-trip", test_to_string_round_trip);
	g_test_add_func("/ratmode/out-of-range", test_to_string_out_of_range);

	return g_test_run();
}

// vim:ts=4:sw=4:noexpandtab
