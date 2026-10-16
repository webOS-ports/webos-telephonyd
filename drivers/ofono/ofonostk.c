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

#include <glib-object.h>
#include <gio/gio.h>

#include "utils.h"
#include "ofonostk.h"
#include "ofono-interface.h"

/* The errors ofono's stkagent.c compares replies against. */
#define STK_ERROR_GO_BACK		"org.ofono.Error.GoBack"
#define STK_ERROR_END_SESSION	"org.ofono.Error.EndSession"
#define STK_ERROR_BUSY			"org.ofono.Error.Busy"

#define STK_AGENT_PATH_FMT		"/com/palm/telephony/stk/sim%d/agent"
#define STK_SESSION_PATH_FMT	"/com/palm/telephony/stk/sim%d/session"

static guint retry_first_ms = 1000;
static guint retry_max_attempts = 6;

struct ofono_stk;

/* user_data of the agent handlers: which of the two agents was called */
struct stk_agent_ref {
	struct ofono_stk *stk;
	bool session;
};

struct ofono_stk {
	gchar *path;
	int sim_id;
	OfonoInterfaceSimToolkit *remote;
	GDBusConnection *conn;
	struct ofono_base *base;
	GCancellable *cancellable;

	struct stk_menu menu;

	OfonoInterfaceSimToolkitAgent *agent;
	OfonoInterfaceSimToolkitAgent *session_agent;
	struct stk_agent_ref agent_ref;
	struct stk_agent_ref session_ref;
	gchar *agent_path;
	gchar *session_path;
	bool agent_registered;
	bool register_pending;
	guint retry_source;
	guint retry_attempts;

	struct ofono_stk_callbacks callbacks;
	void *callbacks_data;
};

static void register_agent(struct ofono_stk *stk);

void ofono_stk_set_retry_interval(guint first_ms, guint max_attempts)
{
	retry_first_ms = first_ms;
	retry_max_attempts = max_attempts;
}

/* ------------------------------------------------------------------------
 * Properties
 * ------------------------------------------------------------------------ */

static void parse_menu_items(GVariant *value, struct stk_menu_item **items, guint *n_items)
{
	GVariantIter iter;
	const gchar *label;
	guchar icon;
	guint n = 0;
	gsize count;

	*items = NULL;
	*n_items = 0;

	if (!g_str_equal(g_variant_get_type_string(value), "a(sy)"))
		return;

	count = g_variant_n_children(value);
	if (count == 0)
		return;

	*items = g_new0(struct stk_menu_item, count);

	g_variant_iter_init(&iter, value);
	while (n < count && g_variant_iter_next(&iter, "(&sy)", &label, &icon)) {
		(*items)[n].label = g_strdup(label);
		(*items)[n].icon_id = icon;
		n++;
	}

	*n_items = n;
}

static void update_property(const gchar *name, GVariant *value, void *user_data)
{
	struct ofono_stk *stk = user_data;
	bool menu_changed = true;

	if (g_str_equal(name, "MainMenu")) {
		stk_menu_items_free(stk->menu.items, stk->menu.n_items);
		parse_menu_items(value, &stk->menu.items, &stk->menu.n_items);
	}
	else if (g_str_equal(name, "MainMenuTitle") && g_variant_classify(value) == G_VARIANT_CLASS_STRING) {
		g_free(stk->menu.title);
		stk->menu.title = g_variant_dup_string(value, NULL);
	}
	else if (g_str_equal(name, "MainMenuIcon")) {
		/* doc/stk-api.txt says string, ofono 2.19 sends a byte */
		if (g_variant_classify(value) == G_VARIANT_CLASS_BYTE)
			stk->menu.icon_id = g_variant_get_byte(value);
	}
	else if (g_str_equal(name, "IdleModeText") && g_variant_classify(value) == G_VARIANT_CLASS_STRING) {
		g_free(stk->menu.idle_mode_text);
		stk->menu.idle_mode_text = g_variant_dup_string(value, NULL);
	}
	else {
		menu_changed = false;
	}

	if (menu_changed && stk->callbacks.menu_changed)
		stk->callbacks.menu_changed(&stk->menu, stk->callbacks_data);
}

