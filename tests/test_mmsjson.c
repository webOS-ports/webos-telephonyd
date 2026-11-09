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

/*
 * com.palm.mmsmessage:1 records: what an outbox record the app writes turns
 * into, what a received message is stored as, and the status merges.
 */

#include <string.h>

#include <glib.h>
#include <pbnjson.h>

#include "mmsjson.h"

static jvalue_ref parse(const char *json)
{
	jvalue_ref v = jdom_create(j_cstr_to_buffer(json), jschema_all(), NULL);

	g_assert_true(jis_object(v));
	return v;
}

static struct mms_outgoing* outgoing(const char *json, gchar **id, const char **error_code)
{
	jvalue_ref v = parse(json);
	struct mms_outgoing *msg = mms_outgoing_from_record(v, id, error_code);

	j_release(&v);
	return msg;
}

static const struct mms_file* file_at(struct mms_outgoing *msg, guint n)
{
	return g_ptr_array_index(msg->files, n);
}

static void test_app_record(void)
{
	gchar *id = NULL;
	const char *error_code = "x";
	struct mms_outgoing *msg = outgoing(
		"{\"_id\":\"m1\",\"_kind\":\"com.palm.mmsmessage:1\",\"folder\":\"outbox\",\"status\":\"pending\","
		"\"serviceName\":\"mms\",\"to\":[{\"addr\":\"+31 6 1234-5678\",\"name\":\"A\"},{\"addr\":\"0612345679\"},"
		"{\"addr\":\"+31612345678\"}],\"messageText\":\"Hi there\",\"simId\":1,"
		"\"attachments\":[{\"path\":\"/media/internal/DCIM/a.jpg\",\"mimeType\":\"image/jpeg\",\"name\":\"a.jpg\"},"
		"{\"path\":\"/media/internal/.im-attachments/v.m4a\"}],\"filePath\":\"/media/internal/DCIM/a.jpg\"}",
		&id, &error_code);

	g_assert_nonnull(msg);
	g_assert_null(id);
	g_assert_null(error_code);
	g_assert_cmpstr(msg->id, ==, "m1");
	/* normalized, and the same number twice is one recipient */
	g_assert_cmpuint(msg->recipients->len, ==, 2);
	g_assert_cmpstr(g_ptr_array_index(msg->recipients, 0), ==, "+31612345678");
	g_assert_cmpstr(g_ptr_array_index(msg->recipients, 1), ==, "0612345679");
	g_assert_cmpstr(msg->text, ==, "Hi there");
	g_assert_cmpint(msg->sim_id, ==, 1);
	/* attachments win over filePath, which only repeats the first */
	g_assert_cmpuint(msg->files->len, ==, 2);
	g_assert_cmpstr(file_at(msg, 0)->path, ==, "/media/internal/DCIM/a.jpg");
	g_assert_cmpstr(file_at(msg, 0)->content_type, ==, "image/jpeg");
	g_assert_cmpstr(file_at(msg, 1)->path, ==, "/media/internal/.im-attachments/v.m4a");
	g_assert_null(file_at(msg, 1)->content_type);
	mms_outgoing_free(msg);
}

