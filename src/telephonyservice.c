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
#include "telephonyservice.h"
#include "telephonyservice_internal.h"
#include "telephonyservice_sms.h"
#include "wanservice.h"
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
bool _service_dial_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_answer_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_ignore_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_hangup_cb(LSHandle *handle, LSMessage *message, void *user_data);

bool _service_internal_send_sms_from_db_cb(LSHandle *handle, LSMessage *message, void *user_data);

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
	{ "dial", _service_dial_cb },
	{ "answer", _service_answer_cb },
	{ "ignore", _service_ignore_cb },
	{ "hangup", _service_hangup_cb },
	{ "sendSmsFromDb", _service_internal_send_sms_from_db_cb },
	{ "simListQuery", _service_sim_list_query_cb },
	{ "defaultSimQuery", _service_default_sim_query_cb },
	{ "defaultSimSet", _service_default_sim_set_cb },
	{ "simNameSet", _service_sim_name_set_cb },
	{ 0, 0 }
};

/* ------------------------------------------------------------------------
 * Settings helpers
 * ------------------------------------------------------------------------ */

static const char* sim_role_key(enum telephony_sim_role role)
{
	switch (role) {
	case TELEPHONY_SIM_ROLE_VOICE:
		return "voice";
	case TELEPHONY_SIM_ROLE_SMS:
		return "sms";
	case TELEPHONY_SIM_ROLE_DATA:
		return "data";
	default:
		break;
	}

	return "voice";
}

static jvalue_ref load_setting_object(enum telephony_settings_type type)
{
	char *setting_value = NULL;
	jvalue_ref parsed_obj = NULL;

	setting_value = telephony_settings_load(type);
	if (setting_value == NULL)
		return jinvalid();

	parsed_obj = luna_service_message_parse_and_validate(setting_value);
	g_free(setting_value);

	if (jis_null(parsed_obj) || !jis_object(parsed_obj)) {
		if (!jis_null(parsed_obj))
			j_release(&parsed_obj);
		return jinvalid();
	}

	return parsed_obj;
}

static void store_setting_object(enum telephony_settings_type type, jvalue_ref obj)
{
	jschema_ref schema = NULL;

	schema = jschema_parse(j_cstr_to_buffer("{}"), DOMOPT_NOOPT, NULL);
	if (!schema)
		return;

	telephony_settings_store(type, jvalue_tostring(obj, schema));

	jschema_release(&schema);
}

/**
 * The device wide power state as stored by legacy (single SIM) versions. It is
 * still honoured as the fallback for slots which have no per-slot entry yet.
 */
static bool retrieve_power_state_from_settings(void)
{
	jvalue_ref parsed_obj = jinvalid();
	jvalue_ref state_obj = NULL;
	bool power_state = true;

	parsed_obj = load_setting_object(TELEPHONY_SETTINGS_TYPE_POWER_STATE);
	if (jis_null(parsed_obj))
		return true;

	if (jobject_get_exists(parsed_obj, J_CSTR_TO_BUF("state"), &state_obj))
		jboolean_get(state_obj, &power_state);

	j_release(&parsed_obj);

	return power_state;
}

static bool retrieve_power_state_for_sim(int sim_id)
{
	jvalue_ref parsed_obj = jinvalid();
	jvalue_ref state_obj = NULL;
	bool power_state;
	char key[16];

	power_state = retrieve_power_state_from_settings();

	parsed_obj = load_setting_object(TELEPHONY_SETTINGS_TYPE_SIM_POWER_STATE);
	if (jis_null(parsed_obj))
		return power_state;

	snprintf(key, sizeof(key), "%d", sim_id);

	if (jobject_get_exists(parsed_obj, j_cstr_to_buffer(key), &state_obj))
		jboolean_get(state_obj, &power_state);

	j_release(&parsed_obj);

	return power_state;
}

