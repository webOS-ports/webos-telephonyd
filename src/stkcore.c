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

#include "stkcore.h"

struct stk_pending {
	struct stk_core *core;
	struct stk_request *request;
	stk_reply_fn reply_fn;
	void *token;
	guint timeout_source;
};

struct stk_core {
	/* struct stk_pending*, oldest first */
	GQueue pending;
	guint next_id;
	guint timeout_ms;
	stk_event_fn event_fn;
	void *event_data;
};

/* ------------------------------------------------------------------------
 * Request types
 * ------------------------------------------------------------------------ */

struct stk_type_info {
	const char *name;
	enum stk_answer_kind answer;
	unsigned int allowed_errors;
};

/* The allowed errors are the ones ofono 2.19's src/stkagent.c passes to
 * check_error() for the method; anything else makes ofono drop the agent. */
static const struct stk_type_info type_info[STK_REQUEST_TYPE_COUNT] = {
	[STK_REQUEST_SELECTION] = { "selection", STK_ANSWER_INDEX,
		STK_ALLOW_GO_BACK | STK_ALLOW_END_SESSION },
	[STK_REQUEST_DISPLAY_TEXT] = { "displayText", STK_ANSWER_NONE,
		STK_ALLOW_GO_BACK | STK_ALLOW_END_SESSION | STK_ALLOW_BUSY },
	[STK_REQUEST_INPUT] = { "input", STK_ANSWER_STRING,
		STK_ALLOW_GO_BACK | STK_ALLOW_END_SESSION },
	[STK_REQUEST_DIGITS] = { "digits", STK_ANSWER_STRING,
		STK_ALLOW_GO_BACK | STK_ALLOW_END_SESSION },
	[STK_REQUEST_KEY] = { "key", STK_ANSWER_STRING,
		STK_ALLOW_GO_BACK | STK_ALLOW_END_SESSION },
	[STK_REQUEST_DIGIT] = { "digit", STK_ANSWER_STRING,
		STK_ALLOW_GO_BACK | STK_ALLOW_END_SESSION },
	[STK_REQUEST_QUICK_DIGIT] = { "quickDigit", STK_ANSWER_STRING,
		STK_ALLOW_GO_BACK | STK_ALLOW_END_SESSION },
	[STK_REQUEST_CONFIRMATION] = { "confirmation", STK_ANSWER_BOOLEAN,
		STK_ALLOW_GO_BACK | STK_ALLOW_END_SESSION },
	[STK_REQUEST_CONFIRM_CALL_SETUP] = { "confirmCallSetup", STK_ANSWER_BOOLEAN,
		STK_ALLOW_END_SESSION },
	[STK_REQUEST_PLAY_TONE] = { "playTone", STK_ANSWER_NONE,
		STK_ALLOW_END_SESSION },
	[STK_REQUEST_LOOP_TONE] = { "loopTone", STK_ANSWER_NONE,
		STK_ALLOW_END_SESSION },
	[STK_REQUEST_DISPLAY_ACTION_INFORMATION] = { "displayActionInformation", STK_ANSWER_NONE,
		0 },
	[STK_REQUEST_CONFIRM_LAUNCH_BROWSER] = { "confirmLaunchBrowser", STK_ANSWER_BOOLEAN,
		0 },
	[STK_REQUEST_DISPLAY_ACTION] = { "displayAction", STK_ANSWER_NONE,
		STK_ALLOW_END_SESSION },
	[STK_REQUEST_CONFIRM_OPEN_CHANNEL] = { "confirmOpenChannel", STK_ANSWER_BOOLEAN,
		STK_ALLOW_END_SESSION },
};

static bool type_valid(enum stk_request_type type)
{
	return (int) type >= 0 && type < STK_REQUEST_TYPE_COUNT;
}

const char* stk_request_type_to_string(enum stk_request_type type)
{
	if (!type_valid(type))
		return "unknown";

	return type_info[type].name;
}

enum stk_answer_kind stk_request_answer_kind(enum stk_request_type type)
{
	if (!type_valid(type))
		return STK_ANSWER_NONE;

	return type_info[type].answer;
}

unsigned int stk_request_allowed_errors(enum stk_request_type type)
{
	if (!type_valid(type))
		return 0;

	return type_info[type].allowed_errors;
}

const char* stk_end_reason_to_string(enum stk_end_reason reason)
{
	switch (reason) {
	case STK_END_ANSWERED:
		return "answered";
	case STK_END_TIMEOUT:
		return "timeout";
	case STK_END_CANCELLED:
		return "cancelled";
	case STK_END_RELEASED:
		return "released";
	case STK_END_SUPERSEDED:
		return "superseded";
	case STK_END_SHUTDOWN:
		return "shutdown";
	default:
		break;
	}

	return "unknown";
}

