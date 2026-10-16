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

#include <glib.h>
#include <string.h>

#include "stkcore.h"

/* What the fake "ofono side" saw. */
struct recorder {
	int replies;
	enum stk_reply_kind kind;
	enum stk_answer_kind answer;
	guint8 index;
	gchar *string;
	bool boolean;
	guint replied_id;

	int requests;
	int ended;
	enum stk_end_reason last_reason;
	guint last_ended_id;

	/* when set, the reply callback submits this request (re-entrancy) */
	struct stk_core *core;
	struct stk_request *resubmit;

	GMainLoop *loop;
	bool guard_fired;
};

static void record_reply(void *token, const struct stk_request *request, const struct stk_reply *reply)
{
	struct recorder *rec = token;
	struct stk_request *next;

	rec->replies++;
	rec->kind = reply->kind;
	rec->answer = reply->answer;
	rec->index = reply->index;
	g_free(rec->string);
	rec->string = g_strdup(reply->string);
	rec->boolean = reply->boolean;
	rec->replied_id = request->id;

	if (rec->resubmit) {
		next = rec->resubmit;
		rec->resubmit = NULL;
		stk_core_submit(rec->core, next, record_reply, rec);
	}
}

static void record_event(void *data, enum stk_event event, const struct stk_request *request,
                         enum stk_end_reason reason)
{
	struct recorder *rec = data;

	if (event == STK_EVENT_REQUEST) {
		rec->requests++;
		return;
	}

	rec->ended++;
	rec->last_reason = reason;
	rec->last_ended_id = request->id;

	if (rec->loop)
		g_main_loop_quit(rec->loop);
}

static void recorder_clear(struct recorder *rec)
{
	g_free(rec->string);
	if (rec->loop)
		g_main_loop_unref(rec->loop);
	memset(rec, 0, sizeof(*rec));
}

static struct stk_answer string_answer(const char *s)
{
	struct stk_answer a;

	memset(&a, 0, sizeof(a));
	a.has_string = true;
	a.string = s;
	a.string_len = strlen(s);

	return a;
}

static struct stk_answer index_answer(gint64 index)
{
	struct stk_answer a;

	memset(&a, 0, sizeof(a));
	a.has_index = true;
	a.index = index;

	return a;
}

static struct stk_answer bool_answer(bool value)
{
	struct stk_answer a;

	memset(&a, 0, sizeof(a));
	a.has_boolean = true;
	a.boolean = value;

	return a;
}

static struct stk_request* selection_request(int sim_id, guint n_items)
{
	struct stk_request *req = stk_request_new(STK_REQUEST_SELECTION, sim_id);
	guint n;

	req->text = g_strdup("Services");
	req->items = g_new0(struct stk_menu_item, n_items);
	req->n_items = n_items;
	for (n = 0; n < n_items; n++)
		req->items[n].label = g_strdup_printf("Item %u", n);

	return req;
}

static struct stk_request* input_request(enum stk_request_type type, guint min, guint max, bool hidden)
{
	struct stk_request *req = stk_request_new(type, 0);

	req->text = g_strdup("Enter");
	req->min_length = min;
	req->max_length = max;
	req->hide_typing = hidden;

	return req;
}

/* Submit, answer, and report whether the answer was accepted. */
static bool try_answer(struct stk_request *req, const char *action, const struct stk_answer *answer,
                       struct recorder *rec, const char **error_text)
{
	struct stk_core *core = stk_core_new(record_event, rec);
	guint id;
	bool ok;

	stk_core_set_timeout(core, 0);
	id = stk_core_submit(core, req, record_reply, rec);
	ok = stk_core_respond(core, id, action, answer, error_text);

	/* a refused answer leaves the request pending */
	g_assert_cmpuint(stk_core_pending_count(core), ==, ok ? 0 : 1);

	stk_core_free(core);

	return ok;
}

/* ------------------------------------------------------------------------ */