void telephony_service_store_power_state_for_sim(int sim_id, bool power)
{
	jvalue_ref parsed_obj = jinvalid();
	char key[16];

	parsed_obj = load_setting_object(TELEPHONY_SETTINGS_TYPE_SIM_POWER_STATE);
	if (jis_null(parsed_obj))
		parsed_obj = jobject_create();

	snprintf(key, sizeof(key), "%d", sim_id);
	jobject_put(parsed_obj, jstring_create(key), jboolean_create(power));

	store_setting_object(TELEPHONY_SETTINGS_TYPE_SIM_POWER_STATE, parsed_obj);

	j_release(&parsed_obj);
}

static void load_default_sim_preferences(struct telephony_service *service)
{
	jvalue_ref parsed_obj = jinvalid();
	jvalue_ref value_obj = NULL;
	raw_buffer buf;
	int role;

	parsed_obj = load_setting_object(TELEPHONY_SETTINGS_TYPE_DEFAULT_SIM);
	if (jis_null(parsed_obj))
		return;

	for (role = 0; role < TELEPHONY_SIM_ROLE_MAX; role++) {
		if (!jobject_get_exists(parsed_obj, j_cstr_to_buffer(sim_role_key(role)), &value_obj))
			continue;

		if (!jis_string(value_obj))
			continue;

		buf = jstring_get_fast(value_obj);
		if (buf.m_str && buf.m_len > 0) {
			g_free(service->default_sim_iccid[role]);
			service->default_sim_iccid[role] = g_strndup(buf.m_str, buf.m_len);
		}
	}

	j_release(&parsed_obj);
}

static void store_default_sim_preferences(struct telephony_service *service)
{
	jvalue_ref obj = jobject_create();
	int role;

	for (role = 0; role < TELEPHONY_SIM_ROLE_MAX; role++) {
		if (!service->default_sim_iccid[role])
			continue;

		jobject_put(obj, jstring_create(sim_role_key(role)),
					jstring_create(service->default_sim_iccid[role]));
	}

	store_setting_object(TELEPHONY_SETTINGS_TYPE_DEFAULT_SIM, obj);

	j_release(&obj);
}

static void load_sim_names(struct telephony_service *service)
{
	jvalue_ref parsed_obj = jinvalid();
	jobject_iter iter;
	jobject_key_value key_value;
	raw_buffer key_buf, value_buf;

	parsed_obj = load_setting_object(TELEPHONY_SETTINGS_TYPE_SIM_NAMES);
	if (jis_null(parsed_obj))
		return;

	if (!jobject_iter_init(&iter, parsed_obj)) {
		j_release(&parsed_obj);
		return;
	}

	while (jobject_iter_next(&iter, &key_value)) {
		if (!jis_string(key_value.value))
			continue;

		key_buf = jstring_get_fast(key_value.key);
		value_buf = jstring_get_fast(key_value.value);

		if (!key_buf.m_str || !value_buf.m_str)
			continue;

		g_hash_table_replace(service->sim_names,
							 g_strndup(key_buf.m_str, key_buf.m_len),
							 g_strndup(value_buf.m_str, value_buf.m_len));
	}

	j_release(&parsed_obj);
}

static void store_sim_names(struct telephony_service *service)
{
	jvalue_ref obj = jobject_create();
	GHashTableIter iter;
	gpointer key, value;

	g_hash_table_iter_init(&iter, service->sim_names);
	while (g_hash_table_iter_next(&iter, &key, &value))
		jobject_put(obj, jstring_create((const char*) key), jstring_create((const char*) value));

	store_setting_object(TELEPHONY_SETTINGS_TYPE_SIM_NAMES, obj);

	j_release(&obj);
}

/* ------------------------------------------------------------------------
 * Per SIM state bookkeeping
 * ------------------------------------------------------------------------ */

struct telephony_sim_state* telephony_service_sim_state(struct telephony_service *service, int sim_id)
{
	if (!service || !service->sims)
		return NULL;

