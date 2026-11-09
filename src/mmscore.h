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
 * The parts of sending and receiving an MMS that need neither D-Bus nor luna.
 *
 * mmsd (org.ofono.mms) sends a message from a list of recipients, a SMIL
 * document and a list of (content id, content type, file name) parts. It
 * opens each file itself, as user mmsd and inside its systemd sandbox, and
 * encodes the whole message into its store before SendMessage returns. So
 * the files the user picked are copied by us (root) into a spool directory
 * that only root and group mmsd can read, handed over, and removed again as
 * soon as SendMessage has answered.
 *
 * A received message stays in mmsd's store as the raw PDU; its properties
 * name that file and the offset and length of every part. The parts are
 * copied out into files the Messaging app can show.
 *
 * Everything here only needs GLib and POSIX, so it can be tested on its own.
 */

#ifndef MMS_CORE_H_
#define MMS_CORE_H_

#include <stdbool.h>
#include <sys/types.h>
#include <glib.h>

/* At most this many files go with one message (the app stages up to 10). */
#define MMS_MAX_FILES 10
/* The size an outgoing message is fitted into, in bytes: its text, its files
 * and what mmsd puts around them. A choice, not a carrier's number: 300 KB is
 * the message size class of OMA's MMS conformance document that phones and
 * carriers take most widely. Pictures are made smaller to fit
 * (src/mmsimage.h); other files have to fit as they are. */
#define MMS_DEFAULT_MESSAGE_SIZE ((gsize) 300 * 1024)
/* Kept from that for the SMIL body and the headers mmsd adds to the message
 * and to each part (an allowance, not a measurement). */
#define MMS_ENVELOPE_RESERVE ((gsize) 4 * 1024)
/* mmsd's own limit for one number (src/service.c valid_number_format). */
#define MMS_MAX_NUMBER_DIGITS 20

/* Error codes as the Messaging app knows them (messagingUtils errorCodes). */
#define MMS_ERR_UNSPECIFIED "mmsErrorUnspecified"
#define MMS_ERR_NETWORK "mmsErrorNetwork"
#define MMS_ERR_BAD_ADDRESS "mmsErrorSendingAddressUnresolved"
#define MMS_ERR_TOO_LARGE "mmsErrorAttachmentTooLarge"
#define MMS_ERR_REJECTED "mmsErrorRejected"
#define MMS_ERR_DENIED "mmsErrorServiceDenied"
#define MMS_ERR_CORRUPT "mmsErrorContentCorrupt"
#define MMS_ERR_UNSUPPORTED "mmsErrorUnsupportedMessage"
#define MMS_ERR_NOT_READY "mmsErrorServiceNotInitialized"
#define MMS_ERR_DB "mmsErrorDbError"

/* One file of an outgoing message, as the app named it. */
struct mms_file {
	gchar *path;
	gchar *content_type;	/* NULL: guessed from the name */
};

/* One outgoing message as read from its com.palm.mmsmessage:1 record. */
struct mms_outgoing {
	gchar *id;		/* db8 _id */
	GPtrArray *recipients;	/* gchar*, normalized numbers */
	gchar *text;		/* NULL or "" when the message has no text */
	gchar *subject;		/* not sent yet; kept for later */
	GPtrArray *files;	/* struct mms_file* */
	int sim_id;		/* -1: the default SMS SIM */
};

struct mms_outgoing* mms_outgoing_new(const char *id);
void mms_outgoing_free(struct mms_outgoing *msg);
/* Takes a copy; content_type may be NULL. */
void mms_outgoing_add_file(struct mms_outgoing *msg, const char *path, const char *content_type);

/**
 * A phone number as mmsd accepts it: an optional leading '+' and 1 to 20
 * digits. Blanks, '-', '.', '(' and ')' are dropped. Anything else (an email
 * address, letters, a second '+') gives NULL.
 */
gchar* mms_normalize_number(const char *addr);

/**
 * The content type for a file name, by its extension, lower or upper case.
 * Unknown extensions give "application/octet-stream". Never NULL.
 */
const char* mms_content_type_for_name(const char *name);

/**
 * A file extension (without the dot) for a content type, parameters
 * ("; charset=...") ignored; "bin" when unknown. Never NULL.
 */
const char* mms_extension_for_content_type(const char *content_type);

/**
 * The bare type of a Content-Type value, lower case, without parameters and
 * blanks: "Image/JPEG; name=x" gives "image/jpeg". NULL in, NULL out.
 */
gchar* mms_content_type_bare(const char *content_type);

