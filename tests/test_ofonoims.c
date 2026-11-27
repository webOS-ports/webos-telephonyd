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

/*
 * A modem's IMS registration against a fake ofono on a private bus.
 *
 * The fake implements org.ofono.IpMultimediaSystem on /ril_0 as ofono 2.19's
 * src/ims.c does: GetProperties with Registered, VoiceCapable and
 * SmsCapable, and PropertyChanged for each change.
 */

#include <string.h>

#include <glib.h>
#include <gio/gio.h>

#include "ofonoims.h"

#define MODEM_PATH "/ril_0"
#define IMS_IFACE "org.ofono.IpMultimediaSystem"

static const gchar fake_ofono_xml[] =
	"<node>"
	"  <interface name='org.ofono.IpMultimediaSystem'>"
	"    <method name='GetProperties'><arg type='a{sv}' direction='out'/></method>"
	"    <method name='SetProperty'><arg type='s' direction='in'/><arg type='v' direction='in'/></method>"
	"    <signal name='PropertyChanged'><arg type='s'/><arg type='v'/></signal>"
	"  </interface>"
	"</node>";

static struct {
	GDBusConnection *conn;
	gboolean registered;
	gboolean voice_capable;
	gboolean sms_capable;
} fake;

/* What the code under test reported */
static struct {
	int changes;
	gchar *last;
} seen;

static void fake_method_call(GDBusConnection *conn, const gchar *sender, const gchar *path,
                             const gchar *iface, const gchar *method, GVariant *params,
                             GDBusMethodInvocation *invocation, gpointer user_data)
{
	GVariantBuilder props;

	if (!g_str_equal(method, "GetProperties")) {
		g_dbus_method_invocation_return_dbus_error(invocation, "org.ofono.Error.NotImplemented", method);
		return;
	}

	g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&props, "{sv}", "Registered", g_variant_new_boolean(fake.registered));
	g_variant_builder_add(&props, "{sv}", "VoiceCapable", g_variant_new_boolean(fake.voice_capable));
	g_variant_builder_add(&props, "{sv}", "SmsCapable", g_variant_new_boolean(fake.sms_capable));
	g_dbus_method_invocation_return_value(invocation, g_variant_new("(@a{sv})", g_variant_builder_end(&props)));
}

static const GDBusInterfaceVTable fake_vtable = { fake_method_call, NULL, NULL, { 0 } };

static void fake_change(const gchar *name, GVariant *value)
{
	GError *error = NULL;

	g_dbus_connection_emit_signal(fake.conn, NULL, MODEM_PATH, IMS_IFACE, "PropertyChanged",
								  g_variant_new("(sv)", name, value), &error);
	g_assert_no_error(error);
	g_dbus_connection_flush_sync(fake.conn, NULL, &error);
	g_assert_no_error(error);
}

static void changed_cb(const gchar *name, void *data)
{
	g_assert_true(data == &seen);
	seen.changes++;
	g_free(seen.last);
	seen.last = g_strdup(name);
}

static gboolean timed_out(gpointer data)
{
	*(gboolean *) data = TRUE;
	return G_SOURCE_REMOVE;
}

/* Runs the main loop until there were this many changes, or for a while */
static void wait_for_changes(int changes)
{
	gboolean timeout = FALSE;
	guint id = g_timeout_add(3000, timed_out, &timeout);

	while (seen.changes < changes && !timeout)
		g_main_context_iteration(NULL, TRUE);

	if (!timeout)
		g_source_remove(id);
	g_assert_cmpint(seen.changes, ==, changes);
}

/* Lets queued signals through without waiting for a change */
static void spin(void)
{
	gboolean timeout = FALSE;

	g_timeout_add(200, timed_out, &timeout);
	while (!timeout)
		g_main_context_iteration(NULL, TRUE);
}

static struct ofono_ims* create_ims(void)
{
	struct ofono_ims *ims;

	memset(&seen, 0, sizeof(seen));
	ims = ofono_ims_create(MODEM_PATH);
	g_assert_nonnull(ims);
	ofono_ims_register_prop_changed_handler(ims, changed_cb, &seen);
	return ims;
}

static void free_ims(struct ofono_ims *ims)
{
	ofono_ims_free(ims);
	g_free(seen.last);
	seen.last = NULL;
}

static void test_registered_with_voice(void)
{
	struct ofono_ims *ims;

	fake.registered = TRUE;
	fake.voice_capable = TRUE;
	ims = create_ims();

	/* Registered, VoiceCapable and SmsCapable from GetProperties */
	wait_for_changes(3);
	g_assert_true(ofono_ims_get_registered(ims));
	g_assert_true(ofono_ims_get_voice_capable(ims));
	g_assert_true(ofono_ims_voice_available(ims));
	g_assert_false(ofono_ims_get_sms_capable(ims));

	free_ims(ims);
}

