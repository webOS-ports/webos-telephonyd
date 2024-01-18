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

#include "telephonyservice.h"
#include "telephonydriver.h"
#include "ofonobase.h"
#include "ofonomanager.h"
#include "ofonomodem.h"
#include "ofonosimmanager.h"
#include "ofononetworkregistration.h"
#include "ofononetworkoperator.h"
#include "ofonoradiosettings.h"
#include "ofonovoicecallmanager.h"
#include "ofonovoicecall.h"
#include "ofonomessagemanager.h"
#include "ofonomessage.h"
#include "ofonomessagewatch.h"
#include "utils.h"

struct ofono_data {
	struct telephony_service *service;
	struct ofono_manager *manager;
	struct ofono_modem *modem;
	struct ofono_sim_manager *sim;
	struct ofono_network_registration *netreg;
	struct ofono_radio_settings *rs;
	struct ofono_voicecall_manager *vm;
	struct ofono_message_manager *mm;
	enum telephony_sim_status sim_status;
	bool initializing;
	bool power_set_pending;
	bool power_target;
	guint service_watch;
	GCancellable *network_scan_cancellable;
	GHashTable *calls;
	unsigned int next_call_id;
};

struct call_info {
	int id;
	struct ofono_voicecall *call;
};

void set_online_cb(struct ofono_error *error, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_data *od = cbd->user;
	telephony_result_cb cb = cbd->cb;
	struct telephony_error terr;

	if (error) {
		terr.code = 1; /* FIXME */
		cb(&terr, cbd->data);
		goto cleanup;
	}

	cb(NULL, cbd->data);
	od->power_set_pending = false;

cleanup:
	g_free(cbd);
}

void set_powered_cb(struct ofono_error *error, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_data *od = cbd->user;
	telephony_result_cb cb = cbd->cb;
	struct telephony_error terr;

	if (error) {
		terr.code = 1; /* FIXME */
		cb(&terr, cbd->data);
		goto cleanup;
	}

	if (od->power_target) {
		ofono_modem_set_online(od->modem, od->power_target, set_online_cb, cbd);
	}
	else {
		od->power_set_pending = false;
		cb(NULL, cbd->data);
		goto cleanup;
	}

	return;

cleanup:
	g_free(cbd);
}

void ofono_power_set(struct telephony_service *service, bool power, telephony_result_cb cb, void *data)
{
	struct cb_data *cbd = cb_data_new(cb, data);
	struct ofono_data *od = telephony_service_get_data(service);
	bool powered = false;
	struct telephony_error error;

	if (od->power_set_pending) {
		error.code = TELEPHONY_ERROR_ALREADY_INPROGRESS;
		cb(&error, data);
		return;
	}

	od->power_set_pending = true;
	od->power_target = power;

	cbd->user = od;

	powered = ofono_modem_get_powered(od->modem);
	if (!powered) {
		ofono_modem_set_powered(od->modem, power, set_powered_cb, cbd);
	}
	else {
		ofono_modem_set_online(od->modem, power, set_online_cb, cbd);
	}
}

void ofono_power_query(struct telephony_service *service, telephony_power_query_cb cb, void *data)
{
	bool powered = false;
	struct ofono_data *od = telephony_service_get_data(service);
	struct telephony_error error;

	if (!od->modem) {
		error.code = TELEPHONY_ERROR_INTERNAL;
		cb(&error, false, data);
		return;
	}

	powered = ofono_modem_get_powered(od->modem) &&
			  ofono_modem_get_online(od->modem);

	cb(NULL, powered, data);
}

