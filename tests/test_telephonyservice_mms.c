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
 * MMS end to end inside the daemon (src/telephonyservice_mms.c): db8 and the
 * activity manager are recording luna stubs the test answers by hand, mmsd is
 * the fake on a private bus (tests/fake_mmsd.c), and everything between is
 * the real code - mmsjson, mmscore, the mmsd client, the spool on disk.
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <pbnjson.h>
#include <luna-service2/lunaservice.h>

#include "telephonyservice.h"
#include "telephonyservice_internal.h"
#include "telephonyservice_mms.h"
#include "mmscore.h"
#include "fake_mmsd.h"

#define IMSI "001010000000001"

/* ------------------------------------------------------------------------
 * luna-service2 stubs
 * ------------------------------------------------------------------------ */

struct LSHandle {
	const char *name;
};

struct LSMessage {
	gchar *payload;
	bool subscription;
	int refs;
	GPtrArray *replies;
};

struct call {
	gchar *uri;
	gchar *payload;
	LSFilterFunc cb;
	void *ctx;
};

static GPtrArray *calls;		/* struct call*, not answered yet */
static GPtrArray *posts;		/* "key payload" */
static GPtrArray *subscription_keys;

bool LSErrorInit(LSError *error)
{
	memset(error, 0, sizeof(*error));
	return true;
}

void LSErrorFree(LSError *error)
{
}

void LSErrorPrint(LSError *lserror, FILE *out)
{
}

void LSMessageRef(LSMessage *message)
{
	message->refs++;
}

void LSMessageUnref(LSMessage *message)
{
	message->refs--;
}

const char* LSMessageGetPayload(LSMessage *message)
{
	return message->payload;
}

bool LSMessageIsSubscription(LSMessage *message)
{
	return message->subscription;
}

bool LSMessageReply(LSHandle *sh, LSMessage *lsmsg, const char *payload, LSError *lserror)
{
	g_ptr_array_add(lsmsg->replies, g_strdup(payload));
	return true;
}

bool LSSubscriptionAdd(LSHandle *sh, const char *key, LSMessage *message, LSError *lserror)
{
	g_ptr_array_add(subscription_keys, g_strdup(key));
	return true;
}

bool LSSubscriptionProcess(LSHandle *sh, LSMessage *message, bool *subscribed, LSError *lserror)
{
	g_assert_not_reached();
	return false;
}

bool LSSubscriptionReply(LSHandle *sh, const char *key, const char *payload, LSError *lserror)
{
	g_ptr_array_add(posts, g_strdup_printf("%s %s", key, payload));
	return true;
}

bool LSSubscriptionPost(LSHandle *sh, const char *category, const char *method, const char *payload,
                        LSError *lserror)
{
	g_assert_not_reached();
	return false;
}

bool LSCallOneReply(LSHandle *sh, const char *uri, const char *payload, LSFilterFunc callback, void *ctx,
                    LSMessageToken *ret_token, LSError *lserror)
{
	struct call *c = g_new0(struct call, 1);

	c->uri = g_strdup(uri);
	c->payload = g_strdup(payload);
	c->cb = callback;
	c->ctx = ctx;
	g_ptr_array_add(calls, c);

	return true;
}

static void call_free(gpointer data)
{
	struct call *c = data;

	g_free(c->uri);
	g_free(c->payload);
	g_free(c);
}

static LSMessage* message_new(const char *payload, bool subscription)
{
	LSMessage *m = g_new0(LSMessage, 1);

	m->payload = g_strdup(payload);
	m->subscription = subscription;
	m->replies = g_ptr_array_new_with_free_func(g_free);

	return m;
}

static void message_free(LSMessage *m)
{
	g_assert_cmpint(m->refs, ==, 0);
	g_free(m->payload);
	g_ptr_array_free(m->replies, TRUE);
	g_free(m);
}

/* ------------------------------------------------------------------------
 * telephonyservice.c stubs: one slot, sim 0, with IMSI
 * ------------------------------------------------------------------------ */

static struct telephony_sim_state sim0 = { .sim_id = 0, .initialized = true };

struct telephony_sim_state* telephony_service_sim_state(struct telephony_service *service, int sim_id)
{
	return sim_id == 0 ? &sim0 : NULL;
}

int telephony_service_get_default_sim(struct telephony_service *service, enum telephony_sim_role role)
{
	return 0;
}

void telephony_service_post_subscription(struct telephony_service *service, const char *method,
                                         jvalue_ref reply_obj)
{
	jschema_ref schema = jschema_parse(j_cstr_to_buffer("{}"), DOMOPT_NOOPT, NULL);

	g_ptr_array_add(posts, g_strdup_printf("/%s %s", method, jvalue_tostring(reply_obj, schema)));
	jschema_release(&schema);
}

/* ------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------ */

static struct fake_mmsd fake;
static struct LSHandle palm = { "com.palm.telephony" };
static gchar *tmp;
static gchar *media;	/* the allowed root */
static gchar *spool;
static gchar *inbox;
static struct telephony_service *service;
static gsize message_limit;	/* 0: the default */

static jvalue_ref parse(const char *json)
{
	jvalue_ref v = jdom_create(j_cstr_to_buffer(json), jschema_all(), NULL);

	g_assert_true(jis_object(v));
	return v;
}

