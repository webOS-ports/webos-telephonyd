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
#include "ofononetworkregistration.h"

#define is_flag_set(flags, flag) \
	((flags & flag) == flag)

struct ofono_wan_data {
	struct wan_service *service;
	guint service_watch;
	struct ofono_manager *manager;
	struct ofono_modem *modem;
	struct ofono_connection_manager *cm;
	struct ofono_network_registration *netreg;
	bool status_update_pending;
	struct wan_configuration *pending_configuration;
	guint connman_watch;
	GDBusProxy *connman_manager_proxy;
	gboolean wan_disabled;
	gboolean pending_wan_disabled;
	gchar *current_service_path;
	guint current_service_watch;
	GDBusProxy *current_service_proxy;
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

enum wan_network_type convert_ofono_network_technology_to_wan_network_type(enum ofono_network_technology technology)
{
	switch (technology) {
	case OFONO_NETWORK_TECHNOLOGOY_GSM:
		return WAN_NETWORK_TYPE_GPRS;
	case OFONO_NETWORK_TECHNOLOGOY_EDGE:
		return WAN_NETWORK_TYPE_EDGE;
	case OFONO_NETWORK_TECHNOLOGOY_UMTS:
		return WAN_NETWORK_TYPE_UMTS;
	case OFONO_NETWORK_TECHNOLOGOY_HSPA:
	case OFONO_NETWORK_TECHNOLOGOY_LTE:
		return WAN_NETWORK_TYPE_HSDPA;
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

static void current_service_autoconnect_set_cb(GObject *source, GAsyncResult *res, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_wan_data *od = cbd->user;
	wan_result_cb cb = cbd->cb;
	struct wan_error werror;
	GError *error = 0;
	GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);

	GVariant *result = g_dbus_connection_call_finish(conn, res, &error);
	if (error) {
		g_warning("Failed to set auto connect field for cellular service %s: %s",
				  od->current_service_path, error->message);
		g_error_free(error);

		werror.code = WAN_ERROR_FAILED;
		cb(&werror, cbd->data);

		goto cleanup;
	}

	g_variant_unref(result);

	cb(NULL, cbd->data);

cleanup:
	g_free(cbd);
}

static void current_service_enabled_cb(GObject *source, GAsyncResult *res, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_wan_data *od = cbd->user;
	wan_result_cb cb = cbd->cb;
	struct wan_error werror;
	GError *error = 0;
	GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);

	GVariant *result = g_dbus_connection_call_finish(conn, res, &error);
	if (error) {
		g_warning("Failed to %s current cellular service %s: %s",
				  (!od->pending_configuration || !od->pending_configuration->disablewan) ? "enable" : "disable",
				  od->current_service_path, error->message);
		g_error_free(error);

		if(cb) {
			werror.code = WAN_ERROR_FAILED;
			cb(&werror, cbd->data);
		}

		g_free(cbd);

		return;
	}

	g_variant_unref(result);

	g_dbus_connection_call(conn, "net.connman", od->current_service_path,
						   "net.connman.Service", "SetProperty",
						   g_variant_new("(sv)", "AutoConnect", g_variant_new_boolean(!od->pending_configuration->disablewan)), NULL,
						   G_DBUS_CALL_FLAGS_NONE, -1, NULL,
						   current_service_autoconnect_set_cb, od);
}

static void switch_current_service_state(struct ofono_wan_data *od, bool enable,
										 wan_result_cb cb, void *data)
{
	GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);

	g_message("enable %d", enable);

	g_dbus_connection_call(conn, "net.connman", od->current_service_path,
						   "net.connman.Service", enable ? "Connect" : "Disconnect", NULL, NULL,
						   G_DBUS_CALL_FLAGS_NONE, -1, NULL,
						   (GAsyncReadyCallback) current_service_enabled_cb, data);
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
	status.disablewan = od->wan_disabled;

	status.roam_guard = !ofono_connection_manager_get_roaming_allowed(od->cm);
	status.network_attached = ofono_connection_manager_get_attached(od->cm);

