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
#include "wanservice.h"
#include "telephonysettings.h"
#include "utils.h"
#include "luna_service_utils.h"

extern GMainLoop *event_loop;
static GSList *g_driver_list;
/* the process hosts exactly one wan service */
static struct wan_service *g_wan_service;

struct wan_service {
	struct wan_driver *driver;
	void *data;
	LSHandle *serviceHandle;
	bool initialized;
};

bool _wan_service_getstatus_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _wan_service_set_cb(LSHandle *handle, LSMessage *message, void *user_data);

static LSMethod _wan_service_methods[]  = {
	{ "getstatus", _wan_service_getstatus_cb },
	{ "set", _wan_service_set_cb },
	{ 0, 0 }
};

void wan_service_set_data_sim(int sim_id)
{
	if (!g_wan_service || !g_wan_service->driver || !g_wan_service->driver->set_data_sim)
		return;

	g_wan_service->driver->set_data_sim(g_wan_service, sim_id);
}

int wan_service_get_data_sim(void)
{
	if (!g_wan_service || !g_wan_service->driver || !g_wan_service->driver->get_data_sim)
		return -1;

	return g_wan_service->driver->get_data_sim(g_wan_service);
}

const char* wan_network_type_to_string(enum wan_network_type type)
{
	switch (type) {
	case WAN_NETWORK_TYPE_GPRS:
		return "gprs";
	case WAN_NETWORK_TYPE_EDGE:
		return "edge";
	case WAN_NETWORK_TYPE_UMTS:
		return "umts";
	case WAN_NETWORK_TYPE_HSDPA:
		return "hsdpa";
	case WAN_NETWORK_TYPE_LTE:
		return "lte";
	case WAN_NETWORK_TYPE_1X:
		return "1x";
	case WAN_NETWORK_TYPE_EVDO:
		return "evdo";
	default:
		break;
	}

	return "none";
}

static const char* wan_error_to_string(int code)
{
	switch (code) {
	case WAN_ERROR_NOT_IMPLEMENTED:
		return "Not implemented";
	case WAN_ERROR_INTERNAL:
		return "Internal error";
	case WAN_ERROR_INVALID_ARGUMENT:
		return "Invalid argument";
	case WAN_ERROR_NOT_AVAILABLE:
		return "Not available";
	case WAN_ERROR_ALREADY_INPROGRESS:
		return "Already in progress";
	default:
		break;
	}

	return "Failed";
}

const char* wan_status_type_to_string(enum wan_status_type status)
{
	switch (status) {
	case WAN_STATUS_TYPE_DISABLE:
		return "disable";
	case WAN_STATUS_TYPE_DISABLING:
		return "disabling";
	case WAN_STATUS_TYPE_ENABLE:
		return "enable";
	}

	return NULL;
}

const char* wan_connection_status_to_string(enum wan_connection_status status)
{
	switch (status) {
	case WAN_CONNECTION_STATUS_ACTIVE:
		return "active";
	case WAN_CONNECTION_STATUS_CONNECTING:
		return "connecting";
	case WAN_CONNECTION_STATUS_DISCONNECTED:
		return "disconnected";
	case WAN_CONNECTION_STATUS_DISCONNECTING:
		return "disconnecting";
	case WAN_CONNECTION_STATUS_DORMANT:
		return "dormant";
	}

	/* jstring_create() must never see NULL */
	return "unknown";
}

const char* wan_service_type_to_string(enum wan_service_type type)
{
	switch (type) {
	case WAN_SERVICE_TYPE_INTERNET:
		return "internet";
	case WAN_SERVICE_TYPE_MMS:
		return "mms";
	case WAN_SERVICE_TYPE_SPRINT_PROVISIONING:
		return "sprintProvisioning";
	case WAN_SERVICE_TYPE_TETHERED:
		return "tethered";
	default:
		break;
	}

	return "unknown";
}

