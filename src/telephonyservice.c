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
#include <cjson/json.h>
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
	struct json_object *response_object;
	LSError error;

	response_object = json_object_new_object();

	json_object_object_add(response_object, "returnValue", json_object_new_boolean(success));

	if (LSMessageReply(req_data->handle, req_data->message, json_object_to_json_string(response_object), &error)) {
		LSErrorFree(&error);
		return -1;
	}

	json_object_put(response_object);

	g_free(req_data);

	return 0;
}

bool _service_power_set_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct telephony_service *service = user_data;
	struct service_request_data *req_data;
	bool power = false;
	struct json_object *request_object;
	struct json_object *response_object;
	struct json_object *state_object;
	LSError error;
	const char *payload;
	const char *state_value;

	if (!service->driver || !service->driver->power_set) {
		g_error("No implementation available for service powerSet API method");
		return false;
	}

	payload = LSMessageGetPayload(message);
	if (!payload)
		return false;

	request_object = json_tokener_parse(payload);
	if (!request_object || is_error(request_object)) {
		request_object = 0;
		goto error;
	}

	if (!json_object_object_get_ex(request_object, "state", &state_object))
		goto error;

	state_value = json_object_get_string(state_object);

	if (!strncmp(state_value, "on", 2))
		power = true;
	else if (!strncmp(state_value, "off", 3))
		power = false;
	/* FIXME we're not supporting saving the power state yet so default will always power
	 * up the service */
	else if (!strncmp(state_value, "default", 7))
		power = true;

	req_data = service_request_data_new(handle, message);

	if (service->driver->power_set(service, power, _service_power_set_finish, req_data) < 0) {
		g_error("Failed to process service powerSet request in our driver");
		goto error;
	}

	return true;

error:
	response_object = json_object_new_object();
	json_object_object_add(response_object, "returnValue", json_object_new_boolean(false));

	if (LSMessageReply(handle, message, json_object_to_json_string(response_object), &error)) {
		LSErrorPrint(&error, stderr);
		LSErrorFree(&error);
	}

	json_object_put(response_object);

	if (request_object)
		json_object_put(request_object);

	return true;
}

// vim:ts=4:sw=4:noexpandtab
