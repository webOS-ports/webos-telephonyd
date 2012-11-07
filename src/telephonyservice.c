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
#include <luna-service2/lunaservice.h>

#include "telephonydriver.h"

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

bool _service_power_set_cb(LSHandle* lshandle, LSMessage *message, void *user_data)
{
	return true;
}

// vim:ts=4:sw=4:noexpandtab
