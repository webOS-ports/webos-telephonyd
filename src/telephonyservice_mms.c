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

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <pbnjson.h>
#include <luna-service2/lunaservice.h>

#include "telephonyservice.h"
#include "telephonyservice_internal.h"
#include "telephonyservice_mms.h"
#include "luna_service_utils.h"
#include "mmscore.h"
#include "mmsdclient.h"
#include "mmsjson.h"
#include "timeutils.h"

#define MMS_ACTIVITY "telephony-send-outgoing-mms"
#define STATUS_METHOD "mmsStatusQuery"

const struct telephony_mms_config telephony_mms_default_config = {
	.bus_type = G_BUS_TYPE_SYSTEM,
	.spool_dir = "/var/lib/webos-telephonyd/mms-spool",
	.inbox_dir = "/media/internal/.mms-attachments",
	.spool_gid = (gid_t) -1,
	.roots = mms_default_roots,
	.send_timeout_s = 600,
};

struct inflight;

struct telephony_mms {
	struct telephony_service *service;
	struct mmsd_client *client;
	gchar *spool_dir;
	gchar *inbox_dir;
	gid_t spool_gid;
	bool spool_gid_known;
	const char * const *roots;
	guint send_timeout_s;

	GQueue *queue;			/* struct mms_outgoing* waiting to go */
	GHashTable *known_ids;		/* db _id -> NULL, queued or in flight */
	struct inflight *inflight;	/* the one SendMessage under way */
	GHashTable *timeouts;		/* mmsd uuid -> struct send_timeout* */
	GHashTable *importing;		/* mmsd uuid -> NULL */
};

struct inflight {
	struct telephony_mms *mms;	/* NULL once the service shut down */
	struct mms_outgoing *msg;
	gchar *spool;
	GPtrArray *parts;		/* struct mms_send_part* */
};

struct send_timeout {
	struct telephony_mms *mms;
	gchar *uuid;
	guint source;
};

struct import {
	struct telephony_mms *mms;
	gchar *message_path;
	gchar *uuid;
	gchar *service_path;
	GVariant *props;
	gchar *dir;			/* where its parts went */
	GPtrArray *files;		/* gchar*, the files written there */
};

static void process_queue(struct telephony_mms *mms);

static gint64 now_ms(void)
{
	return g_get_real_time() / 1000;
}

/* ------------------------------------------------------------------------
 * db8 and activity manager
 * ------------------------------------------------------------------------ */

static void db_call(struct telephony_mms *mms, const char *method, jvalue_ref req,
                    LSFilterFunc callback, void *user_data)
{
	gchar *uri = g_strdup_printf("luna://com.palm.db/%s", method);

	if (!luna_service_call_validate_and_send(mms->service->palmHandle, uri, req, callback, user_data))
		g_warning("[MMS] db8 %s could not be sent", method);

	g_free(uri);
}

static void set_status(struct telephony_mms *mms, const char *id, const char *remote_id, const char *status,
                       const char *error_code)
{
	jvalue_ref req = mms_status_merge_request(id, remote_id, status, error_code);

	g_message("[MMS] message %s: %s%s%s", id ? id : remote_id, status, error_code ? " " : "",
	          error_code ? error_code : "");
	db_call(mms, "merge", req, NULL, NULL);
	j_release(&req);
}

static void restart_activity(struct telephony_mms *mms)
{
	jvalue_ref req = jobject_create();

	jobject_put(req, J_CSTR_TO_JVAL("activityName"), jstring_create(MMS_ACTIVITY));
	jobject_put(req, J_CSTR_TO_JVAL("restart"), jboolean_create(true));

	if (!luna_service_call_validate_and_send(mms->service->palmHandle,
	                                         TELEPHONY_ACTIVITY_COMPLETE_URI, req, NULL, NULL))
		g_warning("[MMS] could not re-arm the outgoing MMS activity");

	j_release(&req);
}

/* The results array of a db8 reply, or NULL; *parsed has to be released. */
static jvalue_ref reply_results(LSMessage *message, jvalue_ref *parsed)
{
	jvalue_ref results = NULL;
	jvalue_ref rv = NULL;
	bool ok = false;

	*parsed = luna_service_message_parse_and_validate(LSMessageGetPayload(message));
	if (jis_null(*parsed))
		return NULL;

	if (jobject_get_exists(*parsed, J_CSTR_TO_BUF("returnValue"), &rv))
		jboolean_get(rv, &ok);

	if (!ok || !jobject_get_exists(*parsed, J_CSTR_TO_BUF("results"), &results) || !jis_array(results))
		return NULL;

	return results;
}

