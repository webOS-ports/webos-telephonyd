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
 * The SIM toolkit luna methods (src/telephonyservice_stk.c) with the luna bus
 * replaced by recording stubs: what the UI gets back, what goes to the
 * subscribers, when the app is launched and when a toast is posted instead.
 * The per-SIM request helpers of telephonyservice.c are stubbed as well, for
 * a single slot 0; real pbnjson, stkcore and stkjson are used.
 */

#include <stdio.h>
#include <string.h>

#include <glib.h>
#include <pbnjson.h>
#include <luna-service2/lunaservice.h>

#include "telephonyservice.h"
#include "telephonyservice_internal.h"
#include "utils.h"
#include "stkcore.h"

/* ------------------------------------------------------------------------
 * luna-service2 stubs
 * ------------------------------------------------------------------------ */

struct LSHandle {
	const char *name;
};

struct LSMessage {
	gchar *payload;
	bool subscription;
	int refs;
	GPtrArray *replies;		/* gchar* */
};

/* subscription posts: "key payload" */
static GPtrArray *posts;
/* outgoing calls */
struct call {
	gchar *uri;
	gchar *payload;
	LSFilterFunc cb;
	void *ctx;
};
static GPtrArray *calls;
static GPtrArray *subscription_keys;
static unsigned int subscriber_count;

bool LSErrorInit(LSError *error)
{
	memset(error, 0, sizeof(*error));
	return true;
}

void LSErrorFree(LSError *error)
{
}

void LSErrorPrint(LSError *lserror, FILE *out)
{
}

void LSMessageRef(LSMessage *message)
{
	message->refs++;
}

void LSMessageUnref(LSMessage *message)
{
	message->refs--;
}

const char* LSMessageGetPayload(LSMessage *message)
{
	return message->payload;
}

bool LSMessageIsSubscription(LSMessage *message)
{
	return message->subscription;
}

bool LSMessageReply(LSHandle *sh, LSMessage *lsmsg, const char *payload, LSError *lserror)
{
	g_ptr_array_add(lsmsg->replies, g_strdup(payload));
	return true;
}

bool LSSubscriptionAdd(LSHandle *sh, const char *key, LSMessage *message, LSError *lserror)
{
	g_ptr_array_add(subscription_keys, g_strdup(key));
	return true;
}

bool LSSubscriptionProcess(LSHandle *sh, LSMessage *message, bool *subscribed, LSError *lserror)
{
	*subscribed = message->subscription;
	return true;
}

bool LSSubscriptionReply(LSHandle *sh, const char *key, const char *payload, LSError *lserror)
{
	g_ptr_array_add(posts, g_strdup_printf("%s %s", key, payload));
	return true;
}

/* used by luna_service_utils.c, not by the SIM toolkit methods */
bool LSSubscriptionPost(LSHandle *sh, const char *category, const char *method, const char *payload,
                        LSError *lserror)
{
	g_assert_not_reached();
	return false;
}

unsigned int LSSubscriptionGetHandleSubscribersCount(LSHandle *sh, const char *key)
{
	return subscriber_count;
}

bool LSCallOneReply(LSHandle *sh, const char *uri, const char *payload, LSFilterFunc callback, void *ctx,
                    LSMessageToken *ret_token, LSError *lserror)
{
	struct call *c = g_new0(struct call, 1);

	c->uri = g_strdup(uri);
	c->payload = g_strdup(payload);
	c->cb = callback;
	c->ctx = ctx;
	g_ptr_array_add(calls, c);

	return true;
}

static void call_free(gpointer data)
{
	struct call *c = data;

	g_free(c->uri);
	g_free(c->payload);
	g_free(c);
}

static LSMessage* message_new(const char *payload, bool subscription)
{
	LSMessage *m = g_new0(LSMessage, 1);

	m->payload = g_strdup(payload);
	m->subscription = subscription;
	m->replies = g_ptr_array_new_with_free_func(g_free);

	return m;
}

static void message_free(LSMessage *m)
{
	g_assert_cmpint(m->refs, ==, 0);
	g_free(m->payload);
	g_ptr_array_free(m->replies, TRUE);
	g_free(m);
}

