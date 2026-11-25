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
 * The SMS side of the ofono driver against a fake ofono on a private bus:
 * the message manager setting UseDeliveryReports for each message before
 * SendMessage, and the status reports of org.ofono.SmsHistory.
 *
 * The fake MessageManager on /ril_0 answers GetProperties, SetProperty
 * (with PropertyChanged, as ofono does) and SendMessage with the
 * signatures ofono has, and logs the calls in the order they came. It
 * runs on a thread of its own, as ofono is a process of its own: the
 * manager makes a blocking call when it is created.
 */

#include <string.h>

#include <glib.h>
#include <gio/gio.h>

#include "ofonobase.h"
#include "ofonomessagemanager.h"
#include "ofonosmshistory.h"

#define MODEM "/ril_0"
#define MM_IFACE "org.ofono.MessageManager"
#define HISTORY_IFACE "org.ofono.SmsHistory"

static const gchar fake_mm_xml[] =
	"<node>"
	"  <interface name='org.ofono.MessageManager'>"
	"    <method name='GetProperties'><arg type='a{sv}' direction='out'/></method>"
	"    <method name='SetProperty'><arg type='s' direction='in'/><arg type='v' direction='in'/></method>"
	"    <method name='SendMessage'><arg type='s' direction='in'/><arg type='s' direction='in'/>"
	"      <arg type='o' direction='out'/></method>"
	"    <signal name='PropertyChanged'><arg type='s'/><arg type='v'/></signal>"
	"  </interface>"
	"</node>";

struct fake_ofono {
	GMutex lock;
	GThread *thread;
	GMainContext *context;
	GMainLoop *loop;
	GDBusConnection *conn;
	GDBusInterfaceInfo *iface;
	guint object_id;

	gboolean delivery_reports;
	/* "set:true", "send:<to>", in the order they came */
	GPtrArray *calls;
	int sends;
	gboolean fail_set;
	/* answer GetProperties only when released */
	GDBusMethodInvocation *held_get;
	gboolean hold_get;
};

static struct fake_ofono fake;

static void fake_reply_get(GDBusMethodInvocation *invocation)
{
	GVariantBuilder b;

	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&b, "{sv}", "UseDeliveryReports", g_variant_new_boolean(fake.delivery_reports));
	g_variant_builder_add(&b, "{sv}", "Bearer", g_variant_new_string("cs-preferred"));
	g_dbus_method_invocation_return_value(invocation, g_variant_new("(a{sv})", &b));
}

static void fake_method_call(GDBusConnection *conn, const gchar *sender, const gchar *path,
                             const gchar *iface, const gchar *method, GVariant *params,
                             GDBusMethodInvocation *invocation, gpointer user_data)
{
	g_mutex_lock(&fake.lock);
	if (g_str_equal(method, "GetProperties")) {
		if (fake.hold_get)
			fake.held_get = invocation;
		else
			fake_reply_get(invocation);
	}
	else if (g_str_equal(method, "SetProperty")) {
		const gchar *name;
		GVariant *value;

		g_variant_get(params, "(&sv)", &name, &value);
		g_assert_cmpstr(name, ==, "UseDeliveryReports");
		g_assert_true(g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN));
		g_ptr_array_add(fake.calls, g_strdup_printf("set:%s", g_variant_get_boolean(value) ? "true" : "false"));

		if (fake.fail_set) {
			g_dbus_method_invocation_return_dbus_error(invocation, "org.ofono.Error.Failed", "Operation failed");
		}
		else {
			if (fake.delivery_reports != g_variant_get_boolean(value)) {
				fake.delivery_reports = g_variant_get_boolean(value);
				g_dbus_connection_emit_signal(fake.conn, NULL, MODEM, MM_IFACE, "PropertyChanged",
					g_variant_new("(sv)", "UseDeliveryReports", g_variant_new_boolean(fake.delivery_reports)),
					NULL);
			}
			g_dbus_method_invocation_return_value(invocation, NULL);
		}
		g_variant_unref(value);
	}
	else if (g_str_equal(method, "SendMessage")) {
		const gchar *to, *text;
		gchar *msg_path;

		g_variant_get(params, "(&s&s)", &to, &text);
		g_ptr_array_add(fake.calls, g_strdup_printf("send:%s", to));
		msg_path = g_strdup_printf(MODEM "/message_%d", ++fake.sends);
		g_dbus_method_invocation_return_value(invocation, g_variant_new("(o)", msg_path));
		g_free(msg_path);
	}
	g_mutex_unlock(&fake.lock);
}

static const GDBusInterfaceVTable fake_vtable = { fake_method_call, NULL, NULL, { 0 } };

