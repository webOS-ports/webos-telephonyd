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
 * The SIM toolkit agent against a fake ofono on a private bus.
 *
 * GTestDBus starts a dbus-daemon of our own and the test points the system
 * bus at it, so the code under test talks D-Bus exactly as on a device. The
 * fake implements org.ofono.SimToolkit on /ril_0 and drives the agent the way
 * ofono 2.19's src/stkagent.c does: method calls with the documented
 * signatures, Cancel() and Release() without a reply, and the same error
 * names on the way back.
 */

#include <string.h>

#include <glib.h>
#include <gio/gio.h>

#include "ofonostk.h"
#include "stkcore.h"

#define MODEM_PATH "/ril_0"
#define STK_IFACE "org.ofono.SimToolkit"
#define AGENT_IFACE "org.ofono.SimToolkitAgent"

static const gchar fake_ofono_xml[] =
	"<node>"
	"  <interface name='org.ofono.SimToolkit'>"
	"    <method name='GetProperties'><arg type='a{sv}' direction='out'/></method>"
	"    <method name='SelectItem'><arg type='y' direction='in'/><arg type='o' direction='in'/></method>"
	"    <method name='RegisterAgent'><arg type='o' direction='in'/></method>"
	"    <method name='UnregisterAgent'><arg type='o' direction='in'/></method>"
	"    <signal name='PropertyChanged'><arg type='s'/><arg type='v'/></signal>"
	"  </interface>"
	"</node>";

/* ------------------------------------------------------------------------
 * The fake ofono
 * ------------------------------------------------------------------------ */

struct fake_ofono {
	GDBusConnection *conn;
	guint object_id;

	int register_calls;
	int fail_register;		/* fail this many RegisterAgent calls first */
	gchar *agent_path;
	gchar *agent_bus;
	int unregister_calls;

	int select_calls;
	guint8 selected_item;
	gchar *select_agent_path;

	/* the last agent reply */
	int replies;
	GVariant *reply;
	gchar *reply_error;
};

static struct fake_ofono fake;

static GVariant* fake_properties(void)
{
	GVariantBuilder props, items;

	g_variant_builder_init(&items, G_VARIANT_TYPE("a(sy)"));
	g_variant_builder_add(&items, "(sy)", "Balance", 1);
	g_variant_builder_add(&items, "(sy)", "Services", 0);

	g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&props, "{sv}", "MainMenuTitle", g_variant_new_string("Operator"));
	g_variant_builder_add(&props, "{sv}", "MainMenuIcon", g_variant_new_byte(3));
	g_variant_builder_add(&props, "{sv}", "MainMenu", g_variant_builder_end(&items));
	g_variant_builder_add(&props, "{sv}", "IdleModeText", g_variant_new_string(""));
	g_variant_builder_add(&props, "{sv}", "IdleModeIcon", g_variant_new_byte(0));

	return g_variant_new("(@a{sv})", g_variant_builder_end(&props));
}

static void fake_method_call(GDBusConnection *conn, const gchar *sender, const gchar *path,
                             const gchar *iface, const gchar *method, GVariant *params,
                             GDBusMethodInvocation *invocation, gpointer user_data)
{
	const gchar *agent;
	guint8 item;

	if (g_str_equal(method, "GetProperties")) {
		g_dbus_method_invocation_return_value(invocation, fake_properties());
	}
	else if (g_str_equal(method, "RegisterAgent")) {
		fake.register_calls++;
		if (fake.fail_register > 0) {
			fake.fail_register--;
			g_dbus_method_invocation_return_dbus_error(invocation, "org.ofono.Error.InProgress",
													   "Operation already in progress");
			return;
		}
		g_variant_get(params, "(&o)", &agent);
		g_free(fake.agent_path);
		fake.agent_path = g_strdup(agent);
		g_free(fake.agent_bus);
		fake.agent_bus = g_strdup(sender);
		g_dbus_method_invocation_return_value(invocation, NULL);
	}
	else if (g_str_equal(method, "UnregisterAgent")) {
		fake.unregister_calls++;
		g_dbus_method_invocation_return_value(invocation, NULL);
	}
	else if (g_str_equal(method, "SelectItem")) {
		g_variant_get(params, "(y&o)", &item, &agent);
		fake.select_calls++;
		fake.selected_item = item;
		g_free(fake.select_agent_path);
		fake.select_agent_path = g_strdup(agent);
		if (item == 9)
			g_dbus_method_invocation_return_dbus_error(invocation, "org.ofono.Error.InProgress", "Busy");
		else
			g_dbus_method_invocation_return_value(invocation, NULL);
	}
	else {
		g_dbus_method_invocation_return_dbus_error(invocation, "org.freedesktop.DBus.Error.UnknownMethod",
												   method);
	}
}

