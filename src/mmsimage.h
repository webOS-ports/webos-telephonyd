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
 * Making a picture small enough for an MMS.
 *
 * A photo or a screenshot is easily larger than what a carrier takes in one
 * MMS. Such a picture is decoded with gdk-pixbuf, turned upright by its EXIF
 * orientation, scaled down with its aspect kept and encoded as JPEG, a size
 * and a quality step lower each time, until it fits. Only the copy that goes
 * into the spool is made; the user's file is only read.
 *
 * Only JPEG and PNG are taken: those are the formats LuneOS' gdk-pixbuf
 * (2.44, no loader modules) has built in. A GIF can be animated and would
 * lose that, and WebP, BMP and the rest cannot be decoded on the device at
 * all; they go as they are, or not at all when they are too large.
 */

#ifndef MMS_IMAGE_H_
#define MMS_IMAGE_H_

#include <stdbool.h>
#include <glib.h>

/* A picture file larger than this is not even read. */
#define MMS_IMAGE_MAX_FILE_SIZE ((gsize) 50 * 1024 * 1024)
/* Nor one with more pixels than this: PNG is decoded at full size first, at
 * 4 bytes a pixel. 40 million covers a 12 MP camera and an 8K screenshot. */
#define MMS_IMAGE_MAX_PIXELS ((guint64) 40 * 1000 * 1000)
/* The longest side a picture is first tried at; never scaled up. */
#define MMS_IMAGE_FIRST_SIDE 1280

/* Errors of mms_image_fit, besides the G_FILE_ERROR of reading the file. */
#define MMS_IMAGE_ERROR (mms_image_error_quark())
enum mms_image_error {
	MMS_IMAGE_ERROR_DECODE,		/* not a picture gdk-pixbuf can read */
	MMS_IMAGE_ERROR_TOO_LARGE,	/* too large a file or too many pixels to decode */
	MMS_IMAGE_ERROR_NO_FIT,		/* larger than asked even at the smallest step */
};
GQuark mms_image_error_quark(void);

/* Whether a picture of this content type (bare, lower case) can be made
 * smaller: image/jpeg (and its aliases image/jpg, image/pjpeg) and
 * image/png. */
bool mms_image_fittable(const char *content_type);

/**
 * Read the picture in fd (from its current offset, at most
 * MMS_IMAGE_MAX_FILE_SIZE bytes) and encode it upright as a JPEG of at most
 * max_bytes, trying the longest side MMS_IMAGE_FIRST_SIDE (or the picture's
 * own, when smaller) and then smaller ones, each at a few qualities. A
 * transparent picture is put on white. Returns the JPEG, or NULL with error
 * set when the picture cannot be decoded, is too large to decode, or does
 * not fit even at the smallest step.
 */
GBytes* mms_image_fit(int fd, gsize max_bytes, GError **error);

#endif

// vim:ts=4:sw=4:noexpandtab