static bool reply_ok(LSMessage *message)
{
	jvalue_ref parsed = luna_service_message_parse_and_validate(LSMessageGetPayload(message));
	jvalue_ref rv = NULL;
	bool ok = false;

	if (!jis_null(parsed) && jobject_get_exists(parsed, J_CSTR_TO_BUF("returnValue"), &rv))
		jboolean_get(rv, &ok);

	j_release(&parsed);
	return ok;
}

/* ------------------------------------------------------------------------
 * SIM slots
 * ------------------------------------------------------------------------ */

static const char* identity_for_sim(struct telephony_mms *mms, int sim_id)
{
	struct telephony_sim_state *sim = NULL;

	if (sim_id >= 0)
		sim = telephony_service_sim_state(mms->service, sim_id);

	if (!sim)
		sim = telephony_service_sim_state(mms->service,
		                                  telephony_service_get_default_sim(mms->service, TELEPHONY_SIM_ROLE_SMS));

	return sim ? sim->imsi : NULL;
}

static int sim_for_service(struct telephony_mms *mms, const char *service_path)
{
	const char *identity = strrchr(service_path, '/');
	int sim_id;

	if (!identity)
		return -1;

	for (sim_id = 0; sim_id < TELEPHONY_MAX_SIMS; sim_id++) {
		const struct telephony_sim_state *sim = telephony_service_sim_state(mms->service, sim_id);

		if (sim && sim->imsi && strcmp(sim->imsi, identity + 1) == 0)
			return sim_id;
	}

	return -1;
}

/* ------------------------------------------------------------------------
 * Sending
 * ------------------------------------------------------------------------ */

static void remove_spool(const char *spool, GPtrArray *parts)
{
	guint n;

	if (!spool)
		return;

	for (n = 0; parts && n < parts->len; n++) {
		const struct mms_send_part *part = g_ptr_array_index(parts, n);

		if (part->file && g_unlink(part->file) < 0 && errno != ENOENT)
			g_warning("[MMS] could not remove a spooled part: %s", g_strerror(errno));
	}

	if (g_rmdir(spool) < 0)
		g_warning("[MMS] could not remove the spool directory: %s", g_strerror(errno));
}

static void inflight_free(struct inflight *in)
{
	remove_spool(in->spool, in->parts);
	g_free(in->spool);
	if (in->parts)
		g_ptr_array_free(in->parts, TRUE);
	mms_outgoing_free(in->msg);
	g_free(in);
}

static void fail_message(struct telephony_mms *mms, const char *id, const char *error_code)
{
	set_status(mms, id, NULL, "failed", error_code);
	g_hash_table_remove(mms->known_ids, id);
}

static struct mms_send_part* add_part(struct inflight *in, const char *name, const char *content_type)
{
	struct mms_send_part *part = g_new0(struct mms_send_part, 1);

	part->content_id = g_strdup_printf("<%s>", name);
	part->smil_src = g_strdup_printf("cid:%s", name);
	part->content_type = g_strdup(content_type);
	part->file = g_build_filename(in->spool, name, NULL);
	g_ptr_array_add(in->parts, part);

	return part;
}

static gchar* file_content_type(const struct mms_file *file)
{
	gchar *bare = mms_content_type_bare(file->content_type);

	if (bare && mms_content_type_valid(bare))
		return bare;

	g_free(bare);
	return g_strdup(mms_content_type_for_name(file->path));
}

/* Copies text and files into a fresh spool directory. NULL when that worked,
 * else the error code to fail the message with. */