static void test_shapes(void)
{
	gchar *id = NULL;
	const char *error_code = NULL;
	struct mms_outgoing *msg;

	/* one recipient as an object, the file as filePath alone */
	msg = outgoing("{\"_id\":\"m2\",\"to\":{\"addr\":\"112\"},\"filePath\":\"/media/internal/x.png\"}",
	               &id, &error_code);
	g_assert_nonnull(msg);
	g_assert_cmpuint(msg->recipients->len, ==, 1);
	g_assert_null(msg->text);
	g_assert_cmpuint(msg->files->len, ==, 1);
	g_assert_cmpstr(file_at(msg, 0)->path, ==, "/media/internal/x.png");
	g_assert_cmpint(msg->sim_id, ==, -1);
	mms_outgoing_free(msg);

	/* the legacy shape: the body as a text part */
	msg = outgoing("{\"_id\":\"m3\",\"to\":[{\"addr\":\"112\"}],\"attachments\":["
	               "{\"mimeType\":\"text/plain\",\"partText\":\"old body\"}]}", &id, &error_code);
	g_assert_nonnull(msg);
	g_assert_cmpstr(msg->text, ==, "old body");
	g_assert_cmpuint(msg->files->len, ==, 0);
	mms_outgoing_free(msg);

	/* messageText wins over a text part saying the same */
	msg = outgoing("{\"_id\":\"m4\",\"to\":[{\"addr\":\"112\"}],\"messageText\":\"body\",\"attachments\":["
	               "{\"mimeType\":\"text/plain\",\"partText\":\"body\"},{\"path\":\"/media/internal/a.gif\"}]}",
	               &id, &error_code);
	g_assert_nonnull(msg);
	g_assert_cmpstr(msg->text, ==, "body");
	g_assert_cmpuint(msg->files->len, ==, 1);
	mms_outgoing_free(msg);

	/* a negative or odd simId means the default SIM */
	msg = outgoing("{\"_id\":\"m5\",\"to\":[{\"addr\":\"112\"}],\"messageText\":\"x\",\"simId\":-3}",
	               &id, &error_code);
	g_assert_nonnull(msg);
	g_assert_cmpint(msg->sim_id, ==, -1);
	mms_outgoing_free(msg);
	msg = outgoing("{\"_id\":\"m6\",\"to\":[{\"addr\":\"112\"}],\"messageText\":\"x\",\"simId\":\"1\"}",
	               &id, &error_code);
	g_assert_nonnull(msg);
	g_assert_cmpint(msg->sim_id, ==, -1);
	mms_outgoing_free(msg);
}

static void expect_failure(const char *json, const char *want_id, const char *want_code)
{
	gchar *id = NULL;
	const char *error_code = NULL;
	struct mms_outgoing *msg = outgoing(json, &id, &error_code);

	g_assert_null(msg);
	g_assert_cmpstr(id, ==, want_id);
	g_assert_cmpstr(error_code, ==, want_code);
	g_free(id);
}

static void test_bad_records(void)
{
	/* nothing to mark failed without an id */
	expect_failure("{\"to\":[{\"addr\":\"112\"}],\"messageText\":\"x\"}", NULL, MMS_ERR_UNSUPPORTED);
	expect_failure("{\"_id\":\"\",\"to\":[{\"addr\":\"112\"}],\"messageText\":\"x\"}", NULL, MMS_ERR_UNSUPPORTED);
	expect_failure("{\"_id\":5,\"to\":[{\"addr\":\"112\"}],\"messageText\":\"x\"}", NULL, MMS_ERR_UNSUPPORTED);

	expect_failure("{\"_id\":\"b1\",\"messageText\":\"x\"}", "b1", MMS_ERR_BAD_ADDRESS);
	expect_failure("{\"_id\":\"b2\",\"to\":[],\"messageText\":\"x\"}", "b2", MMS_ERR_BAD_ADDRESS);
	/* one address mmsd cannot take fails the whole message, not just that one */
	expect_failure("{\"_id\":\"b3\",\"to\":[{\"addr\":\"112\"},{\"addr\":\"a@b.c\"}],\"messageText\":\"x\"}",
	               "b3", MMS_ERR_BAD_ADDRESS);
	expect_failure("{\"_id\":\"b3a\",\"to\":[{\"addr\":\"06-1234 abcd\"}],\"messageText\":\"x\"}", "b3a",
	               MMS_ERR_BAD_ADDRESS);
	expect_failure("{\"_id\":\"b4\",\"to\":[{\"name\":\"no addr\"}],\"messageText\":\"x\"}", "b4",
	               MMS_ERR_BAD_ADDRESS);
	expect_failure("{\"_id\":\"b5\",\"to\":\"112\",\"messageText\":\"x\"}", "b5", MMS_ERR_BAD_ADDRESS);

	/* nothing to send */
	expect_failure("{\"_id\":\"b6\",\"to\":[{\"addr\":\"112\"}]}", "b6", MMS_ERR_UNSUPPORTED);
	expect_failure("{\"_id\":\"b7\",\"to\":[{\"addr\":\"112\"}],\"messageText\":\"\"}", "b7", MMS_ERR_UNSUPPORTED);
	/* a part that is neither a file nor text */
	expect_failure("{\"_id\":\"b8\",\"to\":[{\"addr\":\"112\"}],\"messageText\":\"x\",\"attachments\":["
	               "{\"mimeType\":\"image/png\"}]}", "b8", MMS_ERR_UNSUPPORTED);

	/* too many files */
	expect_failure("{\"_id\":\"b9\",\"to\":[{\"addr\":\"112\"}],\"attachments\":["
	               "{\"path\":\"/a/1\"},{\"path\":\"/a/2\"},{\"path\":\"/a/3\"},{\"path\":\"/a/4\"},"
	               "{\"path\":\"/a/5\"},{\"path\":\"/a/6\"},{\"path\":\"/a/7\"},{\"path\":\"/a/8\"},"
	               "{\"path\":\"/a/9\"},{\"path\":\"/a/10\"},{\"path\":\"/a/11\"}]}", "b9", MMS_ERR_TOO_LARGE);
}

