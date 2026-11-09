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
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mmscore.h"

/* Reading a received part: more than this is not an MMS. */
#define MMS_MAX_PART_READ ((guint64) 64 * 1024 * 1024)

const char * const mms_default_roots[] = { "/media/internal/", NULL };

struct mms_outgoing* mms_outgoing_new(const char *id)
{
	struct mms_outgoing *msg = g_new0(struct mms_outgoing, 1);

	msg->id = g_strdup(id);
	msg->recipients = g_ptr_array_new_with_free_func(g_free);
	msg->files = g_ptr_array_new();
	msg->sim_id = -1;

	return msg;
}

static void mms_file_free(struct mms_file *file)
{
	if (!file)
		return;

	g_free(file->path);
	g_free(file->content_type);
	g_free(file);
}

void mms_outgoing_free(struct mms_outgoing *msg)
{
	guint n;

	if (!msg)
		return;

	for (n = 0; n < msg->files->len; n++)
		mms_file_free(g_ptr_array_index(msg->files, n));

	g_ptr_array_free(msg->files, TRUE);
	g_ptr_array_free(msg->recipients, TRUE);
	g_free(msg->id);
	g_free(msg->text);
	g_free(msg->subject);
	g_free(msg);
}

void mms_outgoing_add_file(struct mms_outgoing *msg, const char *path, const char *content_type)
{
	struct mms_file *file = g_new0(struct mms_file, 1);

	file->path = g_strdup(path);
	file->content_type = g_strdup(content_type);
	g_ptr_array_add(msg->files, file);
}

gchar* mms_normalize_number(const char *addr)
{
	GString *out;
	const char *p;
	unsigned int digits = 0;

	if (!addr)
		return NULL;

	out = g_string_new(NULL);

	for (p = addr; *p; p++) {
		if (*p == ' ' || *p == '\t' || *p == '-' || *p == '.' || *p == '(' || *p == ')')
			continue;

		if (*p == '+' && out->len == 0) {
			g_string_append_c(out, '+');
			continue;
		}

		if (*p >= '0' && *p <= '9') {
			if (++digits > MMS_MAX_NUMBER_DIGITS)
				goto bad;
			g_string_append_c(out, *p);
			continue;
		}

		goto bad;
	}

	if (digits == 0)
		goto bad;

	return g_string_free(out, FALSE);

bad:
	g_string_free(out, TRUE);
	return NULL;
}

struct type_map {
	const char *ext;
	const char *type;
};

/* The first entry for a type is its extension for received parts. */
static const struct type_map type_map[] = {
	{ "jpg", "image/jpeg" },
	{ "jpeg", "image/jpeg" },
	{ "png", "image/png" },
	{ "gif", "image/gif" },
	{ "webp", "image/webp" },
	{ "bmp", "image/bmp" },
	{ "wbmp", "image/vnd.wap.wbmp" },
	{ "heic", "image/heic" },
	{ "3gp", "video/3gpp" },
	{ "3g2", "video/3gpp2" },
	{ "mp4", "video/mp4" },
	{ "m4v", "video/mp4" },
	{ "webm", "video/webm" },
	{ "amr", "audio/amr" },
	{ "m4a", "audio/mp4" },
	{ "aac", "audio/aac" },
	{ "mp3", "audio/mpeg" },
	{ "ogg", "audio/ogg" },
	{ "oga", "audio/ogg" },
	{ "opus", "audio/ogg" },
	{ "wav", "audio/wav" },
	{ "txt", "text/plain" },
	{ "vcf", "text/x-vcard" },
	{ "vcs", "text/x-vcalendar" },
	{ "ics", "text/calendar" },
	{ "pdf", "application/pdf" },
	{ "smil", "application/smil" },
	{ NULL, NULL }
};