static const GDBusInterfaceVTable fake_vtable = { fake_method_call, NULL, NULL, { 0 } };

static void fake_reply_cb(GObject *source, GAsyncResult *res, gpointer user_data)
{
	GError *error = NULL;
	GVariant *reply;

	reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);

	if (fake.reply)
		g_variant_unref(fake.reply);
	fake.reply = reply;
	g_free(fake.reply_error);
	fake.reply_error = NULL;

	if (error) {
		fake.reply_error = g_dbus_error_get_remote_error(error);
		if (!fake.reply_error)
			fake.reply_error = g_strdup(error->message);
		g_error_free(error);
	}

	fake.replies++;
}

/* ofono calling a method of the agent at path (default: the registered one) */
static void fake_call_agent(const gchar *path, const gchar *method, GVariant *params)
{
	g_dbus_connection_call(fake.conn, fake.agent_bus, path ? path : fake.agent_path, AGENT_IFACE,
						   method, params, NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, fake_reply_cb, NULL);
}

static void fake_call_agent_noreply(const gchar *path, const gchar *method)
{
	GDBusMessage *msg;

	msg = g_dbus_message_new_method_call(fake.agent_bus, path ? path : fake.agent_path,
										 AGENT_IFACE, method);
	g_dbus_message_set_flags(msg, G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED);
	g_dbus_connection_send_message(fake.conn, msg, G_DBUS_SEND_MESSAGE_FLAGS_NONE, NULL, NULL);
	g_object_unref(msg);
}

static void fake_emit_menu(const char *label)
{
	GVariantBuilder items;

	g_variant_builder_init(&items, G_VARIANT_TYPE("a(sy)"));
	g_variant_builder_add(&items, "(sy)", label, 5);

	g_dbus_connection_emit_signal(fake.conn, NULL, MODEM_PATH, STK_IFACE, "PropertyChanged",
								  g_variant_new("(sv)", "MainMenu", g_variant_builder_end(&items)), NULL);
}

static void fake_reset_counters(void)
{
	fake.register_calls = 0;
	fake.fail_register = 0;
	fake.unregister_calls = 0;
	fake.select_calls = 0;
	fake.replies = 0;
}

/* ------------------------------------------------------------------------
 * The "service" side: a stk_core fed by the agent
 * ------------------------------------------------------------------------ */

struct service {
	struct stk_core *core;
	int menu_changes;
	gchar *menu_title;
	guint menu_items;
	gchar *first_label;
	guint8 menu_icon;

	int requests;
	guint last_id;
	struct stk_request last;	/* shallow copy of the scalar fields */
	gchar *last_text;
	gchar *last_default;
	gchar *last_tone;
	gchar *last_url;
	guint last_items;

	int ended;
	enum stk_end_reason last_reason;

	int cancels;
	bool last_cancel_session;
	bool last_cancel_released;
};

static struct service svc;

static void svc_menu_changed(const struct stk_menu *menu, void *data)
{
	svc.menu_changes++;
	g_free(svc.menu_title);
	svc.menu_title = g_strdup(menu->title);
	svc.menu_items = menu->n_items;
	g_free(svc.first_label);
	svc.first_label = menu->n_items ? g_strdup(menu->items[0].label) : NULL;
	svc.menu_icon = menu->icon_id;
}