static const char* spool_message(struct telephony_mms *mms, struct inflight *in)
{
	gsize used = 0;
	gchar *tmpl;
	GError *error = NULL;
	int dir_fd;
	guint n;

	if (!mms->spool_gid_known)
		return MMS_ERR_NOT_READY;

	if (g_mkdir_with_parents(mms->spool_dir, 0755) < 0) {
		g_warning("[MMS] cannot create the spool: %s", g_strerror(errno));
		return MMS_ERR_UNSPECIFIED;
	}

	tmpl = g_build_filename(mms->spool_dir, "msg-XXXXXX", NULL);
	if (!g_mkdtemp_full(tmpl, 0700)) {
		g_warning("[MMS] cannot create a spool directory: %s", g_strerror(errno));
		g_free(tmpl);
		return MMS_ERR_UNSPECIFIED;
	}
	in->spool = tmpl;

	/* mmsd (its group) may read it, nobody else */
	dir_fd = open(in->spool, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (dir_fd < 0 || fchown(dir_fd, (uid_t) -1, mms->spool_gid) < 0 || fchmod(dir_fd, 0750) < 0) {
		g_warning("[MMS] cannot hand the spool directory to mmsd: %s", g_strerror(errno));
		if (dir_fd >= 0)
			close(dir_fd);
		return MMS_ERR_UNSPECIFIED;
	}
	close(dir_fd);

	if (in->msg->text && *in->msg->text) {
		const struct mms_send_part *part = add_part(in, "text_0.txt", "text/plain");
		gsize len = strlen(in->msg->text);

		if (!mms_write_new_file(part->file, in->msg->text, len, 0640, mms->spool_gid, &error)) {
			g_warning("[MMS] cannot spool the text: %s", error->message);
			g_clear_error(&error);
			return MMS_ERR_UNSPECIFIED;
		}
		used += len;
	}

	for (n = 0; n < in->msg->files->len; n++) {
		const struct mms_file *file = g_ptr_array_index(in->msg->files, n);
		const char *error_code = NULL;
		const struct mms_send_part *part;
		gchar *content_type;
		gchar *name;
		gsize size = 0;
		int fd;

		if (used >= MMS_MAX_TOTAL_SIZE)
			return MMS_ERR_TOO_LARGE;

		fd = mms_open_file(file->path, mms->roots, MMS_MAX_TOTAL_SIZE - used, &size, &error_code);
		if (fd < 0) {
			g_warning("[MMS] message %s: file %u cannot go (%s)", in->msg->id, n, error_code);
			return error_code;
		}

		content_type = file_content_type(file);
		name = g_strdup_printf("part%u.%s", n + 1, mms_extension_for_content_type(content_type));
		part = add_part(in, name, content_type);

		if (!mms_copy_fd_to_file(fd, part->file, MMS_MAX_TOTAL_SIZE - used, 0640, mms->spool_gid, &error)) {
			g_warning("[MMS] cannot spool file %u: %s", n, error->message);
			g_clear_error(&error);
			g_free(content_type);
			g_free(name);
			close(fd);
			return MMS_ERR_UNSPECIFIED;
		}

		used += size;
		g_free(content_type);
		g_free(name);
		close(fd);
	}

	return in->parts->len > 0 ? NULL : MMS_ERR_UNSUPPORTED;
}

static gboolean send_timed_out(gpointer user_data)
{
	struct send_timeout *t = user_data;

	t->source = 0;
	set_status(t->mms, NULL, t->uuid, "failed", MMS_ERR_NETWORK);
	g_hash_table_remove(t->mms->timeouts, t->uuid);

	return G_SOURCE_REMOVE;
}

static void send_timeout_free(gpointer data)
{
	struct send_timeout *t = data;

	if (t->source)
		g_source_remove(t->source);
	g_free(t->uuid);
	g_free(t);
}

static void send_done(const char *message_path, const GError *error, gpointer user_data)
{
	struct inflight *in = user_data;
	struct telephony_mms *mms = in->mms;

	if (!mms) {
		inflight_free(in);
		return;
	}

	mms->inflight = NULL;
	g_hash_table_remove(mms->known_ids, in->msg->id);

	if (error) {
		gchar *remote = g_dbus_error_get_remote_error(error);

		g_warning("[MMS] mmsd did not take message %s: %s", in->msg->id, remote ? remote : error->message);
		set_status(mms, in->msg->id, NULL, "failed", mms_error_code_for_dbus_error(remote));
		g_free(remote);
	}
	else {
		gchar *uuid = mms_path_basename(message_path);
		struct send_timeout *t = g_new0(struct send_timeout, 1);

		set_status(mms, in->msg->id, uuid, "sending", NULL);

		t->mms = mms;
		t->uuid = uuid;
		t->source = g_timeout_add_seconds(mms->send_timeout_s, send_timed_out, t);
		g_hash_table_replace(mms->timeouts, t->uuid, t);
	}

	inflight_free(in);
	process_queue(mms);
}

static void process_queue(struct telephony_mms *mms)
{
	while (!mms->inflight && !g_queue_is_empty(mms->queue)) {
		struct mms_outgoing *msg = g_queue_pop_head(mms->queue);
		const char *service_path = mmsd_client_pick_service(mms->client, identity_for_sim(mms, msg->sim_id));
		struct inflight *in;
		const char *error_code;
		gchar *smil;

		if (!service_path) {
			fail_message(mms, msg->id, MMS_ERR_NOT_READY);
			mms_outgoing_free(msg);
			continue;
		}

		in = g_new0(struct inflight, 1);
		in->mms = mms;
		in->msg = msg;
		in->parts = g_ptr_array_new_with_free_func((GDestroyNotify) mms_send_part_free);

		error_code = spool_message(mms, in);
		if (error_code) {
			fail_message(mms, msg->id, error_code);
			inflight_free(in);
			continue;
		}

		smil = mms_build_smil(in->parts);
		mms->inflight = in;

		g_message("[MMS] handing message %s to mmsd (%u recipient(s), %u part(s))", msg->id,
		          msg->recipients->len, in->parts->len);
		mmsd_client_send(mms->client, service_path, msg->recipients, smil, in->parts, send_done, in);
		g_free(smil);
	}
}

static bool find_outbox_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct telephony_mms *mms = user_data;
	jvalue_ref parsed = NULL;
	jvalue_ref results = reply_results(message, &parsed);
	ssize_t n;

	for (n = 0; results && n < jarray_size(results); n++) {
		const char *error_code = NULL;
		gchar *bad_id = NULL;
		struct mms_outgoing *msg = mms_outgoing_from_record(jarray_get(results, n), &bad_id, &error_code);

		if (!msg) {
			if (bad_id)
				fail_message(mms, bad_id, error_code);
			else
				g_warning("[MMS] an outgoing MMS without an id; skipped");
			g_free(bad_id);
			continue;
		}

		if (g_hash_table_contains(mms->known_ids, msg->id)) {
			mms_outgoing_free(msg);
			continue;
		}

		g_hash_table_add(mms->known_ids, g_strdup(msg->id));
		set_status(mms, msg->id, NULL, "sending", NULL);
		g_queue_push_tail(mms->queue, msg);
	}

	j_release(&parsed);

	restart_activity(mms);
	process_queue(mms);

	return true;
}

