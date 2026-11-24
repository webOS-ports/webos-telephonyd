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

/*
 * The radio access technology, from ofono's names to com.palm.wan's network
 * type, including 5G: "nr" from ofono, "5g" towards the status bar.
 */

#include <glib.h>

#include "ofonotechnology.h"

static void test_parse_technology(void)
{
	g_assert_cmpint(parse_ofono_network_technology("gsm"), ==, OFONO_NETWORK_TECHNOLOGY_GSM);
	g_assert_cmpint(parse_ofono_network_technology("edge"), ==, OFONO_NETWORK_TECHNOLOGY_EDGE);
	g_assert_cmpint(parse_ofono_network_technology("umts"), ==, OFONO_NETWORK_TECHNOLOGY_UMTS);
	g_assert_cmpint(parse_ofono_network_technology("hspa"), ==, OFONO_NETWORK_TECHNOLOGY_HSPA);
	g_assert_cmpint(parse_ofono_network_technology("lte"), ==, OFONO_NETWORK_TECHNOLOGY_LTE);
	g_assert_cmpint(parse_ofono_network_technology("nr"), ==, OFONO_NETWORK_TECHNOLOGY_NR);

	/* An empty name is what oFono gives for a technology it cannot name */
	g_assert_cmpint(parse_ofono_network_technology(""), ==, OFONO_NETWORK_TECHNOLOGY_UNKNOWN);
	g_assert_cmpint(parse_ofono_network_technology("5g"), ==, OFONO_NETWORK_TECHNOLOGY_UNKNOWN);
	g_assert_cmpint(parse_ofono_network_technology(NULL), ==, OFONO_NETWORK_TECHNOLOGY_UNKNOWN);
}

static void test_parse_bearer(void)
{
	g_assert_cmpint(parse_ofono_connection_bearer("gprs"), ==, OFONO_CONNECTION_BEARER_GPRS);
	g_assert_cmpint(parse_ofono_connection_bearer("hspa"), ==, OFONO_CONNECTION_BEARER_HSPA);
	g_assert_cmpint(parse_ofono_connection_bearer("lte"), ==, OFONO_CONNECTION_BEARER_LTE);
	g_assert_cmpint(parse_ofono_connection_bearer("none"), ==, OFONO_CONNECTION_BEARER_UNKNOWN);
	g_assert_cmpint(parse_ofono_connection_bearer(NULL), ==, OFONO_CONNECTION_BEARER_UNKNOWN);
}

static void test_select_without_data(void)
{
	/* No bearer: the registered technology decides */
	g_assert_cmpint(select_wan_network_type(OFONO_CONNECTION_BEARER_UNKNOWN, OFONO_NETWORK_TECHNOLOGY_UMTS),
					==, WAN_NETWORK_TYPE_UMTS);
	g_assert_cmpint(select_wan_network_type(OFONO_CONNECTION_BEARER_UNKNOWN, OFONO_NETWORK_TECHNOLOGY_HSPA),
					==, WAN_NETWORK_TYPE_HSDPA);
	g_assert_cmpint(select_wan_network_type(OFONO_CONNECTION_BEARER_UNKNOWN, OFONO_NETWORK_TECHNOLOGY_LTE),
					==, WAN_NETWORK_TYPE_LTE);
	g_assert_cmpint(select_wan_network_type(OFONO_CONNECTION_BEARER_UNKNOWN, OFONO_NETWORK_TECHNOLOGY_NR),
					==, WAN_NETWORK_TYPE_NR);
	g_assert_cmpint(select_wan_network_type(OFONO_CONNECTION_BEARER_UNKNOWN, OFONO_NETWORK_TECHNOLOGY_UNKNOWN),
					==, WAN_NETWORK_TYPE_NONE);
}

static void test_select_with_data(void)
{
	/* A bearer otherwise decides, as before */
	g_assert_cmpint(select_wan_network_type(OFONO_CONNECTION_BEARER_HSPA, OFONO_NETWORK_TECHNOLOGY_UMTS),
					==, WAN_NETWORK_TYPE_HSDPA);
	g_assert_cmpint(select_wan_network_type(OFONO_CONNECTION_BEARER_LTE, OFONO_NETWORK_TECHNOLOGY_LTE),
					==, WAN_NETWORK_TYPE_LTE);

	/* LTE data on a cell with an NR leg (EN-DC) is 5G */
	g_assert_cmpint(select_wan_network_type(OFONO_CONNECTION_BEARER_LTE, OFONO_NETWORK_TECHNOLOGY_NR),
					==, WAN_NETWORK_TYPE_NR);

	/* A 3G bearer while registration still says NR follows the bearer */
	g_assert_cmpint(select_wan_network_type(OFONO_CONNECTION_BEARER_HSPA, OFONO_NETWORK_TECHNOLOGY_NR),
					==, WAN_NETWORK_TYPE_HSDPA);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ofonotechnology/parse-technology", test_parse_technology);
	g_test_add_func("/ofonotechnology/parse-bearer", test_parse_bearer);
	g_test_add_func("/ofonotechnology/select-without-data", test_select_without_data);
	g_test_add_func("/ofonotechnology/select-with-data", test_select_with_data);

	return g_test_run();
}
