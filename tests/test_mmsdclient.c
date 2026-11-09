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
 * The mmsd client against a fake mmsd on a private bus (tests/fake_mmsd.c):
 * following the name and its services, SendMessage's arguments and errors,
 * the signals it takes and the ones it must not take.
 */

#include <string.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>

#include "mmsdclient.h"
#include "mmscore.h"
#include "fake_mmsd.h"

static struct fake_mmsd fake;
static gchar *tmp;

struct recorder {
	int available_calls;
	gboolean available;
	GPtrArray *added;	/* "service message status" */
	GPtrArray *statuses;	/* "message status" */
	int sends_done;
	gchar *sent_path;
	GError *send_error;
};

static struct recorder rec;

static void rec_reset(void)
{
	if (rec.added)
		g_ptr_array_free(rec.added, TRUE);
	if (rec.statuses)
		g_ptr_array_free(rec.statuses, TRUE);
	g_free(rec.sent_path);
	g_clear_error(&rec.send_error);
	memset(&rec, 0, sizeof(rec));
	rec.added = g_ptr_array_new_with_free_func(g_free);
	rec.statuses = g_ptr_array_new_with_free_func(g_free);
}

static void on_available(struct mmsd_client *client, gboolean available, gpointer user_data)
{
	g_assert_true(user_data == &rec);
	rec.available_calls++;
	rec.available = available;
}

static void on_added(struct mmsd_client *client, const char *service_path, const char *message_path,
                     GVariant *props, gpointer user_data)
{
	const char *status = "?";

	g_variant_lookup(props, "Status", "&s", &status);
	g_ptr_array_add(rec.added, g_strdup_printf("%s %s %s", service_path, message_path, status));
}

static void on_status(struct mmsd_client *client, const char *message_path, const char *status,
                      gpointer user_data)
{
	g_ptr_array_add(rec.statuses, g_strdup_printf("%s %s", message_path, status));
}

static const struct mmsd_client_callbacks callbacks = { on_available, on_added, on_status };

static void on_sent(const char *message_path, const GError *error, gpointer user_data)
{
	g_assert_true(user_data == &rec);
	rec.sends_done++;
	g_free(rec.sent_path);
	rec.sent_path = g_strdup(message_path);
	g_clear_error(&rec.send_error);
	if (error)
		rec.send_error = g_error_copy(error);
}

static gboolean is_available(gpointer data)
{
	return rec.available;
}

static gboolean is_unavailable(gpointer data)
{
	return !rec.available && rec.available_calls > 0;
}

static gboolean send_done(gpointer data)
{
	return rec.sends_done > 0;
}

static gboolean added_count(gpointer data)
{
	return rec.added->len >= GPOINTER_TO_UINT(data);
}

static gboolean status_count(gpointer data)
{
	return rec.statuses->len >= GPOINTER_TO_UINT(data);
}

static gboolean deleted_count(gpointer data)
{
	return fake.deleted->len >= GPOINTER_TO_UINT(data);
}

static void reset_fake(void)
{
	guint n;

	for (n = fake.services->len; n > 0; n--)
		fake_mmsd_remove_service(&fake, g_ptr_array_index(fake.services, n - 1));
	g_hash_table_remove_all(fake.stored);
	g_ptr_array_set_size(fake.deleted, 0);
	g_clear_pointer(&fake.send_error, g_free);
	if (fake.owned)
		fake_mmsd_release(&fake);
	fake_settle();
	rec_reset();
}