bool _service_internal_send_mms_from_db_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct telephony_service *service = user_data;
	struct telephony_mms *mms = service->mms;
	jvalue_ref req;
	jvalue_ref query;
	jvalue_ref where;
	jvalue_ref clause;

	luna_service_message_reply_success(handle, message);

	if (!mms)
		return true;

	req = jobject_create();
	query = jobject_create();
	where = jarray_create(NULL);

	clause = jobject_create();
	jobject_put(clause, J_CSTR_TO_JVAL("prop"), jstring_create("folder"));
	jobject_put(clause, J_CSTR_TO_JVAL("op"), jstring_create("="));
	jobject_put(clause, J_CSTR_TO_JVAL("val"), jstring_create("outbox"));
	jarray_append(where, clause);

	clause = jobject_create();
	jobject_put(clause, J_CSTR_TO_JVAL("prop"), jstring_create("status"));
	jobject_put(clause, J_CSTR_TO_JVAL("op"), jstring_create("="));
	jobject_put(clause, J_CSTR_TO_JVAL("val"), jstring_create("pending"));
	jarray_append(where, clause);

	jobject_put(query, J_CSTR_TO_JVAL("from"), jstring_create(MMS_DB_KIND));
	jobject_put(query, J_CSTR_TO_JVAL("where"), where);
	jobject_put(query, J_CSTR_TO_JVAL("orderBy"), jstring_create("localTimestamp"));
	jobject_put(req, J_CSTR_TO_JVAL("query"), query);

	db_call(mms, "find", req, find_outbox_cb, mms);
	j_release(&req);

	return true;
}

static void message_sent(struct telephony_mms *mms, const char *message_path)
{
	gchar *uuid = mms_path_basename(message_path);

	if (!uuid)
		return;

	g_hash_table_remove(mms->timeouts, uuid);
	set_status(mms, NULL, uuid, "successful", NULL);
	/* the record has it all; mmsd need not keep its copy */
	mmsd_client_delete_message(mms->client, message_path);
	g_free(uuid);
}

