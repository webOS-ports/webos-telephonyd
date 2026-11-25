/* @@@LICENSE
*
* Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
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

#include <glib.h>
#include <gio/gio.h>

#include "ofonosmshistory.h"
#include "ofono-interface.h"

struct ofono_sms_history {
	gchar *path;
	OfonoInterfaceSmsHistory *remote;
	ofono_sms_history_report_cb cb;
	void *user_data;
};

static void status_report_cb(OfonoInterfaceSmsHistory *source, const gchar *message, GVariant *info,
							 gpointer user_data)
{
	struct ofono_sms_history *history = user_data;
	gboolean delivered;

	if (!g_variant_lookup(info, "Delivered", "b", &delivered)) {
		g_warning("[SmsHistory:%s] status report for %s without Delivered", history->path, message);
		return;
	}

	g_message("[SmsHistory:%s] %s %s", history->path, message, delivered ? "delivered" : "not delivered");

	if (history->cb)
		history->cb(message, delivered, history->user_data);
}

struct ofono_sms_history* ofono_sms_history_create(const char *modem_path,
												   ofono_sms_history_report_cb cb, void *user_data)
{
	struct ofono_sms_history *history;
	GError *error = NULL;

	if (!modem_path)
		return NULL;

	history = g_new0(struct ofono_sms_history, 1);

	/* Only the signal is used: there are no properties to load, and a
	 * proxy listens whether or not ofono has the interface. */
	history->remote = ofono_interface_sms_history_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM,
							G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES | G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
							"org.ofono", modem_path, NULL, &error);
	if (error) {
		g_warning("Unable to initialize proxy for the org.ofono.SmsHistory interface: %s", error->message);
		g_error_free(error);
		g_free(history);
		return NULL;
	}

	history->path = g_strdup(modem_path);
	history->cb = cb;
	history->user_data = user_data;

	g_signal_connect(G_OBJECT(history->remote), "status-report", G_CALLBACK(status_report_cb), history);

	return history;
}

void ofono_sms_history_free(struct ofono_sms_history *history)
{
	if (!history)
		return;

	if (history->remote) {
		g_signal_handlers_disconnect_by_data(history->remote, history);
		g_object_unref(history->remote);
	}

	g_free(history->path);
	g_free(history);
}

// vim:ts=4:sw=4:noexpandtab