void ofono_platform_query(struct telephony_service *service, telephony_platform_query_cb cb, void *data)
{
	struct ofono_data *od = telephony_service_get_data(service);
	struct telephony_platform_info pinfo;
	const char *mnc = NULL;
	const char *mcc = NULL;
	struct telephony_error error;

	if (!od->modem) {
		error.code = TELEPHONY_ERROR_INTERNAL;
		cb(&error, NULL, data);
		return;
	}

	memset(&pinfo, 0, sizeof(struct telephony_platform_info));
	pinfo.platform_type = TELEPHONY_PLATFORM_TYPE_GSM;
	pinfo.imei = ofono_modem_get_serial(od->modem);
	pinfo.version = ofono_modem_get_revision(od->modem);

	if (ofono_modem_is_interface_supported(od->modem, OFONO_MODEM_INTERFACE_SIM_MANAGER)) {
		mcc = ofono_sim_manager_get_mcc(od->sim);
		mnc = ofono_sim_manager_get_mnc(od->sim);

		if (mcc && mnc) {
			pinfo.mcc = g_ascii_strtoll(mcc, NULL, 0);
			pinfo.mnc = g_ascii_strtoll(mnc, NULL, 0);
		}
	}

	cb(NULL, &pinfo, data);
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

static void determine_pin_status(struct ofono_data *od, struct telephony_pin_status *pin_status,
								  enum ofono_sim_pin pin_type, enum ofono_sim_pin puk_type)
{
	enum ofono_sim_pin pin_required;

	pin_required = ofono_sim_manager_get_pin_required(od->sim);
	if (pin_required == pin_type)
		pin_status->required = true;
	else if (pin_required == puk_type)
		pin_status->puk_required = true;

	pin_status->enabled = ofono_sim_manager_is_pin_locked(od->sim, pin_type);

	/* FIXME how can we map device_locked and perm_blocked to the ofono bits ? */

	pin_status->pin_attempts_remaining = ofono_sim_manager_get_pin_retries(od->sim, pin_type);
	pin_status->puk_attempts_remaining = ofono_sim_manager_get_pin_retries(od->sim, puk_type);
}

static void sim_prop_changed_cb(const gchar *name, void *data)
{
	struct ofono_data *od = data;
	enum telephony_sim_status sim_status = TELEPHONY_SIM_STATUS_SIM_INVALID;
	struct telephony_pin_status pin_status;

	sim_status = determine_sim_status(od);

	if (sim_status != od->sim_status) {
		od->sim_status = sim_status;
		telephony_service_sim_status_notify(od->service, sim_status);

		determine_pin_status(od, &pin_status, OFONO_SIM_PIN_TYPE_PIN, OFONO_SIM_PIN_TYPE_PUK);
		telephony_service_pin1_status_changed_notify(od->service, &pin_status);
	}
}

void ofono_sim_status_query(struct telephony_service *service, telephony_sim_status_query_cb cb, void *data)
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
}

void ofono_pin1_status_query(struct telephony_service *service, telephony_pin_status_query_cb cb, void *data)
{
	struct ofono_data *od = telephony_service_get_data(service);
	struct telephony_pin_status pin_status;
	struct telephony_error err;

	if (!ofono_modem_is_interface_supported(od->modem, OFONO_MODEM_INTERFACE_SIM_MANAGER)) {
		err.code = TELEPHONY_ERROR_NOT_IMPLEMENTED;
		cb(&err, NULL, data);
		return;
	}

	memset(&pin_status, 0, sizeof(pin_status));

	if (od->sim && ofono_sim_manager_get_present(od->sim)) {
		determine_pin_status(od, &pin_status, OFONO_SIM_PIN_TYPE_PIN, OFONO_SIM_PIN_TYPE_PUK);
		cb(NULL, &pin_status, data);
	}
	else {
		/* No SIM available return error */
		err.code = 1;
		cb(&err, NULL, data);
	}
}

void ofono_pin2_status_query(struct telephony_service *service, telephony_pin_status_query_cb cb, void *data)
{
	struct ofono_data *od = telephony_service_get_data(service);
	struct telephony_pin_status pin_status;
	struct telephony_error err;

	if (!ofono_modem_is_interface_supported(od->modem, OFONO_MODEM_INTERFACE_SIM_MANAGER)) {
		err.code = TELEPHONY_ERROR_NOT_IMPLEMENTED;
		cb(&err, NULL, data);
		return;
	}

	memset(&pin_status, 0, sizeof(pin_status));

	if (od->sim && ofono_sim_manager_get_present(od->sim)) {
		determine_pin_status(od, &pin_status, OFONO_SIM_PIN_TYPE_PIN2, OFONO_SIM_PIN_TYPE_PUK2);
		cb(NULL, &pin_status, data);
	}
	else {
		/* No SIM available return error */
		err.code = 1;
		cb(&err, NULL, data);
	}
}

