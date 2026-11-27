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

/*
 * What clients that subscribed to networkStatusQuery without a simId hear:
 * the real src/telephonyservice.c and src/telephonyservice_net.c, with the
 * luna bus replaced by recording stubs and a fake driver for two SIMs.
 *
 * Such a client, the status bar's, follows the voice SIM. When telephonyd
 * starts, each SIM reports its registration and IMS before the default
 * voice SIM is known; the client has to hear the new default's state when
 * it becomes the default, not only at its next change.
 */

#include <stdio.h>
#include <string.h>

#include <glib.h>
#include <pbnjson.h>
#include <luna-service2/lunaservice.h>

#include "telephonyservice.h"
#include "telephonyservice_internal.h"
#include "telephonydriver.h"
#include "telephonysettings.h"

GMainLoop *event_loop;

/* ------------------------------------------------------------------------
 * luna-service2 stubs
 * ------------------------------------------------------------------------ */

struct LSHandle {
	gchar *name;
};

struct LSMessage {
	const char *payload;
	bool subscribe;
};

/* subscription posts on com.palm.telephony: key -> payloads, in order */
static GHashTable *posts;
/* keys clients were subscribed under, in order */
static GPtrArray *subscribed;
/* what a call was answered */
static char *last_reply;

bool _service_network_status_query_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_rat_query_cb(LSHandle *handle, LSMessage *message, void *user_data);

bool LSErrorInit(LSError *error)
{
	memset(error, 0, sizeof(*error));
	return true;
}

void LSErrorFree(LSError *error)
{
}

void LSErrorPrint(LSError *lserror, FILE *out)
{
}

bool LSRegister(const char *name, LSHandle **sh, LSError *lserror)
{
	*sh = g_new0(LSHandle, 1);
	(*sh)->name = g_strdup(name);
	return true;
}

bool LSUnregister(LSHandle *sh, LSError *lserror)
{
	g_free(sh->name);
	g_free(sh);
	return true;
}

bool LSGmainAttach(LSHandle *sh, GMainLoop *loop, LSError *lserror)
{
	return true;
}

bool LSRegisterCategory(LSHandle *sh, const char *category, LSMethod *methods, LSSignal *signals,
                        LSProperty *properties, LSError *lserror)
{
	return true;
}

bool LSCategorySetData(LSHandle *sh, const char *category, void *user_data, LSError *lserror)
{
	return true;
}

const char* LSMessageGetPayload(LSMessage *message)
{
	return message->payload;
}

bool LSMessageIsSubscription(LSMessage *message)
{
	return message->subscribe;
}

void LSMessageRef(LSMessage *message)
{
}

void LSMessageUnref(LSMessage *message)
{
}

bool LSMessageReply(LSHandle *sh, LSMessage *lsmsg, const char *payload, LSError *lserror)
{
	g_free(last_reply);
	last_reply = g_strdup(payload);
	return true;
}

bool LSSubscriptionAdd(LSHandle *sh, const char *key, LSMessage *message, LSError *lserror)
{
	g_ptr_array_add(subscribed, g_strdup(key));
	return true;
}

bool LSSubscriptionProcess(LSHandle *sh, LSMessage *message, bool *subscribed, LSError *lserror)
{
	*subscribed = false;
	return true;
}

bool LSSubscriptionReply(LSHandle *sh, const char *key, const char *payload, LSError *lserror)
{
	GPtrArray *list;

	/* both bus names get the same; keep one */
	if (!g_str_equal(sh->name, "com.palm.telephony"))
		return true;

	list = g_hash_table_lookup(posts, key);
	if (!list) {
		list = g_ptr_array_new_with_free_func(g_free);
		g_hash_table_insert(posts, g_strdup(key), list);
	}
	g_ptr_array_add(list, g_strdup(payload));
	return true;
}

bool LSSubscriptionPost(LSHandle *sh, const char *category, const char *method, const char *payload,
                        LSError *lserror)
{
	return LSSubscriptionReply(sh, method, payload, lserror);
}

bool LSCallOneReply(LSHandle *sh, const char *uri, const char *payload, LSFilterFunc callback, void *ctx,
                    LSMessageToken *ret_token, LSError *lserror)
{
	return true;
}