/* ------------------------------------------------------------------------
 * Requests and menus
 * ------------------------------------------------------------------------ */

struct stk_request* stk_request_new(enum stk_request_type type, int sim_id)
{
	struct stk_request *request = g_new0(struct stk_request, 1);

	request->type = type;
	request->sim_id = sim_id;
	request->default_item = -1;

	return request;
}

void stk_menu_items_free(struct stk_menu_item *items, guint n_items)
{
	guint n;

	if (!items)
		return;

	for (n = 0; n < n_items; n++)
		g_free(items[n].label);

	g_free(items);
}

void stk_menu_items_copy(const struct stk_menu_item *items, guint n_items,
                         struct stk_menu_item **out_items)
{
	guint n;

	*out_items = NULL;

	if (!items || n_items == 0)
		return;

	*out_items = g_new0(struct stk_menu_item, n_items);

	for (n = 0; n < n_items; n++) {
		(*out_items)[n].label = g_strdup(items[n].label);
		(*out_items)[n].icon_id = items[n].icon_id;
	}
}

void stk_request_free(struct stk_request *request)
{
	if (!request)
		return;

	g_free(request->text);
	stk_menu_items_free(request->items, request->n_items);
	g_free(request->default_text);
	g_free(request->tone);
	g_free(request->url);
	g_free(request);
}

struct stk_menu* stk_menu_copy(const struct stk_menu *menu)
{
	struct stk_menu *copy;

	if (!menu)
		return NULL;

	copy = g_new0(struct stk_menu, 1);
	copy->title = g_strdup(menu->title);
	copy->icon_id = menu->icon_id;
	copy->n_items = menu->items ? menu->n_items : 0;
	stk_menu_items_copy(menu->items, copy->n_items, &copy->items);
	copy->idle_mode_text = g_strdup(menu->idle_mode_text);

	return copy;
}

void stk_menu_free(struct stk_menu *menu)
{
	if (!menu)
		return;

	g_free(menu->title);
	stk_menu_items_free(menu->items, menu->n_items);
	g_free(menu->idle_mode_text);
	g_free(menu);
}

/* ------------------------------------------------------------------------
 * Answer checks
 * ------------------------------------------------------------------------ */

/* Every byte of s (length len) is in accept. */
static bool all_in_set(const gchar *s, gsize len, const char *accept)
{
	gsize n;

	for (n = 0; n < len; n++) {
		if (s[n] == '\0' || !strchr(accept, s[n]))
			return false;
	}

	return true;
}

static bool check_string(const struct stk_request *request, const gchar *s, gsize len,
                         const char **error_text)
{
	glong chars;

	switch (request->type) {
	case STK_REQUEST_INPUT:
		/* ofono counts UTF-8 characters here */
		chars = g_utf8_strlen(s, (gssize) len);
		if (chars < (glong) request->min_length || chars > (glong) request->max_length) {
			*error_text = "Input length out of range";
			return false;
		}
		return true;

	case STK_REQUEST_DIGITS:
		/* ...and bytes here, which is the same for digits */
		if (len < request->min_length || len > request->max_length) {
			*error_text = "Input length out of range";
			return false;
		}
		if (!all_in_set(s, len, request->hide_typing ? "0123456789*#" : "0123456789*#+")) {
			*error_text = "Only digits, * and # are allowed";
			return false;
		}
		return true;

	case STK_REQUEST_KEY:
		if (len == 0 || g_utf8_strlen(s, (gssize) len) != 1) {
			*error_text = "Exactly one character is expected";
			return false;
		}
		return true;

	case STK_REQUEST_DIGIT:
		if (len != 1 || !all_in_set(s, len, "0123456789*#+")) {
			*error_text = "Exactly one digit is expected";
			return false;
		}
		return true;

	case STK_REQUEST_QUICK_DIGIT:
		/* hidden entry: ofono refuses '+' */
		if (len != 1 || !all_in_set(s, len, "0123456789*#")) {
			*error_text = "Exactly one digit is expected";
			return false;
		}
		return true;

	default:
		break;
	}

	*error_text = "Request does not take a string";
	return false;
}

bool stk_request_check_answer(const struct stk_request *request, const struct stk_answer *answer,
                              struct stk_reply *reply, const char **error_text)
{
	static const struct stk_answer no_answer;
	const char *dummy_error;

	if (!error_text)
		error_text = &dummy_error;

	if (!request || !reply) {
		*error_text = "Internal error";
		return false;
	}

	if (!answer)
		answer = &no_answer;

	memset(reply, 0, sizeof(*reply));
	reply->kind = STK_REPLY_ANSWER;
	reply->answer = stk_request_answer_kind(request->type);

