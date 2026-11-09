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
#include <string.h>
#include <unistd.h>

#include <gdk-pixbuf/gdk-pixbuf.h>

#include "mmsimage.h"

/* The longest sides tried after the first, and the JPEG qualities tried at
 * each side, best first. */
static const int fit_sides[] = { 1024, 800, 640, 480, 320, 240, 160 };
static const int fit_qualities[] = { 85, 70, 55 };

G_DEFINE_QUARK(mms-image-error-quark, mms_image_error)

bool mms_image_fittable(const char *content_type)
{
	static const char * const types[] = { "image/jpeg", "image/jpg", "image/pjpeg", "image/png", NULL };
	int n;

	for (n = 0; content_type && types[n]; n++) {
		if (strcmp(content_type, types[n]) == 0)
			return true;
	}

	return false;
}

struct load_state {
	gboolean too_large;
};

/* Refuses a picture with too many pixels before it is decoded, and has the
 * loader scale the rest down to the first side while decoding (a JPEG is
 * then decoded at a fraction of its size). */
static void size_prepared(GdkPixbufLoader *loader, int width, int height, gpointer user_data)
{
	struct load_state *state = user_data;
	int longest = MAX(width, height);

	if (width <= 0 || height <= 0 || (guint64) width * (guint64) height > MMS_IMAGE_MAX_PIXELS) {
		state->too_large = TRUE;
		/* makes the loader stop with an error */
		gdk_pixbuf_loader_set_size(loader, 0, 0);
		return;
	}

	if (longest > MMS_IMAGE_FIRST_SIDE) {
		double scale = (double) MMS_IMAGE_FIRST_SIDE / longest;

		gdk_pixbuf_loader_set_size(loader, MAX(1, (int) (width * scale + 0.5)),
		                           MAX(1, (int) (height * scale + 0.5)));
	}
}

static GdkPixbuf* load(int fd, GError **error)
{
	GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
	struct load_state state = { FALSE };
	GdkPixbuf *pixbuf = NULL;
	gboolean ok = TRUE;
	gsize total = 0;
	guchar buf[65536];

	g_signal_connect(loader, "size-prepared", G_CALLBACK(size_prepared), &state);

	while (ok) {
		ssize_t r = read(fd, buf, sizeof(buf));

		if (r < 0 && errno == EINTR)
			continue;

		if (r < 0) {
			g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "read: %s", g_strerror(errno));
			ok = FALSE;
			break;
		}

		if (r == 0)
			break;

		total += (gsize) r;
		if (total > MMS_IMAGE_MAX_FILE_SIZE) {
			g_set_error(error, MMS_IMAGE_ERROR, MMS_IMAGE_ERROR_TOO_LARGE, "the picture is larger than %" G_GSIZE_FORMAT
			            " bytes", MMS_IMAGE_MAX_FILE_SIZE);
			ok = FALSE;
			break;
		}

		ok = gdk_pixbuf_loader_write(loader, buf, (gsize) r, error);
	}

	/* a loader has to be closed in any case; its error only counts when
	 * nothing went wrong before */
	if (!gdk_pixbuf_loader_close(loader, ok ? error : NULL))
		ok = FALSE;

	if (state.too_large) {
		g_clear_error(error);
		g_set_error(error, MMS_IMAGE_ERROR, MMS_IMAGE_ERROR_TOO_LARGE, "the picture has more than %" G_GUINT64_FORMAT
		            " pixels", MMS_IMAGE_MAX_PIXELS);
		ok = FALSE;
	}

	if (ok) {
		pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
		if (pixbuf)
			g_object_ref(pixbuf);
		else
			g_set_error(error, MMS_IMAGE_ERROR, MMS_IMAGE_ERROR_DECODE, "no picture in the file");
	}
	else if (error && !*error) {
		g_set_error(error, MMS_IMAGE_ERROR, MMS_IMAGE_ERROR_DECODE, "the picture cannot be decoded");
	}

	g_object_unref(loader);
	return pixbuf;
}

/* The picture at the given longest side, without transparency. */
static GdkPixbuf* scaled_opaque(GdkPixbuf *src, int side)
{
	int sw = gdk_pixbuf_get_width(src);
	int sh = gdk_pixbuf_get_height(src);
	int w, h;
	GdkPixbuf *dest;

	if (sw >= sh) {
		w = side;
		h = MAX(1, (int) ((double) sh * side / sw + 0.5));
	}
	else {
		h = side;
		w = MAX(1, (int) ((double) sw * side / sh + 0.5));
	}

	if (!gdk_pixbuf_get_has_alpha(src))
		return (w == sw && h == sh) ? g_object_ref(src) : gdk_pixbuf_scale_simple(src, w, h, GDK_INTERP_BILINEAR);

	/* JPEG has no alpha: the saver would just drop it, and whatever colour
	 * the transparent pixels have would show. Put it on white. */
	dest = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, w, h);
	if (!dest)
		return NULL;
	gdk_pixbuf_fill(dest, 0xffffffffu);
	gdk_pixbuf_composite(src, dest, 0, 0, w, h, 0.0, 0.0, (double) w / sw, (double) h / sh,
	                     GDK_INTERP_BILINEAR, 255);

	return dest;
}

/* Tries one side at each quality; the JPEG on the first that fits. */
static GBytes* encode_at(GdkPixbuf *upright, int side, gsize max_bytes, GError **error)
{
	GdkPixbuf *scaled = scaled_opaque(upright, side);
	GBytes *out = NULL;
	guint q;

	if (!scaled) {
		g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOMEM, "no memory for a %d pixel picture", side);
		return NULL;
	}

	for (q = 0; q < G_N_ELEMENTS(fit_qualities) && !out; q++) {
		gchar *data = NULL;
		gsize len = 0;
		char quality[8];

		g_snprintf(quality, sizeof(quality), "%d", fit_qualities[q]);
		if (!gdk_pixbuf_save_to_buffer(scaled, &data, &len, "jpeg", error, "quality", quality, NULL))
			break;

		if (len <= max_bytes)
			out = g_bytes_new_take(data, len);
		else
			g_free(data);
	}

	g_object_unref(scaled);
	return out;
}

GBytes* mms_image_fit(int fd, gsize max_bytes, GError **error)
{
	GdkPixbuf *loaded;
	GdkPixbuf *upright;
	GBytes *out = NULL;
	GError *local = NULL;
	int longest;
	int side;
	guint n;

	loaded = load(fd, error);
	if (!loaded)
		return NULL;

	/* the orientation option of a JPEG's EXIF data, if any */
	upright = gdk_pixbuf_apply_embedded_orientation(loaded);
	g_object_unref(loaded);
	if (!upright) {
		g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOMEM, "the picture cannot be turned upright");
		return NULL;
	}

	longest = MAX(gdk_pixbuf_get_width(upright), gdk_pixbuf_get_height(upright));
	side = MIN(longest, MMS_IMAGE_FIRST_SIDE);

	out = encode_at(upright, side, max_bytes, &local);
	for (n = 0; !out && !local && n < G_N_ELEMENTS(fit_sides); n++) {
		if (fit_sides[n] < side)
			out = encode_at(upright, fit_sides[n], max_bytes, &local);
	}

	if (!out) {
		if (local)
			g_propagate_error(error, local);
		else
			g_set_error(error, MMS_IMAGE_ERROR, MMS_IMAGE_ERROR_NO_FIT, "the picture does not fit into %"
			            G_GSIZE_FORMAT " bytes", max_bytes);
	}

	g_object_unref(upright);
	return out;
}

// vim:ts=4:sw=4:noexpandtab