/* ------------------------------------------------------------------------
 * What telephonyservice.c sets up beside the network methods
 * ------------------------------------------------------------------------ */

#define WEAK_HANDLER(name) \
	__attribute__((weak)) bool name(LSHandle *handle, LSMessage *message, void *user_data) \
	{ g_assert_not_reached(); return false; }

/* handlers of the method table that live in the files not built here */
WEAK_HANDLER(_service_airplane_mode_query_cb)
WEAK_HANDLER(_service_airplane_mode_set_cb)
WEAK_HANDLER(_service_answer_cb)
WEAK_HANDLER(_service_charge_source_query_cb)
WEAK_HANDLER(_service_default_sim_query_cb)
WEAK_HANDLER(_service_default_sim_set_cb)
WEAK_HANDLER(_service_device_lock_query_cb)
WEAK_HANDLER(_service_dial_cb)
WEAK_HANDLER(_service_fdn_status_query_cb)
WEAK_HANDLER(_service_hangup_cb)
WEAK_HANDLER(_service_ignore_cb)
WEAK_HANDLER(_service_internal_send_mms_from_db_cb)
WEAK_HANDLER(_service_internal_send_sms_from_db_cb)
WEAK_HANDLER(_service_is_telephony_ready_cb)
WEAK_HANDLER(_service_mms_status_query_cb)
WEAK_HANDLER(_service_pin1_change_cb)
WEAK_HANDLER(_service_pin1_disable_cb)
WEAK_HANDLER(_service_pin1_enable_cb)
WEAK_HANDLER(_service_pin1_status_query_cb)
WEAK_HANDLER(_service_pin1_unblock_cb)
WEAK_HANDLER(_service_pin1_verify_cb)
WEAK_HANDLER(_service_pin2_status_query_cb)
WEAK_HANDLER(_service_platform_query_cb)
WEAK_HANDLER(_service_power_query_cb)
WEAK_HANDLER(_service_power_set_cb)
WEAK_HANDLER(_service_sim_list_query_cb)
WEAK_HANDLER(_service_sim_name_set_cb)
WEAK_HANDLER(_service_sim_status_query_cb)
WEAK_HANDLER(_service_stk_agent_request_cb)
WEAK_HANDLER(_service_stk_get_main_menu_cb)
WEAK_HANDLER(_service_stk_respond_cb)
WEAK_HANDLER(_service_stk_select_item_cb)
WEAK_HANDLER(_service_subscribe_cb)
WEAK_HANDLER(_service_subscriber_id_query_cb)

void telephony_service_stk_setup(struct telephony_service *service)
{
}

void telephony_service_stk_shutdown(struct telephony_service *service)
{
}

void telephonyservice_sms_setup(struct telephony_service *service)
{
}

void telephony_service_mms_setup(struct telephony_service *service, const char *mmsd_name)
{
}

void telephony_service_mms_shutdown(struct telephony_service *service)
{
}

int telephonyservice_common_finish(const struct telephony_error *error, void *data)
{
	return 0;
}

char* telephony_settings_load(enum telephony_settings_type type)
{
	return NULL;
}

bool telephony_settings_store(enum telephony_settings_type type, const char *data)
{
	return true;
}

static int data_sim = -1;

void wan_service_set_data_sim(int sim_id, const char *imsi)
{
	data_sim = sim_id;
}

/* ------------------------------------------------------------------------
 * Fake driver: two SIMs, each with the status ofono would give
 * ------------------------------------------------------------------------ */

static struct telephony_network_status sim_status[2];
static bool sim_fails[2];
static int queries[2];

static int driver_probe(struct telephony_service *service)
{
	return 0;
}

static void driver_remove(struct telephony_service *service)
{
}

static void driver_network_status_query(struct telephony_service *service, int sim_id,
                                        telephony_network_status_query_cb cb, void *data)
{
	g_assert_cmpint(sim_id, >=, 0);
	g_assert_cmpint(sim_id, <, 2);
	queries[sim_id]++;

	if (sim_fails[sim_id]) {
		struct telephony_error error = { .code = TELEPHONY_ERROR_NOT_AVAILABLE };
		cb(&error, NULL, data);
		return;
	}
	cb(NULL, &sim_status[sim_id], data);
}