static void svc_request(struct stk_request *request, void *token, void *data)
{
	stk_core_submit(svc.core, request, ofono_stk_send_reply, token);
}

static void svc_cancel(bool session, bool released, void *data)
{
	svc.cancels++;
	svc.last_cancel_session = session;
	svc.last_cancel_released = released;
	stk_core_end_agent(svc.core, 0, session, released ? STK_END_RELEASED : STK_END_CANCELLED);
}

static void svc_event(void *data, enum stk_event event, const struct stk_request *request,
                      enum stk_end_reason reason)
{
	if (event == STK_EVENT_ENDED) {
		svc.ended++;
		svc.last_reason = reason;
		return;
	}

	svc.requests++;
	svc.last_id = request->id;
	svc.last = *request;
	svc.last.text = svc.last.default_text = svc.last.tone = svc.last.url = NULL;
	svc.last.items = NULL;
	g_free(svc.last_text);
	svc.last_text = g_strdup(request->text);
	g_free(svc.last_default);
	svc.last_default = g_strdup(request->default_text);
	g_free(svc.last_tone);
	svc.last_tone = g_strdup(request->tone);
	g_free(svc.last_url);
	svc.last_url = g_strdup(request->url);
	svc.last_items = request->n_items;
}

static const struct ofono_stk_callbacks svc_callbacks = {
	.menu_changed = svc_menu_changed,
	.request = svc_request,
	.cancel = svc_cancel,
};

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

/* Let pending D-Bus traffic settle without waiting for anything in particular. */
static void spin(guint ms)
{
	gboolean expired = FALSE;

	g_timeout_add(ms, deadline_cb, &expired);
	while (!expired)
		g_main_context_iteration(NULL, TRUE);
}

static struct ofono_stk* setup_stk(void)
{
	struct ofono_stk *stk;

	fake_reset_counters();
	memset(&svc, 0, sizeof(svc));
	svc.core = stk_core_new(svc_event, NULL);
	stk_core_set_timeout(svc.core, 0);

	stk = ofono_stk_create(MODEM_PATH, 0, &svc_callbacks, NULL);
	g_assert_nonnull(stk);

	RUN_UNTIL(ofono_stk_is_agent_registered(stk) && svc.menu_changes >= 4);

	return stk;
}

static void teardown_stk(struct ofono_stk *stk)
{
	ofono_stk_free(stk);
	stk_core_free(svc.core);
	g_free(svc.menu_title);
	g_free(svc.first_label);
	g_free(svc.last_text);
	g_free(svc.last_default);
	g_free(svc.last_tone);
	g_free(svc.last_url);
	memset(&svc, 0, sizeof(svc));
	/* the fire-and-forget UnregisterAgent */
	spin(20);
}

/* Answer the last request and wait for ofono to see the reply. */
static void respond(const char *action, const struct stk_answer *answer)
{
	int before = fake.replies;
	const char *err = NULL;

	if (!stk_core_respond(svc.core, svc.last_id, action, answer, &err))
		g_error("respond failed: %s", err);

	RUN_UNTIL(fake.replies == before + 1);
}

static void call_and_wait_request(const char *method, GVariant *params)
{
	int before = svc.requests;

	fake_call_agent(NULL, method, params);
	RUN_UNTIL(svc.requests == before + 1);
}

/* The first value of the last agent reply. */
static guint8 reply_byte(void)
{
	guint8 v;

	g_variant_get(fake.reply, "(y)", &v);
	return v;
}

static gchar* reply_string(void)
{
	gchar *v;

	g_variant_get(fake.reply, "(s)", &v);
	return v;
}

static gboolean reply_bool(void)
{
	gboolean v;

	g_variant_get(fake.reply, "(b)", &v);
	return v;
}

/* ------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------ */

