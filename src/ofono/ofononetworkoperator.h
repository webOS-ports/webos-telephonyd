/* @@@LICENSE
*
* Copyright (c) 2013 Simon Busch <morphis@gravedo.de>
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

#ifndef OFONO_NETWORK_OPERATOR_H_
#define OFONO_NETWORK_OPERATOR_H_

#include <glib.h>

struct ofono_network_operator;

enum ofono_network_operator_status {
	OFONO_NETWORK_OPERATOR_STATUS_UNKNOWN,
	OFONO_NETWORK_OPERATOR_STATUS_AVAILABLE,
	OFONO_NETWORK_OPERATOR_STATUS_CURRENT,
	OFONO_NETWORK_OPERATOR_STATUS_FORBIDDEN
};

struct ofono_network_operator* ofono_network_operator_create(const char *path);
void ofono_network_operator_free(struct ofono_network_operator *netop);

void ofono_network_operator_register(struct ofono_network_operator *netop, ofono_base_result_cb cb, void *user_data);

const char* ofono_network_operator_get_name(struct ofono_network_operator *netop);
enum ofono_network_operator_status ofono_network_operator_get_status(struct ofono_network_operator *netop);
const char* ofono_network_operator_get_mcc(struct ofono_network_operator *netop);
const char* ofono_network_operator_get_mnc(struct ofono_network_operator *netop);

#endif

// vim:ts=4:sw=4:noexpandtab
