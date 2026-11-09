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
 * Pictures made small enough for an MMS (src/mmsimage.c) and how the files
 * of a message share its size (mms_plan_fit in src/mmscore.c). The pictures
 * are made here with gdk-pixbuf: noise, so that they do not compress away.
 */

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#include "mmscore.h"
#include "mmsimage.h"

#define LIMIT (MMS_DEFAULT_MESSAGE_SIZE - MMS_ENVELOPE_RESERVE)

static gchar *tmp;

/* A picture of noise; with alpha, its left quarter is fully transparent. */
static GdkPixbuf* noise(int width, int height, gboolean alpha, guint32 seed)
{
	GdkPixbuf *p = gdk_pixbuf_new(GDK_COLORSPACE_RGB, alpha, 8, width, height);
	GRand *rand = g_rand_new_with_seed(seed);
	int channels = gdk_pixbuf_get_n_channels(p);
	int stride = gdk_pixbuf_get_rowstride(p);
	guchar *pixels = gdk_pixbuf_get_pixels(p);
	int x, y;

	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			guchar *px = pixels + (gsize) y * stride + (gsize) x * channels;
			int c;

			for (c = 0; c < 3; c++)
				px[c] = (guchar) g_rand_int_range(rand, 0, 256);
			if (alpha)
				px[3] = x < width / 4 ? 0 : 255;
		}
	}

	g_rand_free(rand);
	return p;
}

static gchar* save(GdkPixbuf *p, const char *name, const char *type, const char *quality)
{
	gchar *path = g_build_filename(tmp, name, NULL);

	if (quality)
		g_assert_true(gdk_pixbuf_save(p, path, type, NULL, "quality", quality, NULL));
	else
		g_assert_true(gdk_pixbuf_save(p, path, type, NULL, NULL));

	return path;
}

static gsize file_size(const char *path)
{
	GStatBuf st;

	g_assert_cmpint(g_stat(path, &st), ==, 0);
	return (gsize) st.st_size;
}

static GBytes* fit_file(const char *path, gsize max, GError **error)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	GBytes *out;

	g_assert_cmpint(fd, >=, 0);
	out = mms_image_fit(fd, max, error);
	close(fd);

	return out;
}

/* The JPEG decoded again; checks that it is one. */
static GdkPixbuf* decode_jpeg(GBytes *jpeg)
{
	GdkPixbufLoader *loader = gdk_pixbuf_loader_new_with_type("jpeg", NULL);
	GdkPixbuf *p;
	gsize len;
	const guchar *data = g_bytes_get_data(jpeg, &len);

	g_assert_cmpuint(len, >, 2);
	g_assert_cmpuint(data[0], ==, 0xff);
	g_assert_cmpuint(data[1], ==, 0xd8);
	g_assert_true(gdk_pixbuf_loader_write(loader, data, len, NULL));
	g_assert_true(gdk_pixbuf_loader_close(loader, NULL));
	p = g_object_ref(gdk_pixbuf_loader_get_pixbuf(loader));
	g_object_unref(loader);

	return p;
}

static void assert_aspect(GdkPixbuf *p, int width, int height)
{
	double want = (double) width / height;
	double got = (double) gdk_pixbuf_get_width(p) / gdk_pixbuf_get_height(p);

	g_assert_cmpfloat(got, >, want * 0.99);
	g_assert_cmpfloat(got, <, want * 1.01);
}

static void pixel(GdkPixbuf *p, int x, int y, guchar rgb[3])
{
	const guchar *px = gdk_pixbuf_read_pixels(p) + (gsize) y * gdk_pixbuf_get_rowstride(p) +
	                   (gsize) x * gdk_pixbuf_get_n_channels(p);

	memcpy(rgb, px, 3);
}

static gchar* checksum_of(const char *path)
{
	gchar *data = NULL;
	gsize len = 0;
	gchar *sum;

	g_assert_true(g_file_get_contents(path, &data, &len, NULL));
	sum = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *) data, len);
	g_free(data);

	return sum;
}

static void test_fittable(void)
{
	g_assert_true(mms_image_fittable("image/jpeg"));
	g_assert_true(mms_image_fittable("image/jpg"));
	g_assert_true(mms_image_fittable("image/pjpeg"));
	g_assert_true(mms_image_fittable("image/png"));
	/* animated, or not decodable on the device: they go as they are */
	g_assert_false(mms_image_fittable("image/gif"));
	g_assert_false(mms_image_fittable("image/webp"));
	g_assert_false(mms_image_fittable("image/bmp"));
	g_assert_false(mms_image_fittable("video/mp4"));
	g_assert_false(mms_image_fittable("text/plain"));
	g_assert_false(mms_image_fittable(NULL));
}

