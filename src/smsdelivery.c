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

#include <glib.h>

#include "smsdelivery.h"

struct message {
	gchar *id;
	/* references still without a report */
	guint waiting;
	bool sealed;
	bool undelivered;
	/* the outcome was handed out; later reports are ignored */
	bool done;
};

struct sms_delivery {
	/* reference -> struct message (shared by its references) */
	GHashTable *refs;
	/* message id -> struct message, owns it */
	GHashTable *messages;
	/* references, oldest first, to drop the oldest beyond the limit */
	GQueue *order;
};

static void message_free(gpointer data)
{
	struct message *m = data;

	g_free(m->id);
	g_free(m);
}

struct sms_delivery *sms_delivery_new(void)
{
	struct sms_delivery *sd = g_new0(struct sms_delivery, 1);

	sd->refs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	sd->messages = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, message_free);
	sd->order = g_queue_new();

	return sd;
}

void sms_delivery_free(struct sms_delivery *sd)
{
	if (!sd)
		return;

	/* the queue's strings are the keys of refs */
	g_queue_free(sd->order);
	g_hash_table_destroy(sd->refs);
	g_hash_table_destroy(sd->messages);
	g_free(sd);
}

/*
 * Drops a message once all its recipients have been sent to and no report
 * is outstanding. Until it is sealed more sends may join it, also when its
 * outcome is known already or it was given up on.
 */
static void release(struct sms_delivery *sd, struct message *m)
{
	if (m->waiting == 0 && m->sealed)
		g_hash_table_remove(sd->messages, m->id);
}

static void drop_ref(struct sms_delivery *sd, const char *ref)
{
	struct message *m = g_hash_table_lookup(sd->refs, ref);

	g_queue_remove(sd->order, ref);
	/* frees ref, which the queue shared */
	g_hash_table_remove(sd->refs, ref);

	if (m) {
		m->waiting--;
		/* given up on: it can't be complete any more */
		m->done = true;
		release(sd, m);
	}
}

void sms_delivery_expect(struct sms_delivery *sd, const char *msg_id, const char *ref)
{
	struct message *m;
	gchar *key;

	if (!sd || !msg_id || !ref || g_hash_table_contains(sd->refs, ref))
		return;

	m = g_hash_table_lookup(sd->messages, msg_id);
	if (!m) {
		m = g_new0(struct message, 1);
		m->id = g_strdup(msg_id);
		g_hash_table_insert(sd->messages, m->id, m);
	}

	key = g_strdup(ref);
	g_hash_table_insert(sd->refs, key, m);
	g_queue_push_tail(sd->order, key);
	m->waiting++;

	while (g_queue_get_length(sd->order) > SMS_DELIVERY_MAX_REFS)
		drop_ref(sd, g_queue_peek_head(sd->order));
}

static enum sms_delivery_result outcome(struct message *m)
{
	if (m->done)
		return SMS_DELIVERY_UNKNOWN;

	if (m->undelivered) {
		m->done = true;
		return SMS_DELIVERY_UNDELIVERED;
	}

	if (m->sealed && m->waiting == 0) {
		m->done = true;
		return SMS_DELIVERY_DELIVERED;
	}

	return SMS_DELIVERY_PENDING;
}

enum sms_delivery_result sms_delivery_seal(struct sms_delivery *sd, const char *msg_id)
{
	struct message *m;
	enum sms_delivery_result result;

	if (!sd || !msg_id)
		return SMS_DELIVERY_UNKNOWN;

	m = g_hash_table_lookup(sd->messages, msg_id);
	if (!m)
		return SMS_DELIVERY_UNKNOWN;

	m->sealed = true;
	result = outcome(m);
	release(sd, m);

	return result;
}

void sms_delivery_forget(struct sms_delivery *sd, const char *msg_id)
{
	const struct message *m;
	GList *l, *next;

	if (!sd || !msg_id)
		return;

	m = g_hash_table_lookup(sd->messages, msg_id);
	if (!m)
		return;

	for (l = sd->order->head; l; l = next) {
		next = l->next;
		if (g_hash_table_lookup(sd->refs, l->data) == m) {
			g_hash_table_remove(sd->refs, l->data);
			g_queue_delete_link(sd->order, l);
		}
	}

	g_hash_table_remove(sd->messages, msg_id);
}

enum sms_delivery_result sms_delivery_report(struct sms_delivery *sd, const char *ref,
											 bool delivered, gchar **msg_id)
{
	gpointer key, value;
	struct message *m;
	enum sms_delivery_result result;

	if (msg_id)
		*msg_id = NULL;

	if (!sd || !ref || !g_hash_table_lookup_extended(sd->refs, ref, &key, &value))
		return SMS_DELIVERY_UNKNOWN;

	m = value;
	/* the queue holds our copy of the reference, not the caller's */
	g_queue_remove(sd->order, key);
	g_hash_table_remove(sd->refs, key);
	m->waiting--;

	if (!delivered)
		m->undelivered = true;

	result = outcome(m);
	if ((result == SMS_DELIVERY_DELIVERED || result == SMS_DELIVERY_UNDELIVERED) && msg_id)
		*msg_id = g_strdup(m->id);

	release(sd, m);

	return result;
}
