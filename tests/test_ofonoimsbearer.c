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
 * What carries a modem's IMS registration, against a fake ofono on a private
 * bus, and the VoLTE and VoWiFi that telephonyd makes of it.
 *
 * The fake implements on /ril_0 org.ofono.IpMultimediaSystem as ofono's
 * src/ims.c does, and org.ofono.mtk.IpMultimediaSystem as the MediaTek
 * ofono-binder extension does: GetProperties with Bearer, "wifi",
 * "cellular" or "", and PropertyChanged for each change.
 */

#include <string.h>

#include <glib.h>
#include <gio/gio.h>

#include "ofonoims.h"
#include "ofonoimsbearer.h"

#define MODEM_PATH "/ril_0"
#define IMS_IFACE "org.ofono.IpMultimediaSystem"
#define BEARER_IFACE "org.ofono.mtk.IpMultimediaSystem"

static const gchar fake_ofono_xml[] =
	"<node>"
	"  <interface name='org.ofono.IpMultimediaSystem'>"
	"    <method name='GetProperties'><arg type='a{sv}' direction='out'/></method>"
	"    <method name='SetProperty'><arg type='s' direction='in'/><arg type='v' direction='in'/></method>"
	"    <signal name='PropertyChanged'><arg type='s'/><arg type='v'/></signal>"
	"  </interface>"
	"  <interface name='org.ofono.mtk.IpMultimediaSystem'>"
	"    <method name='GetProperties'><arg type='a{sv}' direction='out'/></method>"
	"    <signal name='PropertyChanged'><arg type='s'/><arg type='v'/></signal>"
	"  </interface>"
	"</node>";

static struct {
	GDBusConnection *conn;
	gboolean registered;
	gboolean voice_capable;
	const gchar *bearer;
	/* NULL: an extension from before WfcPreference */
	const gchar *wfc_preference;
} fake;

/* What the code under test reported */
static struct {
	int ims_changes;
	int bearer_changes;
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
	if (g_str_equal(iface, IMS_IFACE)) {
		g_variant_builder_add(&props, "{sv}", "Registered", g_variant_new_boolean(fake.registered));
		g_variant_builder_add(&props, "{sv}", "VoiceCapable", g_variant_new_boolean(fake.voice_capable));
		g_variant_builder_add(&props, "{sv}", "SmsCapable", g_variant_new_boolean(TRUE));
	}
	else {
		g_variant_builder_add(&props, "{sv}", "Bearer", g_variant_new_string(fake.bearer));
		if (fake.wfc_preference)
			g_variant_builder_add(&props, "{sv}", "WfcPreference", g_variant_new_string(fake.wfc_preference));
	}
	g_dbus_method_invocation_return_value(invocation, g_variant_new("(@a{sv})", g_variant_builder_end(&props)));
}

static const GDBusInterfaceVTable fake_vtable = { fake_method_call, NULL, NULL, { 0 } };

static void fake_change(const gchar *iface, const gchar *name, GVariant *value)
{
	GError *error = NULL;

	g_dbus_connection_emit_signal(fake.conn, NULL, MODEM_PATH, iface, "PropertyChanged",
								  g_variant_new("(sv)", name, value), &error);
	g_assert_no_error(error);
	g_dbus_connection_flush_sync(fake.conn, NULL, &error);
	g_assert_no_error(error);
}

static void ims_changed_cb(const gchar *name, void *data)
{
	g_assert_true(data == &seen);
	seen.ims_changes++;
}

static void bearer_changed_cb(const gchar *name, void *data)
{
	g_assert_true(data == &seen);
	seen.bearer_changes++;
	g_free(seen.last);
	seen.last = g_strdup(name);
}

static gboolean timed_out(gpointer data)
{
	*(gboolean *) data = TRUE;
	return G_SOURCE_REMOVE;
}

/* Runs the main loop until there were this many changes, or for a while */
static void wait_for_changes(int ims_changes, int bearer_changes)
{
	gboolean timeout = FALSE;
	guint id = g_timeout_add(3000, timed_out, &timeout);

	while ((seen.ims_changes < ims_changes || seen.bearer_changes < bearer_changes) && !timeout)
		g_main_context_iteration(NULL, TRUE);

	if (!timeout)
		g_source_remove(id);
	g_assert_cmpint(seen.ims_changes, ==, ims_changes);
	g_assert_cmpint(seen.bearer_changes, ==, bearer_changes);
}

/* Lets queued signals through without waiting for a change */
static void spin(void)
{
	gboolean timeout = FALSE;

	g_timeout_add(200, timed_out, &timeout);
	while (!timeout)
		g_main_context_iteration(NULL, TRUE);
}