static void message_status(struct mmsd_client *client, const char *message_path, const char *status,
                           gpointer user_data)
{
	struct telephony_mms *mms = user_data;
	gchar *uuid;

	if (strcmp(status, "sent") == 0) {
		message_sent(mms, message_path);
		return;
	}

	uuid = mms_path_basename(message_path);
	/* "draft" again after mmsd tried: it gave up on a message we sent */
	if (uuid && strcmp(status, "draft") == 0 && g_hash_table_contains(mms->timeouts, uuid)) {
		g_hash_table_remove(mms->timeouts, uuid);
		set_status(mms, NULL, uuid, "failed", MMS_ERR_NETWORK);
	}
	g_free(uuid);
}

/* ------------------------------------------------------------------------
 * Receiving
 * ------------------------------------------------------------------------ */

static void import_free(struct import *imp)
{
	if (imp->mms)
		g_hash_table_remove(imp->mms->importing, imp->uuid);
	g_free(imp->message_path);
	g_free(imp->uuid);
	g_free(imp->service_path);
	if (imp->props)
		g_variant_unref(imp->props);
	g_ptr_array_free(imp->files, TRUE);
	g_free(imp->dir);
	g_free(imp);
}

/* Takes back what an import that failed wrote, so that the next try starts
 * clean. */
static void remove_import_files(struct import *imp)
{
	guint n;

	for (n = 0; n < imp->files->len; n++)
		g_unlink(g_ptr_array_index(imp->files, n));
	g_ptr_array_set_size(imp->files, 0);

	if (imp->dir)
		g_rmdir(imp->dir);
}

static bool uuid_usable(const char *uuid)
{
	const char *p;

	if (!uuid || !*uuid || strlen(uuid) > 128)
		return false;

	for (p = uuid; *p; p++) {
		if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_')
			return false;
	}

	return true;
}

/* Copies one part out of mmsd's store; false on an error. */
static bool extract_part(struct import *imp, struct mms_received *msg, guint index, const char *content_id,
                         const char *content_type, const char *file, guint64 offset, guint64 len)
{
	struct mms_received_part *part;
	GError *error = NULL;
	gchar *bare = mms_content_type_bare(content_type);
	GBytes *data;
	gconstpointer bytes;
	gsize size;

	if (!bare || g_str_has_prefix(bare, "application/smil")) {
		g_free(bare);
		return true;
	}

	data = mms_read_range(file, offset, len, &error);
	if (!data) {
		g_warning("[MMS] message %s: part %u unreadable: %s", imp->uuid, index, error->message);
		g_clear_error(&error);
		g_free(bare);
		return false;
	}

	bytes = g_bytes_get_data(data, &size);
	part = g_new0(struct mms_received_part, 1);
	part->content_type = bare;

	if (strcmp(bare, "text/plain") == 0) {
		part->text = mms_part_text(bytes, size, content_type);
	}
	else {
		if (!imp->dir) {
			imp->dir = g_build_filename(imp->mms->inbox_dir, imp->uuid, NULL);
			if (g_mkdir_with_parents(imp->dir, 0755) < 0) {
				g_warning("[MMS] cannot create the directory for received parts: %s", g_strerror(errno));
				g_clear_pointer(&imp->dir, g_free);
				mms_received_part_free(part);
				g_bytes_unref(data);
				return false;
			}
		}

		part->name = mms_received_file_name(content_id, index, content_type);
		part->path = g_build_filename(imp->dir, part->name, NULL);

		if (!mms_write_new_file(part->path, bytes, size, 0644, (gid_t) -1, &error)) {
			g_warning("[MMS] message %s: part %u not stored: %s", imp->uuid, index, error->message);
			g_clear_error(&error);
			mms_received_part_free(part);
			g_bytes_unref(data);
			return false;
		}
		g_ptr_array_add(imp->files, g_strdup(part->path));
	}

	g_ptr_array_add(msg->parts, part);
	g_bytes_unref(data);
	return true;
}

static struct mms_received* read_received(struct import *imp, bool *ok)
{
	struct mms_received *msg = mms_received_new(imp->uuid);
	const char *str = NULL;
	GVariant *attachments;
	GVariant *recipients;

	*ok = true;
	msg->local_timestamp_ms = now_ms();
	msg->sim_id = sim_for_service(imp->mms, imp->service_path);

	if (g_variant_lookup(imp->props, "Sender", "&s", &str))
		msg->sender = g_strdup(str);
	if (g_variant_lookup(imp->props, "Subject", "&s", &str))
		msg->subject = g_strdup(str);
	if (g_variant_lookup(imp->props, "Date", "&s", &str)) {
		time_t t = telephony_decode_iso8601_time(str);

		if (t != (time_t) -1)
			msg->timestamp_ms = (gint64) t * 1000;
	}

