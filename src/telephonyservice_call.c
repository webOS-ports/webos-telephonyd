/* @@@LICENSE
*
* Copyright (c) 2013 Simon Busch <morphis@gravedo.de>
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

#include "telephonydriver.h"
#include "telephonyservice_internal.h"
#include "utils.h"
#include "luna_service_utils.h"

bool _service_dial_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct telephony_service *service = user_data;
	struct luna_service_req_data *req_data = NULL;
	jvalue_ref parsed_obj = NULL;
	jvalue_ref number_obj = NULL;
	jvalue_ref block_id_obj = NULL;
	const char *payload;
	bool block_id = false;
	raw_buffer number_buf;

	if (!service->initialized) {
		luna_service_message_reply_custom_error(handle, message, "Service not yet successfully initialized.");
		goto cleanup;
	}

	if (!service->driver || !service->driver->dial) {
		g_warning("No implementation available for service dial API method");
		luna_service_message_reply_error_not_implemented(handle, message);
		goto cleanup;
	}

	payload = LSMessageGetPayload(message);
	parsed_obj = luna_service_message_parse_and_validate(payload);
	if (jis_null(parsed_obj)) {
		luna_service_message_reply_error_bad_json(handle, message);
		goto cleanup;
	}

	if (!jobject_get_exists(parsed_obj, J_CSTR_TO_BUF("number"), &number_obj)) {
		luna_service_message_reply_error_bad_json(handle, message);
		goto cleanup;
	}

	if (!jobject_get_exists(parsed_obj, J_CSTR_TO_BUF("blockId"), &block_id_obj)) {
		luna_service_message_reply_error_bad_json(handle, message);
		goto cleanup;
	}

	number_buf = jstring_get(number_obj);
	jboolean_get(block_id_obj, &block_id);

	req_data = luna_service_req_data_new(handle, message);
	req_data->user_data = service;

	if (service->driver->dial(service, number_buf.m_str, block_id, telephonyservice_common_finish, req_data) < 0) {
		g_warning("Failed to process service dial request in our driver");
		luna_service_message_reply_custom_error(handle, message, "Failed create outgoing call");
		goto cleanup;
	}

	return true;

cleanup:
	if (!jis_null(parsed_obj))
		j_release(&parsed_obj);

	if (req_data)
		luna_service_req_data_free(req_data);

	return true;
}

bool _service_answer_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct telephony_service *service = user_data;
	struct luna_service_req_data *req_data = NULL;
	jvalue_ref parsed_obj = NULL;
	jvalue_ref id_obj = NULL;
	const char *payload;
	int call_id = 0;

	if (!service->initialized) {
		luna_service_message_reply_custom_error(handle, message, "Service not yet successfully initialized.");
		goto cleanup;
	}

	if (!service->driver || !service->driver->dial) {
		g_warning("No implementation available for service answer API method");
		luna_service_message_reply_error_not_implemented(handle, message);
		goto cleanup;
	}

	payload = LSMessageGetPayload(message);
	parsed_obj = luna_service_message_parse_and_validate(payload);
	if (jis_null(parsed_obj)) {
		luna_service_message_reply_error_bad_json(handle, message);
		goto cleanup;
	}

	if (!jobject_get_exists(parsed_obj, J_CSTR_TO_BUF("id"), &id_obj)) {
		luna_service_message_reply_error_bad_json(handle, message);
		goto cleanup;
	}

	jnumber_get_i32(id_obj, &call_id);

	req_data = luna_service_req_data_new(handle, message);
	req_data->user_data = service;

	if (service->driver->answer(service, call_id, telephonyservice_common_finish, req_data) < 0) {
		g_warning("Failed to process service answer request in our driver");
		luna_service_message_reply_custom_error(handle, message, "Failed answer incoming call");
		goto cleanup;
	}

	return true;

cleanup:
	if (!jis_null(parsed_obj))
		j_release(&parsed_obj);

	if (req_data)
		luna_service_req_data_free(req_data);

	return true;
}

bool _service_ignore_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct telephony_service *service = user_data;
	struct luna_service_req_data *req_data = NULL;
	jvalue_ref parsed_obj = NULL;
	jvalue_ref id_obj = NULL;
	const char *payload;
	int call_id = 0;

	if (!service->initialized) {
		luna_service_message_reply_custom_error(handle, message, "Service not yet successfully initialized.");
		goto cleanup;
	}

	if (!service->driver || !service->driver->dial) {
		g_warning("No implementation available for service ignore API method");
		luna_service_message_reply_error_not_implemented(handle, message);
		goto cleanup;
	}

	payload = LSMessageGetPayload(message);
	parsed_obj = luna_service_message_parse_and_validate(payload);
	if (jis_null(parsed_obj)) {
		luna_service_message_reply_error_bad_json(handle, message);
		goto cleanup;
	}

	if (!jobject_get_exists(parsed_obj, J_CSTR_TO_BUF("id"), &id_obj)) {
		luna_service_message_reply_error_bad_json(handle, message);
		goto cleanup;
	}

	jnumber_get_i32(id_obj, &call_id);

	req_data = luna_service_req_data_new(handle, message);
	req_data->user_data = service;

	if (service->driver->ignore(service, call_id, telephonyservice_common_finish, req_data) < 0) {
		g_warning("Failed to process service answer ignore in our driver");
		luna_service_message_reply_custom_error(handle, message, "Failed ignore incoming call");
		goto cleanup;
	}

	return true;

cleanup:
	if (!jis_null(parsed_obj))
		j_release(&parsed_obj);

	if (req_data)
		luna_service_req_data_free(req_data);

	return true;
}

bool _service_hangup_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct telephony_service *service = user_data;
	struct luna_service_req_data *req_data = NULL;
	jvalue_ref parsed_obj = NULL;
	jvalue_ref id_obj = NULL;
	const char *payload;
	int call_id = 0;

	if (!service->initialized) {
		luna_service_message_reply_custom_error(handle, message, "Service not yet successfully initialized.");
		goto cleanup;
	}

	if (!service->driver || !service->driver->dial) {
		g_warning("No implementation available for service hangup API method");
		luna_service_message_reply_error_not_implemented(handle, message);
		goto cleanup;
	}

	payload = LSMessageGetPayload(message);
	parsed_obj = luna_service_message_parse_and_validate(payload);
	if (jis_null(parsed_obj)) {
		luna_service_message_reply_error_bad_json(handle, message);
		goto cleanup;
	}

	if (!jobject_get_exists(parsed_obj, J_CSTR_TO_BUF("id"), &id_obj)) {
		luna_service_message_reply_error_bad_json(handle, message);
		goto cleanup;
	}

	jnumber_get_i32(id_obj, &call_id);

	req_data = luna_service_req_data_new(handle, message);
	req_data->user_data = service;

	if (service->driver->hangup(service, call_id, telephonyservice_common_finish, req_data) < 0) {
		g_warning("Failed to process service hangup request in our driver");
		luna_service_message_reply_custom_error(handle, message, "Failed to hang up active call");
		goto cleanup;
	}

	return true;

cleanup:
	if (!jis_null(parsed_obj))
		j_release(&parsed_obj);

	if (req_data)
		luna_service_req_data_free(req_data);

	return true;
}