struct modem {
	struct ofono_ims *ims;
	struct ofono_ims_bearer_info *info;
	struct telephony_network_status status;
};

/* Both interfaces, read once: three IMS properties and the bearer */
static void create_modem(struct modem *modem)
{
	memset(&seen, 0, sizeof(seen));
	memset(modem, 0, sizeof(*modem));

	modem->ims = ofono_ims_create(MODEM_PATH);
	g_assert_nonnull(modem->ims);
	ofono_ims_register_prop_changed_handler(modem->ims, ims_changed_cb, &seen);

	modem->info = ofono_ims_bearer_create(MODEM_PATH);
	g_assert_nonnull(modem->info);
	ofono_ims_bearer_register_prop_changed_handler(modem->info, bearer_changed_cb, &seen);

	wait_for_changes(3, 1);
	g_assert_cmpstr(seen.last, ==, "Bearer");
}

static void fill(struct modem *modem)
{
	/* What is not set stays unlike anything that could be */
	memset(&modem->status, 0xa5, sizeof(modem->status));
	ofono_ims_fill_network_status(modem->ims, modem->info, &modem->status);
}

static void free_modem(struct modem *modem)
{
	ofono_ims_bearer_free(modem->info);
	ofono_ims_free(modem->ims);
	g_free(seen.last);
	seen.last = NULL;
}

static void test_over_wifi(void)
{
	struct modem modem;

	fake.registered = TRUE;
	fake.voice_capable = TRUE;
	fake.bearer = "wifi";
	create_modem(&modem);

	g_assert_cmpint(ofono_ims_bearer_get(modem.info), ==, OFONO_IMS_BEARER_WIFI);
	fill(&modem);
	g_assert_true(modem.status.volte);
	g_assert_true(modem.status.vowifi);
	g_assert_true(modem.status.ims_registered);
	g_assert_true(modem.status.ims_voice_capable);
	g_assert_true(modem.status.ims_sms_capable);
	g_assert_cmpstr(modem.status.ims_bearer, ==, "wifi");
	/* not reported, so not to be shown */
	g_assert_null(ofono_ims_bearer_get_wfc_preference(modem.info));
	g_assert_null(modem.status.ims_wfc_preference);

	free_modem(&modem);
}

/* The Brax3's: Wi-Fi preferred, while on LTE; then changed in its settings */
static void test_wfc_preference(void)
{
	struct modem modem;

	fake.registered = TRUE;
	fake.voice_capable = TRUE;
	fake.bearer = "cellular";
	fake.wfc_preference = "wifi";
	memset(&seen, 0, sizeof(seen));
	memset(&modem, 0, sizeof(modem));
	modem.ims = ofono_ims_create(MODEM_PATH);
	ofono_ims_register_prop_changed_handler(modem.ims, ims_changed_cb, &seen);
	modem.info = ofono_ims_bearer_create(MODEM_PATH);
	ofono_ims_bearer_register_prop_changed_handler(modem.info, bearer_changed_cb, &seen);
	wait_for_changes(3, 2);
	g_assert_cmpstr(seen.last, ==, "WfcPreference");

	g_assert_cmpstr(ofono_ims_bearer_get_wfc_preference(modem.info), ==, "wifi");
	fill(&modem);
	g_assert_cmpstr(modem.status.ims_wfc_preference, ==, "wifi");
	g_assert_cmpstr(modem.status.ims_bearer, ==, "cellular");
	g_assert_false(modem.status.vowifi);

	fake_change(BEARER_IFACE, "WfcPreference", g_variant_new_string("wifi-only"));
	wait_for_changes(3, 3);
	fill(&modem);
	g_assert_cmpstr(modem.status.ims_wfc_preference, ==, "wifi-only");

	fake_change(BEARER_IFACE, "WfcPreference", g_variant_new_string("cellular"));
	wait_for_changes(3, 4);
	g_assert_cmpstr(ofono_ims_bearer_get_wfc_preference(modem.info), ==, "cellular");

	/* unknown to the extension, or a name telephonyd does not know: "" */
	fake_change(BEARER_IFACE, "WfcPreference", g_variant_new_string(""));
	wait_for_changes(3, 5);
	g_assert_cmpstr(ofono_ims_bearer_get_wfc_preference(modem.info), ==, "");
	fake_change(BEARER_IFACE, "WfcPreference", g_variant_new_string("wifi"));
	wait_for_changes(3, 6);
	fake_change(BEARER_IFACE, "WfcPreference", g_variant_new_string("roaming-wifi"));
	wait_for_changes(3, 7);
	g_assert_cmpstr(ofono_ims_bearer_get_wfc_preference(modem.info), ==, "");

	/* not a string: kept */
	fake_change(BEARER_IFACE, "WfcPreference", g_variant_new_string("wifi"));
	wait_for_changes(3, 8);
	fake_change(BEARER_IFACE, "WfcPreference", g_variant_new_uint32(1));
	spin();
	g_assert_cmpint(seen.bearer_changes, ==, 8);
	g_assert_cmpstr(ofono_ims_bearer_get_wfc_preference(modem.info), ==, "wifi");

	/* A setting: shown without an IMS registration too */
	fake_change(IMS_IFACE, "Registered", g_variant_new_boolean(FALSE));
	wait_for_changes(4, 8);
	fill(&modem);
	g_assert_false(modem.status.ims_registered);
	g_assert_cmpstr(modem.status.ims_bearer, ==, "");
	g_assert_cmpstr(modem.status.ims_wfc_preference, ==, "wifi");

	fake.wfc_preference = NULL;
	free_modem(&modem);
}

