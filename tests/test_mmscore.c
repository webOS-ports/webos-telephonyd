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
 * The MMS helpers that need neither D-Bus nor luna: numbers as mmsd takes
 * them, content types, which files may be sent, the spool copies, SMIL, the
 * status and error mapping, and reading parts of a received message.
 */

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "mmscore.h"

static gchar *tmp;

static gchar* path_in_tmp(const char *name)
{
	return g_build_filename(tmp, name, NULL);
}

static void write_file(const char *name, const char *data, gssize len)
{
	gchar *path = path_in_tmp(name);

	g_assert_true(g_file_set_contents(path, data, len, NULL));
	g_free(path);
}

static void test_numbers(void)
{
	gchar *n;

	n = mms_normalize_number("+31 6 1234-5678");
	g_assert_cmpstr(n, ==, "+31612345678");
	g_free(n);

	n = mms_normalize_number("(030) 123.45.67");
	g_assert_cmpstr(n, ==, "0301234567");
	g_free(n);

	n = mms_normalize_number("12345678901234567890");
	g_assert_cmpstr(n, ==, "12345678901234567890");
	g_free(n);

	/* what mmsd would refuse */
	g_assert_null(mms_normalize_number("123456789012345678901"));
	g_assert_null(mms_normalize_number("someone@example.org"));
	g_assert_null(mms_normalize_number("+"));
	g_assert_null(mms_normalize_number(""));
	g_assert_null(mms_normalize_number("12+34"));
	g_assert_null(mms_normalize_number("++123"));
	g_assert_null(mms_normalize_number("123/TYPE=PLMN"));
	g_assert_null(mms_normalize_number(NULL));
}

static void test_content_types(void)
{
	gchar *bare;

	g_assert_cmpstr(mms_content_type_for_name("/a/b/photo.JPG"), ==, "image/jpeg");
	g_assert_cmpstr(mms_content_type_for_name("note.m4a"), ==, "audio/mp4");
	g_assert_cmpstr(mms_content_type_for_name("card.vcf"), ==, "text/x-vcard");
	g_assert_cmpstr(mms_content_type_for_name("archive.tar.gz"), ==, "application/octet-stream");
	g_assert_cmpstr(mms_content_type_for_name("/dir.d/noext"), ==, "application/octet-stream");
	g_assert_cmpstr(mms_content_type_for_name(NULL), ==, "application/octet-stream");

	g_assert_cmpstr(mms_extension_for_content_type("image/jpeg"), ==, "jpg");
	g_assert_cmpstr(mms_extension_for_content_type("Image/JPEG; name=x.jpg"), ==, "jpg");
	g_assert_cmpstr(mms_extension_for_content_type("image/jpg"), ==, "jpg");
	g_assert_cmpstr(mms_extension_for_content_type("text/plain;charset=utf-8"), ==, "txt");
	g_assert_cmpstr(mms_extension_for_content_type("application/x-whatever"), ==, "bin");
	g_assert_cmpstr(mms_extension_for_content_type(NULL), ==, "bin");

	bare = mms_content_type_bare("  Text/Plain ; charset=\"UTF-8\"");
	g_assert_cmpstr(bare, ==, "text/plain");
	g_free(bare);
	g_assert_null(mms_content_type_bare(NULL));

	g_assert_true(mms_content_type_valid("image/jpeg"));
	g_assert_true(mms_content_type_valid("application/vnd.wap.mms-message"));
	g_assert_false(mms_content_type_valid("image"));
	g_assert_false(mms_content_type_valid("image/"));
	g_assert_false(mms_content_type_valid("/jpeg"));
	g_assert_false(mms_content_type_valid("image/jpeg; x=1"));
	g_assert_false(mms_content_type_valid("image/jp eg"));
	g_assert_false(mms_content_type_valid("a/b/c"));
	g_assert_false(mms_content_type_valid(""));
	g_assert_false(mms_content_type_valid(NULL));
}

static void test_content_type_param(void)
{
	gchar *v;

	v = mms_content_type_param("text/plain; charset=\"ISO-8859-1\"; name=a.txt", "CHARSET");
	g_assert_cmpstr(v, ==, "ISO-8859-1");
	g_free(v);

	v = mms_content_type_param("text/plain;name=a.txt", "name");
	g_assert_cmpstr(v, ==, "a.txt");
	g_free(v);

	g_assert_null(mms_content_type_param("text/plain", "charset"));
	g_assert_null(mms_content_type_param("text/plain; charset", "charset"));
	g_assert_null(mms_content_type_param(NULL, "charset"));
}

