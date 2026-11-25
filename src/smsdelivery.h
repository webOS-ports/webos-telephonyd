/* @@@LICENSE
*
* Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
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

#ifndef SMSDELIVERY_H_
#define SMSDELIVERY_H_

#include <stdbool.h>
#include <glib.h>

/*
 * Keeps track of the SMS sent with a status report requested until the
 * reports are in, and says when a message's outcome is known.
 *
 * A message goes out once per recipient and each send has a reference of
 * its own (with ofono, the path the message had while it was sent). The
 * message counts as delivered when every recipient's report says so, and
 * as undelivered as soon as one says it was not. Its references are added
 * as the sends complete, and the message is sealed after the last one, so
 * that an early report for the first recipient isn't taken for all of
 * them.
 *
 * Reports may never come (the network or the other phone may not send
 * them), so only the newest SMS_DELIVERY_MAX_REFS references are kept.
 */

#define SMS_DELIVERY_MAX_REFS 256

enum sms_delivery_result {
	/* not one of ours, or the message's outcome is known already */
	SMS_DELIVERY_UNKNOWN = 0,
	/* other recipients' reports are still to come */
	SMS_DELIVERY_PENDING,
	SMS_DELIVERY_DELIVERED,
	SMS_DELIVERY_UNDELIVERED,
};

struct sms_delivery;

struct sms_delivery *sms_delivery_new(void);
void sms_delivery_free(struct sms_delivery *sd);

/* One recipient's send of message msg_id went out with reference ref */
void sms_delivery_expect(struct sms_delivery *sd, const char *msg_id, const char *ref);

/*
 * Every recipient of msg_id has been sent to. Returns what is known by
 * then: DELIVERED or UNDELIVERED when all the reports came in before the
 * last send finished, else PENDING, or UNKNOWN when nothing is expected.
 */
enum sms_delivery_result sms_delivery_seal(struct sms_delivery *sd, const char *msg_id);

/* Stop waiting for msg_id's reports, e.g. when sending it failed */
void sms_delivery_forget(struct sms_delivery *sd, const char *msg_id);

/*
 * A report for ref came in. When the message's outcome is known now, the
 * result says which and *msg_id gets its id (free it with g_free());
 * otherwise *msg_id is NULL.
 */
enum sms_delivery_result sms_delivery_report(struct sms_delivery *sd, const char *ref,
											 bool delivered, gchar **msg_id);

#endif
