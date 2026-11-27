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

#include "ofonotechnology.h"

enum ofono_network_technology parse_ofono_network_technology(const char *technology)
{
	if (!technology)
		return OFONO_NETWORK_TECHNOLOGY_UNKNOWN;

	if (g_str_equal(technology, "gsm"))
		return OFONO_NETWORK_TECHNOLOGY_GSM;
	else if (g_str_equal(technology, "edge"))
		return OFONO_NETWORK_TECHNOLOGY_EDGE;
	else if (g_str_equal(technology, "umts"))
		return OFONO_NETWORK_TECHNOLOGY_UMTS;
	else if (g_str_equal(technology, "hspa"))
		return OFONO_NETWORK_TECHNOLOGY_HSPA;
	else if (g_str_equal(technology, "lte"))
		return OFONO_NETWORK_TECHNOLOGY_LTE;
	else if (g_str_equal(technology, "nr"))
		return OFONO_NETWORK_TECHNOLOGY_NR;

	return OFONO_NETWORK_TECHNOLOGY_UNKNOWN;
}

const char* ofono_network_technology_to_string(enum ofono_network_technology technology)
{
	switch (technology) {
	case OFONO_NETWORK_TECHNOLOGY_GSM:
		return "gsm";
	case OFONO_NETWORK_TECHNOLOGY_EDGE:
		return "edge";
	case OFONO_NETWORK_TECHNOLOGY_UMTS:
		return "umts";
	case OFONO_NETWORK_TECHNOLOGY_HSPA:
		return "hspa";
	case OFONO_NETWORK_TECHNOLOGY_LTE:
		return "lte";
	case OFONO_NETWORK_TECHNOLOGY_NR:
		return "nr";
	default:
		return "";
	}
}

enum ofono_connection_bearer parse_ofono_connection_bearer(const char *bearer)
{
	if (!bearer)
		return OFONO_CONNECTION_BEARER_UNKNOWN;

	if (g_str_equal(bearer, "gprs"))
		return OFONO_CONNECTION_BEARER_GPRS;
	else if (g_str_equal(bearer, "edge"))
		return OFONO_CONNECTION_BEARER_EDGE;
	else if (g_str_equal(bearer, "umts"))
		return OFONO_CONNECTION_BEARER_UMTS;
	else if (g_str_equal(bearer, "hsupa"))
		return OFONO_CONNECTION_BEARER_HSUPA;
	else if (g_str_equal(bearer, "hsdpa"))
		return OFONO_CONNECTION_BEARER_HSDPA;
	else if (g_str_equal(bearer, "hspa"))
		return OFONO_CONNECTION_BEARER_HSPA;
	else if (g_str_equal(bearer, "lte"))
		return OFONO_CONNECTION_BEARER_LTE;

	return OFONO_CONNECTION_BEARER_UNKNOWN;
}

enum wan_network_type convert_ofono_connection_bearer_to_wan_network_type(enum ofono_connection_bearer bearer)
{
	switch (bearer) {
	case OFONO_CONNECTION_BEARER_GPRS:
		return WAN_NETWORK_TYPE_GPRS;
	case OFONO_CONNECTION_BEARER_EDGE:
		return WAN_NETWORK_TYPE_EDGE;
	case OFONO_CONNECTION_BEARER_HSUPA:
	case OFONO_CONNECTION_BEARER_HSDPA:
	case OFONO_CONNECTION_BEARER_HSPA:
		return WAN_NETWORK_TYPE_HSDPA;
	case OFONO_CONNECTION_BEARER_UMTS:
		return WAN_NETWORK_TYPE_UMTS;
	case OFONO_CONNECTION_BEARER_LTE:
		return WAN_NETWORK_TYPE_LTE;
	default:
		break;
	}

	return WAN_NETWORK_TYPE_NONE;
}

enum wan_network_type convert_ofono_network_technology_to_wan_network_type(enum ofono_network_technology technology)
{
	switch (technology) {
	case OFONO_NETWORK_TECHNOLOGY_GSM:
		return WAN_NETWORK_TYPE_GPRS;
	case OFONO_NETWORK_TECHNOLOGY_EDGE:
		return WAN_NETWORK_TYPE_EDGE;
	case OFONO_NETWORK_TECHNOLOGY_UMTS:
		return WAN_NETWORK_TYPE_UMTS;
	case OFONO_NETWORK_TECHNOLOGY_HSPA:
		return WAN_NETWORK_TYPE_HSDPA;
	case OFONO_NETWORK_TECHNOLOGY_LTE:
		return WAN_NETWORK_TYPE_LTE;
	case OFONO_NETWORK_TECHNOLOGY_NR:
		return WAN_NETWORK_TYPE_NR;
	default:
		break;
	}

	return WAN_NETWORK_TYPE_NONE;
}

enum wan_network_type select_wan_network_type(enum ofono_connection_bearer bearer,
											  enum ofono_network_technology technology)
{
	/* LTE data on a cell with an NR leg is 5G non-standalone */
	if (technology == OFONO_NETWORK_TECHNOLOGY_NR &&
			(bearer == OFONO_CONNECTION_BEARER_UNKNOWN || bearer == OFONO_CONNECTION_BEARER_LTE))
		return WAN_NETWORK_TYPE_NR;

	if (bearer == OFONO_CONNECTION_BEARER_UNKNOWN)
		return convert_ofono_network_technology_to_wan_network_type(technology);

	return convert_ofono_connection_bearer_to_wan_network_type(bearer);
}