static void test_path_allowed(void)
{
	const char * const roots[] = { "/media/internal/", NULL };

	g_assert_true(mms_path_allowed("/media/internal/DCIM/a.jpg", roots));
	g_assert_true(mms_path_allowed("/media/internal/.im-attachments/v.m4a", roots));

	g_assert_false(mms_path_allowed("/media/internal/", roots));
	g_assert_false(mms_path_allowed("/media/internal", roots));
	g_assert_false(mms_path_allowed("/media/internalx/a.jpg", roots));
	g_assert_false(mms_path_allowed("/media/internal/../../etc/shadow", roots));
	g_assert_false(mms_path_allowed("/media/internal/./a.jpg", roots));
	g_assert_false(mms_path_allowed("/media/internal//a.jpg", roots));
	g_assert_false(mms_path_allowed("/media/internal/dir/", roots));
	g_assert_false(mms_path_allowed("media/internal/a.jpg", roots));
	g_assert_false(mms_path_allowed("/etc/passwd", roots));
	g_assert_false(mms_path_allowed(NULL, roots));
	g_assert_false(mms_path_allowed("/media/internal/a.jpg", NULL));
}

static void test_open_file(void)
{
	gchar *root = g_strdup_printf("%s/root/", tmp);
	gchar *outside = path_in_tmp("outside.jpg");
	gchar *path;
	const char * const roots[] = { root, NULL };
	const char *error_code = NULL;
	gsize size = 0;
	int fd;

	g_assert_cmpint(g_mkdir(root, 0700), ==, 0);
	write_file("root/a.jpg", "JPEGDATA", 8);
	write_file("root/empty.jpg", "", 0);
	write_file("outside.jpg", "SECRET", 6);

	/* the good case */
	path = g_strdup_printf("%sa.jpg", root);
	fd = mms_open_file(path, roots, 100, &size, &error_code);
	g_assert_cmpint(fd, >=, 0);
	g_assert_null(error_code);
	g_assert_cmpuint(size, ==, 8);
	close(fd);

	/* larger than what is left */
	fd = mms_open_file(path, roots, 7, &size, &error_code);
	g_assert_cmpint(fd, ==, -1);
	g_assert_cmpstr(error_code, ==, MMS_ERR_TOO_LARGE);
	g_free(path);

	/* an empty file */
	path = g_strdup_printf("%sempty.jpg", root);
	fd = mms_open_file(path, roots, 100, &size, &error_code);
	g_assert_cmpint(fd, ==, -1);
	g_assert_cmpstr(error_code, ==, MMS_ERR_CORRUPT);
	g_free(path);

	/* missing */
	path = g_strdup_printf("%snone.jpg", root);
	fd = mms_open_file(path, roots, 100, &size, &error_code);
	g_assert_cmpint(fd, ==, -1);
	g_assert_cmpstr(error_code, ==, MMS_ERR_UNSPECIFIED);
	g_free(path);

	/* outside the roots */
	fd = mms_open_file(outside, roots, 100, &size, &error_code);
	g_assert_cmpint(fd, ==, -1);
	g_assert_cmpstr(error_code, ==, MMS_ERR_UNSUPPORTED);

	/* a symlink below the root that leads out of it */
	path = g_strdup_printf("%slink.jpg", root);
	g_assert_cmpint(symlink(outside, path), ==, 0);
	fd = mms_open_file(path, roots, 100, &size, &error_code);
	g_assert_cmpint(fd, ==, -1);
	g_assert_cmpstr(error_code, ==, MMS_ERR_UNSUPPORTED);
	g_unlink(path);
	g_free(path);

	/* a directory symlink below the root that leads out of it */
	path = g_strdup_printf("%sdirlink", root);
	g_assert_cmpint(symlink(tmp, path), ==, 0);
	g_free(path);
	path = g_strdup_printf("%sdirlink/outside.jpg", root);
	fd = mms_open_file(path, roots, 100, &size, &error_code);
	g_assert_cmpint(fd, ==, -1);
	g_assert_cmpstr(error_code, ==, MMS_ERR_UNSUPPORTED);
	g_free(path);
	path = g_strdup_printf("%sdirlink", root);
	g_unlink(path);
	g_free(path);

	/* not a regular file */
	path = g_strdup_printf("%sfifo.jpg", root);
	g_assert_cmpint(mkfifo(path, 0600), ==, 0);
	fd = mms_open_file(path, roots, 100, &size, &error_code);
	g_assert_cmpint(fd, ==, -1);
	g_assert_cmpstr(error_code, ==, MMS_ERR_UNSUPPORTED);
	g_unlink(path);
	g_free(path);

	path = g_strdup_printf("%sa.jpg", root);
	g_unlink(path);
	g_free(path);
	path = g_strdup_printf("%sempty.jpg", root);
	g_unlink(path);
	g_free(path);
	g_rmdir(root);
	g_unlink(outside);
	g_free(outside);
	g_free(root);
}

