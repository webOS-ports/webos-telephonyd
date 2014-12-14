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
#include "telephonyservice_internal.h"
#include "telephonyservice.h"
#include "utils.h"
#include "luna_service_utils.h"

GQueue *tx_queue = 0;
guint tx_timeout = 0;
gboolean tx_active = FALSE;

static bool create_message_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	// FIXME parse response and handle results

	g_message("Successfully stored SMS in db8");

	return true;
}

void telephony_service_incoming_message_notify(struct telephony_service *service, struct telephony_message *message)
{
	// We store each message in the db8 database with the com.palm.smsmessage kind which has the following
	// structure:
	// {
	//   "messageText": <string>,
	//   "from": {
	//     "addr": <string> # plain number
	//   },
	//   "timestamp": <int>,
	//   "localTimestamp": <int>,
	//   "conversations": [ ] # to be added by the chat threader
	//   "flags": {
	//     "read": <boolean> # default to false
	//   },
	//   "folder": <string>, # inbox, outbox
	//   "serviceName": <string>, # sms
	//   "status": <string>, # successful
	//   "to": [ # only for outgoing
	//     {
	//       "_id": <string>, # db8 id
	//       "addr": <string>
	//     }
	//   ]
	// }

	jvalue_ref req_obj = NULL;
	jvalue_ref objects_obj = NULL;
	jvalue_ref message_obj = NULL;
	jvalue_ref flags_obj = NULL;
	jvalue_ref from_obj = NULL;

	req_obj = jobject_create();
	objects_obj = jarray_create(NULL);

	message_obj = jobject_create();

	flags_obj = jobject_create();
	jobject_put(flags_obj, J_CSTR_TO_JVAL("read"), jboolean_create(false));

	jobject_put(message_obj, J_CSTR_TO_JVAL("_kind"), jstring_create("com.palm.smsmessage:1"));
	jobject_put(message_obj, J_CSTR_TO_JVAL("flags"), flags_obj);
	jobject_put(message_obj, J_CSTR_TO_JVAL("folder"), jstring_create("inbox"));
	jobject_put(message_obj, J_CSTR_TO_JVAL("successful"), jstring_create("successful"));
	jobject_put(message_obj, J_CSTR_TO_JVAL("serviceName"), jstring_create("sms"));
	jobject_put(message_obj, J_CSTR_TO_JVAL("messageText"), jstring_create(message->text));
	jobject_put(message_obj, J_CSTR_TO_JVAL("localTimestamp"), jnumber_create_i64(message->local_sent_time));
	jobject_put(message_obj, J_CSTR_TO_JVAL("timestamp"), jnumber_create_i64(message->sent_time));

	from_obj = jobject_create();
	jobject_put(from_obj, J_CSTR_TO_JVAL("addr"), jstring_create(message->sender));

	jobject_put(message_obj, J_CSTR_TO_JVAL("from"), from_obj);

	jarray_append(objects_obj, message_obj);

	jobject_put(req_obj, J_CSTR_TO_JVAL("objects"), objects_obj);

	if (!luna_service_call_validate_and_send(service->private_service, "luna://com.palm.db/put", req_obj,
	                                         create_message_cb, service))
		g_warning("Failed to create db8 message object for incoming SMS message");

	j_release(&req_obj);
}

struct pending_sms {
	char *id;
	char *to;
	char *text;
};

static bool message_updated_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	// FIXME parse response and handle result correctly

	g_message("Message object successfully updated");

	return true;
}


static int send_msg_cb(const struct telephony_error* error, void *user_data)
{
	struct cb_data *cbd = user_data;
	struct telephony_service *service = cbd->data;
	struct pending_sms *msg = cbd->user;

	bool success = (error == NULL);

	jvalue_ref req_obj = 0;
	jvalue_ref objects_obj = 0;
	jvalue_ref msg_obj = 0;

	req_obj = jobject_create();
	objects_obj = jarray_create(NULL);

	msg_obj = jobject_create();
	jobject_put(msg_obj, J_CSTR_TO_JVAL("_id"), jstring_create(msg->id));
	jobject_put(msg_obj, J_CSTR_TO_JVAL("status"), jstring_create(success ? "successful" : "failed"));

	jobject_put(req_obj, J_CSTR_TO_JVAL("objects"), objects_obj);

	if (!luna_service_call_validate_and_send(service->private_service, "luna://com.palm.db/merge", req_obj,
	                                         message_updated_cb, service)) {
		g_warning("Failed to update message status");
		return 0;
	}

	/* FIXME eventually retry failed messages after some time again */

	g_free(msg->id);
	g_free(msg->to);
	g_free(msg->text);
	g_free(msg);

	g_free(cbd);

	j_release(&req_obj);

	/* now we can send the next message */
	tx_active = FALSE;

	return 0;
}

static gboolean tx_timeout_cb(gpointer user_data);

static gboolean restart_tx_queue_cb(gpointer user_data)
{
	tx_timeout = g_timeout_add(100, tx_timeout_cb, user_data);

	return FALSE;
}

