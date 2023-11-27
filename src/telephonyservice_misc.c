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

int telephonyservice_common_finish(const struct telephony_error *error, void *data)
{
	struct luna_service_req_data *req_data = data;
	jvalue_ref reply_obj = NULL;
	bool success = (error == NULL);

	reply_obj = jobject_create();

	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(success));
	if (!success)
		jobject_put(reply_obj, J_CSTR_TO_JVAL("errorCode"), jnumber_create_i32(error->code));


	if(!luna_service_message_validate_and_send(req_data->handle, req_data->message, reply_obj)) {
		luna_service_message_reply_error_internal(req_data->handle, req_data->message);
		goto cleanup;
	}

cleanup:
	j_release(&reply_obj);
	luna_service_req_data_free(req_data);
	return 0;
}

void telephony_service_power_status_notify(struct telephony_service *service, bool power)
{
	jvalue_ref reply_obj = NULL;
	jvalue_ref extended_obj = NULL;

	service->powered = power;

	if (!service->initialized) {
		g_message("Service not yet successfully initialized. Not sending power status notification");
		return;
	}

	reply_obj = jobject_create();
	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));

	extended_obj = jobject_create();
	jobject_put(extended_obj, J_CSTR_TO_JVAL("powerState"), jstring_create(power ? "on": "off"));

	jobject_put(reply_obj, J_CSTR_TO_JVAL("extended"), extended_obj);

	luna_service_post_subscription(service->palmHandle, "/", "powerQuery", reply_obj);

	j_release(&reply_obj);
}

int _service_power_set_finish(const struct telephony_error *error, void *data)
{
	struct luna_service_req_data *req_data = data;
	struct telephony_service *service = req_data->user_data;
	jvalue_ref reply_obj = NULL;

	reply_obj = jobject_create();

	service->power_off_pending = false;

	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create((error == NULL)));

	if(!luna_service_message_validate_and_send(req_data->handle, req_data->message, reply_obj))
		luna_service_message_reply_error_internal(req_data->handle, req_data->message);

	j_release(&reply_obj);
	luna_service_req_data_free(req_data);
	return 0;
}

/**
 * @brief Set power mode for the telephony service
 *
 * JSON format:
 *    {"state":"<on|off|default>"}
 **/

bool _service_power_set_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct telephony_service *service = user_data;
	struct luna_service_req_data *req_data = NULL;
	bool power = false;
	jvalue_ref parsed_obj = NULL;
	jvalue_ref state_obj = NULL;
	jvalue_ref save_obj = NULL;
	const char *payload;
	bool should_save = false;

	if (!service->initialized) {
		luna_service_message_reply_custom_error(handle, message, "Backend not initialized");
		return true;
	}

	if (!service->driver || !service->driver->power_set) {
		g_warning("No implementation available for service powerSet API method");
		luna_service_message_reply_error_not_implemented(handle, message);
		goto cleanup;
	}

	payload = LSMessageGetPayload(message);
	parsed_obj = luna_service_message_parse_and_validate(payload);
	if (jis_null(parsed_obj)) {
		luna_service_message_reply_error_bad_json(handle, message);
		goto cleanup;
	}

	if (!jobject_get_exists(parsed_obj, J_CSTR_TO_BUF("state"), &state_obj)) {
		luna_service_message_reply_error_bad_json(handle, message);
		goto cleanup;
	}

	if (jstring_equal2(state_obj, J_CSTR_TO_BUF("on")))
		power = true;
	else if (jstring_equal2(state_obj, J_CSTR_TO_BUF("off")))
		power = false;
	else if (jstring_equal2(state_obj, J_CSTR_TO_BUF("default"))) {
		power = true;
	}
	else {
		luna_service_message_reply_error_bad_json(handle, message);
		goto cleanup;
	}

	if (jobject_get_exists(parsed_obj, J_CSTR_TO_BUF("save"), &save_obj)) {
		jboolean_get(save_obj, &should_save);
		if (should_save)
			telephony_settings_store(TELEPHONY_SETTINGS_TYPE_POWER_STATE,
									 power ? "{\"state\":true}" : "{\"state\":false}");
	}

	service->power_off_pending = !power;

	req_data = luna_service_req_data_new(handle, message);
	req_data->user_data = service;

	service->driver->power_set(service, power, _service_power_set_finish, req_data);