	switch (reply->answer) {
	case STK_ANSWER_NONE:
		return true;

	case STK_ANSWER_INDEX:
		if (!answer->has_index) {
			*error_text = "An item index is expected";
			return false;
		}
		if (answer->index < 0 || answer->index >= (gint64) request->n_items) {
			*error_text = "Item index out of range";
			return false;
		}
		reply->index = (guint8) answer->index;
		return true;

	case STK_ANSWER_BOOLEAN:
		if (!answer->has_boolean) {
			*error_text = "A boolean is expected";
			return false;
		}
		reply->boolean = answer->boolean;
		return true;

	case STK_ANSWER_STRING:
		if (!answer->has_string || !answer->string) {
			*error_text = "A string is expected";
			return false;
		}
		/* answer->string is NUL terminated (jstring_get()); a shorter
		 * strlen() means an embedded NUL. */
		if (strlen(answer->string) != answer->string_len) { /* Flawfinder: ignore */
			*error_text = "Embedded NUL characters are not allowed";
			return false;
		}
		if (!g_utf8_validate(answer->string, (gssize) answer->string_len, NULL)) {
			*error_text = "Invalid UTF-8";
			return false;
		}
		if (!check_string(request, answer->string, answer->string_len, error_text))
			return false;
		reply->string = answer->string;
		return true;

	default:
		break;
	}

	*error_text = "Internal error";
	return false;
}

void stk_request_fallback_reply(const struct stk_request *request, struct stk_reply *reply)
{
	memset(reply, 0, sizeof(*reply));

	if (stk_request_allowed_errors(request->type) & STK_ALLOW_END_SESSION) {
		reply->kind = STK_REPLY_END_SESSION;
		return;
	}

	/* DisplayActionInformation and ConfirmLaunchBrowser take no error at
	 * all: dismiss / decline instead. */
	reply->kind = STK_REPLY_ANSWER;
	reply->answer = stk_request_answer_kind(request->type);
	reply->boolean = false;
	reply->string = "";
	reply->index = 0;
}

/* ------------------------------------------------------------------------
 * Pending set
 * ------------------------------------------------------------------------ */

struct stk_core* stk_core_new(stk_event_fn event_fn, void *event_data)
{
	struct stk_core *core = g_new0(struct stk_core, 1);

	g_queue_init(&core->pending);
	core->next_id = 1;
	core->timeout_ms = STK_CORE_DEFAULT_TIMEOUT_MS;
	core->event_fn = event_fn;
	core->event_data = event_data;

	return core;
}

void stk_core_set_timeout(struct stk_core *core, guint timeout_ms)
{
	if (core)
		core->timeout_ms = timeout_ms;
}

/**
 * Take one request out of the pending set and send its reply. The request is
 * unlinked before anything is called, so the reply and event callbacks may
 * look at, submit to or answer the core without seeing it again.
 */
static void end_pending(struct stk_pending *p, const struct stk_reply *reply,
                        enum stk_end_reason reason)
{
	struct stk_core *core = p->core;

	g_queue_remove(&core->pending, p);

	if (p->timeout_source) {
		g_source_remove(p->timeout_source);
		p->timeout_source = 0;
	}

	if (p->reply_fn)
		p->reply_fn(p->token, p->request, reply);

	if (core->event_fn)
		core->event_fn(core->event_data, STK_EVENT_ENDED, p->request, reason);

	stk_request_free(p->request);
	g_free(p);
}

static void end_with_fallback(struct stk_pending *p, enum stk_end_reason reason)
{
	struct stk_reply reply;

	stk_request_fallback_reply(p->request, &reply);
	end_pending(p, &reply, reason);
}

static gboolean pending_timeout_cb(gpointer user_data)
{
	struct stk_pending *p = user_data;

	p->timeout_source = 0;

	g_message("SIM toolkit request %u (%s) timed out", p->request->id,
			  stk_request_type_to_string(p->request->type));

	end_with_fallback(p, STK_END_TIMEOUT);

	return G_SOURCE_REMOVE;
}

static struct stk_pending* find_pending(const struct stk_core *core, guint id)
{
	const GList *iter;

	for (iter = core->pending.head; iter; iter = iter->next) {
		struct stk_pending *p = iter->data;

		if (p->request->id == id)
			return p;
	}

	return NULL;
}

/* session: 0 or 1 to match the agent, -1 for either */
static bool matches_agent(const struct stk_pending *p, int sim_id, int session)
{
	if (p->request->sim_id != sim_id)
		return false;

	return session < 0 || (int) p->request->session == session;
}