static gboolean tx_timeout_cb(gpointer user_data)
{
	struct telephony_service *service = user_data;
	struct pending_sms *msg = 0;
	struct cb_data *cbd = 0;

	if (tx_active)
		return TRUE;

	if (!service->initialized) {
		/* if service isn't initialized yet we have to wait a bit before trying
		 * again to send all messages */
		tx_timeout = g_timeout_add_seconds(5, restart_tx_queue_cb, service);
		return FALSE;
	}

	if (g_queue_is_empty(tx_queue)) {
		tx_timeout = 0;
		return FALSE;
	}

	msg = g_queue_pop_head(tx_queue);

	cbd = cb_data_new(NULL, service);
	cbd->user = msg;

	tx_active = TRUE;

	service->driver->send_sms(service, msg->to, msg->text, send_msg_cb, cbd);

	return TRUE;
}

static bool query_pending_messages_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct telephony_service *service = user_data;
	const char *payload = 0;
	jvalue_ref parsed_obj = 0;
	jvalue_ref results_obj = 0;
	int n = 0;

	// FIXME:
	// - record all messages in a list and sent them one after another and mark them
	// as successfully sent in the database
	// - mark all invalid messages as failed

	payload = LSMessageGetPayload(message);
	parsed_obj = luna_service_message_parse_and_validate(payload);
	if (jis_null(parsed_obj))
		goto cleanup;

	if (!jobject_get_exists(parsed_obj, J_CSTR_TO_BUF("results"), &results_obj) ||
	    !jis_array(results_obj))
		goto cleanup;

	for (n = 0; n < jarray_size(results_obj); n++) {
		jvalue_ref result_obj = jarray_get(results_obj, n);
		jvalue_ref id_obj = 0;
		jvalue_ref to_obj = 0;
		jvalue_ref text_obj = 0;
		jvalue_ref addr_obj = 0;
		raw_buffer id_buf;
		raw_buffer to_buf;
		raw_buffer text_buf;
		struct pending_sms *msg;

		if (!jobject_get_exists(result_obj, J_CSTR_TO_BUF("_id"), &id_obj)) {
			g_warning("Found pending outgoing SMS message without a id. Skipping it.");
			continue;
		}

		if (!jobject_get_exists(result_obj, J_CSTR_TO_BUF("to"), &to_obj)) {
			g_warning("Found pending outgoing SMS message without a recipient. Skipping it.");
			// FIXME mark message as failed
			continue;
		}

		if (!jobject_get_exists(to_obj, J_CSTR_TO_BUF("addr"), &addr_obj)) {
			g_warning("Found pending outgoing SMS message without a recipient. Skipping it.");
			// FIXME mark message as failed
			continue;
		}

		if (!jobject_get_exists(result_obj, J_CSTR_TO_BUF("messageText"), &text_obj)) {
			g_warning("Found pending outgoing SMS message without a text. Skipping it.");
			// FIXME mark message as failed
			continue;
		}

		id_buf = jstring_get(id_obj);
		to_buf = jstring_get(to_obj);
		text_buf = jstring_get(text_obj);

		msg = g_new(struct pending_sms, 1);

		msg->id = g_strdup(id_buf.m_str);
		msg->to = g_strdup(to_buf.m_str);
		msg->text = g_strdup(text_buf.m_str);

		/* if we're the first one using it then create the queue */
		if (!tx_queue)
			tx_queue = g_queue_new();

		g_queue_push_tail(tx_queue, msg);
	}

	/* if the tx queue isn't already processed trigger it */
	if (!tx_timeout)
		tx_timeout = g_timeout_add(100, tx_timeout_cb, service);

cleanup:
	j_release(&parsed_obj);

	return true;
}

bool _service_internal_send_sms_from_db_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct telephony_service *service = user_data;
	jvalue_ref req_obj = 0;
	jvalue_ref where_obj = 0;
	jvalue_ref folder_obj = 0;
	jvalue_ref status_obj = 0;

	jobject_put(req_obj, J_CSTR_TO_JVAL("from"), jstring_create("com.palm.smsmessage:1"));

	where_obj = jarray_create(0);

	folder_obj = jobject_create();
	jobject_put(folder_obj, J_CSTR_TO_JVAL("prop"), jstring_create("folder"));
	jobject_put(folder_obj, J_CSTR_TO_JVAL("op"), jstring_create("="));
	jobject_put(folder_obj, J_CSTR_TO_JVAL("val"), jstring_create("outbox"));

	jarray_append(where_obj, folder_obj);

	status_obj = jobject_create();
	jobject_put(status_obj, J_CSTR_TO_JVAL("prop"), jstring_create("status"));
	jobject_put(status_obj, J_CSTR_TO_JVAL("op"), jstring_create("="));
	jobject_put(status_obj, J_CSTR_TO_JVAL("val"), jstring_create("pending"));

	jarray_append(where_obj, status_obj);

	jobject_put(req_obj, J_CSTR_TO_JVAL("where"), where_obj);

	if (!luna_service_call_validate_and_send(service->private_service, "luna://com.palm.db/find",
	                                         req_obj, query_pending_messages_cb, service))
		// FIXME eventually add a time to try again a bit later but if this fails
		// something should be really broken and it doubtfull that a later try will
		// work again.
		g_warning("Failed to query for pending SMS messages!?");

	j_release(&req_obj);

	return true;
}
