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
 * The ofono WAN driver following the data SIM between the modems of a fake
 * ofono on a private bus.
 *
 * The fake has two modems, /ril_0 on LTE and /ril_1 on NR, each with a
 * Modem, a ConnectionManager and a NetworkRegistration interface. The
 * status get_status gives carries the data modem's technology, so it says
 * which modem the driver is following, and that it has both of that
 * modem's interfaces: the status needs the ConnectionManager, the
 * technology the NetworkRegistration. It runs on a thread of its own, as
 * ofono is a process of its own: the proxies are created with blocking
 * calls.
 *
 * The luna side (wanservice.c) is replaced by the three functions the
 * driver calls on it.
 */

#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <gio/gio.h>

#include "wanservice.h"
#include "wandriver.h"

/* the driver's entry points, from drivers/ofono/wan.c */
int ofono_wan_probe(struct wan_service *service);
void ofono_wan_remove(struct wan_service *service);
void ofono_wan_get_status(struct wan_service *service, wan_get_status_cb cb, void *data);
void ofono_wan_set_data_sim(struct wan_service *service, int sim_id, const char *imsi);

static const gchar fake_ofono_xml[] =
	"<node>"
	"  <interface name='org.ofono.Manager'>"
	"    <method name='GetModems'><arg type='a(oa{sv})' direction='out'/></method>"
	"    <signal name='ModemAdded'><arg type='o'/><arg type='a{sv}'/></signal>"
	"    <signal name='ModemRemoved'><arg type='o'/></signal>"
	"  </interface>"
	"  <interface name='org.ofono.Modem'>"
	"    <method name='GetProperties'><arg type='a{sv}' direction='out'/></method>"
	"    <method name='SetProperty'><arg type='s' direction='in'/><arg type='v' direction='in'/></method>"
	"    <signal name='PropertyChanged'><arg type='s'/><arg type='v'/></signal>"
	"  </interface>"
	"  <interface name='org.ofono.ConnectionManager'>"
	"    <method name='GetProperties'><arg type='a{sv}' direction='out'/></method>"
	"    <method name='SetProperty'><arg type='s' direction='in'/><arg type='v' direction='in'/></method>"
	"    <method name='GetContexts'><arg type='a(oa{sv})' direction='out'/></method>"
	"    <signal name='PropertyChanged'><arg type='s'/><arg type='v'/></signal>"
	"    <signal name='ContextAdded'><arg type='o'/><arg type='a{sv}'/></signal>"
	"    <signal name='ContextRemoved'><arg type='o'/></signal>"
	"  </interface>"
	"  <interface name='org.ofono.NetworkRegistration'>"
	"    <method name='GetProperties'><arg type='a{sv}' direction='out'/></method>"
	"    <signal name='PropertyChanged'><arg type='s'/><arg type='v'/></signal>"
	"  </interface>"
	"</node>";

#define N_MODEMS 2

struct fake_ofono {
	GMutex lock;
	GThread *thread;
	GMainContext *context;
	GMainLoop *loop;
	GDBusConnection *conn;
	GDBusNodeInfo *node;
	GArray *objects;		/* guint registration ids */

	/* whether a modem lists org.ofono.ConnectionManager in Interfaces */
	gboolean has_cm[N_MODEMS];
	/* hold the next GetContexts until fake_fail_held() answers it */
	gboolean hold_get_contexts;
	GDBusMethodInvocation *held;
};

static struct fake_ofono fake;

/* n of /ril_<n>, or -1 for any other path */
static int modem_index(const gchar *path)
{
	const gchar *digits;
	gchar *end = NULL;
	long n;

	if (!g_str_has_prefix(path, "/ril_"))
		return -1;
	digits = path + sizeof("/ril_") - 1;
	n = strtol(digits, &end, 10);
	if (end == digits || *end != '\0' || n < 0 || n >= N_MODEMS)
		return -1;
	return (int) n;
}

