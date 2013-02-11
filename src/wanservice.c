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

#include "wandriver.h"
#include "utils.h"
#include "luna_service_utils.h"

extern GMainLoop *event_loop;

struct wan_service {
	struct wan_driver *driver;
	void *data;
	LSPalmService *palm_service;
	LSHandle *private_service;
};

static LSMethod _wan_service_methods[]  = {
	{ 0, 0 }
};

struct wan_service* wan_service_create(void)
{
	struct wan_service *service;
	LSError error;

	service = g_try_new0(struct wan_service, 1);
	if (!service)
		return NULL;

	LSErrorInit(&error);

	if (!LSRegisterPalmService("com.palm.wan", &service->palm_service, &error)) {
		g_error("Failed to initialize the WAN service: %s", error.message);
		LSErrorFree(&error);
		goto error;
	}

	if (!LSGmainAttachPalmService(service->palm_service, event_loop, &error)) {
		g_error("Failed to attach to glib mainloop for WAN service: %s", error.message);
		LSErrorFree(&error);
		goto error;
	}

	if (!LSPalmServiceRegisterCategory(service->palm_service, "/", NULL, _wan_service_methods,
			NULL, service, &error)) {
		g_warning("Could not register category for WAN service");
		LSErrorFree(&error);
		return NULL;
	}

	service->private_service = LSPalmServiceGetPrivateConnection(service->palm_service);

	return service;

error:
	if (service->palm_service &&
		LSUnregisterPalmService(service->palm_service, &error) < 0) {
		g_error("Could not unregister palm service: %s", error.message);
		LSErrorFree(&error);
	}

	g_free(service);

	return NULL;
}

void wan_service_free(struct wan_service *service)
{
	LSError error;

	LSErrorInit(&error);

	if (service->palm_service != NULL &&
		LSUnregisterPalmService(service->palm_service, &error) < 0) {
		g_error("Could not unregister palm service: %s", error.message);
		LSErrorFree(&error);
	}

	if (service->driver) {
		service->driver->remove(service);
		service->driver = NULL;
	}

	g_free(service);
}

void wan_service_set_data(struct wan_service *service, void *data)
{
	g_assert(service != NULL);
	service->data = data;
}

void* wan_service_get_data(struct wan_service *service)
{
	g_assert(service != NULL);
	return service->data;
}

void wan_service_register_driver(struct wan_service *service, struct telephony_driver *driver)
{
	if (service->driver) {
		g_warning("Can not register a second WAN driver");
		return;
	}

	service->driver = driver;

	if (service->driver->probe(service) < 0) {
		g_warning("WAN driver failed to initialize");
		service->driver = NULL;
	}
}

void wan_service_unregister_driver(struct wan_service *service, struct telephony_driver *driver)
{
	if (!service->driver || service->driver != driver)
		return;

	service->driver = NULL;
}

// vim:ts=4:sw=4:noexpandtab
