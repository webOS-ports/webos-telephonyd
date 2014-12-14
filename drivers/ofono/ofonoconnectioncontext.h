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

#ifndef OFONO_CONNECTION_CONTEXT_H_
#define OFONO_CONNECTION_CONTEXT_H_

#include "ofonobase.h"

enum ofono_connection_context_type {
	OFONO_CONNECTION_CONTEXT_TYPE_UNKNOWN = 0,
	OFONO_CONNECTION_CONTEXT_TYPE_INTERNET,
	OFONO_CONNECTION_CONTEXT_TYPE_MMS,
	OFONO_CONNECTION_CONTEXT_TYPE_WAP,
	OFONO_CONNECTION_CONTEXT_TYPE_IMS
};

enum ofono_connection_context_protocol {
	OFONO_CONNECTION_CONTEXT_PROTOCOL_UNKNOWN = 0,
	OFONO_CONNECTION_CONTEXT_PROTOCOL_IP,
	OFONO_CONNECTION_CONTEXT_PROTOCOL_IPV6,
	OFONO_CONNECTION_CONTEXT_PROTOCOL_DUAL
};

struct ofono_connection_context;

struct ofono_connection_context* ofono_connection_context_create(const gchar *path);
void ofono_connection_context_ref(struct ofono_connection_context *ctx);
void ofono_connection_context_unref(struct ofono_connection_context *ctx);
void ofono_connection_context_free(struct ofono_connection_context *ctx);

const char* ofono_connection_context_get_path(struct ofono_connection_context *ctx);

void ofono_connection_context_register_prop_changed_cb(struct ofono_connection_context *ctx,
													   ofono_property_changed_cb cb, void *data);

bool ofono_connection_context_get_active(struct ofono_connection_context *ctx);
const char* ofono_connection_context_get_access_point_name(struct ofono_connection_context *ctx);
void ofono_connection_context_set_access_point_name(struct ofono_connection_context *ctx,
													ofono_base_result_cb cb, void *data);
enum ofono_connection_context_type ofono_connection_context_get_type(struct ofono_connection_context *ctx);
void ofono_connection_context_set_type(struct ofono_connection_context *ctx,
					enum ofono_connection_context_type type, ofono_base_result_cb cb, void *data);
const char* ofono_connection_context_get_username(struct ofono_connection_context *ctx);
void ofono_connection_context_set_username(struct ofono_connection_context *ctx,
										   const char *username, ofono_base_result_cb cb, void *data);
const char* ofono_connection_context_get_password(struct ofono_connection_context *ctx);
void ofono_connection_context_set_password(struct ofono_connection_context *ctx,
										   const char *password, ofono_base_result_cb cb, void *data);
enum ofono_connection_context_protocol ofono_connection_context_get_protocol(
		struct ofono_connection_context *ctx);
void ofono_connection_context_set_protocol(struct ofono_connection_context *ctx,
										   enum ofono_connection_context_protocol protocol,
										   ofono_base_result_cb cb, void *data);
const char* ofono_connection_context_get_name(struct ofono_connection_context *ctx);
void ofono_connection_context_set_name(struct ofono_connection_context *ctx, const char *name,
									   ofono_base_result_cb cb, void *data);
const char* ofono_connection_context_get_address(struct ofono_connection_context *ctx);
const char* ofono_connection_context_get_netmask(struct ofono_connection_context *ctx);
const char* ofono_connection_context_get_gateway(struct ofono_connection_context *ctx);

#endif

// vim:ts=4:sw=4:noexpandtab