void pin1_common_cb(struct ofono_error *error, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	telephony_result_cb cb = cbd->cb;
	struct telephony_error terr;

	if (error) {
		terr.code = 1;
		cb(&terr, cbd->data);
	}
	else {
		cb(NULL, cbd->data);
	}

	g_free(cbd);
}

void ofono_pin1_verify(struct telephony_service *service, const gchar *pin, telephony_result_cb cb, void *data)
{
	struct ofono_data *od = telephony_service_get_data(service);
	struct telephony_error err;
	struct cb_data *cbd;

	if (!ofono_modem_is_interface_supported(od->modem, OFONO_MODEM_INTERFACE_SIM_MANAGER)) {
		err.code = TELEPHONY_ERROR_NOT_AVAILABLE;
		cb(&err, data);
		return;
	}

	cbd = cb_data_new(cb, data);

	ofono_sim_manager_enter_pin(od->sim, OFONO_SIM_PIN_TYPE_PIN, pin, pin1_common_cb, cbd);
}

void ofono_pin1_enable(struct telephony_service *service, const gchar *pin, telephony_result_cb cb, void *data)
{
	struct ofono_data *od = telephony_service_get_data(service);
	struct telephony_error err;
	struct cb_data *cbd;

	if (!ofono_modem_is_interface_supported(od->modem, OFONO_MODEM_INTERFACE_SIM_MANAGER)) {
		err.code = TELEPHONY_ERROR_NOT_AVAILABLE;
		cb(&err, data);
		return;
	}

	cbd = cb_data_new(cb, data);

	ofono_sim_manager_lock_pin(od->sim, OFONO_SIM_PIN_TYPE_PIN, pin, pin1_common_cb, cbd);
}

void ofono_pin1_disable(struct telephony_service *service, const gchar *pin, telephony_result_cb cb, void *data)
{
	struct ofono_data *od = telephony_service_get_data(service);
	struct telephony_error err;
	struct cb_data *cbd;

	if (!ofono_modem_is_interface_supported(od->modem, OFONO_MODEM_INTERFACE_SIM_MANAGER)) {
		err.code = TELEPHONY_ERROR_NOT_AVAILABLE;
		cb(&err, data);
		return;
	}

	cbd = cb_data_new(cb, data);

	ofono_sim_manager_unlock_pin(od->sim, OFONO_SIM_PIN_TYPE_PIN, pin, pin1_common_cb, cbd);
}

void ofono_pin1_change(struct telephony_service *service, const gchar *old_pin, const gchar *new_pin, telephony_result_cb cb, void *data)
{
	struct ofono_data *od = telephony_service_get_data(service);
	struct telephony_error err;
	struct cb_data *cbd;

	if (!ofono_modem_is_interface_supported(od->modem, OFONO_MODEM_INTERFACE_SIM_MANAGER)) {
		err.code = TELEPHONY_ERROR_NOT_AVAILABLE;
		cb(&err, data);
		return;
	}

	cbd = cb_data_new(cb, data);

	ofono_sim_manager_change_pin(od->sim, OFONO_SIM_PIN_TYPE_PIN, old_pin, new_pin, pin1_common_cb, cbd);
}

void ofono_pin1_unblock(struct telephony_service *service, const gchar *puk, const gchar *new_pin, telephony_result_cb cb, void *data)
{
	struct ofono_data *od = telephony_service_get_data(service);
	struct telephony_error err;
	struct cb_data *cbd;

	if (!ofono_modem_is_interface_supported(od->modem, OFONO_MODEM_INTERFACE_SIM_MANAGER)) {
		err.code = TELEPHONY_ERROR_NOT_AVAILABLE;
		cb(&err, data);
		return;
	}

	cbd = cb_data_new(cb, data);

	ofono_sim_manager_reset_pin(od->sim, OFONO_SIM_PIN_TYPE_PIN, puk, new_pin, pin1_common_cb, cbd);
}