static GVariant *modem_interfaces(int n)
{
	GVariantBuilder b;

	g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
	g_variant_builder_add(&b, "s", "org.ofono.NetworkRegistration");
	if (n >= 0 && n < N_MODEMS && fake.has_cm[n])
		g_variant_builder_add(&b, "s", "org.ofono.ConnectionManager");
	return g_variant_builder_end(&b);
}

static void fake_method_call(GDBusConnection *conn, const gchar *sender, const gchar *path,
                             const gchar *iface, const gchar *method, GVariant *params,
                             GDBusMethodInvocation *invocation, gpointer user_data)
{
	GVariantBuilder b;
	int n = modem_index(path);

	g_mutex_lock(&fake.lock);

	if (g_str_equal(iface, "org.ofono.Manager") && g_str_equal(method, "GetModems")) {
		int i;

		g_variant_builder_init(&b, G_VARIANT_TYPE("a(oa{sv})"));
		for (i = 0; i < N_MODEMS; i++) {
			gchar *modem = g_strdup_printf("/ril_%d", i);
			GVariantBuilder props;

			g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
			g_variant_builder_add(&b, "(oa{sv})", modem, &props);
			g_free(modem);
		}
		g_dbus_method_invocation_return_value(invocation, g_variant_new("(a(oa{sv}))", &b));
	}
	else if (g_str_equal(method, "GetProperties")) {
		g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
		if (g_str_equal(iface, "org.ofono.Modem")) {
			g_assert_true(n >= 0);
			g_variant_builder_add(&b, "{sv}", "Powered", g_variant_new_boolean(TRUE));
			g_variant_builder_add(&b, "{sv}", "Online", g_variant_new_boolean(TRUE));
			g_variant_builder_add(&b, "{sv}", "Interfaces", modem_interfaces(n));
		}
		else if (g_str_equal(iface, "org.ofono.ConnectionManager")) {
			g_variant_builder_add(&b, "{sv}", "Powered", g_variant_new_boolean(TRUE));
			g_variant_builder_add(&b, "{sv}", "Attached", g_variant_new_boolean(TRUE));
			g_variant_builder_add(&b, "{sv}", "RoamingAllowed", g_variant_new_boolean(FALSE));
		}
		else if (g_str_equal(iface, "org.ofono.NetworkRegistration")) {
			g_variant_builder_add(&b, "{sv}", "Status", g_variant_new_string("registered"));
			g_variant_builder_add(&b, "{sv}", "Technology", g_variant_new_string(n == 1 ? "nr" : "lte"));
		}
		g_dbus_method_invocation_return_value(invocation, g_variant_new("(a{sv})", &b));
	}
	else if (g_str_equal(method, "GetContexts") && fake.hold_get_contexts) {
		fake.hold_get_contexts = FALSE;
		fake.held = invocation;
	}
	else if (g_str_equal(method, "GetContexts")) {
		g_assert_true(n >= 0 && n < N_MODEMS);
		g_variant_builder_init(&b, G_VARIANT_TYPE("a(oa{sv})"));
		g_dbus_method_invocation_return_value(invocation, g_variant_new("(a(oa{sv}))", &b));
	}
	else if (g_str_equal(method, "SetProperty")) {
		g_dbus_method_invocation_return_value(invocation, NULL);
	}
	else {
		g_dbus_method_invocation_return_dbus_error(invocation, "org.freedesktop.DBus.Error.UnknownMethod", method);
	}

	g_mutex_unlock(&fake.lock);
}

static const GDBusInterfaceVTable fake_vtable = { fake_method_call, NULL, NULL, { 0 } };

static gpointer fake_thread(gpointer data)
{
	g_main_context_push_thread_default(fake.context);
	g_main_loop_run(fake.loop);
	g_main_context_pop_thread_default(fake.context);
	return NULL;
}

/* data: the modem's has_cm entry, which says which modem it is */
static gboolean emit_interfaces_idle(gpointer data)
{
	int n = (int) ((gboolean *) data - fake.has_cm);
	gchar *path = g_strdup_printf("/ril_%d", n);

	g_mutex_lock(&fake.lock);
	g_dbus_connection_emit_signal(fake.conn, NULL, path, "org.ofono.Modem", "PropertyChanged",
		g_variant_new("(sv)", "Interfaces", modem_interfaces(n)), NULL);
	g_mutex_unlock(&fake.lock);
	g_free(path);
	return G_SOURCE_REMOVE;
}

