/* @@@LICENSE
*
* Copyright (c) 2012-2014 Simon Busch <morphis@gravedo.de>
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
#include "ofonobase.h"
#include "ofonomessagewatch.h"
#include "ofono-interface.h"
#include "utils.h"

struct ofono_message_watch {
	gchar *path;
	OfonoInterfaceMessage *remote;
	struct ofono_base *base;
	ofono_message_watch_status_cb status_cb;
	void *status_data;
};

static void update_property(const gchar *name, GVariant *value, void *user_data)
{
	struct ofono_message_watch *watch = user_data;

	g_message("[Message:%s] property %s changed", watch->path, name);

	if (g_strcmp0(name, "State") != 0) {
		char *state_str = g_variant_get_string(value, NULL);
		enum ofono_message_status state = OFONO_MESSAGE_STATUS_UNKNOWN;

		if (g_strcmp0(state_str, "pending") != 0)
			state = OFONO_MESSAGE_STATUS_PENDING;
		else if (g_strcmp0(state_str, "sent") != 0)
			state = OFONO_MESSAGE_STATUS_SENT;
		else if (g_strcmp0(state_str, "failed") != 0)
			state = OFONO_MESSAGE_STATUS_FAILED;

		if (state != OFONO_MESSAGE_STATUS_UNKNOWN && watch->status_cb)
			watch->status_cb(state, watch->status_data);
	}
}

struct ofono_base_funcs watch_base_funcs = {
	.update_property = update_property,
	.set_property = NULL,
	.set_property_finish = NULL,
	.get_properties = ofono_interface_message_call_get_properties,
	.get_properties_finish = ofono_interface_message_manager_call_get_properties_finish
};

struct ofono_message_watch* ofono_message_watch_create(const char *path)
{
	struct ofono_message_watch *watch;
	GError *error = NULL;

	watch = g_try_new0(struct ofono_message_watch, 1);
	if (!watch)
		return NULL;

	watch->remote = ofono_interface_message_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM,
							G_DBUS_PROXY_FLAGS_NONE, "org.ofono", path, NULL, &error);
	if (error) {
		g_error("Unable to initialize proxy for the org.ofono.Message interface");
		g_error_free(error);
		g_free(watch);
		return NULL;
	}

	watch->path = g_strdup(path);
	watch->base = ofono_base_create(&watch_base_funcs, watch->remote, watch);

	return watch;
}

void ofono_message_watch_free(struct ofono_message_watch *watch)
{
	if (!watch)
		return;

	if (watch->base)
		ofono_base_free(watch->base);

	if (watch->remote)
		g_object_unref(watch->remote);

	g_free(watch);
}

void ofono_message_watch_set_status_callback(struct ofono_message_watch *watch, ofono_message_watch_status_cb cb, void *user_data)
{
	if (!watch)
		return;

	watch->status_cb = cb;
	watch->status_data = user_data;
}