/* ofono_base calls through void* signatures; calling the generated functions
 * through a cast pointer would be undefined behaviour (clang's
 * -fsanitize=function reports it), so go through wrappers of the right type. */
static void stk_get_properties(void *proxy, GCancellable *cancellable, GAsyncReadyCallback callback,
                               gpointer user_data)
{
	ofono_interface_sim_toolkit_call_get_properties(proxy, cancellable, callback, user_data);
}

static gboolean stk_get_properties_finish(void *proxy, GVariant **properties, GAsyncResult *res,
                                          GError **error)
{
	return ofono_interface_sim_toolkit_call_get_properties_finish(proxy, properties, res, error);
}

static struct ofono_base_funcs stk_base_funcs = {
	.update_property = update_property,
	.get_properties = stk_get_properties,
	.get_properties_finish = stk_get_properties_finish
};

/* ------------------------------------------------------------------------
 * Replies
 * ------------------------------------------------------------------------ */

void ofono_stk_send_reply(void *token, const struct stk_request *request, const struct stk_reply *reply)
{
	GDBusMethodInvocation *invocation = token;

	if (!invocation)
		return;

	switch (reply->kind) {
	case STK_REPLY_GO_BACK:
		g_dbus_method_invocation_return_dbus_error(invocation, STK_ERROR_GO_BACK, "Go back");
		return;
	case STK_REPLY_END_SESSION:
		g_dbus_method_invocation_return_dbus_error(invocation, STK_ERROR_END_SESSION, "End session");
		return;
	case STK_REPLY_BUSY:
		g_dbus_method_invocation_return_dbus_error(invocation, STK_ERROR_BUSY, "Busy");
		return;
	case STK_REPLY_ANSWER:
	default:
		break;
	}

	switch (reply->answer) {
	case STK_ANSWER_INDEX:
		g_dbus_method_invocation_return_value(invocation, g_variant_new("(y)", reply->index));
		break;
	case STK_ANSWER_STRING:
		g_dbus_method_invocation_return_value(invocation,
			g_variant_new("(s)", reply->string ? reply->string : ""));
		break;
	case STK_ANSWER_BOOLEAN:
		g_dbus_method_invocation_return_value(invocation,
			g_variant_new("(b)", reply->boolean ? TRUE : FALSE));
		break;
	case STK_ANSWER_NONE:
	default:
		g_dbus_method_invocation_return_value(invocation, NULL);
		break;
	}
}

/* ------------------------------------------------------------------------
 * Agent methods
 *
 * The "handle-*" signal handlers take the object as gpointer: that is the
 * type the generated marshallers call them with, and calling through any
 * other pointer type is undefined behaviour (clang -fsanitize=function).
 * ------------------------------------------------------------------------ */

/**
 * Only the ofono instance behind our proxy may drive the agent: the D-Bus
 * policy lets any root process send to the interface, and a prompt shown to
 * the user has to come from the SIM.
 */
static bool caller_is_ofono(struct ofono_stk *stk, GDBusMethodInvocation *invocation)
{
	gchar *owner;
	bool ok;

	owner = g_dbus_proxy_get_name_owner(G_DBUS_PROXY(stk->remote));
	ok = owner && g_strcmp0(owner, g_dbus_method_invocation_get_sender(invocation)) == 0;
	g_free(owner);

	if (!ok) {
		g_warning("SIM toolkit agent call from %s refused",
				  g_dbus_method_invocation_get_sender(invocation));
		g_dbus_method_invocation_return_dbus_error(invocation,
			"org.freedesktop.DBus.Error.AccessDenied", "Not ofono");
	}

	return ok;
}

static struct stk_request* begin_request(struct stk_agent_ref *ref, GDBusMethodInvocation *invocation,
                                         enum stk_request_type type, const gchar *text, guchar icon)
{
	struct stk_request *request;

	if (!caller_is_ofono(ref->stk, invocation))
		return NULL;

	request = stk_request_new(type, ref->stk->sim_id);
	request->session = ref->session;
	request->text = g_strdup(text ? text : "");
	request->icon_id = icon;

	return request;
}

static gboolean dispatch(struct stk_agent_ref *ref, GDBusMethodInvocation *invocation,
                         struct stk_request *request)
{
	struct ofono_stk *stk = ref->stk;
	struct stk_reply reply;