	if (sim_id < 0 || sim_id >= (int) service->sims->len)
		return NULL;

	return g_ptr_array_index(service->sims, sim_id);
}

int telephony_service_get_sim_count(struct telephony_service *service)
{
	if (!service || !service->sims)
		return 0;

	return (int) service->sims->len;
}

bool telephony_service_any_initialized(struct telephony_service *service)
{
	struct telephony_sim_state *sim;
	int n;

	for (n = 0; n < telephony_service_get_sim_count(service); n++) {
		sim = telephony_service_sim_state(service, n);
		if (sim && sim->initialized)
			return true;
	}

	return false;
}

static struct telephony_sim_state* sim_state_new(int sim_id)
{
	struct telephony_sim_state *sim;

	sim = g_new0(struct telephony_sim_state, 1);
	sim->sim_id = sim_id;
	sim->sim_status = TELEPHONY_SIM_STATUS_SIM_NOT_FOUND;
	sim->network_state = TELEPHONY_NETWORK_STATE_NO_SERVICE;
	sim->network_registration = TELEPHONY_NETWORK_REGISTRATION_NO_SERVICE;
	sim->signal_bars = 0;

	return sim;
}

static void sim_state_free(gpointer data)
{
	struct telephony_sim_state *sim = data;

	if (!sim)
		return;

	g_free(sim->iccid);
	g_free(sim->imsi);
	g_free(sim->msisdn);
	g_free(sim->operator_name);
	g_free(sim->modem_path);
	g_free(sim->name);
	g_free(sim);
}

/* Fallback label when the user did not name the slot: "SIM 1", "SIM 2", ... */
static const char* sim_display_name(struct telephony_sim_state *sim, char *buf, size_t buf_len)
{
	if (sim->name && strlen(sim->name) > 0)
		return sim->name;

	snprintf(buf, buf_len, "SIM %d", sim->sim_id + 1);

	return buf;
}

/**
 * Work out which slot should serve each role.
 *
 * A stored ICCID always wins so that the user's choice follows the physical
 * card. Otherwise we fall back to the first slot holding a usable SIM, and only
 * then to slot 0, which is what a single SIM device ends up with.
 */
static bool recompute_default_sims(struct telephony_service *service)
{
	struct telephony_sim_state *sim;
	int role, n;
	int previous;
	bool changed = false;

	for (role = 0; role < TELEPHONY_SIM_ROLE_MAX; role++) {
		previous = service->default_sim[role];
		service->default_sim[role] = -1;

		if (service->default_sim_iccid[role]) {
			for (n = 0; n < telephony_service_get_sim_count(service); n++) {
				sim = telephony_service_sim_state(service, n);
				if (sim && sim->iccid && g_strcmp0(sim->iccid, service->default_sim_iccid[role]) == 0) {
					service->default_sim[role] = n;
					break;
				}
			}
		}

		if (service->default_sim[role] < 0) {
			for (n = 0; n < telephony_service_get_sim_count(service); n++) {
				sim = telephony_service_sim_state(service, n);
				if (sim && sim->present) {
					service->default_sim[role] = n;
					break;
				}
			}
		}

		if (service->default_sim[role] < 0 && telephony_service_get_sim_count(service) > 0)
			service->default_sim[role] = 0;

		if (service->default_sim[role] != previous) {
			changed = true;

			if (role == TELEPHONY_SIM_ROLE_DATA)
				wan_service_set_data_sim(service->default_sim[role]);
		}
	}

	return changed;
}

int telephony_service_get_default_sim(struct telephony_service *service, enum telephony_sim_role role)
{
	if (!service || role < 0 || role >= TELEPHONY_SIM_ROLE_MAX)
		return -1;

	return service->default_sim[role];
}