const char* wan_request_status_to_string(enum wan_request_status status)
{
	switch (status) {
	case WAN_REQUEST_STATUS_CONNECT_FAILED:
		return "connect failed";
	case WAN_REQUEST_STATUS_CONNECT_SUCCEEDED:
		return "connect succeeded";
	case WAN_REQUEST_STATUS_DISCONNECT_FAILED:
		return "disconnect failed";
	case WAN_REQUEST_STATUS_DISCONNECT_SUCCEEDED:
		return "disconnect succeeded";
	case WAN_REQUEST_STATUS_EVENT:
		return "event";
	default:
		break;
	}

	/* jstring_create() must never see NULL */
	return "unknown";
}

struct wan_service* wan_service_create(void)
{
	struct wan_service *service;
	LSError error;

	if (g_driver_list == NULL) {
		g_warning("Can't create WAN service as no suitable driver is available");
		return NULL;
	}

	service = g_try_new0(struct wan_service, 1);
	if (!service)
		return NULL;

	/* take first driver until we have some mechanism to determine the best driver */
	service->driver = g_driver_list->data;

	if (service->driver->probe(service) < 0) {
		g_free(service);
		return NULL;
	}

	LSErrorInit(&error);

	if (!LSRegister("com.palm.wan", &service->serviceHandle, &error)) {
		g_critical("Failed to initialize the WAN service: %s", error.message);
		LSErrorFree(&error);
		goto error;
	}

	if (!LSGmainAttach(service->serviceHandle, event_loop, &error)) {
		g_critical("Failed to attach to glib mainloop for WAN service: %s", error.message);
		LSErrorFree(&error);
		goto error;
	}

	if (!LSRegisterCategory(service->serviceHandle, "/", _wan_service_methods,
			NULL, NULL, &error)) {
		g_critical("Could not register category for WAN service");
		LSErrorFree(&error);
		goto error;
	}

	if (!LSCategorySetData(service->serviceHandle, "/", service, &error)) {
		g_warning("Could not set data for service category");
		LSErrorFree(&error);
		goto error;
	}

	g_wan_service = service;

	return service;

error:
	if (service->serviceHandle &&
		!LSUnregister(service->serviceHandle, &error)) {
		g_critical("Could not unregister service: %s", error.message);
		LSErrorFree(&error);
	}

	if (service->driver)
		service->driver->remove(service);

	g_free(service);

	return NULL;
}