	if (!request)
		return TRUE;

	g_message("[STK:%s] %s request on the %s agent", stk->path,
			  stk_request_type_to_string(request->type), ref->session ? "session" : "default");

	if (!stk->callbacks.request) {
		stk_request_fallback_reply(request, &reply);
		ofono_stk_send_reply(invocation, request, &reply);
		stk_request_free(request);
		return TRUE;
	}

	stk->callbacks.request(request, invocation, stk->callbacks_data);

	return TRUE;
}

static gboolean handle_request_selection(gpointer object,
                                         GDBusMethodInvocation *invocation, const gchar *title,
                                         guchar icon, GVariant *items, gint16 default_item,
                                         gpointer user_data)
{
	struct stk_agent_ref *ref = user_data;
	struct stk_request *request;

	request = begin_request(ref, invocation, STK_REQUEST_SELECTION, title, icon);
	if (request) {
		parse_menu_items(items, &request->items, &request->n_items);
		request->default_item = (default_item >= 0 && default_item < (gint) request->n_items) ?
								default_item : -1;
	}

	return dispatch(ref, invocation, request);
}

static gboolean handle_display_text(gpointer object,
                                    GDBusMethodInvocation *invocation, const gchar *text,
                                    guchar icon, gboolean urgent, gpointer user_data)
{
	struct stk_agent_ref *ref = user_data;
	struct stk_request *request;

	request = begin_request(ref, invocation, STK_REQUEST_DISPLAY_TEXT, text, icon);
	if (request)
		request->urgent = urgent;

	return dispatch(ref, invocation, request);
}

static struct stk_request* text_entry_request(struct stk_agent_ref *ref, GDBusMethodInvocation *invocation,
                                              enum stk_request_type type, const gchar *alpha,
                                              guchar icon, const gchar *default_text, guchar min,
                                              guchar max, gboolean hide)
{
	struct stk_request *request;

	request = begin_request(ref, invocation, type, alpha, icon);
	if (request) {
		request->default_text = g_strdup(default_text ? default_text : "");
		request->min_length = min;
		request->max_length = max;
		request->hide_typing = hide;
	}

	return request;
}

static gboolean handle_request_input(gpointer object,
                                     GDBusMethodInvocation *invocation, const gchar *alpha,
                                     guchar icon, const gchar *default_text, guchar min,
                                     guchar max, gboolean hide, gpointer user_data)
{
	struct stk_agent_ref *ref = user_data;

	return dispatch(ref, invocation, text_entry_request(ref, invocation, STK_REQUEST_INPUT, alpha,
														icon, default_text, min, max, hide));
}

static gboolean handle_request_digits(gpointer object,
                                      GDBusMethodInvocation *invocation, const gchar *alpha,
                                      guchar icon, const gchar *default_text, guchar min,
                                      guchar max, gboolean hide, gpointer user_data)
{
	struct stk_agent_ref *ref = user_data;

	return dispatch(ref, invocation, text_entry_request(ref, invocation, STK_REQUEST_DIGITS, alpha,
														icon, default_text, min, max, hide));
}

static gboolean handle_request_key(gpointer object,
                                   GDBusMethodInvocation *invocation, const gchar *alpha,
                                   guchar icon, gpointer user_data)
{
	struct stk_agent_ref *ref = user_data;

	return dispatch(ref, invocation, begin_request(ref, invocation, STK_REQUEST_KEY, alpha, icon));
}

static gboolean handle_request_digit(gpointer object,
                                     GDBusMethodInvocation *invocation, const gchar *alpha,
                                     guchar icon, gpointer user_data)
{
	struct stk_agent_ref *ref = user_data;

	return dispatch(ref, invocation, begin_request(ref, invocation, STK_REQUEST_DIGIT, alpha, icon));
}

static gboolean handle_request_quick_digit(gpointer object,
                                           GDBusMethodInvocation *invocation, const gchar *alpha,
                                           guchar icon, gpointer user_data)
{
	struct stk_agent_ref *ref = user_data;

	return dispatch(ref, invocation, begin_request(ref, invocation, STK_REQUEST_QUICK_DIGIT, alpha, icon));
}