bool telephony_service_set_default_sim(struct telephony_service *service, enum telephony_sim_role role, int sim_id)
{
	struct telephony_sim_state *sim;

	if (!service || role < 0 || role >= TELEPHONY_SIM_ROLE_MAX)
		return false;

	sim = telephony_service_sim_state(service, sim_id);
	if (!sim)
		return false;

	service->default_sim[role] = sim_id;

	g_free(service->default_sim_iccid[role]);
	service->default_sim_iccid[role] = sim->iccid ? g_strdup(sim->iccid) : NULL;

	store_default_sim_preferences(service);

	if (role == TELEPHONY_SIM_ROLE_DATA)
		wan_service_set_data_sim(sim_id);

	return true;
}

bool telephony_service_set_sim_name(struct telephony_service *service, int sim_id, const char *name)
{
	struct telephony_sim_state *sim;

	sim = telephony_service_sim_state(service, sim_id);
	if (!sim)
		return false;

	g_free(sim->name);
	sim->name = (name && strlen(name) > 0) ? g_strdup(name) : NULL;

	/* Names are remembered per card, not per slot. Without an ICCID we can
	 * still show the label for this session but cannot persist it. */
	if (sim->iccid) {
		if (sim->name)
			g_hash_table_replace(service->sim_names, g_strdup(sim->iccid), g_strdup(sim->name));
		else
			g_hash_table_remove(service->sim_names, sim->iccid);

		store_sim_names(service);
	}

	return true;
}

/* ------------------------------------------------------------------------
 * Subscription plumbing
 * ------------------------------------------------------------------------ */

void telephony_service_add_sim_id(jvalue_ref reply_obj, int sim_id)
{
	if (jis_null(reply_obj))
		return;

	jobject_put(reply_obj, J_CSTR_TO_JVAL("simId"), jnumber_create_i32(sim_id));
}

bool telephony_service_process_sim_subscription(struct telephony_service *service, LSHandle *handle,
                                                LSMessage *message, const char *method, int sim_id,
                                                bool explicit_sim)
{
	char *key = NULL;
	bool subscribed = false;

	if (explicit_sim)
		key = g_strdup_printf("/%s/%d", method, sim_id);
	else
		key = g_strdup_printf("/%s", method);

	subscribed = luna_service_check_for_subscription_with_key(handle, message, key);

	g_free(key);

	return subscribed;
}

void telephony_service_post_subscription(struct telephony_service *service, const char *method,
                                         jvalue_ref reply_obj)
{
	char *key = NULL;

	if (!service)
		return;

	key = g_strdup_printf("/%s", method);

	if (service->palmHandle)
		luna_service_post_subscription_with_key(service->palmHandle, key, reply_obj);
	if (service->webosHandle)
		luna_service_post_subscription_with_key(service->webosHandle, key, reply_obj);

	g_free(key);
}

void telephony_service_post_sim_subscription(struct telephony_service *service, const char *method,
                                             int sim_id, enum telephony_sim_role role, jvalue_ref reply_obj)
{
	char *key = NULL;

	if (!service)
		return;

	key = g_strdup_printf("/%s/%d", method, sim_id);

	if (service->palmHandle)
		luna_service_post_subscription_with_key(service->palmHandle, key, reply_obj);
	if (service->webosHandle)
		luna_service_post_subscription_with_key(service->webosHandle, key, reply_obj);

	g_free(key);

	/* Clients from before dual SIM support subscribed without a simId and
	 * expect to hear about whichever SIM currently serves the role. */
	if (telephony_service_get_default_sim(service, role) == sim_id)
		telephony_service_post_subscription(service, method, reply_obj);
}

/* ------------------------------------------------------------------------
 * SIM list payloads
 * ------------------------------------------------------------------------ */

