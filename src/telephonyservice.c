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

extern GMainLoop *event_loop;

bool _service_subscribe_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_is_telephony_ready_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_power_set_cb(LSHandle* lshandle, LSMessage *message, void *user_data);
bool _service_power_query_cb(LSHandle *lshandle, LSMessage *message, void *user_data);
bool _service_platform_query_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_sim_status_query_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_pin1_status_query_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_pin1_verify_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_pin1_enable_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_pin1_disable_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_pin1_change_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_pin1_unblock_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_signal_strength_query_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_network_status_query_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_network_list_query_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_network_list_query_cancel_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_network_id_query_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_network_selection_mode_query_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_network_set_cb(LSHandle *handle, LSMessage *message, void *user_data);

static LSMethod _telephony_service_methods[]  = {
	{ "subscribe", _service_subscribe_cb },
	{ "isTelephonyReady", _service_is_telephony_ready_cb },
	{ "powerSet", _service_power_set_cb },
	{ "powerQuery", _service_power_query_cb },
	{ "platformQuery", _service_platform_query_cb },
	{ "simStatusQuery", _service_sim_status_query_cb },
	{ "pin1StatusQuery", _service_pin1_status_query_cb },
	{ "pin1Verify", _service_pin1_verify_cb },
	{ "pin1Enable", _service_pin1_enable_cb },
	{ "pin1Disable", _service_pin1_disable_cb },
	{ "pin1Change", _service_pin1_change_cb },
	{ "pin1Unblock", _service_pin1_unblock_cb },
	{ "signalStrengthQuery", _service_signal_strength_query_cb },
	{ "networkStatusQuery", _service_network_status_query_cb },
	{ "networkListQuery", _service_network_list_query_cb },
	{ "networkListQueryCancel", _service_network_list_query_cancel_cb },
	{ "netorkIdQuery", _service_network_id_query_cb },
	{ "networkSelectionModeQuery", _service_network_selection_mode_query_cb },
	{ "networkSet", _service_network_set_cb },
	{ 0, 0 }
};

static bool retrieve_power_state_from_settings(void)
{
	const char *setting_value = NULL;
	jvalue_ref parsed_obj;
	jvalue_ref state_obj;
	bool power_state = true;

	setting_value = telephony_settings_load(TELEPHONY_SETTINGS_TYPE_POWER_STATE);
	if (setting_value == NULL)
		return true;

	parsed_obj = luna_service_message_parse_and_validate(setting_value);
	if (jis_null(parsed_obj))
		return true;

	if (!jobject_get_exists(parsed_obj, J_CSTR_TO_BUF("state"), &state_obj))
		return true;

	jboolean_get(state_obj, &power_state);
	j_release(parsed_obj);
	return power_state;
}

static void store_power_state_setting(bool power_state)
{
	telephony_settings_store(TELEPHONY_SETTINGS_TYPE_POWER_STATE,
						power_state ? "{\"state\":true}" : "{\"state\":false}");
}

static int initialize_luna_service(struct telephony_service *service)
{
	LSError error;

	g_message("Initializing luna service ...");

	LSErrorInit(&error);

	if (!LSPalmServiceRegisterCategory(service->palm_service, "/", NULL, _telephony_service_methods,
			NULL, service, &error)) {
		g_warning("Could not register service category");
		LSErrorFree(&error);
		return -EIO;
	}

	return 0;
}

int _service_initial_power_set_finish(const struct telephony_error *error, void *data)
{
	return 0;
}

static int configure_service(struct telephony_service *service)
{
	bool power_state = true;

	power_state = retrieve_power_state_from_settings();
	if (!service->driver || !service->driver->power_set) {
		g_warning("API method powerSet not available for setting initial power mode");
		return -EINVAL;
	}

	if (service->driver->power_set(service, power_state, _service_initial_power_set_finish, service) < 0) {
		g_warning("Failed to set initial power state");
		return -EIO;
	}

	return 0;
}

