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

static void create_pin1_status_response(jvalue_ref reply_obj, struct telephony_pin_status *pin_status)
{
	jvalue_ref extended_obj;

	extended_obj = jobject_create();

	jobject_put(extended_obj, J_CSTR_TO_JVAL("enabled"), jboolean_create(pin_status->enabled));
	jobject_put(extended_obj, J_CSTR_TO_JVAL("pinrequired"), jboolean_create(pin_status->required));
	jobject_put(extended_obj, J_CSTR_TO_JVAL("pukrequired"), jboolean_create(pin_status->puk_required));
	jobject_put(extended_obj, J_CSTR_TO_JVAL("pinpermblocked"), jboolean_create(pin_status->perm_blocked));
	jobject_put(extended_obj, J_CSTR_TO_JVAL("devicelocked"), jboolean_create(pin_status->device_locked));
	jobject_put(extended_obj, J_CSTR_TO_JVAL("pinAttemptsRemaining"), jnumber_create_i32(pin_status->pin_attempts_remaining));
	jobject_put(extended_obj, J_CSTR_TO_JVAL("pukAttemptsRemaining"), jnumber_create_i32(pin_status->puk_attempts_remaining));

	jobject_put(reply_obj, J_CSTR_TO_JVAL("extended"), extended_obj);
}

void telephony_service_pin1_status_changed_notify(struct telephony_service *service, struct telephony_pin_status *pin_status)
{
	jvalue_ref reply_obj = NULL;

	reply_obj = jobject_create();
	create_pin1_status_response(reply_obj, pin_status);

	luna_service_post_subscription(service->private_service, "/", "pin1StatusQuery", reply_obj);

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
		create_pin1_status_response(reply_obj, pin_status);
		if (!luna_service_message_validate_and_send(req_data->handle, req_data->message, reply_obj)) {
			luna_service_message_reply_error_internal(req_data->handle, req_data->message);
		}
	}
	else {
		/* FIXME better error message */
		luna_service_message_reply_error_unknown(req_data->handle, req_data->message);
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

/**
 * @brief Send the PIN1 to the SIM card for verification
 **/
bool _service_pin1_verify_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct telephony_service *service = user_data;
	struct luna_service_req_data *req_data = NULL;
	jvalue_ref parsed_obj = NULL;
	jvalue_ref pin_obj = NULL;
	const char *payload;
	raw_buffer pin_buf;

	if (!service->initialized) {
		luna_service_message_reply_custom_error(handle, message, "Service not yet successfully initialized.");
		goto cleanup;
	}

	if (!service->driver || !service->driver->pin1_verify) {
		g_warning("No implementation available for service pin1Verify API method");
		luna_service_message_reply_error_not_implemented(handle, message);
		goto cleanup;
	}

	payload = LSMessageGetPayload(message);
	parsed_obj = luna_service_message_parse_and_validate(payload);
	if (jis_null(parsed_obj)) {
		luna_service_message_reply_error_bad_json(handle, message);
		goto cleanup;
	}

	if (!jobject_get_exists(parsed_obj, J_CSTR_TO_BUF("pin"), &pin_obj)) {
		luna_service_message_reply_error_bad_json(handle, message);
		goto cleanup;
	}

	pin_buf = jstring_get(pin_obj);

	req_data = luna_service_req_data_new(handle, message);

	if (service->driver->pin1_verify(service, pin_buf.m_str, telephonyservice_common_finish, req_data) < 0) {
		g_warning("Failed to process service pin1Verify request in our driver");
		luna_service_message_reply_custom_error(handle, message, "Failed send PIN1 for verification to the SIM card");
		goto cleanup;
	}

	return true;

cleanup:
	if (req_data)
		luna_service_req_data_free(req_data);

	return true;
}

/**
 * @brief Enable PIN1 verification for the SIM card.
 *
 * JSON format:
 *  { "pin": "<pin code>" }
 **/
