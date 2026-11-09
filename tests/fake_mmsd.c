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
#include <sys/stat.h>

#include "fake_mmsd.h"

static const gchar fake_xml[] =
	"<node>"
	"  <interface name='org.ofono.mms.Manager'>"
	"    <method name='GetServices'><arg type='a(oa{sv})' direction='out'/></method>"
	"    <signal name='ServiceAdded'><arg type='o'/><arg type='a{sv}'/></signal>"
	"    <signal name='ServiceRemoved'><arg type='o'/></signal>"
	"  </interface>"
	"  <interface name='org.ofono.mms.Service'>"
	"    <method name='GetMessages'><arg type='a(oa{sv})' direction='out'/></method>"
	"    <method name='SendMessage'>"
	"      <arg type='as' direction='in'/><arg type='s' direction='in'/>"
	"      <arg type='a(sss)' direction='in'/><arg type='o' direction='out'/>"
	"    </method>"
	"    <signal name='MessageAdded'><arg type='o'/><arg type='a{sv}'/></signal>"
	"    <signal name='MessageRemoved'><arg type='o'/></signal>"
	"  </interface>"
	"  <interface name='org.ofono.mms.Message'>"
	"    <method name='Delete'/>"
	"    <method name='MarkRead'/>"
	"    <signal name='PropertyChanged'><arg type='s'/><arg type='v'/></signal>"
	"  </interface>"
	"</node>";

static GDBusNodeInfo *node;

static GVariant* list_reply(GPtrArray *entries)
{
	GVariantBuilder b;
	guint n;

	g_variant_builder_init(&b, G_VARIANT_TYPE("a(oa{sv})"));
	for (n = 0; entries && n < entries->len; n++)
		g_variant_builder_add_value(&b, g_ptr_array_index(entries, n));

	return g_variant_new("(a(oa{sv}))", &b);
}

static void manager_call(GDBusConnection *conn, const gchar *sender, const gchar *path, const gchar *iface,
                         const gchar *method, GVariant *params, GDBusMethodInvocation *inv, gpointer user_data)
{
	struct fake_mmsd *fake = user_data;
	GPtrArray *entries = g_ptr_array_new_with_free_func((GDestroyNotify) g_variant_unref);
	guint n;

	for (n = 0; n < fake->services->len; n++)
		g_ptr_array_add(entries, g_variant_ref_sink(g_variant_new("(o@a{sv})",
		                (const char *) g_ptr_array_index(fake->services, n),
		                g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0))));

	g_dbus_method_invocation_return_value(inv, list_reply(entries));
	g_ptr_array_free(entries, TRUE);
}

static void register_message(struct fake_mmsd *fake, const char *path);

static void service_call(GDBusConnection *conn, const gchar *sender, const gchar *path, const gchar *iface,
                         const gchar *method, GVariant *params, GDBusMethodInvocation *inv, gpointer user_data)
{
	struct fake_mmsd *fake = user_data;

	if (strcmp(method, "GetMessages") == 0) {
		g_dbus_method_invocation_return_value(inv, list_reply(g_hash_table_lookup(fake->stored, path)));
		return;
	}

	if (strcmp(method, "SendMessage") == 0) {
		GVariantIter *iter;
		const char *id, *ct, *file;
		gchar *msg_path;

		fake->sends++;
		if (fake->last_send)
			g_variant_unref(fake->last_send);
		fake->last_send = g_variant_ref(params);
		g_ptr_array_set_size(fake->last_send_data, 0);
		g_ptr_array_set_size(fake->last_send_modes, 0);

		/* what mmsd does before it answers: read every file */
		g_variant_get(params, "(assa(sss))", NULL, NULL, &iter);
		while (g_variant_iter_next(iter, "(&s&s&s)", &id, &ct, &file)) {
			gchar *data = NULL;
			gsize len = 0;
			struct stat st;

			if (!g_file_get_contents(file, &data, &len, NULL)) {
				g_variant_iter_free(iter);
				g_dbus_method_invocation_return_dbus_error(inv, "org.ofono.mms.Error.InvalidArguments",
				                                           "cannot read an attachment");
				return;
			}
			g_ptr_array_add(fake->last_send_data, g_bytes_new_take(data, len));
			g_ptr_array_add(fake->last_send_modes,
			                GUINT_TO_POINTER(stat(file, &st) == 0 ? (guint) (st.st_mode & 0777) : 0u));
		}
		g_variant_iter_free(iter);

		if (fake->send_error) {
			g_dbus_method_invocation_return_dbus_error(inv, fake->send_error, "refused by the fake");
			return;
		}

		msg_path = g_strdup_printf("%s/msg%u", path, ++fake->counter);
		register_message(fake, msg_path);
		g_free(fake->last_message);
		fake->last_message = g_strdup(msg_path);

		/* mmsd announces the new message (a draft) before it answers */
		g_dbus_connection_emit_signal(conn, NULL, path, "org.ofono.mms.Service", "MessageAdded",
		                              g_variant_new_parsed("(%o, {'Status': <'draft'>})", msg_path), NULL);
		g_dbus_method_invocation_return_value(inv, g_variant_new("(o)", msg_path));
		g_free(msg_path);
		return;
	}

	g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.UnknownMethod", method);
}