static const char* last_reply(LSMessage *m)
{
	g_assert_cmpuint(m->replies->len, >, 0);
	return g_ptr_array_index(m->replies, m->replies->len - 1);
}

/* Answer an outgoing call the way the callee would. */
static void finish_call(struct call *c, const char *reply_payload)
{
	LSMessage *reply = message_new(reply_payload, false);

	c->cb(NULL, reply, c->ctx);
	message_free(reply);
}

/* ------------------------------------------------------------------------
 * telephonyservice.c stubs (one slot, sim 0)
 * ------------------------------------------------------------------------ */

void telephony_service_add_sim_id(jvalue_ref reply_obj, int sim_id)
{
	jobject_put(reply_obj, J_CSTR_TO_JVAL("simId"), jnumber_create_i32(sim_id));
}

static void post(const char *key, jvalue_ref obj)
{
	jschema_ref schema = jschema_parse(j_cstr_to_buffer("{}"), DOMOPT_NOOPT, NULL);

	LSSubscriptionReply(NULL, key, jvalue_tostring(obj, schema), NULL);
	jschema_release(&schema);
}

void telephony_service_post_subscription(struct telephony_service *service, const char *method,
                                         jvalue_ref reply_obj)
{
	gchar *key = g_strdup_printf("/%s", method);

	post(key, reply_obj);
	g_free(key);
}

void telephony_service_post_sim_subscription(struct telephony_service *service, const char *method,
                                             int sim_id, enum telephony_sim_role role, jvalue_ref reply_obj)
{
	gchar *key = g_strdup_printf("/%s/%d", method, sim_id);

	post(key, reply_obj);
	g_free(key);
}

struct luna_service_req_data* telephony_service_begin_request(struct telephony_service *service,
                                                              LSHandle *handle, LSMessage *message,
                                                              const char *method,
                                                              enum telephony_sim_role role,
                                                              bool require_initialized,
                                                              bool subscribable,
                                                              jvalue_ref *parsed_obj)
{
	struct luna_service_req_data *req = luna_service_req_data_new(handle, message);

	req->sim_id = 0;
	req->user_data = service;
	req->subscribed = subscribable && message->subscription;
	if (parsed_obj)
		*parsed_obj = jinvalid();

	return req;
}

struct luna_service_req_data* telephony_service_begin_parsed_request(struct telephony_service *service,
                                                                     LSHandle *handle, LSMessage *message,
                                                                     jvalue_ref parsed_obj,
                                                                     enum telephony_sim_role role,
                                                                     bool require_initialized)
{
	return telephony_service_begin_request(service, handle, message, NULL, role, require_initialized,
										   false, NULL);
}

/* ------------------------------------------------------------------------
 * Driver stub
 * ------------------------------------------------------------------------ */

static int selected_index = -1;
static telephony_result_cb select_cb;
static void *select_data;

static void driver_stk_select_item(struct telephony_service *service, int sim_id, int index,
                                   telephony_result_cb cb, void *data)
{
	selected_index = index;
	select_cb = cb;
	select_data = data;
}

static struct telephony_driver driver = {
	.stk_select_item = driver_stk_select_item,
};

/* The "ofono side" of the requests */
static int replies;
static struct stk_reply last_ofono_reply;
static gchar *last_ofono_string;

static void ofono_reply(void *token, const struct stk_request *request, const struct stk_reply *reply)
{
	replies++;
	last_ofono_reply = *reply;
	g_free(last_ofono_string);
	last_ofono_string = g_strdup(reply->string);
	last_ofono_reply.string = last_ofono_string;
}

/* ------------------------------------------------------------------------
 * Fixture
 * ------------------------------------------------------------------------ */

static struct LSHandle palm = { "com.palm.telephony" };

static struct telephony_service* service_new(void)
{
	struct telephony_service *service = g_new0(struct telephony_service, 1);

	service->driver = &driver;
	service->palmHandle = &palm;
	telephony_service_stk_setup(service);

	posts = g_ptr_array_new_with_free_func(g_free);
	calls = g_ptr_array_new_with_free_func(call_free);
	subscription_keys = g_ptr_array_new_with_free_func(g_free);
	subscriber_count = 1;
	replies = 0;
	selected_index = -1;

