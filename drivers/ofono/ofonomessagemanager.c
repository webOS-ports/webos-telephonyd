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
#include "ofonomessagemanager.h"
#include "ofonomessage.h"
#include "ofono-interface.h"

struct ofono_message_manager {
	gchar *path;
	OfonoInterfaceMessageManager *remote;
	struct ofono_base *base;
	ofono_property_changed_cb prop_changed_cb;
	void *prop_changed_data;
	ofono_message_manager_incoming_message_cb incoming_message_cb;
	void *incoming_message_data;
};

static void update_property(const gchar *name, GVariant *value, void *user_data)
{
	struct ofono_message_manager *mm = user_data;

	g_message("[MessageManager:%s] property %s changed", mm->path, name);

	if (mm->prop_changed_cb)
		mm->prop_changed_cb(name, mm->prop_changed_data);
}

struct ofono_base_funcs mm_base_funcs = {
	.update_property = update_property,
	.set_property = ofono_interface_message_manager_call_set_property,
	.set_property_finish = ofono_interface_message_manager_call_set_property_finish,
	.get_properties = ofono_interface_message_manager_call_get_properties,
	.get_properties_finish = ofono_interface_message_manager_call_get_properties_finish
};

static time_t decode_iso8601_time(const char *str)
{
	struct tm t;
	memset(&t, 0, sizeof(struct tm));

	sscanf(str, "%4d%2d%2dT%2d%2d%2dZ",
		   &t.tm_year, &t.tm_mon, &t.tm_mday,
		   &t.tm_hour, &t.tm_min, &t.tm_sec);

	t.tm_year = abs(t.tm_year - 1900);

	if (t.tm_mon > 0)
		t.tm_mon -= 1;
	if (t.tm_hour > 0)
		t.tm_hour -= 1;
	if (t.tm_min > 0)
		t.tm_min -= 1;
	if (t.tm_sec > 0)
		t.tm_sec -= 1;

	return mktime(&t);
}

static void incoming_message_cb(OfonoInterfaceMessageManager *source, gchar *text, GVariant *properties, gpointer user_data)
{
	struct ofono_message_manager *manager = user_data;

	g_message("[MessageManager:%s] new incoming message", manager->path);

	if (!manager->incoming_message_cb)
		return;

	gchar *property_name = NULL;
	GVariant *property_value = NULL;
	GVariantIter iter;

	struct ofono_message *message;

	message = ofono_message_create();
	if (!message)
		return;

	ofono_message_set_type(message, OFONO_MESSAGE_TYPE_TEXT);

	g_variant_iter_init(&iter, properties);
	while (g_variant_iter_loop(&iter, "{sv}", &property_name, &property_value)) {
		if (g_strcmp0(property_name, "Sender") != 0)
			ofono_message_set_sender(message, g_variant_get_string(property_value, NULL));
		else if (g_strcmp0(property_name, "SentTime") != 0 || g_strcmp0(property_name, "LocalSentTime") != 0) {
			time_t sent_time = decode_iso8601_time(g_variant_get_string(property_value, NULL));
			ofono_message_set_sent_time(message, sent_time);
		}
	}

	ofono_message_set_text(message, text);

	manager->incoming_message_cb(message, manager->incoming_message_data);
}

struct ofono_message_manager* ofono_message_manager_create(const gchar *path)
{
	struct ofono_message_manager *mm;
	GError *error = NULL;

	mm = g_try_new0(struct ofono_message_manager, 1);
	if (!mm)
		return NULL;

	mm->remote = ofono_interface_message_manager_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM,
							G_DBUS_PROXY_FLAGS_NONE, "org.ofono", path, NULL, &error);
	if (error) {
		g_error("Unable to initialize proxy for the org.ofono.MessageManager interface");
		g_error_free(error);
		g_free(mm);
		return NULL;
	}

	mm->path = g_strdup(path);
	mm->base = ofono_base_create(&mm_base_funcs, mm->remote, mm);

	g_signal_connect(G_OBJECT(mm->remote), "incoming-message",
		G_CALLBACK(incoming_message_cb), mm);

	return mm;
}

void ofono_message_manager_free(struct ofono_message_manager *manager)
{
	if (!manager)
		return;

	if (manager->base)
		ofono_base_free(manager->base);

	if (manager->remote)
		g_object_unref(manager->remote);

	g_free(manager);
}

void ofono_message_manager_set_prop_changed_callback(struct ofono_message_manager *manager, ofono_property_changed_cb cb, void *data)
{
	if (!manager)
		return;

	manager->prop_changed_cb = cb;
	manager->prop_changed_data = data;
}

void ofono_message_manager_set_incoming_message_callback(struct ofono_message_manager *manager,
														 ofono_message_manager_incoming_message_cb cb,
														 gpointer user_data)
{
	if (!manager)
		return;

	manager->incoming_message_cb = cb;
	manager->incoming_message_data = user_data;
}

static void send_message_cb(GObject *source, GAsyncResult *res, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_message_manager *manager = cbd->user;
	ofono_message_manager_send_message_cb cb = cbd->cb;
	struct ofono_error oerr;
	GError *error = NULL;
	gboolean success = FALSE;
	gchar *path = NULL;

	success = ofono_interface_message_manager_call_send_message_finish(manager->remote, &path, res, &error);
	if (!success) {
		oerr.type = OFONO_ERROR_TYPE_FAILED;
		oerr.message = error->message;
		cb(&oerr, NULL, cbd->data);
		g_error_free(error);
		goto cleanup;
	}

	cb(NULL, path, cbd->data);

cleanup:
	g_free(cbd);
}


void ofono_message_manager_send_message(struct ofono_message_manager *manager, const char *to, const char *text,
										ofono_message_manager_send_message_cb cb, void *data)
{
	struct cb_data *cbd;
	struct ofono_error error;

	g_message("[MessageManager:%s] sending SMS to '%s'' with text '%s'", manager->path, to, text);

	if (!manager) {
		error.type = OFONO_ERROR_TYPE_INVALID_ARGUMENTS;
		error.message = NULL;
		cb(&error, NULL, data);
		return;
	}

	cbd = cb_data_new(cb, data);
	cbd->user = manager;

	ofono_interface_message_manager_call_send_message(manager->remote, to, text, NULL, send_message_cb, cbd);
}
