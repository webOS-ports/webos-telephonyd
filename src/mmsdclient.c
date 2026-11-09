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

#include <string.h>

#include "mmsdclient.h"
#include "mmscore.h"

/* mmsd encodes the whole message into its store before it answers */
#define SEND_TIMEOUT_MS 60000

struct mmsd_client {
	struct mmsd_client_callbacks callbacks;
	gpointer user_data;
	GCancellable *cancellable;
	guint watch_id;
	GDBusConnection *conn;
	gchar *owner;
	/* bumped whenever the owner goes, so late replies for the old one
	 * are dropped */
	guint generation;
	guint sub_manager;
	guint sub_service;
	guint sub_message;
	GPtrArray *services;	/* gchar* object paths */
};

struct pending {
	struct mmsd_client *client;
	guint generation;
	gchar *path;
};

static struct pending* pending_new(struct mmsd_client *client, const char *path)
{
	struct pending *p = g_new0(struct pending, 1);

	p->client = client;
	p->generation = client->generation;
	p->path = g_strdup(path);

	return p;
}

static void pending_free(struct pending *p)
{
	g_free(p->path);
	g_free(p);
}

/* The client behind a reply, or NULL when the reply is for a client that is
 * gone (cancelled) or for an owner that has gone since. */
static struct mmsd_client* pending_client(const struct pending *p, const GError *error)
{
	if (error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		return NULL;

	if (p->client->generation != p->generation)
		return NULL;

	return p->client;
}

static gint find_service(struct mmsd_client *client, const char *path)
{
	guint n;

	for (n = 0; n < client->services->len; n++) {
		if (strcmp(g_ptr_array_index(client->services, n), path) == 0)
			return (gint) n;
	}

	return -1;
}

static void announce_messages(struct mmsd_client *client, const char *service_path, GVariant *messages)
{
	GVariantIter iter;
	const char *path;
	GVariant *props;

	if (!client->callbacks.message_added)
		return;

	g_variant_iter_init(&iter, messages);
	while (g_variant_iter_next(&iter, "(&o@a{sv})", &path, &props)) {
		client->callbacks.message_added(client, service_path, path, props, client->user_data);
		g_variant_unref(props);
	}
}

static void get_messages_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
	struct pending *p = user_data;
	struct mmsd_client *client;
	GError *error = NULL;
	GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);

	client = pending_client(p, error);

	if (client && reply && find_service(client, p->path) >= 0) {
		GVariant *messages = g_variant_get_child_value(reply, 0);

		announce_messages(client, p->path, messages);
		g_variant_unref(messages);
	}
	else if (client && error) {
		g_warning("[MMS] GetMessages failed: %s", error->message);
	}

	if (reply)
		g_variant_unref(reply);
	g_clear_error(&error);
	pending_free(p);
}

static void add_service(struct mmsd_client *client, const char *path)
{
	if (find_service(client, path) >= 0)
		return;

	g_ptr_array_add(client->services, g_strdup(path));
	g_message("[MMS] mmsd offers a service (%u now)", client->services->len);

	if (client->services->len == 1 && client->callbacks.availability_changed)
		client->callbacks.availability_changed(client, TRUE, client->user_data);

	g_dbus_connection_call(client->conn, client->owner, path, MMSD_SERVICE_INTERFACE, "GetMessages",
	                       NULL, G_VARIANT_TYPE("(a(oa{sv}))"), G_DBUS_CALL_FLAGS_NONE, -1,
	                       client->cancellable, get_messages_done, pending_new(client, path));
}

static void remove_service(struct mmsd_client *client, const char *path)
{
	gint index = find_service(client, path);

	if (index < 0)
		return;

	g_ptr_array_remove_index(client->services, (guint) index);
	g_message("[MMS] an mmsd service went away (%u left)", client->services->len);

	if (client->services->len == 0 && client->callbacks.availability_changed)
		client->callbacks.availability_changed(client, FALSE, client->user_data);
}

static void get_services_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
	struct pending *p = user_data;
	struct mmsd_client *client;
	GError *error = NULL;
	GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);

	client = pending_client(p, error);

	if (client && reply) {
		GVariantIter *iter;
		const char *path;

		g_variant_get(reply, "(a(oa{sv}))", &iter);
		while (g_variant_iter_next(iter, "(&oa{sv})", &path, NULL))
			add_service(client, path);
		g_variant_iter_free(iter);
	}
	else if (client && error) {
		g_warning("[MMS] GetServices failed: %s", error->message);
	}

	if (reply)
		g_variant_unref(reply);
	g_clear_error(&error);
	pending_free(p);
}