jvalue_ref telephony_service_build_sim_list(struct telephony_service *service)
{
	jvalue_ref sim_list_obj = jarray_create(NULL);
	jvalue_ref sim_obj = NULL;
	struct telephony_sim_state *sim;
	char name_buf[32];
	int n, role;

	for (n = 0; n < telephony_service_get_sim_count(service); n++) {
		sim = telephony_service_sim_state(service, n);
		if (!sim)
			continue;

		sim_obj = jobject_create();

		jobject_put(sim_obj, J_CSTR_TO_JVAL("simId"), jnumber_create_i32(sim->sim_id));
		jobject_put(sim_obj, J_CSTR_TO_JVAL("present"), jboolean_create(sim->present));
		jobject_put(sim_obj, J_CSTR_TO_JVAL("name"),
					jstring_create(sim_display_name(sim, name_buf, sizeof(name_buf))));
		jobject_put(sim_obj, J_CSTR_TO_JVAL("simStatus"),
					jstring_create(telephony_sim_status_to_string(sim->sim_status)));
		jobject_put(sim_obj, J_CSTR_TO_JVAL("powered"), jboolean_create(sim->powered));
		jobject_put(sim_obj, J_CSTR_TO_JVAL("ready"), jboolean_create(sim->initialized));
		jobject_put(sim_obj, J_CSTR_TO_JVAL("networkRegistered"), jboolean_create(sim->network_registered));
		jobject_put(sim_obj, J_CSTR_TO_JVAL("state"),
					jstring_create(telephony_network_state_to_string(sim->network_state)));
		jobject_put(sim_obj, J_CSTR_TO_JVAL("registration"),
					jstring_create(telephony_network_registration_to_string(sim->network_registration)));
		jobject_put(sim_obj, J_CSTR_TO_JVAL("dataRegistered"), jboolean_create(sim->data_registered));
		jobject_put(sim_obj, J_CSTR_TO_JVAL("bars"), jnumber_create_i32(sim->signal_bars));

		if (sim->iccid)
			jobject_put(sim_obj, J_CSTR_TO_JVAL("iccid"), jstring_create(sim->iccid));
		if (sim->imsi)
			jobject_put(sim_obj, J_CSTR_TO_JVAL("imsi"), jstring_create(sim->imsi));
		if (sim->msisdn)
			jobject_put(sim_obj, J_CSTR_TO_JVAL("msisdn"), jstring_create(sim->msisdn));
		if (sim->operator_name)
			jobject_put(sim_obj, J_CSTR_TO_JVAL("operatorName"), jstring_create(sim->operator_name));
		if (sim->modem_path)
			jobject_put(sim_obj, J_CSTR_TO_JVAL("modemPath"), jstring_create(sim->modem_path));

		for (role = 0; role < TELEPHONY_SIM_ROLE_MAX; role++) {
			char *field = g_strdup_printf("defaultFor%c%s",
										  g_ascii_toupper(sim_role_key(role)[0]),
										  sim_role_key(role) + 1);
			jobject_put(sim_obj, jstring_create(field),
						jboolean_create(service->default_sim[role] == sim->sim_id));
			g_free(field);
		}

		jarray_append(sim_list_obj, sim_obj);
	}

	return sim_list_obj;
}

jvalue_ref telephony_service_build_default_sims(struct telephony_service *service)
{
	jvalue_ref obj = jobject_create();
	int role;

	for (role = 0; role < TELEPHONY_SIM_ROLE_MAX; role++)
		jobject_put(obj, jstring_create(sim_role_key(role)), jnumber_create_i32(service->default_sim[role]));

	return obj;
}

void telephony_service_repost_sim_list(struct telephony_service *service)
{
	jvalue_ref reply_obj = NULL;

	if (!service)
		return;

	reply_obj = jobject_create();
	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("simCount"),
				jnumber_create_i32(telephony_service_get_sim_count(service)));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("sims"), telephony_service_build_sim_list(service));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("defaultSim"), telephony_service_build_default_sims(service));

	telephony_service_post_subscription(service, "simListQuery", reply_obj);

	j_release(&reply_obj);

	reply_obj = jobject_create();
	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("defaultSim"), telephony_service_build_default_sims(service));

	telephony_service_post_subscription(service, "defaultSimQuery", reply_obj);

	j_release(&reply_obj);
}