	return service;
}

static void service_free(struct telephony_service *service)
{
	telephony_service_stk_shutdown(service);
	g_free(service);
	g_ptr_array_free(posts, TRUE);
	g_ptr_array_free(calls, TRUE);
	g_ptr_array_free(subscription_keys, TRUE);
	g_free(last_ofono_string);
	last_ofono_string = NULL;
}

static const char* last_post(void)
{
	g_assert_cmpuint(posts->len, >, 0);
	return g_ptr_array_index(posts, posts->len - 1);
}

static struct stk_request* input_request(void)
{
	struct stk_request *req = stk_request_new(STK_REQUEST_INPUT, 0);

	req->text = g_strdup("Enter your \"code\"");
	req->min_length = 1;
	req->max_length = 4;

	return req;
}

#define ASSERT_CONTAINS(haystack, needle) \
	do { if (!strstr((haystack), (needle))) \
		g_error("'%s' not found in '%s'", (needle), (haystack)); } while (0)

/* ------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------ */

static void test_agent_request_subscription(void)
{
	struct telephony_service *service = service_new();
	LSMessage *sub = message_new("{\"subscribe\":true}", true);
	LSMessage *query;

	/* a request is already waiting when the UI subscribes */
	telephony_service_stk_request_notify(service, input_request(), ofono_reply, NULL);

	_service_stk_agent_request_cb(&palm, sub, service);
	g_assert_cmpstr(g_ptr_array_index(subscription_keys, 0), ==, "/stkAgentRequest");
	ASSERT_CONTAINS(last_reply(sub), "\"subscribed\":true");
	ASSERT_CONTAINS(last_reply(sub), "\"type\":\"input\"");
	ASSERT_CONTAINS(last_reply(sub), "\"maxLength\":4");
	/* the quotes in the SIM's text arrive escaped */
	ASSERT_CONTAINS(last_reply(sub), "Enter your \\\"code\\\"");

	/* a plain call lists, but does not subscribe */
	query = message_new("{}", false);
	_service_stk_agent_request_cb(&palm, query, service);
	ASSERT_CONTAINS(last_reply(query), "\"subscribed\":false");
	g_assert_cmpuint(subscription_keys->len, ==, 1);

	message_free(sub);
	message_free(query);
	service_free(service);
}

static void test_request_posts_and_respond(void)
{
	struct telephony_service *service = service_new();
	LSMessage *respond;
	guint id;
	gchar *payload;

	telephony_service_stk_request_notify(service, input_request(), ofono_reply, NULL);
	ASSERT_CONTAINS(last_post(), "/stkAgentRequest ");
	ASSERT_CONTAINS(last_post(), "\"event\":\"request\"");
	/* a UI is listening: nothing launched */
	g_assert_cmpuint(calls->len, ==, 0);

	id = 1;
	/* too long for max 4 */
	payload = g_strdup_printf("{\"requestId\":%u,\"value\":\"12345\"}", id);
	respond = message_new(payload, false);
	_service_stk_respond_cb(&palm, respond, service);
	ASSERT_CONTAINS(last_reply(respond), "\"returnValue\":false");
	ASSERT_CONTAINS(last_reply(respond), "Input length out of range");
	g_assert_cmpint(replies, ==, 0);
	message_free(respond);
	g_free(payload);

	payload = g_strdup_printf("{\"requestId\":%u,\"value\":\"1234\"}", id);
	respond = message_new(payload, false);
	_service_stk_respond_cb(&palm, respond, service);
	ASSERT_CONTAINS(last_reply(respond), "\"returnValue\":true");
	g_assert_cmpint(replies, ==, 1);
	g_assert_cmpint(last_ofono_reply.kind, ==, STK_REPLY_ANSWER);
	g_assert_cmpstr(last_ofono_reply.string, ==, "1234");
	ASSERT_CONTAINS(last_post(), "\"event\":\"ended\"");
	ASSERT_CONTAINS(last_post(), "\"reason\":\"answered\"");
	message_free(respond);
	g_free(payload);

	/* stale id */
	respond = message_new("{\"requestId\":1,\"action\":\"endSession\"}", false);
	_service_stk_respond_cb(&palm, respond, service);
	ASSERT_CONTAINS(last_reply(respond), "No such request");
	message_free(respond);

	/* garbage */
	respond = message_new("not json", false);
	_service_stk_respond_cb(&palm, respond, service);
	ASSERT_CONTAINS(last_reply(respond), "\"returnValue\":false");
	message_free(respond);

	service_free(service);
}

