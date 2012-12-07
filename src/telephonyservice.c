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

#include "telephonydriver.h"
#include "utils.h"

extern GMainLoop *event_loop;

struct telephony_service {
	struct telephony_driver *driver;
	void *data;
	LSPalmService *palm_service;
	LSHandle *private_service;
	bool initialized;
};

bool _service_power_set_cb(LSHandle* lshandle, LSMessage *message, void *user_data);
bool _service_power_query_cb(LSHandle *lshandle, LSMessage *message, void *user_data);

static LSMethod _telephony_service_methods[]  = {
	{ "powerSet", _service_power_set_cb },
	{ "powerQuery", _service_power_query_cb },
	{ 0, 0 }
};

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

	return service;
}

void telephony_service_free(struct telephony_service *service)
{
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

	/* If backend is now available but service is not yet initialized do it now */
	if (!service->initialized && available) {
		if (initialize_luna_service(service) < 0) {
			g_critical("Failed to initialize luna service. Wront service configuration?");
			return;
		}
	}
	else if (service->initialized && !available)
		shutdown_luna_service(service);

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

void telephony_service_power_status_notify(struct telephony_service *service, bool power)
{
	jvalue_ref reply_obj = NULL;
	jschema_ref response_schema = NULL;
	LSError lserror;

	LSErrorInit(&lserror);

	reply_obj = jobject_create();
	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("eventPower"), jboolean_create(power));

	response_schema = jschema_parse (j_cstr_to_buffer("{}"), DOMOPT_NOOPT, NULL);
	if(!response_schema)
		goto cleanup;

	if (!LSSubscriptionPost(service->private_service, "/", "powerQuery",
						jvalue_tostring(reply_obj, response_schema), &lserror)) {
		LSErrorPrint(&lserror, stderr);
		LSErrorFree(&lserror);
	}

cleanup:
	if (response_schema)
		jschema_release(&response_schema);

	j_release(&reply_obj);
}

int _service_power_set_finish(bool success, void *data)
{
	struct luna_service_req_data *req_data = data;
	jvalue_ref reply_obj = NULL;
	jschema_ref response_schema = NULL;
	LSError lserror;

	LSErrorInit(&lserror);

	reply_obj = jobject_create();

	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(success));

	response_schema = jschema_parse (j_cstr_to_buffer("{}"), DOMOPT_NOOPT, NULL);
	if(!response_schema)
	{
		luna_service_message_reply_error_internal(req_data->handle, req_data->message);
		goto cleanup;
	}

	if (!LSMessageReply(req_data->handle, req_data->message,
					jvalue_tostring(reply_obj, response_schema), &lserror)) {
		LSErrorPrint(&lserror, stderr);
		LSErrorFree(&lserror);
		goto cleanup;
	}

cleanup:
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
	jschema_ref input_schema = NULL;
	jvalue_ref parsed_obj = NULL;
	jvalue_ref state_obj = NULL;
	JSchemaInfo schema_info;
	LSError error;
	const char *payload;
	const char *state_value;

	if (!service->initialized) {
		luna_service_message_reply_custom_error(handle, message, "Service not yet successfully initialized.");
		goto cleanup;
	}

	if (!service->driver || !service->driver->power_set) {
		g_warning("No implementation available for service powerSet API method");
		luna_service_message_reply_error_not_implemented(handle, message);
		goto cleanup;
	}

	input_schema = jschema_parse(j_cstr_to_buffer("{}"), DOMOPT_NOOPT, NULL);
	if (!input_schema) {
		g_warning("Failed to create json validation schema");
		luna_service_message_reply_error_internal(handle, message);
		goto cleanup;
	}

	payload = LSMessageGetPayload(message);

	jschema_info_init(&schema_info, input_schema, NULL, NULL);
	parsed_obj = jdom_parse(j_cstr_to_buffer(payload), DOMOPT_NOOPT, &schema_info);
	jschema_release(&input_schema);

	if (jis_null(parsed_obj)) {
		luna_service_message_reply_error_bad_json(handle, message);
		goto cleanup;
	}

	if (!jobject_get_exists(parsed_obj, J_CSTR_TO_BUF("state"), &state_obj)) {
		luna_service_message_reply_error_bad_json(handle, message);
		goto cleanup;
	}

	if (jstring_equal2(state_obj, J_CSTR_TO_BUF("on"))) {
		power = true;
	}
	else if (jstring_equal2(state_obj, J_CSTR_TO_BUF("off"))) {
		power = false;
	}
	/* FIXME we're not supporting saving the power state yet so default will always power
	 * up the service */
	else if (jstring_equal2(state_obj, J_CSTR_TO_BUF("default"))) {
		power = true;
	}
	else {
		luna_service_message_reply_error_bad_json(handle, message);
		goto cleanup;
	}

	req_data = luna_service_req_data_new(handle, message);

	if (service->driver->power_set(service, power, _service_power_set_finish, req_data) < 0) {
		g_warning("Failed to process service powerSet request in our driver");
		luna_service_message_reply_custom_error(handle, message, "Failed to set power mode");
		goto cleanup;
	}

	return true;