/* A modem gains its ConnectionManager, as ofono reports when it comes up. */
static void fake_add_connection_manager(int n)
{
	g_mutex_lock(&fake.lock);
	fake.has_cm[n] = TRUE;
	g_mutex_unlock(&fake.lock);
	g_main_context_invoke(fake.context, emit_interfaces_idle, &fake.has_cm[n]);
}

static gboolean fail_held_idle(gpointer data)
{
	g_mutex_lock(&fake.lock);
	g_dbus_method_invocation_return_dbus_error(fake.held, "org.ofono.Error.Failed", "Operation failed");
	fake.held = NULL;
	g_mutex_unlock(&fake.lock);
	return G_SOURCE_REMOVE;
}

static gboolean fake_has_held(void)
{
	gboolean held;

	g_mutex_lock(&fake.lock);
	held = fake.held != NULL;
	g_mutex_unlock(&fake.lock);
	return held;
}

/* Answer the held GetContexts with an error, as a cancelled one ends. */
static void fake_fail_held(void)
{
	g_main_context_invoke(fake.context, fail_held_idle, NULL);
}

static void fake_reset(gboolean cm0, gboolean cm1)
{
	g_mutex_lock(&fake.lock);
	fake.has_cm[0] = cm0;
	fake.has_cm[1] = cm1;
	g_mutex_unlock(&fake.lock);
}

/* --- what wanservice.c would be ------------------------------------------ */

struct wan_service {
	void *data;
	int notifies;
	enum wan_network_type last_type;
	bool last_attached;
};

void wan_service_set_data(struct wan_service *service, void *data)
{
	service->data = data;
}

void *wan_service_get_data(struct wan_service *service)
{
	return service->data;
}

void wan_service_status_changed_notify(struct wan_service *service, struct wan_status *status)
{
	service->notifies++;
	service->last_type = status->network_type;
	service->last_attached = status->network_attached;
}

/* --- helpers ------------------------------------------------------------- */

static void spin(guint ms)
{
	gint64 end = g_get_monotonic_time() + (gint64) ms * 1000;

	while (g_get_monotonic_time() < end)
		g_main_context_iteration(NULL, FALSE);
}

#define RUN_UNTIL(cond) do { \
	gint64 _end = g_get_monotonic_time() + (gint64) 3 * G_USEC_PER_SEC; \
	while (!(cond) && g_get_monotonic_time() < _end) \
		g_main_context_iteration(NULL, FALSE); \
	g_assert_true(cond); \
} while (0)

struct status_result {
	gboolean done;
	gboolean ok;
	int error_code;
	enum wan_network_type network_type;
	gboolean attached;
};

static void status_cb(const struct wan_error *error, struct wan_status *status, void *data)
{
	struct status_result *r = data;

	r->done = TRUE;
	r->ok = (error == NULL && status != NULL);
	r->error_code = error ? error->code : -1;
	if (status) {
		r->network_type = status->network_type;
		r->attached = status->network_attached;
	}
}

static enum wan_network_type technology_of(int n)
{
	return n == 1 ? WAN_NETWORK_TYPE_NR : WAN_NETWORK_TYPE_LTE;
}

/*
 * What a getstatus answers once the driver has the data modem's properties:
 * they come asynchronously, after ofono is found, after a modem is chosen.
 * Modem n's status, then: attached, with its technology.
 */
static void assert_status_from(struct wan_service *service, int n)
{
	gint64 end = g_get_monotonic_time() + (gint64) 3 * G_USEC_PER_SEC;
	struct status_result r = { 0 };

	while (g_get_monotonic_time() < end) {
		memset(&r, 0, sizeof(r));
		ofono_wan_get_status(service, status_cb, &r);
		RUN_UNTIL(r.done);
		if (r.ok && r.attached && r.network_type == technology_of(n))
			break;
		spin(20);
	}
	g_assert_true(r.ok);
	g_assert_true(r.attached);
	g_assert_cmpint(r.network_type, ==, technology_of(n));
}

