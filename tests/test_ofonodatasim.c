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

/*
 * Handing the data SIM to ofono, against a fake slot manager on a private bus.
 *
 * The fake implements the part of org.nemomobile.ofono.ModemManager on "/"
 * that ofono-ext's slot-manager-dbus.c offers and we use: GetDefaultDataSim
 * and SetDefaultDataSim with their signatures, "auto" when nothing is set,
 * and DefaultDataSimChanged after a change.
 */

#include <string.h>

#include <glib.h>
#include <gio/gio.h>

#include "ofonodatasim.h"

#define SM_PATH "/"
#define SM_IFACE "org.nemomobile.ofono.ModemManager"

static const gchar fake_sm_xml[] =
	"<node>"
	"  <interface name='org.nemomobile.ofono.ModemManager'>"
	"    <method name='GetDefaultDataSim'><arg type='s' direction='out'/></method>"
	"    <method name='SetDefaultDataSim'><arg type='s' direction='in'/></method>"
	"    <signal name='DefaultDataSimChanged'><arg type='s'/></signal>"
	"  </interface>"
	"</node>";

struct fake_sm {
	GDBusConnection *conn;
	GDBusInterfaceInfo *iface;
	guint object_id;

	gchar *data_sim;
	int get_calls;
	int set_calls;
	gchar *last_set;
	/* hold the replies to these until released */
	gboolean hold_get;
	gboolean hold_set;
	GPtrArray *held;
	gboolean fail_set;
	/* ofono without ofono-ext: "/" is there, the interface is not */
	gboolean missing;
	int missing_calls;
};

static struct fake_sm fake;

static void fake_reply_get(GDBusMethodInvocation *invocation)
{
	g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", fake.data_sim));
}

static void fake_reply_set(GDBusMethodInvocation *invocation)
{
	GVariant *params = g_dbus_method_invocation_get_parameters(invocation);
	const gchar *imsi;

	if (fake.fail_set) {
		g_dbus_method_invocation_return_dbus_error(invocation, "org.ofono.Error.Failed", "Operation failed");
		return;
	}

	g_variant_get(params, "(&s)", &imsi);
	if (g_strcmp0(fake.data_sim, imsi) != 0) {
		g_free(fake.data_sim);
		fake.data_sim = g_strdup(imsi);
		g_dbus_connection_emit_signal(fake.conn, NULL, SM_PATH, SM_IFACE, "DefaultDataSimChanged",
									  g_variant_new("(s)", imsi), NULL);
	}
	g_dbus_method_invocation_return_value(invocation, NULL);
}

static void fake_method_call(GDBusConnection *conn, const gchar *sender, const gchar *path,
                             const gchar *iface, const gchar *method, GVariant *params,
                             GDBusMethodInvocation *invocation, gpointer user_data)
{
	if (fake.missing) {
		fake.missing_calls++;
		g_dbus_method_invocation_return_dbus_error(invocation, "org.freedesktop.DBus.Error.UnknownMethod",
												   "Method not found");
		return;
	}

	if (g_str_equal(method, "GetDefaultDataSim")) {
		fake.get_calls++;
		if (fake.hold_get)
			g_ptr_array_add(fake.held, invocation);
		else
			fake_reply_get(invocation);
	}
	else if (g_str_equal(method, "SetDefaultDataSim")) {
		const gchar *imsi;

		fake.set_calls++;
		g_variant_get(params, "(&s)", &imsi);
		g_free(fake.last_set);
		fake.last_set = g_strdup(imsi);
		if (fake.hold_set)
			g_ptr_array_add(fake.held, invocation);
		else
			fake_reply_set(invocation);
	}
}

static const GDBusInterfaceVTable fake_vtable = { fake_method_call, NULL, NULL, { 0 } };

static void fake_release_held(void)
{
	guint n;

	for (n = 0; n < fake.held->len; n++) {
		GDBusMethodInvocation *invocation = g_ptr_array_index(fake.held, n);

		if (g_str_equal(g_dbus_method_invocation_get_method_name(invocation), "GetDefaultDataSim"))
			fake_reply_get(invocation);
		else
			fake_reply_set(invocation);
	}
	g_ptr_array_set_size(fake.held, 0);
}

