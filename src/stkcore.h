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
 * SIM toolkit request bookkeeping.
 *
 * ofono hands every user interaction of a SIM application (a menu, a text, a
 * prompt, a confirmation) to a registered org.ofono.SimToolkitAgent as a D-Bus
 * method call and waits for its reply. The daemon owns that agent; this unit
 * keeps the calls that still wait for the user, matches the user interface's
 * answers to them, checks each answer the way ofono will check it, and makes
 * sure every call gets exactly one reply - answered, timed out, cancelled or
 * dropped.
 *
 * It only needs GLib, so it can be tested without D-Bus or luna-service2.
 */

#ifndef STK_CORE_H_
#define STK_CORE_H_

#include <stdbool.h>
#include <glib.h>

/* One value per org.ofono.SimToolkitAgent method that carries a request. */
enum stk_request_type {
	STK_REQUEST_SELECTION = 0,
	STK_REQUEST_DISPLAY_TEXT,
	STK_REQUEST_INPUT,
	STK_REQUEST_DIGITS,
	STK_REQUEST_KEY,
	STK_REQUEST_DIGIT,
	STK_REQUEST_QUICK_DIGIT,
	STK_REQUEST_CONFIRMATION,
	STK_REQUEST_CONFIRM_CALL_SETUP,
	STK_REQUEST_PLAY_TONE,
	STK_REQUEST_LOOP_TONE,
	STK_REQUEST_DISPLAY_ACTION_INFORMATION,
	STK_REQUEST_CONFIRM_LAUNCH_BROWSER,
	STK_REQUEST_DISPLAY_ACTION,
	STK_REQUEST_CONFIRM_OPEN_CHANNEL,
	STK_REQUEST_TYPE_COUNT
};

/* What an "answer" to a request carries back to ofono. */
enum stk_answer_kind {
	STK_ANSWER_NONE = 0,	/* empty method return */
	STK_ANSWER_INDEX,		/* byte: index into the request's items */
	STK_ANSWER_STRING,		/* string */
	STK_ANSWER_BOOLEAN		/* boolean */
};

/* The D-Bus errors ofono accepts instead of an answer; which ones depends on
 * the method (ofono drops the agent on any other error). */
#define STK_ALLOW_GO_BACK		0x1
#define STK_ALLOW_END_SESSION	0x2
#define STK_ALLOW_BUSY			0x4

enum stk_reply_kind {
	STK_REPLY_ANSWER = 0,
	STK_REPLY_GO_BACK,
	STK_REPLY_END_SESSION,
	STK_REPLY_BUSY
};

/* Why a request left the pending set without the user answering it. */
enum stk_end_reason {
	STK_END_ANSWERED = 0,
	STK_END_TIMEOUT,		/* nobody answered in time */
	STK_END_CANCELLED,		/* ofono called Cancel() */
	STK_END_RELEASED,		/* the agent was released / the SIM went away */
	STK_END_SUPERSEDED,		/* a newer request for the same SIM arrived */
	STK_END_SHUTDOWN		/* the daemon is going away */
};

struct stk_menu_item {
	gchar *label;
	guint8 icon_id;
};

struct stk_menu {
	gchar *title;
	guint8 icon_id;
	struct stk_menu_item *items;
	guint n_items;
	/* Idle mode text the SIM asked to show, may be NULL. */
	gchar *idle_mode_text;
};

struct stk_request {
	/* Filled in by stk_core_submit(); unique and never 0. */
	guint id;
	int sim_id;
	enum stk_request_type type;
	/* True when the call came in on the session agent (a menu selection
	 * started it) rather than the default agent (SIM initiated). */
	bool session;

	gchar *text;			/* title / alpha / information */
	guint8 icon_id;

	/* RequestSelection */
	struct stk_menu_item *items;
	guint n_items;
	gint default_item;		/* -1 when the SIM gave none */

	/* RequestInput / RequestDigits */
	gchar *default_text;
	guint min_length;
	guint max_length;
	bool hide_typing;

	/* DisplayText */
	bool urgent;

	/* PlayTone / LoopTone */
	gchar *tone;

	/* ConfirmLaunchBrowser */
	gchar *url;
};

struct stk_reply {
	enum stk_reply_kind kind;
	/* Only for STK_REPLY_ANSWER; the kind matches stk_request_answer_kind(). */
	enum stk_answer_kind answer;
	guint8 index;
	const gchar *string;
	bool boolean;
};