void wan_service_free(struct wan_service *service)
{
	if (g_wan_service == service)
		g_wan_service = NULL;

	LSError error;

	LSErrorInit(&error);

	if (service->serviceHandle != NULL &&
		!LSUnregister(service->serviceHandle, &error)) {
		g_critical("Could not unregister service: %s", error.message);
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

int wan_driver_register(struct wan_driver *driver)
{
	if (driver->probe == NULL)
		return -EINVAL;

	g_driver_list = g_slist_prepend(g_driver_list, driver);

	return 0;
}

void wan_driver_unregister(struct wan_driver *driver)
{
	g_driver_list = g_slist_remove(g_driver_list, driver);
}

static jvalue_ref create_status_update_reply(struct wan_status *status)
{
	jvalue_ref reply_obj = NULL;
	jvalue_ref connected_services_obj = NULL;
	jvalue_ref service_obj = NULL;
	jvalue_ref services_obj = NULL;
	int n;
	struct wan_connected_service *wanservice;
	GSList *iter;
	const char *wanstatus;

	reply_obj = jobject_create();

	/* tell clients which SIM the reported connection belongs to */
	jobject_put(reply_obj, J_CSTR_TO_JVAL("simId"), jnumber_create_i32(wan_service_get_data_sim()));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("state"),
				jstring_create(status->state ? "enable" : "disable"));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("roamguard"),
				jstring_create(status->roam_guard ? "enable" : "disable"));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("networktype"),
				jstring_create(wan_network_type_to_string(status->network_type)));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("dataaccess"),
				jstring_create(status->dataaccess_usable ? "usable" : "unusable"));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("networkstatus"),
				jstring_create(status->network_attached ? "attached" : "notattached"));

	wanstatus = wan_status_type_to_string(status->wan_status);
	if (wanstatus)
		jobject_put(reply_obj, J_CSTR_TO_JVAL("wanstate"),
				jstring_create(wanstatus));

	jobject_put(reply_obj, J_CSTR_TO_JVAL("disablewan"),
				jstring_create(status->disablewan ? "on" : "off"));

	connected_services_obj = jarray_create(NULL);
	for (iter = status->connected_services; iter != NULL; iter = g_slist_next(iter)) {
		wanservice = iter->data;

		service_obj = jobject_create();
		services_obj = jarray_create(NULL);

		for (n = 0; n < WAN_SERVICE_TYPE_MAX; n++) {
			if (wanservice->services[n]) {
				jarray_append(services_obj,
					jstring_create(wan_service_type_to_string((enum wan_service_type) n)));
			}
		}

		jobject_put(service_obj, J_CSTR_TO_JVAL("service"), services_obj);
		jobject_put(service_obj, J_CSTR_TO_JVAL("cid"),
					jnumber_create_i32(wanservice->cid));
		jobject_put(service_obj, J_CSTR_TO_JVAL("connectstatus"),
					jstring_create(wan_connection_status_to_string(wanservice->connection_status)));
		jobject_put(service_obj, J_CSTR_TO_JVAL("ipaddress"),
					jstring_create(wanservice->ipaddress ? wanservice->ipaddress : ""));
		jobject_put(service_obj, J_CSTR_TO_JVAL("requeststatus"),
					jstring_create(wan_request_status_to_string(wanservice->req_status)));
		jobject_put(service_obj, J_CSTR_TO_JVAL("errorCode"),
					jnumber_create_i32(wanservice->error_code));
		jobject_put(service_obj, J_CSTR_TO_JVAL("causeCode"),
					jnumber_create_i32(wanservice->cause_code));
		jobject_put(service_obj, J_CSTR_TO_JVAL("mipFailureCode"),
					jnumber_create_i32(wanservice->mip_failure_code));

		jarray_append(connected_services_obj, service_obj);
	}

	jobject_put(reply_obj, J_CSTR_TO_JVAL("connectedservices"), connected_services_obj);

	return reply_obj;
}

void wan_service_status_changed_notify(struct wan_service *service, struct wan_status *status)
{
	jvalue_ref reply_obj = NULL;

	reply_obj = create_status_update_reply(status);
	jobject_put(reply_obj, J_CSTR_TO_JVAL("subscribed"), jboolean_create(true));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));

	luna_service_post_subscription(service->serviceHandle, "/", "getstatus", reply_obj);

	j_release(&reply_obj);
}

static void get_status_cb(const struct wan_error *error, struct wan_status *status, void *data)
{
	struct luna_service_req_data *req_data = data;
	jvalue_ref reply_obj = NULL;

	if (error || !status) {
		reply_obj = jobject_create();
		jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(false));
		jobject_put(reply_obj, J_CSTR_TO_JVAL("errorCode"),
					jnumber_create_i32(error ? error->code : WAN_ERROR_INTERNAL));
		jobject_put(reply_obj, J_CSTR_TO_JVAL("errorText"),
					jstring_create(wan_error_to_string(error ? error->code : WAN_ERROR_INTERNAL)));
	}
	else {
		reply_obj = create_status_update_reply(status);
		jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
	}

	luna_service_message_validate_and_send(req_data->handle, req_data->message, reply_obj);

	j_release(&reply_obj);
	luna_service_req_data_free(req_data);
}

