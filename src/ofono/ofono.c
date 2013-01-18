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

#include "telephonyservice.h"
#include "telephonydriver.h"
#include "ofonobase.h"
#include "ofonomanager.h"
#include "ofonomodem.h"
#include "ofonosimmanager.h"
#include "ofononetworkregistration.h"
#include "utils.h"

struct ofono_data {
	struct telephony_service *service;
	struct ofono_manager *manager;
	struct ofono_modem *modem;
	struct ofono_sim_manager *sim;
	struct ofono_network_registration *netreg;
	enum telephony_sim_status sim_status;
	bool initializing;
	guint service_watch;
};

void set_online_cb(gboolean result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_data *od = cbd->user;
	telephony_result_cb cb = cbd->cb;
	struct telephony_error error;

	if (!result) {
		error.code = 1; /* FIXME */
		cb(&error, cbd->data);
		goto cleanup;
	}

	cb(NULL, cbd->data);

cleanup:
	g_free(cbd);
}

void set_powered_cb(gboolean result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_data *od = cbd->user;
	telephony_result_cb cb = cbd->cb;
	struct telephony_error error;
	bool powered;

	if (!result) {
		error.code = 1; /* FIXME */
		cb(&error, cbd->data);
		g_free(cbd);
		return;
	}

	ofono_modem_set_online(od->modem, powered, set_online_cb, cbd);
}

int ofono_power_set(struct telephony_service *service, bool power, telephony_result_cb cb, void *data)
{
	struct cb_data *cbd = cb_data_new(cb, data);
	struct ofono_data *od = telephony_service_get_data(service);
	bool powered = false;

	cbd->user = od;

	powered = ofono_modem_get_powered(od->modem);
	if (!powered) {
		ofono_modem_set_powered(od->modem, power, set_powered_cb, cbd);
	}
	else {
		ofono_modem_set_online(od->modem, power, set_online_cb, cbd);
	}

	return 0;
}

int ofono_power_query(struct telephony_service *service, telephony_power_query_cb cb, void *data)
{
	bool powered = false;
	struct ofono_data *od = telephony_service_get_data(service);

	if (!od->modem)
		return -EINVAL;

	powered = ofono_modem_get_powered(od->modem) &&
			  ofono_modem_get_online(od->modem);

	cb(NULL, powered, data);

	return 0;
}

int ofono_platform_query(struct telephony_service *service, telephony_platform_query_cb cb, void *data)
{
	struct ofono_data *od = telephony_service_get_data(service);
	struct telephony_platform_info pinfo;

	if (!od->modem)
		return -EINVAL;

	memset(&pinfo, 0, sizeof(struct telephony_platform_info));
	pinfo.platform_type = TELEPHONY_PLATFORM_TYPE_GSM;
	pinfo.imei = ofono_modem_get_serial(od->modem);
	pinfo.version = ofono_modem_get_revision(od->modem);
	/* FIXME set mcc/mnc */

	cb(NULL, &pinfo, data);

	return 0;
}

static enum telephony_sim_status determine_sim_status(struct ofono_data *od)
{
	enum telephony_sim_status sim_status = TELEPHONY_SIM_STATUS_SIM_INVALID;

	if (od->sim && ofono_sim_manager_get_present(od->sim)) {
		enum ofono_sim_pin pin_type = ofono_sim_manager_get_pin_required(od->sim);

		if (pin_type == OFONO_SIM_PIN_TYPE_NONE)
			sim_status = TELEPHONY_SIM_STATUS_SIM_READY;
		else if (pin_type == OFONO_SIM_PIN_TYPE_PIN)
			sim_status = TELEPHONY_SIM_STATUS_PIN_REQUIRED;
		else if (pin_type == OFONO_SIM_PIN_TYPE_PUK)
			sim_status = TELEPHONY_SIM_STATUS_PUK_REQUIRED;

		/* FIXME maybe we have to take the lock status of the pin/puk into account here */
	}

	return sim_status;
}

static void sim_prop_changed_cb(const gchar *name, void *data)
{
	struct ofono_data *od = data;
	enum telephony_sim_status sim_status = TELEPHONY_SIM_STATUS_SIM_INVALID;

	sim_status = determine_sim_status(od);

	if (sim_status != od->sim_status) {
		od->sim_status = sim_status;
		telephony_service_sim_status_notify(od->service, sim_status);
	}
}

int ofono_sim_status_query(struct telephony_service *service, telephony_sim_status_query_cb cb, void *data)
{
	struct ofono_data *od = telephony_service_get_data(service);
	struct telephony_error err;

	if (!ofono_modem_is_interface_supported(od->modem, OFONO_MODEM_INTERFACE_SIM_MANAGER)) {
		err.code = TELEPHONY_ERROR_NOT_IMPLEMENTED;
		cb(&err, TELEPHONY_SIM_STATUS_SIM_INVALID, data);
		return;
	}

	od->sim_status = determine_sim_status(od);
	cb(NULL, od->sim_status, data);

	return 0;
}