static void test_type_table(void)
{
	/* answer kinds */
	g_assert_cmpint(stk_request_answer_kind(STK_REQUEST_SELECTION), ==, STK_ANSWER_INDEX);
	g_assert_cmpint(stk_request_answer_kind(STK_REQUEST_DISPLAY_TEXT), ==, STK_ANSWER_NONE);
	g_assert_cmpint(stk_request_answer_kind(STK_REQUEST_INPUT), ==, STK_ANSWER_STRING);
	g_assert_cmpint(stk_request_answer_kind(STK_REQUEST_DIGITS), ==, STK_ANSWER_STRING);
	g_assert_cmpint(stk_request_answer_kind(STK_REQUEST_KEY), ==, STK_ANSWER_STRING);
	g_assert_cmpint(stk_request_answer_kind(STK_REQUEST_DIGIT), ==, STK_ANSWER_STRING);
	g_assert_cmpint(stk_request_answer_kind(STK_REQUEST_QUICK_DIGIT), ==, STK_ANSWER_STRING);
	g_assert_cmpint(stk_request_answer_kind(STK_REQUEST_CONFIRMATION), ==, STK_ANSWER_BOOLEAN);
	g_assert_cmpint(stk_request_answer_kind(STK_REQUEST_CONFIRM_CALL_SETUP), ==, STK_ANSWER_BOOLEAN);
	g_assert_cmpint(stk_request_answer_kind(STK_REQUEST_PLAY_TONE), ==, STK_ANSWER_NONE);
	g_assert_cmpint(stk_request_answer_kind(STK_REQUEST_LOOP_TONE), ==, STK_ANSWER_NONE);
	g_assert_cmpint(stk_request_answer_kind(STK_REQUEST_DISPLAY_ACTION_INFORMATION), ==, STK_ANSWER_NONE);
	g_assert_cmpint(stk_request_answer_kind(STK_REQUEST_CONFIRM_LAUNCH_BROWSER), ==, STK_ANSWER_BOOLEAN);
	g_assert_cmpint(stk_request_answer_kind(STK_REQUEST_DISPLAY_ACTION), ==, STK_ANSWER_NONE);
	g_assert_cmpint(stk_request_answer_kind(STK_REQUEST_CONFIRM_OPEN_CHANNEL), ==, STK_ANSWER_BOOLEAN);

	/* the errors ofono 2.19 accepts per method */
	g_assert_cmpuint(stk_request_allowed_errors(STK_REQUEST_DISPLAY_TEXT), ==,
					 STK_ALLOW_GO_BACK | STK_ALLOW_END_SESSION | STK_ALLOW_BUSY);
	g_assert_cmpuint(stk_request_allowed_errors(STK_REQUEST_SELECTION), ==,
					 STK_ALLOW_GO_BACK | STK_ALLOW_END_SESSION);
	g_assert_cmpuint(stk_request_allowed_errors(STK_REQUEST_CONFIRM_CALL_SETUP), ==, STK_ALLOW_END_SESSION);
	g_assert_cmpuint(stk_request_allowed_errors(STK_REQUEST_PLAY_TONE), ==, STK_ALLOW_END_SESSION);
	g_assert_cmpuint(stk_request_allowed_errors(STK_REQUEST_DISPLAY_ACTION_INFORMATION), ==, 0);
	g_assert_cmpuint(stk_request_allowed_errors(STK_REQUEST_CONFIRM_LAUNCH_BROWSER), ==, 0);
	g_assert_cmpuint(stk_request_allowed_errors(STK_REQUEST_CONFIRM_OPEN_CHANNEL), ==, STK_ALLOW_END_SESSION);

	g_assert_cmpstr(stk_request_type_to_string(STK_REQUEST_QUICK_DIGIT), ==, "quickDigit");
	g_assert_cmpstr(stk_request_type_to_string(STK_REQUEST_TYPE_COUNT), ==, "unknown");
	g_assert_cmpstr(stk_request_type_to_string((enum stk_request_type) -1), ==, "unknown");
	g_assert_cmpstr(stk_end_reason_to_string(STK_END_TIMEOUT), ==, "timeout");
}