static void message_call(GDBusConnection *conn, const gchar *sender, const gchar *path, const gchar *iface,
                         const gchar *method, GVariant *params, GDBusMethodInvocation *inv, gpointer user_data)
{
	struct fake_mmsd *fake = user_data;

	if (strcmp(method, "Delete") == 0)
		g_ptr_array_add(fake->deleted, g_strdup(path));

	g_dbus_method_invocation_return_value(inv, NULL);
}

static const GDBusInterfaceVTable manager_vtable = { manager_call, NULL, NULL, { 0 } };
static const GDBusInterfaceVTable service_vtable = { service_call, NULL, NULL, { 0 } };
static const GDBusInterfaceVTable message_vtable = { message_call, NULL, NULL, { 0 } };

static void register_message(struct fake_mmsd *fake, const char *path)
{
	guint id;

	if (g_hash_table_contains(fake->objects, path))
		return;

	id = g_dbus_connection_register_object(fake->conn, path, node->interfaces[2], &message_vtable, fake,
	                                       NULL, NULL);
	g_assert_cmpuint(id, >, 0);
	g_hash_table_insert(fake->objects, g_strdup(path), GUINT_TO_POINTER(id));
}

static void unregister_objects(struct fake_mmsd *fake)
{
	GHashTableIter iter;
	gpointer id;

	g_hash_table_iter_init(&iter, fake->objects);
	while (g_hash_table_iter_next(&iter, NULL, &id))
		g_dbus_connection_unregister_object(fake->conn, GPOINTER_TO_UINT(id));
	g_hash_table_remove_all(fake->objects);
}

void fake_mmsd_start(struct fake_mmsd *fake)
{
	memset(fake, 0, sizeof(*fake));

	if (!node)
		node = g_dbus_node_info_new_for_xml(fake_xml, NULL);

	fake->bus = g_test_dbus_new(G_TEST_DBUS_NONE);
	g_test_dbus_up(fake->bus);
	g_setenv("DBUS_SYSTEM_BUS_ADDRESS", g_test_dbus_get_bus_address(fake->bus), TRUE);

	fake->conn = g_dbus_connection_new_for_address_sync(g_test_dbus_get_bus_address(fake->bus),
	                                                    G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
	                                                    G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
	                                                    NULL, NULL, NULL);
	g_assert_nonnull(fake->conn);

	fake->objects = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	fake->services = g_ptr_array_new_with_free_func(g_free);
	fake->last_send_data = g_ptr_array_new_with_free_func((GDestroyNotify) g_bytes_unref);
	fake->last_send_modes = g_ptr_array_new();
	fake->deleted = g_ptr_array_new_with_free_func(g_free);
	fake->stored = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_ptr_array_unref);

	fake->manager_id = g_dbus_connection_register_object(fake->conn, "/org/ofono/mms", node->interfaces[0],
	                                                     &manager_vtable, fake, NULL, NULL);
	g_assert_cmpuint(fake->manager_id, >, 0);
}

void fake_mmsd_stop(struct fake_mmsd *fake)
{
	unregister_objects(fake);
	g_dbus_connection_unregister_object(fake->conn, fake->manager_id);
	g_dbus_connection_close_sync(fake->conn, NULL, NULL);
	g_object_unref(fake->conn);

	g_hash_table_destroy(fake->objects);
	g_ptr_array_free(fake->services, TRUE);
	g_ptr_array_free(fake->last_send_data, TRUE);
	g_ptr_array_free(fake->last_send_modes, TRUE);
	g_ptr_array_free(fake->deleted, TRUE);
	g_hash_table_destroy(fake->stored);
	if (fake->last_send)
		g_variant_unref(fake->last_send);
	g_free(fake->send_error);
	g_free(fake->last_message);

	g_test_dbus_down(fake->bus);
	g_object_unref(fake->bus);
}