/* A user's answer as it arrives from the UI, before it is checked. */
struct stk_answer {
	bool has_index;
	gint64 index;
	bool has_string;
	const gchar *string;
	gsize string_len;		/* byte length; strings with embedded NUL are refused */
	bool has_boolean;
	bool boolean;
};

/**
 * Sends the reply for one request back to ofono. Called exactly once per
 * submitted request; token is what was handed to stk_core_submit().
 */
typedef void (*stk_reply_fn)(void *token, const struct stk_request *request,
                             const struct stk_reply *reply);

enum stk_event {
	STK_EVENT_REQUEST = 0,	/* a new request waits for the user */
	STK_EVENT_ENDED			/* a request left the pending set (reason says why) */
};

typedef void (*stk_event_fn)(void *data, enum stk_event event, const struct stk_request *request,
                             enum stk_end_reason reason);

struct stk_core;

/* ofono's own agent call timeout is 180 s for most requests; ours sits above
 * it so that ofono's Cancel() normally wins, and only kicks in when ofono
 * never gives up (DisplayActionInformation is sent without a timeout). */
#define STK_CORE_DEFAULT_TIMEOUT_MS	(200 * 1000)

struct stk_core* stk_core_new(stk_event_fn event_fn, void *event_data);
/* Replies to everything still pending (STK_END_SHUTDOWN) and frees the core. */
void stk_core_free(struct stk_core *core);

void stk_core_set_timeout(struct stk_core *core, guint timeout_ms);

/**
 * Take ownership of a request and keep it until it is answered. A request
 * still pending for the same SIM is ended first (STK_END_SUPERSEDED): ofono
 * has only one outstanding agent call per modem, so an older one is stale.
 * Returns the request id.
 */
guint stk_core_submit(struct stk_core *core, struct stk_request *request,
                      stk_reply_fn reply_fn, void *token);

/**
 * Answer a pending request.
 *
 * action is one of "answer", "goBack", "endSession", "busy" or "cancel"
 * ("cancel" ends the session where ofono allows that, otherwise declines).
 * Returns true when the reply went out; otherwise *error_text says why and
 * the request stays pending.
 */
bool stk_core_respond(struct stk_core *core, guint id, const char *action,
                      const struct stk_answer *answer, const char **error_text);

/* End whatever is pending for a SIM (ofono's Cancel()/Release(), modem gone). */
void stk_core_end_sim(struct stk_core *core, int sim_id, enum stk_end_reason reason);

/* Same, limited to the requests that came in on one of the SIM's two agents. */
void stk_core_end_agent(struct stk_core *core, int sim_id, bool session, enum stk_end_reason reason);

const struct stk_request* stk_core_find(const struct stk_core *core, guint id);

/* Calls fn for every pending request, oldest first. fn must not change the core. */
void stk_core_foreach(const struct stk_core *core,
                      void (*fn)(const struct stk_request *request, void *data), void *data);
guint stk_core_pending_count(const struct stk_core *core);

/* Static description of the request types. */
const char* stk_request_type_to_string(enum stk_request_type type);
enum stk_answer_kind stk_request_answer_kind(enum stk_request_type type);
unsigned int stk_request_allowed_errors(enum stk_request_type type);
const char* stk_end_reason_to_string(enum stk_end_reason reason);

/**
 * Check an answer against what ofono will accept for this request, and build
 * the reply. On success the reply's string points into the answer.
 */
bool stk_request_check_answer(const struct stk_request *request, const struct stk_answer *answer,
                              struct stk_reply *reply, const char **error_text);

/* The reply sent when nobody answers: end the session where that is allowed,
 * otherwise the negative / empty answer. */
void stk_request_fallback_reply(const struct stk_request *request, struct stk_reply *reply);

struct stk_request* stk_request_new(enum stk_request_type type, int sim_id);
void stk_request_free(struct stk_request *request);

/* items is copied */
void stk_menu_items_copy(const struct stk_menu_item *items, guint n_items,
                         struct stk_menu_item **out_items);
void stk_menu_items_free(struct stk_menu_item *items, guint n_items);

struct stk_menu* stk_menu_copy(const struct stk_menu *menu);
void stk_menu_free(struct stk_menu *menu);

#endif

// vim:ts=4:sw=4:noexpandtab
