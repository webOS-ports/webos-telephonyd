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

/* Reports come with the caller's own copy of the reference */
static enum sms_delivery_result report(struct sms_delivery *sd, const char *ref, bool delivered,
									   gchar **id)
{
	gchar *copy = g_strdup(ref);
	enum sms_delivery_result r = sms_delivery_report(sd, copy, delivered, id);

	g_free(copy);
	return r;
}

static void check(struct sms_delivery *sd, const char *ref, bool delivered,
				  enum sms_delivery_result expected, const char *expected_id)
{
	gchar *id = (gchar *) "untouched";

	g_assert_cmpint(report(sd, ref, delivered, &id), ==, expected);
	g_assert_cmpstr(id, ==, expected_id);
	g_free(id);
}

static void test_delivered(void)
{
	struct sms_delivery *sd = sms_delivery_new();

	sms_delivery_expect(sd, "msg1", "/ril_0/message_1");
	g_assert_cmpint(sms_delivery_seal(sd, "msg1"), ==, SMS_DELIVERY_PENDING);
	check(sd, "/ril_0/message_1", true, SMS_DELIVERY_DELIVERED, "msg1");
	/* the same report again is not news */
	check(sd, "/ril_0/message_1", true, SMS_DELIVERY_UNKNOWN, NULL);
	sms_delivery_free(sd);
}

static void test_undelivered(void)
{
	struct sms_delivery *sd = sms_delivery_new();

	sms_delivery_expect(sd, "msg1", "/ril_0/message_1");
	g_assert_cmpint(sms_delivery_seal(sd, "msg1"), ==, SMS_DELIVERY_PENDING);
	check(sd, "/ril_0/message_1", false, SMS_DELIVERY_UNDELIVERED, "msg1");
	sms_delivery_free(sd);
}

/* The report beat the end of the send: the seal gives the outcome */
static void test_report_before_seal(void)
{
	struct sms_delivery *sd = sms_delivery_new();

	sms_delivery_expect(sd, "msg1", "/ril_0/message_1");
	check(sd, "/ril_0/message_1", true, SMS_DELIVERY_PENDING, NULL);
	g_assert_cmpint(sms_delivery_seal(sd, "msg1"), ==, SMS_DELIVERY_DELIVERED);
	g_assert_cmpint(sms_delivery_seal(sd, "msg1"), ==, SMS_DELIVERY_UNKNOWN);
	sms_delivery_free(sd);
}

/* The first recipient's report must not count for the second, not yet sent */
static void test_two_recipients(void)
{
	struct sms_delivery *sd = sms_delivery_new();

	sms_delivery_expect(sd, "msg1", "/ril_0/message_1");
	check(sd, "/ril_0/message_1", true, SMS_DELIVERY_PENDING, NULL);
	sms_delivery_expect(sd, "msg1", "/ril_0/message_2");
	g_assert_cmpint(sms_delivery_seal(sd, "msg1"), ==, SMS_DELIVERY_PENDING);
	check(sd, "/ril_0/message_2", true, SMS_DELIVERY_DELIVERED, "msg1");
	sms_delivery_free(sd);
}

/* One recipient not reached is enough to say so, at once */
static void test_one_of_two_undelivered(void)
{
	struct sms_delivery *sd = sms_delivery_new();

	sms_delivery_expect(sd, "msg1", "/ril_0/message_1");
	sms_delivery_expect(sd, "msg1", "/ril_0/message_2");
	g_assert_cmpint(sms_delivery_seal(sd, "msg1"), ==, SMS_DELIVERY_PENDING);
	check(sd, "/ril_0/message_1", false, SMS_DELIVERY_UNDELIVERED, "msg1");
	check(sd, "/ril_0/message_2", true, SMS_DELIVERY_UNKNOWN, NULL);
	sms_delivery_free(sd);
}

static void test_undelivered_before_seal(void)
{
	struct sms_delivery *sd = sms_delivery_new();

	sms_delivery_expect(sd, "msg1", "/ril_0/message_1");
	check(sd, "/ril_0/message_1", false, SMS_DELIVERY_UNDELIVERED, "msg1");
	g_assert_cmpint(sms_delivery_seal(sd, "msg1"), ==, SMS_DELIVERY_UNKNOWN);
	sms_delivery_free(sd);
}

/* Messages are kept apart, on either modem */
static void test_separate_messages(void)
{
	struct sms_delivery *sd = sms_delivery_new();

	sms_delivery_expect(sd, "msg1", "/ril_0/message_1");
	sms_delivery_expect(sd, "msg2", "/ril_1/message_1");
	g_assert_cmpint(sms_delivery_seal(sd, "msg1"), ==, SMS_DELIVERY_PENDING);
	g_assert_cmpint(sms_delivery_seal(sd, "msg2"), ==, SMS_DELIVERY_PENDING);
	check(sd, "/ril_1/message_1", false, SMS_DELIVERY_UNDELIVERED, "msg2");
	check(sd, "/ril_0/message_1", true, SMS_DELIVERY_DELIVERED, "msg1");
	sms_delivery_free(sd);
}