static void test_register_and_menu(void)
{
	struct ofono_stk *stk = setup_stk();

	g_assert_cmpint(fake.register_calls, ==, 1);
	g_assert_cmpstr(fake.agent_path, ==, "/com/palm/telephony/stk/sim0/agent");
	g_assert_cmpstr(ofono_stk_get_agent_path(stk), ==, fake.agent_path);

	g_assert_cmpstr(svc.menu_title, ==, "Operator");
	g_assert_cmpuint(svc.menu_icon, ==, 3);
	g_assert_cmpuint(svc.menu_items, ==, 2);
	g_assert_cmpstr(svc.first_label, ==, "Balance");
	g_assert_cmpuint(ofono_stk_get_menu(stk)->n_items, ==, 2);

	/* the SIM sends a new Setup Menu */
	fake_emit_menu("Roaming");
	RUN_UNTIL(svc.menu_items == 1);
	g_assert_cmpstr(svc.first_label, ==, "Roaming");

	teardown_stk(stk);
	g_assert_cmpint(fake.unregister_calls, ==, 1);
}

static void test_selection_and_input(void)
{
	struct ofono_stk *stk = setup_stk();
	GVariantBuilder items;
	struct stk_answer a;
	gchar *text;

	g_variant_builder_init(&items, G_VARIANT_TYPE("a(sy)"));
	g_variant_builder_add(&items, "(sy)", "Prepaid", 0);
	g_variant_builder_add(&items, "(sy)", "Top up", 0);
	g_variant_builder_add(&items, "(sy)", "Help", 0);

	call_and_wait_request("RequestSelection",
						  g_variant_new("(sya(sy)n)", "Choose", 2, &items, (gint16) 1));
	g_assert_cmpint(svc.last.type, ==, STK_REQUEST_SELECTION);
	g_assert_cmpstr(svc.last_text, ==, "Choose");
	g_assert_cmpuint(svc.last.icon_id, ==, 2);
	g_assert_cmpuint(svc.last_items, ==, 3);
	g_assert_cmpint(svc.last.default_item, ==, 1);
	g_assert_false(svc.last.session);

	memset(&a, 0, sizeof(a));
	a.has_index = true;
	a.index = 2;
	respond("answer", &a);
	g_assert_null(fake.reply_error);
	g_assert_true(g_variant_is_of_type(fake.reply, G_VARIANT_TYPE("(y)")));
	g_assert_cmpuint(reply_byte(), ==, 2);

	call_and_wait_request("RequestInput",
						  g_variant_new("(sysyyb)", "Name", 0, "abc", (guchar) 1, (guchar) 5, TRUE) );
	g_assert_cmpint(svc.last.type, ==, STK_REQUEST_INPUT);
	g_assert_cmpstr(svc.last_default, ==, "abc");
	g_assert_cmpuint(svc.last.min_length, ==, 1);
	g_assert_cmpuint(svc.last.max_length, ==, 5);
	g_assert_true(svc.last.hide_typing);

	memset(&a, 0, sizeof(a));
	a.has_string = true;
	a.string = "Herman";	/* six characters, too long */
	a.string_len = strlen(a.string);
	g_assert_false(stk_core_respond(svc.core, svc.last_id, "answer", &a, NULL));
	a.string = "Herr";
	a.string_len = strlen(a.string);
	respond("answer", &a);
	g_assert_true(g_variant_is_of_type(fake.reply, G_VARIANT_TYPE("(s)")));
	text = reply_string();
	g_assert_cmpstr(text, ==, "Herr");
	g_free(text);

	teardown_stk(stk);
}

/* One table row per agent method that is not covered above. */
struct method_case {
	const char *method;
	const char *signature;
	enum stk_request_type type;
	const char *reply_type;	/* NULL: empty reply */
};

