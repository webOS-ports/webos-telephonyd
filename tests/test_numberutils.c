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

static void check(const char *addr, const char *expected)
{
	gchar *number = phone_number_normalize(addr);

	g_assert_cmpstr(number, ==, expected);
	g_free(number);
}

/* As the Messaging app has an address when it comes from a contact */
static void test_formatted(void)
{
	check("+31 6 1234 5678", "+31612345678");
	check("+31 6 1234-5678", "+31612345678");
	check("(030) 123.45.67", "0301234567");
	check("\t+44 (0)20 7946 0018 ", "+4402079460018");
}

static void test_plain(void)
{
	check("+31612345678", "+31612345678");
	check("0612345678", "0612345678");
	check("112", "112");
}

static void test_length(void)
{
	check("12345678901234567890", "12345678901234567890");
	check("+12345678901234567890", "+12345678901234567890");
	check("1234 5678 9012 3456 7890", "12345678901234567890");
	check("123456789012345678901", NULL);
}

/* Not numbers: these go on as they are */
static void test_not_numbers(void)
{
	check(NULL, NULL);
	check("", NULL);
	check(" ", NULL);
	check("+", NULL);
	check("++31612345678", NULL);
	check("31+612345678", NULL);
	check("someone@example.org", NULL);
	check("Lebara", NULL);
	check("123/TYPE=PLMN", NULL);
	check("*100#", NULL);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/numberutils/formatted", test_formatted);
	g_test_add_func("/numberutils/plain", test_plain);
	g_test_add_func("/numberutils/length", test_length);
	g_test_add_func("/numberutils/not-numbers", test_not_numbers);

	return g_test_run();
}
