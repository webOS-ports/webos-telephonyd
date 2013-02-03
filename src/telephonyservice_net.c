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

void telephony_service_signal_strength_changed_notify(struct telephony_service *service, int bars)
{
	jvalue_ref reply_obj = NULL;
	jvalue_ref signal_obj = NULL;

	if (service->power_off_pending)
		return;

	reply_obj = jobject_create();
	signal_obj = jobject_create();

	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("errorCode"), jnumber_create_i32(0));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("errorText"), jstring_create(""));

	jobject_put(signal_obj, J_CSTR_TO_JVAL("bars"), jnumber_create_i32(bars));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("eventSignal"), signal_obj);

	luna_service_post_subscription(service->private_service, "/", "signalStrengthQuery", reply_obj);

	j_release(&reply_obj);
}

void telephony_service_network_status_changed_notify(struct telephony_service *service, struct telephony_network_status *net_status)
{
	jvalue_ref reply_obj = NULL;
	jvalue_ref network_obj = NULL;

	if (service->power_off_pending)
		return;

	reply_obj = jobject_create();
	network_obj = jobject_create();

	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("errorCode"), jnumber_create_i32(0));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("errorText"), jstring_create(""));

	service->network_registered = (net_status->state == TELEPHONY_NETWORK_STATE_SERVICE);

	jobject_put(network_obj, J_CSTR_TO_JVAL("state"),
				jstring_create(telephony_network_state_to_string(net_status->state)));
	jobject_put(network_obj, J_CSTR_TO_JVAL("registration"),
				jstring_create(telephony_network_registration_to_string(net_status->registration)));
	jobject_put(network_obj, J_CSTR_TO_JVAL("networkName"), jstring_create(net_status->name != NULL ? net_status->name : ""));
	jobject_put(network_obj, J_CSTR_TO_JVAL("causeCode"), jstring_create(""));

	jobject_put(reply_obj, J_CSTR_TO_JVAL("eventNetwork"), network_obj);

	luna_service_post_subscription(service->private_service, "/", "networkStatusQuery", reply_obj);

	j_release(&reply_obj);
}

static int _service_signal_strength_query_finish(const struct telephony_error *error, unsigned int bars, void *data)
{
	struct luna_service_req_data *req_data = data;
	jvalue_ref reply_obj = NULL;
	jvalue_ref extended_obj = NULL;
	bool success = (error == NULL);

	reply_obj = jobject_create();
	extended_obj = jobject_create();

	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(success));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("errorCode"), jnumber_create_i32(0));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("errorText"), jstring_create(""));

	/* handle possible subscriptions */
	if (req_data->subscribed)
		jobject_put(reply_obj, J_CSTR_TO_JVAL("subscribed"), jboolean_create(req_data->subscribed));

	if (success) {
		jobject_put(extended_obj, J_CSTR_TO_JVAL("bars"), jnumber_create_i32(bars));
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
 * @brief Query the strength of the current network
 *
 * JSON format:
 *  request:
 *    { }
 *  response:
 *    {
 *       "returnValue": <boolean>,
 *       "errorCode": <integer>,
 *       "errorString": <string>,
 *       "subscribed": <boolean>;
 *       "rssi": <integer>,
 *       "bars": <integer>,
 *       "maxBars": <integer>,
 *       "extended": {
 *           "value": <integer>,
 *       },
 *    }
 **/
bool _service_signal_strength_query_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct telephony_service *service = user_data;
	struct luna_service_req_data *req_data = NULL;

	if (!service->initialized) {
		luna_service_message_reply_custom_error(handle, message, "Service not yet successfully initialized.");
		goto cleanup;
	}

	if (!service->driver || !service->driver->signal_strength_query) {
		g_warning("No implementation available for service signalStrengthQuery API method");
		luna_service_message_reply_error_not_implemented(handle, message);
		goto cleanup;
	}

	req_data = luna_service_req_data_new(handle, message);
	req_data->subscribed = luna_service_check_for_subscription_and_process(req_data->handle, req_data->message);

	if (service->driver->signal_strength_query(service, _service_signal_strength_query_finish, req_data) < 0) {
		g_warning("Failed to process service signalStrengthQuery request in our driver");
		luna_service_message_reply_custom_error(handle, message, "Failed to query network signal strength");
		goto cleanup;
	}

	return true;

cleanup:
	if (req_data)
		luna_service_req_data_free(req_data);

	return true;
}