static void test_every_method(void)
{
	static const struct method_case cases[] = {
		{ "DisplayText", "(syb)", STK_REQUEST_DISPLAY_TEXT, NULL },
		{ "RequestDigits", "(sysyyb)", STK_REQUEST_DIGITS, "(s)" },
		{ "RequestKey", "(sy)", STK_REQUEST_KEY, "(s)" },
		{ "RequestDigit", "(sy)", STK_REQUEST_DIGIT, "(s)" },
		{ "RequestQuickDigit", "(sy)", STK_REQUEST_QUICK_DIGIT, "(s)" },
		{ "RequestConfirmation", "(sy)", STK_REQUEST_CONFIRMATION, "(b)" },
		{ "ConfirmCallSetup", "(sy)", STK_REQUEST_CONFIRM_CALL_SETUP, "(b)" },
		{ "PlayTone", "(ssy)", STK_REQUEST_PLAY_TONE, NULL },
		{ "LoopTone", "(ssy)", STK_REQUEST_LOOP_TONE, NULL },
		{ "DisplayActionInformation", "(sy)", STK_REQUEST_DISPLAY_ACTION_INFORMATION, NULL },
		{ "ConfirmLaunchBrowser", "(sys)", STK_REQUEST_CONFIRM_LAUNCH_BROWSER, "(b)" },
		{ "DisplayAction", "(sy)", STK_REQUEST_DISPLAY_ACTION, NULL },
		{ "ConfirmOpenChannel", "(sy)", STK_REQUEST_CONFIRM_OPEN_CHANNEL, "(b)" },
	};
	struct ofono_stk *stk = setup_stk();
	struct stk_answer a;
	GVariant *params;
	guint n;

	for (n = 0; n < G_N_ELEMENTS(cases); n++) {
		const struct method_case *c = &cases[n];

		if (g_str_equal(c->signature, "(syb)"))
			params = g_variant_new(c->signature, "Hello", 4, TRUE);
		else if (g_str_equal(c->signature, "(sysyyb)"))
			params = g_variant_new(c->signature, "PIN", 4, "", (guchar) 4, (guchar) 8, FALSE);
		else if (g_str_equal(c->signature, "(ssy)"))
			params = g_variant_new(c->signature, "general-beep", "Beep", 4);
		else if (g_str_equal(c->signature, "(sys)"))
			params = g_variant_new(c->signature, "Open page?", 4, "http://example.com/");
		else
			params = g_variant_new(c->signature, "Hello", 4);

		call_and_wait_request(c->method, params);
		g_assert_cmpint(svc.last.type, ==, c->type);
		g_assert_cmpuint(svc.last.icon_id, ==, 4);

		memset(&a, 0, sizeof(a));
		switch (stk_request_answer_kind(c->type)) {
		case STK_ANSWER_STRING:
			a.has_string = true;
			a.string = c->type == STK_REQUEST_DIGITS ? "1234" : "7";
			a.string_len = strlen(a.string);
			break;
		case STK_ANSWER_BOOLEAN:
			a.has_boolean = true;
			a.boolean = true;
			break;
		default:
			break;
		}

		respond("answer", &a);
		g_assert_null(fake.reply_error);

		if (c->reply_type)
			g_assert_true(g_variant_is_of_type(fake.reply, G_VARIANT_TYPE(c->reply_type)));
		else
			g_assert_true(g_variant_is_of_type(fake.reply, G_VARIANT_TYPE_UNIT));
	}

	g_assert_true(svc.last.type == STK_REQUEST_CONFIRM_OPEN_CHANNEL);

	teardown_stk(stk);
}

static void test_request_fields(void)
{
	struct ofono_stk *stk = setup_stk();

	call_and_wait_request("DisplayText", g_variant_new("(syb)", "Urgent!", 0, TRUE));
	g_assert_true(svc.last.urgent);
	respond("busy", NULL);
	g_assert_cmpstr(fake.reply_error, ==, "org.ofono.Error.Busy");

	call_and_wait_request("LoopTone", g_variant_new("(ssy)", "ringing-tone", "Ringing", 0));
	g_assert_cmpstr(svc.last_tone, ==, "ringing-tone");
	g_assert_cmpstr(svc.last_text, ==, "Ringing");
	respond("endSession", NULL);
	g_assert_cmpstr(fake.reply_error, ==, "org.ofono.Error.EndSession");

	call_and_wait_request("ConfirmLaunchBrowser", g_variant_new("(sys)", "Go?", 0, "http://example.com/"));
	g_assert_cmpstr(svc.last_url, ==, "http://example.com/");
	/* no error allowed: cancel declines with false */
	respond("cancel", NULL);
	g_assert_null(fake.reply_error);
	g_assert_false(reply_bool());

	call_and_wait_request("RequestKey", g_variant_new("(sy)", "Key?", 0));
	respond("goBack", NULL);
	g_assert_cmpstr(fake.reply_error, ==, "org.ofono.Error.GoBack");

	teardown_stk(stk);
}