	enum ofono_connection_bearer bearer = ofono_connection_manager_get_bearer(od->cm);
	if (bearer == OFONO_CONNECTION_BEARER_UNKNOWN && od->netreg) {
		enum ofono_network_technology tech = ofono_network_registration_get_technology(od->netreg);
		status.network_type = convert_ofono_network_technology_to_wan_network_type(tech);
	}
	else {
		status.network_type = convert_ofono_connection_bearer_to_wan_network_type(bearer);
	}

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

static void roamguard_set_cb(const struct wan_error* error, void *data)
{
	struct cb_data *cbd = data;
	struct ofono_wan_data *od = cbd->user;
	wan_result_cb cb = cbd->cb;

	if (error) {
		cb(error, cbd->data);
		goto cleanup;
	}

	od->pending_configuration = NULL;

	cb(NULL, cbd->data);

cleanup:
	g_free(cbd);
}

static void disablewan_set_cb(struct ofono_error *error, void *data)
{
	struct cb_data *cbd = data;
	struct ofono_wan_data *od = cbd->user;
	wan_result_cb cb = cbd->cb;
	struct wan_error werror;

	if (error) {
		werror.code = WAN_ERROR_FAILED;
		cb(&werror, cbd->data);
		goto cleanup;
	}

	if (is_flag_set(od->pending_configuration->flags, WAN_CONFIGURATION_TYPE_ROAMGUARD)) {
		if (od->pending_configuration->roamguard == ofono_connection_manager_get_roaming_allowed(od->cm)) {
			ofono_connection_manager_set_roaming_allowed(od->cm, !od->pending_configuration->roamguard,
														 roamguard_set_cb, cbd);
			return;
		}
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
	struct wan_error error;

	if (!od->current_service_path) {
		error.code = WAN_ERROR_NOT_AVAILABLE;
		cb(&error, data);
		return;
	}

	od->pending_configuration = configuration;

	if (is_flag_set(configuration->flags, WAN_CONFIGURATION_TYPE_DISABLEWAN)) {
		if (configuration->disablewan != od->wan_disabled) {
			cbd = cb_data_new(cb, data);
			cbd->user = od;

			switch_current_service_state(od, !configuration->disablewan,
										 disablewan_set_cb, cbd);
		}
	}
	else if (is_flag_set(configuration->flags, WAN_CONFIGURATION_TYPE_ROAMGUARD)) {
		if (configuration->roamguard == ofono_connection_manager_get_roaming_allowed(od->cm)) {
			cbd = cb_data_new(cb, data);
			cbd->user = od;

			ofono_connection_manager_set_roaming_allowed(od->cm, !configuration->roamguard,
														 roamguard_set_cb, cbd);
		}
	}
	else {
		cb (NULL, data);
	}
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

static void network_prop_changed_cb(const gchar *name, void *data)
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
		if (!od->netreg && ofono_modem_is_interface_supported(od->modem, OFONO_MODEM_INTERFACE_NETWORK_REGISTRATION)) {
			od->netreg = ofono_network_registration_create(path);
			ofono_network_registration_register_prop_changed_handler(od->netreg, network_prop_changed_cb, od);
		}
		else if (od->netreg && !ofono_modem_is_interface_supported(od->modem, OFONO_MODEM_INTERFACE_NETWORK_REGISTRATION)) {
			ofono_network_registration_free(od->netreg);
			od->netreg = NULL;
			send_status_update_cb(od);
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

static void handle_current_service_property(struct ofono_wan_data *od, const gchar *name, GVariant *value)
{
	GVariant *state_v = 0;
	const gchar *state = 0;
	gboolean wan_disable_before = od->wan_disabled;

	if (g_strcmp0(name, "State") == 0) {
		state_v = g_variant_get_variant(value);
		state = g_variant_get_string(state_v, NULL);

		if (g_strcmp0(state, "ready") == 0 || g_strcmp0(state, "online") == 0)
			od->wan_disabled = FALSE;
		else
			od->wan_disabled = TRUE;

		g_variant_unref(state_v);
	}

	if (od->wan_disabled != wan_disable_before)
		send_status_update_cb(od);
}

static void current_service_signal_cb(GDBusProxy *proxy, gchar *sender_name, gchar *signal_name,
									  GVariant *parameters, gpointer user_data)
{
	struct ofono_wan_data *od = user_data;
	GVariant *prop_name = 0, *prop_value = 0;
	const gchar *name = 0;

	if (g_strcmp0(signal_name, "PropertyChanged") != 0)
		return;

	prop_name = g_variant_get_child_value(parameters, 0);
	prop_value = g_variant_get_child_value(parameters, 1);
	name = g_variant_get_string(prop_name, NULL);

	handle_current_service_property(od, name, prop_value);

	g_variant_unref(prop_name);
	g_variant_unref(prop_value);
}

static void cellular_service_setup_cb(const struct wan_error* error, void *data)
{
	if (error) {
		g_message("[WAN] Failed to connect to cellular service");
		return;
	}

	g_message("[WAN] Successfully connected to celluar service the first time");
}

static void assign_current_cellular_service(struct ofono_wan_data *od, const gchar *path, GVariant *properties)
{
	GVariant *property = 0, *prop_name = 0, *prop_value = 0;
	gsize n = 0;
	const gchar *name = 0;
	bool favorite = false;

	if (!path) {
		if (od->current_service_watch)
			g_signal_handler_disconnect(od->current_service_proxy,
								od->current_service_watch);

		if (od->current_service_proxy)
			g_object_unref(od->current_service_proxy);


		od->current_service_path = NULL;
		od->current_service_proxy = 0;
		od->current_service_watch = 0;

		return;
	}

	od->current_service_path = g_strdup(path);

	od->current_service_proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM,
															  G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
															  NULL, "net.connman",
															  path, "net.connman.Service",
															  NULL, NULL);

	od->current_service_watch = g_signal_connect(od->current_service_proxy, "g-signal",
												 G_CALLBACK(current_service_signal_cb), od);

	for (n = 0; n < g_variant_n_children(properties); n++) {
		property = g_variant_get_child_value(properties, n);

		prop_name = g_variant_get_child_value(property, 0);
		prop_value = g_variant_get_child_value(property, 1);

		name = g_variant_get_string(prop_name, NULL);

		if (g_strcmp0(name, "Favorite") == 0) {
			favorite = g_variant_get_boolean(g_variant_get_variant(prop_value));

			if (!favorite) {
				g_message("[WAN] Found a not yet configured cellular service; connecting to it for the first time");

				struct cb_data *cbd = NULL;
				cbd = cb_data_new(NULL, NULL);
				cbd->user = od;
				switch_current_service_state(od, true, cellular_service_setup_cb, cbd);
			}
		}

		handle_current_service_property(od, name, prop_value);
	}
}

static void update_from_service_list(struct ofono_wan_data *od, GVariant *service_list)
{
	GVariant *service = 0, *object_path = 0, *properties = 0;
	const gchar *path = 0;
	gboolean found = FALSE;
	gsize n = 0;

	for (n = 0; n < g_variant_n_children(service_list); n++) {
		service = g_variant_get_child_value(service_list, n);
		object_path = g_variant_get_child_value(service, 0);
		properties = g_variant_get_child_value(service, 1);
		path = g_variant_get_string(object_path, NULL);

		if (g_strrstr(path, "cellular") != 0) {
			g_message("[WAN] Found a cellular service %s", path);
			/* FIXME we assume here that we only have one cellular service. When
			 * we come to the point where we support multi-sim modems we have to
			 * revisit this decision */
			assign_current_cellular_service(od, path, properties);
			found = TRUE;
		}

		g_variant_unref(object_path);
		g_variant_unref(service);

		if (found)
			break;
	}

	if (!found) {
		g_message("[WAN] Didn't find a cellular service");
		assign_current_cellular_service(od, NULL, NULL);
	}
}

static void update_from_changed_services(struct ofono_wan_data *od, GVariant *parameters)
{
	GVariant *services_added = 0;
	GVariant *services_removed = 0;
	gsize n = 0;
	gboolean done = FALSE;

	services_added = g_variant_get_child_value(parameters, 0);
	services_removed = g_variant_get_child_value(parameters, 1);

	if (!od->current_service_path)
		update_from_service_list(od, services_added);

	if (!od->current_service_path)
		return;

	for (n = 0; n < g_variant_n_children(services_removed); n++) {
		GVariant *service = g_variant_get_child_value(services_removed, n);
		const gchar *path = g_variant_get_string(service, NULL);

		if (g_strcmp0(od->current_service_path, path) == 0) {
			g_warning("[WAN] Current cellular service %s disappeared", path);

			g_object_unref(od->current_service_proxy);
			od->current_service_proxy = 0;

			g_source_remove(od->current_service_watch);
			od->current_service_watch = 0;

			g_free(od->current_service_path);
			od->current_service_path = 0;

			done = TRUE;
		}

		g_variant_unref(service);

		if (done)
			break;
	}
}

static void services_changed_cb(GDBusProxy *proxy, gchar *sender_name, gchar *signal_name,
									  GVariant *parameters, gpointer user_data)
{
	struct ofono_wan_data *od = user_data;

	if (g_strcmp0(signal_name, "ServicesChanged") != 0)
		return;

	update_from_changed_services(od, parameters);
}

static void connman_manager_get_services_cb(GObject *source, GAsyncResult *res, gpointer user_data)
{
	struct ofono_wan_data *od = user_data;
	GError *error = 0;
	GVariant *service_list = 0, *response = 0;

	response = g_dbus_proxy_call_finish(od->connman_manager_proxy, res, &error);
	if (error) {
		g_warning("Failed to create proxy for connman manager service: %s", error->message);
		g_error_free(error);
		return;
	}

	service_list = g_variant_get_child_value(response, 0);

	g_message("[WAN] got %d services from connman", g_variant_n_children(service_list));

	update_from_service_list(od, service_list);
}

static void connman_manager_proxy_connect_cb(GObject *source, GAsyncResult *res, gpointer user_data)
{
	struct ofono_wan_data *od = user_data;
	GError *error = 0;

	od->connman_manager_proxy = g_dbus_proxy_new_finish(res, &error);
	if (error) {
		g_warning("Failed to create proxy for connman manager service: %s", error->message);
		g_error_free(error);
		return;
	}

	g_message("[WAN] successfully created proxy for connman manager");

	g_signal_connect(od->connman_manager_proxy, "g-signal", G_CALLBACK(services_changed_cb), od);

	g_dbus_proxy_call(od->connman_manager_proxy, "GetServices", NULL, G_DBUS_CALL_FLAGS_NONE,
					  -1, NULL, (GAsyncReadyCallback) connman_manager_get_services_cb, od);
}

static void connman_appeared_cb(GDBusConnection *conn, const gchar *name, const gchar *name_owner,
								gpointer user_data)
{
	struct ofono_wan_data *od = user_data;

	g_message("connman dbus service available");

	g_dbus_proxy_new_for_bus(G_BUS_TYPE_SYSTEM,
							 G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
							 NULL, "net.connman", "/", "net.connman.Manager",
							 NULL, (GAsyncReadyCallback) connman_manager_proxy_connect_cb,
							 od);
}

static void connman_vanished_cb(GDBusConnection *conn, const gchar *name, gpointer user_data)
{
	struct ofono_wan_data *od = user_data;

	g_message("connman dbus service disappeared");

	if (od->connman_manager_proxy) {
		g_object_unref(od->connman_manager_proxy);
		od->connman_manager_proxy = 0;
	}
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

	data->connman_watch = g_bus_watch_name(G_BUS_TYPE_SYSTEM, "net.connman", G_BUS_NAME_WATCHER_FLAGS_NONE,
					connman_appeared_cb, connman_vanished_cb, data, NULL);

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