int ofono_pin1_status_query(struct telephony_service *service, telephony_pin_status_query_cb cb, void *data)
{
	struct ofono_data *od = telephony_service_get_data(service);
	struct telephony_pin_status pin_status;
	struct telephony_error err;
	enum ofono_sim_pin pin_required;

	if (!ofono_modem_is_interface_supported(od->modem, OFONO_MODEM_INTERFACE_SIM_MANAGER)) {
		err.code = TELEPHONY_ERROR_NOT_IMPLEMENTED;
		cb(&err, TELEPHONY_SIM_STATUS_SIM_INVALID, data);
		return;
	}

	memset(&pin_status, 0, sizeof(pin_status));

	if (od->sim && ofono_sim_manager_get_present(od->sim)) {
		pin_required = ofono_sim_manager_get_pin_required(od->sim);
		if (pin_required == OFONO_SIM_PIN_TYPE_PIN)
			pin_status.required = true;
		else if (pin_required == OFONO_SIM_PIN_TYPE_PUK)
			pin_status.puk_required = true;

		/* FIXME how can we map device_locked and perm_blocked to the ofono bits ? */

		pin_status.pin_attempts_remaining = ofono_sim_manager_get_pin_retries(od->sim, OFONO_SIM_PIN_TYPE_PIN);
		pin_status.puk_attempts_remaining = ofono_sim_manager_get_pin_retries(od->sim, OFONO_SIM_PIN_TYPE_PUK);

		cb(NULL, &pin_status, data);
	}
	else {
		/* No SIM available return error */
		err.code = 1;
		cb(&err, NULL, data);
	}

	return 0;
}

static int retrieve_network_status(struct ofono_data *od, struct telephony_network_status *status)
{
	enum ofono_network_status net_status;

	net_status = ofono_network_registration_get_status(od->netreg);
	switch (net_status) {
	case OFONO_NETWORK_REGISTRATION_STATUS_REGISTERED:
		status->state = TELEPHONY_NETWORK_STATE_SERVICE;
		status->registration = TELEPHONY_NETWORK_REGISTRATION_HOME;
		break;
	case OFONO_NETWORK_REGISTRATION_STATUS_UNREGISTERED:
	case OFONO_NETWORK_REGISTRATION_STATUS_SEARCHING:
		status->state = TELEPHONY_NETWORK_STATE_NO_SERVICE;
		status->registration = TELEPHONY_NETWORK_REGISTRATION_SEARCHING;
		break;
	case OFONO_NETWORK_REGISTRATION_STATUS_DENIED:
		status->state = TELEPHONY_NETWORK_STATE_NO_SERVICE;
		status->registration = TELEPHONY_NETWORK_REGISTRATION_DENIED;
		break;
	case OFONO_NETWORK_REGISTRATION_STATUS_ROAMING:
		status->state = TELEPHONY_NETWORK_STATE_SERVICE;
		status->registration = TELEPHONY_NETWORK_REGISTRATION_ROAM;
		break;
	default:
		break;
	}

	status->name = ofono_network_registration_get_operator_name(od->netreg);
	/* FIXME when we support the relevant ofono interfaces set this correctly */
	status->cause_code = 0;
	status->data_registered = false;

	return 0;
}

int ofono_network_status_query(struct telephony_service *service, telephony_network_status_query_cb cb, void *data)
{
	struct ofono_data *od = telephony_service_get_data(service);
	struct telephony_network_status status;
	struct telephony_error err;

	if (od->netreg) {
		if (retrieve_network_status(od, &status) < 0) {
			err.code = TELEPHONY_ERROR_INTERNAL;
			cb(&err, NULL, data);
			return 0;
		}
	}
	else {
		status.state = TELEPHONY_NETWORK_STATE_NO_SERVICE;
		status.registration = TELEPHONY_NETWORK_REGISTRATION_NO_SERVICE;
		status.name = NULL;
		status.data_registered = false;
	}

	cb(NULL, &status, data);

	return 0;
}

static int convert_strength_to_bars(int rssi)
{
	return (rssi * 5) / 100;
}

int ofono_signal_strength_query(struct telephony_service *service, telephony_signal_strength_query_cb cb, void *data)
{
	struct ofono_data *od = telephony_service_get_data(service);
	struct telephony_error err;
	unsigned int strength = 0;

	if (od->netreg)
		strength = convert_strength_to_bars(ofono_network_registration_get_strength(od->netreg));

	cb(NULL, strength, data);

	return 0;
}

static void network_prop_changed_cb(const gchar *name, void *data)
{
	struct ofono_data *od = data;
	struct telephony_network_status net_status;
	int strength;

	if (g_str_equal(name, "Status") || g_str_equal(name, "Name")) {
		if (retrieve_network_status(od, &net_status) < 0)
			return 0;

		telephony_service_network_status_changed_notify(od->service, &net_status);
	}
	else if (g_str_equal(name, "Strength")) {
		strength = ofono_network_registration_get_strength(od->netreg);
		telephony_service_signal_strength_changed_notify(od->service, convert_strength_to_bars(strength));
	}
}