cleanup:
	if (!jis_null(parsed_obj))
		j_release(&parsed_obj);

	return true;
}

int _service_power_query_finish(const struct telephony_error *error, bool power, void *data)
{
	struct luna_service_req_data *req_data = data;
	jvalue_ref reply_obj = NULL;
	jvalue_ref extended_obj = NULL;
	bool success = (error == NULL);

	reply_obj = jobject_create();
	extended_obj = jobject_create();

	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(success));

	/* handle possible subscriptions */
	if (req_data->subscribed)
		jobject_put(reply_obj, J_CSTR_TO_JVAL("subscribed"), jboolean_create(req_data->subscribed));

	if (success) {
		jobject_put(extended_obj, J_CSTR_TO_JVAL("powerState"), jstring_create(power ? "on" : "off"));
		jobject_put(reply_obj, J_CSTR_TO_JVAL("extended"), extended_obj);
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
 * @brief Query the current power status of the telephony service
 *
 * JSON format:
 *    { ["subscribe": <boolean>] }
 **/

bool _service_power_query_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct telephony_service *service = user_data;
	struct luna_service_req_data *req_data = NULL;
	struct telephony_error terr;

	if (!service->driver || !service->driver->power_query) {
		g_warning("No implementation available for service powerQuery API method");
		luna_service_message_reply_error_not_implemented(handle, message);
		return true;
	}

	req_data = luna_service_req_data_new(handle, message);
	req_data->subscribed = luna_service_check_for_subscription_and_process(req_data->handle, req_data->message);

	if (!service->initialized) {
		// no service -> no power. But still process the subscription and return an answer.
		terr.code = 1;
		g_warning("Backend not initialized yet.");
		_service_power_query_finish(&terr, false, (void*)req_data);
	}
	else {
		service->driver->power_query(service, _service_power_query_finish, req_data);
	}

	return true;
}

static int _service_platform_query_finish(const struct telephony_error *error, struct telephony_platform_info *platform_info, void *data)
{
	struct luna_service_req_data *req_data = data;
	jvalue_ref reply_obj = NULL;
	jvalue_ref extended_obj = NULL;
	bool success = (error == NULL);

	reply_obj = jobject_create();
	extended_obj = jobject_create();

	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(success));

	if (success) {
		jobject_put(extended_obj, J_CSTR_TO_JVAL("platformType"),
			jstring_create(telephony_platform_type_to_string(platform_info->platform_type)));

		if (platform_info->imei != NULL)
			jobject_put(extended_obj, J_CSTR_TO_JVAL("imei"), jstring_create(platform_info->imei));

		if (platform_info->carrier != NULL)
			jobject_put(extended_obj, J_CSTR_TO_JVAL("carrier"), jstring_create(platform_info->carrier));

		if (platform_info->mcc > 0 && platform_info->mnc > 0) {
			jobject_put(extended_obj, J_CSTR_TO_JVAL("mcc"), jnumber_create_i32(platform_info->mcc));
			jobject_put(extended_obj, J_CSTR_TO_JVAL("mnc"), jnumber_create_i32(platform_info->mnc));
		}

		if (platform_info->version != NULL)
			jobject_put(extended_obj, J_CSTR_TO_JVAL("version"), jstring_create(platform_info->version));

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
 * @brief Query various information about the platform we're running on
 **/

bool _service_platform_query_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct telephony_service *service = user_data;
	struct luna_service_req_data *req_data = NULL;

	if (!service->initialized) {
		luna_service_message_reply_custom_error(handle, message, "Backend not initialized");
		return true;
	}

	if (!service->driver || !service->driver->platform_query) {
		g_warning("No implementation available for service platformQuery API method");
		luna_service_message_reply_error_not_implemented(handle, message);
		goto cleanup;
	}

	req_data = luna_service_req_data_new(handle, message);
	req_data->subscribed = luna_service_check_for_subscription_and_process(req_data->handle, req_data->message);

	service->driver->platform_query(service, _service_platform_query_finish, req_data);

cleanup:
	return true;
}