static void test_copy(void)
{
	gchar *src = path_in_tmp("src.bin");
	gchar *dest = path_in_tmp("dest.bin");
	gchar *contents = NULL;
	gsize len = 0;
	GError *error = NULL;
	struct stat st;
	int fd;

	write_file("src.bin", "0123456789", 10);

	fd = open(src, O_RDONLY);
	g_assert_true(mms_copy_fd_to_file(fd, dest, 10, 0640, getgid(), &error));
	g_assert_no_error(error);
	close(fd);

	g_assert_true(g_file_get_contents(dest, &contents, &len, NULL));
	g_assert_cmpuint(len, ==, 10);
	g_assert_cmpmem(contents, len, "0123456789", 10);
	g_free(contents);
	g_assert_cmpint(stat(dest, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0640);
	g_assert_cmpint(st.st_gid, ==, getgid());

	/* never over an existing file */
	fd = open(src, O_RDONLY);
	g_assert_false(mms_copy_fd_to_file(fd, dest, 10, 0640, (gid_t) -1, &error));
	g_assert_error(error, G_FILE_ERROR, G_FILE_ERROR_EXIST);
	g_clear_error(&error);
	close(fd);
	/* ... and the existing one is left alone */
	g_assert_true(g_file_test(dest, G_FILE_TEST_EXISTS));
	g_unlink(dest);

	/* a file that grew after it was checked: no partial copy is left */
	fd = open(src, O_RDONLY);
	g_assert_false(mms_copy_fd_to_file(fd, dest, 9, 0640, (gid_t) -1, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	close(fd);
	g_assert_false(g_file_test(dest, G_FILE_TEST_EXISTS));

	g_assert_true(mms_write_new_file(dest, "abc", 3, 0644, (gid_t) -1, &error));
	g_assert_true(g_file_get_contents(dest, &contents, &len, NULL));
	g_assert_cmpmem(contents, len, "abc", 3);
	g_free(contents);
	g_assert_cmpint(stat(dest, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0644);

	g_unlink(dest);
	g_unlink(src);
	g_free(dest);
	g_free(src);
}

static struct mms_send_part* part(const char *name, const char *type)
{
	struct mms_send_part *p = g_new0(struct mms_send_part, 1);

	p->content_id = g_strdup_printf("<%s>", name);
	p->smil_src = g_strdup_printf("cid:%s", name);
	p->content_type = g_strdup(type);
	p->file = g_strdup(name);

	return p;
}

static void test_smil(void)
{
	GPtrArray *parts = g_ptr_array_new_with_free_func((GDestroyNotify) mms_send_part_free);
	gchar *smil;

	/* text only */
	g_ptr_array_add(parts, part("text_0.txt", "text/plain"));
	smil = mms_build_smil(parts);
	g_assert_nonnull(strstr(smil, "<body><par dur=\"5000ms\"><text src=\"cid:text_0.txt\" region=\"Text\"/></par></body>"));
	g_assert_true(g_str_has_prefix(smil, "<smil><head><layout>"));
	g_assert_true(g_str_has_suffix(smil, "</smil>"));
	g_free(smil);

	/* text and two pictures: the text goes with the first */
	g_ptr_array_add(parts, part("part1.jpg", "image/jpeg"));
	g_ptr_array_add(parts, part("part2.mp4", "video/mp4"));
	g_ptr_array_add(parts, part("part3.vcf", "text/x-vcard"));
	g_ptr_array_add(parts, part("part4.m4a", "audio/mp4"));
	smil = mms_build_smil(parts);
	g_assert_nonnull(strstr(smil,
		"<body>"
		"<par dur=\"5000ms\"><img src=\"cid:part1.jpg\" region=\"Image\"/>"
		"<text src=\"cid:text_0.txt\" region=\"Text\"/></par>"
		"<par dur=\"5000ms\"><video src=\"cid:part2.mp4\" region=\"Image\"/></par>"
		"<par dur=\"5000ms\"><ref src=\"cid:part3.vcf\" region=\"Image\"/></par>"
		"<par dur=\"5000ms\"><audio src=\"cid:part4.m4a\" region=\"Image\"/></par>"
		"</body>"));
	g_free(smil);
	g_ptr_array_set_size(parts, 0);

	/* a picture without text, and a name that needs escaping */
	g_ptr_array_add(parts, part("a\"&<b>.png", "image/png"));
	smil = mms_build_smil(parts);
	g_assert_nonnull(strstr(smil, "<img src=\"cid:a&quot;&amp;&lt;b&gt;.png\" region=\"Image\"/>"));
	g_assert_null(strstr(smil, "<text"));
	g_free(smil);

	g_ptr_array_free(parts, TRUE);
}

static void test_status_and_errors(void)
{
	g_assert_cmpstr(mms_db_status_for("sent"), ==, "successful");
	g_assert_cmpstr(mms_db_status_for("received"), ==, "successful");
	g_assert_cmpstr(mms_db_status_for("read"), ==, "successful");
	g_assert_null(mms_db_status_for("draft"));
	g_assert_null(mms_db_status_for(NULL));

	g_assert_cmpstr(mms_error_code_for_dbus_error("org.ofono.mms.Error.PermanentInvalidAddress"), ==,
	                MMS_ERR_BAD_ADDRESS);
	g_assert_cmpstr(mms_error_code_for_dbus_error("org.ofono.mms.Error.PermanentContentNotAccepted"), ==,
	                MMS_ERR_REJECTED);
	g_assert_cmpstr(mms_error_code_for_dbus_error("org.ofono.mms.Error.PermanentLackOfPrepaid"), ==,
	                MMS_ERR_DENIED);
	g_assert_cmpstr(mms_error_code_for_dbus_error("org.ofono.mms.Error.TransientNetworkProblem"), ==,
	                MMS_ERR_NETWORK);
	g_assert_cmpstr(mms_error_code_for_dbus_error("org.ofono.mms.Error.InvalidArguments"), ==,
	                MMS_ERR_UNSUPPORTED);
	g_assert_cmpstr(mms_error_code_for_dbus_error("org.freedesktop.DBus.Error.ServiceUnknown"), ==,
	                MMS_ERR_NOT_READY);
	g_assert_cmpstr(mms_error_code_for_dbus_error("org.example.Other"), ==, MMS_ERR_UNSPECIFIED);
	g_assert_cmpstr(mms_error_code_for_dbus_error(NULL), ==, MMS_ERR_UNSPECIFIED);
}

static void test_basename(void)
{
	gchar *b = mms_path_basename("/org/ofono/mms/001010123456789/0123abcd");

	g_assert_cmpstr(b, ==, "0123abcd");
	g_free(b);
	g_assert_null(mms_path_basename("/org/ofono/mms/"));
	g_assert_null(mms_path_basename("relative"));
	g_assert_null(mms_path_basename(NULL));
}

static void test_received_names(void)
{
	gchar *n;

	n = mms_received_file_name("<IMG_0001.JPG>", 2, "image/jpeg");
	g_assert_cmpstr(n, ==, "2-IMG_0001.jpg");
	g_free(n);

	/* a name that does not fit the type keeps its stem and gets the type's extension */
	n = mms_received_file_name("photo.gif", 3, "image/png");
	g_assert_cmpstr(n, ==, "3-photo.gif.png");
	g_free(n);

	/* no way out of the directory */
	n = mms_received_file_name("../../etc/passwd", 4, "text/x-vcard");
	g_assert_cmpstr(n, ==, "4-etcpasswd.vcf");
	g_free(n);

	n = mms_received_file_name("...", 5, "audio/amr");
	g_assert_cmpstr(n, ==, "part5.amr");
	g_free(n);

	n = mms_received_file_name(NULL, 6, "application/x-unknown");
	g_assert_cmpstr(n, ==, "part6.bin");
	g_free(n);

	n = mms_received_file_name("<\xc3\xa9t\xc3\xa9.jpg>", 7, "image/jpeg");
	g_assert_cmpstr(n, ==, "7-t.jpg");
	g_free(n);
}

static void test_read_range(void)
{
	gchar *store = path_in_tmp("store.mms");
	GError *error = NULL;
	GBytes *b;

	write_file("store.mms", "HEADERhelloTAIL", 15);

	b = mms_read_range(store, 6, 5, &error);
	g_assert_no_error(error);
	g_assert_cmpmem(g_bytes_get_data(b, NULL), g_bytes_get_size(b), "hello", 5);
	g_bytes_unref(b);

	b = mms_read_range(store, 15, 0, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(g_bytes_get_size(b), ==, 0);
	g_bytes_unref(b);

	/* past the end, and an offset+length that wraps */
	g_assert_null(mms_read_range(store, 11, 5, &error));
	g_assert_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL);
	g_clear_error(&error);
	g_assert_null(mms_read_range(store, 16, 0, &error));
	g_clear_error(&error);
	g_assert_null(mms_read_range(store, 6, G_MAXUINT64 - 2, &error));
	g_clear_error(&error);

	g_unlink(store);
	g_assert_null(mms_read_range(store, 0, 1, &error));
	g_assert_error(error, G_FILE_ERROR, G_FILE_ERROR_NOENT);
	g_clear_error(&error);
	g_free(store);
}

static void test_part_text(void)
{
	gchar *t;

	t = mms_part_text("hello\0", 6, "text/plain;charset=utf-8");
	g_assert_cmpstr(t, ==, "hello");
	g_free(t);

	t = mms_part_text("caf\xe9", 4, "text/plain; charset=\"ISO-8859-1\"");
	g_assert_cmpstr(t, ==, "caf\xc3\xa9");
	g_free(t);

	t = mms_part_text("\xa3" "5 \xe0 la", 7, "text/plain; charset=latin1");
	g_assert_cmpstr(t, ==, "\xc2\xa3" "5 \xc3\xa0 la");
	g_free(t);

	/* broken UTF-8 does not get into db8 */
	t = mms_part_text("a\xff" "b", 3, "text/plain");
	g_assert_true(g_utf8_validate(t, -1, NULL));
	g_assert_true(g_str_has_prefix(t, "a"));
	g_assert_true(g_str_has_suffix(t, "b"));
	g_free(t);

	/* an unknown charset falls back to UTF-8 */
	t = mms_part_text("ok", 2, "text/plain; charset=x-no-such-charset");
	g_assert_cmpstr(t, ==, "ok");
	g_free(t);

	t = mms_part_text("", 0, NULL);
	g_assert_cmpstr(t, ==, "");
	g_free(t);
}

static void test_outgoing(void)
{
	struct mms_outgoing *msg = mms_outgoing_new("id1");

	g_assert_cmpint(msg->sim_id, ==, -1);
	mms_outgoing_add_file(msg, "/media/internal/a.jpg", NULL);
	mms_outgoing_add_file(msg, "/media/internal/b.png", "image/png");
	g_assert_cmpuint(msg->files->len, ==, 2);
	g_assert_null(((struct mms_file *) g_ptr_array_index(msg->files, 0))->content_type);
	g_assert_cmpstr(((struct mms_file *) g_ptr_array_index(msg->files, 1))->content_type, ==, "image/png");
	mms_outgoing_free(msg);
	mms_outgoing_free(NULL);
}

int main(int argc, char **argv)
{
	int ret;

	g_test_init(&argc, &argv, NULL);

	tmp = g_dir_make_tmp("test_mmscore-XXXXXX", NULL);
	g_assert_nonnull(tmp);

	g_test_add_func("/mmscore/numbers", test_numbers);
	g_test_add_func("/mmscore/content-types", test_content_types);
	g_test_add_func("/mmscore/content-type-param", test_content_type_param);
	g_test_add_func("/mmscore/path-allowed", test_path_allowed);
	g_test_add_func("/mmscore/open-file", test_open_file);
	g_test_add_func("/mmscore/copy", test_copy);
	g_test_add_func("/mmscore/smil", test_smil);
	g_test_add_func("/mmscore/status-and-errors", test_status_and_errors);
	g_test_add_func("/mmscore/basename", test_basename);
	g_test_add_func("/mmscore/received-names", test_received_names);
	g_test_add_func("/mmscore/read-range", test_read_range);
	g_test_add_func("/mmscore/part-text", test_part_text);
	g_test_add_func("/mmscore/outgoing", test_outgoing);

	ret = g_test_run();

	g_rmdir(tmp);
	g_free(tmp);

	return ret;
}

// vim:ts=4:sw=4:noexpandtab