static gchar* str_at(jvalue_ref obj, const char *path)
{
	gchar **keys = g_strsplit(path, ".", -1);
	jvalue_ref v = obj;
	gchar *out = NULL;
	raw_buffer b;
	int n;

	for (n = 0; keys[n]; n++) {
		char *end = NULL;
		long index = strtol(keys[n], &end, 10);

		if (*end == '\0' && jis_array(v)) {
			if (index >= jarray_size(v))
				goto done;
			v = jarray_get(v, index);
		}
		else if (!jis_object(v) || !jobject_get_exists(v, j_cstr_to_buffer(keys[n]), &v)) {
			goto done;
		}
	}

	if (jis_string(v)) {
		b = jstring_get_fast(v);
		out = g_strndup(b.m_str, (gsize) b.m_len);
	}
	else {
		out = g_strdup(jvalue_tostring_simple(v));
	}

done:
	g_strfreev(keys);
	return out;
}

static void assert_json(const char *payload, const char *path, const char *want)
{
	jvalue_ref v = parse(payload);
	gchar *got = str_at(v, path);

	if (g_strcmp0(got, want) != 0)
		g_test_message("in %s", payload);
	g_assert_cmpstr(got, ==, want);
	g_free(got);
	j_release(&v);
}

static gboolean has_call(gpointer data)
{
	guint n;

	for (n = 0; n < calls->len; n++) {
		if (g_str_has_suffix(((struct call *) g_ptr_array_index(calls, n))->uri, data))
			return TRUE;
	}

	return FALSE;
}

/* The first outstanding call to a uri ending in suffix, taken off the list. */
static struct call* take_call(const char *suffix)
{
	guint n;

	g_assert_true(fake_wait(has_call, (gpointer) suffix));

	for (n = 0; n < calls->len; n++) {
		struct call *c = g_ptr_array_index(calls, n);

		if (g_str_has_suffix(c->uri, suffix)) {
			g_ptr_array_steal_index(calls, n);
			return c;
		}
	}

	g_assert_not_reached();
	return NULL;
}

static void answer(struct call *c, const char *reply_payload)
{
	if (c->cb) {
		LSMessage *reply = message_new(reply_payload, false);

		c->cb(NULL, reply, c->ctx);
		message_free(reply);
	}
	call_free(c);
}

static void drop_calls(const char *suffix)
{
	guint n = 0;

	while (n < calls->len) {
		struct call *c = g_ptr_array_index(calls, n);

		if (g_str_has_suffix(c->uri, suffix)) {
			g_ptr_array_remove_index(calls, n);
			continue;
		}
		n++;
	}
}

static guint count_calls(const char *suffix)
{
	guint n, count = 0;

	for (n = 0; n < calls->len; n++) {
		if (g_str_has_suffix(((struct call *) g_ptr_array_index(calls, n))->uri, suffix))
			count++;
	}

	return count;
}

static gboolean sends_reached(gpointer data)
{
	return fake.sends >= GPOINTER_TO_UINT(data);
}

static gboolean deleted_reached(gpointer data)
{
	return fake.deleted->len >= GPOINTER_TO_UINT(data);
}

static gboolean last_post_says_available(gpointer data)
{
	gboolean want = GPOINTER_TO_INT(data);
	const char *last;

	if (posts->len == 0)
		return FALSE;
	last = g_ptr_array_index(posts, posts->len - 1);

	return g_str_has_prefix(last, "/mmsStatusQuery ") &&
	       strstr(last, want ? "\"available\":true" : "\"available\":false") != NULL;
}

static void setup(guint send_timeout_s)
{
	static const char *roots[2];
	struct telephony_mms_config config;

	calls = g_ptr_array_new_with_free_func(call_free);
	posts = g_ptr_array_new_with_free_func(g_free);
	subscription_keys = g_ptr_array_new_with_free_func(g_free);

	service = g_new0(struct telephony_service, 1);
	service->palmHandle = &palm;

	roots[0] = media;
	roots[1] = NULL;

	config.bus_type = G_BUS_TYPE_SYSTEM;
	config.spool_dir = spool;
	config.inbox_dir = inbox;
	config.spool_gid = getgid();
	config.roots = roots;
	config.send_timeout_s = send_timeout_s;
	config.max_message_size = message_limit;

	telephony_service_mms_setup(service, &config);
	g_assert_nonnull(service->mms);

	/* the activity is re-armed at start, under the redirected name
	 * (TELEPHONY_ACTIVITY_COMPLETE_URI) */
	answer(take_call("//com.palm.activitymanager/complete"), "{\"returnValue\":true}");
}

static void teardown(void)
{
	telephony_service_mms_shutdown(service);
	g_assert_null(service->mms);
	fake_settle();
	g_free(service);
	g_ptr_array_free(calls, TRUE);
	g_ptr_array_free(posts, TRUE);
	g_ptr_array_free(subscription_keys, TRUE);

	{
		guint n;

		for (n = fake.services->len; n > 0; n--)
			fake_mmsd_remove_service(&fake, g_ptr_array_index(fake.services, n - 1));
	}
	g_hash_table_remove_all(fake.stored);
	g_ptr_array_set_size(fake.deleted, 0);
	g_clear_pointer(&fake.send_error, g_free);
	fake.sends = 0;
	if (fake.owned)
		fake_mmsd_release(&fake);
	fake_settle();
}

