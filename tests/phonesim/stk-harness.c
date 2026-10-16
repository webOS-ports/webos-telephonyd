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
 * Drives telephonyd's SIM toolkit agent (ofonostk + stkcore + stkjson, the
 * same code the daemon runs) against a real ofono, without luna-service2:
 * prints the main menu and every agent request as the JSON the UI would
 * get, and answers them like a user would.
 *
 *   stk-harness <modem path> [--select=N] [--pick=I,J,...] [--action=A,B,...]
 *               [--seconds=S]
 *
 *   --select   SelectItem the main menu item N once the menu is there
 *   --pick     indices to answer successive selection requests with
 *              (default 0)
 *   --action   actions for successive requests: answer (default), goBack,
 *              endSession, busy, cancel, ignore (let it time out / be
 *              cancelled by ofono)
 *   --seconds  how long to run (default 20)
 *
 * tests/phonesim/run-phonesim.sh sets up ofono and phonesim to run it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <pbnjson.h>

#include "ofonostk.h"
#include "stkcore.h"
#include "stkjson.h"

static struct stk_core *core;
static struct ofono_stk *stk;
static int select_item = -1;
static bool selected;
static gchar **picks;
static guint n_pick;
static gchar **actions;
static guint n_action;
static int requests_seen;

static void print_json(const char *prefix, jvalue_ref v)
{
	jschema_ref schema = jschema_parse(j_cstr_to_buffer("{}"), DOMOPT_NOOPT, NULL);

	printf("%s %s\n", prefix, jvalue_tostring(v, schema));
	fflush(stdout);
	jschema_release(&schema);
}

static void select_done(struct ofono_error *error, void *data)
{
	if (error)
		printf("SELECT failed: %s\n", error->message ? error->message : "?");
	else
		printf("SELECT ok\n");
	fflush(stdout);
}

static gboolean do_select(gpointer data)
{
	printf("SELECT item %d\n", select_item);
	ofono_stk_select_item(stk, (guint8) select_item, select_done, NULL);
	return G_SOURCE_REMOVE;
}

static void menu_changed(const struct stk_menu *menu, void *data)
{
	jvalue_ref v = stk_menu_to_json(menu);

	print_json("MENU", v);
	j_release(&v);

	if (!selected && select_item >= 0 && menu->n_items > (guint) select_item &&
		ofono_stk_is_agent_registered(stk)) {
		selected = true;
		g_timeout_add(300, do_select, NULL);
	}
}

/* An answer a user could give, within the request's limits. */
static gchar* typical_string(const struct stk_request *r)
{
	guint len;

	switch (r->type) {
	case STK_REQUEST_INPUT:
	case STK_REQUEST_DIGITS:
		if (r->default_text && *r->default_text)
			return g_strdup(r->default_text);
		len = r->min_length > 0 ? r->min_length : 1;
		if (len > r->max_length)
			len = r->max_length;
		return g_strnfill(len, r->type == STK_REQUEST_DIGITS ? '1' : 'x');
	case STK_REQUEST_KEY:
		return g_strdup("y");
	default:
		return g_strdup("1");
	}
}

static gboolean answer_cb(gpointer data)
{
	guint id = GPOINTER_TO_UINT(data);
	const struct stk_request *r = stk_core_find(core, id);
	const char *action;
	struct stk_answer a;
	gchar *s = NULL;
	const char *err = NULL;
	int idx = requests_seen - 1;

	if (!r)
		return G_SOURCE_REMOVE;

	action = (idx >= 0 && (guint) idx < n_action) ? actions[idx] : "answer";
	if (g_str_equal(action, "ignore")) {
		printf("IGNORE request %u\n", id);
		fflush(stdout);
		return G_SOURCE_REMOVE;
	}

	memset(&a, 0, sizeof(a));
	switch (stk_request_answer_kind(r->type)) {
	case STK_ANSWER_INDEX:
		a.has_index = true;
		a.index = 0;
		if (n_pick > 0) {
			static guint next_pick;

			a.index = g_ascii_strtoll(picks[next_pick < n_pick ? next_pick : n_pick - 1], NULL, 10);
			next_pick++;
		}
		break;
	case STK_ANSWER_STRING:
		s = typical_string(r);
		a.has_string = true;
		a.string = s;
		a.string_len = strlen(s);
		break;
	case STK_ANSWER_BOOLEAN:
		a.has_boolean = true;
		a.boolean = true;
		break;
	default:
		break;
	}

	if (stk_core_respond(core, id, action, &a, &err))
		printf("RESPOND %u %s%s%s\n", id, action, s ? " " : "", s ? s : "");
	else
		printf("RESPOND %u %s refused: %s\n", id, action, err);
	fflush(stdout);

	g_free(s);
	return G_SOURCE_REMOVE;
}

