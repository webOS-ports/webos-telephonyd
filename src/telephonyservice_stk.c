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
 * SIM toolkit luna methods.
 *
 *   stkGetMainMenu   {simId?, subscribe?}   the SIM's main menu
 *   stkSelectItem    {simId?, index}        start a session with a menu item
 *   stkAgentRequest  {subscribe: true}      what the SIM asks the user, as it happens
 *   stkRespond       {requestId, action?, value?}
 *
 * The agent itself lives in the driver; requests reach us through
 * telephony_service_stk_request_notify() and are kept in a stk_core until the
 * UI answers them, ofono cancels them or they time out.
 */

#include <string.h>
#include <glib.h>
#include <pbnjson.h>
#include <luna-service2/lunaservice.h>

#include "telephonydriver.h"
#include "telephonyservice.h"
#include "telephonyservice_internal.h"
#include "luna_service_utils.h"
#include "utils.h"
#include "stkcore.h"
#include "stkjson.h"

#define STK_APP_ID				"org.webosports.app.simtoolkit"
#define STK_REQUEST_KEY			"stkAgentRequest"
#define STK_MENU_METHOD			"stkGetMainMenu"
/* createToast takes up to 60 characters */
#define STK_TOAST_TEXT_MAX		60

/* ------------------------------------------------------------------------
 * Main menu
 * ------------------------------------------------------------------------ */

static jvalue_ref build_menu_reply(struct telephony_service *service, int sim_id)
{
	const struct stk_menu *menu = NULL;
	jvalue_ref obj;

	if (sim_id >= 0 && sim_id < TELEPHONY_MAX_SIMS)
		menu = service->stk_menu[sim_id];

	obj = stk_menu_to_json(menu);
	jobject_put(obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
	jobject_put(obj, J_CSTR_TO_JVAL("available"), jboolean_create(menu != NULL));
	telephony_service_add_sim_id(obj, sim_id);

	return obj;
}

void telephony_service_stk_menu_changed_notify(struct telephony_service *service, int sim_id,
                                               const struct stk_menu *menu)
{
	jvalue_ref obj;

	/* stk is NULL once the service is shutting down */
	if (!service || !service->stk || sim_id < 0 || sim_id >= TELEPHONY_MAX_SIMS)
		return;

	stk_menu_free(service->stk_menu[sim_id]);
	service->stk_menu[sim_id] = stk_menu_copy(menu);

	obj = build_menu_reply(service, sim_id);
	telephony_service_post_sim_subscription(service, STK_MENU_METHOD, sim_id, TELEPHONY_SIM_ROLE_VOICE, obj);
	j_release(&obj);
}

bool _service_stk_get_main_menu_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct telephony_service *service = user_data;
	struct luna_service_req_data *req_data;
	jvalue_ref reply_obj;

	req_data = telephony_service_begin_request(service, handle, message, STK_MENU_METHOD,
											   TELEPHONY_SIM_ROLE_VOICE, false, true, NULL);
	if (!req_data)
		return true;

	reply_obj = build_menu_reply(service, req_data->sim_id);
	jobject_put(reply_obj, J_CSTR_TO_JVAL("subscribed"), jboolean_create(req_data->subscribed));

	if (!luna_service_message_validate_and_send(handle, message, reply_obj))
		luna_service_message_reply_error_internal(handle, message);

	j_release(&reply_obj);
	luna_service_req_data_free(req_data);

	return true;
}

/* ------------------------------------------------------------------------
 * stkSelectItem
 * ------------------------------------------------------------------------ */

static const char* select_error_text(int code)
{
	switch (code) {
	case TELEPHONY_ERROR_ALREADY_INPROGRESS:
		return "A SIM toolkit session is already running";
	case TELEPHONY_ERROR_INVALID_ARGUMENT:
		return "No such menu item";
	case TELEPHONY_ERROR_NOT_AVAILABLE:
		return "SIM toolkit is not available";
	case TELEPHONY_ERROR_NOT_IMPLEMENTED:
		return "Not supported by the SIM";
	default:
		break;
	}

	return "Selecting the item failed";
}

static int stk_select_item_finish(const struct telephony_error *error, void *data)
{
	struct luna_service_req_data *req_data = data;
	jvalue_ref reply_obj = jobject_create();

	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(error == NULL));
	telephony_service_add_sim_id(reply_obj, req_data->sim_id);

	if (error) {
		jobject_put(reply_obj, J_CSTR_TO_JVAL("errorCode"), jnumber_create_i32(error->code));
		jobject_put(reply_obj, J_CSTR_TO_JVAL("errorText"), jstring_create(select_error_text(error->code)));
	}

	if (!luna_service_message_validate_and_send(req_data->handle, req_data->message, reply_obj))
		luna_service_message_reply_error_internal(req_data->handle, req_data->message);

	j_release(&reply_obj);
	luna_service_req_data_free(req_data);

	return 0;
}