static void test_selection(void)
{
	struct recorder rec = { 0 };
	struct stk_answer a;
	const char *err = NULL;

	a = index_answer(2);
	g_assert_true(try_answer(selection_request(0, 3), "answer", &a, &rec, &err));
	g_assert_cmpint(rec.replies, ==, 1);
	g_assert_cmpint(rec.kind, ==, STK_REPLY_ANSWER);
	g_assert_cmpint(rec.answer, ==, STK_ANSWER_INDEX);
	g_assert_cmpuint(rec.index, ==, 2);

	a = index_answer(3);
	g_assert_false(try_answer(selection_request(0, 3), "answer", &a, &rec, &err));
	g_assert_cmpstr(err, ==, "Item index out of range");

	a = index_answer(-1);
	g_assert_false(try_answer(selection_request(0, 3), "answer", &a, &rec, &err));

	/* a string is not an index */
	a = string_answer("1");
	g_assert_false(try_answer(selection_request(0, 3), "answer", &a, &rec, &err));
	g_assert_cmpstr(err, ==, "An item index is expected");

	/* no answer at all */
	g_assert_false(try_answer(selection_request(0, 3), "answer", NULL, &rec, &err));

	/* refused answers were fallback-replied by stk_core_free() only */
	recorder_clear(&rec);
}

static void test_input_lengths(void)
{
	struct recorder rec = { 0 };
	struct stk_answer a;
	const char *err = NULL;

	/* two characters, four bytes: input counts characters */
	a = string_answer("\xc3\xa4\xc3\xb6");
	g_assert_true(try_answer(input_request(STK_REQUEST_INPUT, 1, 2, false), "answer", &a, &rec, &err));
	g_assert_cmpstr(rec.string, ==, "\xc3\xa4\xc3\xb6");

	a = string_answer("abc");
	g_assert_false(try_answer(input_request(STK_REQUEST_INPUT, 1, 2, false), "answer", &a, &rec, &err));
	g_assert_cmpstr(err, ==, "Input length out of range");

	a = string_answer("");
	g_assert_false(try_answer(input_request(STK_REQUEST_INPUT, 1, 2, false), "answer", &a, &rec, &err));
	g_assert_true(try_answer(input_request(STK_REQUEST_INPUT, 0, 2, false), "answer", &a, &rec, &err));

	/* invalid UTF-8 never reaches D-Bus */
	a = string_answer("\xff\xfe");
	g_assert_false(try_answer(input_request(STK_REQUEST_INPUT, 0, 10, false), "answer", &a, &rec, &err));
	g_assert_cmpstr(err, ==, "Invalid UTF-8");

	/* embedded NUL: the byte length disagrees with strlen */
	a = string_answer("ab");
	a.string_len = 3;
	a.string = "ab\0c";
	g_assert_false(try_answer(input_request(STK_REQUEST_INPUT, 0, 10, false), "answer", &a, &rec, &err));
	g_assert_cmpstr(err, ==, "Embedded NUL characters are not allowed");

	recorder_clear(&rec);
}

static void test_digits(void)
{
	struct recorder rec = { 0 };
	struct stk_answer a;
	const char *err = NULL;

	a = string_answer("12*#+");
	g_assert_true(try_answer(input_request(STK_REQUEST_DIGITS, 1, 8, false), "answer", &a, &rec, &err));

	/* hidden entry refuses '+' like ofono does */
	g_assert_false(try_answer(input_request(STK_REQUEST_DIGITS, 1, 8, true), "answer", &a, &rec, &err));
	g_assert_cmpstr(err, ==, "Only digits, * and # are allowed");

	a = string_answer("12a");
	g_assert_false(try_answer(input_request(STK_REQUEST_DIGITS, 1, 8, false), "answer", &a, &rec, &err));

	a = string_answer("123456789");
	g_assert_false(try_answer(input_request(STK_REQUEST_DIGITS, 1, 8, false), "answer", &a, &rec, &err));
	g_assert_cmpstr(err, ==, "Input length out of range");

	a = string_answer("1");
	g_assert_false(try_answer(input_request(STK_REQUEST_DIGITS, 4, 8, false), "answer", &a, &rec, &err));

	recorder_clear(&rec);
}