/* ratQuery, which cannot be subscribed to; no SIM is ever asked here */
static void driver_rat_query(struct telephony_service *service, int sim_id,
                             telephony_rat_query_cb cb, void *data)
{
	g_assert_not_reached();
}

static struct telephony_driver driver = {
	.probe = driver_probe,
	.remove = driver_remove,
	.network_status_query = driver_network_status_query,
	.rat_query = driver_rat_query,
};

/* ------------------------------------------------------------------------
 * Fixture
 * ------------------------------------------------------------------------ */

static void status_noservice(struct telephony_network_status *s)
{
	memset(s, 0, sizeof(*s));
	s->state = TELEPHONY_NETWORK_STATE_NO_SERVICE;
	s->registration = TELEPHONY_NETWORK_REGISTRATION_NO_SERVICE;
	s->technology = "";
	s->ims_bearer = "";
}

/* The Brax3's SIM 2: registered, VoLTE */
static void status_volte(struct telephony_network_status *s)
{
	memset(s, 0, sizeof(*s));
	s->state = TELEPHONY_NETWORK_STATE_SERVICE;
	s->registration = TELEPHONY_NETWORK_REGISTRATION_HOME;
	s->name = "simyo";
	s->technology = "lte";
	s->volte = true;
	s->ims_registered = true;
	s->ims_voice_capable = true;
	s->ims_sms_capable = true;
	s->ims_bearer = "cellular";
	s->ims_wfc_preference = "wifi";
}

static unsigned int post_count(const char *key)
{
	GPtrArray *list = g_hash_table_lookup(posts, key);
	return list ? list->len : 0;
}

/* The last networkStatusQuery post under key, parsed */
static jvalue_ref last_post(const char *key)
{
	GPtrArray *list = g_hash_table_lookup(posts, key);
	jschema_ref schema;
	jvalue_ref parsed;

	g_assert_nonnull(list);
	g_assert_cmpuint(list->len, >, 0);
	schema = jschema_parse(j_cstr_to_buffer("{}"), DOMOPT_NOOPT, NULL);
	parsed = jdom_create(j_cstr_to_buffer(g_ptr_array_index(list, list->len - 1)), schema, NULL);
	jschema_release(&schema);
	g_assert_true(jis_object(parsed));
	return parsed;
}

static bool extended_bool(jvalue_ref post, const char *name)
{
	jvalue_ref extended = NULL, value = NULL;
	bool result = false;

	g_assert_true(jobject_get_exists(post, j_cstr_to_buffer("extended"), &extended));
	g_assert_true(jobject_get_exists(extended, j_cstr_to_buffer(name), &value));
	jboolean_get(value, &result);
	return result;
}

static int extended_sim(jvalue_ref post)
{
	jvalue_ref extended = NULL, value = NULL;
	int32_t sim_id = -1;

	g_assert_true(jobject_get_exists(post, j_cstr_to_buffer("extended"), &extended));
	g_assert_true(jobject_get_exists(extended, j_cstr_to_buffer("simId"), &value));
	jnumber_get_i32(value, &sim_id);
	return sim_id;
}

static void sim_info(struct telephony_service *service, int sim_id, bool present, const char *iccid)
{
	struct telephony_sim_info info;

	memset(&info, 0, sizeof(info));
	info.present = present;
	info.powered = present;
	info.iccid = iccid;
	telephony_service_sim_info_changed_notify(service, sim_id, &info);
}

static struct telephony_service* start(void)
{
	struct telephony_service *service;

	g_hash_table_remove_all(posts);
	g_ptr_array_set_size(subscribed, 0);
	g_clear_pointer(&last_reply, g_free);
	memset(queries, 0, sizeof(queries));
	memset(sim_fails, 0, sizeof(sim_fails));
	status_noservice(&sim_status[0]);
	status_noservice(&sim_status[1]);

	service = telephony_service_create();
	g_assert_nonnull(service);
	return service;
}

/* ------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------ */

/* telephonyd restarted: SIM 2 registers with IMS while SIM 1, which has no
 * service, is still taken as the voice SIM; then SIM 2 becomes it */