bool _service_pin1_enable_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct telephony_service *service = user_data;
	struct luna_service_req_data *req_data = NULL;
	jvalue_ref parsed_obj = NULL;
	jvalue_ref pin_obj = NULL;
	const char *payload;
	raw_buffer pin_buf;

	if (!service->initialized) {
		luna_service_message_reply_custom_error(handle, message, "Service not yet successfully initialized.");
		goto cleanup;
	}

	if (!service->driver || !service->driver->pin1_enable) {
		g_warning("No implementation available for service pin1Enable API method");
		luna_service_message_reply_error_not_implemented(handle, message);
		goto cleanup;
	}

	payload = LSMessageGetPayload(message);
	parsed_obj = luna_service_message_parse_and_validate(payload);
	if (jis_null(parsed_obj)) {
		luna_service_message_reply_error_bad_json(handle, message);
		goto cleanup;
	}

	if (!jobject_get_exists(parsed_obj, J_CSTR_TO_BUF("pin"), &pin_obj)) {
		luna_service_message_reply_error_bad_json(handle, message);
		goto cleanup;
	}

	pin_buf = jstring_get(pin_obj);

	req_data = luna_service_req_data_new(handle, message);

	if (service->driver->pin1_enable(service, pin_buf.m_str, telephonyservice_common_finish, req_data) < 0) {
		g_warning("Failed to process service pin1Verify request in our driver");
		luna_service_message_reply_custom_error(handle, message, "Failed enable PIN1 on the SIM card");
		goto cleanup;
	}

	return true;

cleanup:
	if (req_data)
		luna_service_req_data_free(req_data);

	return true;
}

/**
 * @brief Disable PIN1 verification for the SIM card.
 *
 * JSON format:
 *  { "pin": "<pin code>" }
 **/
bool _service_pin1_disable_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct telephony_service *service = user_data;
	struct luna_service_req_data *req_data = NULL;
	jvalue_ref parsed_obj = NULL;
	jvalue_ref pin_obj = NULL;
	const char *payload;
	raw_buffer pin_buf;

	if (!service->initialized) {
		luna_service_message_reply_custom_error(handle, message, "Service not yet successfully initialized.");
		goto cleanup;
	}

	if (!service->driver || !service->driver->pin1_disable) {
		g_warning("No implementation available for service pin1Disable API method");
		luna_service_message_reply_error_not_implemented(handle, message);
		goto cleanup;
	}

	payload = LSMessageGetPayload(message);
	parsed_obj = luna_service_message_parse_and_validate(payload);
	if (jis_null(parsed_obj)) {
		luna_service_message_reply_error_bad_json(handle, message);
		goto cleanup;
	}

	if (!jobject_get_exists(parsed_obj, J_CSTR_TO_BUF("pin"), &pin_obj)) {
		luna_service_message_reply_error_bad_json(handle, message);
		goto cleanup;
	}

	pin_buf = jstring_get(pin_obj);

	req_data = luna_service_req_data_new(handle, message);

	if (service->driver->pin1_disable(service, pin_buf.m_str, telephonyservice_common_finish, req_data) < 0) {
		g_warning("Failed to process service pin1Disable request in our driver");
		luna_service_message_reply_custom_error(handle, message, "Failed disable PIN1 on the SIM card");
		goto cleanup;
	}

	return true;

cleanup:
	if (req_data)
		luna_service_req_data_free(req_data);

	return true;
}

/**
 * @brief Change PIN1 on the SIM card.
 *
 * JSON format:
 *  { "oldPin": "<pin code>",
 *    "newPin": "<pin code>" }
 **/