static void request_name(struct fake_mmsd *fake, const char *method, GVariant *params)
{
	GError *error = NULL;
	GVariant *reply = g_dbus_connection_call_sync(fake->conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
	                                              "org.freedesktop.DBus", method, params, NULL,
	                                              G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);

	g_assert_no_error(error);
	g_variant_unref(reply);
}

void fake_mmsd_own(struct fake_mmsd *fake)
{
	request_name(fake, "RequestName", g_variant_new("(su)", "org.ofono.mms", 4 /* DO_NOT_QUEUE */));
	fake->owned = TRUE;
}

void fake_mmsd_release(struct fake_mmsd *fake)
{
	request_name(fake, "ReleaseName", g_variant_new("(s)", "org.ofono.mms"));
	fake->owned = FALSE;
}

const char* fake_mmsd_add_service(struct fake_mmsd *fake, const char *identity, gboolean signal)
{
	gchar *path = g_strdup_printf("/org/ofono/mms/%s", identity);
	guint id = g_dbus_connection_register_object(fake->conn, path, node->interfaces[1], &service_vtable, fake,
	                                             NULL, NULL);

	g_assert_cmpuint(id, >, 0);
	g_hash_table_insert(fake->objects, g_strdup(path), GUINT_TO_POINTER(id));
	g_ptr_array_add(fake->services, path);

	if (signal)
		g_dbus_connection_emit_signal(fake->conn, NULL, "/org/ofono/mms", "org.ofono.mms.Manager", "ServiceAdded",
		                              g_variant_new_parsed("(%o, @a{sv} {})", path), NULL);

	return path;
}

void fake_mmsd_remove_service(struct fake_mmsd *fake, const char *path)
{
	gpointer id;
	guint n;

	if (g_hash_table_lookup_extended(fake->objects, path, NULL, &id)) {
		g_dbus_connection_unregister_object(fake->conn, GPOINTER_TO_UINT(id));
		g_hash_table_remove(fake->objects, path);
	}

	g_dbus_connection_emit_signal(fake->conn, NULL, "/org/ofono/mms", "org.ofono.mms.Manager", "ServiceRemoved",
	                              g_variant_new("(o)", path), NULL);

	for (n = 0; n < fake->services->len; n++) {
		if (strcmp(g_ptr_array_index(fake->services, n), path) == 0) {
			g_ptr_array_remove_index(fake->services, n);
			break;
		}
	}
}

void fake_mmsd_store_message(struct fake_mmsd *fake, const char *service_path, const char *message_path,
                             GVariant *props)
{
	GPtrArray *list = g_hash_table_lookup(fake->stored, service_path);

	if (!list) {
		list = g_ptr_array_new_with_free_func((GDestroyNotify) g_variant_unref);
		g_hash_table_insert(fake->stored, g_strdup(service_path), list);
	}

	register_message(fake, message_path);
	g_ptr_array_add(list, g_variant_ref_sink(g_variant_new("(o@a{sv})", message_path, props)));
}

void fake_mmsd_emit_message_added(struct fake_mmsd *fake, const char *service_path, const char *message_path,
                                  GVariant *props)
{
	register_message(fake, message_path);
	g_dbus_connection_emit_signal(fake->conn, NULL, service_path, "org.ofono.mms.Service", "MessageAdded",
	                              g_variant_new("(o@a{sv})", message_path, props), NULL);
}

void fake_mmsd_emit_status(struct fake_mmsd *fake, GDBusConnection *from, const char *message_path,
                           const char *status)
{
	g_dbus_connection_emit_signal(from ? from : fake->conn, NULL, message_path, "org.ofono.mms.Message",
	                              "PropertyChanged", g_variant_new("(sv)", "Status", g_variant_new_string(status)),
	                              NULL);
	g_dbus_connection_flush_sync(from ? from : fake->conn, NULL, NULL);
}

static gboolean wait_timeout(gpointer data)
{
	*(gboolean *) data = TRUE;
	return G_SOURCE_REMOVE;
}

gboolean fake_wait(gboolean (*cond)(gpointer), gpointer data)
{
	gboolean timed_out = FALSE;
	guint id = g_timeout_add_seconds(5, wait_timeout, &timed_out);

	while (!cond(data) && !timed_out)
		g_main_context_iteration(NULL, TRUE);

	if (!timed_out)
		g_source_remove(id);

	return cond(data);
}

void fake_settle(void)
{
	gboolean done = FALSE;

	g_timeout_add(200, wait_timeout, &done);
	while (!done)
		g_main_context_iteration(NULL, TRUE);
}

// vim:ts=4:sw=4:noexpandtab
