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

#include <string.h>

#include <glib.h>
#include <gio/gio.h>

#include "ofonocellinfo.h"

#define CELL_INFO_INTERFACE "org.nemomobile.ofono.CellInfo"
#define CELL_INTERFACE "org.nemomobile.ofono.Cell"

struct ofono_cell_info {
	gchar *path;
	GDBusConnection *conn;
	/* cancelled on free: the pending calls answer their queries then */
	GCancellable *cancellable;
	unsigned int linger_ms;
	guint linger_id;
	bool subscribed;
};

/* One query: the cells to read, and what was read of them */
struct cell_query {
	struct ofono_cell_info *info;
	GCancellable *cancellable;
	ofono_cell_info_query_cb cb;
	void *data;
	GArray *cells;
	unsigned int pending;
};

static bool in_range(GVariant *properties, const char *name, int min, int max, int *value)
{
	gint32 v;

	if (!g_variant_lookup(properties, name, "i", &v) || v < min || v > max)
		return false;

	*value = v;
	return true;
}

/* TS 27.007 8.5: 0 is -113 dBm or less, 31 -51 dBm or more, 2 dB a step */
static void parse_rssi(GVariant *properties, struct telephony_cell_signal *signal)
{
	int asu;

	if (in_range(properties, "signalStrength", 0, 31, &asu)) {
		signal->has_rssi = true;
		signal->rssi = -113 + 2 * asu;
	}
}

bool ofono_cell_signal_parse(const gchar *type, GVariant *properties, struct telephony_cell_signal *signal)
{
	int v;

	memset(signal, 0, sizeof(*signal));

	if (!type || !properties || !g_str_equal(g_variant_get_type_string(properties), "a{sv}"))
		return false;

	if (g_str_equal(type, "gsm")) {
		signal->type = "gsm";
		parse_rssi(properties, signal);
	}
	else if (g_str_equal(type, "wcdma")) {
		signal->type = "wcdma";
		parse_rssi(properties, signal);
	}
	else if (g_str_equal(type, "lte")) {
		signal->type = "lte";
		parse_rssi(properties, signal);
		/* dBm times -1, 44 to 140 */
		if ((signal->has_rsrp = in_range(properties, "rsrp", 44, 140, &v)))
			signal->rsrp = -v;
		/* dB times -1, 3 to 20 */
		if ((signal->has_rsrq = in_range(properties, "rsrq", 3, 20, &v)))
			signal->rsrq = -v;
		/* 0.1 dB, -200 to 300 */
		if ((signal->has_sinr = in_range(properties, "rssnr", -200, 300, &v)))
			signal->sinr = v / 10.0;
	}
	else if (g_str_equal(type, "nr")) {
		signal->type = "nr";
		/* dBm times -1, 44 to 140 */
		if ((signal->has_rsrp = in_range(properties, "ssRsrp", 44, 140, &v)))
			signal->rsrp = -v;
		/* dB times -1, -20 to 43 */
		if ((signal->has_rsrq = in_range(properties, "ssRsrq", -20, 43, &v)))
			signal->rsrq = -v;
		/* dB, -23 to 40 */
		if ((signal->has_sinr = in_range(properties, "ssSinr", -23, 40, &v)))
			signal->sinr = v;
	}
	else
		return false;

	return true;
}

static void unsubscribed_cb(GObject *source, GAsyncResult *res, gpointer user_data)
{
	GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, NULL);

	/* ofono has dropped the subscription, or the bus has gone and it with it */
	if (reply)
		g_variant_unref(reply);
	g_object_unref(user_data);
}

static void unsubscribe(struct ofono_cell_info *info)
{
	if (!info->subscribed)
		return;

	/* The call holds the connection, so a free right after still sends it */
	g_dbus_connection_call(info->conn, "org.ofono", info->path, CELL_INFO_INTERFACE, "Unsubscribe",
						   NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, unsubscribed_cb,
						   g_object_ref(info->conn));
	info->subscribed = false;
	g_message("[CellInfo:%s] unsubscribed", info->path);
}

static gboolean linger_done(gpointer user_data)
{
	struct ofono_cell_info *info = user_data;

	info->linger_id = 0;
	unsubscribe(info);
	return G_SOURCE_REMOVE;
}

static void cell_query_finish(struct cell_query *query, const GError *error)
{
	if (error)
		query->cb(error, NULL, 0, query->data);
	else
		query->cb(NULL, (const struct telephony_cell_signal *) (void *) query->cells->data,
				  query->cells->len, query->data);

	g_array_free(query->cells, TRUE);
	g_object_unref(query->cancellable);
	g_free(query);
}