const char* mms_content_type_for_name(const char *name)
{
	const char *dot;
	const struct type_map *m;

	if (!name)
		return "application/octet-stream";

	dot = strrchr(name, '.');
	if (!dot || strchr(dot, '/'))
		return "application/octet-stream";

	for (m = type_map; m->ext; m++) {
		if (g_ascii_strcasecmp(dot + 1, m->ext) == 0)
			return m->type;
	}

	return "application/octet-stream";
}

gchar* mms_content_type_bare(const char *content_type)
{
	const char *end;
	gchar *bare, *stripped;

	if (!content_type)
		return NULL;

	end = strchr(content_type, ';');
	bare = end ? g_strndup(content_type, (gsize) (end - content_type)) : g_strdup(content_type);
	stripped = g_ascii_strdown(g_strstrip(bare), -1);
	g_free(bare);

	return stripped;
}

const char* mms_extension_for_content_type(const char *content_type)
{
	gchar *bare = mms_content_type_bare(content_type);

	if (bare) {
		const struct type_map *m;

		/* Some senders still label JPEGs with the old non-standard name */
		if (strcmp(bare, "image/jpg") == 0 || strcmp(bare, "image/pjpeg") == 0) {
			g_free(bare);
			return "jpg";
		}

		for (m = type_map; m->ext; m++) {
			if (strcmp(bare, m->type) == 0) {
				g_free(bare);
				return m->ext;
			}
		}
	}

	g_free(bare);
	return "bin";
}

bool mms_content_type_valid(const char *content_type)
{
	const char *p;
	int slashes = 0;

	if (!content_type || !*content_type || *content_type == '/')
		return false;

	for (p = content_type; *p; p++) {
		if (*p == '/') {
			slashes++;
			continue;
		}

		if ((unsigned char) *p <= ' ' || (unsigned char) *p >= 0x7f || strchr("()<>@,;:\\\"[]?={}", *p))
			return false;
	}

	return slashes == 1 && p[-1] != '/';
}

static bool has_bad_component(const char *path)
{
	gchar **parts = g_strsplit(path + 1, "/", -1);
	bool bad = false;
	int n;

	for (n = 0; parts[n]; n++) {
		/* an empty last component is a trailing slash: not a file either */
		if (parts[n][0] == '\0' || strcmp(parts[n], ".") == 0 || strcmp(parts[n], "..") == 0) {
			bad = true;
			break;
		}
	}

	g_strfreev(parts);
	return bad;
}

bool mms_path_allowed(const char *path, const char * const *roots)
{
	int n;

	if (!path || path[0] != '/' || !roots)
		return false;

	if (has_bad_component(path))
		return false;

	for (n = 0; roots[n]; n++) {
		size_t len = strlen(roots[n]);

		if (len > 1 && roots[n][len - 1] == '/' && strncmp(path, roots[n], len) == 0 && path[len] != '\0')
			return true;
	}

	return false;
}

/* The same check for a resolved path, against the resolved roots. */
static bool real_path_allowed(const char *real, const char * const *roots)
{
	int n;

	for (n = 0; roots[n]; n++) {
		char *root = realpath(roots[n], NULL);
		size_t len;
		bool ok;

		if (!root)
			continue;

		len = strlen(root);
		ok = strncmp(real, root, len) == 0 && real[len] == '/' && real[len + 1] != '\0';
		free(root);

		if (ok)
			return true;
	}

	return false;
}