/* ------------------------------------------------------------------------
 * Request parameter handling
 * ------------------------------------------------------------------------ */

int telephony_service_resolve_sim_id(struct telephony_service *service, jvalue_ref parsed_obj,
                                     enum telephony_sim_role role, bool *explicit_sim)
{
	jvalue_ref sim_id_obj = NULL;
	int sim_id = 0;

	if (explicit_sim)
		*explicit_sim = false;

	if (!jis_null(parsed_obj) && jis_object(parsed_obj) &&
		jobject_get_exists(parsed_obj, J_CSTR_TO_BUF("simId"), &sim_id_obj) &&
		jis_number(sim_id_obj) &&
		jnumber_get_i32(sim_id_obj, &sim_id) == 0) {

		if (explicit_sim)
			*explicit_sim = true;

		/* An explicitly named slot which does not exist is an error rather
		 * than something we silently redirect to the default SIM. */
		if (!telephony_service_sim_state(service, sim_id))
			return -1;

		return sim_id;
	}

	return telephony_service_get_default_sim(service, role);
}

struct luna_service_req_data* telephony_service_begin_parsed_request(struct telephony_service *service,
                                                                     LSHandle *handle, LSMessage *message,
                                                                     jvalue_ref parsed_obj,
                                                                     enum telephony_sim_role role,
                                                                     bool require_initialized)
{
	struct telephony_sim_state *sim = NULL;
	struct luna_service_req_data *req_data = NULL;
	bool explicit_sim = false;
	int sim_id;

	sim_id = telephony_service_resolve_sim_id(service, parsed_obj, role, &explicit_sim);

	sim = telephony_service_sim_state(service, sim_id);
	if (!sim) {
		luna_service_message_reply_custom_error(handle, message,
			explicit_sim ? "Unknown SIM slot" : "No SIM slot available");
		return NULL;
	}

	if (require_initialized && !sim->initialized) {
		luna_service_message_reply_custom_error(handle, message, "Backend not initialized");
		return NULL;
	}

	req_data = luna_service_req_data_new(handle, message);
	req_data->sim_id = sim_id;
	req_data->user_data = service;

	return req_data;
}

struct luna_service_req_data* telephony_service_begin_request(struct telephony_service *service,
                                                              LSHandle *handle, LSMessage *message,
                                                              const char *method,
                                                              enum telephony_sim_role role,
                                                              bool require_initialized,
                                                              bool subscribable,
                                                              jvalue_ref *parsed_obj)
{
	struct telephony_sim_state *sim = NULL;
	struct luna_service_req_data *req_data = NULL;
	jvalue_ref parsed = jinvalid();
	const char *payload;
	bool explicit_sim = false;
	int sim_id;

	payload = LSMessageGetPayload(message);
	parsed = luna_service_message_parse_and_validate(payload);

	sim_id = telephony_service_resolve_sim_id(service, parsed, role, &explicit_sim);

	sim = telephony_service_sim_state(service, sim_id);
	if (!sim) {
		luna_service_message_reply_custom_error(handle, message,
			explicit_sim ? "Unknown SIM slot" : "No SIM slot available");
		goto failed;
	}

	if (require_initialized && !sim->initialized) {
		luna_service_message_reply_custom_error(handle, message, "Backend not initialized");
		goto failed;
	}

	req_data = luna_service_req_data_new(handle, message);
	req_data->sim_id = sim_id;
	req_data->user_data = service;

	if (subscribable)
		req_data->subscribed = telephony_service_process_sim_subscription(service, handle, message,
												method, sim_id, explicit_sim);

	if (parsed_obj)
		*parsed_obj = parsed;
	else if (!jis_null(parsed))
		j_release(&parsed);

	return req_data;

failed:
	if (!jis_null(parsed))
		j_release(&parsed);

	if (parsed_obj)
		*parsed_obj = jinvalid();

	return NULL;
}