static void start_with_service(guint send_timeout_s)
{
	fake_mmsd_own(&fake);
	fake_mmsd_add_service(&fake, IMSI, FALSE);
	setup(send_timeout_s);
	g_assert_true(fake_wait(last_post_says_available, GINT_TO_POINTER(TRUE)));
}

static void media_file(const char *name, const char *data, gsize len)
{
	gchar *path = g_build_filename(media, name, NULL);

	g_assert_true(g_file_set_contents(path, data, (gssize) len, NULL));
	g_free(path);
}

/* The activity calls sendMmsFromDb; db8 answers its find with results. */
static void run_activity(const char *results_json)
{
	LSMessage *m = message_new("{}", false);
	struct call *find;
	gchar *reply;

	g_assert_true(_service_internal_send_mms_from_db_cb(&palm, m, service));
	assert_json(g_ptr_array_index(m->replies, 0), "returnValue", "true");
	message_free(m);

	find = take_call("com.palm.db/find");
	assert_json(find->payload, "query.from", "com.palm.mmsmessage:1");
	assert_json(find->payload, "query.where.0.prop", "folder");
	assert_json(find->payload, "query.where.0.val", "outbox");
	assert_json(find->payload, "query.where.1.prop", "status");
	assert_json(find->payload, "query.where.1.val", "pending");

	reply = g_strdup_printf("{\"returnValue\":true,\"results\":%s}", results_json);
	answer(find, reply);
	g_free(reply);

	answer(take_call("//com.palm.activitymanager/complete"), "{\"returnValue\":true}");
}

/* The next merge, checked and answered. */
static void expect_merge(const char *id, const char *remote_id, const char *status, const char *error_code)
{
	struct call *c = take_call("com.palm.db/merge");

	if (id) {
		assert_json(c->payload, "objects.0._id", id);
		assert_json(c->payload, "objects.0.status", status);
		assert_json(c->payload, "objects.0.errorCode", error_code ? error_code : "");
		if (remote_id)
			assert_json(c->payload, "objects.0.remoteMessageId", remote_id);
	}
	else {
		assert_json(c->payload, "query.from", "com.palm.mmsmessage:1");
		assert_json(c->payload, "query.where.0.prop", "remoteMessageId");
		assert_json(c->payload, "query.where.0.val", remote_id);
		assert_json(c->payload, "props.status", status);
		assert_json(c->payload, "props.errorCode", error_code ? error_code : "");
	}

	answer(c, "{\"returnValue\":true}");
}

static guint spool_entries(void)
{
	GDir *dir = g_dir_open(spool, 0, NULL);
	guint n = 0;

	if (!dir)
		return 0;
	while (g_dir_read_name(dir))
		n++;
	g_dir_close(dir);

	return n;
}

static gchar* record(const char *id, const char *to, const char *extra)
{
	return g_strdup_printf("{\"_id\":\"%s\",\"_kind\":\"com.palm.mmsmessage:1\",\"folder\":\"outbox\","
	                       "\"status\":\"pending\",\"serviceName\":\"mms\",\"to\":[{\"addr\":\"%s\"}]%s}",
	                       id, to, extra);
}

/* ------------------------------------------------------------------------
 * Sending
 * ------------------------------------------------------------------------ */

static void test_send_and_sent(void)
{
	gchar *rec_json;
	gchar *results;
	gchar *pic;
	GVariantIter *to, *att;
	const char *smil, *s, *id, *ct, *file;
	gchar *uuid;

	start_with_service(600);
	media_file("pic.jpg", "\xff\xd8JPEGDATA", 10);
	pic = g_build_filename(media, "pic.jpg", NULL);

	rec_json = record("m1", "+31 6 1234 5678",
	                  ",\"messageText\":\"Hello\",\"attachments\":[{\"path\":\"PIC\",\"mimeType\":\"image/jpeg\"}]");
	results = g_strdup_printf("[%s]", rec_json);
	{
		gchar **split = g_strsplit(results, "PIC", -1);

		g_free(results);
		results = g_strjoinv(pic, split);
		g_strfreev(split);
	}

	run_activity(results);
	expect_merge("m1", NULL, "sending", NULL);

	g_assert_true(fake_wait(sends_reached, GUINT_TO_POINTER(1)));

	/* what mmsd got */
	g_variant_get(fake.last_send, "(as&sa(sss))", &to, &smil, &att);
	g_assert_true(g_variant_iter_next(to, "&s", &s));
	g_assert_cmpstr(s, ==, "+31612345678");
	g_assert_false(g_variant_iter_next(to, "&s", &s));
	g_assert_nonnull(strstr(smil, "<img src=\"cid:part1.jpg\" region=\"Image\"/><text src=\"cid:text_0.txt\""));
	g_assert_true(g_variant_iter_next(att, "(&s&s&s)", &id, &ct, &file));
	g_assert_cmpstr(id, ==, "<text_0.txt>");
	g_assert_cmpstr(ct, ==, "text/plain");
	g_assert_true(g_str_has_prefix(file, spool));
	g_assert_true(g_variant_iter_next(att, "(&s&s&s)", &id, &ct, &file));
	g_assert_cmpstr(id, ==, "<part1.jpg>");
	g_assert_cmpstr(ct, ==, "image/jpeg");
	g_assert_true(g_str_has_prefix(file, spool));
	g_assert_false(g_variant_iter_next(att, "(&s&s&s)", &id, &ct, &file));
	g_variant_iter_free(to);
	g_variant_iter_free(att);

	/* the copies it read: the text and the picture, readable by group mmsd only */
	g_assert_cmpmem(g_bytes_get_data(g_ptr_array_index(fake.last_send_data, 0), NULL),
	                g_bytes_get_size(g_ptr_array_index(fake.last_send_data, 0)), "Hello", 5);
	g_assert_cmpmem(g_bytes_get_data(g_ptr_array_index(fake.last_send_data, 1), NULL),
	                g_bytes_get_size(g_ptr_array_index(fake.last_send_data, 1)), "\xff\xd8JPEGDATA", 10);
	g_assert_cmpuint(GPOINTER_TO_UINT(g_ptr_array_index(fake.last_send_modes, 0)), ==, 0640);
	g_assert_cmpuint(GPOINTER_TO_UINT(g_ptr_array_index(fake.last_send_modes, 1)), ==, 0640);

	/* mmsd took it: the record learns its id, the spool is empty again */
	uuid = g_path_get_basename(fake.last_message);
	expect_merge("m1", uuid, "sending", NULL);
	g_assert_cmpuint(spool_entries(), ==, 0);

	/* and sent it */
	fake_mmsd_emit_status(&fake, NULL, fake.last_message, "sent");
	expect_merge(NULL, uuid, "successful", NULL);
	g_assert_true(fake_wait(deleted_reached, GUINT_TO_POINTER(1)));
	g_assert_cmpstr(g_ptr_array_index(fake.deleted, 0), ==, fake.last_message);

	/* the original is untouched */
	g_assert_true(g_file_test(pic, G_FILE_TEST_IS_REGULAR));

	g_assert_cmpuint(calls->len, ==, 0);
	g_free(uuid);
	g_free(pic);
	g_free(rec_json);
	g_free(results);
	teardown();
}

