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
 * MMS through mmsd.
 *
 * Sending: the Messaging app puts a com.palm.mmsmessage:1 record into the
 * outbox (status "pending"); the activity "telephony-send-outgoing-mms"
 * watches for those and calls sendMmsFromDb, which marks each "sending",
 * copies its files into the spool and hands it to mmsd's SendMessage, one
 * message at a time. mmsd's answer gives the message a remoteMessageId; its
 * Status "sent" makes the record "successful", a refusal, mmsd giving up
 * ("draft" again) or no word within the send time-out make it "failed" with
 * an errorCode the app knows.
 *
 * Receiving: every message mmsd announces with Status "received" (and every
 * one it still holds when it or this daemon starts) has its parts copied out
 * of mmsd's store into the inbox directory and is put into db8 as an inbox
 * com.palm.mmsmessage:1 record; once that is stored, mmsd's copy is deleted.
 *
 * mmsStatusQuery {subscribe?} answers { available }: whether mmsd offers a
 * service, i.e. a SIM with an MMS context. The app offers MMS only then.
 */

#ifndef TELEPHONY_SERVICE_MMS_H_
#define TELEPHONY_SERVICE_MMS_H_

#include <sys/types.h>
#include <gio/gio.h>
#include <luna-service2/lunaservice.h>

struct telephony_service;

struct telephony_mms_config {
	GBusType bus_type;
	/* Each message's files are copied below this, in a directory only
	 * root and group mmsd can read, and removed once mmsd answered. */
	const char *spool_dir;
	/* Received parts are extracted to <inbox_dir>/<message>/. */
	const char *inbox_dir;
	/* The group mmsd runs as; (gid_t) -1: look up the group "mmsd". */
	gid_t spool_gid;
	/* Where the files of an outgoing message may come from. */
	const char * const *roots;
	/* No "sent" from mmsd within this many seconds: the record is marked
	 * failed (and still becomes successful should mmsd send it later). */
	guint send_timeout_s;
	/* An outgoing message is fitted into this many bytes, pictures made
	 * smaller to fit; 0: MMS_DEFAULT_MESSAGE_SIZE (300 KB). */
	gsize max_message_size;
};

/* What the daemon uses: the system bus, /var/lib/webos-telephonyd/mms-spool,
 * /media/internal/.mms-attachments, group mmsd, files below /media/internal,
 * 10 minutes, 300 KB. */
extern const struct telephony_mms_config telephony_mms_default_config;

void telephony_service_mms_setup(struct telephony_service *service, const struct telephony_mms_config *config);
void telephony_service_mms_shutdown(struct telephony_service *service);

bool _service_internal_send_mms_from_db_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_mms_status_query_cb(LSHandle *handle, LSMessage *message, void *user_data);

#endif

// vim:ts=4:sw=4:noexpandtab
