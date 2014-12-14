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

#include <glib.h>
#include <errno.h>
#include <string.h>

#include "utils.h"

#include "wanservice.h"
#include "wandriver.h"

#include "ofonobase.h"
#include "ofonomanager.h"
#include "ofonomodem.h"
#include "ofonoconnectioncontext.h"
#include "ofonoconnectionmanager.h"

struct ofono_wan_data {
	struct wan_service *service;
	guint service_watch;
	struct ofono_manager *manager;
	struct ofono_modem *modem;
	struct ofono_connection_manager *cm;
	bool status_update_pending;
};

enum wan_network_type convert_ofono_connection_bearer_to_wan_network_type(enum ofono_connection_bearer bearer)
{
	switch (bearer) {
	case OFONO_CONNECTION_BEARER_GPRS:
		return WAN_NETWORK_TYPE_GPRS;
	case OFONO_CONNECTION_BEARER_EDGE:
		return WAN_NETWORK_TYPE_EDGE;
	case OFONO_CONNECTION_BEARER_HSUPA:
	case OFONO_CONNECTION_BEARER_HSDPA:
	case OFONO_CONNECTION_BEARER_HSPA:
		return WAN_NETWORK_TYPE_HSDPA;
	case OFONO_CONNECTION_BEARER_UMTS:
		return WAN_NETWORK_TYPE_UMTS;
	default:
		break;
	}

	return WAN_NETWORK_TYPE_NONE;
}

static void free_used_instances(struct ofono_wan_data *od)
{
	if (od->manager) {
		ofono_manager_free(od->manager);
		od->manager = 0;
	}
}

static void context_prop_changed_cb(const char *name, void *data);

static void get_contexts_cb(const struct ofono_error *error, GSList *contexts, void *data)
{
	struct cb_data *cbd = data;
	wan_get_status_cb cb = cbd->cb;
	struct ofono_wan_data *od = cbd->user;
	struct wan_status status;
	struct ofono_connection_context *context;
	struct wan_connected_service *wanservice;
	GSList *iter;

	memset(&status, 0, sizeof(struct wan_status));

	status.state = ofono_connection_manager_get_powered(od->cm);

	/* FIXME until we have voicecall functionality and can determine wether we have an active call
	 * this is true forever */
	status.dataaccess_usable = true;

	status.wan_status = WAN_STATUS_TYPE_ENABLE;
	status.disablewan = false;

	status.roam_guard = ofono_connection_manager_get_roaming_allowed(od->cm);
	status.network_attached = ofono_connection_manager_get_attached(od->cm);
	status.network_type =
		convert_ofono_connection_bearer_to_wan_network_type(ofono_connection_manager_get_bearer(od->cm));

	for (iter = contexts; iter != NULL; iter = g_slist_next(iter)) {
		context = iter->data;

		ofono_connection_context_register_prop_changed_cb(context, context_prop_changed_cb, od);

		wanservice = g_new0(struct wan_connected_service, 1);

		switch (ofono_connection_context_get_type(context)) {
		case OFONO_CONNECTION_CONTEXT_TYPE_INTERNET:
			wanservice->services[WAN_SERVICE_TYPE_INTERNET] = true;
			break;
		case OFONO_CONNECTION_CONTEXT_TYPE_MMS:
			wanservice->services[WAN_SERVICE_TYPE_MMS] = true;
			break;
		default:
			break;
		}

		wanservice->ipaddress = ofono_connection_context_get_address(context);
		wanservice->connection_status = ofono_connection_context_get_active(context) ?
					WAN_CONNECTION_STATUS_ACTIVE : WAN_CONNECTION_STATUS_DISCONNECTED;

		/* FIXME to what we have to set the following field? */
		wanservice->req_status = WAN_REQUEST_STATUS_CONNECT_SUCCEEDED;

		/* If at least one service is active we report a active connection */
		if (wanservice->connection_status == WAN_CONNECTION_STATUS_ACTIVE)
			status.connection_status = WAN_CONNECTION_STATUS_ACTIVE;

		status.connected_services = g_slist_append(status.connected_services, wanservice);
	}

	cb(NULL, &status, cbd->data);

	g_slist_free_full(status.connected_services, g_free);
	g_free(cbd);
}

void ofono_wan_get_status(struct wan_service *service, wan_get_status_cb cb, void *data)
{
	struct ofono_wan_data *od = wan_service_get_data(service);
	struct cb_data *cbd = NULL;

	cbd = cb_data_new(cb, data);
	cbd->user = od;

	/* we need to retrieve all available connection contexts first */
	ofono_connection_manager_get_contexts(od->cm, get_contexts_cb, cbd);
}