static void test_unknown(void)
{
	struct sms_delivery *sd = sms_delivery_new();

	check(sd, "/ril_0/message_9", true, SMS_DELIVERY_UNKNOWN, NULL);
	g_assert_cmpint(sms_delivery_seal(sd, "nothing"), ==, SMS_DELIVERY_UNKNOWN);
	/* a message that wasn't sent with a report expects nothing */
	g_assert_cmpint(sms_delivery_seal(sd, "msg1"), ==, SMS_DELIVERY_UNKNOWN);

	g_assert_cmpint(sms_delivery_report(NULL, "x", true, NULL), ==, SMS_DELIVERY_UNKNOWN);
	g_assert_cmpint(sms_delivery_report(sd, NULL, true, NULL), ==, SMS_DELIVERY_UNKNOWN);
	sms_delivery_expect(sd, NULL, "x");
	sms_delivery_expect(sd, "x", NULL);
	sms_delivery_forget(sd, NULL);
	g_assert_cmpint(sms_delivery_seal(sd, NULL), ==, SMS_DELIVERY_UNKNOWN);
	sms_delivery_free(NULL);
	sms_delivery_free(sd);
}

/* Sending to the second recipient failed: nothing more is expected */
static void test_forget(void)
{
	struct sms_delivery *sd = sms_delivery_new();

	sms_delivery_expect(sd, "msg1", "/ril_0/message_1");
	sms_delivery_expect(sd, "msg2", "/ril_0/message_2");
	sms_delivery_forget(sd, "msg1");
	check(sd, "/ril_0/message_1", true, SMS_DELIVERY_UNKNOWN, NULL);
	g_assert_cmpint(sms_delivery_seal(sd, "msg1"), ==, SMS_DELIVERY_UNKNOWN);
	/* the other message is untouched */
	g_assert_cmpint(sms_delivery_seal(sd, "msg2"), ==, SMS_DELIVERY_PENDING);
	check(sd, "/ril_0/message_2", true, SMS_DELIVERY_DELIVERED, "msg2");
	sms_delivery_free(sd);
}

/* The same reference twice is one send */
static void test_duplicate_expect(void)
{
	struct sms_delivery *sd = sms_delivery_new();

	sms_delivery_expect(sd, "msg1", "/ril_0/message_1");
	sms_delivery_expect(sd, "msg1", "/ril_0/message_1");
	g_assert_cmpint(sms_delivery_seal(sd, "msg1"), ==, SMS_DELIVERY_PENDING);
	check(sd, "/ril_0/message_1", true, SMS_DELIVERY_DELIVERED, "msg1");
	sms_delivery_free(sd);
}

/* Reports that never come don't pile up: the oldest are given up */
static void test_limit(void)
{
	struct sms_delivery *sd = sms_delivery_new();
	gchar id[32], ref[32];
	guint n;

	/* one answered early, so its reference is gone from the middle */
	sms_delivery_expect(sd, "early", "/ril_0/message_early");
	g_assert_cmpint(sms_delivery_seal(sd, "early"), ==, SMS_DELIVERY_PENDING);
	check(sd, "/ril_0/message_early", true, SMS_DELIVERY_DELIVERED, "early");

	for (n = 0; n < SMS_DELIVERY_MAX_REFS + 2; n++) {
		g_snprintf(id, sizeof(id), "msg%u", n);
		g_snprintf(ref, sizeof(ref), "/ril_0/message_%u", n);
		sms_delivery_expect(sd, id, ref);
		g_assert_cmpint(sms_delivery_seal(sd, id), ==, SMS_DELIVERY_PENDING);
	}

	/* the two oldest went */
	check(sd, "/ril_0/message_0", true, SMS_DELIVERY_UNKNOWN, NULL);
	check(sd, "/ril_0/message_1", true, SMS_DELIVERY_UNKNOWN, NULL);
	/* the rest are all still there */
	for (n = 2; n < SMS_DELIVERY_MAX_REFS + 2; n++) {
		g_snprintf(id, sizeof(id), "msg%u", n);
		g_snprintf(ref, sizeof(ref), "/ril_0/message_%u", n);
		check(sd, ref, true, SMS_DELIVERY_DELIVERED, id);
	}
	sms_delivery_free(sd);
}

/* A message whose first reference was given up can't be complete */
static void test_limit_partial(void)
{
	struct sms_delivery *sd = sms_delivery_new();
	gchar id[32], ref[32];
	guint n;

	sms_delivery_expect(sd, "multi", "/ril_0/message_a");
	for (n = 0; n < SMS_DELIVERY_MAX_REFS; n++) {
		g_snprintf(id, sizeof(id), "msg%u", n);
		g_snprintf(ref, sizeof(ref), "/ril_0/message_%u", n);
		sms_delivery_expect(sd, id, ref);
	}
	/* "/ril_0/message_a" is given up now */
	sms_delivery_expect(sd, "multi", "/ril_0/message_b");
	g_assert_cmpint(sms_delivery_seal(sd, "multi"), ==, SMS_DELIVERY_UNKNOWN);
	check(sd, "/ril_0/message_b", true, SMS_DELIVERY_UNKNOWN, NULL);
	sms_delivery_free(sd);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/smsdelivery/delivered", test_delivered);
	g_test_add_func("/smsdelivery/undelivered", test_undelivered);
	g_test_add_func("/smsdelivery/report-before-seal", test_report_before_seal);
	g_test_add_func("/smsdelivery/two-recipients", test_two_recipients);
	g_test_add_func("/smsdelivery/one-of-two-undelivered", test_one_of_two_undelivered);
	g_test_add_func("/smsdelivery/undelivered-before-seal", test_undelivered_before_seal);
	g_test_add_func("/smsdelivery/separate-messages", test_separate_messages);
	g_test_add_func("/smsdelivery/unknown", test_unknown);
	g_test_add_func("/smsdelivery/forget", test_forget);
	g_test_add_func("/smsdelivery/duplicate-expect", test_duplicate_expect);
	g_test_add_func("/smsdelivery/limit", test_limit);
	g_test_add_func("/smsdelivery/limit-partial", test_limit_partial);

	return g_test_run();
}