bool _service_stk_select_item_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct telephony_service *service = user_data;
	struct luna_service_req_data *req_data;
	jvalue_ref parsed_obj = NULL;
	jvalue_ref index_obj = NULL;
	int32_t index = -1;

	if (!service->driver || !service->driver->stk_select_item) {
		luna_service_message_reply_error_not_implemented(handle, message);
		return true;
	}

	parsed_obj = luna_service_message_parse_and_validate(LSMessageGetPayload(message));
	if (jis_null(parsed_obj)) {
		luna_service_message_reply_error_bad_json(handle, message);
		return true;
	}

	if (!jobject_get_exists(parsed_obj, J_CSTR_TO_BUF("index"), &index_obj) || !jis_number(index_obj) ||
		jnumber_get_i32(index_obj, &index) != CONV_OK || index < 0 || index > 255) {
		luna_service_message_reply_custom_error(handle, message, "A valid index is required");
		goto cleanup;
	}

	req_data = telephony_service_begin_parsed_request(service, handle, message, parsed_obj,
													  TELEPHONY_SIM_ROLE_VOICE, false);
	if (!req_data)
		goto cleanup;

	service->driver->stk_select_item(service, req_data->sim_id, index, stk_select_item_finish, req_data);

cleanup:
	j_release(&parsed_obj);

	return true;
}

/* ------------------------------------------------------------------------
 * Agent requests
 * ------------------------------------------------------------------------ */

static unsigned int ui_subscriber_count(struct telephony_service *service)
{
	unsigned int count = 0;

	if (service->palmHandle)
		count += LSSubscriptionGetHandleSubscribersCount(service->palmHandle, "/" STK_REQUEST_KEY);
	if (service->webosHandle)
		count += LSSubscriptionGetHandleSubscribersCount(service->webosHandle, "/" STK_REQUEST_KEY);

	return count;
}

static bool ignore_reply_cb(LSHandle *handle, LSMessage *reply, void *ctx)
{
	return true;
}

/* The app could not be started: leave a toast that opens it. */
static void post_toast(LSHandle *handle, const char *text)
{
	jvalue_ref req = jobject_create();
	jvalue_ref onclick = jobject_create();
	gchar *message;
	glong len;

	len = g_utf8_strlen(text, -1);
	if (len == 0)
		message = g_strdup("The SIM card asks for your attention");
	else if (len > STK_TOAST_TEXT_MAX)
		message = g_utf8_substring(text, 0, STK_TOAST_TEXT_MAX);
	else
		message = g_strdup(text);

	jobject_put(onclick, J_CSTR_TO_JVAL("appId"), jstring_create(STK_APP_ID));

	jobject_put(req, J_CSTR_TO_JVAL("sourceId"), jstring_create("com.palm.telephony"));
	jobject_put(req, J_CSTR_TO_JVAL("message"), jstring_create(message));
	jobject_put(req, J_CSTR_TO_JVAL("onclick"), onclick);

	if (!luna_service_call_validate_and_send(handle, "luna://com.webos.notification/createToast", req,
											 ignore_reply_cb, NULL))
		g_warning("Could not post the SIM toolkit toast");

	j_release(&req);
	g_free(message);
}

static bool launch_reply_cb(LSHandle *handle, LSMessage *reply, void *ctx)
{
	gchar *text = ctx;
	jvalue_ref parsed = NULL;
	jvalue_ref rv = NULL;
	bool launched = false;

	parsed = luna_service_message_parse_and_validate(LSMessageGetPayload(reply));
	if (!jis_null(parsed) && jobject_get_exists(parsed, J_CSTR_TO_BUF("returnValue"), &rv) &&
		jis_boolean(rv))
		jboolean_get(rv, &launched);

	if (!launched) {
		g_warning("Could not launch %s for a SIM toolkit request: %s", STK_APP_ID,
				  LSMessageGetPayload(reply));
		post_toast(handle, text);
	}

	if (!jis_null(parsed))
		j_release(&parsed);
	g_free(text);

	return true;
}