static int _service_network_status_query_finish(const struct telephony_error *error, struct telephony_network_status *net_status, void *data)
{
	struct luna_service_req_data *req_data = data;
	jvalue_ref reply_obj = NULL;
	jvalue_ref extended_obj = NULL;
	bool success = (error == NULL);

	reply_obj = jobject_create();
	extended_obj = jobject_create();

	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(success));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("errorCode"), jnumber_create_i32(0));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("errorText"), jstring_create(""));

	/* handle possible subscriptions */
	if (req_data->subscribed)
		jobject_put(reply_obj, J_CSTR_TO_JVAL("subscribed"), jboolean_create(req_data->subscribed));

	if (success) {
		jobject_put(extended_obj, J_CSTR_TO_JVAL("state"),
					jstring_create(telephony_network_state_to_string(net_status->state)));
		jobject_put(extended_obj, J_CSTR_TO_JVAL("registration"),
				jstring_create(telephony_network_registration_to_string(net_status->registration)));
		jobject_put(extended_obj, J_CSTR_TO_JVAL("networkName"), jstring_create(net_status->name != NULL ? net_status->name : ""));
		jobject_put(extended_obj, J_CSTR_TO_JVAL("causeCode"), jstring_create(""));
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
 * @brief Query the status of the current network
 *
 * JSON format:
 *  request:
 *    { }
 *  response:
 *    {
 *       "returnValue": <boolean>,
 *       "errorCode": <integer>,
 *       "errorString": <string>,
 *       "subscribed": <boolean>;
 *       "extended": {
 *           "state": <string>,
 *           "registration": <string>,
 *           "networkName": <string>,
 *           "dataRegistered": <boolean>,
 *           "dataType": <string>,
 *           "causeCode": <string>,
 *       },
 *    }
 **/
bool _service_network_status_query_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct telephony_service *service = user_data;
	struct luna_service_req_data *req_data = NULL;

	if (!service->initialized) {
		luna_service_message_reply_custom_error(handle, message, "Service not yet successfully initialized.");
		goto cleanup;
	}

	if (!service->driver || !service->driver->network_status_query) {
		g_warning("No implementation available for service networkStatusQuery API method");
		luna_service_message_reply_error_not_implemented(handle, message);
		goto cleanup;
	}

	req_data = luna_service_req_data_new(handle, message);
	req_data->subscribed = luna_service_check_for_subscription_and_process(req_data->handle, req_data->message);

	if (service->driver->network_status_query(service, _service_network_status_query_finish, req_data) < 0) {
		g_warning("Failed to process service networkStatusQuery request in our driver");
		luna_service_message_reply_custom_error(handle, message, "Failed to query network status");
		goto cleanup;
	}

	return true;

cleanup:
	if (req_data)
		luna_service_req_data_free(req_data);

	return true;
}

