/* @@@LICENSE
*
* Copyright (C) 2026 Herman van Hazendonk <github.com@herrie.org>
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
#include "utils.h"
#include "luna_service_utils.h"

/**
 * @brief List the SIM slots the device has, together with the state of each.
 *
 * JSON format:
 *  request:
 *    { ["subscribe": <boolean>] }
 *  response:
 *    {
 *      "returnValue": true,
 *      "simCount": <integer>,
 *      "sims": [ { "simId": <integer>, "present": <boolean>, "name": <string>,
 *                  "iccid": <string>, "imsi": <string>, "msisdn": <string>,
 *                  "operatorName": <string>, "simStatus": <string>,
 *                  "powered": <boolean>, "ready": <boolean>, "bars": <integer>,
 *                  "networkRegistered": <boolean>, "dataRegistered": <boolean>,
 *                  "defaultForVoice": <boolean>, "defaultForSms": <boolean>,
 *                  "defaultForData": <boolean> }, ... ],
 *      "defaultSim": { "voice": <integer>, "sms": <integer>, "data": <integer> }
 *    }
 **/
bool _service_sim_list_query_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct telephony_service *service = user_data;
	jvalue_ref reply_obj = NULL;
	bool subscribed = false;

	subscribed = luna_service_check_for_subscription_with_key(handle, message, "/simListQuery");

	reply_obj = jobject_create();
	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("subscribed"), jboolean_create(subscribed));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("simCount"),
				jnumber_create_i32(telephony_service_get_sim_count(service)));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("sims"), telephony_service_build_sim_list(service));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("defaultSim"), telephony_service_build_default_sims(service));

	if (!luna_service_message_validate_and_send(handle, message, reply_obj))
		luna_service_message_reply_error_internal(handle, message);

	j_release(&reply_obj);

	return true;
}

/**
 * @brief Query which slot is currently the default for each role.
 *
 * JSON format:
 *  request:
 *    { ["subscribe": <boolean>] }
 *  response:
 *    {
 *      "returnValue": true,
 *      "defaultSim": { "voice": <integer>, "sms": <integer>, "data": <integer> }
 *    }
 **/
bool _service_default_sim_query_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct telephony_service *service = user_data;
	jvalue_ref reply_obj = NULL;
	bool subscribed = false;

	subscribed = luna_service_check_for_subscription_with_key(handle, message, "/defaultSimQuery");

	reply_obj = jobject_create();
	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("subscribed"), jboolean_create(subscribed));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("defaultSim"), telephony_service_build_default_sims(service));

	if (!luna_service_message_validate_and_send(handle, message, reply_obj))
		luna_service_message_reply_error_internal(handle, message);

	j_release(&reply_obj);

	return true;
}

/**
 * @brief Choose the slot to use for outgoing calls, outgoing messages and/or data.
 *
 * At least one of the three roles has to be given. The selection is remembered
 * by ICCID so that it follows the card rather than the slot.
 *
 * JSON format:
 *  request:
 *    { ["voice": <integer>], ["sms": <integer>], ["data": <integer>] }
 *  response:
 *    {
 *      "returnValue": <boolean>,
 *      "defaultSim": { "voice": <integer>, "sms": <integer>, "data": <integer> }
 *    }
 **/
bool _service_default_sim_set_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct telephony_service *service = user_data;
	jvalue_ref parsed_obj = NULL;
	jvalue_ref reply_obj = NULL;
	jvalue_ref value_obj = NULL;
	const char *payload;
	int role;
	int sim_id;
	bool any_set = false;

	payload = LSMessageGetPayload(message);
	parsed_obj = luna_service_message_parse_and_validate(payload);
	if (jis_null(parsed_obj)) {
		luna_service_message_reply_error_bad_json(handle, message);
		return true;
	}

	for (role = 0; role < TELEPHONY_SIM_ROLE_MAX; role++) {
		if (!jobject_get_exists(parsed_obj, j_cstr_to_buffer(telephony_sim_role_to_string(role)), &value_obj))
			continue;

		if (!jis_number(value_obj) || jnumber_get_i32(value_obj, &sim_id) != 0) {
			luna_service_message_reply_error_invalid_params(handle, message);
			goto cleanup;
		}

		if (!telephony_service_set_default_sim(service, role, sim_id)) {
			luna_service_message_reply_custom_error(handle, message, "Unknown SIM slot");
			goto cleanup;
		}

		any_set = true;
	}

	if (!any_set) {
		luna_service_message_reply_error_invalid_params(handle, message);
		goto cleanup;
	}

	/* Everything that keyed off the old default has to be told about the swap. */
	telephony_service_repost_sim_list(service);

	reply_obj = jobject_create();
	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("defaultSim"), telephony_service_build_default_sims(service));

	if (!luna_service_message_validate_and_send(handle, message, reply_obj))
		luna_service_message_reply_error_internal(handle, message);

	j_release(&reply_obj);

cleanup:
	j_release(&parsed_obj);

	return true;
}

/**
 * @brief Give a slot a user visible label, e.g. "Work" or "Private".
 *
 * JSON format:
 *  request:
 *    { "simId": <integer>, "name": <string> }
 *  response:
 *    { "returnValue": <boolean> }
 **/
bool _service_sim_name_set_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct telephony_service *service = user_data;
	jvalue_ref parsed_obj = NULL;
	jvalue_ref name_obj = NULL;
	raw_buffer name_buf;
	const char *payload;
	char *name = NULL;
	int sim_id;
	bool explicit_sim = false;

	payload = LSMessageGetPayload(message);
	parsed_obj = luna_service_message_parse_and_validate(payload);
	if (jis_null(parsed_obj)) {
		luna_service_message_reply_error_bad_json(handle, message);
		return true;
	}

	sim_id = telephony_service_resolve_sim_id(service, parsed_obj, TELEPHONY_SIM_ROLE_VOICE, &explicit_sim);
	if (sim_id < 0) {
		luna_service_message_reply_custom_error(handle, message, "Unknown SIM slot");
		goto cleanup;
	}

	if (!jobject_get_exists(parsed_obj, J_CSTR_TO_BUF("name"), &name_obj) || !jis_string(name_obj)) {
		luna_service_message_reply_error_invalid_params(handle, message);
		goto cleanup;
	}

	name_buf = jstring_get_fast(name_obj);
	name = g_strndup(name_buf.m_str, name_buf.m_len);

	if (!telephony_service_set_sim_name(service, sim_id, name)) {
		luna_service_message_reply_error_internal(handle, message);
		goto cleanup;
	}

	telephony_service_repost_sim_list(service);

	luna_service_message_reply_success(handle, message);

cleanup:
	g_free(name);
	j_release(&parsed_obj);

	return true;
}

// vim:ts=4:sw=4:noexpandtab