/* ------------------------------------------------------------------------
 * Service lifecycle
 * ------------------------------------------------------------------------ */

int _service_initial_power_set_finish(const struct telephony_error *error, void *data)
{
	return 0;
}

static int configure_sim(struct telephony_service *service, int sim_id)
{
	bool power_state = true;

	if (!service->driver || !service->driver->power_set) {
		g_warning("API method powerSet not available for setting initial power mode");
		return -EINVAL;
	}

	power_state = retrieve_power_state_for_sim(sim_id);

	service->driver->power_set(service, sim_id, power_state, _service_initial_power_set_finish, service);

	return 0;
}

void telephony_service_sim_count_changed_notify(struct telephony_service *service, int sim_count)
{
	struct telephony_sim_state *sim;
	int n;

	if (!service)
		return;

	if (sim_count < 0)
		sim_count = 0;

	if (sim_count > TELEPHONY_MAX_SIMS) {
		g_warning("Reporting only the first %d of %d modems", TELEPHONY_MAX_SIMS, sim_count);
		sim_count = TELEPHONY_MAX_SIMS;
	}

	if (sim_count == telephony_service_get_sim_count(service))
		return;

	g_message("Number of available SIM slots changed to %d", sim_count);

	while (telephony_service_get_sim_count(service) > sim_count)
		g_ptr_array_remove_index(service->sims, service->sims->len - 1);

	while (telephony_service_get_sim_count(service) < sim_count) {
		n = telephony_service_get_sim_count(service);
		sim = sim_state_new(n);
		g_ptr_array_add(service->sims, sim);
	}

	recompute_default_sims(service);
	telephony_service_repost_sim_list(service);
}

void telephony_service_sim_info_changed_notify(struct telephony_service *service, int sim_id,
                                               struct telephony_sim_info *info)
{
	struct telephony_sim_state *sim;
	const char *stored_name = NULL;

	sim = telephony_service_sim_state(service, sim_id);
	if (!sim || !info)
		return;

	sim->present = info->present;
	sim->sim_status = info->sim_status;
	sim->powered = info->powered;

	g_free(sim->iccid);
	sim->iccid = info->iccid ? g_strdup(info->iccid) : NULL;

	g_free(sim->imsi);
	sim->imsi = info->imsi ? g_strdup(info->imsi) : NULL;

	g_free(sim->msisdn);
	sim->msisdn = info->msisdn ? g_strdup(info->msisdn) : NULL;

	g_free(sim->operator_name);
	sim->operator_name = info->operator_name ? g_strdup(info->operator_name) : NULL;

	g_free(sim->modem_path);
	sim->modem_path = info->modem_path ? g_strdup(info->modem_path) : NULL;

	if (sim->iccid) {
		stored_name = g_hash_table_lookup(service->sim_names, sim->iccid);
		if (stored_name) {
			g_free(sim->name);
			sim->name = g_strdup(stored_name);
		}
	}

	recompute_default_sims(service);
	telephony_service_repost_sim_list(service);
}

struct telephony_service* telephony_service_create()
{
	struct telephony_service *service;
	int role;

	if (g_driver_list == NULL) {
		g_message("Can't create telephony servie as no suitable driver is available");
		return NULL;
	}

	service = g_try_new0(struct telephony_service, 1);
	if (!service)
		return NULL;

	service->sims = g_ptr_array_new_with_free_func(sim_state_free);
	service->sim_names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

	for (role = 0; role < TELEPHONY_SIM_ROLE_MAX; role++)
		service->default_sim[role] = -1;

	load_default_sim_preferences(service);
	load_sim_names(service);

	/* take first driver until we have some machanism to determine the best driver */
	service->driver = g_driver_list->data;

	if (service->driver->probe(service) < 0) {
		g_ptr_array_free(service->sims, TRUE);
		g_hash_table_destroy(service->sim_names);
		g_free(service);
		return NULL;
	}