int mms_open_file(const char *path, const char * const *roots, gsize max_size,
                  gsize *size, const char **error_code)
{
	char *real = NULL;
	char proc[64];
	char opened[PATH_MAX];
	ssize_t opened_len;
	struct stat st;
	int fd = -1;

	*size = 0;
	*error_code = MMS_ERR_UNSUPPORTED;

	if (!mms_path_allowed(path, roots))
		return -1;

	real = realpath(path, NULL);
	if (!real) {
		*error_code = MMS_ERR_UNSPECIFIED;
		return -1;
	}

	if (!real_path_allowed(real, roots))
		goto fail;

	fd = open(real, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
	if (fd < 0) {
		*error_code = MMS_ERR_UNSPECIFIED;
		goto fail;
	}

	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode))
		goto fail;

	/* Whatever was swapped in between realpath() and open(): the file we
	 * hold has to be the one below the root. */
	snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
	opened_len = readlink(proc, opened, sizeof(opened) - 1);
	if (opened_len <= 0 || (size_t) opened_len >= sizeof(opened) - 1)
		goto fail;
	opened[opened_len] = '\0';

	if (strcmp(opened, real) != 0 || !real_path_allowed(opened, roots))
		goto fail;

	if (st.st_size <= 0) {
		*error_code = MMS_ERR_CORRUPT;
		goto fail;
	}

	if ((guint64) st.st_size > max_size) {
		*error_code = MMS_ERR_TOO_LARGE;
		goto fail;
	}

	free(real);
	*size = (gsize) st.st_size;
	*error_code = NULL;
	return fd;

fail:
	if (fd >= 0)
		close(fd);
	free(real);
	return -1;
}

static int create_new_file(const char *dest, mode_t mode, gid_t gid, GError **error)
{
	int fd = open(dest, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, mode);

	if (fd < 0) {
		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "create %s: %s", dest,
		            g_strerror(errno));
		return -1;
	}

	if ((gid != (gid_t) -1 && fchown(fd, (uid_t) -1, gid) < 0) || fchmod(fd, mode) < 0) {
		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "set owner of %s: %s", dest,
		            g_strerror(errno));
		close(fd);
		unlink(dest);
		return -1;
	}

	return fd;
}

static gboolean write_all(int fd, const char *data, gsize len, const char *dest, GError **error)
{
	while (len > 0) {
		ssize_t w = write(fd, data, len);

		if (w < 0 && errno == EINTR)
			continue;

		if (w <= 0) {
			g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno ? errno : EIO),
			            "write %s: %s", dest, g_strerror(errno ? errno : EIO));
			return FALSE;
		}

		data += w;
		len -= (gsize) w;
	}

	return TRUE;
}

static gboolean finish_new_file(int fd, const char *dest, gboolean ok, GError **error)
{
	if (close(fd) < 0 && ok) {
		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "close %s: %s", dest,
		            g_strerror(errno));
		ok = FALSE;
	}

	if (!ok)
		unlink(dest);

	return ok;
}

gboolean mms_copy_fd_to_file(int fd, const char *dest, gsize max, mode_t mode, gid_t gid, GError **error)
{
	char buf[16384];
	gsize total = 0;
	gboolean ok = TRUE;
	int out = create_new_file(dest, mode, gid, error);

	if (out < 0)
		return FALSE;

	for (;;) {
		ssize_t r = read(fd, buf, sizeof(buf));

		if (r < 0 && errno == EINTR)
			continue;

		if (r < 0) {
			g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "read: %s",
			            g_strerror(errno));
			ok = FALSE;
			break;
		}

		if (r == 0)
			break;

		total += (gsize) r;
		if (total > max) {
			g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "%s: the file grew while it was copied",
			            dest);
			ok = FALSE;
			break;
		}

		if (!write_all(out, buf, (gsize) r, dest, error)) {
			ok = FALSE;
			break;
		}
	}

	return finish_new_file(out, dest, ok, error);
}

gboolean mms_write_new_file(const char *dest, const void *data, gsize len, mode_t mode, gid_t gid,
                            GError **error)
{
	int out = create_new_file(dest, mode, gid, error);

	if (out < 0)
		return FALSE;

	return finish_new_file(out, dest, write_all(out, data, len, dest, error), error);
}

static gint compare_sizes(gconstpointer a, gconstpointer b, gpointer user_data)
{
	const gsize *sizes = user_data;
	gsize sa = sizes[*(const guint *) a];
	gsize sb = sizes[*(const guint *) b];

	return sa < sb ? -1 : (sa > sb ? 1 : 0);
}