static void test_plan(void)
{
	gsize targets[4];

	/* everything fits: nothing changes */
	{
		const gsize sizes[] = { 1000, 50000, 3000 };
		const gboolean fit[] = { TRUE, TRUE, FALSE };

		g_assert_true(mms_plan_fit(sizes, fit, 3, 60000, targets));
		g_assert_cmpuint(targets[0], ==, 1000);
		g_assert_cmpuint(targets[1], ==, 50000);
		g_assert_cmpuint(targets[2], ==, 3000);
	}

	/* two large pictures share what the other file leaves */
	{
		const gsize sizes[] = { 900000, 20000, 2000000 };
		const gboolean fit[] = { TRUE, FALSE, TRUE };

		g_assert_true(mms_plan_fit(sizes, fit, 3, 300000, targets));
		g_assert_cmpuint(targets[1], ==, 20000);
		g_assert_cmpuint(targets[0], ==, 140000);
		g_assert_cmpuint(targets[2], ==, 140000);
	}

	/* a small picture keeps its size, the large one gets the rest */
	{
		const gsize sizes[] = { 2000000, 10000 };
		const gboolean fit[] = { TRUE, TRUE };

		g_assert_true(mms_plan_fit(sizes, fit, 2, 300000, targets));
		g_assert_cmpuint(targets[1], ==, 10000);
		g_assert_cmpuint(targets[0], ==, 290000);
	}

	/* what cannot be made smaller has to fit as it is */
	{
		const gsize sizes[] = { 10, 300001 };
		const gboolean fit[] = { TRUE, FALSE };

		g_assert_false(mms_plan_fit(sizes, fit, 2, 300000, targets));
	}

	/* nothing left for a picture */
	{
		const gsize sizes[] = { 300000, 5 };
		const gboolean fit[] = { FALSE, TRUE };

		g_assert_false(mms_plan_fit(sizes, fit, 2, 300000, targets));
	}

	/* no files */
	g_assert_true(mms_plan_fit(NULL, NULL, 0, 300000, targets));
}

static void test_big_png(void)
{
	GdkPixbuf *src = noise(1600, 1000, TRUE, 1);
	gchar *path = save(src, "shot.png", "png", NULL);
	gchar *before = checksum_of(path);
	gchar *after;
	GError *error = NULL;
	GBytes *jpeg;
	GdkPixbuf *out;
	guchar rgb[3];

	g_assert_cmpuint(file_size(path), >, MMS_DEFAULT_MESSAGE_SIZE);

	jpeg = fit_file(path, LIMIT, &error);
	g_assert_no_error(error);
	g_assert_nonnull(jpeg);
	g_assert_cmpuint(g_bytes_get_size(jpeg), <=, LIMIT);

	out = decode_jpeg(jpeg);
	assert_aspect(out, 1600, 1000);
	g_assert_cmpint(gdk_pixbuf_get_width(out), <=, MMS_IMAGE_FIRST_SIDE);
	g_assert_false(gdk_pixbuf_get_has_alpha(out));
	/* the transparent quarter came out white, not black */
	pixel(out, 2, gdk_pixbuf_get_height(out) / 2, rgb);
	g_assert_cmpuint(rgb[0], >, 230);
	g_assert_cmpuint(rgb[1], >, 230);
	g_assert_cmpuint(rgb[2], >, 230);

	/* the file is only read */
	after = checksum_of(path);
	g_assert_cmpstr(before, ==, after);

	g_object_unref(out);
	g_bytes_unref(jpeg);
	g_object_unref(src);
	g_free(before);
	g_free(after);
	g_free(path);
}