	LSError error;

	LSErrorInit(&error);

	if (!LSRegister("com.palm.telephony", &service->palmHandle, &error)) {
		g_critical("Failed to initialize the Luna Palm service: %s", error.message);
		LSErrorFree(&error);
		goto failed;
	}

	if (!LSGmainAttach(service->palmHandle, event_loop, &error)) {
		g_critical("Failed to attach to glib mainloop for palm service: %s", error.message);
		LSErrorFree(&error);
		goto failed;
	}

	if (!LSRegisterCategory(service->palmHandle, "/", _telephony_service_methods, NULL, NULL, &error)) {
		g_warning("Could not register palm service category");
		LSErrorFree(&error);
		goto failed;
	}

	if (!LSCategorySetData(service->palmHandle, "/", service, &error)) {
		g_warning("Could not set data for palm service category");
		LSErrorFree(&error);
		goto failed;
	}

	if (!LSRegister("com.webos.service.telephony", &service->webosHandle, &error)) {
		g_critical("Failed to initialize the Luna webOS service: %s", error.message);
		LSErrorFree(&error);
		goto failed;
	}

	if (!LSGmainAttach(service->webosHandle, event_loop, &error)) {
		g_critical("Failed to attach to glib mainloop for webos service: %s", error.message);
		LSErrorFree(&error);
		goto failed;
	}

	if (!LSRegisterCategory(service->webosHandle, "/", _telephony_service_methods, NULL, NULL, &error)) {
		g_warning("Could not register webos service category");
		LSErrorFree(&error);
		goto failed;
	}

	if (!LSCategorySetData(service->webosHandle, "/", service, &error)) {
		g_warning("Could not set data for webos service category");
		LSErrorFree(&error);
		goto failed;
	}

	telephonyservice_sms_setup(service);

	return service;

failed:
	g_ptr_array_free(service->sims, TRUE);
	g_hash_table_destroy(service->sim_names);
	g_free(service);
	return NULL;
}

void telephony_service_free(struct telephony_service *service)
{
	LSError error;
	int role;

	LSErrorInit(&error);

	if (service->palmHandle != NULL &&
		LSUnregister(service->palmHandle, &error) < 0) {
		g_critical("Could not unregister palm service: %s", error.message);
		LSErrorFree(&error);
	}

	if (service->webosHandle != NULL &&
		LSUnregister(service->webosHandle, &error) < 0) {
		g_critical("Could not unregister webos service: %s", error.message);
		LSErrorFree(&error);
	}

	if (service->driver) {
		service->driver->remove(service);
		service->driver = NULL;
	}

	for (role = 0; role < TELEPHONY_SIM_ROLE_MAX; role++)
		g_free(service->default_sim_iccid[role]);

	if (service->sims)
		g_ptr_array_free(service->sims, TRUE);

	if (service->sim_names)
		g_hash_table_destroy(service->sim_names);

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

void telephony_service_availability_changed_notify(struct telephony_service *service, int sim_id, bool available)
{
	struct telephony_sim_state *sim;

	sim = telephony_service_sim_state(service, sim_id);
	if (!sim)
		return;

	g_debug("Availability of SIM %d changed to: %s", sim_id, available ? "available" : "not available");

	if (!sim->initialized && available) {
		if (configure_sim(service, sim_id) < 0) {
			g_critical("Could not configure SIM %d", sim_id);
			return;
		}
	}

	sim->initialized = available;

	if (!available) {
		sim->powered = false;
		sim->network_registered = false;
		sim->data_registered = false;
		sim->signal_bars = 0;
		sim->network_state = TELEPHONY_NETWORK_STATE_NO_SERVICE;
		sim->network_registration = TELEPHONY_NETWORK_REGISTRATION_NO_SERVICE;
	}

	telephony_service_repost_sim_list(service);
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