static void test_refusals(void)
{
	gchar *results;
	gchar *outside = g_build_filename(tmp, "secret.txt", NULL);

	start_with_service(600);
	g_assert_true(g_file_set_contents(outside, "secret", 6, NULL));

	/* a file outside the media partition: never copied, never sent */
	results = g_strdup_printf("[{\"_id\":\"r1\",\"to\":[{\"addr\":\"112\"}],\"filePath\":\"%s\"}]", outside);
	run_activity(results);
	g_free(results);
	expect_merge("r1", NULL, "sending", NULL);
	expect_merge("r1", NULL, "failed", "mmsErrorUnsupportedMessage");
	fake_settle();
	g_assert_cmpuint(fake.sends, ==, 0);
	g_assert_cmpuint(spool_entries(), ==, 0);

	/* an address mmsd cannot take: failed straight away */
	run_activity("[{\"_id\":\"r2\",\"to\":[{\"addr\":\"someone@example.org\"}],\"messageText\":\"x\"}]");
	expect_merge("r2", NULL, "failed", "mmsErrorSendingAddressUnresolved");

	/* too large */
	{
		gchar *big = g_malloc0(MMS_DEFAULT_MESSAGE_SIZE + 1);
		gchar *path = g_build_filename(media, "big.jpg", NULL);

		g_assert_true(g_file_set_contents(path, big, MMS_DEFAULT_MESSAGE_SIZE + 1, NULL));
		results = g_strdup_printf("[{\"_id\":\"r3\",\"to\":[{\"addr\":\"112\"}],\"filePath\":\"%s\"}]", path);
		run_activity(results);
		expect_merge("r3", NULL, "sending", NULL);
		expect_merge("r3", NULL, "failed", "mmsErrorAttachmentTooLarge");
		g_unlink(path);
		g_free(path);
		g_free(big);
		g_free(results);
	}

	/* mmsd refuses it */
	fake.send_error = g_strdup("org.ofono.mms.Error.PermanentContentNotAccepted");
	run_activity("[{\"_id\":\"r4\",\"to\":[{\"addr\":\"112\"}],\"messageText\":\"x\"}]");
	expect_merge("r4", NULL, "sending", NULL);
	expect_merge("r4", NULL, "failed", "mmsErrorRejected");
	g_assert_cmpuint(fake.sends, ==, 1);
	g_assert_cmpuint(spool_entries(), ==, 0);

	/* a record without an id cannot be marked; it is skipped */
	run_activity("[{\"to\":[{\"addr\":\"112\"}],\"messageText\":\"x\"}]");
	fake_settle();
	g_assert_cmpuint(count_calls("com.palm.db/merge"), ==, 0);

	g_unlink(outside);
	g_free(outside);
	teardown();
}

static void test_one_at_a_time(void)
{
	start_with_service(600);

	/* the watch fires twice for the same records before mmsd answered */
	run_activity("[{\"_id\":\"o1\",\"to\":[{\"addr\":\"112\"}],\"messageText\":\"one\"},"
	             "{\"_id\":\"o2\",\"to\":[{\"addr\":\"113\"}],\"messageText\":\"two\"}]");
	run_activity("[{\"_id\":\"o1\",\"to\":[{\"addr\":\"112\"}],\"messageText\":\"one\"}]");
	expect_merge("o1", NULL, "sending", NULL);
	expect_merge("o2", NULL, "sending", NULL);

	g_assert_true(fake_wait(sends_reached, GUINT_TO_POINTER(2)));
	fake_settle();
	g_assert_cmpuint(fake.sends, ==, 2);
	drop_calls("com.palm.db/merge");

	teardown();
}

