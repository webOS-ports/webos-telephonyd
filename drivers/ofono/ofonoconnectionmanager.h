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

#ifndef OFONO_CONNECTION_MANAGER_H_
#define OFONO_CONNECTION_MANAGER_H_

#include "ofonobase.h"

enum ofono_connection_bearer {
	OFONO_CONNECTION_BEARER_UNKNOWN = 0,
	OFONO_CONNECTION_BEARER_GPRS,
	OFONO_CONNECTION_BEARER_EDGE,
	OFONO_CONNECTION_BEARER_UMTS,
	OFONO_CONNECTION_BEARER_HSUPA,
	OFONO_CONNECTION_BEARER_HSDPA,
	OFONO_CONNECTION_BEARER_HSPA,
	OFONO_CONNECTION_BEARER_LTE
};

typedef void (*ofono_connection_manager_get_contexts_cb)(const struct ofono_error *error, GSList *contexts, void *data);

struct ofono_connection_manager;

struct ofono_connection_manager* ofono_connection_manager_create(const gchar *path);
void ofono_connection_manager_ref(struct ofono_connection_manager *cm);
void ofono_connection_manager_unref(struct ofono_connection_manager *cm);
void ofono_connection_manager_free(struct ofono_connection_manager *cm);

void ofono_connection_manager_register_prop_changed_cb(struct ofono_connection_manager *cm,
													   ofono_property_changed_cb cb, void *data);
void ofono_connection_manager_register_contexts_changed_cb(struct ofono_connection_manager *cm,
												  ofono_base_cb cb, void *data);

void ofono_connection_manager_deactivate_all(struct ofono_connection_manager *cm, ofono_base_result_cb cb, void *data);
void ofono_connection_manager_get_contexts(struct ofono_connection_manager *cm, ofono_connection_manager_get_contexts_cb cb, void *data);

bool ofono_connection_manager_get_attached(struct ofono_connection_manager *cm);
enum ofono_connection_bearer ofono_connection_manager_get_bearer(struct ofono_connection_manager *cm);
bool ofono_connection_manager_get_powered(struct ofono_connection_manager *cm);
bool ofono_connection_manager_get_suspended(struct ofono_connection_manager *cm);
bool ofono_connection_manager_get_roaming_allowed(struct ofono_connection_manager *cm);

#endif

// vim:ts=4:sw=4:noexpandtab
