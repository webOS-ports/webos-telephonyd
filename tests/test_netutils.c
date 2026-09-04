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

#include "netutils.h"

static void test_format_two_digit_mnc(void)
{
	char buf[TELEPHONY_NETWORK_ID_SIZE];

	g_assert_true(telephony_format_network_id("262", "02", buf, sizeof(buf)));
	g_assert_cmpstr(buf, ==, "26202");
}

static void test_format_three_digit_mnc(void)
{
	char buf[TELEPHONY_NETWORK_ID_SIZE];

	/* The old char[6] buffer truncated the last digit of 3-digit MNCs */
	g_assert_true(telephony_format_network_id("302", "220", buf, sizeof(buf)));
	g_assert_cmpstr(buf, ==, "302220");
}

static void test_format_missing_parts(void)
{
	char buf[TELEPHONY_NETWORK_ID_SIZE];

	g_assert_false(telephony_format_network_id(NULL, "02", buf, sizeof(buf)));
	g_assert_cmpstr(buf, ==, "");
	g_assert_false(telephony_format_network_id("262", NULL, buf, sizeof(buf)));
	g_assert_cmpstr(buf, ==, "");
	g_assert_false(telephony_format_network_id("", "", buf, sizeof(buf)));
	g_assert_cmpstr(buf, ==, "");
}

static void test_format_buffer_too_small(void)
{
	char buf[4];

	g_assert_false(telephony_format_network_id("302", "220", buf, sizeof(buf)));
	g_assert_cmpstr(buf, ==, "");
}

static void test_id_to_number(void)
{
	g_assert_cmpint(telephony_network_id_to_number("262", "02"), ==, 26202);
	/* The old (mcc * 100) + mnc arithmetic turned 302/220 into 30420 */
	g_assert_cmpint(telephony_network_id_to_number("302", "220"), ==, 302220);
	g_assert_cmpint(telephony_network_id_to_number(NULL, "220"), ==, -1);
	g_assert_cmpint(telephony_network_id_to_number("302", NULL), ==, -1);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/netutils/format-two-digit-mnc", test_format_two_digit_mnc);
	g_test_add_func("/netutils/format-three-digit-mnc", test_format_three_digit_mnc);
	g_test_add_func("/netutils/format-missing-parts", test_format_missing_parts);
	g_test_add_func("/netutils/format-buffer-too-small", test_format_buffer_too_small);
	g_test_add_func("/netutils/id-to-number", test_id_to_number);

	return g_test_run();
}

// vim:ts=4:sw=4:noexpandtab