bool _wan_service_getstatus_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct wan_service *service = user_data;
	jvalue_ref reply_obj = NULL;
	bool subscribed = false;
	struct luna_service_req_data *req_data = NULL;

	if (!service->driver || !service->driver->get_status) {
		g_warning("No implementation available for service getstatus API method");
		luna_service_message_reply_error_not_implemented(handle, message);
		return true;
	}

	subscribed = luna_service_check_for_subscription_and_process(handle, message);

	reply_obj = jobject_create();

	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("errorCode"), jnumber_create_i32(0));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("errorText"), jstring_create("success"));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("subscribed"), jboolean_create(subscribed));

	if(!luna_service_message_validate_and_send(handle, message, reply_obj))
		luna_service_message_reply_error_internal(handle, message);

	j_release(&reply_obj);

	/* Trigger a status update so connected client gets an reply immediately */
	if (subscribed) {
		req_data = luna_service_req_data_new(handle, message);
		service->driver->get_status(service, get_status_cb, req_data);
	}

	return true;
}

#define is_flag_set(flags, flag) \
	((((flags) & (flag))) == (flag))

static void _service_set_finish(const struct wan_error *error, void *data)
{
	struct luna_service_req_data *req_data = data;
	jvalue_ref reply_obj = NULL;
	bool success = (error == NULL);

	reply_obj = jobject_create();

	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(success));
	if (!success) {
		/* This used to answer errorCode 0 with an empty errorText, which told a
		 * caller nothing about why its set was refused. */
		jobject_put(reply_obj, J_CSTR_TO_JVAL("errorCode"), jnumber_create_i32(error->code));
		jobject_put(reply_obj, J_CSTR_TO_JVAL("errorText"),
					jstring_create(wan_error_to_string(error->code)));
	}

	if(!luna_service_message_validate_and_send(req_data->handle, req_data->message, reply_obj)) {
		luna_service_message_reply_error_internal(req_data->handle, req_data->message);
		goto cleanup;
	}

cleanup:
	j_release(&reply_obj);
	luna_service_req_data_free(req_data);
}

bool _wan_service_set_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct wan_service *service = user_data;
	struct luna_service_req_data *req_data = NULL;
	jvalue_ref parsed_obj = NULL;
	jvalue_ref disablewan_obj = NULL;
	jvalue_ref roamguard_obj = NULL;
	const char *payload;
	/* Each request carries its own configuration: writing into shared state
	 * let a second set clobber one still travelling through the driver. The
	 * driver copies what it needs before this handler returns. */
	struct wan_configuration configuration;

	if (!service->driver || !service->driver->set_configuration) {
		g_warning("No implementation available for service set API method");
		luna_service_message_reply_error_not_implemented(handle, message);
		return true;
	}

	payload = LSMessageGetPayload(message);
	parsed_obj = luna_service_message_parse_and_validate(payload);
	if (jis_null(parsed_obj)) {
		luna_service_message_reply_error_bad_json(handle, message);
		goto cleanup;
	}

	memset(&configuration, 0, sizeof(configuration));

	if (jobject_get_exists(parsed_obj, J_CSTR_TO_BUF("disablewan"), &disablewan_obj)) {
		if (jstring_equal2(disablewan_obj, J_CSTR_TO_BUF("on")))
			configuration.disablewan = true;
		else if (jstring_equal2(disablewan_obj, J_CSTR_TO_BUF("off")))
			configuration.disablewan = false;

		configuration.flags |= WAN_CONFIGURATION_TYPE_DISABLEWAN;
	}

	if (jobject_get_exists(parsed_obj, J_CSTR_TO_BUF("roamguard"), &roamguard_obj)) {
		if (jstring_equal2(roamguard_obj, J_CSTR_TO_BUF("enable"))) {
			configuration.roamguard = true;
		}
		else if (jstring_equal2(roamguard_obj, J_CSTR_TO_BUF("disable"))) {
			configuration.roamguard = false;
		}

		configuration.flags |= WAN_CONFIGURATION_TYPE_ROAMGUARD;
	}

	req_data = luna_service_req_data_new(handle, message);
	req_data->user_data = service;

	service->driver->set_configuration(service, &configuration, _service_set_finish, req_data);

cleanup:
	if (!jis_null(parsed_obj))
		j_release(&parsed_obj);

	return true;
}

// vim:ts=4:sw=4:noexpandtab
