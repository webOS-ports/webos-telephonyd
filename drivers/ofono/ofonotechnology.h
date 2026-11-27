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

#ifndef OFONO_TECHNOLOGY_H_
#define OFONO_TECHNOLOGY_H_

/*
 * The radio access technology, from ofono's names to the network type
 * com.palm.wan reports.
 *
 * ofono names what the modem is registered on in
 * NetworkRegistration.Technology and what carries packet data in
 * ConnectionManager.Bearer. They differ on 5G non-standalone (EN-DC): the
 * phone registers on an LTE cell with an NR leg, which ofono calls "nr",
 * while data still runs over the LTE anchor, a bearer of "lte". Android and
 * other systems show 5G there, so the registration wins when it is NR.
 */

/* The headers below use bool without including it */
#include <stdbool.h>

#include "ofonoconnectionmanager.h"
#include "ofononetworkregistration.h"
#include "wandriver.h"

enum ofono_network_technology parse_ofono_network_technology(const char *technology);
enum ofono_connection_bearer parse_ofono_connection_bearer(const char *bearer);

/* ofono's name for a technology, "" for one it has none for */
const char* ofono_network_technology_to_string(enum ofono_network_technology technology);

enum wan_network_type convert_ofono_connection_bearer_to_wan_network_type(enum ofono_connection_bearer bearer);
enum wan_network_type convert_ofono_network_technology_to_wan_network_type(enum ofono_network_technology technology);

/* The network type for a data bearer and the registered technology */
enum wan_network_type select_wan_network_type(enum ofono_connection_bearer bearer,
											  enum ofono_network_technology technology);

#endif
