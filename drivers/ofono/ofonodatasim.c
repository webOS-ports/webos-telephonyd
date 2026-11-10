/* @@@LICENSE
*
* Copyright (c) 2026 LuneOS project
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

#include "ofonodatasim.h"

#define OFONO_SERVICE "org.ofono"
#define SLOT_MANAGER_PATH "/"
#define SLOT_MANAGER_INTERFACE "org.nemomobile.ofono.ModemManager"

struct ofono_data_sim {
	GDBusConnection *conn;
	/* cancelled on free, so no reply ever reaches a freed object */
	GCancellable *cancellable;
	guint changed_watch;
	/* what we want, what ofono has, and what is on its way to ofono */
	gchar *wanted;
	gchar *current;
	gchar *sending;
	/* ofono's setting has been read (or reading it failed) */
	gboolean known;
	gboolean supported;
};

static gboolean is_missing_interface(const GError *error)
{
	gchar *remote;
	gboolean missing;

	if (!g_dbus_error_is_remote_error(error))
		return FALSE;

	remote = g_dbus_error_get_remote_error(error);
	missing = g_strcmp0(remote, "org.freedesktop.DBus.Error.UnknownMethod") == 0 ||
			  g_strcmp0(remote, "org.freedesktop.DBus.Error.UnknownInterface") == 0 ||
			  g_strcmp0(remote, "org.freedesktop.DBus.Error.UnknownObject") == 0;
	g_free(remote);

	return missing;
}

static void push(struct ofono_data_sim *ds);

static void set_reply_cb(GObject *source, GAsyncResult *res, gpointer user_data)
{
	struct ofono_data_sim *ds = user_data;
	GError *error = NULL;
	GVariant *result;

	result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
	if (!result && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		/* ds is gone */
		g_error_free(error);
		return;
	}

	if (result) {
		g_message("[DataSim] ofono now carries data on the SIM with IMSI %s", ds->sending);
		g_free(ds->current);
		ds->current = g_strdup(ds->sending);
		g_variant_unref(result);
	}
	else {
		if (is_missing_interface(error))
			ds->supported = FALSE;
		g_warning("[DataSim] Could not set ofono's data SIM: %s", error->message);
		g_error_free(error);
	}

	/* the wish may have changed while the call was out; a failed one is not retried */
	if (g_strcmp0(ds->wanted, ds->sending) == 0) {
		g_free(ds->sending);
		ds->sending = NULL;
		return;
	}

	g_free(ds->sending);
	ds->sending = NULL;
	push(ds);
}

static void push(struct ofono_data_sim *ds)
{
	if (!ds->supported || !ds->known || ds->sending || !ds->wanted)
		return;

	if (g_strcmp0(ds->wanted, ds->current) == 0)
		return;

	ds->sending = g_strdup(ds->wanted);

	g_dbus_connection_call(ds->conn, OFONO_SERVICE, SLOT_MANAGER_PATH, SLOT_MANAGER_INTERFACE,
						   "SetDefaultDataSim", g_variant_new("(s)", ds->sending), NULL,
						   G_DBUS_CALL_FLAGS_NONE, -1, ds->cancellable, set_reply_cb, ds);
}

static void get_reply_cb(GObject *source, GAsyncResult *res, gpointer user_data)
{
	struct ofono_data_sim *ds = user_data;
	GError *error = NULL;
	GVariant *result;
	const gchar *imsi = NULL;

	result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
	if (!result && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		g_error_free(error);
		return;
	}

	ds->known = TRUE;

	if (!result) {
		if (is_missing_interface(error)) {
			g_message("[DataSim] ofono has no slot manager, it picks the data SIM itself");
			ds->supported = FALSE;
		}
		else {
			g_warning("[DataSim] Could not read ofono's data SIM: %s", error->message);
		}
		g_error_free(error);
		push(ds);
		return;
	}

	g_variant_get(result, "(&s)", &imsi);
	/* a DefaultDataSimChanged that came first is newer than this reply */
	if (!ds->current)
		ds->current = g_strdup(imsi);
	g_variant_unref(result);

	push(ds);
}

static void changed_cb(GDBusConnection *conn, const gchar *sender, const gchar *path,
					   const gchar *iface, const gchar *signal, GVariant *params, gpointer user_data)
{
	struct ofono_data_sim *ds = user_data;
	const gchar *imsi = NULL;

	if (!g_variant_is_of_type(params, G_VARIANT_TYPE("(s)")))
		return;

	g_variant_get(params, "(&s)", &imsi);

	g_free(ds->current);
	ds->current = g_strdup(imsi);
}

struct ofono_data_sim* ofono_data_sim_create(void)
{
	struct ofono_data_sim *ds;
	GError *error = NULL;

	ds = g_new0(struct ofono_data_sim, 1);
	ds->supported = TRUE;

	ds->conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
	if (!ds->conn) {
		g_warning("[DataSim] No system bus: %s", error->message);
		g_error_free(error);
		ds->supported = FALSE;
		return ds;
	}

	ds->cancellable = g_cancellable_new();

	ds->changed_watch = g_dbus_connection_signal_subscribe(ds->conn, OFONO_SERVICE, SLOT_MANAGER_INTERFACE,
														   "DefaultDataSimChanged", SLOT_MANAGER_PATH, NULL,
														   G_DBUS_SIGNAL_FLAGS_NONE, changed_cb, ds, NULL);

	g_dbus_connection_call(ds->conn, OFONO_SERVICE, SLOT_MANAGER_PATH, SLOT_MANAGER_INTERFACE,
						   "GetDefaultDataSim", NULL, G_VARIANT_TYPE("(s)"),
						   G_DBUS_CALL_FLAGS_NONE, -1, ds->cancellable, get_reply_cb, ds);

	return ds;
}

void ofono_data_sim_free(struct ofono_data_sim *ds)
{
	if (!ds)
		return;

	if (ds->cancellable) {
		g_cancellable_cancel(ds->cancellable);
		g_object_unref(ds->cancellable);
	}

	if (ds->conn) {
		if (ds->changed_watch)
			g_dbus_connection_signal_unsubscribe(ds->conn, ds->changed_watch);
		g_object_unref(ds->conn);
	}

	g_free(ds->wanted);
	g_free(ds->current);
	g_free(ds->sending);
	g_free(ds);
}

void ofono_data_sim_set_imsi(struct ofono_data_sim *ds, const char *imsi)
{
	if (!ds || !imsi || !imsi[0] || g_strcmp0(ds->wanted, imsi) == 0)
		return;

	g_free(ds->wanted);
	ds->wanted = g_strdup(imsi);

	push(ds);
}

const char* ofono_data_sim_get_current(struct ofono_data_sim *ds)
{
	return ds ? ds->current : NULL;
}

gboolean ofono_data_sim_is_supported(struct ofono_data_sim *ds)
{
	return ds ? ds->supported : FALSE;
}

// vim:ts=4:sw=4:noexpandtab