static void test_cancel(void)
{
	struct ofono_stk *stk = setup_stk();
	int before;

	call_and_wait_request("RequestConfirmation", g_variant_new("(sy)", "Sure?", 0));
	before = fake.replies;

	/* ofono gives up on the call and tells the agent */
	fake_call_agent_noreply(NULL, "Cancel");
	RUN_UNTIL(svc.ended == 1);

	g_assert_cmpint(svc.last_reason, ==, STK_END_CANCELLED);
	g_assert_cmpint(svc.cancels, ==, 1);
	g_assert_false(svc.last_cancel_session);
	g_assert_false(svc.last_cancel_released);
	g_assert_cmpuint(stk_core_pending_count(svc.core), ==, 0);

	/* the abandoned call still got its (ignored) reply */
	RUN_UNTIL(fake.replies == before + 1);
	g_assert_cmpstr(fake.reply_error, ==, "org.ofono.Error.EndSession");

	teardown_stk(stk);
}

static void test_release_reregisters(void)
{
	struct ofono_stk *stk = setup_stk();

	g_assert_cmpint(fake.register_calls, ==, 1);

	fake_call_agent_noreply(NULL, "Release");
	RUN_UNTIL(fake.register_calls == 2 && ofono_stk_is_agent_registered(stk));
	g_assert_cmpint(svc.cancels, ==, 1);
	g_assert_true(svc.last_cancel_released);

	/* a released session agent does not re-register anything */
	fake_call_agent_noreply(ofono_stk_get_session_agent_path(stk), "Release");
	RUN_UNTIL(svc.cancels == 2);
	g_assert_true(svc.last_cancel_session);
	spin(50);
	g_assert_cmpint(fake.register_calls, ==, 2);

	teardown_stk(stk);
}

static void test_register_retries(void)
{
	struct ofono_stk *stk;

	fake_reset_counters();
	memset(&svc, 0, sizeof(svc));
	svc.core = stk_core_new(svc_event, NULL);
	fake.fail_register = 2;

	stk = ofono_stk_create(MODEM_PATH, 0, &svc_callbacks, NULL);
	RUN_UNTIL(ofono_stk_is_agent_registered(stk));
	g_assert_cmpint(fake.register_calls, ==, 3);

	teardown_stk(stk);
}

static void test_register_gives_up(void)
{
	struct ofono_stk *stk;

	fake_reset_counters();
	memset(&svc, 0, sizeof(svc));
	svc.core = stk_core_new(svc_event, NULL);
	fake.fail_register = 1000;

	stk = ofono_stk_create(MODEM_PATH, 0, &svc_callbacks, NULL);
	/* first try plus the configured three retries (5, 10, 20 ms) */
	RUN_UNTIL(fake.register_calls == 4);
	spin(150);
	g_assert_cmpint(fake.register_calls, ==, 4);
	g_assert_false(ofono_stk_is_agent_registered(stk));

	teardown_stk(stk);
	/* never registered, so nothing to unregister */
	g_assert_cmpint(fake.unregister_calls, ==, 0);
}

static void foreign_reply_cb(GObject *source, GAsyncResult *res, gpointer user_data)
{
	gchar **error_name = user_data;
	GError *error = NULL;
	GVariant *reply;

	reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
	if (reply)
		g_variant_unref(reply);
	if (error) {
		*error_name = g_dbus_error_get_remote_error(error);
		g_error_free(error);
	}
	else {
		*error_name = g_strdup("none");
	}
}