static void test_single_keys(void)
{
	struct recorder rec = { 0 };
	struct stk_answer a;
	const char *err = NULL;

	/* key: one character, which may be several bytes */
	a = string_answer("\xe2\x82\xac");
	g_assert_true(try_answer(input_request(STK_REQUEST_KEY, 0, 0, false), "answer", &a, &rec, &err));
	a = string_answer("ab");
	g_assert_false(try_answer(input_request(STK_REQUEST_KEY, 0, 0, false), "answer", &a, &rec, &err));
	a = string_answer("");
	g_assert_false(try_answer(input_request(STK_REQUEST_KEY, 0, 0, false), "answer", &a, &rec, &err));

	/* digit */
	a = string_answer("+");
	g_assert_true(try_answer(input_request(STK_REQUEST_DIGIT, 0, 0, false), "answer", &a, &rec, &err));
	a = string_answer("x");
	g_assert_false(try_answer(input_request(STK_REQUEST_DIGIT, 0, 0, false), "answer", &a, &rec, &err));
	a = string_answer("12");
	g_assert_false(try_answer(input_request(STK_REQUEST_DIGIT, 0, 0, false), "answer", &a, &rec, &err));

	/* quick digit: no '+' */
	a = string_answer("#");
	g_assert_true(try_answer(input_request(STK_REQUEST_QUICK_DIGIT, 0, 0, false), "answer", &a, &rec, &err));
	a = string_answer("+");
	g_assert_false(try_answer(input_request(STK_REQUEST_QUICK_DIGIT, 0, 0, false), "answer", &a, &rec, &err));

	recorder_clear(&rec);
}

static void test_booleans_and_empty(void)
{
	struct recorder rec = { 0 };
	struct stk_answer a;
	const char *err = NULL;

	a = bool_answer(true);
	g_assert_true(try_answer(stk_request_new(STK_REQUEST_CONFIRM_CALL_SETUP, 0), "answer", &a, &rec, &err));
	g_assert_cmpint(rec.answer, ==, STK_ANSWER_BOOLEAN);
	g_assert_true(rec.boolean);

	a = string_answer("yes");
	g_assert_false(try_answer(stk_request_new(STK_REQUEST_CONFIRMATION, 0), "answer", &a, &rec, &err));
	g_assert_cmpstr(err, ==, "A boolean is expected");

	/* DisplayText takes no value; one given is ignored */
	g_assert_true(try_answer(stk_request_new(STK_REQUEST_DISPLAY_TEXT, 0), "answer", &a, &rec, &err));
	g_assert_cmpint(rec.kind, ==, STK_REPLY_ANSWER);
	g_assert_cmpint(rec.answer, ==, STK_ANSWER_NONE);

	recorder_clear(&rec);
}

static void test_error_actions(void)
{
	struct recorder rec = { 0 };
	const char *err = NULL;

	g_assert_true(try_answer(stk_request_new(STK_REQUEST_DISPLAY_TEXT, 0), "busy", NULL, &rec, &err));
	g_assert_cmpint(rec.kind, ==, STK_REPLY_BUSY);

	g_assert_false(try_answer(stk_request_new(STK_REQUEST_CONFIRMATION, 0), "busy", NULL, &rec, &err));
	g_assert_cmpstr(err, ==, "Busy is not possible here");

	g_assert_true(try_answer(selection_request(0, 2), "goBack", NULL, &rec, &err));
	g_assert_cmpint(rec.kind, ==, STK_REPLY_GO_BACK);

	/* ConfirmCallSetup has no "back" in ofono */
	g_assert_false(try_answer(stk_request_new(STK_REQUEST_CONFIRM_CALL_SETUP, 0), "goBack", NULL, &rec, &err));

	g_assert_true(try_answer(stk_request_new(STK_REQUEST_LOOP_TONE, 0), "endSession", NULL, &rec, &err));
	g_assert_cmpint(rec.kind, ==, STK_REPLY_END_SESSION);

	g_assert_false(try_answer(stk_request_new(STK_REQUEST_DISPLAY_ACTION_INFORMATION, 0), "endSession",
							  NULL, &rec, &err));

	g_assert_false(try_answer(stk_request_new(STK_REQUEST_DISPLAY_TEXT, 0), "explode", NULL, &rec, &err));
	g_assert_cmpstr(err, ==, "Unknown action");

	recorder_clear(&rec);
}