/* Nobody shows the request yet: bring the SIM toolkit app up. */
static void summon_ui(struct telephony_service *service, const struct stk_request *request)
{
	jvalue_ref req;
	jvalue_ref params;
	gchar *text;

	if (!service->palmHandle)
		return;

	req = jobject_create();
	params = jobject_create();
	jobject_put(params, J_CSTR_TO_JVAL("stkRequestId"), jnumber_create_i64(request->id));
	jobject_put(req, J_CSTR_TO_JVAL("id"), jstring_create(STK_APP_ID));
	jobject_put(req, J_CSTR_TO_JVAL("params"), params);

	text = g_strdup(request->text ? request->text : "");

	if (!luna_service_call_validate_and_send(service->palmHandle,
											 "luna://com.webos.service.applicationmanager/launch", req,
											 launch_reply_cb, text)) {
		post_toast(service->palmHandle, text);
		g_free(text);
	}

	j_release(&req);
}

static void stk_event_cb(void *data, enum stk_event event, const struct stk_request *request,
                         enum stk_end_reason reason)
{
	struct telephony_service *service = data;
	jvalue_ref obj;

	/* The daemon is going away; the luna handles may already be gone. */
	if (event == STK_EVENT_ENDED && reason == STK_END_SHUTDOWN)
		return;

	obj = jobject_create();
	jobject_put(obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));

	if (event == STK_EVENT_REQUEST) {
		g_message("SIM toolkit request %u (%s) for SIM %d", request->id,
				  stk_request_type_to_string(request->type), request->sim_id);
		jobject_put(obj, J_CSTR_TO_JVAL("event"), jstring_create("request"));
		jobject_put(obj, J_CSTR_TO_JVAL("request"), stk_request_to_json(request));
	}
	else {
		jobject_put(obj, J_CSTR_TO_JVAL("event"), jstring_create("ended"));
		jobject_put(obj, J_CSTR_TO_JVAL("ended"), stk_ended_to_json(request, reason));
	}

	telephony_service_post_subscription(service, STK_REQUEST_KEY, obj);
	j_release(&obj);

	if (event == STK_EVENT_REQUEST && ui_subscriber_count(service) == 0)
		summon_ui(service, request);
}

void telephony_service_stk_request_notify(struct telephony_service *service, struct stk_request *request,
                                          stk_reply_fn reply_fn, void *token)
{
	struct stk_reply reply;

	if (!service || !service->stk) {
		/* shutting down: answer straight away */
		stk_request_fallback_reply(request, &reply);
		reply_fn(token, request, &reply);
		stk_request_free(request);
		return;
	}

	stk_core_submit(service->stk, request, reply_fn, token);
}

void telephony_service_stk_cancel_notify(struct telephony_service *service, int sim_id, bool session,
                                         bool released)
{
	if (!service || !service->stk)
		return;

	stk_core_end_agent(service->stk, sim_id, session, released ? STK_END_RELEASED : STK_END_CANCELLED);
}

static void append_request(const struct stk_request *request, void *data)
{
	jarray_append((jvalue_ref) data, stk_request_to_json(request));
}

bool _service_stk_agent_request_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct telephony_service *service = user_data;
	jvalue_ref reply_obj;
	jvalue_ref requests;
	bool subscribed;

	subscribed = luna_service_check_for_subscription_with_key(handle, message, "/" STK_REQUEST_KEY);

	requests = jarray_create(NULL);
	stk_core_foreach(service->stk, append_request, requests);

	reply_obj = jobject_create();
	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("subscribed"), jboolean_create(subscribed));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("requests"), requests);

	if (!luna_service_message_validate_and_send(handle, message, reply_obj))
		luna_service_message_reply_error_internal(handle, message);

	j_release(&reply_obj);

	return true;
}

bool _service_stk_respond_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	const struct telephony_service *service = user_data;
	struct stk_respond_params params;
	jvalue_ref parsed_obj;
	const char *error_text = NULL;

	parsed_obj = luna_service_message_parse_and_validate(LSMessageGetPayload(message));

	if (!stk_respond_params_parse(parsed_obj, &params, &error_text) ||
		!stk_core_respond(service->stk, params.request_id, params.action, &params.answer, &error_text))
		luna_service_message_reply_custom_error(handle, message, error_text ? error_text : "Internal error");
	else
		luna_service_message_reply_success(handle, message);

	stk_respond_params_clear(&params);
	if (!jis_null(parsed_obj))
		j_release(&parsed_obj);

	return true;
}

/* ------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------ */

void telephony_service_stk_setup(struct telephony_service *service)
{
	service->stk = stk_core_new(stk_event_cb, service);
}

void telephony_service_stk_shutdown(struct telephony_service *service)
{
	int n;

	if (service->stk) {
		/* replies to everything still pending, without posting to luna */
		stk_core_free(service->stk);
		service->stk = NULL;
	}

	for (n = 0; n < TELEPHONY_MAX_SIMS; n++) {
		stk_menu_free(service->stk_menu[n]);
		service->stk_menu[n] = NULL;
	}
}

// vim:ts=4:sw=4:noexpandtab