static gchar* get_str(jvalue_ref obj, const char *key)
{
	jvalue_ref v = NULL;
	raw_buffer b;

	if (!jobject_get_exists(obj, j_cstr_to_buffer(key), &v) || !jis_string(v))
		return NULL;
	b = jstring_get_fast(v);
	return g_strndup(b.m_str, (gsize) b.m_len);
}

static void assert_str(jvalue_ref obj, const char *key, const char *want)
{
	gchar *got = get_str(obj, key);

	g_assert_cmpstr(got, ==, want);
	g_free(got);
}

static void test_received_record(void)
{
	struct mms_received *msg = mms_received_new("0a1b2c");
	struct mms_received_part *part;
	jvalue_ref rec;
	jvalue_ref v = NULL;
	jvalue_ref parts = NULL;
	gint64 n64 = 0;
	bool b = true;

	msg->sender = g_strdup("+31612345678");
	g_ptr_array_add(msg->recipients, g_strdup("+31612345679"));
	msg->subject = g_strdup("Subj");
	msg->timestamp_ms = 1000;
	msg->local_timestamp_ms = 2000;
	msg->sim_id = 0;

	part = g_new0(struct mms_received_part, 1);
	part->content_type = g_strdup("text/plain");
	part->text = g_strdup("first");
	g_ptr_array_add(msg->parts, part);
	part = g_new0(struct mms_received_part, 1);
	part->content_type = g_strdup("image/jpeg");
	part->path = g_strdup("/media/internal/.mms-attachments/0a1b2c/2-a.jpg");
	part->name = g_strdup("2-a.jpg");
	g_ptr_array_add(msg->parts, part);
	part = g_new0(struct mms_received_part, 1);
	part->content_type = g_strdup("text/plain");
	part->text = g_strdup("second");
	g_ptr_array_add(msg->parts, part);

	rec = mms_received_record(msg);

	assert_str(rec, "_kind", "com.palm.mmsmessage:1");
	assert_str(rec, "folder", "inbox");
	assert_str(rec, "status", "successful");
	assert_str(rec, "serviceName", "mms");
	assert_str(rec, "remoteMessageId", "0a1b2c");
	assert_str(rec, "subject", "Subj");
	assert_str(rec, "messageText", "first\nsecond");
	assert_str(rec, "filePath", "/media/internal/.mms-attachments/0a1b2c/2-a.jpg");

	g_assert_true(jobject_get_exists(rec, J_CSTR_TO_BUF("from"), &v));
	assert_str(v, "addr", "+31612345678");
	g_assert_true(jobject_get_exists(rec, J_CSTR_TO_BUF("to"), &v));
	g_assert_cmpint(jarray_size(v), ==, 1);
	assert_str(jarray_get(v, 0), "addr", "+31612345679");
	g_assert_true(jobject_get_exists(rec, J_CSTR_TO_BUF("flags"), &v));
	g_assert_true(jobject_get_exists(v, J_CSTR_TO_BUF("read"), &v));
	jboolean_get(v, &b);
	g_assert_false(b);
	g_assert_true(jobject_get_exists(rec, J_CSTR_TO_BUF("timestamp"), &v));
	jnumber_get_i64(v, &n64);
	g_assert_cmpint(n64, ==, 1000);
	g_assert_true(jobject_get_exists(rec, J_CSTR_TO_BUF("localTimestamp"), &v));
	jnumber_get_i64(v, &n64);
	g_assert_cmpint(n64, ==, 2000);
	g_assert_true(jobject_get_exists(rec, J_CSTR_TO_BUF("simId"), &v));

	/* the parts in the shape the apps have always read */
	g_assert_true(jobject_get_exists(rec, J_CSTR_TO_BUF("attachments"), &parts));
	g_assert_cmpint(jarray_size(parts), ==, 3);
	assert_str(jarray_get(parts, 0), "mimeType", "text/plain");
	assert_str(jarray_get(parts, 0), "partText", "first");
	assert_str(jarray_get(parts, 1), "mimeType", "image/jpeg");
	assert_str(jarray_get(parts, 1), "path", "/media/internal/.mms-attachments/0a1b2c/2-a.jpg");
	assert_str(jarray_get(parts, 1), "name", "2-a.jpg");
	g_assert_false(jobject_get_exists(jarray_get(parts, 1), J_CSTR_TO_BUF("partText"), &v));

	j_release(&rec);

	/* no date from mmsd: the time we got it; no slot: no simId; no file: no filePath */
	msg->timestamp_ms = 0;
	msg->sim_id = -1;
	g_ptr_array_remove_index(msg->parts, 1);
	rec = mms_received_record(msg);
	g_assert_true(jobject_get_exists(rec, J_CSTR_TO_BUF("timestamp"), &v));
	jnumber_get_i64(v, &n64);
	g_assert_cmpint(n64, ==, 2000);
	g_assert_false(jobject_get_exists(rec, J_CSTR_TO_BUF("simId"), &v));
	g_assert_false(jobject_get_exists(rec, J_CSTR_TO_BUF("filePath"), &v));
	j_release(&rec);

	mms_received_free(msg);
}