static void shutdown_luna_service(struct telephony_service *service)
{
	g_message("Shutting down luna service ...");
}

struct telephony_service* telephony_service_create(LSPalmService *palm_service)
{
	struct telephony_service *service;

	service = g_try_new0(struct telephony_service, 1);
	if (!service)
		return NULL;

	service->palm_service = palm_service;
	service->private_service = LSPalmServiceGetPrivateConnection(palm_service);
	service->initialized = false;
	service->power_off_pending = false;
	service->network_status_query_pending = false;

	if (initialize_luna_service(service) < 0)
		g_critical("Failed to initialize luna service. Wront service configuration?");

	return service;
}

void telephony_service_free(struct telephony_service *service)
{
	shutdown_luna_service(service);

	if (service->driver) {
		service->driver->remove(service);
		service->driver = NULL;
	}

	g_free(service);
}

void telephony_service_set_data(struct telephony_service *service, void *data)
{
	g_assert(service != NULL);
	service->data = data;
}

void* telephony_service_get_data(struct telephony_service *service)
{
	g_assert(service != NULL);
	return service->data;
}

void telephony_service_availability_changed_notify(struct telephony_service *service, bool available)
{
	if (!service)
		return;

	g_debug("Availability of the telephony service changed to: %s", available ? "available" : "not available");

	if (!service->initialized && available) {
		if (configure_service(service) < 0) {
			g_error("Could not configure service");
			return;
		}
	}

	service->initialized = available;
}

void telephony_service_register_driver(struct telephony_service *service, struct telephony_driver *driver)
{
	int err;

	if (service->driver) {
		g_warning("Can not register a second telephony driver");
		return;
	}

	service->driver = driver;

	if (service->driver->probe(service) < 0) {
		g_warning("Telephony driver failed to initialize");
		service->driver = NULL;
	}
}

int telephonyservice_common_finish(const struct telephony_error *error, void *data)
{
	struct luna_service_req_data *req_data = data;
	jvalue_ref reply_obj = NULL;
	bool success = (error == NULL);

	reply_obj = jobject_create();

	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(success));

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
 * @brief Check wether telephony service is ready
 **/

bool _service_is_telephony_ready_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct telephony_service *service = user_data;
	jvalue_ref reply_obj = NULL;
	jvalue_ref extended_obj = NULL;

	reply_obj = jobject_create();
	extended_obj = jobject_create();

	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
	jobject_put(extended_obj, J_CSTR_TO_JVAL("radioConnected"), jboolean_create(service->initialized));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("extended"), extended_obj);

	if(!luna_service_message_validate_and_send(handle, message, reply_obj))
		luna_service_message_reply_error_internal(handle, message);

	j_release(reply_obj);

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
	bool result = false;
	LSError lserror;
	char *payload;

	if (!service->initialized)
		goto cleanup;

	payload = LSMessageGetPayload(message);
	parsed_obj = luna_service_message_parse_and_validate(payload);
	if (jis_null(parsed_obj))
		goto cleanup;

	if (!jobject_get_exists(parsed_obj, J_CSTR_TO_BUF("events"), &events_obj))
		goto cleanup;

	if (jstring_equal2(events_obj, J_CSTR_TO_BUF("network"))) {
		result = LSSubscriptionAdd(handle, "/networkStatusQuery", message, &lserror);
		if (!result) {
			LSErrorPrint(&lserror, stderr);
			LSErrorFree(&lserror);
			goto cleanup;
		}
	}
	else if (jstring_equal2(events_obj, J_CSTR_TO_BUF("signal"))) {
		result = LSSubscriptionAdd(handle, "/signalStrengthQuery", message, &lserror);
		if (!result) {
			LSErrorPrint(&lserror, stderr);
			LSErrorFree(&lserror);
			goto cleanup;
		}
	}

cleanup:
	j_release(&parsed_obj);

	return true;
}

// vim:ts=4:sw=4:noexpandtab