static int _service_network_list_query_finish(const struct telephony_error *error, GList *networks, void *data)
{
	struct luna_service_req_data *req_data = data;
	struct telephony_service *service = req_data->user_data;
	jvalue_ref reply_obj = NULL;
	jvalue_ref extended_obj = NULL;
	jvalue_ref networks_obj = NULL;
	jvalue_ref network_obj = NULL;
	bool success = (error == NULL);
	struct telephony_network *current_network = NULL;
	int n;

	reply_obj = jobject_create();
	extended_obj = jobject_create();
	networks_obj = jarray_create(NULL);

	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(success));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("errorCode"), jnumber_create_i32(0));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("errorText"), jstring_create(""));

	if (success) {
		for (n = 0; n < g_list_length(networks); n++) {
			current_network = g_list_nth_data(networks, n);

			network_obj = jobject_create();
			jobject_put(network_obj, J_CSTR_TO_JVAL("id"), jnumber_create_i32(current_network->id));
			jobject_put(network_obj, J_CSTR_TO_JVAL("name"),
						jstring_create(current_network->name ? current_network->name : ""));
			const char *rat_str = telephony_radio_access_mode_to_string(current_network->radio_access_mode);
			jobject_put(network_obj, J_CSTR_TO_JVAL("rat"), jstring_create(rat_str));
			jarray_append(networks_obj, network_obj);
		}

		jobject_put(extended_obj, J_CSTR_TO_JVAL("networks"), networks_obj);
		jobject_put(reply_obj, J_CSTR_TO_JVAL("extended"), extended_obj);
	}

	if(!luna_service_message_validate_and_send(req_data->handle, req_data->message, reply_obj)) {
		luna_service_message_reply_error_internal(req_data->handle, req_data->message);
		goto cleanup;
	}

cleanup:
	service->network_status_query_pending = false;

	j_release(&reply_obj);
	luna_service_req_data_free(req_data);

	return 0;
}


/**
 * @brief Query for a list of available networks.
 *
 * JSON format:
 *  request:
 *    { }
 *  response:
 *    {
 *       "returnValue": <boolean>,
 *       "errorCode": <integer>,
 *       "errorString": <string>,
 *       "extended": {
 *           "networks": [
 *              {
 *                  "id": <string>,
 *                  "name": <string>,
 *                  "rat": <string>,
 *              },
 *              [...]
 *           ],
 *       },
 *    }
 **/
bool _service_network_list_query_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct telephony_service *service = user_data;
	struct luna_service_req_data *req_data = NULL;

	if (!service->initialized) {
		luna_service_message_reply_custom_error(handle, message, "Service not yet successfully initialized.");
		goto cleanup;
	}

	if (!service->driver || !service->driver->network_list_query) {
		g_warning("No implementation available for service networkListQuery API method");
		luna_service_message_reply_error_not_implemented(handle, message);
		goto cleanup;
	}

	if (service->network_status_query_pending) {
		luna_service_message_reply_custom_error(handle, message,
				"Another networkListQuery call is already pending");
		goto cleanup;
	}

	req_data = luna_service_req_data_new(handle, message);
	req_data->user_data = service;

	service->network_status_query_pending = true;

	if (service->driver->network_list_query(service, _service_network_list_query_finish, req_data) < 0) {
		g_warning("Failed to process service networkListQuery request in our driver");
		luna_service_message_reply_custom_error(handle, message, "Failed to query for available networks");
		goto cleanup;
	}

	return true;

cleanup:
	if (req_data)
		luna_service_req_data_free(req_data);

	return true;
}

static int _service_network_list_query_cancel_finish(const struct telephony_error *error, void *data)
{
	struct luna_service_req_data *req_data = data;
	struct telephony_service *service = req_data->user_data;
	jvalue_ref reply_obj = NULL;
	bool success = (error == NULL);

	if (!success) {
		luna_service_message_reply_error_unknown(req_data->handle, req_data->message);
		goto cleanup;
	}

	reply_obj = jobject_create();
	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(success));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("errorCode"), jnumber_create_i32(0));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("errorText"), jstring_create(""));

	if(!luna_service_message_validate_and_send(req_data->handle, req_data->message, reply_obj)) {
		luna_service_message_reply_error_internal(req_data->handle, req_data->message);
		goto cleanup;
	}

	service->network_status_query_pending = false;

cleanup:
	j_release(&reply_obj);
	luna_service_req_data_free(req_data);
	return 0;
}