static void test_merge_requests(void)
{
	jvalue_ref req = mms_status_merge_request("id1", "uuid1", "sending", NULL);
	jvalue_ref v = NULL;
	jvalue_ref obj;

	g_assert_true(jobject_get_exists(req, J_CSTR_TO_BUF("objects"), &v));
	g_assert_cmpint(jarray_size(v), ==, 1);
	obj = jarray_get(v, 0);
	assert_str(obj, "_id", "id1");
	assert_str(obj, "remoteMessageId", "uuid1");
	assert_str(obj, "status", "sending");
	assert_str(obj, "errorCode", "");
	g_assert_false(jobject_get_exists(req, J_CSTR_TO_BUF("query"), &v));
	j_release(&req);

	req = mms_status_merge_request(NULL, "uuid2", "failed", MMS_ERR_NETWORK);
	g_assert_true(jobject_get_exists(req, J_CSTR_TO_BUF("props"), &obj));
	assert_str(obj, "status", "failed");
	assert_str(obj, "errorCode", MMS_ERR_NETWORK);
	g_assert_false(jobject_get_exists(obj, J_CSTR_TO_BUF("_id"), &v));
	g_assert_true(jobject_get_exists(req, J_CSTR_TO_BUF("query"), &obj));
	assert_str(obj, "from", "com.palm.mmsmessage:1");
	g_assert_true(jobject_get_exists(obj, J_CSTR_TO_BUF("where"), &v));
	g_assert_cmpint(jarray_size(v), ==, 1);
	assert_str(jarray_get(v, 0), "prop", "remoteMessageId");
	assert_str(jarray_get(v, 0), "op", "=");
	assert_str(jarray_get(v, 0), "val", "uuid2");
	j_release(&req);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/mmsjson/app-record", test_app_record);
	g_test_add_func("/mmsjson/shapes", test_shapes);
	g_test_add_func("/mmsjson/bad-records", test_bad_records);
	g_test_add_func("/mmsjson/received-record", test_received_record);
	g_test_add_func("/mmsjson/merge-requests", test_merge_requests);

	return g_test_run();
}

// vim:ts=4:sw=4:noexpandtab