static void fake_register(void)
{
	GError *error = NULL;

	fake.object_id = g_dbus_connection_register_object(fake.conn, SM_PATH, fake.iface, &fake_vtable,
													   NULL, NULL, &error);
	g_assert_no_error(error);
}

static void fake_unregister(void)
{
	if (fake.object_id)
		g_dbus_connection_unregister_object(fake.conn, fake.object_id);
	fake.object_id = 0;
}

static void fake_reset(const char *data_sim)
{
	g_free(fake.data_sim);
	fake.data_sim = g_strdup(data_sim);
	g_free(fake.last_set);
	fake.last_set = NULL;
	fake.get_calls = fake.set_calls = 0;
	fake.hold_get = fake.hold_set = fake.fail_set = fake.missing = FALSE;
	fake.missing_calls = 0;
	g_ptr_array_set_size(fake.held, 0);
	if (!fake.object_id)
		fake_register();
}

/* ------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------ */

static gboolean deadline_cb(gpointer data)
{
	*(gboolean *) data = TRUE;
	return G_SOURCE_REMOVE;
}

#define RUN_UNTIL(cond) \
	do { \
		gboolean expired_ = FALSE; \
		guint src_ = g_timeout_add(5000, deadline_cb, &expired_); \
		while (!(cond) && !expired_) \
			g_main_context_iteration(NULL, TRUE); \
		if (!expired_) \
			g_source_remove(src_); \
		g_assert_true(cond); \
	} while (0)

static void spin(guint ms)
{
	gboolean expired = FALSE;

	g_timeout_add(ms, deadline_cb, &expired);
	while (!expired)
		g_main_context_iteration(NULL, TRUE);
}

/* ------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------ */

/* Nothing chosen in ofono yet ("auto"): our choice is sent, once. */
static void test_sets_when_auto(void)
{
	struct ofono_data_sim *ds;

	fake_reset("auto");
	ds = ofono_data_sim_create();
	RUN_UNTIL(ofono_data_sim_get_current(ds) != NULL);
	g_assert_cmpstr(ofono_data_sim_get_current(ds), ==, "auto");

	ofono_data_sim_set_imsi(ds, "204040000000001");
	RUN_UNTIL(fake.set_calls == 1 && g_strcmp0(ofono_data_sim_get_current(ds), "204040000000001") == 0);
	g_assert_cmpstr(fake.data_sim, ==, "204040000000001");

	/* the same again changes nothing */
	ofono_data_sim_set_imsi(ds, "204040000000001");
	spin(50);
	g_assert_cmpint(fake.set_calls, ==, 1);
	g_assert_true(ofono_data_sim_is_supported(ds));

	ofono_data_sim_free(ds);
}

/* ofono already has the SIM we want (stored from before): no call. */
static void test_no_call_when_already_set(void)
{
	struct ofono_data_sim *ds;

	fake_reset("204040000000001");
	ds = ofono_data_sim_create();
	ofono_data_sim_set_imsi(ds, "204040000000001");
	RUN_UNTIL(ofono_data_sim_get_current(ds) != NULL);
	spin(50);
	g_assert_cmpint(fake.set_calls, ==, 0);

	ofono_data_sim_free(ds);
}

/* A choice made before ofono's setting is known waits for it. */
static void test_waits_for_current(void)
{
	struct ofono_data_sim *ds;

	fake_reset("auto");
	fake.hold_get = TRUE;
	ds = ofono_data_sim_create();
	RUN_UNTIL(fake.get_calls == 1 && fake.held->len == 1);

	ofono_data_sim_set_imsi(ds, "204040000000002");
	spin(50);
	g_assert_cmpint(fake.set_calls, ==, 0);

	fake_release_held();
	RUN_UNTIL(fake.set_calls == 1);
	RUN_UNTIL(g_strcmp0(ofono_data_sim_get_current(ds), "204040000000002") == 0);
	g_assert_cmpstr(fake.last_set, ==, "204040000000002");

	ofono_data_sim_free(ds);
}

