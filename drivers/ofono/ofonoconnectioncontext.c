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

#include <string.h>
#include <errno.h>

#include <glib-object.h>
#include <gio/gio.h>

#include "utils.h"
#include "ofonoconnectioncontext.h"
#include "ofono-interface.h"

struct ofono_connection_context {
	gchar *path;
	OfonoInterfaceConnectionContext *remote;
	struct ofono_base *base;
	int ref_count;
	bool active;
	char *access_point_name;
	char *username;
	char *password;
	enum ofono_connection_context_type type;
	enum ofono_connection_context_protocol protocol;
	char *name;
	ofono_property_changed_cb prop_changed_cb;
	void *prop_changed_data;
};

static enum ofono_connection_context_type parse_ofono_connection_context_type(const char *type)
{
	if (g_str_equal(type, "internet"))
		return OFONO_CONNECTION_CONTEXT_TYPE_INTERNET;
	else if (g_str_equal(type, "mms"))
		return OFONO_CONNECTION_CONTEXT_TYPE_MMS;
	else if (g_str_equal(type, "wap"))
		return OFONO_CONNECTION_CONTEXT_TYPE_WAP;
	else if (g_str_equal(type, "ims"))
		return OFONO_CONNECTION_CONTEXT_TYPE_IMS;

	return OFONO_CONNECTION_CONTEXT_TYPE_UNKNOWN;
}

static enum ofono_connection_context_protocol parse_ofono_connection_context_protocol(const char *protocol)
{
	if (g_str_equal(protocol, "ip"))
		return OFONO_CONNECTION_CONTEXT_PROTOCOL_IP;
	else if (g_str_equal(protocol, "ipv6"))
		return OFONO_CONNECTION_CONTEXT_PROTOCOL_IPV6;
	else if (g_str_equal(protocol, "dual"))
		return OFONO_CONNECTION_CONTEXT_PROTOCOL_DUAL;

	return OFONO_CONNECTION_CONTEXT_PROTOCOL_UNKNOWN;
}

static void update_property(const gchar *name, GVariant *value, void *user_data)
{
	struct ofono_connection_context *ctx = user_data;
	const char *type_str;
	const char *protocol_str;

	g_message("[ConnectionContext:%s] property %s changed", ctx->path, name);

	if (g_str_equal(name, "Active"))
		ctx->active = g_variant_get_boolean(value);
	else if (g_str_equal(name, "AccessPointName")) {
		if (ctx->access_point_name)
			g_free(ctx->access_point_name);
		ctx->access_point_name = g_variant_dup_string(value, NULL);
	}
	else if (g_str_equal(name, "Username")) {
		if (ctx->username)
			g_free(ctx->username);
		ctx->username = g_variant_dup_string(value, NULL);
	}
	else if (g_str_equal(name, "Password")) {
		if (ctx->password)
			g_free(ctx->password);
		ctx->password = g_variant_dup_string(value, NULL);
	}
	else if (g_str_equal(name, "Type")) {
		type_str = g_variant_get_string(value, NULL);
		ctx->type = parse_ofono_connection_context_type(type_str);
	}
	else if (g_str_equal(name, "Protocol")) {
		protocol_str = g_variant_get_string(value, NULL);
		ctx->protocol = parse_ofono_connection_context_protocol(protocol_str);
	}
	else if (g_str_equal(name, "Name")) {
		if (ctx->name)
			g_free(ctx->name);
		ctx->name = g_variant_dup_string(value, NULL);
	}

	if (ctx->prop_changed_cb)
		ctx->prop_changed_cb(name, ctx->prop_changed_data);
}

struct ofono_base_funcs ctx_base_funcs = {
	.update_property = update_property,
	.set_property = ofono_interface_connection_context_call_set_property,
	.set_property_finish = ofono_interface_connection_context_call_set_property_finish,
	.get_properties = ofono_interface_connection_context_call_get_properties,
	.get_properties_finish = ofono_interface_connection_context_call_get_properties_finish
};

struct ofono_connection_context* ofono_connection_context_create(const gchar *path)
{
	struct ofono_connection_context *ctx;
	GError *error = NULL;