static gboolean handle_request_confirmation(gpointer object,
                                            GDBusMethodInvocation *invocation, const gchar *alpha,
                                            guchar icon, gpointer user_data)
{
	struct stk_agent_ref *ref = user_data;

	return dispatch(ref, invocation, begin_request(ref, invocation, STK_REQUEST_CONFIRMATION, alpha, icon));
}

static gboolean handle_confirm_call_setup(gpointer object,
                                          GDBusMethodInvocation *invocation, const gchar *information,
                                          guchar icon, gpointer user_data)
{
	struct stk_agent_ref *ref = user_data;

	return dispatch(ref, invocation,
					begin_request(ref, invocation, STK_REQUEST_CONFIRM_CALL_SETUP, information, icon));
}

static gboolean handle_tone(struct stk_agent_ref *ref, GDBusMethodInvocation *invocation,
                            enum stk_request_type type, const gchar *tone, const gchar *text,
                            guchar icon)
{
	struct stk_request *request;

	request = begin_request(ref, invocation, type, text, icon);
	if (request)
		request->tone = g_strdup(tone ? tone : "");

	return dispatch(ref, invocation, request);
}

static gboolean handle_play_tone(gpointer object,
                                 GDBusMethodInvocation *invocation, const gchar *tone,
                                 const gchar *text, guchar icon, gpointer user_data)
{
	return handle_tone(user_data, invocation, STK_REQUEST_PLAY_TONE, tone, text, icon);
}

static gboolean handle_loop_tone(gpointer object,
                                 GDBusMethodInvocation *invocation, const gchar *tone,
                                 const gchar *text, guchar icon, gpointer user_data)
{
	return handle_tone(user_data, invocation, STK_REQUEST_LOOP_TONE, tone, text, icon);
}

static gboolean handle_display_action_information(gpointer object,
                                                  GDBusMethodInvocation *invocation, const gchar *text,
                                                  guchar icon, gpointer user_data)
{
	struct stk_agent_ref *ref = user_data;

	return dispatch(ref, invocation,
					begin_request(ref, invocation, STK_REQUEST_DISPLAY_ACTION_INFORMATION, text, icon));
}

static gboolean handle_confirm_launch_browser(gpointer object,
                                              GDBusMethodInvocation *invocation,
                                              const gchar *information, guchar icon,
                                              const gchar *url, gpointer user_data)
{
	struct stk_agent_ref *ref = user_data;
	struct stk_request *request;

	request = begin_request(ref, invocation, STK_REQUEST_CONFIRM_LAUNCH_BROWSER, information, icon);
	if (request)
		request->url = g_strdup(url ? url : "");

	return dispatch(ref, invocation, request);
}

static gboolean handle_display_action(gpointer object,
                                      GDBusMethodInvocation *invocation, const gchar *text,
                                      guchar icon, gpointer user_data)
{
	struct stk_agent_ref *ref = user_data;

	return dispatch(ref, invocation, begin_request(ref, invocation, STK_REQUEST_DISPLAY_ACTION, text, icon));
}

static gboolean handle_confirm_open_channel(gpointer object,
                                            GDBusMethodInvocation *invocation,
                                            const gchar *information, guchar icon, gpointer user_data)
{
	struct stk_agent_ref *ref = user_data;

	return dispatch(ref, invocation,
					begin_request(ref, invocation, STK_REQUEST_CONFIRM_OPEN_CHANNEL, information, icon));
}

static gboolean handle_cancel(gpointer object,
                              GDBusMethodInvocation *invocation, gpointer user_data)
{
	struct stk_agent_ref *ref = user_data;
	struct ofono_stk *stk = ref->stk;

	if (!caller_is_ofono(stk, invocation))
		return TRUE;

	/* [noreply]: completing it sends nothing, but frees the invocation */
	g_dbus_method_invocation_return_value(invocation, NULL);

	g_message("[STK:%s] ofono cancelled the %s agent's request", stk->path,
			  ref->session ? "session" : "default");

	if (stk->callbacks.cancel)
		stk->callbacks.cancel(ref->session, false, stk->callbacks_data);

	return TRUE;
}

static gboolean retry_register_cb(gpointer user_data)
{
	struct ofono_stk *stk = user_data;

	stk->retry_source = 0;
	register_agent(stk);

	return G_SOURCE_REMOVE;
}