static void on_signal(GDBusConnection *conn, const gchar *sender, const gchar *object_path,
                      const gchar *interface_name, const gchar *signal_name, GVariant *parameters,
                      gpointer user_data)
{
	struct mmsd_client *client = user_data;

	if (strcmp(interface_name, MMSD_MANAGER_INTERFACE) == 0) {
		const char *path;

		if (strcmp(object_path, MMSD_MANAGER_PATH) != 0)
			return;

		if (strcmp(signal_name, "ServiceAdded") == 0 &&
		    g_variant_is_of_type(parameters, G_VARIANT_TYPE("(oa{sv})"))) {
			g_variant_get(parameters, "(&oa{sv})", &path, NULL);
			add_service(client, path);
		}
		else if (strcmp(signal_name, "ServiceRemoved") == 0 &&
		         g_variant_is_of_type(parameters, G_VARIANT_TYPE("(o)"))) {
			g_variant_get(parameters, "(&o)", &path);
			remove_service(client, path);
		}
	}
	else if (strcmp(interface_name, MMSD_SERVICE_INTERFACE) == 0) {
		const char *path;
		GVariant *props;

		if (strcmp(signal_name, "MessageAdded") != 0 ||
		    !g_variant_is_of_type(parameters, G_VARIANT_TYPE("(oa{sv})")) ||
		    find_service(client, object_path) < 0 || !client->callbacks.message_added)
			return;

		g_variant_get(parameters, "(&o@a{sv})", &path, &props);
		client->callbacks.message_added(client, object_path, path, props, client->user_data);
		g_variant_unref(props);
	}
	else if (strcmp(interface_name, MMSD_MESSAGE_INTERFACE) == 0) {
		const char *name;
		GVariant *value;

		if (strcmp(signal_name, "PropertyChanged") != 0 ||
		    !g_variant_is_of_type(parameters, G_VARIANT_TYPE("(sv)")))
			return;

		g_variant_get(parameters, "(&sv)", &name, &value);
		if (strcmp(name, "Status") == 0 && g_variant_classify(value) == G_VARIANT_CLASS_STRING &&
		    client->callbacks.message_status)
			client->callbacks.message_status(client, object_path, g_variant_get_string(value, NULL),
			                                 client->user_data);
		g_variant_unref(value);
	}
}

static void unsubscribe(struct mmsd_client *client)
{
	guint *ids[] = { &client->sub_manager, &client->sub_service, &client->sub_message };
	guint n;

	for (n = 0; n < G_N_ELEMENTS(ids); n++) {
		if (*ids[n] && client->conn)
			g_dbus_connection_signal_unsubscribe(client->conn, *ids[n]);
		*ids[n] = 0;
	}
}

static void drop_owner(struct mmsd_client *client)
{
	gboolean had_services = client->services->len > 0;

	unsubscribe(client);
	client->generation++;
	g_ptr_array_set_size(client->services, 0);
	g_clear_pointer(&client->owner, g_free);
	g_clear_object(&client->conn);

	if (had_services && client->callbacks.availability_changed)
		client->callbacks.availability_changed(client, FALSE, client->user_data);
}

static void name_appeared(GDBusConnection *conn, const gchar *name, const gchar *name_owner,
                          gpointer user_data)
{
	struct mmsd_client *client = user_data;

	drop_owner(client);

	g_message("[MMS] mmsd is on the bus");

	client->conn = g_object_ref(conn);
	client->owner = g_strdup(name_owner);

	/* Only the owner of org.ofono.mms may tell us about messages */
	client->sub_manager = g_dbus_connection_signal_subscribe(conn, name_owner, MMSD_MANAGER_INTERFACE, NULL,
	                                                         MMSD_MANAGER_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
	                                                         on_signal, client, NULL);
	client->sub_service = g_dbus_connection_signal_subscribe(conn, name_owner, MMSD_SERVICE_INTERFACE,
	                                                         "MessageAdded", NULL, NULL,
	                                                         G_DBUS_SIGNAL_FLAGS_NONE, on_signal, client, NULL);
	client->sub_message = g_dbus_connection_signal_subscribe(conn, name_owner, MMSD_MESSAGE_INTERFACE,
	                                                         "PropertyChanged", NULL, NULL,
	                                                         G_DBUS_SIGNAL_FLAGS_NONE, on_signal, client, NULL);

	g_dbus_connection_call(conn, name_owner, MMSD_MANAGER_PATH, MMSD_MANAGER_INTERFACE, "GetServices", NULL,
	                       G_VARIANT_TYPE("(a(oa{sv}))"), G_DBUS_CALL_FLAGS_NONE, -1, client->cancellable,
	                       get_services_done, pending_new(client, MMSD_MANAGER_PATH));
}

