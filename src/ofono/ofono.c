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
#include "ofonomanager.h"
#include "ofonomodem.h"
#include "ofonosimmanager.h"
#include "utils.h"

struct ofono_data {
	struct telephony_service *service;
	struct ofono_manager *manager;
	struct ofono_modem *modem;
	struct ofono_sim_manager *sim;
	enum telephony_sim_status sim_status;
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

	/* When the modem was set to be unpowered we don't have to set it offline */
	powered = ofono_modem_get_powered(od->modem);
	if (powered) {
		ofono_modem_set_online(od->modem, powered, set_online_cb, cbd);
	}
	else {
		cb(NULL, cbd->data);
		g_free(cbd);
	}
}

int ofono_power_set(struct telephony_service *service, bool power, telephony_result_cb cb, void *data)
{
	struct cb_data *cbd = cb_data_new(cb, data);
	struct ofono_data *od = telephony_service_get_data(service);

	cbd->user = od;

	ofono_modem_set_powered(od->modem, power, set_powered_cb, cbd);

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

static void sim_status_changed_cb(void *data)
{
	struct ofono_data *od = data;
	enum telephony_sim_status sim_status = TELEPHONY_SIM_STATUS_SIM_INVALID;

	sim_status = determine_sim_status(od);

	if (sim_status != od->sim_status) {
		od->sim_status = sim_status;
		telephony_service_sim_status_notify(od->service, sim_status);
	}
}

static struct ofono_sim_manager* create_sim_manager(struct ofono_data *od)
{
	od->sim = ofono_sim_manager_create(ofono_modem_get_path(od->modem));
	ofono_sim_manager_register_status_changed_handler(od->sim, sim_status_changed_cb, od);
}

int ofono_sim_status_query(struct telephony_service *service, telephony_sim_status_query_cb cb, void *data)
{
	struct ofono_data *od = telephony_service_get_data(service);

	if (!od->sim && ofono_modem_is_interface_supported(od->modem, OFONO_MODEM_INTERFACE_SIM_MANAGER))
		create_sim_manager(od);

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

	if (!od->sim && ofono_modem_is_interface_supported(od->modem, OFONO_MODEM_INTERFACE_SIM_MANAGER))
		create_sim_manager(od);

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

static void modem_powered_changed_cb(bool powered, void *data)
{
	struct ofono_data *od = data;

	telephony_service_power_status_notify(od->service, powered);
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

		ofono_modem_set_powered_changed_handler(data->modem, modem_powered_changed_cb, data);

		telephony_service_availability_changed_notify(data->service, true);
	}
	else {
		if (data->modem)
			ofono_modem_unref(data->modem);

		data->modem = NULL;

		telephony_service_availability_changed_notify(data->service, false);
	}
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

	data->manager = ofono_manager_create();
	ofono_manager_set_modems_changed_callback(data->manager, modems_changed_cb, data);

	return 0;
}

void ofono_remove(struct telephony_service *service)
{
	struct ofono_data *data;

	data = telephony_service_get_data(service);

	if (data->sim)
		ofono_sim_manager_free(data->sim);

	ofono_manager_free(data->manager);

	g_free(data);

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
};

void ofono_init(struct telephony_service *service)
{
	telephony_service_register_driver(service, &driver);
}

void ofono_exit(struct telephony_service *service)
{
}

// vim:ts=4:sw=4:noexpandtab