static void test_cancel_action(void)
{
	struct recorder rec = { 0 };
	const char *err = NULL;

	/* where ofono allows it, cancel ends the session */
	g_assert_true(try_answer(stk_request_new(STK_REQUEST_CONFIRM_OPEN_CHANNEL, 0), "cancel", NULL, &rec, &err));
	g_assert_cmpint(rec.kind, ==, STK_REPLY_END_SESSION);

	/* otherwise it declines */
	g_assert_true(try_answer(stk_request_new(STK_REQUEST_CONFIRM_LAUNCH_BROWSER, 0), "cancel", NULL, &rec, &err));
	g_assert_cmpint(rec.kind, ==, STK_REPLY_ANSWER);
	g_assert_cmpint(rec.answer, ==, STK_ANSWER_BOOLEAN);
	g_assert_false(rec.boolean);

	g_assert_true(try_answer(stk_request_new(STK_REQUEST_DISPLAY_ACTION_INFORMATION, 0), "cancel", NULL,
							 &rec, &err));
	g_assert_cmpint(rec.kind, ==, STK_REPLY_ANSWER);
	g_assert_cmpint(rec.answer, ==, STK_ANSWER_NONE);

	recorder_clear(&rec);
}

static void test_ids_and_lookup(void)
{
	struct recorder rec = { 0 };
	struct stk_core *core = stk_core_new(record_event, &rec);
	struct stk_answer a = bool_answer(false);
	const char *err = NULL;
	guint id1, id2;

	stk_core_set_timeout(core, 0);

	id1 = stk_core_submit(core, stk_request_new(STK_REQUEST_CONFIRMATION, 0), record_reply, &rec);
	id2 = stk_core_submit(core, stk_request_new(STK_REQUEST_CONFIRMATION, 1), record_reply, &rec);

	g_assert_cmpuint(id1, !=, 0);
	g_assert_cmpuint(id2, !=, id1);
	g_assert_cmpint(rec.requests, ==, 2);
	g_assert_cmpuint(stk_core_pending_count(core), ==, 2);
	g_assert_nonnull(stk_core_find(core, id1));
	g_assert_cmpint(stk_core_find(core, id2)->sim_id, ==, 1);

	g_assert_false(stk_core_respond(core, 4242, "answer", &a, &err));
	g_assert_cmpstr(err, ==, "No such request");

	g_assert_true(stk_core_respond(core, id1, "answer", &a, &err));
	g_assert_null(stk_core_find(core, id1));
	g_assert_cmpuint(rec.replied_id, ==, id1);
	g_assert_cmpint(rec.last_reason, ==, STK_END_ANSWERED);

	/* answering twice is refused */
	g_assert_false(stk_core_respond(core, id1, "answer", &a, &err));

	stk_core_free(core);
	g_assert_cmpint(rec.replies, ==, 2);
	g_assert_cmpint(rec.last_reason, ==, STK_END_SHUTDOWN);

	recorder_clear(&rec);
}

static void collect_ids(const struct stk_request *request, void *data)
{
	GArray *ids = data;

	g_array_append_val(ids, request->id);
}

static void test_supersede_and_foreach(void)
{
	struct recorder rec = { 0 };
	struct stk_core *core = stk_core_new(record_event, &rec);
	GArray *ids = g_array_new(FALSE, FALSE, sizeof(guint));
	guint a, b, c;

	stk_core_set_timeout(core, 0);

	a = stk_core_submit(core, stk_request_new(STK_REQUEST_DISPLAY_TEXT, 0), record_reply, &rec);
	b = stk_core_submit(core, stk_request_new(STK_REQUEST_DISPLAY_TEXT, 1), record_reply, &rec);
	/* a newer request for SIM 0 replaces the older one */
	c = stk_core_submit(core, stk_request_new(STK_REQUEST_INPUT, 0), record_reply, &rec);

	g_assert_cmpint(rec.replies, ==, 1);
	g_assert_cmpuint(rec.replied_id, ==, a);
	g_assert_cmpint(rec.kind, ==, STK_REPLY_END_SESSION);
	g_assert_cmpint(rec.last_reason, ==, STK_END_SUPERSEDED);

	stk_core_foreach(core, collect_ids, ids);
	g_assert_cmpuint(ids->len, ==, 2);
	g_assert_cmpuint(g_array_index(ids, guint, 0), ==, b);
	g_assert_cmpuint(g_array_index(ids, guint, 1), ==, c);

	/* ofono's Cancel() for SIM 1 */
	stk_core_end_sim(core, 1, STK_END_CANCELLED);
	g_assert_cmpint(rec.replies, ==, 2);
	g_assert_cmpuint(rec.last_ended_id, ==, b);
	g_assert_cmpint(rec.last_reason, ==, STK_END_CANCELLED);
	g_assert_cmpuint(stk_core_pending_count(core), ==, 1);

	/* nothing pending for SIM 3: no-op */
	stk_core_end_sim(core, 3, STK_END_RELEASED);
	g_assert_cmpint(rec.replies, ==, 2);

	stk_core_free(core);
	g_assert_cmpint(rec.replies, ==, 3);
	g_array_free(ids, TRUE);
	recorder_clear(&rec);
}