static void fake_reset(gboolean delivery_reports)
{
	g_mutex_lock(&fake.lock);
	fake.delivery_reports = delivery_reports;
	g_ptr_array_set_size(fake.calls, 0);
	fake.sends = 0;
	fake.fail_set = FALSE;
	fake.hold_get = FALSE;
	fake.held_get = NULL;
	g_mutex_unlock(&fake.lock);
}

/* Reading and setting the fake's state from the test's thread */
static guint calls_len(void)
{
	guint n;

	g_mutex_lock(&fake.lock);
	n = fake.calls->len;
	g_mutex_unlock(&fake.lock);
	return n;
}

static gboolean fake_get_bool(gboolean *field)
{
	gboolean v;

	g_mutex_lock(&fake.lock);
	v = *field;
	g_mutex_unlock(&fake.lock);
	return v;
}

static void fake_set_bool(gboolean *field, gboolean v)
{
	g_mutex_lock(&fake.lock);
	*field = v;
	g_mutex_unlock(&fake.lock);
}

static gboolean get_is_held(void)
{
	gboolean held;

	g_mutex_lock(&fake.lock);
	held = fake.held_get != NULL;
	g_mutex_unlock(&fake.lock);
	return held;
}

static gboolean release_get_cb(gpointer data)
{
	g_mutex_lock(&fake.lock);
	fake_reply_get(fake.held_get);
	fake.held_get = NULL;
	g_mutex_unlock(&fake.lock);
	return G_SOURCE_REMOVE;
}

static gpointer fake_thread(gpointer data)
{
	g_main_context_push_thread_default(fake.context);
	g_main_loop_run(fake.loop);
	g_main_context_pop_thread_default(fake.context);
	return NULL;
}