/* --- tests --------------------------------------------------------------- */

/*
 * Packet data moved to a modem that was already known, the other SIM's: its
 * interfaces were reported before and report no change, and the driver
 * still has to set up its ConnectionManager. It used to wait for a change
 * that never came, and getstatus said "Not available" from then on.
 */
static void test_switch_to_known_modem(void)
{
	struct wan_service service = { 0 };

	fake_reset(TRUE, TRUE);
	g_assert_cmpint(ofono_wan_probe(&service), ==, 0);
	assert_status_from(&service, 0);

	/* both modems' properties are in by now */
	spin(100);

	ofono_wan_set_data_sim(&service, 1, NULL);
	assert_status_from(&service, 1);

	/* and back, as telephonyd does at start-up */
	ofono_wan_set_data_sim(&service, 0, NULL);
	assert_status_from(&service, 0);

	/* the same SIM again changes nothing */
	ofono_wan_set_data_sim(&service, 0, NULL);
	assert_status_from(&service, 0);

	ofono_wan_remove(&service);
	g_assert_null(service.data);
	spin(50);
}

/*
 * The driver removed while a status request is out, as when telephonyd
 * stops just after switching packet data: ofono answers the request when
 * the ConnectionManager goes, cancelled, after the driver's data is gone.
 * That answer wrote to the freed data (seen under valgrind); ASan and
 * valgrind tell here.
 */
static void test_remove_while_asking(void)
{
	struct wan_service service = { 0 };
	int round;

	fake_reset(TRUE, TRUE);
	g_assert_cmpint(ofono_wan_probe(&service), ==, 0);
	assert_status_from(&service, 0);
	spin(100);

	for (round = 0; round < 3; round++) {
		struct wan_service again = { 0 };

		/* switching packet data asks for the status again */
		g_assert_cmpint(ofono_wan_probe(&again), ==, 0);
		assert_status_from(&again, 0);
		spin(50);
		ofono_wan_set_data_sim(&again, 1, NULL);
		ofono_wan_remove(&again);
		g_assert_null(again.data);
		spin(100);
	}

	ofono_wan_remove(&service);
	spin(50);
}

/*
 * A data modem without a ConnectionManager yet has no status to give;
 * once ofono reports the interface the driver picks it up.
 */
static void test_connection_manager_appears(void)
{
	struct wan_service service = { 0 };
	struct status_result r = { 0 };

	fake_reset(TRUE, FALSE);
	g_assert_cmpint(ofono_wan_probe(&service), ==, 0);
	assert_status_from(&service, 0);
	spin(100);

	ofono_wan_set_data_sim(&service, 1, NULL);
	ofono_wan_get_status(&service, status_cb, &r);
	RUN_UNTIL(r.done);
	g_assert_false(r.ok);
	g_assert_cmpint(r.error_code, ==, WAN_ERROR_NOT_AVAILABLE);

	fake_add_connection_manager(1);
	assert_status_from(&service, 1);

	ofono_wan_remove(&service);
	spin(50);
}

/*
 * Subscribers hear about a change that came while a status request was
 * still out, even when that request then fails. Coming up, the
 * ConnectionManager's properties arrive one by one: the first asks for a
 * status, the others came while it was out and were dropped. When that one
 * failed (it is cancelled when packet data moves to the other SIM, as
 * telephonyd does at every start) subscribers were told nothing, and a
 * getstatus subscription that had been answered "Not available" stayed
 * that way until something else changed.
 */