static void test_over_cellular(void)
{
	struct modem modem;

	fake.registered = TRUE;
	fake.voice_capable = TRUE;
	fake.bearer = "cellular";
	create_modem(&modem);

	g_assert_cmpint(ofono_ims_bearer_get(modem.info), ==, OFONO_IMS_BEARER_CELLULAR);
	fill(&modem);
	g_assert_true(modem.status.volte);
	g_assert_false(modem.status.vowifi);
	g_assert_cmpstr(modem.status.ims_bearer, ==, "cellular");

	free_modem(&modem);
}

/* SMS over IMS on Wi-Fi without voice: no Wi-Fi calling to show */
static void test_wifi_without_voice(void)
{
	struct modem modem;

	fake.registered = TRUE;
	fake.voice_capable = FALSE;
	fake.bearer = "wifi";
	create_modem(&modem);

	fill(&modem);
	g_assert_false(modem.status.volte);
	g_assert_false(modem.status.vowifi);
	g_assert_true(modem.status.ims_registered);
	g_assert_cmpstr(modem.status.ims_bearer, ==, "wifi");

	free_modem(&modem);
}

/* A bearer the extension still reports after ofono dropped the registration */
static void test_bearer_without_registration(void)
{
	struct modem modem;

	fake.registered = FALSE;
	fake.voice_capable = TRUE;
	fake.bearer = "wifi";
	create_modem(&modem);

	g_assert_cmpint(ofono_ims_bearer_get(modem.info), ==, OFONO_IMS_BEARER_WIFI);
	fill(&modem);
	g_assert_false(modem.status.volte);
	g_assert_false(modem.status.vowifi);
	g_assert_false(modem.status.ims_registered);
	g_assert_cmpstr(modem.status.ims_bearer, ==, "");

	free_modem(&modem);
}

/* Walking out of Wi-Fi coverage onto LTE, and losing IMS */
static void test_changes_follow(void)
{
	struct modem modem;

	fake.registered = TRUE;
	fake.voice_capable = TRUE;
	fake.bearer = "";
	create_modem(&modem);

	g_assert_cmpint(ofono_ims_bearer_get(modem.info), ==, OFONO_IMS_BEARER_NONE);
	fill(&modem);
	g_assert_true(modem.status.volte);
	g_assert_false(modem.status.vowifi);
	g_assert_cmpstr(modem.status.ims_bearer, ==, "");

	fake_change(BEARER_IFACE, "Bearer", g_variant_new_string("wifi"));
	wait_for_changes(3, 2);
	fill(&modem);
	g_assert_true(modem.status.vowifi);

	fake_change(BEARER_IFACE, "Bearer", g_variant_new_string("cellular"));
	wait_for_changes(3, 3);
	fill(&modem);
	g_assert_true(modem.status.volte);
	g_assert_false(modem.status.vowifi);
	g_assert_cmpstr(modem.status.ims_bearer, ==, "cellular");

	fake_change(BEARER_IFACE, "Bearer", g_variant_new_string(""));
	wait_for_changes(3, 4);
	g_assert_cmpint(ofono_ims_bearer_get(modem.info), ==, OFONO_IMS_BEARER_NONE);

	free_modem(&modem);
}