	recipients = g_variant_lookup_value(imp->props, "Recipients", G_VARIANT_TYPE("as"));
	if (recipients) {
		GVariantIter iter;

		g_variant_iter_init(&iter, recipients);
		while (g_variant_iter_next(&iter, "&s", &str))
			g_ptr_array_add(msg->recipients, g_strdup(str));
		g_variant_unref(recipients);
	}

	attachments = g_variant_lookup_value(imp->props, "Attachments", G_VARIANT_TYPE("a(ssstt)"));
	if (attachments) {
		GVariantIter iter;
		const char *content_id;
		const char *content_type;
		const char *file;
		guint64 offset;
		guint64 len;
		guint index = 0;

		g_variant_iter_init(&iter, attachments);
		while (*ok && g_variant_iter_next(&iter, "(&s&s&stt)", &content_id, &content_type, &file, &offset, &len))
			*ok = extract_part(imp, msg, ++index, content_id, content_type, file, offset, len);
		g_variant_unref(attachments);
	}

	return msg;
}

static bool put_received_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct import *imp = user_data;

	if (reply_ok(message)) {
		g_message("[MMS] received message %s stored", imp->uuid);
		mmsd_client_delete_message(imp->mms->client, imp->message_path);
	}
	else {
		/* mmsd keeps it; the next start tries again */
		g_warning("[MMS] received message %s could not be stored", imp->uuid);
		remove_import_files(imp);
	}

	import_free(imp);
	return true;
}

static void store_received(struct import *imp)
{
	bool ok = false;
	struct mms_received *msg = read_received(imp, &ok);
	jvalue_ref req;
	jvalue_ref list;

	if (!ok) {
		remove_import_files(imp);
		mms_received_free(msg);
		import_free(imp);
		return;
	}

	req = jobject_create();
	list = jarray_create(NULL);
	jarray_append(list, mms_received_record(msg));
	jobject_put(req, J_CSTR_TO_JVAL("objects"), list);

	db_call(imp->mms, "put", req, put_received_cb, imp);

	j_release(&req);
	mms_received_free(msg);
}

static bool find_received_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct import *imp = user_data;
	jvalue_ref parsed = NULL;
	jvalue_ref results = reply_results(message, &parsed);

	if (!results) {
		g_warning("[MMS] cannot check whether message %s is stored already", imp->uuid);
		import_free(imp);
	}
	else if (jarray_size(results) > 0) {
		/* stored before, but mmsd's copy was not deleted then */
		mmsd_client_delete_message(imp->mms->client, imp->message_path);
		import_free(imp);
	}
	else {
		store_received(imp);
	}

	j_release(&parsed);
	return true;
}

static void import_received(struct telephony_mms *mms, const char *service_path, const char *message_path,
                            GVariant *props)
{
	gchar *uuid = mms_path_basename(message_path);
	struct import *imp;
	jvalue_ref req;
	jvalue_ref query;
	jvalue_ref where;
	jvalue_ref clause;

	if (!uuid_usable(uuid)) {
		g_warning("[MMS] a received message with an unusable path; left in mmsd");
		g_free(uuid);
		return;
	}

	if (g_hash_table_contains(mms->importing, uuid)) {
		g_free(uuid);
		return;
	}

	imp = g_new0(struct import, 1);
	imp->mms = mms;
	imp->uuid = uuid;
	imp->message_path = g_strdup(message_path);
	imp->service_path = g_strdup(service_path);
	imp->props = g_variant_ref(props);
	imp->files = g_ptr_array_new_with_free_func(g_free);
	g_hash_table_add(mms->importing, g_strdup(uuid));

	req = jobject_create();
	query = jobject_create();
	where = jarray_create(NULL);
	clause = jobject_create();

	jobject_put(clause, J_CSTR_TO_JVAL("prop"), jstring_create("remoteMessageId"));
	jobject_put(clause, J_CSTR_TO_JVAL("op"), jstring_create("="));
	jobject_put(clause, J_CSTR_TO_JVAL("val"), jstring_create(uuid));
	jarray_append(where, clause);
	jobject_put(query, J_CSTR_TO_JVAL("from"), jstring_create(MMS_DB_KIND));
	jobject_put(query, J_CSTR_TO_JVAL("where"), where);
	jobject_put(query, J_CSTR_TO_JVAL("limit"), jnumber_create_i32(1));
	jobject_put(req, J_CSTR_TO_JVAL("query"), query);

	db_call(mms, "find", req, find_received_cb, imp);
	j_release(&req);
}