static int _service_subscriber_id_query_finish(const struct telephony_error *error, struct telephony_subscriber_info *info, void *data)
{
	struct luna_service_req_data *req_data = data;
	jvalue_ref reply_obj = NULL;
	jvalue_ref extended_obj = NULL;
	bool success = (error == NULL);

	reply_obj = jobject_create();
	extended_obj = jobject_create();

	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(success));

	if (success) {
		jobject_put(extended_obj, J_CSTR_TO_JVAL("platformType"),
					jstring_create(telephony_platform_type_to_string(info->platform_type)));

		switch (info->platform_type) {
		case TELEPHONY_PLATFORM_TYPE_GSM:
			jobject_put(extended_obj, J_CSTR_TO_JVAL("imsi"), jstring_create(info->imsi));
			jobject_put(extended_obj, J_CSTR_TO_JVAL("msisdn"), jstring_create(info->msisdn));
			break;
		case TELEPHONY_PLATFORM_TYPE_CDMA:
			jobject_put(extended_obj, J_CSTR_TO_JVAL("min"), jstring_create(info->min));
			jobject_put(extended_obj, J_CSTR_TO_JVAL("mdn"), jstring_create(info->mdn));
			break;
		}

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
 * @brief Query the subscriber id (IMSI, MSISDN)
 */
bool _service_subscriber_id_query_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct telephony_service *service = user_data;
	struct luna_service_req_data *req_data = NULL;

	if (!service->initialized) {
		luna_service_message_reply_custom_error(handle, message, "Backend not initialized");
		return true;
	}

	if (!service->driver || !service->driver->subscriber_id_query) {
		g_warning("No implementation available for service subscriberIdQuery API method");
		luna_service_message_reply_error_not_implemented(handle, message);
		return true;
	}

	req_data = luna_service_req_data_new(handle, message);

	service->driver->subscriber_id_query(service, _service_subscriber_id_query_finish, req_data);

	return true;
}

/**
 * @brief Query the lock status of the device
 *
 * JSON format:
 *  request:
 *    { }
 *  response:
 *    {
 *      "returnValue": <boolean>,
 *      "errorCode": <integer>,
 *      "errorString": <string>,
 *      "extended": <object>,
 *      "subscribed": <boolean>
 *    }
 */

bool _service_device_lock_query_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct telephony_service *service = user_data;
	jvalue_ref reply_obj = NULL;
	jvalue_ref extended_obj = NULL;
	bool subscribed = false;

	reply_obj = jobject_create();
	extended_obj = jobject_create();

	subscribed = luna_service_check_for_subscription_and_process(handle, message);

	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));

	if (!service->powered) {
		jobject_put(reply_obj, J_CSTR_TO_JVAL("errorCode"), jnumber_create_i32(1));
		jobject_put(reply_obj, J_CSTR_TO_JVAL("errorText"), jstring_create("Phone Radio is off"));
	}
	else {
		jobject_put(reply_obj, J_CSTR_TO_JVAL("errorCode"), jnumber_create_i32(0));
		jobject_put(reply_obj, J_CSTR_TO_JVAL("errorText"), jstring_create("success"));
	}

	/* FIXME we don't really now which properties are part of the extended object */

	jobject_put(reply_obj, J_CSTR_TO_JVAL("extended"), extended_obj);
	jobject_put(reply_obj, J_CSTR_TO_JVAL("subscribed"), jboolean_create(subscribed));

	if(!luna_service_message_validate_and_send(handle, message, reply_obj))
		luna_service_message_reply_error_internal(handle, message);

	j_release(&reply_obj);

	return true;
}

/**
 * @brief Query the charge source of the device
 *
 * JSON format:
 *  request:
 *    { }
 *  response:
 *    {
 *      "returnValue": <boolean>,
 *      "errorCode": <integer>,
 *      "errorString": <string>
 *    }
 */

bool _service_charge_source_query_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	jvalue_ref reply_obj = NULL;

	reply_obj = jobject_create();

	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));

	/* FIXME tested implementation in legacy webOS always returns the following error message */
	jobject_put(reply_obj, J_CSTR_TO_JVAL("errorCode"), jnumber_create_i32(103));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("errorText"), jstring_create("Not supported by this network type"));

	if(!luna_service_message_validate_and_send(handle, message, reply_obj))
		luna_service_message_reply_error_internal(handle, message);

	j_release(&reply_obj);

	return true;
}

