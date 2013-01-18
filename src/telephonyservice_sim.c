/* @@@LICENSE
*
* Copyright (c) 2012 Simon Busch <morphis@gravedo.de>
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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <glib.h>
#include <pbnjson.h>
#include <luna-service2/lunaservice.h>

#include "telephonysettings.h"
#include "telephonydriver.h"
#include "telephonyservice_internal.h"
#include "utils.h"
#include "luna_service_utils.h"

void telephony_service_sim_status_notify(struct telephony_service *service, enum telephony_sim_status sim_status)
{
	jvalue_ref reply_obj = NULL;
	jvalue_ref extended_obj = NULL;

	reply_obj = jobject_create();
	extended_obj = jobject_create();

	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
	jobject_put(extended_obj, J_CSTR_TO_JVAL("state"), jstring_create(telephony_sim_status_to_string(sim_status)));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("extended"), extended_obj);

	luna_service_post_subscription(service->private_service, "/", "simStatusQuery", reply_obj);

	j_release(&reply_obj);
}

static int _service_sim_status_query_finish(const struct telephony_error *error, enum telephony_sim_status sim_status, void *data)
{
	struct luna_service_req_data *req_data = data;
	jvalue_ref reply_obj = NULL;
	jvalue_ref extended_obj = NULL;
	bool subscribed = false;
	bool success = (error == NULL);

	reply_obj = jobject_create();
	extended_obj = jobject_create();

	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(success));

	/* handle possible subscriptions */
	if (req_data->subscribed)
		jobject_put(reply_obj, J_CSTR_TO_JVAL("subscribed"), jboolean_create(req_data->subscribed));

	if (success) {
		jobject_put(extended_obj, J_CSTR_TO_JVAL("state"),
			jstring_create(telephony_sim_status_to_string(sim_status)));

		jobject_put(reply_obj, J_CSTR_TO_JVAL("extended"), extended_obj);
	}
	else {
		/* FIXME better error message */
		luna_service_message_reply_error_unknown(req_data->handle, req_data->message);
		goto cleanup;
	}

	if(!luna_service_message_validate_and_send(req_data->handle, req_data->message, reply_obj)) {
		luna_service_message_reply_error_internal(req_data->handle, req_data->message);
		goto cleanup;
	}

cleanup:
	j_release(&reply_obj);
	luna_service_req_data_free(req_data);
	return 0;
}

/**
 * @brief Query the current sim status
 **/
bool _service_sim_status_query_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct telephony_service *service = user_data;
	struct luna_service_req_data *req_data = NULL;

	if (!service->initialized) {
		luna_service_message_reply_custom_error(handle, message, "Service not yet successfully initialized.");
		goto cleanup;
	}

	if (!service->driver || !service->driver->sim_status_query) {
		g_warning("No implementation available for service simStatusQuery API method");
		luna_service_message_reply_error_not_implemented(handle, message);
		goto cleanup;
	}

	req_data = luna_service_req_data_new(handle, message);
	req_data->subscribed = luna_service_check_for_subscription_and_process(req_data->handle, req_data->message);

	if (service->driver->sim_status_query(service, _service_sim_status_query_finish, req_data) < 0) {
		g_warning("Failed to process service simStatusQuery request in our driver");
		luna_service_message_reply_custom_error(handle, message, "Failed to query sim card status");
		goto cleanup;
	}

	return true;

cleanup:
	if (req_data)
		luna_service_req_data_free(req_data);

	return true;
}

static int _service_pin1_status_query_finish(const struct telephony_error *error, struct telephony_pin_status* pin_status, void *data)
{
	struct luna_service_req_data *req_data = data;
	jvalue_ref reply_obj = NULL;
	jvalue_ref extended_obj = NULL;
	bool subscribed = false;
	bool success = (error == NULL);

	reply_obj = jobject_create();
	extended_obj = jobject_create();

	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(success));

	/* handle possible subscriptions */
	if (req_data->subscribed)
		jobject_put(reply_obj, J_CSTR_TO_JVAL("subscribed"), jboolean_create(req_data->subscribed));

	if (success) {
		jobject_put(extended_obj, J_CSTR_TO_JVAL("enabled"), jboolean_create(pin_status->enabled));
		jobject_put(extended_obj, J_CSTR_TO_JVAL("pinrequired"), jboolean_create(pin_status->required));
		jobject_put(extended_obj, J_CSTR_TO_JVAL("pukrequired"), jboolean_create(pin_status->puk_required));
		jobject_put(extended_obj, J_CSTR_TO_JVAL("pinpermblocked"), jboolean_create(pin_status->perm_blocked));
		jobject_put(extended_obj, J_CSTR_TO_JVAL("devicelocked"), jboolean_create(pin_status->device_locked));
		jobject_put(extended_obj, J_CSTR_TO_JVAL("pinAttemptsRemaining"), jnumber_create_i32(pin_status->pin_attempts_remaining));
		jobject_put(extended_obj, J_CSTR_TO_JVAL("pukAttemptsRemaining"), jnumber_create_i32(pin_status->puk_attempts_remaining));

		jobject_put(reply_obj, J_CSTR_TO_JVAL("extended"), extended_obj);
	}
	else {
		/* FIXME better error message */
		luna_service_message_reply_error_unknown(req_data->handle, req_data->message);
		goto cleanup;
	}

	if(!luna_service_message_validate_and_send(req_data->handle, req_data->message, reply_obj)) {
		luna_service_message_reply_error_internal(req_data->handle, req_data->message);
		goto cleanup;
	}

cleanup:
	j_release(&reply_obj);
	luna_service_req_data_free(req_data);
	return 0;
}

/**
 * @brief Query the status of the first SIM pin
 **/
bool _service_pin1_status_query_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct telephony_service *service = user_data;
	struct luna_service_req_data *req_data = NULL;

	if (!service->initialized) {
		luna_service_message_reply_custom_error(handle, message, "Service not yet successfully initialized.");
		goto cleanup;
	}

	if (!service->driver || !service->driver->pin1_status_query) {
		g_warning("No implementation available for service simStatusQuery API method");
		luna_service_message_reply_error_not_implemented(handle, message);
		goto cleanup;
	}

	req_data = luna_service_req_data_new(handle, message);
	req_data->subscribed = luna_service_check_for_subscription_and_process(req_data->handle, req_data->message);

	if (service->driver->pin1_status_query(service, _service_pin1_status_query_finish, req_data) < 0) {
		g_warning("Failed to process service simStatusQuery request in our driver");
		luna_service_message_reply_custom_error(handle, message, "Failed to query sim card status");
		goto cleanup;
	}

	return true;

cleanup:
	if (req_data)
		luna_service_req_data_free(req_data);

	return true;
}

// vim:ts=4:sw=4:noexpandtab
