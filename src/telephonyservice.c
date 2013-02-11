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
static GSList *g_driver_list;

bool _service_subscribe_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_is_telephony_ready_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_power_set_cb(LSHandle* lshandle, LSMessage *message, void *user_data);
bool _service_power_query_cb(LSHandle *lshandle, LSMessage *message, void *user_data);
bool _service_platform_query_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_sim_status_query_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_pin1_status_query_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_pin2_status_query_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_pin1_verify_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_pin1_enable_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_pin1_disable_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_pin1_change_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_pin1_unblock_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_fdn_status_query_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_signal_strength_query_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_network_status_query_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_network_list_query_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_network_list_query_cancel_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_network_id_query_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_network_selection_mode_query_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_network_set_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_device_lock_query_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_charge_source_query_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_rat_query_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_rat_set_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_subscriber_id_query_cb(LSHandle *handle, LSMessage *message, void *user_data);

static LSMethod _telephony_service_methods[]  = {
	{ "subscribe", _service_subscribe_cb },
	{ "isTelephonyReady", _service_is_telephony_ready_cb },
	{ "powerSet", _service_power_set_cb },
	{ "powerQuery", _service_power_query_cb },
	{ "platformQuery", _service_platform_query_cb },
	{ "simStatusQuery", _service_sim_status_query_cb },
	{ "pin1StatusQuery", _service_pin1_status_query_cb },
	{ "pin2StatusQuery", _service_pin2_status_query_cb },
	{ "pin1Verify", _service_pin1_verify_cb },
	{ "pin1Enable", _service_pin1_enable_cb },
	{ "pin1Disable", _service_pin1_disable_cb },
	{ "pin1Change", _service_pin1_change_cb },
	{ "pin1Unblock", _service_pin1_unblock_cb },
	{ "fdnStatusQuery", _service_fdn_status_query_cb },
	{ "signalStrengthQuery", _service_signal_strength_query_cb },
	{ "networkStatusQuery", _service_network_status_query_cb },
	{ "networkListQuery", _service_network_list_query_cb },
	{ "networkListQueryCancel", _service_network_list_query_cancel_cb },
	{ "networkIdQuery", _service_network_id_query_cb },
	{ "networkSelectionModeQuery", _service_network_selection_mode_query_cb },
	{ "networkSet", _service_network_set_cb },
	{ "ratQuery", _service_rat_query_cb },
	{ "ratSet", _service_rat_set_cb },
	{ "deviceLockQuery", _service_device_lock_query_cb },
	{ "chargeSourceQuery", _service_charge_source_query_cb },
	{ "subscriberIdQuery", _service_subscriber_id_query_cb },
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
	j_release(&parsed_obj);
	return power_state;
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

struct telephony_service* telephony_service_create()
{
	struct telephony_service *service;
	LSError error;

	if (g_driver_list == NULL) {
		g_message("Can't create telephony servie as no suitable driver is available");
		return NULL;
	}

	service = g_try_new0(struct telephony_service, 1);
	if (!service)
		return NULL;

	/* take first driver until we have some machanism to determine the best driver */
	service->driver = g_driver_list->data;

	if (service->driver->probe(service) < 0) {
		g_free(service);
		return NULL;
	}

	service->initialized = false;
	service->power_off_pending = false;
	service->powered = false;
	service->network_status_query_pending = false;
	service->network_registered = false;

	LSErrorInit(&error);

	if (!LSRegisterPalmService("com.palm.telephony", &service->palm_service, &error)) {
		g_error("Failed to initialize the Luna Palm service: %s", error.message);
		LSErrorFree(&error);
		goto error;
	}

	if (!LSGmainAttachPalmService(service->palm_service, event_loop, &error)) {
		g_error("Failed to attach to glib mainloop for palm service: %s", error.message);
		LSErrorFree(&error);
		goto error;
	}

	if (!LSPalmServiceRegisterCategory(service->palm_service, "/", NULL, _telephony_service_methods,
			NULL, service, &error)) {
		g_warning("Could not register service category");
		LSErrorFree(&error);
		goto error;
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

void telephony_service_free(struct telephony_service *service)
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

int telephony_driver_register(struct telephony_driver *driver)
{
	if (driver->probe == NULL)
		return -EINVAL;

	g_driver_list = g_slist_prepend(g_driver_list, driver);

	return 0;
}

void telephony_driver_unregister(struct telephony_driver *driver)
{
	g_driver_list = g_slist_remove(g_driver_list, driver);
}

// vim:ts=4:sw=4:noexpandtab