gboolean mms_plan_fit(const gsize *sizes, const gboolean *fittable, guint n, gsize budget, gsize *targets)
{
	GArray *pictures = g_array_new(FALSE, FALSE, sizeof(guint));
	gsize left = budget;
	gboolean ok = TRUE;
	guint i;

	for (i = 0; i < n; i++) {
		targets[i] = sizes[i];

		if (fittable[i]) {
			g_array_append_val(pictures, i);
		}
		else if (sizes[i] > left) {
			ok = FALSE;
		}
		else {
			left -= sizes[i];
		}
	}

	/* smallest first: whatever a small one does not need, the larger
	 * ones after it share */
	g_array_sort_with_data(pictures, compare_sizes, (gpointer) sizes);

	for (i = 0; ok && i < pictures->len; i++) {
		guint index = g_array_index(pictures, guint, i);
		gsize share = left / (pictures->len - i);

		if (share == 0) {
			ok = FALSE;
			break;
		}

		targets[index] = MIN(sizes[index], share);
		left -= targets[index];
	}

	g_array_free(pictures, TRUE);
	return ok;
}

void mms_send_part_free(struct mms_send_part *part)
{
	if (!part)
		return;

	g_free(part->content_id);
	g_free(part->content_type);
	g_free(part->file);
	g_free(part->smil_src);
	g_free(part);
}

static const char* smil_element(const char *content_type)
{
	if (g_str_has_prefix(content_type, "image/"))
		return "img";
	if (g_str_has_prefix(content_type, "video/"))
		return "video";
	if (g_str_has_prefix(content_type, "audio/"))
		return "audio";
	if (g_str_has_prefix(content_type, "text/plain"))
		return "text";

	return "ref";
}

static void append_smil_ref(GString *smil, const struct mms_send_part *part)
{
	const char *element = smil_element(part->content_type);
	gchar *src = g_markup_escape_text(part->smil_src, -1);

	g_string_append_printf(smil, "<%s src=\"%s\" region=\"%s\"/>", element, src,
	                       strcmp(element, "text") == 0 ? "Text" : "Image");
	g_free(src);
}

gchar* mms_build_smil(GPtrArray *parts)
{
	GString *smil = g_string_new(
		"<smil><head><layout>"
		"<root-layout width=\"100%\" height=\"100%\"/>"
		"<region id=\"Image\" top=\"0%\" left=\"0%\" height=\"80%\" width=\"100%\" fit=\"meet\"/>"
		"<region id=\"Text\" top=\"80%\" left=\"0%\" height=\"20%\" width=\"100%\" fit=\"scroll\"/>"
		"</layout></head><body>");
	const struct mms_send_part *body_text = NULL;
	bool body_placed = false;
	guint n;

	for (n = 0; n < parts->len; n++) {
		const struct mms_send_part *part = g_ptr_array_index(parts, n);

		if (strcmp(smil_element(part->content_type), "text") == 0) {
			body_text = part;
			break;
		}
	}

	for (n = 0; n < parts->len; n++) {
		const struct mms_send_part *part = g_ptr_array_index(parts, n);

		if (part == body_text)
			continue;

		g_string_append(smil, "<par dur=\"5000ms\">");
		append_smil_ref(smil, part);
		if (body_text && !body_placed &&
		    strcmp(smil_element(part->content_type), "text") != 0) {
			append_smil_ref(smil, body_text);
			body_placed = true;
		}
		g_string_append(smil, "</par>");
	}

	if (body_text && !body_placed) {
		g_string_append(smil, "<par dur=\"5000ms\">");
		append_smil_ref(smil, body_text);
		g_string_append(smil, "</par>");
	}

	g_string_append(smil, "</body></smil>");

	return g_string_free(smil, FALSE);
}

const char* mms_db_status_for(const char *mmsd_status)
{
	if (!mmsd_status)
		return NULL;

	if (strcmp(mmsd_status, "sent") == 0 || strcmp(mmsd_status, "received") == 0 ||
	    strcmp(mmsd_status, "read") == 0)
		return "successful";

	return NULL;
}