static void event_cb(void *data, enum stk_event event, const struct stk_request *request,
                     enum stk_end_reason reason)
{
	jvalue_ref v;

	if (event == STK_EVENT_REQUEST) {
		requests_seen++;
		v = stk_request_to_json(request);
		print_json("REQUEST", v);
		j_release(&v);
		g_timeout_add(200, answer_cb, GUINT_TO_POINTER(request->id));
		return;
	}

	v = stk_ended_to_json(request, reason);
	print_json("ENDED", v);
	j_release(&v);
}

static void request_cb(struct stk_request *request, void *token, void *data)
{
	stk_core_submit(core, request, ofono_stk_send_reply, token);
}

static void cancel_cb(bool session, bool released, void *data)
{
	printf("%s %s agent\n", released ? "RELEASED" : "CANCELLED", session ? "session" : "default");
	fflush(stdout);
	stk_core_end_agent(core, 0, session, released ? STK_END_RELEASED : STK_END_CANCELLED);
}

static gboolean quit_cb(gpointer loop)
{
	g_main_loop_quit(loop);
	return G_SOURCE_REMOVE;
}

static gboolean poll_registered(gpointer data)
{
	const struct stk_menu *menu = ofono_stk_get_menu(stk);

	if (!ofono_stk_is_agent_registered(stk))
		return G_SOURCE_CONTINUE;

	printf("REGISTERED %s\n", ofono_stk_get_agent_path(stk));
	fflush(stdout);

	if (!selected && select_item >= 0 && menu && menu->n_items > (guint) select_item) {
		selected = true;
		g_timeout_add(300, do_select, NULL);
	}

	return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
	static const struct ofono_stk_callbacks callbacks = {
		.menu_changed = menu_changed,
		.request = request_cb,
		.cancel = cancel_cb,
	};
	GMainLoop *loop;
	int seconds = 20;
	int n;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <modem path> [--select=N] [--pick=I,..] [--action=A,..] [--seconds=S]\n",
				argv[0]);
		return 2;
	}

	for (n = 2; n < argc; n++) {
		if (g_str_has_prefix(argv[n], "--select="))
			select_item = atoi(argv[n] + 9);
		else if (g_str_has_prefix(argv[n], "--pick=")) {
			picks = g_strsplit(argv[n] + 7, ",", -1);
			n_pick = g_strv_length(picks);
		}
		else if (g_str_has_prefix(argv[n], "--action=")) {
			actions = g_strsplit(argv[n] + 9, ",", -1);
			n_action = g_strv_length(actions);
		}
		else if (g_str_has_prefix(argv[n], "--seconds="))
			seconds = atoi(argv[n] + 10);
	}

	loop = g_main_loop_new(NULL, FALSE);
	core = stk_core_new(event_cb, NULL);

	stk = ofono_stk_create(argv[1], 0, &callbacks, NULL);
	if (!stk)
		return 1;

	g_timeout_add(100, poll_registered, NULL);
	g_timeout_add_seconds((guint) seconds, quit_cb, loop);
	g_main_loop_run(loop);

	ofono_stk_free(stk);
	stk_core_free(core);
	g_strfreev(picks);
	g_strfreev(actions);
	g_main_loop_unref(loop);

	/* let the UnregisterAgent go out */
	g_main_context_iteration(NULL, FALSE);

	return 0;
}

// vim:ts=4:sw=4:noexpandtab