static void test_unknown_and_ignored(void)
{
	struct modem modem;

	fake.registered = TRUE;
	fake.voice_capable = TRUE;
	fake.bearer = "wifi";
	create_modem(&modem);

	/* Neither ours to read nor of the right type */
	fake_change(BEARER_IFACE, "Technology", g_variant_new_string("iwlan"));
	fake_change(BEARER_IFACE, "Bearer", g_variant_new_boolean(FALSE));
	spin();
	g_assert_cmpint(seen.bearer_changes, ==, 1);
	g_assert_cmpint(ofono_ims_bearer_get(modem.info), ==, OFONO_IMS_BEARER_WIFI);

	/* A bearer a later extension may name is none of the two */
	fake_change(BEARER_IFACE, "Bearer", g_variant_new_string("satellite"));
	wait_for_changes(3, 2);
	g_assert_cmpint(ofono_ims_bearer_get(modem.info), ==, OFONO_IMS_BEARER_NONE);
	fill(&modem);
	g_assert_true(modem.status.volte);
	g_assert_false(modem.status.vowifi);
	g_assert_cmpstr(modem.status.ims_bearer, ==, "");

	free_modem(&modem);
}

/* Stock ofono: VoLTE as before, never VoWiFi */
static void test_without_extension(void)
{
	struct modem modem;

	fake.registered = TRUE;
	fake.voice_capable = TRUE;
	fake.bearer = "wifi";
	create_modem(&modem);
	ofono_ims_bearer_free(modem.info);
	modem.info = NULL;

	fill(&modem);
	g_assert_true(modem.status.volte);
	g_assert_false(modem.status.vowifi);
	g_assert_cmpstr(modem.status.ims_bearer, ==, "");

	free_modem(&modem);
}

static void test_without_ims(void)
{
	struct telephony_network_status status;

	memset(&status, 0xa5, sizeof(status));
	ofono_ims_fill_network_status(NULL, NULL, &status);
	g_assert_false(status.volte);
	g_assert_false(status.vowifi);
	g_assert_false(status.ims_registered);
	g_assert_false(status.ims_voice_capable);
	g_assert_false(status.ims_sms_capable);
	g_assert_cmpstr(status.ims_bearer, ==, "");
	g_assert_null(status.ims_wfc_preference);

	g_assert_cmpint(ofono_ims_bearer_get(NULL), ==, OFONO_IMS_BEARER_NONE);
	g_assert_null(ofono_ims_bearer_get_wfc_preference(NULL));
	ofono_ims_bearer_register_prop_changed_handler(NULL, bearer_changed_cb, &seen);
	ofono_ims_bearer_free(NULL);
}

static void test_to_string(void)
{
	g_assert_cmpstr(ofono_ims_bearer_to_string(OFONO_IMS_BEARER_WIFI), ==, "wifi");
	g_assert_cmpstr(ofono_ims_bearer_to_string(OFONO_IMS_BEARER_CELLULAR), ==, "cellular");
	g_assert_cmpstr(ofono_ims_bearer_to_string(OFONO_IMS_BEARER_NONE), ==, "");
}

int main(int argc, char **argv)
{
	GTestDBus *bus;
	GDBusNodeInfo *node;
	GError *error = NULL;
	GVariant *result;
	guint object_ids[2];
	unsigned int i;
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
	for (i = 0; i < G_N_ELEMENTS(object_ids); i++) {
		object_ids[i] = g_dbus_connection_register_object(fake.conn, MODEM_PATH, node->interfaces[i],
														  &fake_vtable, NULL, NULL, &error);
		g_assert_no_error(error);
	}

	result = g_dbus_connection_call_sync(fake.conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
										 "org.freedesktop.DBus", "RequestName",
										 g_variant_new("(su)", "org.ofono", 0x4), G_VARIANT_TYPE("(u)"),
										 G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
	g_assert_no_error(error);
	g_variant_unref(result);

	g_test_add_func("/ofonoimsbearer/over-wifi", test_over_wifi);
	g_test_add_func("/ofonoimsbearer/over-cellular", test_over_cellular);
	g_test_add_func("/ofonoimsbearer/wfc-preference", test_wfc_preference);
	g_test_add_func("/ofonoimsbearer/wifi-without-voice", test_wifi_without_voice);
	g_test_add_func("/ofonoimsbearer/bearer-without-registration", test_bearer_without_registration);
	g_test_add_func("/ofonoimsbearer/changes-follow", test_changes_follow);
	g_test_add_func("/ofonoimsbearer/unknown-and-ignored", test_unknown_and_ignored);
	g_test_add_func("/ofonoimsbearer/without-extension", test_without_extension);
	g_test_add_func("/ofonoimsbearer/without-ims", test_without_ims);
	g_test_add_func("/ofonoimsbearer/to-string", test_to_string);

	ret = g_test_run();

	for (i = 0; i < G_N_ELEMENTS(object_ids); i++)
		g_dbus_connection_unregister_object(fake.conn, object_ids[i]);
	g_dbus_node_info_unref(node);
	g_object_unref(fake.conn);
	g_test_dbus_down(bus);
	g_object_unref(bus);

	return ret;
}

// vim:ts=4:sw=4:noexpandtab