static void test_update_after_failed_request(void)
{
	struct wan_service service = { 0 };

	fake_reset(TRUE, TRUE);
	g_mutex_lock(&fake.lock);
	fake.hold_get_contexts = TRUE;
	g_mutex_unlock(&fake.lock);

	g_assert_cmpint(ofono_wan_probe(&service), ==, 0);
	RUN_UNTIL(fake_has_held());
	/* the rest of the properties come in meanwhile */
	spin(100);
	g_assert_cmpint(service.notifies, ==, 0);

	fake_fail_held();
	RUN_UNTIL(service.notifies > 0);
	g_assert_true(service.last_attached);
	g_assert_cmpint(service.last_type, ==, technology_of(0));

	ofono_wan_remove(&service);
	spin(50);
}

int main(int argc, char **argv)
{
	GTestDBus *bus;
	GError *error = NULL;
	GVariant *result;
	const gchar *paths[] = { "/", "/ril_0", "/ril_1" };
	guint i, j;
	int ret;

	g_test_init(&argc, &argv, NULL);

	bus = g_test_dbus_new(G_TEST_DBUS_NONE);
	g_test_dbus_up(bus);
	/* the code under test uses G_BUS_TYPE_SYSTEM */
	g_setenv("DBUS_SYSTEM_BUS_ADDRESS", g_test_dbus_get_bus_address(bus), TRUE);

	g_mutex_init(&fake.lock);
	fake.context = g_main_context_new();
	fake.loop = g_main_loop_new(fake.context, FALSE);
	fake.objects = g_array_new(FALSE, FALSE, sizeof(guint));
	/* what the connection and the objects do runs on the fake's thread */
	g_main_context_push_thread_default(fake.context);
	fake.conn = g_dbus_connection_new_for_address_sync(g_test_dbus_get_bus_address(bus),
		G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
		NULL, NULL, &error);
	g_assert_no_error(error);

	fake.node = g_dbus_node_info_new_for_xml(fake_ofono_xml, &error);
	g_assert_no_error(error);

	for (i = 0; i < G_N_ELEMENTS(paths); i++) {
		for (j = 0; fake.node->interfaces[j]; j++) {
			gboolean manager = g_str_equal(fake.node->interfaces[j]->name, "org.ofono.Manager");
			guint id;

			/* the Manager on /, the modem interfaces on the modems */
			if (manager != g_str_equal(paths[i], "/"))
				continue;
			id = g_dbus_connection_register_object(fake.conn, paths[i], fake.node->interfaces[j],
												   &fake_vtable, NULL, NULL, &error);
			g_assert_no_error(error);
			g_array_append_val(fake.objects, id);
		}
	}

	result = g_dbus_connection_call_sync(fake.conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
										 "org.freedesktop.DBus", "RequestName",
										 g_variant_new("(su)", "org.ofono", 0x4), G_VARIANT_TYPE("(u)"),
										 G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
	g_assert_no_error(error);
	g_variant_unref(result);
	g_main_context_pop_thread_default(fake.context);
	fake.thread = g_thread_new("fake-ofono", fake_thread, NULL);

	g_test_add_func("/ofonowan/switch-to-known-modem", test_switch_to_known_modem);
	g_test_add_func("/ofonowan/remove-while-asking", test_remove_while_asking);
	g_test_add_func("/ofonowan/connection-manager-appears", test_connection_manager_appears);
	g_test_add_func("/ofonowan/update-after-failed-request", test_update_after_failed_request);

	ret = g_test_run();

	g_main_loop_quit(fake.loop);
	g_thread_join(fake.thread);
	for (i = 0; i < fake.objects->len; i++)
		g_dbus_connection_unregister_object(fake.conn, g_array_index(fake.objects, guint, i));
	g_array_free(fake.objects, TRUE);
	g_dbus_node_info_unref(fake.node);
	g_dbus_connection_close_sync(fake.conn, NULL, NULL);
	g_object_unref(fake.conn);
	/* let the connection's last sources go */
	while (g_main_context_iteration(fake.context, FALSE))
		;
	g_main_loop_unref(fake.loop);
	g_main_context_unref(fake.context);
	g_mutex_clear(&fake.lock);
	g_test_dbus_down(bus);
	g_object_unref(bus);

	return ret;
}

// vim:ts=4:sw=4:noexpandtab