static void test_registered_without_voice(void)
{
	struct ofono_ims *ims;

	/* Registered for SMS over IMS only: no VoLTE */
	fake.registered = TRUE;
	fake.voice_capable = FALSE;
	fake.sms_capable = TRUE;
	ims = create_ims();

	wait_for_changes(3);
	g_assert_true(ofono_ims_get_registered(ims));
	g_assert_true(ofono_ims_get_sms_capable(ims));
	g_assert_false(ofono_ims_voice_available(ims));
	fake.sms_capable = FALSE;

	free_ims(ims);
}

static void test_changes_follow(void)
{
	struct ofono_ims *ims;

	fake.registered = FALSE;
	fake.voice_capable = FALSE;
	ims = create_ims();
	wait_for_changes(3);
	g_assert_false(ofono_ims_voice_available(ims));

	fake_change("VoiceCapable", g_variant_new_boolean(TRUE));
	wait_for_changes(4);
	g_assert_cmpstr(seen.last, ==, "VoiceCapable");
	g_assert_false(ofono_ims_voice_available(ims));

	fake_change("Registered", g_variant_new_boolean(TRUE));
	wait_for_changes(5);
	g_assert_cmpstr(seen.last, ==, "Registered");
	g_assert_true(ofono_ims_voice_available(ims));

	fake_change("SmsCapable", g_variant_new_boolean(TRUE));
	wait_for_changes(6);
	g_assert_cmpstr(seen.last, ==, "SmsCapable");
	g_assert_true(ofono_ims_get_sms_capable(ims));

	/* Lost IMS, as when the phone leaves LTE */
	fake_change("Registered", g_variant_new_boolean(FALSE));
	wait_for_changes(7);
	g_assert_false(ofono_ims_voice_available(ims));
	g_assert_true(ofono_ims_get_voice_capable(ims));

	free_ims(ims);
}

static void test_other_properties_ignored(void)
{
	struct ofono_ims *ims;

	fake.registered = TRUE;
	fake.voice_capable = TRUE;
	ims = create_ims();
	wait_for_changes(3);

	/* ofono has no such property; one it adds later is not news */
	fake_change("SmsOverIms", g_variant_new_boolean(FALSE));
	/* A property of the wrong type changes nothing */
	fake_change("Registered", g_variant_new_string("no"));
	spin();
	g_assert_cmpint(seen.changes, ==, 3);
	g_assert_true(ofono_ims_voice_available(ims));

	free_ims(ims);
}

static void test_without_ims(void)
{
	g_assert_false(ofono_ims_get_registered(NULL));
	g_assert_false(ofono_ims_get_voice_capable(NULL));
	g_assert_false(ofono_ims_voice_available(NULL));
	g_assert_false(ofono_ims_get_sms_capable(NULL));
	ofono_ims_register_prop_changed_handler(NULL, changed_cb, &seen);
	ofono_ims_free(NULL);
}

int main(int argc, char **argv)
{
	GTestDBus *bus;
	GDBusNodeInfo *node;
	GError *error = NULL;
	GVariant *result;
	guint object_id;
	int ret;

	g_test_init(&argc, &argv, NULL);

	bus = g_test_dbus_new(G_TEST_DBUS_NONE);
	g_test_dbus_up(bus);
	/* the code under test uses G_BUS_TYPE_SYSTEM */
	g_setenv("DBUS_SYSTEM_BUS_ADDRESS", g_test_dbus_get_bus_address(bus), TRUE);

	fake.conn = g_dbus_connection_new_for_address_sync(g_test_dbus_get_bus_address(bus),
		G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
		NULL, NULL, &error);
	g_assert_no_error(error);

	node = g_dbus_node_info_new_for_xml(fake_ofono_xml, &error);
	g_assert_no_error(error);
	object_id = g_dbus_connection_register_object(fake.conn, MODEM_PATH, node->interfaces[0],
												  &fake_vtable, NULL, NULL, &error);
	g_assert_no_error(error);

	result = g_dbus_connection_call_sync(fake.conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
										 "org.freedesktop.DBus", "RequestName",
										 g_variant_new("(su)", "org.ofono", 0x4), G_VARIANT_TYPE("(u)"),
										 G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
	g_assert_no_error(error);
	g_variant_unref(result);

	g_test_add_func("/ofonoims/registered-with-voice", test_registered_with_voice);
	g_test_add_func("/ofonoims/registered-without-voice", test_registered_without_voice);
	g_test_add_func("/ofonoims/changes-follow", test_changes_follow);
	g_test_add_func("/ofonoims/other-properties-ignored", test_other_properties_ignored);
	g_test_add_func("/ofonoims/without-ims", test_without_ims);

	ret = g_test_run();

	g_dbus_connection_unregister_object(fake.conn, object_id);
	g_dbus_node_info_unref(node);
	g_object_unref(fake.conn);
	g_test_dbus_down(bus);
	g_object_unref(bus);

	return ret;
}

// vim:ts=4:sw=4:noexpandtab
