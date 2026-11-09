/* @@@LICENSE
*
* Copyright (c) 2026 LuneOS project
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

#include <string.h>

#include "mmsjson.h"

/* A string member as a new C string; NULL when missing or not a string. */
static gchar* get_string(jvalue_ref obj, raw_buffer key)
{
	jvalue_ref value = NULL;
	raw_buffer buf;

	if (!jis_object(obj) || !jobject_get_exists(obj, key, &value) || !jis_string(value))
		return NULL;

	buf = jstring_get_fast(value);
	if (!buf.m_str)
		return NULL;

	return g_strndup(buf.m_str, (gsize) buf.m_len);
}

/* Appends one recipient; false when it is not a number mmsd can send to. */
static bool add_recipient(struct mms_outgoing *msg, jvalue_ref recipient)
{
	gchar *addr = get_string(recipient, J_CSTR_TO_BUF("addr"));
	gchar *number = mms_normalize_number(addr);
	guint n;

	g_free(addr);

	if (!number)
		return false;

	/* the same number twice is one recipient */
	for (n = 0; n < msg->recipients->len; n++) {
		if (strcmp(g_ptr_array_index(msg->recipients, n), number) == 0) {
			g_free(number);
			return true;
		}
	}

	g_ptr_array_add(msg->recipients, number);
	return true;
}

static void append_text(struct mms_outgoing *msg, const char *text)
{
	gchar *joined;

	if (!text || !*text)
		return;

	if (!msg->text || !*msg->text) {
		g_free(msg->text);
		msg->text = g_strdup(text);
		return;
	}

	joined = g_strconcat(msg->text, "\n", text, NULL);
	g_free(msg->text);
	msg->text = joined;
}

struct mms_outgoing* mms_outgoing_from_record(jvalue_ref record, gchar **id, const char **error_code)
{
	struct mms_outgoing *msg;
	jvalue_ref to = NULL;
	jvalue_ref attachments = NULL;
	jvalue_ref sim = NULL;
	gchar *record_id;
	gchar *text;
	ssize_t n;

	*id = NULL;
	*error_code = MMS_ERR_UNSUPPORTED;

	record_id = get_string(record, J_CSTR_TO_BUF("_id"));
	if (!record_id || !*record_id) {
		g_free(record_id);
		return NULL;
	}

	msg = mms_outgoing_new(record_id);

	if (!jobject_get_exists(record, J_CSTR_TO_BUF("to"), &to)) {
		*error_code = MMS_ERR_BAD_ADDRESS;
		goto fail;
	}

	if (jis_array(to)) {
		for (n = 0; n < jarray_size(to); n++) {
			if (!add_recipient(msg, jarray_get(to, n))) {
				*error_code = MMS_ERR_BAD_ADDRESS;
				goto fail;
			}
		}
	}
	else if (!add_recipient(msg, to)) {
		*error_code = MMS_ERR_BAD_ADDRESS;
		goto fail;
	}

	if (msg->recipients->len == 0) {
		*error_code = MMS_ERR_BAD_ADDRESS;
		goto fail;
	}

	msg->text = get_string(record, J_CSTR_TO_BUF("messageText"));
	msg->subject = get_string(record, J_CSTR_TO_BUF("subject"));

	if (jobject_get_exists(record, J_CSTR_TO_BUF("attachments"), &attachments) && jis_array(attachments) &&
	    jarray_size(attachments) > 0) {
		for (n = 0; n < jarray_size(attachments); n++) {
			jvalue_ref part = jarray_get(attachments, n);
			gchar *path = get_string(part, J_CSTR_TO_BUF("path"));
			gchar *type = get_string(part, J_CSTR_TO_BUF("mimeType"));

			if (path && *path) {
				mms_outgoing_add_file(msg, path, type);
			}
			else if (type && g_ascii_strncasecmp(type, "text/plain", 10) == 0) {
				/* the old shape: the body as a text part; messageText,
				 * when there is one, already says the same */
				if (!msg->text || !*msg->text) {
					text = get_string(part, J_CSTR_TO_BUF("partText"));
					append_text(msg, text);
					g_free(text);
				}
			}
			else {
				g_free(path);
				g_free(type);
				goto fail;
			}

			g_free(path);
			g_free(type);
		}
	}
	else {
		gchar *path = get_string(record, J_CSTR_TO_BUF("filePath"));

		if (path && *path)
			mms_outgoing_add_file(msg, path, NULL);
		g_free(path);
	}

	if (msg->files->len > MMS_MAX_FILES) {
		*error_code = MMS_ERR_TOO_LARGE;
		goto fail;
	}

	if (msg->files->len == 0 && (!msg->text || !*msg->text))
		goto fail;

	if (jobject_get_exists(record, J_CSTR_TO_BUF("simId"), &sim) && jis_number(sim) &&
	    (jnumber_get_i32(sim, &msg->sim_id) != CONV_OK || msg->sim_id < 0))
		msg->sim_id = -1;

	*error_code = NULL;
	g_free(record_id);
	return msg;

fail:
	*id = record_id;
	mms_outgoing_free(msg);
	return NULL;
}

void mms_received_part_free(struct mms_received_part *part)
{
	if (!part)
		return;

	g_free(part->content_type);
	g_free(part->text);
	g_free(part->path);
	g_free(part->name);
	g_free(part);
}

struct mms_received* mms_received_new(const char *remote_id)
{
	struct mms_received *msg = g_new0(struct mms_received, 1);

	msg->remote_id = g_strdup(remote_id);
	msg->recipients = g_ptr_array_new_with_free_func(g_free);
	msg->parts = g_ptr_array_new_with_free_func((GDestroyNotify) mms_received_part_free);
	msg->sim_id = -1;