static void fake_status_report(const char *modem, const char *message, GVariant *info)
{
	g_dbus_connection_emit_signal(fake.conn, NULL, modem, HISTORY_IFACE, "StatusReport",
								  g_variant_new("(s@a{sv})", message, info), NULL);
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
		/* the condition can change on the fake's thread, which doesn't \
		 * wake this loop: look again every millisecond */ \
		while (!(cond) && !expired_) \
			if (!g_main_context_iteration(NULL, FALSE)) \
				g_usleep(1000); \
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

/* The n-th call; the string stays until the next fake_reset() */
static const char *call(guint n)
{
	const char *c;

	g_mutex_lock(&fake.lock);
	g_assert_cmpuint(n, <, fake.calls->len);
	c = g_ptr_array_index(fake.calls, n);
	g_mutex_unlock(&fake.lock);
	return c;
}

struct sent {
	int count;
	gboolean failed;
	gchar *path;
};

static void sent_cb(struct ofono_error *error, const char *path, gpointer user_data)
{
	struct sent *s = user_data;

	s->count++;
	s->failed = (error != NULL);
	g_free(s->path);
	s->path = g_strdup(path);
}

/* The manager knows ofono's setting once GetProperties is answered */
static struct ofono_message_manager *manager_ready(void)
{
	struct ofono_message_manager *mm = ofono_message_manager_create(MODEM);

	g_assert_nonnull(mm);
	/* GetProperties goes out at once; give it time to be answered */
	spin(100);
	return mm;
}

/* ------------------------------------------------------------------------
 * Message manager
 * ------------------------------------------------------------------------ */

/* The setting already matches: SendMessage only */
static void test_no_set_when_matching(void)
{
	struct ofono_message_manager *mm;
	struct sent s = { 0 };

	fake_reset(FALSE);
	mm = manager_ready();

	ofono_message_manager_send_message(mm, "+31612345678", "hi", false, sent_cb, &s);
	RUN_UNTIL(s.count == 1);
	g_assert_false(s.failed);
	g_assert_cmpstr(s.path, ==, MODEM "/message_1");
	g_assert_cmpuint(calls_len(), ==, 1);
	g_assert_cmpstr(call(0), ==, "send:+31612345678");

	ofono_message_manager_free(mm);
	g_free(s.path);
}

/* A report wanted: the setting goes on first, then the message, once */
static void test_set_before_send(void)
{
	struct ofono_message_manager *mm;
	struct sent s = { 0 };

	fake_reset(FALSE);
	mm = manager_ready();

	ofono_message_manager_send_message(mm, "+31612345678", "hi", true, sent_cb, &s);
	RUN_UNTIL(s.count == 1);
	g_assert_false(s.failed);
	g_assert_cmpuint(calls_len(), ==, 2);
	g_assert_cmpstr(call(0), ==, "set:true");
	g_assert_cmpstr(call(1), ==, "send:+31612345678");
	g_assert_true(fake_get_bool(&fake.delivery_reports));

	/* the next one with a report needs no setting */
	ofono_message_manager_send_message(mm, "+31612345679", "hi", true, sent_cb, &s);
	RUN_UNTIL(s.count == 2);
	g_assert_cmpuint(calls_len(), ==, 3);
	g_assert_cmpstr(call(2), ==, "send:+31612345679");
	g_assert_cmpstr(s.path, ==, MODEM "/message_2");

	/* one without a report turns it off again */
	ofono_message_manager_send_message(mm, "+31612345670", "hi", false, sent_cb, &s);
	RUN_UNTIL(s.count == 3);
	g_assert_cmpuint(calls_len(), ==, 5);
	g_assert_cmpstr(call(3), ==, "set:false");
	g_assert_cmpstr(call(4), ==, "send:+31612345670");
	g_assert_false(fake_get_bool(&fake.delivery_reports));

	ofono_message_manager_free(mm);
	g_free(s.path);
}

/* Someone else changed the setting: the manager follows PropertyChanged */
static void test_follows_property_changed(void)
{
	struct ofono_message_manager *mm;
	struct sent s = { 0 };

	fake_reset(FALSE);
	mm = manager_ready();

	fake_set_bool(&fake.delivery_reports, TRUE);
	g_dbus_connection_emit_signal(fake.conn, NULL, MODEM, MM_IFACE, "PropertyChanged",
		g_variant_new("(sv)", "UseDeliveryReports", g_variant_new_boolean(TRUE)), NULL);
	spin(100);

	ofono_message_manager_send_message(mm, "+31612345678", "hi", true, sent_cb, &s);
	RUN_UNTIL(s.count == 1);
	g_assert_cmpuint(calls_len(), ==, 1);
	g_assert_cmpstr(call(0), ==, "send:+31612345678");

	ofono_message_manager_free(mm);
	g_free(s.path);
}

/* The setting could not be changed: the message still goes out */
static void test_set_fails_still_sends(void)
{
	struct ofono_message_manager *mm;
	struct sent s = { 0 };

	fake_reset(FALSE);
	mm = manager_ready();
	fake_set_bool(&fake.fail_set, TRUE);

	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*could not set UseDeliveryReports*");
	ofono_message_manager_send_message(mm, "+31612345678", "hi", true, sent_cb, &s);
	RUN_UNTIL(s.count == 1);
	g_test_assert_expected_messages();
	g_assert_false(s.failed);
	g_assert_cmpuint(calls_len(), ==, 2);
	g_assert_cmpstr(call(0), ==, "set:true");
	g_assert_cmpstr(call(1), ==, "send:+31612345678");

	/* ofono still has it off, so the next message tries again */
	fake_set_bool(&fake.fail_set, FALSE);
	ofono_message_manager_send_message(mm, "+31612345679", "hi", true, sent_cb, &s);
	RUN_UNTIL(s.count == 2);
	g_assert_cmpuint(calls_len(), ==, 4);
	g_assert_cmpstr(call(2), ==, "set:true");
	g_assert_cmpstr(call(3), ==, "send:+31612345679");

	ofono_message_manager_free(mm);
	g_free(s.path);
}

/* Before ofono's setting is known it is set either way */
static void test_unknown_setting_is_set(void)
{
	struct ofono_message_manager *mm;
	struct sent s = { 0 };

	fake_reset(FALSE);
	fake_set_bool(&fake.hold_get, TRUE);
	mm = ofono_message_manager_create(MODEM);
	RUN_UNTIL(get_is_held());

	ofono_message_manager_send_message(mm, "+31612345678", "hi", false, sent_cb, &s);
	RUN_UNTIL(s.count == 1);
	g_assert_cmpuint(calls_len(), ==, 2);
	g_assert_cmpstr(call(0), ==, "set:false");
	g_assert_cmpstr(call(1), ==, "send:+31612345678");

	g_main_context_invoke(fake.context, release_get_cb, NULL);
	spin(50);

	ofono_message_manager_free(mm);
	g_free(s.path);
}

/* Freed while the setting is on its way: the message still goes out and
 * the caller hears about it */
static void test_free_while_setting(void)
{
	struct ofono_message_manager *mm;
	struct sent s = { 0 };

	fake_reset(FALSE);
	mm = manager_ready();

	ofono_message_manager_send_message(mm, "+31612345678", "hi", true, sent_cb, &s);
	ofono_message_manager_free(mm);
	RUN_UNTIL(s.count == 1);
	g_assert_false(s.failed);
	g_assert_cmpuint(calls_len(), ==, 2);
	g_free(s.path);
}

/* ------------------------------------------------------------------------
 * Status reports
 * ------------------------------------------------------------------------ */

struct reports {
	int count;
	gchar *path;
	gboolean delivered;
};

static void report_cb(const char *message_path, bool delivered, void *user_data)
{
	struct reports *r = user_data;

	r->count++;
	g_free(r->path);
	r->path = g_strdup(message_path);
	r->delivered = delivered;
}

static GVariant *info(gboolean delivered)
{
	GVariantBuilder b;

	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&b, "{sv}", "Delivered", g_variant_new_boolean(delivered));
	return g_variant_builder_end(&b);
}

