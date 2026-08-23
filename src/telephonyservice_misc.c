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
#include "telephonyservice.h"
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
	telephony_service_add_sim_id(reply_obj, req_data->sim_id);
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

void telephony_service_power_status_notify(struct telephony_service *service, int sim_id, bool power)
{
	struct telephony_sim_state *sim;
	jvalue_ref reply_obj = NULL;
	jvalue_ref extended_obj = NULL;

	sim = telephony_service_sim_state(service, sim_id);
	if (!sim)
		return;

	sim->powered = power;

	if (!sim->initialized) {
		g_message("SIM %d not yet successfully initialized. Not sending power status notification", sim_id);
		return;
	}

	reply_obj = jobject_create();
	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
	telephony_service_add_sim_id(reply_obj, sim_id);

	extended_obj = jobject_create();
	jobject_put(extended_obj, J_CSTR_TO_JVAL("powerState"), jstring_create(power ? "on": "off"));
	jobject_put(extended_obj, J_CSTR_TO_JVAL("simId"), jnumber_create_i32(sim_id));

	jobject_put(reply_obj, J_CSTR_TO_JVAL("extended"), extended_obj);

	telephony_service_post_sim_subscription(service, "powerQuery", sim_id, TELEPHONY_SIM_ROLE_VOICE, reply_obj);

	j_release(&reply_obj);

	telephony_service_repost_sim_list(service);
}