static void name_vanished(GDBusConnection *conn, const gchar *name, gpointer user_data)
{
	struct mmsd_client *client = user_data;

	if (client->owner)
		g_message("[MMS] mmsd left the bus");

	drop_owner(client);
}

struct mmsd_client* mmsd_client_new(GBusType bus_type, const struct mmsd_client_callbacks *callbacks,
                                    gpointer user_data)
{
	struct mmsd_client *client = g_new0(struct mmsd_client, 1);

	if (callbacks)
		client->callbacks = *callbacks;
	client->user_data = user_data;
	client->cancellable = g_cancellable_new();
	client->services = g_ptr_array_new_with_free_func(g_free);
	client->watch_id = g_bus_watch_name(bus_type, MMSD_SERVICE, G_BUS_NAME_WATCHER_FLAGS_NONE,
	                                    name_appeared, name_vanished, client, NULL);

	return client;
}

void mmsd_client_free(struct mmsd_client *client)
{
	if (!client)
		return;

	g_cancellable_cancel(client->cancellable);
	if (client->watch_id)
		g_bus_unwatch_name(client->watch_id);

	/* no callbacks out of a client that is going away */
	memset(&client->callbacks, 0, sizeof(client->callbacks));
	drop_owner(client);

	g_ptr_array_free(client->services, TRUE);
	g_object_unref(client->cancellable);
	g_free(client);
}

guint mmsd_client_service_count(struct mmsd_client *client)
{
	return client ? client->services->len : 0;
}

const char* mmsd_client_pick_service(struct mmsd_client *client, const char *identity)
{
	if (!client || client->services->len == 0)
		return NULL;

	if (identity && *identity) {
		guint n;

		for (n = 0; n < client->services->len; n++) {
			const char *path = g_ptr_array_index(client->services, n);
			const char *slash = strrchr(path, '/');

			if (slash && strcmp(slash + 1, identity) == 0)
				return path;
		}
	}

	return g_ptr_array_index(client->services, 0);
}

struct send_ctx {
	mmsd_send_cb callback;
	gpointer user_data;
	GError *error;
};

static void send_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
	struct send_ctx *ctx = user_data;
	GError *error = NULL;
	GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);

	if (reply) {
		const char *path;

		g_variant_get(reply, "(&o)", &path);
		ctx->callback(path, NULL, ctx->user_data);
		g_variant_unref(reply);
	}
	else {
		ctx->callback(NULL, error, ctx->user_data);
		g_error_free(error);
	}

	g_free(ctx);
}

static gboolean send_failed_idle(gpointer user_data)
{
	struct send_ctx *ctx = user_data;

	ctx->callback(NULL, ctx->error, ctx->user_data);
	g_error_free(ctx->error);
	g_free(ctx);

	return G_SOURCE_REMOVE;
}

void mmsd_client_send(struct mmsd_client *client, const char *service_path, GPtrArray *recipients,
                      const char *smil, GPtrArray *parts, mmsd_send_cb callback, gpointer user_data)
{
	struct send_ctx *ctx = g_new0(struct send_ctx, 1);
	GVariantBuilder to;
	GVariantBuilder attachments;
	guint n;

	ctx->callback = callback;
	ctx->user_data = user_data;

	if (!client || !client->conn || !service_path || find_service(client, service_path) < 0) {
		ctx->error = g_error_new_literal(G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN,
		                                 "mmsd has no service to send with");
		g_idle_add(send_failed_idle, ctx);
		return;
	}

	g_variant_builder_init(&to, G_VARIANT_TYPE("as"));
	for (n = 0; n < recipients->len; n++)
		g_variant_builder_add(&to, "s", (const char *) g_ptr_array_index(recipients, n));

	g_variant_builder_init(&attachments, G_VARIANT_TYPE("a(sss)"));
	for (n = 0; n < parts->len; n++) {
		const struct mms_send_part *part = g_ptr_array_index(parts, n);

		g_variant_builder_add(&attachments, "(sss)", part->content_id, part->content_type, part->file);
	}

	g_dbus_connection_call(client->conn, client->owner, service_path, MMSD_SERVICE_INTERFACE, "SendMessage",
	                       g_variant_new("(assa(sss))", &to, smil ? smil : "", &attachments),
	                       G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, SEND_TIMEOUT_MS,
	                       client->cancellable, send_done, ctx);
}

void mmsd_client_delete_message(struct mmsd_client *client, const char *message_path)
{
	if (!client || !client->conn || !message_path || !g_variant_is_object_path(message_path))
		return;

	g_dbus_connection_call(client->conn, client->owner, message_path, MMSD_MESSAGE_INTERFACE, "Delete", NULL,
	                       NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

// vim:ts=4:sw=4:noexpandtab