void ofono_fdn_status_query(struct telephony_service *service, telephony_fdn_status_query_cb cb, void *data)
{
	struct ofono_data *od = telephony_service_get_data(service);
	struct telephony_fdn_status fdn_status;
	struct telephony_error err;

	if (!ofono_modem_is_interface_supported(od->modem, OFONO_MODEM_INTERFACE_SIM_MANAGER)) {
		err.code = TELEPHONY_ERROR_NOT_AVAILABLE;
		cb(&err, NULL, data);
		return;
	}

	fdn_status.enabled = ofono_sim_manager_get_fixed_dialing(od->sim);
	fdn_status.permanent_block = ofono_sim_manager_is_pin_locked(od->sim, OFONO_SIM_PIN_TYPE_PIN2);

	cb(NULL, &fdn_status, data);
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

void ofono_network_status_query(struct telephony_service *service, telephony_network_status_query_cb cb, void *data)
{
	struct ofono_data *od = telephony_service_get_data(service);
	struct telephony_network_status status;
	struct telephony_error err;

	if (od->netreg) {
		if (retrieve_network_status(od, &status) < 0) {
			err.code = TELEPHONY_ERROR_INTERNAL;
			cb(&err, NULL, data);
			return;
		}
	}
	else {
		status.state = TELEPHONY_NETWORK_STATE_NO_SERVICE;
		status.registration = TELEPHONY_NETWORK_REGISTRATION_NO_SERVICE;
		status.name = NULL;
		status.data_registered = false;
	}

	cb(NULL, &status, data);
}

static int convert_strength_to_bars(int rssi)
{
	return (rssi * 5) / 100;
}

void ofono_signal_strength_query(struct telephony_service *service, telephony_signal_strength_query_cb cb, void *data)
{
	struct ofono_data *od = telephony_service_get_data(service);
	unsigned int strength = 0;

	if (od->netreg)
		strength = convert_strength_to_bars(ofono_network_registration_get_strength(od->netreg));

	cb(NULL, strength, data);
}

static void network_prop_changed_cb(const gchar *name, void *data)
{
	struct ofono_data *od = data;
	struct telephony_network_status net_status;
	int strength;

	if (g_str_equal(name, "Status") || g_str_equal(name, "Name")) {
		if (retrieve_network_status(od, &net_status) < 0)
			return;

		telephony_service_network_status_changed_notify(od->service, &net_status);
	}
	else if (g_str_equal(name, "Strength")) {
		strength = ofono_network_registration_get_strength(od->netreg);
		telephony_service_signal_strength_changed_notify(od->service, convert_strength_to_bars(strength));
	}
}

enum telephony_radio_access_mode select_best_radio_access_mode(struct ofono_network_operator *netop)
{
	if (ofono_network_operator_supports_technology(netop, OFONO_NETWORK_TECHNOLOGY_LTE))
		return TELEPHONY_RADIO_ACCESS_MODE_LTE;
	else if (ofono_network_operator_supports_technology(netop, OFONO_NETWORK_TECHNOLOGY_HSPA) ||
			 ofono_network_operator_supports_technology(netop, OFONO_NETWORK_TECHNOLOGY_UMTS))
		return TELEPHONY_RADIO_ACCESS_MODE_UMTS;

	return TELEPHONY_RADIO_ACCESS_MODE_GSM;
}

void scan_operators_cb(struct ofono_error *error, GList *operators, void *data)
{
	struct cb_data *cbd = data;
	struct ofono_data *od = cbd->user;
	struct telephony_error terr;
	telephony_network_list_query_cb cb = cbd->cb;
	GList *networks = NULL;
	int n, mcc, mnc;

	if (error) {
		terr.code = TELEPHONY_ERROR_INTERNAL;
		cb(&terr, NULL, cbd->data);
	}
	else {
		for(n = 0; n < g_list_length(operators); n++) {
			struct ofono_network_operator *netop = g_list_nth_data(operators, n);
			struct telephony_network *network = g_new0(struct telephony_network, 1);

			mcc = g_ascii_strtoll(ofono_network_operator_get_mcc(netop), NULL, 0);
			mnc = g_ascii_strtoll(ofono_network_operator_get_mnc(netop), NULL, 0);

			network->id = (mcc * 100) + mnc;
			network->name = ofono_network_operator_get_name(netop);
			network->radio_access_mode = select_best_radio_access_mode(netop);

			networks = g_list_append(networks, network);
		}

		cb(NULL, networks, cbd->data);
		g_list_free_full(networks, g_free);
	}

	od->network_scan_cancellable = 0;
}

void ofono_network_list_query(struct telephony_service *service, telephony_network_list_query_cb cb,
							 void *data)
{
	struct ofono_data *od = telephony_service_get_data(service);
	struct cb_data *cbd;
	struct telephony_error error;

	if (od->netreg) {
		od->network_scan_cancellable = g_cancellable_new();
		cbd = cb_data_new(cb, data);
		cbd->user = od;
		ofono_network_registration_scan(od->netreg, scan_operators_cb,
					od->network_scan_cancellable, cbd);
	}
	else {
		error.code = TELEPHONY_ERROR_NOT_AVAILABLE;
		cb(&error, NULL, data);
	}
}

void ofono_network_list_query_cancel(struct telephony_service *service, telephony_result_cb cb, void *data)
{
	struct ofono_data *od = telephony_service_get_data(service);
	struct telephony_error error;

	if (od->network_scan_cancellable) {
		g_cancellable_cancel(od->network_scan_cancellable);
		cb(NULL, data);
	}
	else {
		error.code = TELEPHONY_ERROR_INVALID_ARGUMENT;
		cb(&error, data);
	}
}

void ofono_network_id_query(struct telephony_service *service, telephony_network_id_query_cb cb, void *data)
{
	struct ofono_data *od = telephony_service_get_data(service);
	struct telephony_error error;
	char netid[6];

	if (od->netreg) {
		snprintf(netid, 6, "%s%s",
				 ofono_network_registration_get_mcc(od->netreg),
				 ofono_network_registration_get_mnc(od->netreg));

		cb(NULL, netid, data);
	}
	else {
		error.code = TELEPHONY_ERROR_NOT_AVAILABLE;
		cb(&error, NULL, data);
	}
}

void ofono_network_selection_mode_query(struct telephony_service *service, telephony_network_selection_mode_query_cb cb, void *data)
{
	struct ofono_data *od = telephony_service_get_data(service);
	struct telephony_error error;
	enum ofono_network_registration_mode mode;
	bool automatic = false;

	if (od->netreg) {
		mode = ofono_network_registration_get_mode(od->netreg);

		automatic = (mode == OFONO_NETWORK_REGISTRATION_MODE_AUTO ||
					 mode == OFONO_NETWORK_REGISTRATION_MODE_AUTO_ONLY);

		cb(NULL, automatic, data);
	}
	else {
		error.code = TELEPHONY_ERROR_NOT_AVAILABLE;
		cb(&error, false, data);
	}
}

void netop_register_cb(struct ofono_error *error, void *data)
{
	struct cb_data *cbd = data;
	telephony_result_cb cb = cbd->cb;
	struct telephony_error terr;

	if (error) {
		terr.code = TELEPHONY_ERROR_INTERNAL;
		cb(&terr, cbd->data);
	}
	else {
		cb(NULL, cbd->data);
	}

	g_free(cbd);
}

void get_operators_cb(struct ofono_error *err, GList *operators, void *data)
{
	struct cb_data *cbd = data;
	telephony_result_cb cb = cbd->cb;
	struct ofono_network_operator *netop = NULL;
	struct telephony_error terr;
	int n;
	const char *id = cbd->user;
	char netid[6];
	bool found = false;

	for (n = 0; n < g_list_length(operators); n++) {
		netop = g_list_nth_data(operators, n);

		snprintf(netid, 6, "%s%s",
				 ofono_network_operator_get_mcc(netop),
				 ofono_network_operator_get_mnc(netop));

		if (g_str_equal(netid, id)) {
			ofono_network_operator_register(netop, netop_register_cb, cbd);
			found = true;
			break;
		}
	}

	if (!found) {
		terr.code = TELEPHONY_ERROR_INVALID_ARGUMENT;
		cb(&terr, cbd->data);
		g_free(cbd);
	}
}

void register_automatically_cb(struct ofono_error *error, void *data)
{
	struct cb_data *cbd = data;
	telephony_result_cb cb = cbd->cb;
	struct telephony_error terr;

	if (error) {
		terr.code = TELEPHONY_ERROR_INTERNAL;
		cb(&terr, cbd->data);
	}
	else {
		cb(NULL, cbd->data);
	}

	g_free(cbd);
}

void ofono_network_set(struct telephony_service *service, bool automatic, const char *id,
					  telephony_result_cb cb, void *data)
{
	struct ofono_data *od = telephony_service_get_data(service);
	struct telephony_error error;
	struct cb_data *cbd;

	if (od->netreg) {
		cbd = cb_data_new(cb, data);

		if (!automatic) {
			cbd->user = (char*) id;
			ofono_network_registration_get_operators(od->netreg, get_operators_cb, cbd);
		}
		else {
			ofono_network_registration_register(od->netreg, register_automatically_cb, cbd);
		}
	}
	else {
		error.code = TELEPHONY_ERROR_NOT_AVAILABLE;
		cb(&error, data);
	}
}

void ofono_rat_query(struct telephony_service *service, telephony_rat_query_cb cb, void *data)
{
	struct ofono_data *od = telephony_service_get_data(service);
	struct telephony_error error;
	enum telephony_radio_access_mode mode;

	if (od->rs) {
		mode = ofono_radio_settings_get_technology_preference(od->rs);
		cb(NULL, mode, data);
	}
	else {
		error.code = TELEPHONY_ERROR_NOT_AVAILABLE;
		cb(&error, -1, data);
	}
}

void rat_set_cb(struct ofono_error *error, void *data)
{
	struct cb_data *cbd = data;
	telephony_result_cb cb = cbd->cb;
	struct telephony_error terr;

	if (error) {
		terr.code = TELEPHONY_ERROR_INTERNAL;
		cb(&terr, cbd->data);
	}
	else {
		cb(NULL, cbd->data);
	}

	g_free(cbd);
}

void ofono_rat_set(struct telephony_service *service, enum telephony_radio_access_mode mode, telephony_result_cb cb, void *data)
{
	struct ofono_data *od = telephony_service_get_data(service);
	struct telephony_error error;
	struct cb_data *cbd;

	if (od->rs) {
		cbd = cb_data_new(cb, data);
		ofono_radio_settings_set_technology_preference(od->rs, mode, rat_set_cb, cbd);
	}
	else {
		error.code = TELEPHONY_ERROR_NOT_AVAILABLE;
		cb(&error, data);
	}
}

void ofono_subscriber_id_query(struct telephony_service *service, telephony_subscriber_id_query_cb cb, void *data)
{
	struct ofono_data *od = telephony_service_get_data(service);
	struct telephony_error error;
	struct telephony_subscriber_info info;
	GSList *subscriber_numbers;

	if (od->sim) {
		memset(&info, 0, sizeof(struct telephony_subscriber_info));

		info.imsi = ofono_sim_manager_get_subscriber_identity(od->sim);

		subscriber_numbers = ofono_sim_manager_get_subscriber_numbers(od->sim);
		if (subscriber_numbers) {
			/* just take the first one as our service API doesn't support more than one */
			info.msisdn = subscriber_numbers->data;
		}

		cb(NULL, &info, data);
	}
	else {
		error.code = TELEPHONY_ERROR_NOT_AVAILABLE;
		cb(&error, NULL, data);
	}
}

static void dial_cb(const struct ofono_error *error, const char *path, void *data)
{
	struct cb_data *cbd = data;
	telephony_result_cb cb = cbd->cb;
	struct telephony_error terr;

	if (error) {
		terr.code = TELEPHONY_ERROR_INTERNAL;
		cb(&terr, cbd->data);
		goto cleanup;
	}

	cb(NULL, cbd->data);

cleanup:
	g_free(cbd);
}

void ofono_dial(struct telephony_service *service, const char *number, bool block_id, telephony_result_cb cb, void *data)
{
	struct ofono_data *od = telephony_service_get_data(service);
	struct telephony_error error;
	struct cb_data *cbd;

	if (!od->vm) {
		error.code = TELEPHONY_ERROR_NOT_AVAILABLE;
		cb(&error, data);
		return;
	}

	cbd = cb_data_new(cb, data);

	ofono_voicecall_manager_dial(od->vm, number,
		block_id ? OFONO_VOICECALL_CLIR_OPTION_ENABLED : OFONO_VOICECALL_CLIR_OPTION_DISABLED,
		dial_cb, cbd);
}

void ofono_answer(struct telephony_service *service, int call_id, telephony_result_cb cb, void *data)
{
	struct ofono_data *od = telephony_service_get_data(service);
	struct telephony_error error;

	if (!od->vm) {
		error.code = TELEPHONY_ERROR_NOT_AVAILABLE;
		cb(&error, data);
		return;
	}
}

static void message_status_cb(enum ofono_message_status status, void *data)
{
	struct cb_data *cbd = data;
	telephony_result_cb cb = cbd->cb;
	struct telephony_error terr;
	struct ofono_message_watch *watch = cbd->user;

	if (status == OFONO_MESSAGE_STATUS_FAILED) {
		terr.code = TELEPHONY_ERROR_FAIL;
		cb(&terr, cbd->data);
		goto cleanup;
	}
	else if (status == OFONO_MESSAGE_STATUS_SENT) {
		cb(NULL, cbd->data);
		goto cleanup;
	}

	return;

cleanup:
	if (watch)
		ofono_message_watch_free(watch);

	g_free(cbd);
}

static void send_sms_cb(struct ofono_error *error, const char *path, void *data)
{
	struct cb_data *cbd = data;
	telephony_result_cb cb = cbd->cb;
	struct telephony_error terr;

	if (error) {
		terr.code = TELEPHONY_ERROR_INTERNAL;
		cb(&terr, cbd->data);
		g_free(cbd);
		return;
	}

	struct ofono_message_watch *watch = ofono_message_watch_create(path);
	cbd->user = watch;
	ofono_message_watch_set_status_callback(watch, message_status_cb, cbd);
}

void ofono_send_sms(struct telephony_service *service, const char *to, const char *text, telephony_result_cb cb, void *data)
{
	struct ofono_data *od = telephony_service_get_data(service);
	struct telephony_error error;
	struct cb_data *cbd;

	if (!od->mm) {
		error.code = TELEPHONY_ERROR_NOT_AVAILABLE;
		cb(&error, data);
		return;
	}

	cbd = cb_data_new(cb, data);

	ofono_message_manager_send_message(od->mm, to, text, send_sms_cb, cbd);
}

static void incoming_message_cb(struct ofono_message *message, void *data)
{
	struct ofono_data *od = data;

	struct telephony_message msg;

	msg.sender = ofono_message_get_sender(message);
	msg.text = ofono_message_get_text(message);
	msg.sent_time = ofono_message_get_sent_time(message);

	switch (ofono_message_get_type(message)) {
	case OFONO_MESSAGE_TYPE_CLASS0:
		msg.type = TELEPHONY_MESSAGE_TYPE_CLASS0;
		break;
	case OFONO_MESSAGE_TYPE_TEXT:
		msg.type = TELEPHONY_MESSAGE_TYPE_TEXT;
		break;
	default:
		msg.type = TELEPHONY_MESSAGE_TYPE_UNKNOWN;
		break;
	}

	telephony_service_incoming_message_notify(od->service, &msg);
}

static void notify_no_network_registration(struct telephony_service *service)
{
	/* notify possible network status subscribers about us having no connectivity
	 * anymore */
	struct telephony_network_status net_status;

	net_status.state = TELEPHONY_NETWORK_STATE_NO_SERVICE;
	net_status.registration = TELEPHONY_NETWORK_REGISTRATION_NO_SERVICE;
	net_status.name = 0;

	telephony_service_network_status_changed_notify(service, &net_status);
}

static void modem_prop_changed_cb(const gchar *name, void *data)
{
	struct ofono_data *od = data;
	bool powered = false, online = false;
	const char *path = ofono_modem_get_path(od->modem);

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
			notify_no_network_registration(od->service);
		}

		if (!od->rs && ofono_modem_is_interface_supported(od->modem, OFONO_MODEM_INTERFACE_RADIO_SETTINGS)) {
			od->rs = ofono_radio_settings_create(path);
		}
		else if (od->rs && !ofono_modem_is_interface_supported(od->modem, OFONO_MODEM_INTERFACE_RADIO_SETTINGS)) {
			ofono_radio_settings_free(od->rs);
			od->rs = NULL;
		}

		if (!od->vm && ofono_modem_is_interface_supported(od->modem, OFONO_MODEM_INTERFACE_VOICE_CALL_MANAGER)) {
			od->vm = ofono_voicecall_manager_create(path);
		}
		else if (od->vm && !ofono_modem_is_interface_supported(od->modem, OFONO_MODEM_INTERFACE_VOICE_CALL_MANAGER)) {
			ofono_voicecall_manager_free(od->vm);
			od->vm = NULL;
		}

		if (!od->mm && ofono_modem_is_interface_supported(od->modem, OFONO_MODEM_INTERFACE_MESSAGE_MANAGER)) {
			od->mm = ofono_message_manager_create(path);
			ofono_message_manager_set_incoming_message_callback(od->mm, incoming_message_cb, od);
		}
		else if (od->mm && !ofono_modem_is_interface_supported(od->modem, OFONO_MODEM_INTERFACE_MESSAGE_MANAGER)) {
			ofono_message_manager_free(od->mm);
			od->mm = NULL;
		}
	}
	else if (g_str_equal(name, "Powered")) {
		powered = ofono_modem_get_powered(od->modem);
		/* We need to handle power status changes differently when in initialization phase */
		if (od->initializing && powered) {
			telephony_service_availability_changed_notify(od->service, true);
			od->initializing = false;
		}

		telephony_service_power_status_notify(od->service, powered);
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
	if (od->mm) {
		ofono_message_manager_free(od->mm);
		od->mm = NULL;
	}

	if (od->rs) {
		ofono_radio_settings_free(od->rs);
		od->rs = NULL;
	}

	if (od->vm) {
		ofono_voicecall_manager_free(od->vm);
		od->vm = NULL;
	}

	if (od->netreg) {
		ofono_network_registration_free(od->netreg);
		od->netreg = NULL;
	}

	if (od->sim) {
		ofono_sim_manager_free(od->sim);
		od->sim = NULL;
	}

	if (od->manager) {
		ofono_manager_free(od->manager);
		od->manager = NULL;
	}

	/* The manager already takes care about releasing the modem instances */
	od->modem = NULL;
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

	telephony_service_availability_changed_notify(od->service, false);
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
	data->calls = g_hash_table_new_full(g_str_hash, g_str_equal,
										g_free, (GDestroyNotify) g_hash_table_destroy);

	data->service_watch = g_bus_watch_name(G_BUS_TYPE_SYSTEM, "org.ofono", G_BUS_NAME_WATCHER_FLAGS_NONE,
					 service_appeared_cb, service_vanished_cb, data, NULL);

	return 0;
}