static void test_foreign_caller_refused(void)
{
	struct ofono_stk *stk = setup_stk();
	GDBusConnection *other;
	gchar *error_name = NULL;

	other = g_dbus_connection_new_for_address_sync(g_getenv("DBUS_SYSTEM_BUS_ADDRESS"),
		G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
		NULL, NULL, NULL);
	g_assert_nonnull(other);

	g_dbus_connection_call(other, fake.agent_bus, fake.agent_path, AGENT_IFACE, "DisplayText",
						   g_variant_new("(syb)", "Phish", 0, FALSE), NULL, G_DBUS_CALL_FLAGS_NONE,
						   5000, NULL, foreign_reply_cb, &error_name);
	RUN_UNTIL(error_name != NULL);

	g_assert_cmpstr(error_name, ==, "org.freedesktop.DBus.Error.AccessDenied");
	g_assert_cmpint(svc.requests, ==, 0);

	g_free(error_name);
	g_object_unref(other);
	teardown_stk(stk);
}

struct select_result {
	bool done;
	bool failed;
	enum ofono_error_type type;
};

static void select_cb(struct ofono_error *error, void *data)
{
	struct select_result *r = data;

	r->done = true;
	r->failed = error != NULL;
	if (error)
		r->type = error->type;
}

static void test_select_item_session(void)
{
	struct ofono_stk *stk = setup_stk();
	struct select_result r = { 0 };
	struct stk_answer a;

	ofono_stk_select_item(stk, 1, select_cb, &r);
	RUN_UNTIL(r.done);
	g_assert_false(r.failed);
	g_assert_cmpint(fake.select_calls, ==, 1);
	g_assert_cmpuint(fake.selected_item, ==, 1);
	g_assert_cmpstr(fake.select_agent_path, ==, "/com/palm/telephony/stk/sim0/session");

	/* the session's first command comes in on the session agent */
	fake_call_agent(fake.select_agent_path, "RequestConfirmation", g_variant_new("(sy)", "Top up?", 0));
	RUN_UNTIL(svc.requests == 1);
	g_assert_true(svc.last.session);

	memset(&a, 0, sizeof(a));
	a.has_boolean = true;
	a.boolean = false;
	respond("answer", &a);
	g_assert_false(reply_bool());

	/* out of range never reaches ofono */
	memset(&r, 0, sizeof(r));
	ofono_stk_select_item(stk, 7, select_cb, &r);
	g_assert_true(r.done && r.failed);
	g_assert_cmpint(r.type, ==, OFONO_ERROR_TYPE_INVALID_ARGUMENTS);
	g_assert_cmpint(fake.select_calls, ==, 1);

	teardown_stk(stk);
}

static void test_select_item_busy(void)
{
	struct ofono_stk *stk = setup_stk();
	struct select_result r = { 0 };
	GVariantBuilder items;
	guint n;

	/* make item 9 exist so the call goes out; the fake answers it busy */
	g_variant_builder_init(&items, G_VARIANT_TYPE("a(sy)"));
	for (n = 0; n < 10; n++)
		g_variant_builder_add(&items, "(sy)", "x", 0);
	g_dbus_connection_emit_signal(fake.conn, NULL, MODEM_PATH, STK_IFACE, "PropertyChanged",
								  g_variant_new("(sv)", "MainMenu", g_variant_builder_end(&items)), NULL);
	RUN_UNTIL(svc.menu_items == 10);

	ofono_stk_select_item(stk, 9, select_cb, &r);
	RUN_UNTIL(r.done);
	g_assert_true(r.failed);
	g_assert_cmpint(r.type, ==, OFONO_ERROR_TYPE_IN_PROGRESS);

	teardown_stk(stk);
}