static void test_services(void)
{
	struct mmsd_client *client;
	gchar *first;
	gchar *second;

	reset_fake();
	fake_mmsd_own(&fake);
	first = g_strdup(fake_mmsd_add_service(&fake, "001010000000001", FALSE));

	client = mmsd_client_new(G_BUS_TYPE_SYSTEM, &callbacks, &rec);
	g_assert_cmpuint(mmsd_client_service_count(client), ==, 0);
	g_assert_null(mmsd_client_pick_service(client, NULL));

	g_assert_true(fake_wait(is_available, NULL));
	g_assert_cmpint(rec.available_calls, ==, 1);
	g_assert_cmpuint(mmsd_client_service_count(client), ==, 1);
	g_assert_cmpstr(mmsd_client_pick_service(client, NULL), ==, first);
	g_assert_cmpstr(mmsd_client_pick_service(client, "nomatch"), ==, first);

	/* a second SIM: picked by its identity */
	second = g_strdup(fake_mmsd_add_service(&fake, "001010000000002", TRUE));
	g_assert_true(fake_wait(added_count, GUINT_TO_POINTER(0)));
	fake_settle();
	g_assert_cmpuint(mmsd_client_service_count(client), ==, 2);
	g_assert_cmpstr(mmsd_client_pick_service(client, "001010000000002"), ==, second);
	g_assert_cmpstr(mmsd_client_pick_service(client, "001010000000001"), ==, first);
	/* still available, not announced twice */
	g_assert_cmpint(rec.available_calls, ==, 1);

	fake_mmsd_remove_service(&fake, first);
	fake_settle();
	g_assert_cmpuint(mmsd_client_service_count(client), ==, 1);
	g_assert_true(rec.available);

	fake_mmsd_remove_service(&fake, second);
	g_assert_true(fake_wait(is_unavailable, NULL));
	g_assert_cmpint(rec.available_calls, ==, 2);
	g_assert_cmpuint(mmsd_client_service_count(client), ==, 0);

	mmsd_client_free(client);
	g_free(first);
	g_free(second);
}

static void test_name_comes_and_goes(void)
{
	struct mmsd_client *client;

	reset_fake();
	fake_mmsd_add_service(&fake, "001010000000001", FALSE);

	/* mmsd not running yet */
	client = mmsd_client_new(G_BUS_TYPE_SYSTEM, &callbacks, &rec);
	fake_settle();
	g_assert_cmpint(rec.available_calls, ==, 0);

	fake_mmsd_own(&fake);
	g_assert_true(fake_wait(is_available, NULL));

	/* mmsd stops: everything it offered is gone */
	fake_mmsd_release(&fake);
	g_assert_true(fake_wait(is_unavailable, NULL));
	g_assert_cmpuint(mmsd_client_service_count(client), ==, 0);

	/* and comes back */
	fake_mmsd_own(&fake);
	g_assert_true(fake_wait(is_available, NULL));
	g_assert_cmpint(rec.available_calls, ==, 3);

	mmsd_client_free(client);
}

static GPtrArray* make_parts(void)
{
	GPtrArray *parts = g_ptr_array_new_with_free_func((GDestroyNotify) mms_send_part_free);
	struct mms_send_part *p;

	p = g_new0(struct mms_send_part, 1);
	p->content_id = g_strdup("<text_0.txt>");
	p->content_type = g_strdup("text/plain");
	p->file = g_build_filename(tmp, "text_0.txt", NULL);
	g_assert_true(g_file_set_contents(p->file, "hello", 5, NULL));
	g_ptr_array_add(parts, p);

	p = g_new0(struct mms_send_part, 1);
	p->content_id = g_strdup("<part1.jpg>");
	p->content_type = g_strdup("image/jpeg");
	p->file = g_build_filename(tmp, "part1.jpg", NULL);
	g_assert_true(g_file_set_contents(p->file, "\xff\xd8JPEG", 6, NULL));
	g_ptr_array_add(parts, p);

	return parts;
}

static void remove_parts(GPtrArray *parts)
{
	guint n;

	for (n = 0; n < parts->len; n++)
		g_unlink(((struct mms_send_part *) g_ptr_array_index(parts, n))->file);
	g_ptr_array_free(parts, TRUE);
}