static void test_status_reports(void)
{
	struct ofono_sms_history *h;
	struct reports r = { 0 };
	GVariantBuilder b;

	h = ofono_sms_history_create(MODEM, report_cb, &r);
	g_assert_nonnull(h);
	spin(50);

	fake_status_report(MODEM, MODEM "/message_1", info(TRUE));
	RUN_UNTIL(r.count == 1);
	g_assert_cmpstr(r.path, ==, MODEM "/message_1");
	g_assert_true(r.delivered);

	fake_status_report(MODEM, MODEM "/message_2", info(FALSE));
	RUN_UNTIL(r.count == 2);
	g_assert_cmpstr(r.path, ==, MODEM "/message_2");
	g_assert_false(r.delivered);

	/* another modem's reports are not ours */
	fake_status_report("/ril_1", "/ril_1/message_1", info(TRUE));
	/* one without Delivered is no report */
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*message_3 without Delivered*");
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*message_4 without Delivered*");
	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	fake_status_report(MODEM, MODEM "/message_3", g_variant_builder_end(&b));
	/* a type it doesn't expect neither */
	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&b, "{sv}", "Delivered", g_variant_new_string("yes"));
	fake_status_report(MODEM, MODEM "/message_4", g_variant_builder_end(&b));
	spin(150);
	g_assert_cmpint(r.count, ==, 2);
	g_test_assert_expected_messages();

	ofono_sms_history_free(h);

	/* none after it is freed */
	fake_status_report(MODEM, MODEM "/message_5", info(TRUE));
	spin(100);
	g_assert_cmpint(r.count, ==, 2);

	g_assert_null(ofono_sms_history_create(NULL, report_cb, &r));
	ofono_sms_history_free(NULL);
	g_free(r.path);
}

int main(int argc, char **argv)
{
	GTestDBus *bus;
	GError *error = NULL;
	GDBusNodeInfo *node;
	GVariant *result;
	int ret;

	g_test_init(&argc, &argv, NULL);

	bus = g_test_dbus_new(G_TEST_DBUS_NONE);
	g_test_dbus_up(bus);
	g_setenv("DBUS_SYSTEM_BUS_ADDRESS", g_test_dbus_get_bus_address(bus), TRUE);

	g_mutex_init(&fake.lock);
	fake.context = g_main_context_new();
	fake.loop = g_main_loop_new(fake.context, FALSE);
	/* what the connection and the object do runs on the fake's thread */
	g_main_context_push_thread_default(fake.context);
	fake.conn = g_dbus_connection_new_for_address_sync(g_test_dbus_get_bus_address(bus),
		G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
		NULL, NULL, &error);
	g_assert_no_error(error);
	fake.calls = g_ptr_array_new_with_free_func(g_free);

	node = g_dbus_node_info_new_for_xml(fake_mm_xml, &error);
	g_assert_no_error(error);
	fake.iface = g_dbus_interface_info_ref(node->interfaces[0]);
	g_dbus_node_info_unref(node);

	fake.object_id = g_dbus_connection_register_object(fake.conn, MODEM, fake.iface, &fake_vtable,
													   NULL, NULL, &error);
	g_assert_no_error(error);

	result = g_dbus_connection_call_sync(fake.conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
										 "org.freedesktop.DBus", "RequestName",
										 g_variant_new("(su)", "org.ofono", 0x4), G_VARIANT_TYPE("(u)"),
										 G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
	g_assert_no_error(error);
	g_variant_unref(result);
	g_main_context_pop_thread_default(fake.context);
	fake.thread = g_thread_new("fake-ofono", fake_thread, NULL);

	g_test_add_func("/ofonosms/no-set-when-matching", test_no_set_when_matching);
	g_test_add_func("/ofonosms/set-before-send", test_set_before_send);
	g_test_add_func("/ofonosms/follows-property-changed", test_follows_property_changed);
	g_test_add_func("/ofonosms/set-fails-still-sends", test_set_fails_still_sends);
	g_test_add_func("/ofonosms/unknown-setting-is-set", test_unknown_setting_is_set);
	g_test_add_func("/ofonosms/free-while-setting", test_free_while_setting);
	g_test_add_func("/ofonosms/status-reports", test_status_reports);

	ret = g_test_run();

	g_main_loop_quit(fake.loop);
	g_thread_join(fake.thread);
	g_dbus_connection_unregister_object(fake.conn, fake.object_id);
	g_dbus_interface_info_unref(fake.iface);
	g_ptr_array_free(fake.calls, TRUE);
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
