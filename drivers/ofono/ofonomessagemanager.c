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
#include "timeutils.h"
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
	/* ofono's UseDeliveryReports, once known */
	bool delivery_reports_known;
	bool delivery_reports;
};

/* A message waiting for UseDeliveryReports to be set before it goes out */
struct pending_send {
	OfonoInterfaceMessageManager *remote;
	gchar *to;
	gchar *text;
	bool delivery_report;
	ofono_message_manager_send_message_cb cb;
	void *data;
};

static void update_property(const gchar *name, GVariant *value, void *user_data)
{
	struct ofono_message_manager *mm = user_data;

	g_message("[MessageManager:%s] property %s changed", mm->path, name);

	if (g_str_equal(name, "UseDeliveryReports") && g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN)) {
		mm->delivery_reports = g_variant_get_boolean(value);
		mm->delivery_reports_known = true;
	}

	if (mm->prop_changed_cb)
		mm->prop_changed_cb(name, mm->prop_changed_data);
}

struct ofono_base_funcs mm_base_funcs = {
	.update_property = update_property,
	.set_property = (ofono_base_set_property_fn) ofono_interface_message_manager_call_set_property,
	.set_property_finish = (ofono_base_set_property_finish_fn) ofono_interface_message_manager_call_set_property_finish,
	.get_properties = (ofono_base_get_properties_fn) ofono_interface_message_manager_call_get_properties,
	.get_properties_finish = (ofono_base_get_properties_finish_fn) ofono_interface_message_manager_call_get_properties_finish
};

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
		if (g_strcmp0(property_name, "Sender") == 0) {
			ofono_message_set_sender(message, g_variant_get_string(property_value, NULL));
		}
		else if (g_strcmp0(property_name, "SentTime") == 0) {
			time_t sent_time = telephony_decode_iso8601_time(g_variant_get_string(property_value, NULL));
			ofono_message_set_sent_time(message, sent_time);
		}
		else if (g_strcmp0(property_name, "LocalSentTime") == 0) {
			time_t local_sent_time = telephony_decode_iso8601_time(g_variant_get_string(property_value, NULL));
			ofono_message_set_local_sent_time(message, local_sent_time);
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
		g_critical("Unable to initialize proxy for the org.ofono.MessageManager interface");
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

	/* In-flight async replies still hold a proxy ref, so the signal
	 * handler does not die with our unref below and must go explicitly. */
	if (manager->remote)
		g_signal_handlers_disconnect_by_data(manager->remote, manager);

	if (manager->base)
		ofono_base_free(manager->base);

	if (manager->remote)
		g_object_unref(manager->remote);

	g_free(manager->path);
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
	ofono_message_manager_send_message_cb cb = cbd->cb;
	struct ofono_error oerr;
	GError *error = NULL;
	gboolean success = FALSE;
	gchar *path = NULL;

	/* Finish against the proxy GIO hands back; the manager can already be
	 * gone when the reply lands. */
	success = ofono_interface_message_manager_call_send_message_finish(
		OFONO_INTERFACE_MESSAGE_MANAGER(source), &path, res, &error);
	if (!success) {
		oerr.type = OFONO_ERROR_TYPE_FAILED;
		oerr.message = error->message;
		cb(&oerr, NULL, cbd->data);
		g_error_free(error);
		goto cleanup;
	}

	cb(NULL, path, cbd->data);
	g_free(path);

cleanup:
	g_free(cbd);
}


static void send_now(OfonoInterfaceMessageManager *remote, const char *path, const char *to, const char *text,
					 ofono_message_manager_send_message_cb cb, void *data)
{
	struct cb_data *cbd;

	g_message("[MessageManager:%s] sending SMS to '%s'", path, to);

	cbd = cb_data_new(cb, data);
	ofono_interface_message_manager_call_send_message(remote, to, text, NULL, send_message_cb, cbd);
}

static void pending_send_free(struct pending_send *ps)
{
	g_object_unref(ps->remote);
	g_free(ps->to);
	g_free(ps->text);
	g_free(ps);
}

static void set_delivery_reports_cb(GObject *source, GAsyncResult *res, gpointer user_data)
{
	struct pending_send *ps = user_data;
	OfonoInterfaceMessageManager *remote = OFONO_INTERFACE_MESSAGE_MANAGER(source);
	GError *error = NULL;
	gchar *path;

	path = g_strdup(g_dbus_proxy_get_object_path(G_DBUS_PROXY(remote)));

	/* Against the proxy GIO hands back: the manager can be gone by now */
	if (!ofono_interface_message_manager_call_set_property_finish(remote, res, &error)) {
		/* the message still goes out, without (or with) a report */
		g_warning("[MessageManager:%s] could not set UseDeliveryReports: %s", path, error->message);
		g_error_free(error);
	}

	send_now(remote, path, ps->to, ps->text, ps->cb, ps->data);

	g_free(path);
	pending_send_free(ps);
}

void ofono_message_manager_send_message(struct ofono_message_manager *manager, const char *to, const char *text,
										bool delivery_report, ofono_message_manager_send_message_cb cb, void *data)
{
	struct pending_send *ps;
	struct ofono_error error;

	if (!manager) {
		error.type = OFONO_ERROR_TYPE_INVALID_ARGUMENTS;
		error.message = "No message manager available";
		cb(&error, NULL, data);
		return;
	}

	/* ofono asks for a status report per its setting at the time a message
	 * goes out, so the setting follows each message. Messages go out one
	 * at a time. */
	if (manager->delivery_reports_known && manager->delivery_reports == delivery_report) {
		send_now(manager->remote, manager->path, to, text, cb, data);
		return;
	}

	ps = g_new0(struct pending_send, 1);
	ps->remote = g_object_ref(manager->remote);
	ps->to = g_strdup(to);
	ps->text = g_strdup(text);
	ps->delivery_report = delivery_report;
	ps->cb = cb;
	ps->data = data;

	/* The cache follows ofono's PropertyChanged, so a setting that failed
	 * is tried again with the next message */
	ofono_interface_message_manager_call_set_property(manager->remote, "UseDeliveryReports",
		g_variant_new_variant(g_variant_new_boolean(delivery_report)), NULL, set_delivery_reports_cb, ps);
}