static void end_matching(struct stk_core *core, int sim_id, int session, enum stk_end_reason reason)
{
	GArray *ids;
	const GList *iter;
	struct stk_pending *p;
	guint n;

	if (!core)
		return;

	/* Collect the ids first: ending a request calls out (reply, event), and
	 * a request submitted from there must not be ended along with the old
	 * ones, nor one answered from there be ended twice. */
	ids = g_array_new(FALSE, FALSE, sizeof(guint));

	for (iter = core->pending.head; iter; iter = iter->next) {
		p = iter->data;
		if (matches_agent(p, sim_id, session))
			g_array_append_val(ids, p->request->id);
	}

	for (n = 0; n < ids->len; n++) {
		p = find_pending(core, g_array_index(ids, guint, n));
		if (p)
			end_with_fallback(p, reason);
	}

	g_array_free(ids, TRUE);
}

void stk_core_end_sim(struct stk_core *core, int sim_id, enum stk_end_reason reason)
{
	end_matching(core, sim_id, -1, reason);
}

void stk_core_end_agent(struct stk_core *core, int sim_id, bool session, enum stk_end_reason reason)
{
	end_matching(core, sim_id, session ? 1 : 0, reason);
}

guint stk_core_submit(struct stk_core *core, struct stk_request *request,
                      stk_reply_fn reply_fn, void *token)
{
	struct stk_pending *p;

	g_return_val_if_fail(core != NULL, 0);
	g_return_val_if_fail(request != NULL, 0);

	stk_core_end_sim(core, request->sim_id, STK_END_SUPERSEDED);

	request->id = core->next_id++;
	if (core->next_id == 0)
		core->next_id = 1;

	p = g_new0(struct stk_pending, 1);
	p->core = core;
	p->request = request;
	p->reply_fn = reply_fn;
	p->token = token;

	g_queue_push_tail(&core->pending, p);

	if (core->timeout_ms > 0)
		p->timeout_source = g_timeout_add(core->timeout_ms, pending_timeout_cb, p);

	if (core->event_fn)
		core->event_fn(core->event_data, STK_EVENT_REQUEST, request, STK_END_ANSWERED);

	return request->id;
}

bool stk_core_respond(struct stk_core *core, guint id, const char *action,
                      const struct stk_answer *answer, const char **error_text)
{
	struct stk_pending *p;
	struct stk_reply reply;
	unsigned int allowed;
	const char *dummy_error;

	if (!error_text)
		error_text = &dummy_error;

	if (!core) {
		*error_text = "Internal error";
		return false;
	}

	p = find_pending(core, id);
	if (!p) {
		*error_text = "No such request";
		return false;
	}

	allowed = stk_request_allowed_errors(p->request->type);
	memset(&reply, 0, sizeof(reply));

	if (!action || g_str_equal(action, "answer")) {
		if (!stk_request_check_answer(p->request, answer, &reply, error_text))
			return false;
	}
	else if (g_str_equal(action, "goBack")) {
		if (!(allowed & STK_ALLOW_GO_BACK)) {
			*error_text = "Going back is not possible here";
			return false;
		}
		reply.kind = STK_REPLY_GO_BACK;
	}
	else if (g_str_equal(action, "endSession")) {
		if (!(allowed & STK_ALLOW_END_SESSION)) {
			*error_text = "Ending the session is not possible here";
			return false;
		}
		reply.kind = STK_REPLY_END_SESSION;
	}
	else if (g_str_equal(action, "busy")) {
		if (!(allowed & STK_ALLOW_BUSY)) {
			*error_text = "Busy is not possible here";
			return false;
		}
		reply.kind = STK_REPLY_BUSY;
	}
	else if (g_str_equal(action, "cancel")) {
		stk_request_fallback_reply(p->request, &reply);
	}
	else {
		*error_text = "Unknown action";
		return false;
	}

	end_pending(p, &reply, STK_END_ANSWERED);

	return true;
}

const struct stk_request* stk_core_find(const struct stk_core *core, guint id)
{
	struct stk_pending *p;

	if (!core)
		return NULL;

	p = find_pending(core, id);

	return p ? p->request : NULL;
}

void stk_core_foreach(const struct stk_core *core,
                      void (*fn)(const struct stk_request *request, void *data), void *data)
{
	const GList *iter;

	if (!core || !fn)
		return;

	for (iter = core->pending.head; iter; iter = iter->next) {
		const struct stk_pending *p = iter->data;

		fn(p->request, data);
	}
}

guint stk_core_pending_count(const struct stk_core *core)
{
	return core ? core->pending.length : 0;
}

void stk_core_free(struct stk_core *core)
{
	struct stk_pending *p;

	if (!core)
		return;

	while ((p = g_queue_peek_head(&core->pending)) != NULL)
		end_with_fallback(p, STK_END_SHUTDOWN);

	g_free(core);
}

// vim:ts=4:sw=4:noexpandtab