struct error_map {
	const char *name;
	const char *code;
};

static const struct error_map error_map[] = {
	{ "org.ofono.mms.Error.InvalidArguments", MMS_ERR_UNSUPPORTED },
	{ "org.ofono.mms.Error.UnsupportedMessage", MMS_ERR_UNSUPPORTED },
	{ "org.ofono.mms.Error.TransientFailure", MMS_ERR_UNSPECIFIED },
	{ "org.ofono.mms.Error.TransientNetworkProblem", MMS_ERR_NETWORK },
	{ "org.ofono.mms.Error.PermanentFailure", MMS_ERR_UNSPECIFIED },
	{ "org.ofono.mms.Error.PermanentServiceDenied", MMS_ERR_DENIED },
	{ "org.ofono.mms.Error.PermanentMessageFormatCorrupt", MMS_ERR_CORRUPT },
	{ "org.ofono.mms.Error.PermanentInvalidAddress", MMS_ERR_BAD_ADDRESS },
	{ "org.ofono.mms.Error.PermanentContentNotAccepted", MMS_ERR_REJECTED },
	{ "org.ofono.mms.Error.PermanentLackOfPrepaid", MMS_ERR_DENIED },
	{ "org.freedesktop.DBus.Error.ServiceUnknown", MMS_ERR_NOT_READY },
	{ "org.freedesktop.DBus.Error.NameHasNoOwner", MMS_ERR_NOT_READY },
	{ "org.freedesktop.DBus.Error.UnknownObject", MMS_ERR_NOT_READY },
	{ "org.freedesktop.DBus.Error.UnknownMethod", MMS_ERR_NOT_READY },
	{ "org.freedesktop.DBus.Error.NoReply", MMS_ERR_NETWORK },
	{ "org.freedesktop.DBus.Error.Timeout", MMS_ERR_NETWORK },
	{ NULL, NULL }
};

const char* mms_error_code_for_dbus_error(const char *error_name)
{
	const struct error_map *m;

	if (!error_name)
		return MMS_ERR_UNSPECIFIED;

	for (m = error_map; m->name; m++) {
		if (strcmp(error_name, m->name) == 0)
			return m->code;
	}

	return MMS_ERR_UNSPECIFIED;
}

gchar* mms_path_basename(const char *object_path)
{
	const char *slash;

	if (!object_path || object_path[0] != '/')
		return NULL;

	slash = strrchr(object_path, '/');
	if (!slash[1])
		return NULL;

	return g_strdup(slash + 1);
}

gchar* mms_received_file_name(const char *content_id, guint index, const char *content_type)
{
	const char *ext = mms_extension_for_content_type(content_type);
	GString *name = g_string_new(NULL);
	const char *p;
	const char *dot;
	gchar *stem;
	gchar *result;

	for (p = content_id ? content_id : ""; *p && name->len < 64; p++) {
		if (g_ascii_isalnum(*p) || *p == '.' || *p == '_' || *p == '-')
			g_string_append_c(name, *p);
	}

	/* nothing usable, or only dots */
	while (name->len > 0 && name->str[0] == '.')
		g_string_erase(name, 0, 1);

	if (name->len == 0) {
		g_string_free(name, TRUE);
		return g_strdup_printf("part%u.%s", index, ext);
	}

	/* keep the sender's extension only when it fits the type */
	dot = strrchr(name->str, '.');
	if (dot && g_ascii_strcasecmp(dot + 1, ext) == 0)
		stem = g_strndup(name->str, (gsize) (dot - name->str));
	else
		stem = g_strdup(name->str);

	result = g_strdup_printf("%u-%s.%s", index, stem, ext);

	g_free(stem);
	g_string_free(name, TRUE);
	return result;
}

