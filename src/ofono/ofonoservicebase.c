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

#include "ofonoservicebase.h"
#include "utils.h"

#define DBUS_MAX_PATH_LENGTH					255
#define DBUS_MAX_INTERFACE_NAME_LENGTH			255

struct ofono_service_base {
	GDBusProxy *proxy;
	char path[DBUS_MAX_PATH_LENGTH];
	char interface_name[DBUS_MAX_INTERFACE_NAME_LENGTH];
	GHashTable *properties;
};

static void _signal_cb(GDBusProxy *proxy, gchar *sender_name, gchar *signal_name,
	GVariant *parameters, gpointer user_data)
{
	struct ofono_service_base *service;

	service = user_data;

	g_message("Received signal %s for interface %s for path %s", signal_name,
		service->interface_name, service->path);
}

static gboolean _properties_remove(gpointer key, gpointer value, gpointer user_data)
{
	g_free(key);
	g_variant_unref((GVariant*) value);
	return TRUE;
}

static void _get_properties_cb(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
	struct ofono_service_base *service;
	GError *error = NULL;
	GVariant *result;
	GVariantIter iter;
	gchar *key;
	GVariant *value;

	service = user_data;

	result = g_dbus_proxy_call_finish(G_DBUS_PROXY(source_object), res, &error);
	if (error) {
		g_error("Unable to set property: %s", error->message);
		/* FIXME propagate error to user through callback */
		g_error_free(error);
		return;
	}

	if (!g_variant_is_container(result))
		return;

	g_hash_table_foreach_remove(service->properties, _properties_remove, NULL);

	g_variant_iter_init(&iter, result);
	while (g_variant_iter_next(&iter, "a{sv}", &key, &value)) {
		g_debug("Property: key=%s, type=%s", key, g_variant_get_type_string(value));
		g_hash_table_insert(service->properties, key, value);
	}

	g_variant_unref(result);
}

struct ofono_service_base* ofono_service_base_create(const char *path, const char *interface_name)
{
	struct ofono_service_base *service;
	GDBusProxyFlags flags;

	service = g_try_new0(struct ofono_service_base, 1);
	if (service == NULL)
		return NULL;

	strncpy(service->path, path, DBUS_MAX_PATH_LENGTH);
	strncpy(service->interface_name, interface_name, DBUS_MAX_INTERFACE_NAME_LENGTH);

	service->properties = g_hash_table_new(g_str_hash, g_str_equal);

	flags = G_DBUS_PROXY_FLAGS_NONE | G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START;

	/* FIXME handle possible errors */
	/* FIXME don't hardcode ofono bus name */
	service->proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, flags, NULL, "org.ofono",
		path, interface_name, NULL, NULL);

	g_signal_connect(service->proxy, "g-signal", G_CALLBACK(_signal_cb), service);

	/* FIXME don't hardcode method name */
	g_dbus_proxy_call(service->proxy, "GetProperties", NULL, G_DBUS_CALL_FLAGS_NONE,
		-1, NULL, _get_properties_cb, service);

	return service;
}

void ofono_service_base_free(struct ofono_service_base *service)
{
	g_assert(service != NULL);

	g_hash_table_destroy(service->properties);
	g_object_unref(service->proxy);
	g_free(service);
}

static void _set_property_cb(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
	GVariant *result;
	GError *error = NULL;
	struct cb_data *cbd = user_data;
	ofono_service_base_result_cb cb = cbd->cb;

	result = g_dbus_proxy_call_finish(G_DBUS_PROXY(source_object), res, &error);
	if (error) {
		g_error("Unable to set property: %s", error->message);
		g_error_free(error);
		cb(FALSE, cbd->data);
		return;
	}

	g_variant_unref(result);
	cb(TRUE, cbd->data);
}

int ofono_service_base_set_property(struct ofono_service_base *service, const char *name, GVariant *value,
									ofono_service_base_result_cb cb, void *user_data)
{
	GVariant *parameters;
	struct cb_data *cbd;

	g_assert(service != NULL);

	/* All existing properties are listed in our table so if a not existing one is
	 * supplied it is invalid for ofono as well and we abort here */
	if (!g_hash_table_contains(service->properties, name))
		return -EINVAL;

	cbd = cb_data_new(cb, user_data);

	parameters = g_variant_new("(sv)", name, value);
	/* FIXME take callback from user to return result */
	g_dbus_proxy_call(service->proxy, "SetProperty", parameters, G_DBUS_CALL_FLAGS_NONE,
		-1, NULL, _set_property_cb, cbd);

	return 0;
}

GVariant* ofono_service_base_get_property(struct ofono_service_base *service, const char *name)
{
	GVariant *result;

	g_assert(service != NULL);

	if (!g_hash_table_contains(service->properties, name))
		return NULL;

	result = g_hash_table_lookup(service->properties, name);

	return result;
}

// vim:ts=4:sw=4:noexpandtab