/* A NULL or empty IMSI (SIM not read yet) is not sent and keeps the choice. */
static void test_unknown_imsi_ignored(void)
{
	struct ofono_data_sim *ds;

	fake_reset("auto");
	ds = ofono_data_sim_create();
	RUN_UNTIL(ofono_data_sim_get_current(ds) != NULL);
	ofono_data_sim_set_imsi(ds, NULL);
	ofono_data_sim_set_imsi(ds, "");
	spin(50);
	g_assert_cmpint(fake.set_calls, ==, 0);

	ofono_data_sim_set_imsi(ds, "204040000000003");
	ofono_data_sim_set_imsi(ds, NULL);
	RUN_UNTIL(fake.set_calls == 1);
	spin(50);
	g_assert_cmpint(fake.set_calls, ==, 1);
	g_assert_cmpstr(fake.data_sim, ==, "204040000000003");

	ofono_data_sim_free(ds);
}

/* The choice changes while a call is out: one call at a time, the last wins. */
static void test_change_while_sending(void)
{
	struct ofono_data_sim *ds;

	fake_reset("auto");
	ds = ofono_data_sim_create();
	RUN_UNTIL(ofono_data_sim_get_current(ds) != NULL);

	fake.hold_set = TRUE;
	ofono_data_sim_set_imsi(ds, "204040000000001");
	RUN_UNTIL(fake.set_calls == 1 && fake.held->len == 1);
	ofono_data_sim_set_imsi(ds, "204040000000002");
	ofono_data_sim_set_imsi(ds, "204040000000003");
	spin(50);
	g_assert_cmpint(fake.set_calls, ==, 1);

	fake.hold_set = FALSE;
	fake_release_held();
	RUN_UNTIL(fake.set_calls == 2 && g_strcmp0(ofono_data_sim_get_current(ds), "204040000000003") == 0);
	g_assert_cmpstr(fake.data_sim, ==, "204040000000003");
	spin(50);
	g_assert_cmpint(fake.set_calls, ==, 2);

	ofono_data_sim_free(ds);
}

/* Changes made by someone else are followed, not fought. */
static void test_follows_signal(void)
{
	struct ofono_data_sim *ds;

	fake_reset("auto");
	ds = ofono_data_sim_create();
	ofono_data_sim_set_imsi(ds, "204040000000001");
	RUN_UNTIL(g_strcmp0(ofono_data_sim_get_current(ds), "204040000000001") == 0);

	g_dbus_connection_emit_signal(fake.conn, NULL, SM_PATH, SM_IFACE, "DefaultDataSimChanged",
								  g_variant_new("(s)", "204040000000009"), NULL);
	RUN_UNTIL(g_strcmp0(ofono_data_sim_get_current(ds), "204040000000009") == 0);
	spin(50);
	g_assert_cmpint(fake.set_calls, ==, 1);

	/* a signal with the wrong signature is ignored */
	g_dbus_connection_emit_signal(fake.conn, NULL, SM_PATH, SM_IFACE, "DefaultDataSimChanged",
								  g_variant_new("(i)", 4), NULL);
	spin(50);
	g_assert_cmpstr(ofono_data_sim_get_current(ds), ==, "204040000000009");

	ofono_data_sim_free(ds);
}

/* A failed call is reported and not retried in a loop. */
static void test_failed_set_not_retried(void)
{
	struct ofono_data_sim *ds;

	fake_reset("auto");
	fake.fail_set = TRUE;
	ds = ofono_data_sim_create();
	ofono_data_sim_set_imsi(ds, "204040000000001");
	RUN_UNTIL(fake.set_calls == 1);
	spin(100);
	g_assert_cmpint(fake.set_calls, ==, 1);
	g_assert_cmpstr(ofono_data_sim_get_current(ds), ==, "auto");
	g_assert_true(ofono_data_sim_is_supported(ds));

	/* a new choice is still sent */
	fake.fail_set = FALSE;
	ofono_data_sim_set_imsi(ds, "204040000000002");
	RUN_UNTIL(fake.set_calls == 2);
	RUN_UNTIL(g_strcmp0(ofono_data_sim_get_current(ds), "204040000000002") == 0);

	ofono_data_sim_free(ds);
}

/* ofono without ofono-ext: noted after the first call, no further calls. */
static void test_no_slot_manager(void)
{
	struct ofono_data_sim *ds;

	fake_reset("auto");
	fake.missing = TRUE;
	ds = ofono_data_sim_create();
	ofono_data_sim_set_imsi(ds, "204040000000001");
	RUN_UNTIL(!ofono_data_sim_is_supported(ds));
	spin(50);
	g_assert_cmpint(fake.missing_calls, ==, 1);
	g_assert_null(ofono_data_sim_get_current(ds));

	ofono_data_sim_set_imsi(ds, "204040000000002");
	spin(50);
	g_assert_cmpint(fake.missing_calls, ==, 1);

	ofono_data_sim_free(ds);

	/* no object at "/" at all */
	fake_reset("auto");
	fake_unregister();
	ds = ofono_data_sim_create();
	RUN_UNTIL(!ofono_data_sim_is_supported(ds));
	ofono_data_sim_free(ds);
	fake_register();
}