static void test_free_with_pending(void)
{
	struct ofono_stk *stk = setup_stk();
	int before;

	call_and_wait_request("RequestDigits", g_variant_new("(sysyyb)", "PIN", 0, "", (guchar) 4, (guchar) 4, TRUE));
	before = fake.replies;

	/* the SimToolkit interface goes away while the user is typing */
	ofono_stk_free(stk);
	g_assert_cmpint(svc.ended, ==, 1);
	g_assert_cmpint(svc.last_reason, ==, STK_END_RELEASED);

	RUN_UNTIL(fake.replies == before + 1 && fake.unregister_calls == 1);
	g_assert_cmpstr(fake.reply_error, ==, "org.ofono.Error.EndSession");

	stk_core_free(svc.core);
	g_free(svc.menu_title);
	g_free(svc.first_label);
	g_free(svc.last_text);
	g_free(svc.last_default);
	g_free(svc.last_tone);
	g_free(svc.last_url);
	memset(&svc, 0, sizeof(svc));
}

static void test_core_timeout_over_dbus(void)
{
	struct ofono_stk *stk = setup_stk();
	int before;

	stk_core_set_timeout(svc.core, 30);
	call_and_wait_request("DisplayActionInformation", g_variant_new("(sy)", "Working", 0));
	before = fake.replies;

	/* nobody answers: DisplayActionInformation takes no error, so the
	 * timeout dismisses it with an empty reply */
	RUN_UNTIL(fake.replies == before + 1);
	g_assert_null(fake.reply_error);
	g_assert_true(g_variant_is_of_type(fake.reply, G_VARIANT_TYPE_UNIT));
	g_assert_cmpint(svc.last_reason, ==, STK_END_TIMEOUT);

	teardown_stk(stk);
}

int main(int argc, char **argv)
{
	GTestDBus *bus;
	GDBusNodeInfo *node;
	GError *error = NULL;
	GVariant *result;
	int ret;

	g_test_init(&argc, &argv, NULL);
	/* The retry and refusal paths log warnings on purpose; criticals stay fatal. */
	g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);

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
	fake.object_id = g_dbus_connection_register_object(fake.conn, MODEM_PATH, node->interfaces[0],
													   &fake_vtable, NULL, NULL, &error);
	g_assert_no_error(error);

	result = g_dbus_connection_call_sync(fake.conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
										 "org.freedesktop.DBus", "RequestName",
										 g_variant_new("(su)", "org.ofono", 0x4), G_VARIANT_TYPE("(u)"),
										 G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
	g_assert_no_error(error);
	g_variant_unref(result);

	ofono_stk_set_retry_interval(5, 3);

	g_test_add_func("/ofonostk/register-and-menu", test_register_and_menu);
	g_test_add_func("/ofonostk/selection-and-input", test_selection_and_input);
	g_test_add_func("/ofonostk/every-method", test_every_method);
	g_test_add_func("/ofonostk/request-fields", test_request_fields);
	g_test_add_func("/ofonostk/cancel", test_cancel);
	g_test_add_func("/ofonostk/release-reregisters", test_release_reregisters);
	g_test_add_func("/ofonostk/register-retries", test_register_retries);
	g_test_add_func("/ofonostk/register-gives-up", test_register_gives_up);
	g_test_add_func("/ofonostk/foreign-caller-refused", test_foreign_caller_refused);
	g_test_add_func("/ofonostk/select-item-session", test_select_item_session);
	g_test_add_func("/ofonostk/select-item-busy", test_select_item_busy);
	g_test_add_func("/ofonostk/free-with-pending", test_free_with_pending);
	g_test_add_func("/ofonostk/core-timeout-over-dbus", test_core_timeout_over_dbus);

	ret = g_test_run();

	g_dbus_connection_unregister_object(fake.conn, fake.object_id);
	g_dbus_node_info_unref(node);
	g_object_unref(fake.conn);
	if (fake.reply)
		g_variant_unref(fake.reply);
	g_free(fake.reply_error);
	g_free(fake.agent_path);
	g_free(fake.agent_bus);
	g_free(fake.select_agent_path);

	g_test_dbus_down(bus);
	g_object_unref(bus);

	return ret;
}

// vim:ts=4:sw=4:noexpandtab