static void schedule_register(struct ofono_stk *stk)
{
	guint delay;
	guint n;

	if (stk->retry_source || stk->register_pending || stk->agent_registered)
		return;

	if (stk->retry_attempts >= retry_max_attempts) {
		g_warning("[STK:%s] giving up registering the SIM toolkit agent", stk->path);
		return;
	}

	/* 1x, 2x, 4x, ... the first interval */
	delay = retry_first_ms;
	for (n = 0; n < stk->retry_attempts && delay < 60000; n++)
		delay *= 2;

	stk->retry_attempts++;
	stk->retry_source = g_timeout_add(delay, retry_register_cb, stk);
}

static gboolean handle_release(gpointer object,
                               GDBusMethodInvocation *invocation, gpointer user_data)
{
	struct stk_agent_ref *ref = user_data;
	struct ofono_stk *stk = ref->stk;

	if (!caller_is_ofono(stk, invocation))
		return TRUE;

	g_dbus_method_invocation_return_value(invocation, NULL);

	g_message("[STK:%s] ofono released the %s agent", stk->path, ref->session ? "session" : "default");

	if (stk->callbacks.cancel)
		stk->callbacks.cancel(ref->session, true, stk->callbacks_data);

	if (!ref->session) {
		/* ofono dropped our default agent while SimToolkit is still
		 * there (it does that on a reply it did not like, too): come
		 * back, or SIM initiated commands get rejected from now on. */
		stk->agent_registered = false;
		stk->retry_attempts = 0;
		schedule_register(stk);
	}

	return TRUE;
}

/* ------------------------------------------------------------------------
 * Registration
 * ------------------------------------------------------------------------ */

static void register_agent_cb(GObject *source, GAsyncResult *res, gpointer user_data)
{
	struct ofono_stk *stk = user_data;
	GError *error = NULL;

	if (!ofono_interface_sim_toolkit_call_register_agent_finish(OFONO_INTERFACE_SIM_TOOLKIT(source),
																 res, &error)) {
		if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
			/* stk is already freed */
			g_error_free(error);
			return;
		}

		stk->register_pending = false;
		g_warning("[STK:%s] RegisterAgent failed: %s", stk->path, error->message);
		g_error_free(error);
		schedule_register(stk);
		return;
	}

	stk->register_pending = false;
	stk->agent_registered = true;
	stk->retry_attempts = 0;

	g_message("[STK:%s] SIM toolkit agent registered at %s", stk->path, stk->agent_path);
}

static void register_agent(struct ofono_stk *stk)
{
	if (stk->agent_registered || stk->register_pending)
		return;

	stk->register_pending = true;
	ofono_interface_sim_toolkit_call_register_agent(stk->remote, stk->agent_path, stk->cancellable,
													register_agent_cb, stk);
}

static OfonoInterfaceSimToolkitAgent* export_agent(struct ofono_stk *stk, struct stk_agent_ref *ref,
                                                   const gchar *path)
{
	OfonoInterfaceSimToolkitAgent *agent;
	GDBusConnection *conn;
	GError *error = NULL;

	agent = ofono_interface_sim_toolkit_agent_skeleton_new();

	g_signal_connect(agent, "handle-request-selection", G_CALLBACK(handle_request_selection), ref);
	g_signal_connect(agent, "handle-display-text", G_CALLBACK(handle_display_text), ref);
	g_signal_connect(agent, "handle-request-input", G_CALLBACK(handle_request_input), ref);
	g_signal_connect(agent, "handle-request-digits", G_CALLBACK(handle_request_digits), ref);
	g_signal_connect(agent, "handle-request-key", G_CALLBACK(handle_request_key), ref);
	g_signal_connect(agent, "handle-request-digit", G_CALLBACK(handle_request_digit), ref);
	g_signal_connect(agent, "handle-request-quick-digit", G_CALLBACK(handle_request_quick_digit), ref);
	g_signal_connect(agent, "handle-request-confirmation", G_CALLBACK(handle_request_confirmation), ref);
	g_signal_connect(agent, "handle-confirm-call-setup", G_CALLBACK(handle_confirm_call_setup), ref);
	g_signal_connect(agent, "handle-play-tone", G_CALLBACK(handle_play_tone), ref);
	g_signal_connect(agent, "handle-loop-tone", G_CALLBACK(handle_loop_tone), ref);
	g_signal_connect(agent, "handle-display-action-information",
					 G_CALLBACK(handle_display_action_information), ref);
	g_signal_connect(agent, "handle-confirm-launch-browser", G_CALLBACK(handle_confirm_launch_browser), ref);
	g_signal_connect(agent, "handle-display-action", G_CALLBACK(handle_display_action), ref);
	g_signal_connect(agent, "handle-confirm-open-channel", G_CALLBACK(handle_confirm_open_channel), ref);
	g_signal_connect(agent, "handle-cancel", G_CALLBACK(handle_cancel), ref);
	g_signal_connect(agent, "handle-release", G_CALLBACK(handle_release), ref);

	/* ofono checks the agent's sender, so it has to live on the connection
	 * the RegisterAgent / SelectItem calls go out on. */
	conn = g_dbus_proxy_get_connection(G_DBUS_PROXY(stk->remote));

	if (!g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(agent), conn, path, &error)) {
		g_critical("[STK:%s] cannot export the SIM toolkit agent at %s: %s", stk->path, path,
				   error->message);
		g_error_free(error);
		g_object_unref(agent);
		return NULL;
	}

	return agent;
}