static void test_restart_race(void)
{
	struct telephony_service *service = start();
	jvalue_ref post;

	/* the modems: SIM 1 the default until the SIMs are read */
	telephony_service_sim_count_changed_notify(service, 2);
	g_assert_cmpint(telephony_service_get_default_sim(service, TELEPHONY_SIM_ROLE_VOICE), ==, 0);

	/* SIM 2's registration and IMS, before the default is known */
	status_volte(&sim_status[1]);
	telephony_service_network_status_changed_notify(service, 1, &sim_status[1]);
	g_assert_cmpuint(post_count("/networkStatusQuery/1"), ==, 1);
	post = last_post("/networkStatusQuery");
	g_assert_cmpint(extended_sim(post), ==, 0);
	g_assert_false(extended_bool(post, "volte"));
	j_release(&post);

	/* the SIMs are read: SIM 1 is empty, so SIM 2 is the voice SIM */
	sim_info(service, 0, false, NULL);
	sim_info(service, 1, true, "8931000000000000002");
	g_assert_cmpint(telephony_service_get_default_sim(service, TELEPHONY_SIM_ROLE_VOICE), ==, 1);

	/* the status bar's subscription hears SIM 2's VoLTE */
	post = last_post("/networkStatusQuery");
	g_assert_cmpint(extended_sim(post), ==, 1);
	g_assert_true(extended_bool(post, "volte"));
	g_assert_false(extended_bool(post, "vowifi"));
	j_release(&post);

	telephony_service_free(service);
}

/*
 * The Card Shell subscribes again as soon as telephonyd is back on the bus,
 * before telephonyd knows of any SIM slot. It is told there is no SIM yet,
 * and it stays subscribed, so it hears the voice SIM once there is one.
 */
static void test_subscribed_before_slots(void)
{
	struct telephony_service *service = start();
	LSMessage message = { "{\"subscribe\":true}", true };
	LSMessage for_sim = { "{\"subscribe\":true,\"simId\":1}", true };
	jvalue_ref post;

	_service_network_status_query_cb(service->palmHandle, &message, service);
	g_assert_nonnull(last_reply);
	g_assert_nonnull(strstr(last_reply, "\"returnValue\":false"));
	g_assert_cmpuint(subscribed->len, ==, 1);
	g_assert_cmpstr(g_ptr_array_index(subscribed, 0), ==, "/networkStatusQuery");

	/* nor is a method that cannot be subscribed to */
	_service_rat_query_cb(service->palmHandle, &message, service);
	g_assert_nonnull(strstr(last_reply, "No SIM slot available"));
	g_assert_cmpuint(subscribed->len, ==, 1);

	/* a SIM that is not there is not one to wait for */
	_service_network_status_query_cb(service->palmHandle, &for_sim, service);
	g_assert_nonnull(strstr(last_reply, "Unknown SIM slot"));
	g_assert_cmpuint(subscribed->len, ==, 1);

	/* the modems and SIMs come; SIM 2 is registered with IMS over Wi-Fi */
	status_volte(&sim_status[1]);
	sim_status[1].ims_bearer = "wifi";
	sim_status[1].vowifi = true;
	telephony_service_sim_count_changed_notify(service, 2);
	sim_info(service, 0, false, NULL);
	sim_info(service, 1, true, "8931000000000000002");

	post = last_post("/networkStatusQuery");
	g_assert_cmpint(extended_sim(post), ==, 1);
	g_assert_true(extended_bool(post, "volte"));
	g_assert_true(extended_bool(post, "vowifi"));
	j_release(&post);

	telephony_service_free(service);
}

/* Only when the voice SIM changes, and only for the voice role */
static void test_only_on_change(void)
{
	struct telephony_service *service = start();
	unsigned int before;

	telephony_service_sim_count_changed_notify(service, 2);
	sim_info(service, 0, true, "8931000000000000001");
	sim_info(service, 1, true, "8931000000000000002");
	g_assert_cmpint(telephony_service_get_default_sim(service, TELEPHONY_SIM_ROLE_VOICE), ==, 0);
	g_assert_cmpint(queries[0], ==, 1);
	g_assert_cmpint(queries[1], ==, 0);

	/* more SIM news, the same default: no new query */
	sim_info(service, 1, true, "8931000000000000002");
	g_assert_cmpint(queries[0], ==, 1);

	/* data and SMS to SIM 2: not followed by networkStatusQuery */
	before = post_count("/networkStatusQuery");
	g_assert_true(telephony_service_set_default_sim(service, TELEPHONY_SIM_ROLE_DATA, 1));
	g_assert_true(telephony_service_set_default_sim(service, TELEPHONY_SIM_ROLE_SMS, 1));
	g_assert_cmpint(queries[1], ==, 0);
	g_assert_cmpuint(post_count("/networkStatusQuery"), ==, before);
	g_assert_cmpint(data_sim, ==, 1);

	/* the same voice SIM again: nothing */
	g_assert_true(telephony_service_set_default_sim(service, TELEPHONY_SIM_ROLE_VOICE, 0));
	g_assert_cmpint(queries[0], ==, 1);
	g_assert_cmpuint(post_count("/networkStatusQuery"), ==, before);

	telephony_service_free(service);
}