bool _service_pin1_change_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct telephony_service *service = user_data;
	struct luna_service_req_data *req_data = NULL;
	jvalue_ref parsed_obj = NULL;
	jvalue_ref pin_obj = NULL;
	const char *payload;
	raw_buffer oldpin_buf, newpin_buf, newpinconfirm_buf;

	if (!service->initialized) {
		luna_service_message_reply_custom_error(handle, message, "Service not yet successfully initialized.");
		goto cleanup;
	}

	if (!service->driver || !service->driver->pin1_change) {
		g_warning("No implementation available for service pin1Change API method");
		luna_service_message_reply_error_not_implemented(handle, message);
		goto cleanup;
	}

	payload = LSMessageGetPayload(message);
	parsed_obj = luna_service_message_parse_and_validate(payload);
	if (jis_null(parsed_obj)) {
		luna_service_message_reply_error_bad_json(handle, message);
		goto cleanup;
	}

	if (!jobject_get_exists(parsed_obj, J_CSTR_TO_BUF("oldPin"), &pin_obj)) {
		luna_service_message_reply_error_bad_json(handle, message);
		goto cleanup;
	}

	oldpin_buf = jstring_get(pin_obj);

	if (!jobject_get_exists(parsed_obj, J_CSTR_TO_BUF("newPin"), &pin_obj)) {
		luna_service_message_reply_error_bad_json(handle, message);
		goto cleanup;
	}

	newpin_buf = jstring_get(pin_obj);

	req_data = luna_service_req_data_new(handle, message);

	if (service->driver->pin1_change(service, oldpin_buf.m_str, newpin_buf.m_str,
									 telephonyservice_common_finish, req_data) < 0) {
		g_warning("Failed to process service pin1Change request in our driver");
		luna_service_message_reply_custom_error(handle, message, "Failed change PIN1 on the SIM card");
		goto cleanup;
	}

	return true;

cleanup:
	if (req_data)
		luna_service_req_data_free(req_data);

	return true;
}

/**
 * @brief Unblock PIN1 on the SIM card.
 *
 * JSON format:
 *  { "puk": "<puk code>",
 *    "newPin": "<pin code>" }
 **/
bool _service_pin1_unblock_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct telephony_service *service = user_data;
	struct luna_service_req_data *req_data = NULL;
	jvalue_ref parsed_obj = NULL;
	jvalue_ref pin_obj = NULL;
	const char *payload;
	raw_buffer puk_buf, newpin_buf;

	if (!service->initialized) {
		luna_service_message_reply_custom_error(handle, message, "Service not yet successfully initialized.");
		goto cleanup;
	}

	if (!service->driver || !service->driver->pin1_unblock) {
		g_warning("No implementation available for service pin1Unblock API method");
		luna_service_message_reply_error_not_implemented(handle, message);
		goto cleanup;
	}

	payload = LSMessageGetPayload(message);
	parsed_obj = luna_service_message_parse_and_validate(payload);
	if (jis_null(parsed_obj)) {
		luna_service_message_reply_error_bad_json(handle, message);
		goto cleanup;
	}

	if (!jobject_get_exists(parsed_obj, J_CSTR_TO_BUF("puk"), &pin_obj)) {
		luna_service_message_reply_error_bad_json(handle, message);
		goto cleanup;
	}

	puk_buf = jstring_get(pin_obj);

	if (!jobject_get_exists(parsed_obj, J_CSTR_TO_BUF("newPin"), &pin_obj)) {
		luna_service_message_reply_error_bad_json(handle, message);
		goto cleanup;
	}

	newpin_buf = jstring_get(pin_obj);

	req_data = luna_service_req_data_new(handle, message);

	if (service->driver->pin1_unblock(service, puk_buf.m_str, newpin_buf.m_str,
									  telephonyservice_common_finish, req_data) < 0) {
		g_warning("Failed to process service pin1Unblock request in our driver");
		luna_service_message_reply_custom_error(handle, message, "Failed unblock PIN1 on the SIM card");
		goto cleanup;
	}

	return true;

cleanup:
	if (req_data)
		luna_service_req_data_free(req_data);

	return true;
}

// vim:ts=4:sw=4:noexpandtab