static void unexport_agent(OfonoInterfaceSimToolkitAgent **agent)
{
	if (!*agent)
		return;

	g_dbus_interface_skeleton_unexport(G_DBUS_INTERFACE_SKELETON(*agent));
	g_object_unref(*agent);
	*agent = NULL;
}

struct ofono_stk* ofono_stk_create(const gchar *modem_path, int sim_id,
                                   const struct ofono_stk_callbacks *callbacks, void *data)
{
	struct ofono_stk *stk;
	GError *error = NULL;

	stk = g_try_new0(struct ofono_stk, 1);
	if (!stk)
		return NULL;

	/* ofono has no org.freedesktop.DBus.Properties; GetProperties is read
	 * asynchronously by ofono_base, so skip GDBus' blocking GetAll. */
	stk->remote = ofono_interface_sim_toolkit_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM,
							G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES, "org.ofono", modem_path,
							NULL, &error);
	if (error) {
		g_critical("Unable to initialize proxy for the org.ofono.SimToolkit interface: %s", error->message);
		g_error_free(error);
		g_free(stk);
		return NULL;
	}

	stk->path = g_strdup(modem_path);
	stk->sim_id = sim_id;
	stk->conn = g_object_ref(g_dbus_proxy_get_connection(G_DBUS_PROXY(stk->remote)));
	stk->cancellable = g_cancellable_new();
	if (callbacks)
		stk->callbacks = *callbacks;
	stk->callbacks_data = data;

	stk->agent_path = g_strdup_printf(STK_AGENT_PATH_FMT, sim_id);
	stk->session_path = g_strdup_printf(STK_SESSION_PATH_FMT, sim_id);
	stk->agent_ref.stk = stk;
	stk->agent_ref.session = false;
	stk->session_ref.stk = stk;
	stk->session_ref.session = true;

	stk->agent = export_agent(stk, &stk->agent_ref, stk->agent_path);
	stk->session_agent = export_agent(stk, &stk->session_ref, stk->session_path);

	stk->base = ofono_base_create(&stk_base_funcs, stk->remote, stk);

	if (stk->agent)
		register_agent(stk);

	return stk;
}