static void test_timeout_and_draft(void)
{
	gchar *uuid;
	gchar *first;

	start_with_service(1);

	run_activity("[{\"_id\":\"t1\",\"to\":[{\"addr\":\"112\"}],\"messageText\":\"x\"}]");
	expect_merge("t1", NULL, "sending", NULL);
	g_assert_true(fake_wait(sends_reached, GUINT_TO_POINTER(1)));
	uuid = g_path_get_basename(fake.last_message);
	first = g_strdup(fake.last_message);
	expect_merge("t1", uuid, "sending", NULL);

	/* no word from mmsd within the time-out */
	expect_merge(NULL, uuid, "failed", "mmsErrorNetwork");

	/* it went after all */
	fake_mmsd_emit_status(&fake, NULL, first, "sent");
	expect_merge(NULL, uuid, "successful", NULL);
	g_free(uuid);

	/* mmsd gives up on the next one */
	run_activity("[{\"_id\":\"t2\",\"to\":[{\"addr\":\"112\"}],\"messageText\":\"y\"}]");
	expect_merge("t2", NULL, "sending", NULL);
	g_assert_true(fake_wait(sends_reached, GUINT_TO_POINTER(2)));
	uuid = g_path_get_basename(fake.last_message);
	expect_merge("t2", uuid, "sending", NULL);
	fake_mmsd_emit_status(&fake, NULL, fake.last_message, "draft");
	expect_merge(NULL, uuid, "failed", "mmsErrorNetwork");

	/* and no time-out after that */
	fake_settle();
	fake_settle();
	fake_settle();
	fake_settle();
	fake_settle();
	fake_settle();
	g_assert_cmpuint(count_calls("com.palm.db/merge"), ==, 0);

	g_free(uuid);
	g_free(first);
	teardown();
}

static void test_no_service(void)
{
	LSMessage *m;

	fake_mmsd_own(&fake);
	setup(600);
	fake_settle();

	m = message_new("{\"subscribe\":true}", true);
	g_assert_true(_service_mms_status_query_cb(&palm, m, service));
	assert_json(g_ptr_array_index(m->replies, 0), "returnValue", "true");
	assert_json(g_ptr_array_index(m->replies, 0), "available", "false");
	assert_json(g_ptr_array_index(m->replies, 0), "subscribed", "true");
	g_assert_cmpstr(g_ptr_array_index(subscription_keys, 0), ==, "/mmsStatusQuery");
	message_free(m);

	/* no SIM with MMS: a message fails right away instead of waiting */
	run_activity("[{\"_id\":\"n1\",\"to\":[{\"addr\":\"112\"}],\"messageText\":\"x\"}]");
	expect_merge("n1", NULL, "sending", NULL);
	expect_merge("n1", NULL, "failed", "mmsErrorServiceNotInitialized");

	/* the SIM comes: subscribers hear of it */
	fake_mmsd_add_service(&fake, IMSI, TRUE);
	g_assert_true(fake_wait(last_post_says_available, GINT_TO_POINTER(TRUE)));

	/* and mmsd going away */
	fake_mmsd_release(&fake);
	g_assert_true(fake_wait(last_post_says_available, GINT_TO_POINTER(FALSE)));

	m = message_new("{}", false);
	g_assert_true(_service_mms_status_query_cb(&palm, m, service));
	assert_json(g_ptr_array_index(m->replies, 0), "available", "false");
	assert_json(g_ptr_array_index(m->replies, 0), "subscribed", "false");
	message_free(m);

	teardown();
}

/* A picture of noise, so that it does not compress away, saved below media. */
static gchar* media_picture(const char *name, int width, int height, const char *type, const char *quality)
{
	GdkPixbuf *p = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, width, height);
	GRand *rand = g_rand_new_with_seed(7);
	gchar *path = g_build_filename(media, name, NULL);
	guchar *pixels = gdk_pixbuf_get_pixels(p);
	int x, y, c;

	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			for (c = 0; c < 4; c++)
				pixels[(gsize) y * gdk_pixbuf_get_rowstride(p) + (gsize) x * 4 + c] =
					c == 3 ? 255 : (guchar) g_rand_int_range(rand, 0, 256);
		}
	}

	if (quality)
		g_assert_true(gdk_pixbuf_save(p, path, type, NULL, "quality", quality, NULL));
	else
		g_assert_true(gdk_pixbuf_save(p, path, type, NULL, NULL));

	g_rand_free(rand);
	g_object_unref(p);
	return path;
}

static GBytes* file_bytes(const char *path)
{
	gchar *data = NULL;
	gsize len = 0;

	g_assert_true(g_file_get_contents(path, &data, &len, NULL));
	return g_bytes_new_take(data, len);
}

/* A screenshot larger than a whole MMS goes as a JPEG that fits, a small
 * photo next to it as it is; the user's files stay as they were. */