static void test_launch_when_nobody_listens(void)
{
	struct telephony_service *service = service_new();
	struct call *c;

	subscriber_count = 0;
	telephony_service_stk_request_notify(service, input_request(), ofono_reply, NULL);

	g_assert_cmpuint(calls->len, ==, 1);
	c = g_ptr_array_index(calls, 0);
	g_assert_cmpstr(c->uri, ==, "luna://com.webos.service.applicationmanager/launch");
	ASSERT_CONTAINS(c->payload, "\"id\":\"org.webosports.app.simtoolkit\"");
	ASSERT_CONTAINS(c->payload, "\"stkRequestId\":1");

	/* the app started: nothing else */
	finish_call(c, "{\"returnValue\":true,\"processId\":\"1\"}");
	g_assert_cmpuint(calls->len, ==, 1);

	service_free(service);
}

static void test_toast_when_launch_fails(void)
{
	struct telephony_service *service = service_new();
	struct stk_request *req = stk_request_new(STK_REQUEST_DISPLAY_TEXT, 0);
	struct call *c;
	GString *longtext = g_string_new(NULL);
	int n;

	for (n = 0; n < 20; n++)
		g_string_append(longtext, "\xc3\xa4" "bcde");	/* five characters, six bytes */
	req->text = g_string_free(longtext, FALSE);

	subscriber_count = 0;
	telephony_service_stk_request_notify(service, req, ofono_reply, NULL);
	g_assert_cmpuint(calls->len, ==, 1);

	finish_call(g_ptr_array_index(calls, 0), "{\"returnValue\":false,\"errorText\":\"not installed\"}");

	g_assert_cmpuint(calls->len, ==, 2);
	c = g_ptr_array_index(calls, 1);
	g_assert_cmpstr(c->uri, ==, "luna://com.webos.notification/createToast");
	ASSERT_CONTAINS(c->payload, "\"sourceId\":\"com.palm.telephony\"");
	ASSERT_CONTAINS(c->payload, "\"appId\":\"org.webosports.app.simtoolkit\"");
	/* cut to 60 characters, on a character boundary */
	ASSERT_CONTAINS(c->payload, "\"message\":\"");
	{
		const char *m = strstr(c->payload, "\"message\":\"") + strlen("\"message\":\"");
		const char *end = strchr(m, '"');
		gchar *msg = g_strndup(m, (gsize) (end - m));

		g_assert_true(g_utf8_validate(msg, -1, NULL));
		g_assert_cmpint(g_utf8_strlen(msg, -1), ==, 60);
		g_free(msg);
	}

	service_free(service);
}

static void test_main_menu(void)
{
	struct telephony_service *service = service_new();
	LSMessage *get = message_new("{\"subscribe\":true}", true);
	struct stk_menu_item items[2] = { { "Balance", 0 }, { "Top up", 0 } };
	struct stk_menu menu = { "Operator", 0, items, 2, NULL };

	_service_stk_get_main_menu_cb(&palm, get, service);
	ASSERT_CONTAINS(last_reply(get), "\"available\":false");
	ASSERT_CONTAINS(last_reply(get), "\"subscribed\":true");
	g_assert_cmpint(get->refs, ==, 0);

	telephony_service_stk_menu_changed_notify(service, 0, &menu);
	ASSERT_CONTAINS(last_post(), "/stkGetMainMenu/0 ");
	ASSERT_CONTAINS(last_post(), "\"available\":true");
	ASSERT_CONTAINS(last_post(), "\"label\":\"Top up\"");

	_service_stk_get_main_menu_cb(&palm, get, service);
	ASSERT_CONTAINS(last_reply(get), "\"title\":\"Operator\"");

	/* the toolkit went away */
	telephony_service_stk_menu_changed_notify(service, 0, NULL);
	ASSERT_CONTAINS(last_post(), "\"available\":false");

	/* out of range slots are ignored */
	telephony_service_stk_menu_changed_notify(service, TELEPHONY_MAX_SIMS, &menu);
	telephony_service_stk_menu_changed_notify(service, -1, &menu);

	message_free(get);
	service_free(service);
}