	ctx = g_try_new0(struct ofono_connection_context, 1);
	if (!ctx)
		return NULL;

	ctx->remote = ofono_interface_connection_context_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM,
							G_DBUS_PROXY_FLAGS_NONE, "org.ofono", path, NULL, &error);
	if (error) {
		g_error("Unable to initialize proxy for the org.ofono.ConnectionContext interface");
		g_error_free(error);
		g_free(ctx);
		return NULL;
	}

	ctx->path = g_strdup(path);
	ctx->base = ofono_base_create(&ctx_base_funcs, ctx->remote, ctx);

	return ctx;
}

void ofono_connection_context_ref(struct ofono_connection_context *ctx)
{
	if (!ctx)
		return;

	__sync_fetch_and_add(&ctx->ref_count, 1);
}

void ofono_connection_context_unref(struct ofono_connection_context *ctx)
{
	if (!ctx)
		return;

	if (__sync_sub_and_fetch(&ctx->ref_count, 1))
		return;

	ofono_connection_context_free(ctx);
}

void ofono_connection_context_free(struct ofono_connection_context *ctx)
{
	if (!ctx)
		return;

	if (ctx->remote)
		g_object_unref(ctx->remote);

	g_free(ctx);
}

void ofono_connection_context_register_prop_changed_cb(struct ofono_connection_context *ctx,
													   ofono_property_changed_cb cb, void *data)
{
	if (!ctx)
		return;

	ctx->prop_changed_cb = cb;
	ctx->prop_changed_data = data;
}

const char* ofono_connection_context_get_path(struct ofono_connection_context *ctx)
{
	if (!ctx)
		return NULL;

	return ctx->path;
}

bool ofono_connection_context_get_active(struct ofono_connection_context *ctx)
{
	if (!ctx)
		return false;

	return ctx->active;
}

const char* ofono_connection_context_get_access_point_name(struct ofono_connection_context *ctx)
{
	if (!ctx)
		return NULL;

	return ctx->access_point_name;
}

void ofono_connection_context_set_access_point_name(struct ofono_connection_context *ctx,
													ofono_base_result_cb cb, void *data)
{
}

enum ofono_connection_context_type ofono_connection_context_get_type(struct ofono_connection_context *ctx)
{
	if (!ctx)
		return OFONO_CONNECTION_CONTEXT_TYPE_UNKNOWN;

	return ctx->type;
}

void ofono_connection_context_set_type(struct ofono_connection_context *ctx,
					enum ofono_connection_context_type type, ofono_base_result_cb cb, void *data)
{
}

const char* ofono_connection_context_get_username(struct ofono_connection_context *ctx)
{
	if (!ctx)
		return NULL;

	return ctx->username;
}

void ofono_connection_context_set_username(struct ofono_connection_context *ctx,
										   const char *username, ofono_base_result_cb cb, void *data)
{
}

const char* ofono_connection_context_get_password(struct ofono_connection_context *ctx)
{
	if (!ctx)
		return NULL;

	return ctx->password;
}

void ofono_connection_context_set_password(struct ofono_connection_context *ctx,
										   const char *password, ofono_base_result_cb cb, void *data)
{
}

enum ofono_connection_context_protocol ofono_connection_context_get_protocol(
		struct ofono_connection_context *ctx)
{
	if (!ctx)
		return OFONO_CONNECTION_CONTEXT_PROTOCOL_UNKNOWN;

	return ctx->protocol;
}

void ofono_connection_context_set_protocol(struct ofono_connection_context *ctx,
										   enum ofono_connection_context_protocol protocol,
										   ofono_base_result_cb cb, void *data)
{
}

const char* ofono_connection_context_get_name(struct ofono_connection_context *ctx)
{
	if (!ctx)
		return NULL;

	return ctx->name;
}

void ofono_connection_context_set_name(struct ofono_connection_context *ctx, const char *name,
									   ofono_base_result_cb cb, void *data)
{
}

// vim:ts=4:sw=4:noexpandtab