/**
 * @brief Cancel an ongoing query for a list of available networks.
 *
 * JSON format:
 *  request:
 *    { }
 *  response:
 *    {
 *       "returnValue": <boolean>,
 *       "errorCode": <integer>,
 *       "errorString" <string>,
 *    }
 **/
bool _service_network_list_query_cancel_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct telephony_service *service = user_data;
	struct luna_service_req_data *req_data = NULL;

	if (!service->initialized) {
		luna_service_message_reply_custom_error(handle, message, "Service not yet successfully initialized.");
		goto cleanup;
	}

	if (!service->driver || !service->driver->network_list_query_cancel) {
		g_warning("No implementation available for service networkListQueryCancel API method");
		luna_service_message_reply_error_not_implemented(handle, message);
		goto cleanup;
	}

	if (!service->network_status_query_pending) {
		luna_service_message_reply_custom_error(handle, message, "No network list query pending");
		goto cleanup;
	}

	req_data = luna_service_req_data_new(handle, message);
	req_data->user_data = service;

	if (service->driver->network_list_query_cancel(service, _service_network_list_query_cancel_finish, req_data) < 0) {
		g_warning("Failed to process service networkListQueryCancel request in our driver");
		luna_service_message_reply_custom_error(handle, message, "Failed to cancel query for available networks");
		goto cleanup;
	}

	return true;

cleanup:
	if (req_data)
		luna_service_req_data_free(req_data);

	return true;
}

static int _service_network_id_query_finish(const struct telephony_error *error, const char *id, void *data)
{
	struct luna_service_req_data *req_data = data;
	jvalue_ref reply_obj = NULL;
	jvalue_ref extended_obj = NULL;
	bool success = (error == NULL);

	reply_obj = jobject_create();
	extended_obj = jobject_create();

	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(success));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("errorCode"), jnumber_create_i32(0));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("errorText"), jstring_create(""));

	/* handle possible subscriptions */
	if (req_data->subscribed)
		jobject_put(reply_obj, J_CSTR_TO_JVAL("subscribed"), jboolean_create(req_data->subscribed));

	if (success) {
		jobject_put(extended_obj, J_CSTR_TO_JVAL("mccmnc"), jstring_create(id));
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
 * @brief Query the id of the network we're connected.
 *
 * JSON format:
 *  request:
 *    { }
 *  response:
 *    {
 *       "returnValue": <boolean>,
 *       "errorCode": <integer>,
 *       "errorString": <string>,
 *       "subscribed": <boolean>,
 *       "extended": {
 *            "mccmnc": <string>,
 *       },
 *    }
 **/
bool _service_network_id_query_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct telephony_service *service = user_data;
	struct luna_service_req_data *req_data = NULL;

	if (!service->initialized) {
		luna_service_message_reply_custom_error(handle, message, "Service not yet successfully initialized.");
		goto cleanup;
	}

	if (!service->driver || !service->driver->network_id_query) {
		g_warning("No implementation available for service networkIdQuery API method");
		luna_service_message_reply_error_not_implemented(handle, message);
		goto cleanup;
	}

	req_data = luna_service_req_data_new(handle, message);
	req_data->subscribed = luna_service_check_for_subscription_and_process(req_data->handle, req_data->message);

	if (service->driver->network_id_query(service, _service_network_id_query_finish, req_data) < 0) {
		g_warning("Failed to process service networkIdQuery request in our driver");
		luna_service_message_reply_custom_error(handle, message, "Failed retrieve id of current connected network");
		goto cleanup;
	}

	return true;

cleanup:
	if (req_data)
		luna_service_req_data_free(req_data);

	return true;
}

