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
#include "netutils.h"

struct ofono_data;

/**
 * Everything that belongs to one modem. Before dual SIM support these fields
 * lived directly in struct ofono_data and there was exactly one modem; now
 * there is one of these per slot and struct ofono_data only holds the set.
 *
 * The service back pointer is kept here so that the many callbacks which are
 * handed this struct can notify the service about their own slot.
 */
struct ofono_sim_data {
	struct telephony_service *service;
	struct ofono_data *parent;
	int sim_id;
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
	GCancellable *network_scan_cancellable;
};

struct ofono_data {
	struct telephony_service *service;
	struct ofono_manager *manager;
	/* struct ofono_sim_data*, index == sim_id */
	GPtrArray *sims;
	guint service_watch;
};

/* Look up the per modem state the service is asking us to act on. */
static struct ofono_sim_data* get_sim_data(struct telephony_service *service, int sim_id)
{
	struct ofono_data *od = telephony_service_get_data(service);

	if (!od || !od->sims || sim_id < 0 || sim_id >= (int) od->sims->len)
		return NULL;

	return g_ptr_array_index(od->sims, sim_id);
}

static void notify_sim_info(struct ofono_sim_data *od);

/**
 * Callbacks that need their slot after an async round trip must not keep the
 * ofono_sim_data pointer itself: the slot can be torn down while the call is
 * in flight (modem removed, ofono gone). They carry service + sim_id instead
 * and re-resolve through get_sim_data(), which returns NULL for a dead slot.
 */
struct slot_ref {
	struct telephony_service *service;
	int sim_id;
};

struct power_set_data {
	struct slot_ref slot;
	telephony_result_cb cb;
	void *data;
};

/* Clear the in-progress marker on whichever slot the request was for; the
 * slot may be gone or reused by the time the modem answers. */
static void power_set_finished(struct power_set_data *psd)
{
	struct ofono_sim_data *od = get_sim_data(psd->slot.service, psd->slot.sim_id);

	if (od)
		od->power_set_pending = false;
}

static void set_online_cb(struct ofono_error *error, gpointer user_data)
{
	struct power_set_data *psd = user_data;
	struct telephony_error terr;

	power_set_finished(psd);

	if (error) {
		terr.code = TELEPHONY_ERROR_INTERNAL;
		psd->cb(&terr, psd->data);
	}
	else {
		psd->cb(NULL, psd->data);
	}

	g_free(psd);
}

static void set_powered_cb(struct ofono_error *error, gpointer user_data)
{
	struct power_set_data *psd = user_data;
	struct ofono_sim_data *od;
	struct telephony_error terr;

	if (error) {
		power_set_finished(psd);
		terr.code = TELEPHONY_ERROR_INTERNAL;
		psd->cb(&terr, psd->data);
		g_free(psd);
		return;
	}

	od = get_sim_data(psd->slot.service, psd->slot.sim_id);
	if (od && od->power_target) {
		ofono_modem_set_online(od->modem, od->power_target, set_online_cb, psd);
		return;
	}

	power_set_finished(psd);
	psd->cb(NULL, psd->data);
	g_free(psd);
}

void ofono_power_set(struct telephony_service *service, int sim_id, bool power, telephony_result_cb cb, void *data)
{
	struct power_set_data *psd = NULL;
	struct ofono_sim_data *od = get_sim_data(service, sim_id);
	bool powered = false;
	struct telephony_error error;

	if (!od) {
		struct telephony_error nosim = { .code = TELEPHONY_ERROR_NOT_AVAILABLE };
		cb(&nosim, data);
		return;
	}

	if (od->power_set_pending) {
		error.code = TELEPHONY_ERROR_ALREADY_INPROGRESS;
		cb(&error, data);
		return;
	}

	od->power_set_pending = true;
	od->power_target = power;

	/* allocated only once we know the call will actually be issued, so that
	 * the early returns above cannot leak it */
	psd = g_new0(struct power_set_data, 1);
	psd->slot.service = service;
	psd->slot.sim_id = sim_id;
	psd->cb = cb;
	psd->data = data;

	powered = ofono_modem_get_powered(od->modem);
	if (!powered) {
		ofono_modem_set_powered(od->modem, power, set_powered_cb, psd);
	}
	else {
		ofono_modem_set_online(od->modem, power, set_online_cb, psd);
	}
}

void ofono_power_query(struct telephony_service *service, int sim_id, telephony_power_query_cb cb, void *data)
{
	bool powered = false;
	struct ofono_sim_data *od = get_sim_data(service, sim_id);
	struct telephony_error error;

	if (!od) {
		struct telephony_error nosim = { .code = TELEPHONY_ERROR_NOT_AVAILABLE };
		cb(&nosim, false, data);
		return;
	}

	if (!od->modem) {
		error.code = TELEPHONY_ERROR_INTERNAL;
		cb(&error, false, data);
		return;
	}

	powered = ofono_modem_get_powered(od->modem) &&
			  ofono_modem_get_online(od->modem);

	cb(NULL, powered, data);
}

