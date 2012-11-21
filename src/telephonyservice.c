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
#include <string.h>
#include <glib.h>
#include <pbnjson.h>
#include <luna-service2/lunaservice.h>

#include "telephonydriver.h"
#include "utils.h"

struct telephony_service {
	struct telephony_driver *driver;
	void *data;
	LSHandle *private_service;
};

bool _service_power_set_cb(LSHandle* lshandle, LSMessage *message, void *user_data);

static LSMethod _telephony_service_methods[]  = {
	{ "powerSet", _service_power_set_cb },
	{ 0, 0 }
};

struct telephony_service* telephony_service_create(LSPalmService *palm_service)
{
	struct telephony_service *service;
	LSError error;

	service = g_try_new0(struct telephony_service, 1);
	if (!service)
		return NULL;

	service->private_service = LSPalmServiceGetPrivateConnection(palm_service);

	LSErrorInit(&error);

	if (!LSPalmServiceRegisterCategory(palm_service, "/", NULL, _telephony_service_methods,
			NULL, service, &error)) {
		g_error("Could not register service category");
		LSErrorFree(&error);
	}

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

void telephony_service_register_driver(struct telephony_service *service, struct telephony_driver *driver)
{
	service->driver = driver;

	/* FIXME maybe move probing to somewhere else */
	if (service->driver->probe(service) < 0) {
		g_error("Telephony driver failed to initialize");
		service->driver = NULL;
	}
}

int _service_power_set_finish(bool success, void *data)
{
	struct service_request_data *req_data = data;
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

	if (LSMessageReply(req_data->handle, req_data->message,
					jvalue_tostring(reply_obj, response_schema), &lserror)) {
		LSErrorPrint(&lserror, stderr);
		LSErrorFree(&lserror);
		goto cleanup;
	}

cleanup:
	j_release(reply_obj);
	g_free(req_data);
	return 0;
}

bool _service_power_set_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct telephony_service *service = user_data;
	struct service_request_data *req_data = NULL;
	bool power = false;
	jschema_ref input_schema = NULL;
	jvalue_ref parsed_obj = NULL;
	jvalue_ref state_obj = NULL;
	JSchemaInfo schema_info;
	LSError error;
	const char *payload;
	const char *state_value;

	if (!service->driver || !service->driver->power_set) {
		g_error("No implementation available for service powerSet API method");
		luna_service_message_reply_error_not_implemented(handle, message);
		goto cleanup;
	}

	input_schema = jschema_parse(j_cstr_to_buffer("{}"), DOMOPT_NOOPT, NULL);
	if (!input_schema) {
		g_error("Failed to create json validation schema");
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

	req_data = service_request_data_new(handle, message);

	if (service->driver->power_set(service, power, _service_power_set_finish, req_data) < 0) {
		g_error("Failed to process service powerSet request in our driver");
		luna_service_message_reply_custom_error(handle, message, "Failed to set power mode");
		goto cleanup;
	}

cleanup:
	if (!jis_null(parsed_obj))
		j_release(&parsed_obj);

	if (req_data)
		g_free(req_data);

	return true;
}

// vim:ts=4:sw=4:noexpandtab