/**
 * @brief Check wether telephony service is ready
 **/

bool _service_is_telephony_ready_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct telephony_service *service = user_data;
	jvalue_ref reply_obj = NULL;
	jvalue_ref extended_obj = NULL;
	bool subscribed = false;

	reply_obj = jobject_create();
	extended_obj = jobject_create();

	subscribed = luna_service_check_for_subscription_and_process(handle, message);

	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("errorCode"), jnumber_create_i32(0));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("errorText"), jstring_create("success"));
	jobject_put(extended_obj, J_CSTR_TO_JVAL("radioConnected"), jboolean_create(service->initialized));
	jobject_put(extended_obj, J_CSTR_TO_JVAL("power"), jboolean_create(service->powered));
	jobject_put(extended_obj, J_CSTR_TO_JVAL("ready"), jboolean_create(service->initialized));
	jobject_put(extended_obj, J_CSTR_TO_JVAL("networkRegistered"), jboolean_create(service->network_registered));
	jobject_put(extended_obj, J_CSTR_TO_JVAL("dataRegistered"), jboolean_create(service->data_registered));

	/* FIXME check in which situations the three fields below are set and updated */
	jobject_put(extended_obj, J_CSTR_TO_JVAL("emergency"), jboolean_create(false));
	jobject_put(extended_obj, J_CSTR_TO_JVAL("security"), jboolean_create(false));
	jobject_put(extended_obj, J_CSTR_TO_JVAL("securityLocked"), jboolean_create(false));

	jobject_put(reply_obj, J_CSTR_TO_JVAL("extended"), extended_obj);
	jobject_put(reply_obj, J_CSTR_TO_JVAL("subscribed"), jboolean_create(subscribed));

	if(!luna_service_message_validate_and_send(handle, message, reply_obj))
		luna_service_message_reply_error_internal(handle, message);

	j_release(&reply_obj);

	return true;
}

/**
 * @brief Subscribe for a specific group of events
 *
 * JSON format:
 *    {"events":"<type>"}
 **/
bool _service_subscribe_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	jvalue_ref parsed_obj = NULL;
	jvalue_ref events_obj = NULL;
	jvalue_ref reply_obj = NULL;
	bool result = false;
	LSError lserror;
	const char *payload;
	bool subscribed = false;

	payload = LSMessageGetPayload(message);
	parsed_obj = luna_service_message_parse_and_validate(payload);
	if (jis_null(parsed_obj)) {
		luna_service_message_reply_error_bad_json(handle, message);
		goto cleanup;
	}

	if (!jobject_get_exists(parsed_obj, J_CSTR_TO_BUF("events"), &events_obj)) {
		luna_service_message_reply_error_invalid_params(handle, message);
		goto cleanup;
	}

	if (jstring_equal2(events_obj, J_CSTR_TO_BUF("network"))) {
		result = LSSubscriptionAdd(handle, "/networkStatusQuery", message, &lserror);
		if (!result) {
			LSErrorPrint(&lserror, stderr);
			LSErrorFree(&lserror);

			luna_service_message_reply_error_internal(handle, message);
			goto cleanup;
		}

		subscribed = true;
	}
	else if (jstring_equal2(events_obj, J_CSTR_TO_BUF("signal"))) {
		result = LSSubscriptionAdd(handle, "/signalStrengthQuery", message, &lserror);
		if (!result) {
			LSErrorPrint(&lserror, stderr);
			LSErrorFree(&lserror);

			luna_service_message_reply_error_internal(handle, message);
			goto cleanup;
		}

		subscribed = true;
	}

	reply_obj = jobject_create();
	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("errorCode"), jnumber_create_i32(0));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("errorText"), jstring_create("success"));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("subscribed"), jboolean_create(subscribed));

	if (!luna_service_message_validate_and_send(handle, message, reply_obj)) {
		luna_service_message_reply_error_internal(handle, message);
		goto cleanup;
	}

cleanup:
	j_release(&parsed_obj);

	return true;
}

// vim:ts=4:sw=4:noexpandtab