/* The user picks SIM 2 for calls: the status bar follows at once */
static void test_voice_sim_set(void)
{
	struct telephony_service *service = start();
	jvalue_ref post;

	telephony_service_sim_count_changed_notify(service, 2);
	sim_info(service, 0, true, "8931000000000000001");
	sim_info(service, 1, true, "8931000000000000002");
	status_volte(&sim_status[1]);

	g_assert_true(telephony_service_set_default_sim(service, TELEPHONY_SIM_ROLE_VOICE, 1));
	g_assert_cmpint(queries[1], ==, 1);
	post = last_post("/networkStatusQuery");
	g_assert_cmpint(extended_sim(post), ==, 1);
	g_assert_true(extended_bool(post, "volte"));
	j_release(&post);
	/* and its own subscribers hear it as well */
	post = last_post("/networkStatusQuery/1");
	g_assert_true(extended_bool(post, "volte"));
	j_release(&post);

	telephony_service_free(service);
}

/* A SIM whose modem has nothing to say yet: no post, no crash */
static void test_driver_fails(void)
{
	struct telephony_service *service = start();

	sim_fails[0] = true;
	sim_fails[1] = true;
	telephony_service_sim_count_changed_notify(service, 2);
	g_assert_cmpint(queries[0], ==, 1);
	g_assert_cmpuint(post_count("/networkStatusQuery"), ==, 0);

	sim_info(service, 0, false, NULL);
	sim_info(service, 1, true, "8931000000000000002");
	g_assert_cmpint(queries[1], ==, 1);
	g_assert_cmpuint(post_count("/networkStatusQuery"), ==, 0);
	g_assert_cmpuint(post_count("/networkStatusQuery/1"), ==, 0);

	/* nor for a SIM that is not there */
	telephony_service_default_sim_changed(service, TELEPHONY_SIM_ROLE_VOICE, 5);
	telephony_service_default_sim_changed(NULL, TELEPHONY_SIM_ROLE_VOICE, 0);
	g_assert_cmpint(queries[0] + queries[1], ==, 2);

	telephony_service_free(service);
}

/* A driver without networkStatusQuery */
static void test_driver_without_query(void)
{
	struct telephony_service *service;

	driver.network_status_query = NULL;
	service = start();
	telephony_service_sim_count_changed_notify(service, 2);
	sim_info(service, 1, true, "8931000000000000002");
	g_assert_cmpuint(post_count("/networkStatusQuery"), ==, 0);
	telephony_service_free(service);
	driver.network_status_query = driver_network_status_query;
}

int main(int argc, char **argv)
{
	int ret;

	g_test_init(&argc, &argv, NULL);
	posts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_ptr_array_unref);
	subscribed = g_ptr_array_new_with_free_func(g_free);
	telephony_driver_register(&driver);

	g_test_add_func("/telephonyservice-net/restart-race", test_restart_race);
	g_test_add_func("/telephonyservice-net/subscribed-before-slots", test_subscribed_before_slots);
	g_test_add_func("/telephonyservice-net/only-on-change", test_only_on_change);
	g_test_add_func("/telephonyservice-net/voice-sim-set", test_voice_sim_set);
	g_test_add_func("/telephonyservice-net/driver-fails", test_driver_fails);
	g_test_add_func("/telephonyservice-net/driver-without-query", test_driver_without_query);

	ret = g_test_run();

	telephony_driver_unregister(&driver);
	g_hash_table_destroy(posts);
	g_ptr_array_unref(subscribed);
	g_free(last_reply);
	return ret;
}

// vim:ts=4:sw=4:noexpandtab
