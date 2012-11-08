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
#include "ofonomanager.h"

struct ofono_manager {
	GDBusProxy *proxy;
};

struct ofono_manager* ofono_manager_create(void)
{
	struct ofono_manager *manager;
	GDBusProxyFlags flags;
	GError *error;

	manager = g_try_new0(struct ofono_manager, 1);
	if (!manager)
		return NULL;

	flags = G_DBUS_PROXY_FLAGS_NONE | G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START;

	manager->proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, flags, NULL, "org.ofono",
		"/", "org.ofono.Manager", NULL, &error);
	if (error) {
		g_error("Unable to initialize proxy for the org.ofono.Manager interface");
		g_error_free(error);
		g_free(manager);
		return NULL;
	}

	return manager;
}

void ofono_manager_free(struct ofono_manager *manager)
{
	if (manager->proxy)
		g_object_unref(manager->proxy);

	g_free(manager);
}

static void _get_modems_cb(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_manager_get_modems_cb cb = cbd->cb;
	GVariantIter iter;
	GList *modems;
	GError *error;
	GVariant *result;
	gchar *path;
	GVariant *properties;

	result = g_dbus_proxy_call_finish(G_DBUS_PROXY(source_object), res, &error);
	if (error) {
		g_error("Unable to retrieve list of available modems from ofono: %s", error->message);
		g_error_free(error);
		cb(false, NULL, cbd->data);
		return;
	}

	modems = g_list_alloc();

	g_variant_get(result, "a(oa{sv})", &iter);
	while (g_variant_iter_loop(&iter, "{oa{sv}}", &path, &properties)) {
		modems = g_list_append(modems, path);
	}

	cb(true, modems, cbd->data);

	g_list_free_full(modems, g_free);

	g_free(cbd);
}

int ofono_manager_get_modems(struct ofono_manager *manager,
									ofono_manager_get_modems_cb cb, void *user_data)
{
	struct cb_data *cbd;

	g_assert(manager != NULL);

	cbd = cb_data_new(cb, user_data);

	g_dbus_proxy_call(manager->proxy, "SetProperty", NULL, G_DBUS_CALL_FLAGS_NONE,
		-1, NULL, _get_modems_cb, cbd);

	return 0;
}

// vim:ts=4:sw=4:noexpandtab