	return msg;
}

void mms_received_free(struct mms_received *msg)
{
	if (!msg)
		return;

	g_free(msg->remote_id);
	g_free(msg->sender);
	g_free(msg->subject);
	g_ptr_array_free(msg->recipients, TRUE);
	g_ptr_array_free(msg->parts, TRUE);
	g_free(msg);
}

jvalue_ref mms_received_record(const struct mms_received *msg)
{
	jvalue_ref record = jobject_create();
	jvalue_ref flags = jobject_create();
	jvalue_ref from = jobject_create();
	jvalue_ref to = jarray_create(NULL);
	jvalue_ref parts = jarray_create(NULL);
	GString *text = g_string_new(NULL);
	const char *file_path = NULL;
	guint n;

	jobject_put(flags, J_CSTR_TO_JVAL("read"), jboolean_create(false));

	jobject_put(record, J_CSTR_TO_JVAL("_kind"), jstring_create(MMS_DB_KIND));
	jobject_put(record, J_CSTR_TO_JVAL("flags"), flags);
	jobject_put(record, J_CSTR_TO_JVAL("folder"), jstring_create("inbox"));
	jobject_put(record, J_CSTR_TO_JVAL("status"), jstring_create("successful"));
	jobject_put(record, J_CSTR_TO_JVAL("serviceName"), jstring_create("mms"));
	jobject_put(record, J_CSTR_TO_JVAL("remoteMessageId"), jstring_create(msg->remote_id));
	jobject_put(record, J_CSTR_TO_JVAL("localTimestamp"), jnumber_create_i64(msg->local_timestamp_ms));
	jobject_put(record, J_CSTR_TO_JVAL("timestamp"),
	            jnumber_create_i64(msg->timestamp_ms ? msg->timestamp_ms : msg->local_timestamp_ms));

	if (msg->sim_id >= 0)
		jobject_put(record, J_CSTR_TO_JVAL("simId"), jnumber_create_i32(msg->sim_id));

	jobject_put(from, J_CSTR_TO_JVAL("addr"), jstring_create(msg->sender ? msg->sender : ""));
	jobject_put(record, J_CSTR_TO_JVAL("from"), from);

	for (n = 0; n < msg->recipients->len; n++) {
		jvalue_ref addr = jobject_create();

		jobject_put(addr, J_CSTR_TO_JVAL("addr"), jstring_create(g_ptr_array_index(msg->recipients, n)));
		jarray_append(to, addr);
	}
	jobject_put(record, J_CSTR_TO_JVAL("to"), to);

	if (msg->subject && *msg->subject)
		jobject_put(record, J_CSTR_TO_JVAL("subject"), jstring_create(msg->subject));

	for (n = 0; n < msg->parts->len; n++) {
		const struct mms_received_part *part = g_ptr_array_index(msg->parts, n);
		jvalue_ref obj = jobject_create();

		jobject_put(obj, J_CSTR_TO_JVAL("mimeType"), jstring_create(part->content_type));

		if (part->text) {
			jobject_put(obj, J_CSTR_TO_JVAL("partText"), jstring_create(part->text));
			if (*part->text) {
				if (text->len)
					g_string_append_c(text, '\n');
				g_string_append(text, part->text);
			}
		}
		else if (part->path) {
			jobject_put(obj, J_CSTR_TO_JVAL("path"), jstring_create(part->path));
			if (part->name)
				jobject_put(obj, J_CSTR_TO_JVAL("name"), jstring_create(part->name));
			if (!file_path)
				file_path = part->path;
		}

		jarray_append(parts, obj);
	}

	jobject_put(record, J_CSTR_TO_JVAL("attachments"), parts);
	jobject_put(record, J_CSTR_TO_JVAL("messageText"), jstring_create(text->str));
	if (file_path)
		jobject_put(record, J_CSTR_TO_JVAL("filePath"), jstring_create(file_path));

	g_string_free(text, TRUE);
	return record;
}

jvalue_ref mms_status_merge_request(const char *id, const char *remote_id, const char *status,
                                    const char *error_code)
{
	jvalue_ref req = jobject_create();
	jvalue_ref props = jobject_create();

	jobject_put(props, J_CSTR_TO_JVAL("status"), jstring_create(status));
	/* a message that went out after all loses the error of its time-out */
	jobject_put(props, J_CSTR_TO_JVAL("errorCode"), jstring_create(error_code ? error_code : ""));

	if (id) {
		jvalue_ref list = jarray_create(NULL);

		jobject_put(props, J_CSTR_TO_JVAL("_id"), jstring_create(id));
		if (remote_id)
			jobject_put(props, J_CSTR_TO_JVAL("remoteMessageId"), jstring_create(remote_id));
		jarray_append(list, props);
		jobject_put(req, J_CSTR_TO_JVAL("objects"), list);
	}
	else {
		jvalue_ref query = jobject_create();
		jvalue_ref where = jarray_create(NULL);
		jvalue_ref clause = jobject_create();

		jobject_put(clause, J_CSTR_TO_JVAL("prop"), jstring_create("remoteMessageId"));
		jobject_put(clause, J_CSTR_TO_JVAL("op"), jstring_create("="));
		jobject_put(clause, J_CSTR_TO_JVAL("val"), jstring_create(remote_id ? remote_id : ""));
		jarray_append(where, clause);

		jobject_put(query, J_CSTR_TO_JVAL("from"), jstring_create(MMS_DB_KIND));
		jobject_put(query, J_CSTR_TO_JVAL("where"), where);

		jobject_put(req, J_CSTR_TO_JVAL("query"), query);
		jobject_put(req, J_CSTR_TO_JVAL("props"), props);
	}

	return req;
}

// vim:ts=4:sw=4:noexpandtab