static void get_all_cb(GObject *source, GAsyncResult *res, gpointer user_data)
{
	struct cell_query *query = user_data;
	GError *error = NULL;
	GVariant *reply;

	reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
	if (reply) {
		gint32 version;
		const gchar *type;
		gboolean registered;
		GVariant *properties;
		struct telephony_cell_signal signal;

		g_variant_get(reply, "(i&sb@a{sv})", &version, &type, &registered, &properties);
		/* The serving cells only; a neighbour's signal is not the phone's */
		if (registered && ofono_cell_signal_parse(type, properties, &signal))
			g_array_append_val(query->cells, signal);
		g_variant_unref(properties);
		g_variant_unref(reply);
	}
	else if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		/* the info is gone: answer once the last call is back */
		if (--query->pending == 0)
			cell_query_finish(query, error);
		g_error_free(error);
		return;
	}
	else {
		/* A cell can go between GetCells and its GetAll */
		g_error_free(error);
	}

	if (--query->pending == 0) {
		if (g_cancellable_is_cancelled(query->cancellable)) {
			GError *cancelled = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cell info is gone");
			cell_query_finish(query, cancelled);
			g_error_free(cancelled);
		}
		else
			cell_query_finish(query, NULL);
	}
}

static void get_cells_cb(GObject *source, GAsyncResult *res, gpointer user_data)
{
	struct cell_query *query = user_data;
	struct ofono_cell_info *info;
	GError *error = NULL;
	GVariant *reply;
	GVariantIter *iter;
	const gchar *cell_path;

	reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
	if (!reply) {
		cell_query_finish(query, error);
		g_error_free(error);
		return;
	}

	/* Not cancelled, so the info is still there */
	info = query->info;
	info->subscribed = true;
	if (info->linger_id)
		g_source_remove(info->linger_id);
	info->linger_id = g_timeout_add(info->linger_ms, linger_done, info);

	g_variant_get(reply, "(ao)", &iter);
	query->pending = 1;
	while (g_variant_iter_loop(iter, "&o", &cell_path)) {
		query->pending++;
		g_dbus_connection_call(info->conn, "org.ofono", cell_path, CELL_INTERFACE, "GetAll", NULL,
							   G_VARIANT_TYPE("(isba{sv})"), G_DBUS_CALL_FLAGS_NONE, -1,
							   query->cancellable, get_all_cb, query);
	}
	g_variant_iter_free(iter);
	g_variant_unref(reply);

	/* the one held while the calls went out */
	if (--query->pending == 0)
		cell_query_finish(query, NULL);
}

struct ofono_cell_info* ofono_cell_info_create(const gchar *path, unsigned int linger_ms)
{
	struct ofono_cell_info *info;
	GError *error = NULL;
	GDBusConnection *conn;

	conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
	if (!conn) {
		g_warning("Unable to reach the system bus for cell info: %s", error->message);
		g_error_free(error);
		return NULL;
	}

	info = g_new0(struct ofono_cell_info, 1);
	info->path = g_strdup(path);
	info->conn = conn;
	info->cancellable = g_cancellable_new();
	info->linger_ms = linger_ms;

	return info;
}

void ofono_cell_info_free(struct ofono_cell_info *info)
{
	if (!info)
		return;

	g_cancellable_cancel(info->cancellable);
	g_object_unref(info->cancellable);

	if (info->linger_id)
		g_source_remove(info->linger_id);
	unsubscribe(info);

	g_object_unref(info->conn);
	g_free(info->path);
	g_free(info);
}

void ofono_cell_info_query(struct ofono_cell_info *info, ofono_cell_info_query_cb cb, void *data)
{
	struct cell_query *query;

	if (!info) {
		/* No CellInfo: no cells to tell of */
		cb(NULL, NULL, 0, data);
		return;
	}

	query = g_new0(struct cell_query, 1);
	query->info = info;
	query->cancellable = g_object_ref(info->cancellable);
	query->cb = cb;
	query->data = data;
	query->cells = g_array_new(FALSE, TRUE, sizeof(struct telephony_cell_signal));

	/* Subscribes, or renews the subscription */
	g_dbus_connection_call(info->conn, "org.ofono", info->path, CELL_INFO_INTERFACE, "GetCells", NULL,
						   G_VARIANT_TYPE("(ao)"), G_DBUS_CALL_FLAGS_NONE, -1,
						   info->cancellable, get_cells_cb, query);
}

bool ofono_cell_info_is_subscribed(const struct ofono_cell_info *info)
{
	return info && info->subscribed;
}

// vim:ts=4:sw=4:noexpandtab