static void test_send_fits_pictures(void)
{
	gchar *shot = media_picture("shot.png", 1400, 900, "png", NULL);
	gchar *photo = media_picture("photo.jpg", 160, 120, "jpeg", "80");
	GBytes *shot_before = file_bytes(shot);
	GBytes *photo_before = file_bytes(photo);
	GBytes *shot_after;
	GBytes *photo_after;
	GVariantIter *to, *att;
	const char *smil, *id, *ct, *file;
	gchar *results;
	gsize total = 0;
	guint n;

	g_assert_cmpuint(g_bytes_get_size(shot_before), >, MMS_DEFAULT_MESSAGE_SIZE);
	start_with_service(600);

	results = g_strdup_printf("[%s]", "{\"_id\":\"f1\",\"to\":[{\"addr\":\"112\"}],\"messageText\":\"Look\","
	                          "\"attachments\":[{\"path\":\"SHOT\",\"mimeType\":\"image/png\"},{\"path\":\"PHOTO\"}]}");
	{
		gchar **split = g_strsplit(results, "SHOT", -1);

		g_free(results);
		results = g_strjoinv(shot, split);
		g_strfreev(split);
		split = g_strsplit(results, "PHOTO", -1);
		g_free(results);
		results = g_strjoinv(photo, split);
		g_strfreev(split);
	}

	run_activity(results);
	expect_merge("f1", NULL, "sending", NULL);
	g_assert_true(fake_wait(sends_reached, GUINT_TO_POINTER(1)));

	g_variant_get(fake.last_send, "(as&sa(sss))", &to, &smil, &att);
	g_assert_true(g_variant_iter_next(att, "(&s&s&s)", &id, &ct, &file));
	g_assert_cmpstr(ct, ==, "text/plain");
	g_assert_true(g_variant_iter_next(att, "(&s&s&s)", &id, &ct, &file));
	g_assert_cmpstr(id, ==, "<part1.jpg>");
	g_assert_cmpstr(ct, ==, "image/jpeg");
	g_assert_true(g_variant_iter_next(att, "(&s&s&s)", &id, &ct, &file));
	g_assert_cmpstr(id, ==, "<part2.jpg>");
	g_assert_cmpstr(ct, ==, "image/jpeg");
	g_assert_false(g_variant_iter_next(att, "(&s&s&s)", &id, &ct, &file));
	g_variant_iter_free(to);
	g_variant_iter_free(att);

	/* all of it within the size, less what mmsd adds */
	for (n = 0; n < fake.last_send_data->len; n++)
		total += g_bytes_get_size(g_ptr_array_index(fake.last_send_data, n));
	g_assert_cmpuint(total, <=, MMS_DEFAULT_MESSAGE_SIZE - MMS_ENVELOPE_RESERVE);

	/* the screenshot became a JPEG, the photo went untouched */
	{
		const guchar *jpeg = g_bytes_get_data(g_ptr_array_index(fake.last_send_data, 1), NULL);

		g_assert_cmpuint(jpeg[0], ==, 0xff);
		g_assert_cmpuint(jpeg[1], ==, 0xd8);
	}
	g_assert_true(g_bytes_equal(g_ptr_array_index(fake.last_send_data, 2), photo_before));

	/* and the user's files are as they were */
	shot_after = file_bytes(shot);
	photo_after = file_bytes(photo);
	g_assert_true(g_bytes_equal(shot_before, shot_after));
	g_assert_true(g_bytes_equal(photo_before, photo_after));

	expect_merge("f1", NULL, "sending", NULL);
	g_assert_cmpuint(spool_entries(), ==, 0);

	g_bytes_unref(shot_before);
	g_bytes_unref(shot_after);
	g_bytes_unref(photo_before);
	g_bytes_unref(photo_after);
	g_unlink(shot);
	g_unlink(photo);
	g_free(shot);
	g_free(photo);
	g_free(results);
	teardown();
}

/* What cannot be made smaller has to fit as it is; text counts too. */
static void test_send_too_large(void)
{
	gchar *results;
	gchar *path = g_build_filename(media, "clip.bin", NULL);
	gchar *data = g_malloc0(60000);
	gchar *text = g_malloc(70000);

	message_limit = 64 * 1024;
	start_with_service(600);

	/* a file that is not a picture, larger than the message may be */
	g_assert_true(g_file_set_contents(path, data, 60000, NULL));
	memset(text, 'x', 69999);
	text[69999] = '\0';

	/* fits */
	results = g_strdup_printf("[{\"_id\":\"t1\",\"to\":[{\"addr\":\"112\"}],\"filePath\":\"%s\"}]", path);
	run_activity(results);
	g_free(results);
	expect_merge("t1", NULL, "sending", NULL);
	g_assert_true(fake_wait(sends_reached, GUINT_TO_POINTER(1)));
	expect_merge("t1", NULL, "sending", NULL);

	/* the same with a text that takes the room it needs */
	results = g_strdup_printf("[{\"_id\":\"t2\",\"to\":[{\"addr\":\"112\"}],\"messageText\":\"%.5000s\","
	                          "\"filePath\":\"%s\"}]", text, path);
	run_activity(results);
	g_free(results);
	expect_merge("t2", NULL, "sending", NULL);
	expect_merge("t2", NULL, "failed", "mmsErrorAttachmentTooLarge");

	/* a text alone larger than the message */
	results = g_strdup_printf("[{\"_id\":\"t3\",\"to\":[{\"addr\":\"112\"}],\"messageText\":\"%s\"}]", text);
	run_activity(results);
	g_free(results);
	expect_merge("t3", NULL, "sending", NULL);
	expect_merge("t3", NULL, "failed", "mmsErrorAttachmentTooLarge");

	fake_settle();
	g_assert_cmpuint(fake.sends, ==, 1);
	g_assert_cmpuint(spool_entries(), ==, 0);

	message_limit = 0;
	g_unlink(path);
	g_free(path);
	g_free(data);
	g_free(text);
	teardown();
}

