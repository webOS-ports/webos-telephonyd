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
 * com.palm.mmsmessage:1 records, in and out.
 *
 * The record keeps the shape the webOS Messaging apps have always read an MMS
 * in (messagingUtils.getMessageText): serviceName "mms" and an "attachments"
 * array of parts, a text part as { mimeType: "text/plain", partText } and a
 * file as { mimeType, path, name }. LuneOS adds messageText (all text parts)
 * and filePath (the first file), which the conversation view already draws
 * for every other message.
 *
 * Outgoing, written by the Messaging app:
 *   { _id, folder: "outbox", status: "pending", serviceName: "mms",
 *     to: [{ addr }] (or one { addr }), messageText?, subject?, simId?,
 *     attachments?: [{ path, mimeType?, name? }], filePath? }
 * Without attachments, filePath alone names the one file.
 */

#ifndef MMS_JSON_H_
#define MMS_JSON_H_

#include <glib.h>
#include <pbnjson.h>

#include "mmscore.h"

#define MMS_DB_KIND "com.palm.mmsmessage:1"

/**
 * The message an outbox record describes. NULL when it cannot be sent; then
 * *error_code says why (an MMS_ERR_* code) and *id is the record's _id, or
 * NULL when it has none (nothing can be marked failed then). On success *id
 * is NULL; the message carries its id. The caller frees *id.
 */
struct mms_outgoing* mms_outgoing_from_record(jvalue_ref record, gchar **id, const char **error_code);

/* One part of a received message, ready to be stored. */
struct mms_received_part {
	gchar *content_type;	/* bare */
	gchar *text;		/* text/plain parts */
	gchar *path;		/* other parts: the extracted file */
	gchar *name;
};

void mms_received_part_free(struct mms_received_part *part);

struct mms_received {
	gchar *remote_id;	/* mmsd's uuid */
	gchar *sender;
	GPtrArray *recipients;	/* gchar* */
	gchar *subject;
	gint64 timestamp_ms;	/* 0: unknown */
	gint64 local_timestamp_ms;
	int sim_id;		/* -1: unknown */
	GPtrArray *parts;	/* struct mms_received_part* */
};

struct mms_received* mms_received_new(const char *remote_id);
void mms_received_free(struct mms_received *msg);

/* The inbox record for a received message. */
jvalue_ref mms_received_record(const struct mms_received *msg);

/**
 * A db8 merge request that sets status (and errorCode, when given) on one
 * record, by _id when id is set, else on every MMS record whose
 * remoteMessageId is remote_id. remote_id is also stored with an id.
 */
jvalue_ref mms_status_merge_request(const char *id, const char *remote_id, const char *status,
                                    const char *error_code);

#endif

// vim:ts=4:sw=4:noexpandtab