static int _service_network_selection_mode_query_finish(const struct telephony_error *error, bool automatic, void *data)
{
	struct luna_service_req_data *req_data = data;
	jvalue_ref reply_obj = NULL;
	bool success = (error == NULL);

	reply_obj = jobject_create();

	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(success));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("errorCode"), jnumber_create_i32(0));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("errorText"), jstring_create(""));

	if (success) {
		jobject_put(reply_obj, J_CSTR_TO_JVAL("automatic"), jboolean_create(automatic));
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
 * @brief Query the current network selection mode.
 *
 * JSON format:
 *  request:
 *    { }
 *  response:
 *    {
 *       "returnValue": <boolean>,
 *       "errorCode": <integer>,
 *       "errorString": <string>,
 *       "automatic": <boolean>
 *    }
 **/
bool _service_network_selection_mode_query_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct telephony_service *service = user_data;
	struct luna_service_req_data *req_data = NULL;

	if (!service->initialized) {
		luna_service_message_reply_custom_error(handle, message, "Service not yet successfully initialized.");
		goto cleanup;
	}

	if (!service->driver || !service->driver->network_selection_mode_query) {
		g_warning("No implementation available for service networkSelectionModeQuery API method");
		luna_service_message_reply_error_not_implemented(handle, message);
		goto cleanup;
	}

	req_data = luna_service_req_data_new(handle, message);

	if (service->driver->network_selection_mode_query(service, _service_network_selection_mode_query_finish, req_data) < 0) {
		g_warning("Failed to process service networkSelectionModeQuery request in our driver");
		luna_service_message_reply_custom_error(handle, message, "Failed retrieve the current network selection mode");
		goto cleanup;
	}

	return true;

cleanup:
	if (req_data)
		luna_service_req_data_free(req_data);

	return true;
}

/**
 * @brief Set the network the modem should connect to.
 *
 * JSON format:
 *  request:
 *    {
 *        "automatic": <boolean>,
 *        "id": <string>,
 *    }
 *  response:
 *    {
 *       "returnValue": <boolean>,
 *       "errorCode": <integer>,
 *       "errorString": <string>,
 *    }
 **/
bool _service_network_set_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct telephony_service *service = user_data;
	struct luna_service_req_data *req_data = NULL;
	jvalue_ref parsed_obj = NULL;
	jvalue_ref automatic_obj = NULL;
	jvalue_ref id_obj = NULL;
	const char *payload;
	raw_buffer id_buf;
	bool automatic = false;

	if (!service->initialized) {
		luna_service_message_reply_custom_error(handle, message, "Service not yet successfully initialized.");
		goto cleanup;
	}

	if (!service->driver || !service->driver->network_set) {
		g_warning("No implementation available for service networkSet API method");
		luna_service_message_reply_error_not_implemented(handle, message);
		goto cleanup;
	}

	payload = LSMessageGetPayload(message);
	parsed_obj = luna_service_message_parse_and_validate(payload);
	if (jis_null(parsed_obj)) {
		luna_service_message_reply_error_bad_json(handle, message);
		goto cleanup;
	}

	if (!jobject_get_exists(parsed_obj, J_CSTR_TO_BUF("automatic"), &automatic_obj)) {
		luna_service_message_reply_error_bad_json(handle, message);
		goto cleanup;
	}

	jboolean_get(automatic_obj, &automatic);

	if (!jobject_get_exists(parsed_obj, J_CSTR_TO_BUF("id"), &id_obj)) {
		luna_service_message_reply_error_bad_json(handle, message);
		goto cleanup;
	}

	id_buf = jstring_get(id_obj);

	req_data = luna_service_req_data_new(handle, message);

	if (service->driver->network_set(service, automatic, id_buf.m_str, telephonyservice_common_finish, req_data) < 0) {
		g_warning("Failed to process service networkSet request in our driver");
		luna_service_message_reply_custom_error(handle, message, "Failed to connect to set network");
		goto cleanup;
	}

	return true;

cleanup:
	if (req_data)
		luna_service_req_data_free(req_data);

	return true;
}

// vim:ts=4:sw=4:noexpandtab