static void test_send(void)
{
	struct mmsd_client *client;
	GPtrArray *recipients = g_ptr_array_new();
	GPtrArray *parts = make_parts();
	const char *service;
	GVariantIter *to_iter, *att_iter;
	const char *smil, *s, *id, *ct, *file;
	gchar *remote;

	reset_fake();
	fake_mmsd_own(&fake);
	service = fake_mmsd_add_service(&fake, "001010000000001", FALSE);

	client = mmsd_client_new(G_BUS_TYPE_SYSTEM, &callbacks, &rec);
	g_assert_true(fake_wait(is_available, NULL));

	g_ptr_array_add(recipients, "+31612345678");
	g_ptr_array_add(recipients, "0612345679");

	mmsd_client_send(client, service, recipients, "<smil/>", parts, on_sent, &rec);
	g_assert_cmpint(rec.sends_done, ==, 0);
	g_assert_true(fake_wait(send_done, NULL));
	g_assert_no_error(rec.send_error);
	g_assert_cmpstr(rec.sent_path, ==, fake.last_message);
	g_assert_true(g_str_has_prefix(rec.sent_path, service));

	/* exactly what mmsd's send_message_get_args reads */
	g_assert_cmpuint(fake.sends, ==, 1);
	g_assert_cmpstr(g_variant_get_type_string(fake.last_send), ==, "(assa(sss))");
	g_variant_get(fake.last_send, "(as&sa(sss))", &to_iter, &smil, &att_iter);
	g_assert_true(g_variant_iter_next(to_iter, "&s", &s));
	g_assert_cmpstr(s, ==, "+31612345678");
	g_assert_true(g_variant_iter_next(to_iter, "&s", &s));
	g_assert_cmpstr(s, ==, "0612345679");
	g_assert_false(g_variant_iter_next(to_iter, "&s", &s));
	g_assert_cmpstr(smil, ==, "<smil/>");
	g_assert_true(g_variant_iter_next(att_iter, "(&s&s&s)", &id, &ct, &file));
	g_assert_cmpstr(id, ==, "<text_0.txt>");
	g_assert_cmpstr(ct, ==, "text/plain");
	g_assert_true(g_variant_iter_next(att_iter, "(&s&s&s)", &id, &ct, &file));
	g_assert_cmpstr(id, ==, "<part1.jpg>");
	g_assert_cmpstr(ct, ==, "image/jpeg");
	g_assert_false(g_variant_iter_next(att_iter, "(&s&s&s)", &id, &ct, &file));
	g_variant_iter_free(to_iter);
	g_variant_iter_free(att_iter);
	g_assert_cmpmem(g_bytes_get_data(g_ptr_array_index(fake.last_send_data, 1), NULL), 6, "\xff\xd8JPEG", 6);

	/* the draft mmsd announces for it reaches us too */
	g_assert_true(fake_wait(added_count, GUINT_TO_POINTER(1)));

	/* mmsd refuses */
	rec.sends_done = 0;
	fake.send_error = g_strdup("org.ofono.mms.Error.PermanentInvalidAddress");
	mmsd_client_send(client, service, recipients, "", parts, on_sent, &rec);
	g_assert_true(fake_wait(send_done, NULL));
	g_assert_null(rec.sent_path);
	g_assert_nonnull(rec.send_error);
	remote = g_dbus_error_get_remote_error(rec.send_error);
	g_assert_cmpstr(remote, ==, "org.ofono.mms.Error.PermanentInvalidAddress");
	g_free(remote);

	/* a service that is not there: answered later, never inline */
	rec.sends_done = 0;
	mmsd_client_send(client, "/org/ofono/mms/none", recipients, "", parts, on_sent, &rec);
	g_assert_cmpint(rec.sends_done, ==, 0);
	g_assert_true(fake_wait(send_done, NULL));
	g_assert_error(rec.send_error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN);

	/* a send still under way when the client goes: answered as cancelled */
	g_clear_pointer(&fake.send_error, g_free);
	rec.sends_done = 0;
	mmsd_client_send(client, service, recipients, "", parts, on_sent, &rec);
	mmsd_client_free(client);
	g_assert_true(fake_wait(send_done, NULL));
	g_assert_error(rec.send_error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
	fake_settle();
	g_assert_cmpint(rec.sends_done, ==, 1);

	g_ptr_array_free(recipients, TRUE);
	remove_parts(parts);
}

static void test_signals(void)
{
	struct mmsd_client *client;
	GDBusConnection *other;
	const char *service;
	gchar *expect;

	reset_fake();
	fake_mmsd_own(&fake);
	service = fake_mmsd_add_service(&fake, "001010000000001", FALSE);

	/* already stored when the client starts: listed once the service is there */
	fake_mmsd_store_message(&fake, service, "/org/ofono/mms/001010000000001/old1",
	                        g_variant_new_parsed("{'Status': <'received'>}"));

	client = mmsd_client_new(G_BUS_TYPE_SYSTEM, &callbacks, &rec);
	g_assert_true(fake_wait(added_count, GUINT_TO_POINTER(1)));
	expect = g_strdup_printf("%s %s received", service, "/org/ofono/mms/001010000000001/old1");
	g_assert_cmpstr(g_ptr_array_index(rec.added, 0), ==, expect);
	g_free(expect);

	fake_mmsd_emit_message_added(&fake, service, "/org/ofono/mms/001010000000001/new1",
	                             g_variant_new_parsed("{'Status': <'received'>, 'Sender': <'+3161'>}"));
	g_assert_true(fake_wait(added_count, GUINT_TO_POINTER(2)));

	fake_mmsd_emit_status(&fake, NULL, "/org/ofono/mms/001010000000001/new1", "read");
	g_assert_true(fake_wait(status_count, GUINT_TO_POINTER(1)));
	g_assert_cmpstr(g_ptr_array_index(rec.statuses, 0), ==, "/org/ofono/mms/001010000000001/new1 read");

	/* somebody else on the bus claims a message was sent: not mmsd, not taken */
	other = g_dbus_connection_new_for_address_sync(g_getenv("DBUS_SYSTEM_BUS_ADDRESS"),
	                                               G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
	                                               G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
	                                               NULL, NULL, NULL);
	g_assert_nonnull(other);
	fake_mmsd_emit_status(&fake, other, "/org/ofono/mms/001010000000001/new1", "sent");
	g_dbus_connection_emit_signal(other, NULL, service, "org.ofono.mms.Service", "MessageAdded",
	                              g_variant_new_parsed("(%o, {'Status': <'received'>})",
	                                                   "/org/ofono/mms/001010000000001/spoof"), NULL);
	g_dbus_connection_flush_sync(other, NULL, NULL);
	fake_settle();
	g_assert_cmpuint(rec.statuses->len, ==, 1);
	g_assert_cmpuint(rec.added->len, ==, 2);
	g_dbus_connection_close_sync(other, NULL, NULL);
	g_object_unref(other);

	/* MessageAdded on a path that is no service of ours */
	g_dbus_connection_emit_signal(fake.conn, NULL, "/org/ofono/mms/unknown", "org.ofono.mms.Service",
	                              "MessageAdded", g_variant_new_parsed("(%o, {'Status': <'received'>})",
	                                                                   "/org/ofono/mms/unknown/m"), NULL);
	fake_settle();
	g_assert_cmpuint(rec.added->len, ==, 2);

	/* Delete */
	mmsd_client_delete_message(client, "/org/ofono/mms/001010000000001/new1");
	mmsd_client_delete_message(client, "not a path");
	g_assert_true(fake_wait(deleted_count, GUINT_TO_POINTER(1)));
	g_assert_cmpstr(g_ptr_array_index(fake.deleted, 0), ==, "/org/ofono/mms/001010000000001/new1");

	mmsd_client_free(client);
	fake_settle();
	g_assert_cmpuint(fake.deleted->len, ==, 1);
}

int main(int argc, char **argv)
{
	int ret;

	g_test_init(&argc, &argv, NULL);
	/* the daemon warns about every message it refuses; that is expected
	 * here, only criticals are bugs */
	g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);

	tmp = g_dir_make_tmp("test_mmsdclient-XXXXXX", NULL);
	g_assert_nonnull(tmp);

	fake_mmsd_start(&fake);
	rec_reset();

	g_test_add_func("/mmsdclient/services", test_services);
	g_test_add_func("/mmsdclient/name-comes-and-goes", test_name_comes_and_goes);
	g_test_add_func("/mmsdclient/send", test_send);
	g_test_add_func("/mmsdclient/signals", test_signals);

	ret = g_test_run();

	reset_fake();
	fake_mmsd_stop(&fake);
	if (rec.added)
		g_ptr_array_free(rec.added, TRUE);
	if (rec.statuses)
		g_ptr_array_free(rec.statuses, TRUE);
	g_free(rec.sent_path);
	g_clear_error(&rec.send_error);
	g_rmdir(tmp);
	g_free(tmp);

	return ret;
}

// vim:ts=4:sw=4:noexpandtab