static void test_select_item(void)
{
	struct telephony_service *service = service_new();
	LSMessage *m;
	struct telephony_error err = { TELEPHONY_ERROR_ALREADY_INPROGRESS };

	m = message_new("{\"index\":300}", false);
	_service_stk_select_item_cb(&palm, m, service);
	ASSERT_CONTAINS(last_reply(m), "A valid index is required");
	g_assert_cmpint(selected_index, ==, -1);
	message_free(m);

	m = message_new("{\"index\":\"1\"}", false);
	_service_stk_select_item_cb(&palm, m, service);
	ASSERT_CONTAINS(last_reply(m), "A valid index is required");
	message_free(m);

	m = message_new("{\"index\":1}", false);
	_service_stk_select_item_cb(&palm, m, service);
	g_assert_cmpint(selected_index, ==, 1);
	g_assert_cmpuint(m->replies->len, ==, 0);
	g_assert_cmpint(m->refs, ==, 1);

	select_cb(&err, select_data);
	ASSERT_CONTAINS(last_reply(m), "\"returnValue\":false");
	ASSERT_CONTAINS(last_reply(m), "already running");
	g_assert_cmpint(m->refs, ==, 0);
	message_free(m);

	m = message_new("{\"index\":0,\"simId\":0}", false);
	_service_stk_select_item_cb(&palm, m, service);
	select_cb(NULL, select_data);
	ASSERT_CONTAINS(last_reply(m), "\"returnValue\":true");
	message_free(m);

	service_free(service);
}

static void test_cancel_and_shutdown(void)
{
	struct telephony_service *service = service_new();
	struct stk_menu menu = { "Operator", 0, NULL, 0, NULL };
	guint n_posts;

	telephony_service_stk_request_notify(service, input_request(), ofono_reply, NULL);

	/* ofono's Cancel() on the other agent does nothing */
	telephony_service_stk_cancel_notify(service, 0, true, false);
	g_assert_cmpint(replies, ==, 0);

	telephony_service_stk_cancel_notify(service, 0, false, false);
	g_assert_cmpint(replies, ==, 1);
	g_assert_cmpint(last_ofono_reply.kind, ==, STK_REPLY_END_SESSION);
	ASSERT_CONTAINS(last_post(), "\"reason\":\"cancelled\"");

	/* shutting down with a request pending: ofono is answered, luna is not touched */
	telephony_service_stk_request_notify(service, input_request(), ofono_reply, NULL);
	n_posts = posts->len;
	telephony_service_stk_shutdown(service);
	g_assert_cmpint(replies, ==, 2);
	g_assert_cmpuint(posts->len, ==, n_posts);

	/* after shutdown: requests are answered straight away, menus dropped */
	telephony_service_stk_request_notify(service, input_request(), ofono_reply, NULL);
	g_assert_cmpint(replies, ==, 3);
	telephony_service_stk_menu_changed_notify(service, 0, &menu);
	telephony_service_stk_cancel_notify(service, 0, false, true);
	g_assert_cmpuint(posts->len, ==, n_posts);

	service_free(service);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	/* the launch-failure path logs a warning on purpose */
	g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);

	g_test_add_func("/telephonyservice-stk/agent-request-subscription", test_agent_request_subscription);
	g_test_add_func("/telephonyservice-stk/request-posts-and-respond", test_request_posts_and_respond);
	g_test_add_func("/telephonyservice-stk/launch-when-nobody-listens", test_launch_when_nobody_listens);
	g_test_add_func("/telephonyservice-stk/toast-when-launch-fails", test_toast_when_launch_fails);
	g_test_add_func("/telephonyservice-stk/main-menu", test_main_menu);
	g_test_add_func("/telephonyservice-stk/select-item", test_select_item);
	g_test_add_func("/telephonyservice-stk/cancel-and-shutdown", test_cancel_and_shutdown);

	return g_test_run();
}

// vim:ts=4:sw=4:noexpandtab