static void message_added(struct mmsd_client *client, const char *service_path, const char *message_path,
                          GVariant *props, gpointer user_data)
{
	struct telephony_mms *mms = user_data;
	const char *status = NULL;

	if (!g_variant_lookup(props, "Status", "&s", &status))
		return;

	if (strcmp(status, "received") == 0)
		import_received(mms, service_path, message_path, props);
	else if (strcmp(status, "sent") == 0)
		/* sent while we were not listening */
		message_sent(mms, message_path);
}

/* ------------------------------------------------------------------------
 * Availability
 * ------------------------------------------------------------------------ */

static jvalue_ref status_reply(struct telephony_mms *mms)
{
	jvalue_ref reply = jobject_create();

	jobject_put(reply, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
	jobject_put(reply, J_CSTR_TO_JVAL("available"),
	            jboolean_create(mms && mmsd_client_service_count(mms->client) > 0));

	return reply;
}

static void availability_changed(struct mmsd_client *client, gboolean available, gpointer user_data)
{
	struct telephony_mms *mms = user_data;
	jvalue_ref reply = status_reply(mms);

	g_message("[MMS] MMS is %savailable", available ? "" : "not ");
	telephony_service_post_subscription(mms->service, STATUS_METHOD, reply);
	j_release(&reply);
}

bool _service_mms_status_query_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct telephony_service *service = user_data;
	jvalue_ref reply = status_reply(service->mms);
	bool subscribed = luna_service_check_for_subscription_with_key(handle, message, "/" STATUS_METHOD);

	jobject_put(reply, J_CSTR_TO_JVAL("subscribed"), jboolean_create(subscribed));
	luna_service_message_validate_and_send(handle, message, reply);
	j_release(&reply);

	return true;
}

/* ------------------------------------------------------------------------
 * Setup
 * ------------------------------------------------------------------------ */

static const struct mmsd_client_callbacks client_callbacks = {
	.availability_changed = availability_changed,
	.message_added = message_added,
	.message_status = message_status,
};

void telephony_service_mms_setup(struct telephony_service *service, const struct telephony_mms_config *config)
{
	struct telephony_mms *mms;

	if (!config)
		config = &telephony_mms_default_config;

	mms = g_new0(struct telephony_mms, 1);
	mms->service = service;
	mms->spool_dir = g_strdup(config->spool_dir);
	mms->inbox_dir = g_strdup(config->inbox_dir);
	mms->roots = config->roots;
	mms->send_timeout_s = config->send_timeout_s;
	mms->queue = g_queue_new();
	mms->known_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	mms->timeouts = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, send_timeout_free);
	mms->importing = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	if (config->spool_gid != (gid_t) -1) {
		mms->spool_gid = config->spool_gid;
		mms->spool_gid_known = true;
	}
	else {
		struct group grp;
		struct group *found = NULL;
		char buf[4096];

		if (getgrnam_r("mmsd", &grp, buf, sizeof(buf), &found) == 0 && found) {
			mms->spool_gid = found->gr_gid;
			mms->spool_gid_known = true;
		}
		else {
			g_warning("[MMS] there is no group mmsd; MMS cannot be sent");
		}
	}

	service->mms = mms;
	mms->client = mmsd_client_new(config->bus_type, &client_callbacks, mms);

	restart_activity(mms);
}

void telephony_service_mms_shutdown(struct telephony_service *service)
{
	struct telephony_mms *mms = service->mms;

	if (!mms)
		return;

	service->mms = NULL;

	/* the reply to a SendMessage under way comes after this; it only
	 * cleans up its spool then */
	if (mms->inflight)
		mms->inflight->mms = NULL;

	mmsd_client_free(mms->client);

	g_queue_free_full(mms->queue, (GDestroyNotify) mms_outgoing_free);
	g_hash_table_destroy(mms->known_ids);
	g_hash_table_destroy(mms->timeouts);
	g_hash_table_destroy(mms->importing);
	g_free(mms->spool_dir);
	g_free(mms->inbox_dir);
	g_free(mms);
}

// vim:ts=4:sw=4:noexpandtab