void ofono_remove(struct telephony_service *service)
{
	struct ofono_data *data = 0;

	data = telephony_service_get_data(service);

	g_hash_table_destroy(data->calls);

	free_used_instances(data);

	g_bus_unwatch_name(data->service_watch);

	g_free(data);

	telephony_service_set_data(service, NULL);
}

struct telephony_driver ofono_telephony_driver = {
	.probe =		ofono_probe,
	.remove =		ofono_remove,
	.platform_query		= ofono_platform_query,
	.power_set =	ofono_power_set,
	.power_query =	ofono_power_query,
	.sim_status_query = ofono_sim_status_query,
	.pin1_status_query = ofono_pin1_status_query,
	.pin2_status_query = ofono_pin2_status_query,
	.pin1_verify = ofono_pin1_verify,
	.pin1_enable = ofono_pin1_enable,
	.pin1_disable = ofono_pin1_disable,
	.pin1_change = ofono_pin1_change,
	.pin1_unblock = ofono_pin1_unblock,
	.fdn_status_query = ofono_fdn_status_query,
	.network_status_query = ofono_network_status_query,
	.signal_strength_query = ofono_signal_strength_query,
	.network_list_query = ofono_network_list_query,
	.network_list_query_cancel = ofono_network_list_query_cancel,
	.network_id_query = ofono_network_id_query,
	.network_selection_mode_query = ofono_network_selection_mode_query,
	.network_set = ofono_network_set,
	.rat_query = ofono_rat_query,
	.rat_set = ofono_rat_set,
	.subscriber_id_query = ofono_subscriber_id_query,
	.dial = ofono_dial,
	.answer = ofono_answer,
	.send_sms = ofono_send_sms
};

// vim:ts=4:sw=4:noexpandtab