cleanup:
	if (!jis_null(parsed_obj))
		j_release(&parsed_obj);

	if (req_data)
		luna_service_req_data_free(req_data);

	return true;
}

int _service_power_query_finish(bool success, bool power, void *data)
{
	struct luna_service_req_data *req_data = data;
	jvalue_ref reply_obj = NULL;
	jvalue_ref extended_obj = NULL;
	jschema_ref response_schema = NULL;
	LSError lserror;
	bool subscribed = false;

	LSErrorInit(&lserror);

	reply_obj = jobject_create();
	extended_obj = jobject_create();

	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(success));

	/* handle possible subscriptions */
	if (LSMessageIsSubscription(req_data->message)) {
		if (!LSSubscriptionProcess(req_data->handle, req_data->message, &subscribed, &lserror)) {
			LSErrorPrint(&lserror, stderr);
			LSErrorFree(&lserror);
		}

		jobject_put(reply_obj, J_CSTR_TO_JVAL("subscribed"), jboolean_create(subscribed));
	}

	if (success) {
		jobject_put(extended_obj, J_CSTR_TO_JVAL("powerState"), jstring_create(power ? "on" : "off"));
		jobject_put(reply_obj, J_CSTR_TO_JVAL("extended"), extended_obj);
	}

	response_schema = jschema_parse (j_cstr_to_buffer("{}"), DOMOPT_NOOPT, NULL);
	if(!response_schema)
	{
		luna_service_message_reply_error_internal(req_data->handle, req_data->message);
		goto cleanup;
	}

	if (!LSMessageReply(req_data->handle, req_data->message,
					jvalue_tostring(reply_obj, response_schema), &lserror)) {
		LSErrorPrint(&lserror, stderr);
		LSErrorFree(&lserror);
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
	jschema_ref input_schema = NULL;
	jvalue_ref parsed_obj = NULL;
	JSchemaInfo schema_info;
	LSError error;
	const char *payload;

	if (!service->initialized) {
		luna_service_message_reply_custom_error(handle, message, "Service not yet successfully initialized.");
		goto cleanup;
	}

	if (!service->driver || !service->driver->power_query) {
		g_warning("No implementation available for service powerQuery API method");
		luna_service_message_reply_error_not_implemented(handle, message);
		goto cleanup;
	}

	payload = LSMessageGetPayload(message);

	input_schema = jschema_parse(j_cstr_to_buffer("{}"), DOMOPT_NOOPT, NULL);

	jschema_info_init(&schema_info, input_schema, NULL, NULL);
	parsed_obj = jdom_parse(j_cstr_to_buffer(payload), DOMOPT_NOOPT, &schema_info);
	jschema_release(&input_schema);

	if (jis_null(parsed_obj)) {
		luna_service_message_reply_error_bad_json(handle, message);
		goto cleanup;
	}

	req_data = luna_service_req_data_new(handle, message);

	if (service->driver->power_query(service, _service_power_query_finish, req_data) < 0) {
		g_warning("Failed to process service powerQuery request in our driver");
		luna_service_message_reply_custom_error(handle, message, "Failed to query power status");
		goto cleanup;
	}

	return true;

cleanup:
	if (!jis_null(parsed_obj))
		j_release(&parsed_obj);

	if (req_data)
		luna_service_req_data_free(req_data);

	return true;
}

// vim:ts=4:sw=4:noexpandtab