static void test_shutdown_while_sending(void)
{
	start_with_service(600);

	run_activity("[{\"_id\":\"s1\",\"to\":[{\"addr\":\"112\"}],\"messageText\":\"x\"}]");
	expect_merge("s1", NULL, "sending", NULL);
	/* the spool is there while SendMessage is under way */
	g_assert_cmpuint(spool_entries(), ==, 1);

	telephony_service_mms_shutdown(service);
	fake_settle();
	/* the cancelled send cleaned its spool and touched nothing else */
	g_assert_cmpuint(spool_entries(), ==, 0);
	g_assert_cmpuint(count_calls("com.palm.db/merge"), ==, 0);

	/* teardown shuts down again: harmless */
	teardown();
}

/* ------------------------------------------------------------------------
 * Receiving
 * ------------------------------------------------------------------------ */

static gchar* write_store(const char *name, const char *data, gsize len)
{
	gchar *path = g_build_filename(tmp, name, NULL);

	g_assert_true(g_file_set_contents(path, data, (gssize) len, NULL));
	return path;
}

static GVariant* received_props(const char *store, guint64 image_len)
{
	return g_variant_new_parsed(
		"{'Status': <'received'>, 'Date': <'2026-11-09T10:00:00+0100'>, 'Sender': <'+31612345678'>,"
		" 'Subject': <'Hi'>, 'Recipients': <['+31612345679']>,"
		" 'Attachments': <[('<smil>', 'application/smil;charset=utf-8', %s, uint64 21, uint64 7),"
		"                  ('<t.txt>', 'text/plain;charset=utf-8', %s, uint64 3, uint64 5),"
		"                  ('<IMG.JPG>', 'image/jpeg', %s, uint64 8, %t)]>}",
		store, store, store, image_len);
}

static void test_receive(void)
{
	gchar *store = write_store("store1", "HDRhelloJPEGDATA<smil><smil/>", 29);
	const char *msg_path = "/org/ofono/mms/" IMSI "/abc123";
	struct call *c;
	gchar *image;
	gchar *data = NULL;
	gsize len = 0;

	start_with_service(600);

	fake_mmsd_emit_message_added(&fake, "/org/ofono/mms/" IMSI, msg_path, received_props(store, 8));

	/* stored already? */
	c = take_call("com.palm.db/find");
	assert_json(c->payload, "query.where.0.prop", "remoteMessageId");
	assert_json(c->payload, "query.where.0.val", "abc123");
	answer(c, "{\"returnValue\":true,\"results\":[]}");

	c = take_call("com.palm.db/put");
	assert_json(c->payload, "objects.0._kind", "com.palm.mmsmessage:1");
	assert_json(c->payload, "objects.0.folder", "inbox");
	assert_json(c->payload, "objects.0.serviceName", "mms");
	assert_json(c->payload, "objects.0.remoteMessageId", "abc123");
	assert_json(c->payload, "objects.0.from.addr", "+31612345678");
	assert_json(c->payload, "objects.0.to.0.addr", "+31612345679");
	assert_json(c->payload, "objects.0.subject", "Hi");
	assert_json(c->payload, "objects.0.messageText", "hello");
	assert_json(c->payload, "objects.0.timestamp", "1794214800000");
	assert_json(c->payload, "objects.0.simId", "0");
	assert_json(c->payload, "objects.0.flags.read", "false");
	/* the SMIL is not a part anybody wants to see */
	assert_json(c->payload, "objects.0.attachments.0.mimeType", "text/plain");
	assert_json(c->payload, "objects.0.attachments.0.partText", "hello");
	assert_json(c->payload, "objects.0.attachments.1.mimeType", "image/jpeg");
	image = g_build_filename(inbox, "abc123", "3-IMG.jpg", NULL);
	assert_json(c->payload, "objects.0.attachments.1.path", image);
	assert_json(c->payload, "objects.0.filePath", image);
	g_assert_true(g_file_get_contents(image, &data, &len, NULL));
	g_assert_cmpmem(data, len, "JPEGDATA", 8);
	g_free(data);
	g_assert_false(fake.deleted->len > 0);

	answer(c, "{\"returnValue\":true}");
	g_assert_true(fake_wait(deleted_reached, GUINT_TO_POINTER(1)));
	g_assert_cmpstr(g_ptr_array_index(fake.deleted, 0), ==, msg_path);

	g_unlink(image);
	g_free(image);
	image = g_build_filename(inbox, "abc123", NULL);
	g_rmdir(image);
	g_free(image);
	g_unlink(store);
	g_free(store);
	teardown();
}