int _service_power_set_finish(const struct telephony_error *error, void *data)
{
	struct luna_service_req_data *req_data = data;
	struct telephony_service *service = req_data->user_data;
	struct telephony_sim_state *sim;
	jvalue_ref reply_obj = NULL;

	reply_obj = jobject_create();

	sim = telephony_service_sim_state(service, req_data->sim_id);
	if (sim)
		sim->power_off_pending = false;

	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create((error == NULL)));
	telephony_service_add_sim_id(reply_obj, req_data->sim_id);

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
	struct telephony_sim_state *sim = NULL;
	struct luna_service_req_data *req_data = NULL;
	bool power = false;
	jvalue_ref parsed_obj = NULL;
	jvalue_ref state_obj = NULL;
	jvalue_ref save_obj = NULL;
	const char *payload;
	bool should_save = false;
	bool explicit_sim = false;
	int sim_id;

	if (!service->driver || !service->driver->power_set) {
		g_warning("No implementation available for service powerSet API method");
		luna_service_message_reply_error_not_implemented(handle, message);
		return true;
	}

	payload = LSMessageGetPayload(message);
	parsed_obj = luna_service_message_parse_and_validate(payload);
	if (jis_null(parsed_obj)) {
		luna_service_message_reply_error_bad_json(handle, message);
		goto cleanup;
	}

	sim_id = telephony_service_resolve_sim_id(service, parsed_obj, TELEPHONY_SIM_ROLE_VOICE, &explicit_sim);
	sim = telephony_service_sim_state(service, sim_id);
	if (!sim) {
		luna_service_message_reply_custom_error(handle, message, "Unknown SIM slot");
		goto cleanup;
	}

	if (!sim->initialized) {
		luna_service_message_reply_custom_error(handle, message, "Backend not initialized");
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
		if (should_save) {
			telephony_service_store_power_state_for_sim(sim_id, power);

			/* Keep the legacy device wide setting in sync while the default
			 * voice SIM is the one being switched, so that an older build
			 * reading it back still sees something sensible. */
			if (telephony_service_get_default_sim(service, TELEPHONY_SIM_ROLE_VOICE) == sim_id)
				telephony_settings_store(TELEPHONY_SETTINGS_TYPE_POWER_STATE,
										 power ? "{\"state\":true}" : "{\"state\":false}");
		}
	}

	sim->power_off_pending = !power;

	req_data = luna_service_req_data_new(handle, message);
	req_data->user_data = service;
	req_data->sim_id = sim_id;

	service->driver->power_set(service, sim_id, power, _service_power_set_finish, req_data);

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

	telephony_service_add_sim_id(reply_obj, req_data->sim_id);

	if (success) {
		jobject_put(extended_obj, J_CSTR_TO_JVAL("powerState"), jstring_create(power ? "on" : "off"));
		jobject_put(extended_obj, J_CSTR_TO_JVAL("simId"), jnumber_create_i32(req_data->sim_id));
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
	struct telephony_sim_state *sim = NULL;
	struct luna_service_req_data *req_data = NULL;
	struct telephony_error terr;
	jvalue_ref parsed_obj = NULL;
	const char *payload;
	bool explicit_sim = false;
	int sim_id;

	if (!service->driver || !service->driver->power_query) {
		g_warning("No implementation available for service powerQuery API method");
		luna_service_message_reply_error_not_implemented(handle, message);
		return true;
	}

	payload = LSMessageGetPayload(message);
	parsed_obj = luna_service_message_parse_and_validate(payload);

	sim_id = telephony_service_resolve_sim_id(service, parsed_obj, TELEPHONY_SIM_ROLE_VOICE, &explicit_sim);
	if (sim_id < 0 && explicit_sim) {
		luna_service_message_reply_custom_error(handle, message, "Unknown SIM slot");
		goto cleanup;
	}

	req_data = luna_service_req_data_new(handle, message);
	req_data->sim_id = sim_id;
	req_data->subscribed = telephony_service_process_sim_subscription(service, handle, message,
											"powerQuery", sim_id, explicit_sim);

	sim = telephony_service_sim_state(service, sim_id);

	if (!sim || !sim->initialized) {
		/* no service -> no power. But still process the subscription and return an answer. */
		terr.code = 1;
		g_warning("Backend not initialized yet.");
		_service_power_query_finish(&terr, false, (void*)req_data);
	}
	else {
		service->driver->power_query(service, sim_id, _service_power_query_finish, req_data);
	}

cleanup:
	if (!jis_null(parsed_obj))
		j_release(&parsed_obj);

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

	telephony_service_add_sim_id(reply_obj, req_data->sim_id);

	if (success) {
		jobject_put(extended_obj, J_CSTR_TO_JVAL("simId"), jnumber_create_i32(req_data->sim_id));
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
	struct telephony_sim_state *sim = NULL;
	struct luna_service_req_data *req_data = NULL;
	jvalue_ref parsed_obj = NULL;
	const char *payload;
	bool explicit_sim = false;
	int sim_id;

	if (!service->driver || !service->driver->platform_query) {
		g_warning("No implementation available for service platformQuery API method");
		luna_service_message_reply_error_not_implemented(handle, message);
		return true;
	}

	payload = LSMessageGetPayload(message);
	parsed_obj = luna_service_message_parse_and_validate(payload);

	sim_id = telephony_service_resolve_sim_id(service, parsed_obj, TELEPHONY_SIM_ROLE_VOICE, &explicit_sim);
	sim = telephony_service_sim_state(service, sim_id);
	if (!sim) {
		luna_service_message_reply_custom_error(handle, message, "Unknown SIM slot");
		goto cleanup;
	}

	if (!sim->initialized) {
		luna_service_message_reply_custom_error(handle, message, "Backend not initialized");
		goto cleanup;
	}

	req_data = luna_service_req_data_new(handle, message);
	req_data->sim_id = sim_id;
	req_data->subscribed = telephony_service_process_sim_subscription(service, handle, message,
											"platformQuery", sim_id, explicit_sim);

	service->driver->platform_query(service, sim_id, _service_platform_query_finish, req_data);

cleanup:
	if (!jis_null(parsed_obj))
		j_release(&parsed_obj);

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

	telephony_service_add_sim_id(reply_obj, req_data->sim_id);

	if (success) {
		jobject_put(extended_obj, J_CSTR_TO_JVAL("simId"), jnumber_create_i32(req_data->sim_id));
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
	struct telephony_sim_state *sim = NULL;
	struct luna_service_req_data *req_data = NULL;
	jvalue_ref parsed_obj = NULL;
	const char *payload;
	bool explicit_sim = false;
	int sim_id;

	if (!service->driver || !service->driver->subscriber_id_query) {
		g_warning("No implementation available for service subscriberIdQuery API method");
		luna_service_message_reply_error_not_implemented(handle, message);
		return true;
	}

	payload = LSMessageGetPayload(message);
	parsed_obj = luna_service_message_parse_and_validate(payload);

	sim_id = telephony_service_resolve_sim_id(service, parsed_obj, TELEPHONY_SIM_ROLE_VOICE, &explicit_sim);
	sim = telephony_service_sim_state(service, sim_id);
	if (!sim) {
		luna_service_message_reply_custom_error(handle, message, "Unknown SIM slot");
		goto cleanup;
	}

	if (!sim->initialized) {
		luna_service_message_reply_custom_error(handle, message, "Backend not initialized");
		goto cleanup;
	}

	req_data = luna_service_req_data_new(handle, message);
	req_data->sim_id = sim_id;

	service->driver->subscriber_id_query(service, sim_id, _service_subscriber_id_query_finish, req_data);

cleanup:
	if (!jis_null(parsed_obj))
		j_release(&parsed_obj);

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
	struct telephony_sim_state *sim = NULL;
	jvalue_ref parsed_obj = NULL;
	jvalue_ref reply_obj = NULL;
	jvalue_ref extended_obj = NULL;
	const char *payload;
	bool subscribed = false;
	bool explicit_sim = false;
	int sim_id;

	payload = LSMessageGetPayload(message);
	parsed_obj = luna_service_message_parse_and_validate(payload);

	sim_id = telephony_service_resolve_sim_id(service, parsed_obj, TELEPHONY_SIM_ROLE_VOICE, &explicit_sim);
	if (!jis_null(parsed_obj))
		j_release(&parsed_obj);

	if (sim_id < 0 && explicit_sim) {
		luna_service_message_reply_custom_error(handle, message, "Unknown SIM slot");
		return true;
	}

	sim = telephony_service_sim_state(service, sim_id);

	reply_obj = jobject_create();
	extended_obj = jobject_create();

	subscribed = telephony_service_process_sim_subscription(service, handle, message,
										"deviceLockQuery", sim_id, explicit_sim);

	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
	telephony_service_add_sim_id(reply_obj, sim_id);

	if (!sim || !sim->powered) {
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
	struct telephony_sim_state *sim = NULL;
	jvalue_ref parsed_obj = NULL;
	jvalue_ref reply_obj = NULL;
	jvalue_ref extended_obj = NULL;
	const char *payload;
	bool subscribed = false;
	bool explicit_sim = false;
	int sim_id;

	payload = LSMessageGetPayload(message);
	parsed_obj = luna_service_message_parse_and_validate(payload);

	sim_id = telephony_service_resolve_sim_id(service, parsed_obj, TELEPHONY_SIM_ROLE_VOICE, &explicit_sim);
	if (!jis_null(parsed_obj))
		j_release(&parsed_obj);

	if (sim_id < 0 && explicit_sim) {
		luna_service_message_reply_custom_error(handle, message, "Unknown SIM slot");
		return true;
	}

	sim = telephony_service_sim_state(service, sim_id);

	reply_obj = jobject_create();
	extended_obj = jobject_create();

	subscribed = telephony_service_process_sim_subscription(service, handle, message,
										"isTelephonyReady", sim_id, explicit_sim);

	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("errorCode"), jnumber_create_i32(0));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("errorText"), jstring_create("success"));
	telephony_service_add_sim_id(reply_obj, sim_id);
	jobject_put(reply_obj, J_CSTR_TO_JVAL("simCount"),
				jnumber_create_i32(telephony_service_get_sim_count(service)));
	jobject_put(extended_obj, J_CSTR_TO_JVAL("simId"), jnumber_create_i32(sim_id));
	jobject_put(extended_obj, J_CSTR_TO_JVAL("radioConnected"), jboolean_create(sim && sim->initialized));
	jobject_put(extended_obj, J_CSTR_TO_JVAL("power"), jboolean_create(sim && sim->powered));
	jobject_put(extended_obj, J_CSTR_TO_JVAL("ready"), jboolean_create(sim && sim->initialized));
	jobject_put(extended_obj, J_CSTR_TO_JVAL("networkRegistered"), jboolean_create(sim && sim->network_registered));
	jobject_put(extended_obj, J_CSTR_TO_JVAL("dataRegistered"), jboolean_create(sim && sim->data_registered));

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
	struct telephony_service *service = user_data;
	jvalue_ref parsed_obj = NULL;
	jvalue_ref events_obj = NULL;
	jvalue_ref reply_obj = NULL;
	const char *payload;
	const char *method = NULL;
	bool subscribed = false;
	bool explicit_sim = false;
	int sim_id;

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

	sim_id = telephony_service_resolve_sim_id(service, parsed_obj, TELEPHONY_SIM_ROLE_VOICE, &explicit_sim);
	if (sim_id < 0 && explicit_sim) {
		luna_service_message_reply_custom_error(handle, message, "Unknown SIM slot");
		goto cleanup;
	}

	if (jstring_equal2(events_obj, J_CSTR_TO_BUF("network")))
		method = "networkStatusQuery";
	else if (jstring_equal2(events_obj, J_CSTR_TO_BUF("signal")))
		method = "signalStrengthQuery";
	else if (jstring_equal2(events_obj, J_CSTR_TO_BUF("sim")))
		method = "simListQuery";

	if (method) {
		if (g_str_equal(method, "simListQuery")) {
			/* not tied to a slot */
			subscribed = luna_service_check_for_subscription_with_key(handle, message, "/simListQuery");
		}
		else {
			subscribed = telephony_service_process_sim_subscription(service, handle, message,
											method, sim_id, explicit_sim);
		}

		if (!subscribed) {
			luna_service_message_reply_error_internal(handle, message);
			goto cleanup;
		}
	}

	reply_obj = jobject_create();
	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("errorCode"), jnumber_create_i32(0));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("errorText"), jstring_create("success"));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("subscribed"), jboolean_create(subscribed));
	telephony_service_add_sim_id(reply_obj, sim_id);

	if (!luna_service_message_validate_and_send(handle, message, reply_obj)) {
		luna_service_message_reply_error_internal(handle, message);
		goto cleanup;
	}

cleanup:
	if (!jis_null(reply_obj))
		j_release(&reply_obj);

	if (!jis_null(parsed_obj))
		j_release(&parsed_obj);

	return true;
}

// vim:ts=4:sw=4:noexpandtab
