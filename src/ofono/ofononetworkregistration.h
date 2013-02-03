/* @@@LICENSE
*
* Copyright (c) 2012 Simon Busch <morphis@gravedo.de>
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

#ifndef OFONO_NETWORK_REGISTRATION_H_
#define OFONO_NETWORK_REGISTRATION_H_

#include <glib.h>

enum ofono_network_registration_mode {
	OFONO_NETWORK_REGISTRATION_MODE_AUTO = 0,
	OFONO_NETWORK_REGISTRATION_MODE_AUTO_ONLY,
	OFONO_NETWORK_REGISTRATION_MODE_MANUAL,
	OFONO_NETWORK_REGISTRATION_MODE_UNKNOWN
};

enum ofono_network_status {
	OFONO_NETWORK_REGISTRATION_STATUS_UNREGISTERED = 0,
	OFONO_NETWORK_REGISTRATION_STATUS_REGISTERED,
	OFONO_NETWORK_REGISTRATION_STATUS_SEARCHING,
	OFONO_NETWORK_REGISTRATION_STATUS_DENIED,
	OFONO_NETWORK_REGISTRATION_STATUS_UNKNOWN,
	OFONO_NETWORK_REGISTRATION_STATUS_ROAMING
};

enum ofono_network_technology {
	OFONO_NETWORK_TECHNOLOGOY_GSM = 0,
	OFONO_NETWORK_TECHNOLOGOY_EDGE,
	OFONO_NETWORK_TECHNOLOGOY_UMTS,
	OFONO_NETWORK_TECHNOLOGOY_HSPA,
	OFONO_NETWORK_TECHNOLOGOY_LTE,
	OFONO_NETWORK_TECHNOLOGOY_UNKNOWN,
	OFONO_NETWORK_TECHNOLOGOY_MAX
};

struct ofono_network_registration;
struct ofono_network_operator;

typedef int (*ofono_network_registration_operator_list_cb)(struct ofono_error *err, GList *operators, void *data);

struct ofono_network_registration* ofono_network_registration_create(const gchar *path);
void ofono_network_registration_ref(struct ofono_network_registration *sim);
void ofono_network_registration_unref(struct ofono_network_registration *sim);
void ofono_network_registration_free(struct ofono_network_registration *sim);

void ofono_network_registration_register_prop_changed_handler(struct ofono_network_registration *netreg,
															ofono_property_changed_cb cb, void *data);

void ofono_network_registration_register(struct ofono_network_registration *netreg,
										ofono_base_result_cb cb, void *data);
void ofono_network_registration_scan(struct ofono_network_registration *netreg,
							ofono_network_registration_operator_list_cb cb, GCancellable *cancellable,
							void *data);
void ofono_network_registration_get_operators(struct ofono_network_registration *netreg,
									ofono_network_registration_operator_list_cb cb, void *data);

enum ofono_network_registration_mode ofono_network_registration_get_mode(struct ofono_network_registration *netreg);
enum ofono_network_status ofono_network_registration_get_status(struct ofono_network_registration *netreg);
unsigned int ofono_network_registration_get_strength(struct ofono_network_registration *netreg);
unsigned int ofono_network_registration_get_location_area_code(struct ofono_network_registration *netreg);
unsigned int ofono_network_registration_get_cell_id(struct ofono_network_registration *netreg);
const gchar* ofono_network_registration_get_mcc(struct ofono_network_registration *netreg);
const gchar* ofono_network_registration_get_mnc(struct ofono_network_registration *netreg);
enum ofono_network_technology ofono_network_registration_get_technology(struct ofono_network_registration *netreg);
const gchar* ofono_network_registration_get_operator_name(struct ofono_network_registration *netreg);
const gchar* ofono_network_registration_get_base_station(struct ofono_network_registration *netreg);

#endif

// vim:ts=4:sw=4:noexpandtab