static void test_end_agent(void)
{
	struct recorder rec = { 0 };
	struct stk_core *core = stk_core_new(record_event, &rec);
	struct stk_request *req;
	guint session_id;

	stk_core_set_timeout(core, 0);

	req = stk_request_new(STK_REQUEST_DISPLAY_TEXT, 0);
	req->session = true;
	session_id = stk_core_submit(core, req, record_reply, &rec);

	/* the default agent's Cancel() leaves the session's request alone */
	stk_core_end_agent(core, 0, false, STK_END_CANCELLED);
	g_assert_cmpint(rec.replies, ==, 0);
	g_assert_nonnull(stk_core_find(core, session_id));

	/* ...and SIM 1's session agent does not touch SIM 0 */
	stk_core_end_agent(core, 1, true, STK_END_RELEASED);
	g_assert_cmpint(rec.replies, ==, 0);

	stk_core_end_agent(core, 0, true, STK_END_RELEASED);
	g_assert_cmpint(rec.replies, ==, 1);
	g_assert_cmpint(rec.last_reason, ==, STK_END_RELEASED);
	g_assert_cmpint(rec.kind, ==, STK_REPLY_END_SESSION);

	stk_core_free(core);
	recorder_clear(&rec);
}

static gboolean guard_cb(gpointer data)
{
	struct recorder *rec = data;

	rec->guard_fired = true;
	g_main_loop_quit(rec->loop);

	return G_SOURCE_REMOVE;
}

static void test_timeout(void)
{
	struct recorder rec = { 0 };
	struct stk_core *core = stk_core_new(record_event, &rec);
	guint id, guard;

	rec.loop = g_main_loop_new(NULL, FALSE);
	stk_core_set_timeout(core, 20);

	/* a 20 ms timeout has to fire well within two seconds */
	guard = g_timeout_add(2000, guard_cb, &rec);

	id = stk_core_submit(core, stk_request_new(STK_REQUEST_INPUT, 0), record_reply, &rec);
	g_main_loop_run(rec.loop);
	g_assert_false(rec.guard_fired);

	g_assert_cmpint(rec.replies, ==, 1);
	g_assert_cmpuint(rec.replied_id, ==, id);
	g_assert_cmpint(rec.kind, ==, STK_REPLY_END_SESSION);
	g_assert_cmpint(rec.last_reason, ==, STK_END_TIMEOUT);
	g_assert_cmpuint(stk_core_pending_count(core), ==, 0);

	/* ConfirmLaunchBrowser takes no error: a timeout declines */
	id = stk_core_submit(core, stk_request_new(STK_REQUEST_CONFIRM_LAUNCH_BROWSER, 0), record_reply, &rec);
	g_main_loop_run(rec.loop);
	g_assert_false(rec.guard_fired);
	g_source_remove(guard);
	g_assert_cmpint(rec.replies, ==, 2);
	g_assert_cmpint(rec.kind, ==, STK_REPLY_ANSWER);
	g_assert_cmpint(rec.answer, ==, STK_ANSWER_BOOLEAN);
	g_assert_false(rec.boolean);

	stk_core_free(core);
	g_assert_cmpint(rec.replies, ==, 2);
	recorder_clear(&rec);
}

static gboolean quit_loop(gpointer data)
{
	g_main_loop_quit(data);
	return G_SOURCE_REMOVE;
}