/* Freed with calls outstanding (ofono went away): the replies find nothing. */
static void test_free_with_pending(void)
{
	struct ofono_data_sim *ds;

	fake_reset("auto");
	fake.hold_get = TRUE;
	ds = ofono_data_sim_create();
	RUN_UNTIL(fake.held->len == 1);
	ofono_data_sim_free(ds);
	fake_release_held();
	spin(50);

	fake_reset("auto");
	ds = ofono_data_sim_create();
	RUN_UNTIL(ofono_data_sim_get_current(ds) != NULL);
	fake.hold_set = TRUE;
	ofono_data_sim_set_imsi(ds, "204040000000001");
	RUN_UNTIL(fake.held->len == 1);
	ofono_data_sim_free(ds);
	fake_release_held();
	/* the DefaultDataSimChanged signal must not reach the freed object either */
	spin(50);

	/* NULL is fine everywhere */
	ofono_data_sim_free(NULL);
	ofono_data_sim_set_imsi(NULL, "1");
	g_assert_null(ofono_data_sim_get_current(NULL));
	g_assert_false(ofono_data_sim_is_supported(NULL));
}

int main(int argc, char **argv)
{
	GTestDBus *bus;
	GDBusNodeInfo *node;
	GError *error = NULL;
	GVariant *result;
	int ret;

	g_test_init(&argc, &argv, NULL);
	/* the failure paths log warnings on purpose; criticals stay fatal */
	g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);

	bus = g_test_dbus_new(G_TEST_DBUS_NONE);
	g_test_dbus_up(bus);
	/* the code under test uses G_BUS_TYPE_SYSTEM */
	g_setenv("DBUS_SYSTEM_BUS_ADDRESS", g_test_dbus_get_bus_address(bus), TRUE);

	fake.conn = g_dbus_connection_new_for_address_sync(g_test_dbus_get_bus_address(bus),
		G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
		NULL, NULL, &error);
	g_assert_no_error(error);
	fake.held = g_ptr_array_new();

	node = g_dbus_node_info_new_for_xml(fake_sm_xml, &error);
	g_assert_no_error(error);
	fake.iface = g_dbus_interface_info_ref(node->interfaces[0]);
	g_dbus_node_info_unref(node);

	result = g_dbus_connection_call_sync(fake.conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
										 "org.freedesktop.DBus", "RequestName",
										 g_variant_new("(su)", "org.ofono", 0x4), G_VARIANT_TYPE("(u)"),
										 G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
	g_assert_no_error(error);
	g_variant_unref(result);

	g_test_add_func("/ofonodatasim/sets-when-auto", test_sets_when_auto);
	g_test_add_func("/ofonodatasim/no-call-when-already-set", test_no_call_when_already_set);
	g_test_add_func("/ofonodatasim/waits-for-current", test_waits_for_current);
	g_test_add_func("/ofonodatasim/unknown-imsi-ignored", test_unknown_imsi_ignored);
	g_test_add_func("/ofonodatasim/change-while-sending", test_change_while_sending);
	g_test_add_func("/ofonodatasim/follows-signal", test_follows_signal);
	g_test_add_func("/ofonodatasim/failed-set-not-retried", test_failed_set_not_retried);
	g_test_add_func("/ofonodatasim/no-slot-manager", test_no_slot_manager);
	g_test_add_func("/ofonodatasim/free-with-pending", test_free_with_pending);

	ret = g_test_run();

	fake_unregister();
	g_dbus_interface_info_unref(fake.iface);
	g_ptr_array_free(fake.held, TRUE);
	g_free(fake.data_sim);
	g_free(fake.last_set);
	g_dbus_connection_close_sync(fake.conn, NULL, NULL);
	g_object_unref(fake.conn);
	g_test_dbus_down(bus);
	g_object_unref(bus);

	return ret;
}

// vim:ts=4:sw=4:noexpandtab