/* True when a content type is one mmsd accepts: "type/subtype" in printable
 * ASCII without blanks or separators beyond the one '/'. */
bool mms_content_type_valid(const char *content_type);

/**
 * Whether the app may hand this file to a message: an absolute path below one
 * of the allowed roots, without "." or ".." components or empty components.
 * The roots end in '/'. This checks the name only; mms_open_file checks the
 * file that is really opened.
 */
bool mms_path_allowed(const char *path, const char * const *roots);

/* The roots the daemon allows: the user's media partition. */
extern const char * const mms_default_roots[];

/**
 * Open a file for sending: resolves the path, checks that the result is
 * still below one of the roots, opens it without following a final symlink,
 * and checks the opened file (a regular file, 1 byte to max_size bytes, and
 * really the one below the root, by /proc/self/fd). Returns the fd and its
 * size, or -1 with *error_code set to one of the MMS_ERR_* codes.
 */
int mms_open_file(const char *path, const char * const *roots, gsize max_size,
                  gsize *size, const char **error_code);

/**
 * Copy at most max bytes from fd (from its current offset) into a new file
 * dest, created exclusively with the given mode and, when gid is not -1,
 * that group. Returns FALSE and removes dest on any error.
 */
gboolean mms_copy_fd_to_file(int fd, const char *dest, gsize max, mode_t mode, gid_t gid, GError **error);

/**
 * How the n files of a message share budget bytes. sizes[i] is the size of
 * file i, fittable[i] whether it is a picture that can be made smaller. The
 * other files take what they are; the pictures share the rest equally, and a
 * picture smaller than its share leaves what it does not need to the larger
 * ones. On TRUE targets[i] is what file i may take: not below sizes[i] means
 * it goes as it is, below means it has to be made to fit. FALSE when the
 * other files alone do not fit, or a picture would get nothing.
 */
gboolean mms_plan_fit(const gsize *sizes, const gboolean *fittable, guint n, gsize budget, gsize *targets);

/* Write data into a new file, as mms_copy_fd_to_file. */
gboolean mms_write_new_file(const char *dest, const void *data, gsize len, mode_t mode, gid_t gid,
                            GError **error);

/* One part as it is handed to mmsd's SendMessage. */
struct mms_send_part {
	gchar *content_id;	/* "<name>" */
	gchar *content_type;
	gchar *file;		/* spooled copy */
	gchar *smil_src;	/* "cid:name" */
};

void mms_send_part_free(struct mms_send_part *part);

/**
 * A SMIL body that shows the parts one after another: a picture or video in
 * the upper region, a text below it. One <par> per media part; the text goes
 * with the first one, or alone when there is no media part.
 */
gchar* mms_build_smil(GPtrArray *parts /* struct mms_send_part* */);

/**
 * The db8 status for an mmsd message status: "sent" is "successful",
 * "received" and "read" are "successful" too; "draft" is NULL - mmsd uses it
 * both for a message still queued and for one it gave up on.
 */
const char* mms_db_status_for(const char *mmsd_status);

/* An MMS_ERR_* code for an mmsd D-Bus error name, e.g.
 * "org.ofono.mms.Error.PermanentInvalidAddress". Never NULL. */
const char* mms_error_code_for_dbus_error(const char *error_name);

/* The last path component of an mmsd object path ("/org/ofono/mms/<id>/<uuid>"
 * gives "<uuid>"); NULL for anything without one. */
gchar* mms_path_basename(const char *object_path);

/**
 * A file name for a received part: the content id or location without the
 * angle brackets, reduced to [A-Za-z0-9._-], at most 64 characters, not
 * starting with '.', and with an extension that fits the content type. Falls
 * back to "part<index>.<ext>". The index keeps names of one message unique:
 * it is put in front when the name came from the sender.
 */
gchar* mms_received_file_name(const char *content_id, guint index, const char *content_type);

/**
 * Read len bytes at offset from file into a new GBytes, checking that the
 * range lies inside the file. NULL with error set otherwise.
 */
GBytes* mms_read_range(const char *file, guint64 offset, guint64 len, GError **error);

/**
 * The text of a text/plain part as UTF-8: converted from the charset the
 * content type names (UTF-8 when none), invalid sequences replaced. Never
 * NULL.
 */
gchar* mms_part_text(const void *data, gsize len, const char *content_type);

/* A value of one parameter of a content type ("charset"), or NULL. */
gchar* mms_content_type_param(const char *content_type, const char *name);

#endif

// vim:ts=4:sw=4:noexpandtab