static void test_answer_stops_timer(void)
{
	struct recorder rec = { 0 };
	struct stk_core *core = stk_core_new(record_event, &rec);
	GMainLoop *loop = g_main_loop_new(NULL, FALSE);
	struct stk_answer a = bool_answer(true);
	guint id;

	stk_core_set_timeout(core, 10);
	id = stk_core_submit(core, stk_request_new(STK_REQUEST_CONFIRMATION, 0), record_reply, &rec);
	g_assert_true(stk_core_respond(core, id, "answer", &a, NULL));

	/* let the old timeout's deadline pass: no second reply, no crash */
	g_timeout_add(50, quit_loop, loop);
	g_main_loop_run(loop);

	g_assert_cmpint(rec.replies, ==, 1);
	g_assert_cmpint(rec.ended, ==, 1);

	stk_core_free(core);
	g_main_loop_unref(loop);
	recorder_clear(&rec);
}

static void test_reentrant_submit(void)
{
	struct recorder rec = { 0 };
	struct stk_core *core = stk_core_new(record_event, &rec);
	struct stk_answer a = index_answer(0);
	guint id;

	stk_core_set_timeout(core, 0);
	rec.core = core;
	rec.resubmit = stk_request_new(STK_REQUEST_DISPLAY_TEXT, 0);

	/* the reply goes out and ofono's next command arrives from within it */
	id = stk_core_submit(core, selection_request(0, 1), record_reply, &rec);
	g_assert_true(stk_core_respond(core, id, "answer", &a, NULL));

	g_assert_cmpuint(stk_core_pending_count(core), ==, 1);
	g_assert_cmpint(rec.requests, ==, 2);
	/* the newer request must not have been superseded by the old one's end */
	g_assert_cmpint(rec.replies, ==, 1);

	/* the same when ofono cancels: what arrives from within the reply stays */
	rec.resubmit = stk_request_new(STK_REQUEST_INPUT, 0);
	stk_core_end_sim(core, 0, STK_END_CANCELLED);
	g_assert_cmpint(rec.replies, ==, 2);
	g_assert_cmpuint(stk_core_pending_count(core), ==, 1);
	g_assert_cmpint(rec.requests, ==, 3);

	stk_core_free(core);
	g_assert_cmpint(rec.replies, ==, 3);
	recorder_clear(&rec);
}

static void test_menu_copy(void)
{
	struct stk_menu menu;
	struct stk_menu_item items[2] = { { "One", 1 }, { "Two", 2 } };
	struct stk_menu *copy;

	memset(&menu, 0, sizeof(menu));
	menu.title = "SIM services";
	menu.icon_id = 7;
	menu.items = items;
	menu.n_items = 2;

	copy = stk_menu_copy(&menu);
	g_assert_cmpstr(copy->title, ==, "SIM services");
	g_assert_cmpuint(copy->icon_id, ==, 7);
	g_assert_cmpuint(copy->n_items, ==, 2);
	g_assert_true(copy->items != items);
	g_assert_cmpstr(copy->items[1].label, ==, "Two");
	g_assert_cmpuint(copy->items[1].icon_id, ==, 2);
	g_assert_null(copy->idle_mode_text);
	stk_menu_free(copy);

	g_assert_null(stk_menu_copy(NULL));
	stk_menu_free(NULL);
	stk_request_free(NULL);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/stkcore/type-table", test_type_table);
	g_test_add_func("/stkcore/selection", test_selection);
	g_test_add_func("/stkcore/input-lengths", test_input_lengths);
	g_test_add_func("/stkcore/digits", test_digits);
	g_test_add_func("/stkcore/single-keys", test_single_keys);
	g_test_add_func("/stkcore/booleans-and-empty", test_booleans_and_empty);
	g_test_add_func("/stkcore/error-actions", test_error_actions);
	g_test_add_func("/stkcore/cancel-action", test_cancel_action);
	g_test_add_func("/stkcore/ids-and-lookup", test_ids_and_lookup);
	g_test_add_func("/stkcore/supersede-and-foreach", test_supersede_and_foreach);
	g_test_add_func("/stkcore/end-agent", test_end_agent);
	g_test_add_func("/stkcore/timeout", test_timeout);
	g_test_add_func("/stkcore/answer-stops-timer", test_answer_stops_timer);
	g_test_add_func("/stkcore/reentrant-submit", test_reentrant_submit);
	g_test_add_func("/stkcore/menu-copy", test_menu_copy);

	return g_test_run();
}

// vim:ts=4:sw=4:noexpandtab