static void modem_prop_changed_cb(const gchar *name, void *data)
{
	struct ofono_data *od = data;
	bool powered = false, online = false;
	gchar *path = ofono_modem_get_path(od->modem);

	if (g_str_equal(name, "Online")) {
		online = ofono_modem_get_online(od->modem);
		telephony_service_power_status_notify(od->service, online);
	}
	else if (g_str_equal(name, "Interfaces")) {
		if (!od->sim && ofono_modem_is_interface_supported(od->modem, OFONO_MODEM_INTERFACE_SIM_MANAGER)) {
			od->sim = ofono_sim_manager_create(path);
			ofono_sim_manager_register_prop_changed_handler(od->sim, sim_prop_changed_cb, od);
		}
		else if (od->sim && !ofono_modem_is_interface_supported(od->modem, OFONO_MODEM_INTERFACE_SIM_MANAGER)) {
			ofono_sim_manager_free(od->sim);
			od->sim = NULL;
		}

		if (!od->netreg && ofono_modem_is_interface_supported(od->modem, OFONO_MODEM_INTERFACE_NETWORK_REGISTRATION)) {
			od->netreg = ofono_network_registration_create(path);
			ofono_network_registration_register_prop_changed_handler(od->netreg, network_prop_changed_cb, od);
		}
		else if (od->netreg && !ofono_modem_is_interface_supported(od->modem, OFONO_MODEM_INTERFACE_NETWORK_REGISTRATION)) {
			ofono_network_registration_free(od->netreg);
			od->netreg = NULL;
		}
	}
	else if (g_str_equal(name, "Powered")) {
		powered = ofono_modem_get_powered(od->modem);
		if (od->initializing && powered) {
			telephony_service_availability_changed_notify(od->service, true);
			od->initializing = false;
		}
	}
}

static void modems_changed_cb(gpointer user_data)
{
	struct ofono_data *data = user_data;
	const GList *modems = NULL;

	modems = ofono_manager_get_modems(data->manager);

	/* select first modem from the list as default for now */
	if (modems) {
		ofono_modem_ref(modems->data);
		data->modem = modems->data;

		data->initializing = true;

		ofono_modem_register_prop_changed_handler(data->modem, modem_prop_changed_cb, data);
	}
	else {
		if (data->sim)
			ofono_sim_manager_free(data->sim);

		if (data->modem)
			ofono_modem_unref(data->modem);

		data->sim = NULL;
		data->modem = NULL;

		telephony_service_availability_changed_notify(data->service, false);
	}
}

static void free_used_instances(struct ofono_data *od)
{
	if (od->sim) {
		ofono_sim_manager_free(od->sim);
		od->sim = NULL;
	}

	if (od->manager) {
		ofono_manager_free(od->manager);
		od->manager = NULL;
	}
}

static void service_appeared_cb(GDBusConnection *conn, const gchar *name, const gchar *name_owner,
								gpointer user_data)
{
	struct ofono_data *od = user_data;

	g_message("ofono dbus service available");

	if (od->manager)
		return;

	od->manager = ofono_manager_create();
	ofono_manager_set_modems_changed_callback(od->manager, modems_changed_cb, od);
}

static void service_vanished_cb(GDBusConnection *conn, const gchar *name, gpointer user_data)
{
	struct ofono_data *od = user_data;

	g_message("ofono dbus service disappeared");

	free_used_instances(od);
}

int ofono_probe(struct telephony_service *service)
{
	struct ofono_data *data;

	data = g_try_new0(struct ofono_data, 1);
	if (!data)
		return -ENOMEM;

	telephony_service_set_data(service, data);
	data->service = service;

	data->sim_status = TELEPHONY_SIM_STATUS_SIM_INVALID;
	data->initializing = false;

	data->service_watch = g_bus_watch_name(G_BUS_TYPE_SYSTEM, "org.ofono", G_BUS_NAME_WATCHER_FLAGS_NONE,
					 service_appeared_cb, service_vanished_cb, data, NULL);

	return 0;
}

void ofono_remove(struct telephony_service *service)
{
	struct ofono_data *data;

	data = telephony_service_get_data(service);

	free_used_instances(data);
	g_free(data);

	g_bus_unwatch_name(data->service_watch);

	telephony_service_set_data(service, NULL);
}

struct telephony_driver driver = {
	.probe =		ofono_probe,
	.remove =		ofono_remove,
	.platform_query		= ofono_platform_query,
	.power_set =	ofono_power_set,
	.power_query =	ofono_power_query,
	.sim_status_query = ofono_sim_status_query,
	.pin1_status_query = ofono_pin1_status_query,
	.network_status_query = ofono_network_status_query,
	.signal_strength_query = ofono_signal_strength_query,
};

void ofono_init(struct telephony_service *service)
{
	telephony_service_register_driver(service, &driver);
}

void ofono_exit(struct telephony_service *service)
{
}

// vim:ts=4:sw=4:noexpandtab