static void test_big_jpeg(void)
{
	GdkPixbuf *src = noise(2400, 1350, FALSE, 2);
	gchar *path = save(src, "photo.jpg", "jpeg", "95");
	GError *error = NULL;
	GBytes *jpeg;
	GdkPixbuf *out;

	g_assert_cmpuint(file_size(path), >, MMS_DEFAULT_MESSAGE_SIZE);

	jpeg = fit_file(path, LIMIT, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(g_bytes_get_size(jpeg), <=, LIMIT);
	out = decode_jpeg(jpeg);
	assert_aspect(out, 2400, 1350);

	/* a tighter budget: smaller still, aspect still kept */
	g_bytes_unref(jpeg);
	g_object_unref(out);
	jpeg = fit_file(path, 40000, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(g_bytes_get_size(jpeg), <=, 40000);
	out = decode_jpeg(jpeg);
	assert_aspect(out, 2400, 1350);
	g_assert_cmpint(gdk_pixbuf_get_width(out), <, MMS_IMAGE_FIRST_SIDE);

	g_object_unref(out);
	g_bytes_unref(jpeg);
	g_object_unref(src);
	g_free(path);
}

/* A JPEG as a camera stores it: 1200x600 on the sensor, left half red and
 * right half blue, with EXIF Orientation 6 ("rotate 90 degrees clockwise to
 * show it"). Upright it is 600x1200 with red on top. */
static gchar* rotated_jpeg(void)
{
	static const guchar exif[] = {
		0xff, 0xe1, 0x00, 0x22,					/* APP1, length 34 */
		'E', 'x', 'i', 'f', 0x00, 0x00,
		'I', 'I', 0x2a, 0x00, 0x08, 0x00, 0x00, 0x00,		/* TIFF, IFD at 8 */
		0x01, 0x00,						/* one entry */
		0x12, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00,		/* Orientation, SHORT, 1 */
		0x06, 0x00, 0x00, 0x00,					/* 6 */
		0x00, 0x00, 0x00, 0x00,					/* no next IFD */
	};
	GdkPixbuf *p = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 1200, 600);
	gchar *plain;
	gchar *data = NULL;
	gsize len = 0;
	GByteArray *out = g_byte_array_new();
	gchar *path = g_build_filename(tmp, "camera.jpg", NULL);
	int x, y;

	for (y = 0; y < 600; y++) {
		for (x = 0; x < 1200; x++) {
			guchar *px = gdk_pixbuf_get_pixels(p) + (gsize) y * gdk_pixbuf_get_rowstride(p) + (gsize) x * 3;

			px[0] = x < 600 ? 255 : 0;
			px[1] = 0;
			px[2] = x < 600 ? 0 : 255;
		}
	}

	plain = save(p, "camera-plain.jpg", "jpeg", "90");
	g_assert_true(g_file_get_contents(plain, &data, &len, NULL));
	g_assert_cmpuint(len, >, 2);

	/* SOI, the EXIF segment, then the rest */
	g_byte_array_append(out, (const guint8 *) data, 2);
	g_byte_array_append(out, exif, sizeof(exif));
	g_byte_array_append(out, (const guint8 *) data + 2, (guint) (len - 2));
	g_assert_true(g_file_set_contents(path, (const gchar *) out->data, out->len, NULL));

	g_byte_array_free(out, TRUE);
	g_free(data);
	g_free(plain);
	g_object_unref(p);

	return path;
}

static void test_orientation(void)
{
	gchar *path = rotated_jpeg();
	GError *error = NULL;
	GBytes *jpeg = fit_file(path, LIMIT, &error);
	GdkPixbuf *out;
	guchar top[3];
	guchar bottom[3];
	int w, h;

	g_assert_no_error(error);
	out = decode_jpeg(jpeg);
	w = gdk_pixbuf_get_width(out);
	h = gdk_pixbuf_get_height(out);

	/* upright: portrait, red above blue */
	g_assert_cmpint(w, ==, 600);
	g_assert_cmpint(h, ==, 1200);
	pixel(out, w / 2, h / 8, top);
	pixel(out, w / 2, h - h / 8, bottom);
	g_assert_cmpuint(top[0], >, 200);
	g_assert_cmpuint(top[2], <, 60);
	g_assert_cmpuint(bottom[0], <, 60);
	g_assert_cmpuint(bottom[2], >, 200);

	g_object_unref(out);
	g_bytes_unref(jpeg);
	g_free(path);
}

static void test_corrupt(void)
{
	static const char * const bad[] = {
		"\xff\xd8\xff\xe0 this is no JPEG at all, only its first bytes",
		"\x89PNG\r\n\x1a\n and then nothing that a PNG has",
		"plain text",
	};
	guint n;

	for (n = 0; n < G_N_ELEMENTS(bad); n++) {
		gchar *path = g_build_filename(tmp, "bad.img", NULL);
		GError *error = NULL;
		GBytes *out;

		g_assert_true(g_file_set_contents(path, bad[n], -1, NULL));
		out = fit_file(path, LIMIT, &error);
		g_assert_null(out);
		g_assert_nonnull(error);
		g_test_message("%u: %s", n, error->message);
		g_clear_error(&error);
		g_unlink(path);
		g_free(path);
	}
}

static guint32 crc32_of(const guchar *data, gsize len)
{
	guint32 crc = 0xffffffffu;
	gsize i;
	int k;

	for (i = 0; i < len; i++) {
		crc ^= data[i];
		for (k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
	}

	return crc ^ 0xffffffffu;
}

static void put_be32(guchar *p, guint32 v)
{
	p[0] = (guchar) (v >> 24);
	p[1] = (guchar) (v >> 16);
	p[2] = (guchar) (v >> 8);
	p[3] = (guchar) v;
}

/* A PNG that says it is 10000x10000 pixels: refused before it is decoded. */
static void test_too_many_pixels(void)
{
	guchar png[8 + 25 + 12 + 12];
	gchar *path = g_build_filename(tmp, "huge.png", NULL);
	GError *error = NULL;
	GBytes *out;

	memcpy(png, "\x89PNG\r\n\x1a\n", 8);
	put_be32(png + 8, 13);
	memcpy(png + 12, "IHDR", 4);
	put_be32(png + 16, 10000);
	put_be32(png + 20, 10000);
	png[24] = 8;	/* bit depth */
	png[25] = 2;	/* RGB */
	png[26] = 0;
	png[27] = 0;
	png[28] = 0;
	put_be32(png + 29, crc32_of(png + 12, 17));
	/* an empty IDAT: libpng reports the header at the first one */
	put_be32(png + 33, 0);
	memcpy(png + 37, "IDAT", 4);
	put_be32(png + 41, crc32_of(png + 37, 4));
	put_be32(png + 45, 0);
	memcpy(png + 49, "IEND", 4);
	put_be32(png + 53, crc32_of(png + 49, 4));

	g_assert_true(g_file_set_contents(path, (const gchar *) png, sizeof(png), NULL));
	out = fit_file(path, LIMIT, &error);
	g_assert_null(out);
	g_assert_error(error, MMS_IMAGE_ERROR, MMS_IMAGE_ERROR_TOO_LARGE);
	g_assert_nonnull(strstr(error->message, "pixels"));

	g_clear_error(&error);
	g_unlink(path);
	g_free(path);
}

static void test_cannot_fit(void)
{
	GdkPixbuf *src = noise(400, 300, FALSE, 3);
	gchar *path = save(src, "small.png", "png", NULL);
	GError *error = NULL;
	GBytes *out = fit_file(path, 200, &error);

	g_assert_null(out);
	g_assert_error(error, MMS_IMAGE_ERROR, MMS_IMAGE_ERROR_NO_FIT);

	g_clear_error(&error);
	g_object_unref(src);
	g_free(path);
}

int main(int argc, char **argv)
{
	int ret;

	g_test_init(&argc, &argv, NULL);

	tmp = g_dir_make_tmp("test_mmsimage-XXXXXX", NULL);
	g_assert_nonnull(tmp);

	g_test_add_func("/mmsimage/fittable", test_fittable);
	g_test_add_func("/mmsimage/plan", test_plan);
	g_test_add_func("/mmsimage/big-png", test_big_png);
	g_test_add_func("/mmsimage/big-jpeg", test_big_jpeg);
	g_test_add_func("/mmsimage/orientation", test_orientation);
	g_test_add_func("/mmsimage/corrupt", test_corrupt);
	g_test_add_func("/mmsimage/too-many-pixels", test_too_many_pixels);
	g_test_add_func("/mmsimage/cannot-fit", test_cannot_fit);

	ret = g_test_run();

	{
		GDir *dir = g_dir_open(tmp, 0, NULL);
		const char *name;

		while (dir && (name = g_dir_read_name(dir))) {
			gchar *path = g_build_filename(tmp, name, NULL);

			g_unlink(path);
			g_free(path);
		}
		if (dir)
			g_dir_close(dir);
		g_rmdir(tmp);
	}
	g_free(tmp);

	return ret;
}

// vim:ts=4:sw=4:noexpandtab