GBytes* mms_read_range(const char *file, guint64 offset, guint64 len, GError **error)
{
	struct stat st;
	guint8 *data;
	guint64 done = 0;
	int fd;

	if (len > MMS_MAX_PART_READ) {
		g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "%s: a part too large for an MMS", file);
		return NULL;
	}

	fd = open(file, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0) {
		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "open %s: %s", file,
		            g_strerror(errno));
		return NULL;
	}

	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
	    offset > (guint64) st.st_size || len > (guint64) st.st_size - offset) {
		g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "%s: range outside the file", file);
		close(fd);
		return NULL;
	}

	data = g_malloc(len ? (gsize) len : 1);

	while (done < len) {
		ssize_t r = pread(fd, data + done, (size_t) (len - done), (off_t) (offset + done));

		if (r < 0 && errno == EINTR)
			continue;

		if (r <= 0) {
			g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(r < 0 ? errno : EIO),
			            "read %s: %s", file, r < 0 ? g_strerror(errno) : "short file");
			g_free(data);
			close(fd);
			return NULL;
		}

		done += (guint64) r;
	}

	close(fd);
	return g_bytes_new_take(data, (gsize) len);
}

gchar* mms_content_type_param(const char *content_type, const char *name)
{
	gchar **params;
	gchar *value = NULL;
	int n;

	if (!content_type || !name)
		return NULL;

	params = g_strsplit(content_type, ";", -1);

	/* params[0] is the type itself */
	for (n = 1; params[0] && params[n] && !value; n++) {
		gchar *param = g_strstrip(params[n]);
		gchar *eq = strchr(param, '=');
		gchar *key;
		gchar *val;
		size_t vlen;

		if (!eq)
			continue;

		*eq = '\0';
		key = g_strstrip(param);
		val = g_strstrip(eq + 1);

		if (g_ascii_strcasecmp(key, name) != 0)
			continue;

		vlen = strlen(val);
		if (vlen >= 2 && val[0] == '"' && val[vlen - 1] == '"') {
			val[vlen - 1] = '\0';
			val++;
		}

		value = g_strdup(val);
	}

	g_strfreev(params);
	return value;
}

static bool is_latin1(const char *charset)
{
	static const char * const names[] = { "iso-8859-1", "iso8859-1", "iso_8859-1", "latin1", "l1", NULL };
	int n;

	for (n = 0; names[n]; n++) {
		if (g_ascii_strcasecmp(charset, names[n]) == 0)
			return true;
	}

	return false;
}

/* Latin-1 is the one 8-bit charset MMS text still comes in; it maps byte
 * for byte onto U+0000..U+00FF. Done here because LuneOS images do not
 * carry glibc's ISO8859-1 gconv module, so g_convert() cannot. */
static gchar* latin1_to_utf8(const char *bytes, gsize len)
{
	GString *out = g_string_sized_new(len + len / 2 + 1);
	gsize n;

	for (n = 0; n < len; n++)
		g_string_append_unichar(out, (gunichar) (guchar) bytes[n]);

	return g_string_free(out, FALSE);
}

gchar* mms_part_text(const void *data, gsize len, const char *content_type)
{
	gchar *charset = mms_content_type_param(content_type, "charset");
	gchar *text = NULL;
	const char *bytes = data;

	/* mmsd's own text parts end in a NUL */
	while (len > 0 && bytes[len - 1] == '\0')
		len--;

	if (charset && is_latin1(charset)) {
		text = latin1_to_utf8(bytes, len);
	}
	else if (charset && g_ascii_strcasecmp(charset, "utf-8") != 0 && g_ascii_strcasecmp(charset, "utf8") != 0 &&
	         g_ascii_strcasecmp(charset, "us-ascii") != 0) {
		gchar *converted = g_convert(bytes, (gssize) len, "UTF-8", charset, NULL, NULL, NULL);

		if (converted) {
			text = g_utf8_make_valid(converted, -1);
			g_free(converted);
		}
	}

	if (!text)
		text = g_utf8_make_valid(len ? bytes : "", (gssize) len);

	g_free(charset);
	return text;
}

// vim:ts=4:sw=4:noexpandtab