static void set_roaming_allowed_cb(struct ofono_error *error, void *data)
{
	struct cb_data *cbd = data;
	wan_result_cb cb = cbd->cb;
	struct wan_error werr;

	if (!error) {
		werr.code = WAN_ERROR_INTERNAL;
		cb(&werr, cbd->data);
		goto cleanup;
	}

	cb(NULL, cbd->data);

cleanup:
	g_free(cbd);
}

void ofono_wan_set_configuration(struct wan_service *service, struct wan_configuration *configuration,
									   wan_result_cb cb, void *data)
{
	struct ofono_wan_data *od = wan_service_get_data(service);
	struct cb_data *cbd = NULL;

	if (configuration->roamguard && !ofono_connection_manager_get_roaming_allowed(od->cm)) {
		cb(NULL, data);
		return;
	}

	cbd = cb_data_new(cb, data);
	ofono_connection_manager_set_roaming_allowed(od->cm, !configuration->roamguard,
												 set_roaming_allowed_cb, cbd);
}

static void get_status_cb(const struct wan_error *error, struct wan_status *status, void *data)
{
	struct ofono_wan_data *od = data;

	od->status_update_pending = false;

	if (!error) {
		wan_service_status_changed_notify(od->service, status);
	}
}

static void send_status_update_cb(void *data)
{
	struct ofono_wan_data *od = data;

	if (!od->status_update_pending) {
		od->status_update_pending = true;
		ofono_wan_get_status(od->service, get_status_cb, od);
	}
}

static void manager_property_changed_cb(const gchar *name, void *data)
{
	send_status_update_cb(data);
}

static void context_prop_changed_cb(const char *name, void *data)
{
	send_status_update_cb(data);
}

static void modem_prop_changed_cb(const gchar *name, void *data)
{
	struct ofono_wan_data *od = data;
	const char *path = ofono_modem_get_path(od->modem);

	if (g_str_equal(name, "Interfaces")) {
		if (!od->cm && ofono_modem_is_interface_supported(od->modem, OFONO_MODEM_INTERFACE_CONNECTION_MANAGER)) {
			od->cm = ofono_connection_manager_create(path);
			ofono_connection_manager_register_prop_changed_cb(od->cm, manager_property_changed_cb, od);
			ofono_connection_manager_register_contexts_changed_cb(od->cm, send_status_update_cb, od);
		}
		else if (od->cm && !ofono_modem_is_interface_supported(od->modem, OFONO_MODEM_INTERFACE_CONNECTION_MANAGER)) {
			ofono_connection_manager_free(od->cm);
			od->cm = NULL;
		}
	}
}

static void modems_changed_cb(gpointer user_data)
{
	struct ofono_wan_data *data = user_data;
	const GList *modems = NULL;

	modems = ofono_manager_get_modems(data->manager);

	/* select first modem from the list as default for now */
	if (modems) {
		ofono_modem_ref(modems->data);
		data->modem = modems->data;

		ofono_modem_register_prop_changed_handler(data->modem, modem_prop_changed_cb, data);
	}
	else {
		if (data->modem)
			ofono_modem_unref(data->modem);

		data->modem = NULL;
	}
}

static void service_appeared_cb(GDBusConnection *conn, const gchar *name, const gchar *name_owner,
								gpointer user_data)
{
	struct ofono_wan_data *od = user_data;

	g_message("ofono dbus service available");

	if (od->manager)
		return;

	od->manager = ofono_manager_create();
	ofono_manager_set_modems_changed_callback(od->manager, modems_changed_cb, od);
}

static void service_vanished_cb(GDBusConnection *conn, const gchar *name, gpointer user_data)
{
	struct ofono_wan_data *od = user_data;

	g_message("ofono dbus service disappeared");

	free_used_instances(od);
}

int ofono_wan_probe(struct wan_service *service)
{
	struct ofono_wan_data *data;

	data = g_try_new0(struct ofono_wan_data, 1);
	if (!data)
		return -ENOMEM;

	wan_service_set_data(service, data);
	data->service = service;

	data->service_watch = g_bus_watch_name(G_BUS_TYPE_SYSTEM, "org.ofono", G_BUS_NAME_WATCHER_FLAGS_NONE,
					 service_appeared_cb, service_vanished_cb, data, NULL);

	return 0;
}

void ofono_wan_remove(struct wan_service *service)
{
	struct ofono_wan_data *data;

	data = wan_service_get_data(service);

	g_bus_unwatch_name(data->service_watch);

	free_used_instances(data);

	g_free(data);

	wan_service_set_data(service, NULL);
}

struct wan_driver ofono_wan_driver = {
	.probe =		ofono_wan_probe,
	.remove =		ofono_wan_remove,
	.get_status = 		ofono_wan_get_status,
	.set_configuration = 		ofono_wan_set_configuration,
};

// vim:ts=4:sw=4:noexpandtab