void ofono_platform_query(struct telephony_service *service, int sim_id, telephony_platform_query_cb cb, void *data)
{
	struct ofono_sim_data *od = get_sim_data(service, sim_id);
	struct telephony_platform_info pinfo;
	const char *mnc = NULL;
	const char *mcc = NULL;
	struct telephony_error error;

	if (!od) {
		struct telephony_error nosim = { .code = TELEPHONY_ERROR_NOT_AVAILABLE };
		cb(&nosim, NULL, data);
		return;
	}

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

static enum telephony_sim_status determine_sim_status(struct ofono_sim_data *od)
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

static void determine_pin_status(struct ofono_sim_data *od, struct telephony_pin_status *pin_status,
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
	struct ofono_sim_data *od = data;
	enum telephony_sim_status sim_status = TELEPHONY_SIM_STATUS_SIM_INVALID;
	struct telephony_pin_status pin_status;

	sim_status = determine_sim_status(od);

	if (sim_status != od->sim_status) {
		od->sim_status = sim_status;
		telephony_service_sim_status_notify(od->service, od->sim_id, sim_status);

		determine_pin_status(od, &pin_status, OFONO_SIM_PIN_TYPE_PIN, OFONO_SIM_PIN_TYPE_PUK);
		telephony_service_pin1_status_changed_notify(od->service, od->sim_id, &pin_status);
	}

	/* CardIdentifier/SubscriberIdentity only turn up once the SIM is readable,
	 * and the stored default SIM is matched on the ICCID, so republish. */
	notify_sim_info(od);
}

void ofono_sim_status_query(struct telephony_service *service, int sim_id, telephony_sim_status_query_cb cb, void *data)
{
	struct ofono_sim_data *od = get_sim_data(service, sim_id);
	struct telephony_error err;

	if (!od) {
		struct telephony_error nosim = { .code = TELEPHONY_ERROR_NOT_AVAILABLE };
		cb(&nosim, TELEPHONY_SIM_STATUS_SIM_NOT_FOUND, data);
		return;
	}

	if (!ofono_modem_is_interface_supported(od->modem, OFONO_MODEM_INTERFACE_SIM_MANAGER)) {
		err.code = TELEPHONY_ERROR_NOT_IMPLEMENTED;
		cb(&err, TELEPHONY_SIM_STATUS_SIM_INVALID, data);
		return;
	}

	od->sim_status = determine_sim_status(od);
	cb(NULL, od->sim_status, data);
}

void ofono_pin1_status_query(struct telephony_service *service, int sim_id, telephony_pin_status_query_cb cb, void *data)
{
	struct ofono_sim_data *od = get_sim_data(service, sim_id);
	struct telephony_pin_status pin_status;
	struct telephony_error err;

	if (!od) {
		struct telephony_error nosim = { .code = TELEPHONY_ERROR_NOT_AVAILABLE };
		cb(&nosim, NULL, data);
		return;
	}

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

void ofono_pin2_status_query(struct telephony_service *service, int sim_id, telephony_pin_status_query_cb cb, void *data)
{
	struct ofono_sim_data *od = get_sim_data(service, sim_id);
	struct telephony_pin_status pin_status;
	struct telephony_error err;

	if (!od) {
		struct telephony_error nosim = { .code = TELEPHONY_ERROR_NOT_AVAILABLE };
		cb(&nosim, NULL, data);
		return;
	}

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

void ofono_pin1_verify(struct telephony_service *service, int sim_id, const gchar *pin, telephony_result_cb cb, void *data)
{
	struct ofono_sim_data *od = get_sim_data(service, sim_id);
	struct telephony_error err;
	struct cb_data *cbd;

	if (!od) {
		struct telephony_error nosim = { .code = TELEPHONY_ERROR_NOT_AVAILABLE };
		cb(&nosim, data);
		return;
	}

	if (!ofono_modem_is_interface_supported(od->modem, OFONO_MODEM_INTERFACE_SIM_MANAGER)) {
		err.code = TELEPHONY_ERROR_NOT_AVAILABLE;
		cb(&err, data);
		return;
	}

	cbd = cb_data_new(cb, data);

	ofono_sim_manager_enter_pin(od->sim, OFONO_SIM_PIN_TYPE_PIN, pin, pin1_common_cb, cbd);
}

void ofono_pin1_enable(struct telephony_service *service, int sim_id, const gchar *pin, telephony_result_cb cb, void *data)
{
	struct ofono_sim_data *od = get_sim_data(service, sim_id);
	struct telephony_error err;
	struct cb_data *cbd;

	if (!od) {
		struct telephony_error nosim = { .code = TELEPHONY_ERROR_NOT_AVAILABLE };
		cb(&nosim, data);
		return;
	}

	if (!ofono_modem_is_interface_supported(od->modem, OFONO_MODEM_INTERFACE_SIM_MANAGER)) {
		err.code = TELEPHONY_ERROR_NOT_AVAILABLE;
		cb(&err, data);
		return;
	}

	cbd = cb_data_new(cb, data);

	ofono_sim_manager_lock_pin(od->sim, OFONO_SIM_PIN_TYPE_PIN, pin, pin1_common_cb, cbd);
}

void ofono_pin1_disable(struct telephony_service *service, int sim_id, const gchar *pin, telephony_result_cb cb, void *data)
{
	struct ofono_sim_data *od = get_sim_data(service, sim_id);
	struct telephony_error err;
	struct cb_data *cbd;

	if (!od) {
		struct telephony_error nosim = { .code = TELEPHONY_ERROR_NOT_AVAILABLE };
		cb(&nosim, data);
		return;
	}

	if (!ofono_modem_is_interface_supported(od->modem, OFONO_MODEM_INTERFACE_SIM_MANAGER)) {
		err.code = TELEPHONY_ERROR_NOT_AVAILABLE;
		cb(&err, data);
		return;
	}

	cbd = cb_data_new(cb, data);

	ofono_sim_manager_unlock_pin(od->sim, OFONO_SIM_PIN_TYPE_PIN, pin, pin1_common_cb, cbd);
}

void ofono_pin1_change(struct telephony_service *service, int sim_id, const gchar *old_pin, const gchar *new_pin, telephony_result_cb cb, void *data)
{
	struct ofono_sim_data *od = get_sim_data(service, sim_id);
	struct telephony_error err;
	struct cb_data *cbd;

	if (!od) {
		struct telephony_error nosim = { .code = TELEPHONY_ERROR_NOT_AVAILABLE };
		cb(&nosim, data);
		return;
	}

	if (!ofono_modem_is_interface_supported(od->modem, OFONO_MODEM_INTERFACE_SIM_MANAGER)) {
		err.code = TELEPHONY_ERROR_NOT_AVAILABLE;
		cb(&err, data);
		return;
	}

	cbd = cb_data_new(cb, data);

	ofono_sim_manager_change_pin(od->sim, OFONO_SIM_PIN_TYPE_PIN, old_pin, new_pin, pin1_common_cb, cbd);
}

void ofono_pin1_unblock(struct telephony_service *service, int sim_id, const gchar *puk, const gchar *new_pin, telephony_result_cb cb, void *data)
{
	struct ofono_sim_data *od = get_sim_data(service, sim_id);
	struct telephony_error err;
	struct cb_data *cbd;

	if (!od) {
		struct telephony_error nosim = { .code = TELEPHONY_ERROR_NOT_AVAILABLE };
		cb(&nosim, data);
		return;
	}

	if (!ofono_modem_is_interface_supported(od->modem, OFONO_MODEM_INTERFACE_SIM_MANAGER)) {
		err.code = TELEPHONY_ERROR_NOT_AVAILABLE;
		cb(&err, data);
		return;
	}

	cbd = cb_data_new(cb, data);

	ofono_sim_manager_reset_pin(od->sim, OFONO_SIM_PIN_TYPE_PIN, puk, new_pin, pin1_common_cb, cbd);
}

void ofono_fdn_status_query(struct telephony_service *service, int sim_id, telephony_fdn_status_query_cb cb, void *data)
{
	struct ofono_sim_data *od = get_sim_data(service, sim_id);
	struct telephony_fdn_status fdn_status;
	struct telephony_error err;

	if (!od) {
		struct telephony_error nosim = { .code = TELEPHONY_ERROR_NOT_AVAILABLE };
		cb(&nosim, NULL, data);
		return;
	}

	if (!ofono_modem_is_interface_supported(od->modem, OFONO_MODEM_INTERFACE_SIM_MANAGER)) {
		err.code = TELEPHONY_ERROR_NOT_AVAILABLE;
		cb(&err, NULL, data);
		return;
	}

	fdn_status.enabled = ofono_sim_manager_get_fixed_dialing(od->sim);
	fdn_status.permanent_block = ofono_sim_manager_is_pin_locked(od->sim, OFONO_SIM_PIN_TYPE_PIN2);

	cb(NULL, &fdn_status, data);
}

static void retrieve_network_status(struct ofono_sim_data *od, struct telephony_network_status *status)
{
	enum ofono_network_status net_status;

	/* Sensible defaults so an unknown registration state cannot hand the
	 * caller uninitialized fields */
	status->state = TELEPHONY_NETWORK_STATE_NO_SERVICE;
	status->registration = TELEPHONY_NETWORK_REGISTRATION_NO_SERVICE;

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
}

void ofono_network_status_query(struct telephony_service *service, int sim_id, telephony_network_status_query_cb cb, void *data)
{
	struct ofono_sim_data *od = get_sim_data(service, sim_id);
	struct telephony_network_status status;

	if (!od) {
		struct telephony_error nosim = { .code = TELEPHONY_ERROR_NOT_AVAILABLE };
		cb(&nosim, NULL, data);
		return;
	}

	if (od->netreg) {
		retrieve_network_status(od, &status);
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

void ofono_signal_strength_query(struct telephony_service *service, int sim_id, telephony_signal_strength_query_cb cb, void *data)
{
	struct ofono_sim_data *od = get_sim_data(service, sim_id);
	unsigned int strength = 0;

	if (!od) {
		struct telephony_error nosim = { .code = TELEPHONY_ERROR_NOT_AVAILABLE };
		cb(&nosim, 0, data);
		return;
	}

	if (od->netreg)
		strength = convert_strength_to_bars(ofono_network_registration_get_strength(od->netreg));

	cb(NULL, strength, data);
}

static void network_prop_changed_cb(const gchar *name, void *data)
{
	struct ofono_sim_data *od = data;
	struct telephony_network_status net_status;
	int strength;

	if (g_str_equal(name, "Status") || g_str_equal(name, "Name")) {
		retrieve_network_status(od, &net_status);
		telephony_service_network_status_changed_notify(od->service, od->sim_id, &net_status);
	}
	else if (g_str_equal(name, "Strength")) {
		strength = ofono_network_registration_get_strength(od->netreg);
		telephony_service_signal_strength_changed_notify(od->service, od->sim_id, convert_strength_to_bars(strength));
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

struct network_scan_data {
	struct slot_ref slot;
	telephony_network_list_query_cb cb;
	void *data;
};

static void scan_operators_cb(struct ofono_error *error, GList *operators, void *data)
{
	struct network_scan_data *nsd = data;
	struct ofono_sim_data *od;
	struct telephony_error terr;
	GList *networks = NULL;
	GList *iter;

	/* The slot can be gone by now; only its bookkeeping needs it */
	od = get_sim_data(nsd->slot.service, nsd->slot.sim_id);
	if (od && od->network_scan_cancellable) {
		g_object_unref(od->network_scan_cancellable);
		od->network_scan_cancellable = NULL;
	}

	if (error) {
		terr.code = TELEPHONY_ERROR_INTERNAL;
		nsd->cb(&terr, NULL, nsd->data);
	}
	else {
		for (iter = operators; iter != NULL; iter = g_list_next(iter)) {
			struct ofono_network_operator *netop = iter->data;
			struct telephony_network *network = g_new0(struct telephony_network, 1);

			network->id = telephony_network_id_to_number(
				ofono_network_operator_get_mcc(netop),
				ofono_network_operator_get_mnc(netop));
			network->name = ofono_network_operator_get_name(netop);
			network->radio_access_mode = select_best_radio_access_mode(netop);

			networks = g_list_append(networks, network);
		}

		nsd->cb(NULL, networks, nsd->data);
		g_list_free_full(networks, g_free);
	}

	/* We own the scan result now */
	g_list_free_full(operators, (GDestroyNotify) ofono_network_operator_free);
	g_free(nsd);
}

void ofono_network_list_query(struct telephony_service *service, int sim_id, telephony_network_list_query_cb cb,
							 void *data)
{
	struct ofono_sim_data *od = get_sim_data(service, sim_id);
	struct telephony_error error;

	if (!od) {
		struct telephony_error nosim = { .code = TELEPHONY_ERROR_NOT_AVAILABLE };
		cb(&nosim, NULL, data);
		return;
	}

	if (od->network_scan_cancellable) {
		error.code = TELEPHONY_ERROR_ALREADY_INPROGRESS;
		cb(&error, NULL, data);
		return;
	}

	if (od->netreg) {
		struct network_scan_data *nsd = g_new0(struct network_scan_data, 1);
		nsd->slot.service = service;
		nsd->slot.sim_id = sim_id;
		nsd->cb = cb;
		nsd->data = data;

		od->network_scan_cancellable = g_cancellable_new();
		ofono_network_registration_scan(od->netreg, scan_operators_cb,
					od->network_scan_cancellable, nsd);
	}
	else {
		error.code = TELEPHONY_ERROR_NOT_AVAILABLE;
		cb(&error, NULL, data);
	}
}

void ofono_network_list_query_cancel(struct telephony_service *service, int sim_id, telephony_result_cb cb, void *data)
{
	struct ofono_sim_data *od = get_sim_data(service, sim_id);
	struct telephony_error error;

	if (!od) {
		struct telephony_error nosim = { .code = TELEPHONY_ERROR_NOT_AVAILABLE };
		cb(&nosim, data);
		return;
	}

	if (od->network_scan_cancellable) {
		g_cancellable_cancel(od->network_scan_cancellable);
		cb(NULL, data);
	}
	else {
		error.code = TELEPHONY_ERROR_INVALID_ARGUMENT;
		cb(&error, data);
	}
}

void ofono_network_id_query(struct telephony_service *service, int sim_id, telephony_network_id_query_cb cb, void *data)
{
	struct ofono_sim_data *od = get_sim_data(service, sim_id);
	struct telephony_error error;
	char netid[TELEPHONY_NETWORK_ID_SIZE];

	if (!od) {
		struct telephony_error nosim = { .code = TELEPHONY_ERROR_NOT_AVAILABLE };
		cb(&nosim, NULL, data);
		return;
	}

	if (od->netreg && telephony_format_network_id(
			ofono_network_registration_get_mcc(od->netreg),
			ofono_network_registration_get_mnc(od->netreg),
			netid, sizeof(netid))) {
		cb(NULL, netid, data);
	}
	else {
		error.code = TELEPHONY_ERROR_NOT_AVAILABLE;
		cb(&error, NULL, data);
	}
}

void ofono_network_selection_mode_query(struct telephony_service *service, int sim_id, telephony_network_selection_mode_query_cb cb, void *data)
{
	struct ofono_sim_data *od = get_sim_data(service, sim_id);
	struct telephony_error error;
	enum ofono_network_registration_mode mode;
	bool automatic = false;

	if (!od) {
		struct telephony_error nosim = { .code = TELEPHONY_ERROR_NOT_AVAILABLE };
		cb(&nosim, false, data);
		return;
	}

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

struct manual_register_data {
	char *id;
	telephony_result_cb cb;
	void *data;
	struct ofono_network_operator *netop;
};

static void manual_register_data_free(struct manual_register_data *mrd)
{
	ofono_network_operator_free(mrd->netop);
	g_free(mrd->id);
	g_free(mrd);
}

static void netop_register_cb(struct ofono_error *error, void *data)
{
	struct manual_register_data *mrd = data;
	struct telephony_error terr;

	if (error) {
		terr.code = TELEPHONY_ERROR_INTERNAL;
		mrd->cb(&terr, mrd->data);
	}
	else {
		mrd->cb(NULL, mrd->data);
	}

	manual_register_data_free(mrd);
}

static void get_operators_cb(struct ofono_error *err, GList *operators, void *data)
{
	struct manual_register_data *mrd = data;
	struct telephony_error terr;
	GList *iter;
	char netid[TELEPHONY_NETWORK_ID_SIZE];

	if (err) {
		terr.code = TELEPHONY_ERROR_INTERNAL;
		mrd->cb(&terr, mrd->data);
		manual_register_data_free(mrd);
		return;
	}

	for (iter = operators; iter != NULL && !mrd->netop; iter = g_list_next(iter)) {
		struct ofono_network_operator *netop = iter->data;

		if (telephony_format_network_id(ofono_network_operator_get_mcc(netop),
						ofono_network_operator_get_mnc(netop),
						netid, sizeof(netid)) &&
			g_str_equal(netid, mrd->id))
			mrd->netop = netop;
	}

	if (mrd->netop) {
		/* We own the list; the chosen operator has to outlive the async
		 * Register, so unlink it and free it with mrd when that is done */
		operators = g_list_remove(operators, mrd->netop);
		ofono_network_operator_register(mrd->netop, netop_register_cb, mrd);
	}
	else {
		terr.code = TELEPHONY_ERROR_INVALID_ARGUMENT;
		mrd->cb(&terr, mrd->data);
		manual_register_data_free(mrd);
	}

	g_list_free_full(operators, (GDestroyNotify) ofono_network_operator_free);
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

void ofono_network_set(struct telephony_service *service, int sim_id, bool automatic, const char *id,
					  telephony_result_cb cb, void *data)
{
	struct ofono_sim_data *od = get_sim_data(service, sim_id);
	struct telephony_error error;
	struct cb_data *cbd;

	if (!od) {
		struct telephony_error nosim = { .code = TELEPHONY_ERROR_NOT_AVAILABLE };
		cb(&nosim, data);
		return;
	}

	if (od->netreg) {
		if (!automatic) {
			if (!id || !id[0]) {
				error.code = TELEPHONY_ERROR_INVALID_ARGUMENT;
				cb(&error, data);
				return;
			}

			struct manual_register_data *mrd = g_new0(struct manual_register_data, 1);
			mrd->id = g_strdup(id);
			mrd->cb = cb;
			mrd->data = data;

			ofono_network_registration_get_operators(od->netreg, get_operators_cb, mrd);
		}
		else {
			cbd = cb_data_new(cb, data);
			ofono_network_registration_register(od->netreg, register_automatically_cb, cbd);
		}
	}
	else {
		error.code = TELEPHONY_ERROR_NOT_AVAILABLE;
		cb(&error, data);
	}
}

void ofono_rat_query(struct telephony_service *service, int sim_id, telephony_rat_query_cb cb, void *data)
{
	struct ofono_sim_data *od = get_sim_data(service, sim_id);
	struct telephony_error error;
	enum telephony_radio_access_mode mode;

	if (!od) {
		struct telephony_error nosim = { .code = TELEPHONY_ERROR_NOT_AVAILABLE };
		cb(&nosim, -1, data);
		return;
	}

	if (od->rs) {
		switch (ofono_radio_settings_get_technology_preference(od->rs)) {
		case OFONO_RADIO_ACCESS_MODE_ANY:
			mode = TELEPHONY_RADIO_ACCESS_MODE_ANY;
			break;
		case OFONO_RADIO_ACCESS_MODE_GSM:
			mode = TELEPHONY_RADIO_ACCESS_MODE_GSM;
			break;
		case OFONO_RADIO_ACCESS_MODE_UMTS:
			mode = TELEPHONY_RADIO_ACCESS_MODE_UMTS;
			break;
		case OFONO_RADIO_ACCESS_MODE_LTE:
			mode = TELEPHONY_RADIO_ACCESS_MODE_LTE;
			break;
		default:
			mode = TELEPHONY_RADIO_ACCESS_MODE_UNKNOWN;
			break;
		}
		cb(NULL, mode, data);
	}
	else {
		error.code = TELEPHONY_ERROR_NOT_AVAILABLE;
		cb(&error, TELEPHONY_RADIO_ACCESS_MODE_INVALID, data);
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

void ofono_rat_set(struct telephony_service *service, int sim_id, enum telephony_radio_access_mode mode, telephony_result_cb cb, void *data)
{
	struct ofono_sim_data *od = get_sim_data(service, sim_id);
	struct telephony_error error;
	struct cb_data *cbd;

	if (!od) {
		struct telephony_error nosim = { .code = TELEPHONY_ERROR_NOT_AVAILABLE };
		cb(&nosim, data);
		return;
	}

	if (od->rs) {
		enum ofono_radio_access_mode ofono_mode;

		switch (mode) {
		case TELEPHONY_RADIO_ACCESS_MODE_ANY:
			ofono_mode = OFONO_RADIO_ACCESS_MODE_ANY;
			break;
		case TELEPHONY_RADIO_ACCESS_MODE_GSM:
			ofono_mode = OFONO_RADIO_ACCESS_MODE_GSM;
			break;
		case TELEPHONY_RADIO_ACCESS_MODE_UMTS:
			ofono_mode = OFONO_RADIO_ACCESS_MODE_UMTS;
			break;
		case TELEPHONY_RADIO_ACCESS_MODE_LTE:
			ofono_mode = OFONO_RADIO_ACCESS_MODE_LTE;
			break;
		default:
			error.code = TELEPHONY_ERROR_INVALID_ARGUMENT;
			cb(&error, data);
			return;
		}

		cbd = cb_data_new(cb, data);
		ofono_radio_settings_set_technology_preference(od->rs, ofono_mode, rat_set_cb, cbd);
	}
	else {
		error.code = TELEPHONY_ERROR_NOT_AVAILABLE;
		cb(&error, data);
	}
}

void ofono_subscriber_id_query(struct telephony_service *service, int sim_id, telephony_subscriber_id_query_cb cb, void *data)
{
	struct ofono_sim_data *od = get_sim_data(service, sim_id);
	struct telephony_error error;
	struct telephony_subscriber_info info;
	GSList *subscriber_numbers;

	if (!od) {
		struct telephony_error nosim = { .code = TELEPHONY_ERROR_NOT_AVAILABLE };
		cb(&nosim, NULL, data);
		return;
	}

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

void ofono_dial(struct telephony_service *service, int sim_id, const char *number, bool block_id, telephony_result_cb cb, void *data)
{
	struct ofono_sim_data *od = get_sim_data(service, sim_id);
	struct telephony_error error;
	struct cb_data *cbd;

	if (!od) {
		struct telephony_error nosim = { .code = TELEPHONY_ERROR_NOT_AVAILABLE };
		cb(&nosim, data);
		return;
	}

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

static void answer_call_cb(struct ofono_error *error, void *data)
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

static void answer_get_calls_cb(const struct ofono_error *error, GList *calls, void *data)
{
	struct cb_data *cbd = data;
	telephony_result_cb cb = cbd->cb;
	struct telephony_error terr;
	struct ofono_voicecall *incoming = NULL;
	GList *iter;

	if (error) {
		terr.code = TELEPHONY_ERROR_INTERNAL;
		cb(&terr, cbd->data);
		g_free(cbd);
		return;
	}

	/* The service API carries no usable call id, so answer the ringing
	 * call; ofono rejects Answer on anything that is not incoming anyway. */
	for (iter = calls; iter != NULL; iter = g_list_next(iter)) {
		struct ofono_voicecall *call = iter->data;
		enum ofono_voicecall_state state = ofono_voicecall_get_state(call);

		if (state == OFONO_VOICECALL_STATE_INCOMING ||
			state == OFONO_VOICECALL_STATE_WAITING) {
			incoming = call;
			break;
		}
	}

	if (!incoming) {
		terr.code = TELEPHONY_ERROR_INVALID_ARGUMENT;
		cb(&terr, cbd->data);
		g_free(cbd);
		return;
	}

	/* cbd travels on; answer_call_cb frees it */
	ofono_voicecall_answer(incoming, answer_call_cb, cbd);
}

void ofono_answer(struct telephony_service *service, int sim_id, int call_id, telephony_result_cb cb, void *data)
{
	struct ofono_sim_data *od = get_sim_data(service, sim_id);
	struct telephony_error error;
	struct cb_data *cbd;

	if (!od) {
		struct telephony_error nosim = { .code = TELEPHONY_ERROR_NOT_AVAILABLE };
		cb(&nosim, data);
		return;
	}

	if (!od->vm) {
		error.code = TELEPHONY_ERROR_NOT_AVAILABLE;
		cb(&error, data);
		return;
	}

	cbd = cb_data_new(cb, data);
	ofono_voicecall_manager_get_calls(od->vm, answer_get_calls_cb, cbd);
}

static gboolean free_message_watch_idle_cb(gpointer data)
{
	ofono_message_watch_free(data);
	return G_SOURCE_REMOVE;
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
	/* We are called from inside the watch's own property dispatch; freeing
	 * it here would pull the ground from under the iteration that called
	 * us, so hand it to the main loop instead. */
	if (watch)
		g_idle_add(free_message_watch_idle_cb, watch);

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
	if (!watch) {
		/* Without a watch the send result can never be reported */
		terr.code = TELEPHONY_ERROR_INTERNAL;
		cb(&terr, cbd->data);
		g_free(cbd);
		return;
	}

	cbd->user = watch;
	ofono_message_watch_set_status_callback(watch, message_status_cb, cbd);
}

void ofono_send_sms(struct telephony_service *service, int sim_id, const char *to, const char *text, telephony_result_cb cb, void *data)
{
	struct ofono_sim_data *od = get_sim_data(service, sim_id);
	struct telephony_error error;
	struct cb_data *cbd;

	if (!od) {
		struct telephony_error nosim = { .code = TELEPHONY_ERROR_NOT_AVAILABLE };
		cb(&nosim, data);
		return;
	}

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
	struct ofono_sim_data *od = data;

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

	telephony_service_incoming_message_notify(od->service, od->sim_id, &msg);
}

static void notify_no_network_registration(struct ofono_sim_data *od)
{
	/* notify possible network status subscribers about us having no connectivity
	 * anymore */
	struct telephony_network_status net_status;

	memset(&net_status, 0, sizeof(net_status));

	net_status.state = TELEPHONY_NETWORK_STATE_NO_SERVICE;
	net_status.registration = TELEPHONY_NETWORK_REGISTRATION_NO_SERVICE;
	net_status.name = 0;

	telephony_service_network_status_changed_notify(od->service, od->sim_id, &net_status);
}

static void modem_prop_changed_cb(const gchar *name, void *data)
{
	struct ofono_sim_data *od = data;
	bool powered = false, online = false;
	const char *path = ofono_modem_get_path(od->modem);

	if (g_str_equal(name, "Online")) {
		online = ofono_modem_get_online(od->modem);
		telephony_service_power_status_notify(od->service, od->sim_id, online);
		notify_sim_info(od);
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
			notify_no_network_registration(od);
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

		notify_sim_info(od);
	}
	else if (g_str_equal(name, "Powered")) {
		powered = ofono_modem_get_powered(od->modem);
		/* We need to handle power status changes differently when in initialization phase */
		if (od->initializing && powered) {
			telephony_service_availability_changed_notify(od->service, od->sim_id, true);
			od->initializing = false;
		}

		telephony_service_power_status_notify(od->service, od->sim_id, powered);
		notify_sim_info(od);
	}
}

/**
 * Push the descriptive data of this slot (ICCID, IMSI, number, operator) up to
 * the service so it can enumerate the SIMs and match stored preferences.
 */
static void notify_sim_info(struct ofono_sim_data *od)
{
	struct telephony_sim_info info;
	GSList *subscriber_numbers;

	memset(&info, 0, sizeof(info));

	info.sim_id = od->sim_id;
	info.sim_status = determine_sim_status(od);
	info.present = (od->sim && ofono_sim_manager_get_present(od->sim));
	info.modem_path = od->modem ? ofono_modem_get_path(od->modem) : NULL;
	info.powered = od->modem ? (ofono_modem_get_powered(od->modem) &&
								ofono_modem_get_online(od->modem)) : false;

	if (od->sim) {
		info.iccid = ofono_sim_manager_get_card_identifier(od->sim);
		info.imsi = ofono_sim_manager_get_subscriber_identity(od->sim);

		subscriber_numbers = ofono_sim_manager_get_subscriber_numbers(od->sim);
		if (subscriber_numbers)
			info.msisdn = subscriber_numbers->data;
	}

	if (od->netreg)
		info.operator_name = ofono_network_registration_get_operator_name(od->netreg);

	telephony_service_sim_info_changed_notify(od->service, od->sim_id, &info);
}

void ofono_sim_info_query(struct telephony_service *service, int sim_id,
                          telephony_sim_info_query_cb cb, void *data)
{
	struct ofono_sim_data *od = get_sim_data(service, sim_id);
	struct telephony_sim_info info;
	struct telephony_error err;
	GSList *subscriber_numbers;

	if (!od) {
		err.code = TELEPHONY_ERROR_NOT_AVAILABLE;
		cb(&err, NULL, data);
		return;
	}

	memset(&info, 0, sizeof(info));

	info.sim_id = sim_id;
	info.sim_status = determine_sim_status(od);
	info.present = (od->sim && ofono_sim_manager_get_present(od->sim));
	info.modem_path = od->modem ? ofono_modem_get_path(od->modem) : NULL;
	info.powered = od->modem ? (ofono_modem_get_powered(od->modem) &&
								ofono_modem_get_online(od->modem)) : false;

	if (od->sim) {
		info.iccid = ofono_sim_manager_get_card_identifier(od->sim);
		info.imsi = ofono_sim_manager_get_subscriber_identity(od->sim);

		subscriber_numbers = ofono_sim_manager_get_subscriber_numbers(od->sim);
		if (subscriber_numbers)
			info.msisdn = subscriber_numbers->data;
	}

	if (od->netreg)
		info.operator_name = ofono_network_registration_get_operator_name(od->netreg);

	cb(NULL, &info, data);
}

/* Release the per interface proxies of one modem. */
static void free_used_instances(struct ofono_sim_data *od)
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
}

static void sim_data_detach(struct ofono_sim_data *od)
{
	free_used_instances(od);

	if (od->network_scan_cancellable) {
		g_cancellable_cancel(od->network_scan_cancellable);
		g_object_unref(od->network_scan_cancellable);
		od->network_scan_cancellable = NULL;
	}

	if (od->modem) {
		ofono_modem_unref(od->modem);
		od->modem = NULL;
	}
}

static void sim_data_free(gpointer data)
{
	struct ofono_sim_data *od = data;

	if (!od)
		return;

	sim_data_detach(od);

	g_free(od);
}

static struct ofono_sim_data* sim_data_new(struct ofono_data *parent, int sim_id)
{
	struct ofono_sim_data *od;

	od = g_new0(struct ofono_sim_data, 1);
	od->parent = parent;
	od->service = parent->service;
	od->sim_id = sim_id;
	od->sim_status = TELEPHONY_SIM_STATUS_SIM_INVALID;
	od->initializing = false;

	return od;
}

/**
 * ofono hands us the full modem list on every change, so rebuild our slots from
 * it. A modem which is still at the same object path keeps its slot (and with
 * it any state we already gathered); everything else is torn down and set up
 * again.
 */
static void modems_changed_cb(gpointer user_data)
{
	struct ofono_data *data = user_data;
	const GList *modems = NULL;
	const GList *iter = NULL;
	struct ofono_sim_data *od = NULL;
	struct ofono_modem *modem = NULL;
	const gchar *path = NULL;
	int sim_id = 0;
	int n;

	modems = ofono_manager_get_modems(data->manager);

	for (iter = modems; iter != NULL && sim_id < TELEPHONY_MAX_SIMS; iter = iter->next, sim_id++) {
		modem = iter->data;
		path = ofono_modem_get_path(modem);

		if (sim_id < (int) data->sims->len) {
			od = g_ptr_array_index(data->sims, sim_id);

			if (od->modem && g_strcmp0(ofono_modem_get_path(od->modem), path) == 0) {
				/* same modem as before, nothing to re-attach */
				continue;
			}

			sim_data_detach(od);
		}
		else {
			od = sim_data_new(data, sim_id);
			g_ptr_array_add(data->sims, od);
		}

		ofono_modem_ref(modem);
		od->modem = modem;
		od->initializing = true;

		ofono_modem_register_prop_changed_handler(od->modem, modem_prop_changed_cb, od);
	}

	/* drop the slots ofono no longer reports */
	for (n = (int) data->sims->len - 1; n >= sim_id; n--) {
		od = g_ptr_array_index(data->sims, n);
		telephony_service_availability_changed_notify(od->service, od->sim_id, false);
		g_ptr_array_remove_index(data->sims, n);
	}

	telephony_service_sim_count_changed_notify(data->service, (int) data->sims->len);

	for (n = 0; n < (int) data->sims->len; n++)
		notify_sim_info(g_ptr_array_index(data->sims, n));
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
	struct ofono_sim_data *sd = NULL;
	int n;

	g_message("ofono dbus service disappeared");

	for (n = 0; n < (int) od->sims->len; n++) {
		sd = g_ptr_array_index(od->sims, n);
		telephony_service_availability_changed_notify(sd->service, sd->sim_id, false);
	}

	g_ptr_array_set_size(od->sims, 0);

	if (od->manager) {
		ofono_manager_free(od->manager);
		od->manager = NULL;
	}

	telephony_service_sim_count_changed_notify(od->service, 0);
}

int ofono_probe(struct telephony_service *service)
{
	struct ofono_data *data;

	data = g_try_new0(struct ofono_data, 1);
	if (!data)
		return -ENOMEM;

	telephony_service_set_data(service, data);
	data->service = service;
	data->sims = g_ptr_array_new_with_free_func(sim_data_free);

	data->service_watch = g_bus_watch_name(G_BUS_TYPE_SYSTEM, "org.ofono", G_BUS_NAME_WATCHER_FLAGS_NONE,
					 service_appeared_cb, service_vanished_cb, data, NULL);

	return 0;
}

void ofono_remove(struct telephony_service *service)
{
	struct ofono_data *data = 0;

	data = telephony_service_get_data(service);
	if (!data)
		return;

	g_bus_unwatch_name(data->service_watch);

	g_ptr_array_free(data->sims, TRUE);

	if (data->manager) {
		ofono_manager_free(data->manager);
		data->manager = NULL;
	}

	g_free(data);

	telephony_service_set_data(service, NULL);
}

struct telephony_driver ofono_telephony_driver = {
	.probe =		ofono_probe,
	.remove =		ofono_remove,
	.platform_query		= ofono_platform_query,
	.sim_info_query = ofono_sim_info_query,
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