void ofono_stk_free(struct ofono_stk *stk)
{
	if (!stk)
		return;

	if (stk->retry_source) {
		g_source_remove(stk->retry_source);
		stk->retry_source = 0;
	}

	/* RegisterAgent / SelectItem replies must not touch us any more */
	g_cancellable_cancel(stk->cancellable);

	if (stk->agent_registered) {
		/* Fire and forget; when the interface is going away ofono has
		 * dropped the agent already and answers with an error nobody
		 * reads. */
		ofono_interface_sim_toolkit_call_unregister_agent(stk->remote, stk->agent_path, NULL, NULL, NULL);
		stk->agent_registered = false;
	}

	/* Whatever the user still has on screen for this SIM is over. */
	if (stk->callbacks.cancel) {
		stk->callbacks.cancel(false, true, stk->callbacks_data);
		stk->callbacks.cancel(true, true, stk->callbacks_data);
	}

	unexport_agent(&stk->agent);
	unexport_agent(&stk->session_agent);

	if (stk->base)
		ofono_base_free(stk->base);

	if (stk->remote)
		g_object_unref(stk->remote);

	/* The replies sent above are only queued. If ours was the last
	 * reference to the connection it would close with them unsent, and
	 * ofono would see "recipient disconnected"; the flush keeps it alive
	 * until they are out. */
	g_dbus_connection_flush(stk->conn, NULL, NULL, NULL);
	g_object_unref(stk->conn);

	g_object_unref(stk->cancellable);

	g_free(stk->menu.title);
	stk_menu_items_free(stk->menu.items, stk->menu.n_items);
	g_free(stk->menu.idle_mode_text);

	g_free(stk->agent_path);
	g_free(stk->session_path);
	g_free(stk->path);
	g_free(stk);
}

const struct stk_menu* ofono_stk_get_menu(struct ofono_stk *stk)
{
	return stk ? &stk->menu : NULL;
}

bool ofono_stk_is_agent_registered(struct ofono_stk *stk)
{
	return stk && stk->agent_registered;
}

const gchar* ofono_stk_get_agent_path(struct ofono_stk *stk)
{
	return stk ? stk->agent_path : NULL;
}

const gchar* ofono_stk_get_session_agent_path(struct ofono_stk *stk)
{
	return stk ? stk->session_path : NULL;
}

/* ------------------------------------------------------------------------
 * SelectItem
 * ------------------------------------------------------------------------ */

static enum ofono_error_type map_remote_error(GError *error)
{
	gchar *name;
	enum ofono_error_type type = OFONO_ERROR_TYPE_FAILED;

	if (!g_dbus_error_is_remote_error(error))
		return type;

	name = g_dbus_error_get_remote_error(error);
	if (!name)
		return type;

	if (g_str_equal(name, "org.ofono.Error.InProgress"))
		type = OFONO_ERROR_TYPE_IN_PROGRESS;
	else if (g_str_equal(name, "org.ofono.Error.NotSupported") ||
			 g_str_equal(name, "org.ofono.Error.NotImplemented"))
		type = OFONO_ERROR_TYPE_NOT_IMPLEMENTED;
	else if (g_str_equal(name, "org.ofono.Error.InvalidArguments"))
		type = OFONO_ERROR_TYPE_INVALID_ARGUMENTS;
	else if (g_str_equal(name, "org.ofono.Error.InvalidFormat"))
		type = OFONO_ERROR_TYPE_INVALID_FORMAT;

	g_free(name);

	return type;
}

static void select_item_cb(GObject *source, GAsyncResult *res, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_base_result_cb cb = cbd->cb;
	struct ofono_error oerr;
	GError *error = NULL;

	/* Finished against the proxy GIO hands back: the ofono_stk may be gone. */
	if (!ofono_interface_sim_toolkit_call_select_item_finish(OFONO_INTERFACE_SIM_TOOLKIT(source),
															 res, &error)) {
		oerr.type = map_remote_error(error);
		oerr.message = error->message;
		cb(&oerr, cbd->data);
		g_error_free(error);
	}
	else {
		cb(NULL, cbd->data);
	}

	g_free(cbd);
}

void ofono_stk_select_item(struct ofono_stk *stk, guint8 index, ofono_base_result_cb cb, void *data)
{
	struct cb_data *cbd;
	struct ofono_error oerr;

	if (!stk || !stk->session_agent) {
		oerr.type = OFONO_ERROR_TYPE_FAILED;
		oerr.message = "SIM toolkit is not available";
		cb(&oerr, data);
		return;
	}

	if (index >= stk->menu.n_items) {
		oerr.type = OFONO_ERROR_TYPE_INVALID_ARGUMENTS;
		oerr.message = "No such menu item";
		cb(&oerr, data);
		return;
	}

	cbd = cb_data_new(cb, data);

	ofono_interface_sim_toolkit_call_select_item(stk->remote, index, stk->session_path,
												 stk->cancellable, select_item_cb, cbd);
}

// vim:ts=4:sw=4:noexpandtab