static void test_receive_failures(void)
{
	gchar *store = write_store("store2", "HDRhelloJPEGDATA<smil><smil/>", 29);
	gchar *dir = g_build_filename(inbox, "def456", NULL);
	struct call *c;

	/* held by mmsd when we start: listed by GetMessages */
	fake_mmsd_own(&fake);
	fake_mmsd_add_service(&fake, IMSI, FALSE);
	fake_mmsd_store_message(&fake, "/org/ofono/mms/" IMSI, "/org/ofono/mms/" IMSI "/def456",
	                        received_props(store, 8));
	fake_mmsd_store_message(&fake, "/org/ofono/mms/" IMSI, "/org/ofono/mms/" IMSI "/old789",
	                        received_props(store, 8));
	/* a draft of ours from before: left alone */
	fake_mmsd_store_message(&fake, "/org/ofono/mms/" IMSI, "/org/ofono/mms/" IMSI "/draft1",
	                        g_variant_new_parsed("{'Status': <'draft'>}"));
	setup(600);

	/* def456: db8 cannot store it; what was extracted goes again, mmsd keeps it */
	c = take_call("com.palm.db/find");
	assert_json(c->payload, "query.where.0.val", "def456");
	answer(c, "{\"returnValue\":true,\"results\":[]}");
	/* old789: in db8 already, from a run that did not get to delete it */
	c = take_call("com.palm.db/find");
	assert_json(c->payload, "query.where.0.val", "old789");
	answer(c, "{\"returnValue\":true,\"results\":[{\"_id\":\"x\"}]}");
	g_assert_true(fake_wait(deleted_reached, GUINT_TO_POINTER(1)));
	g_assert_cmpstr(g_ptr_array_index(fake.deleted, 0), ==, "/org/ofono/mms/" IMSI "/old789");

	c = take_call("com.palm.db/put");
	g_assert_true(g_file_test(dir, G_FILE_TEST_IS_DIR));
	answer(c, "{\"returnValue\":false,\"errorText\":\"no\"}");
	g_assert_false(g_file_test(dir, G_FILE_TEST_EXISTS));
	fake_settle();
	g_assert_cmpuint(fake.deleted->len, ==, 1);

	/* announced again later: imported clean */
	fake_mmsd_emit_message_added(&fake, "/org/ofono/mms/" IMSI, "/org/ofono/mms/" IMSI "/def456",
	                             received_props(store, 8));
	answer(take_call("com.palm.db/find"), "{\"returnValue\":true,\"results\":[]}");
	answer(take_call("com.palm.db/put"), "{\"returnValue\":true}");
	g_assert_true(fake_wait(deleted_reached, GUINT_TO_POINTER(2)));

	/* a part that lies about its length: nothing stored, nothing deleted, nothing left */
	fake_mmsd_emit_message_added(&fake, "/org/ofono/mms/" IMSI, "/org/ofono/mms/" IMSI "/bad1",
	                             received_props(store, 1000));
	answer(take_call("com.palm.db/find"), "{\"returnValue\":true,\"results\":[]}");
	fake_settle();
	g_assert_cmpuint(count_calls("com.palm.db/put"), ==, 0);
	g_assert_cmpuint(fake.deleted->len, ==, 2);
	{
		gchar *bad = g_build_filename(inbox, "bad1", NULL);

		g_assert_false(g_file_test(bad, G_FILE_TEST_EXISTS));
		g_free(bad);
	}

	/* a find that fails: left for later */
	fake_mmsd_emit_message_added(&fake, "/org/ofono/mms/" IMSI, "/org/ofono/mms/" IMSI "/later1",
	                             received_props(store, 8));
	answer(take_call("com.palm.db/find"), "{\"returnValue\":false}");
	fake_settle();
	g_assert_cmpuint(count_calls("com.palm.db/put"), ==, 0);

	{
		gchar *f = g_build_filename(dir, "3-IMG.jpg", NULL);

		g_unlink(f);
		g_free(f);
		g_rmdir(dir);
	}
	g_free(dir);
	g_unlink(store);
	g_free(store);
	teardown();
}

int main(int argc, char **argv)
{
	int ret;

	g_test_init(&argc, &argv, NULL);
	/* the daemon warns about every message it refuses; that is expected
	 * here, only criticals are bugs */
	g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);

	sim0.imsi = (gchar *) IMSI;

	tmp = g_dir_make_tmp("test_telephonyservice_mms-XXXXXX", NULL);
	g_assert_nonnull(tmp);
	media = g_strdup_printf("%s/media/", tmp);
	spool = g_build_filename(tmp, "spool", NULL);
	inbox = g_build_filename(tmp, "inbox", NULL);
	g_assert_cmpint(g_mkdir(media, 0700), ==, 0);

	fake_mmsd_start(&fake);

	g_test_add_func("/telephonyservice_mms/send-and-sent", test_send_and_sent);
	g_test_add_func("/telephonyservice_mms/refusals", test_refusals);
	g_test_add_func("/telephonyservice_mms/send-fits-pictures", test_send_fits_pictures);
	g_test_add_func("/telephonyservice_mms/send-too-large", test_send_too_large);
	g_test_add_func("/telephonyservice_mms/one-at-a-time", test_one_at_a_time);
	g_test_add_func("/telephonyservice_mms/timeout-and-draft", test_timeout_and_draft);
	g_test_add_func("/telephonyservice_mms/no-service", test_no_service);
	g_test_add_func("/telephonyservice_mms/shutdown-while-sending", test_shutdown_while_sending);
	g_test_add_func("/telephonyservice_mms/receive", test_receive);
	g_test_add_func("/telephonyservice_mms/receive-failures", test_receive_failures);

	ret = g_test_run();

	fake_mmsd_stop(&fake);

	{
		gchar *pic = g_build_filename(media, "pic.jpg", NULL);

		g_unlink(pic);
		g_free(pic);
	}
	g_rmdir(media);
	g_rmdir(spool);
	g_rmdir(inbox);
	g_rmdir(tmp);
	g_free(media);
	g_free(spool);
	g_free(inbox);
	g_free(tmp);

	return ret;
}

// vim:ts=4:sw=4:noexpandtab
