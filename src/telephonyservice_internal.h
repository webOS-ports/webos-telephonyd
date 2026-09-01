/* @@@LICENSE
*
* Copyright (c) 2012 Simon Busch <morphis@gravedo.de>
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

#ifndef TELEPHONY_SERVICE_INTERNAL_H_
#define TELEPHONY_SERVICE_INTERNAL_H_

#include <glib.h>
#include <pbnjson.h>
#include <luna-service2/lunaservice.h>

#include "telephonydriver.h"

/**
 * Everything the service tracks for one SIM slot. Before dual SIM support all
 * of these lived directly in struct telephony_service; the single-SIM case is
 * now simply an array of length one.
 */
struct telephony_sim_state {
	int sim_id;
	bool initialized;
	bool power_off_pending;
	bool network_status_query_pending;
	bool network_registered;
	bool powered;
	bool data_registered;

	/* cached descriptive data, owned by the service */
	bool present;
	gchar *iccid;
	gchar *imsi;
	gchar *msisdn;
	gchar *operator_name;
	gchar *modem_path;
	/* user supplied label for the slot, e.g. "Work" */
	gchar *name;
	enum telephony_sim_status sim_status;
	enum telephony_network_state network_state;
	enum telephony_network_registration network_registration;
	int signal_bars;
};

struct telephony_service {
	struct telephony_driver *driver;
	void *data;
	LSHandle *palmHandle;
	LSHandle *webosHandle;

	/* struct telephony_sim_state*, index == sim_id */
	GPtrArray *sims;
	/* slot index per telephony_sim_role, -1 when unset */
	int default_sim[TELEPHONY_SIM_ROLE_MAX];
	/**
	 * The user's stored preference per role, remembered by ICCID rather than by
	 * slot index: a slot index means nothing once the SIMs are swapped between
	 * slots or one of them is removed.
	 */
	gchar *default_sim_iccid[TELEPHONY_SIM_ROLE_MAX];
	/* ICCID -> user supplied label, loaded from the settings store */
	GHashTable *sim_names;
	/*
	 * Airplane mode. Persisted, and layered over the per slot power state: a
	 * modem is online only when its own stored state says so AND this is off.
	 */
	bool airplane_mode;
};

int telephonyservice_common_finish(const struct telephony_error *error, void *data);

/* Per-SIM state lookup. Returns NULL when sim_id is out of range. */
struct telephony_sim_state* telephony_service_sim_state(struct telephony_service *service, int sim_id);

/**
 * Resolve the "simId" parameter of an incoming request.
 *
 * Returns the slot index to operate on, or -1 when the request named a slot
 * that does not exist. A missing "simId" resolves to the default slot for the
 * given role, which keeps every pre-dual-SIM client working unchanged.
 */
int telephony_service_resolve_sim_id(struct telephony_service *service, jvalue_ref parsed_obj,
                                     enum telephony_sim_role role, bool *explicit_sim);

/* True when at least one slot finished initializing. */
bool telephony_service_any_initialized(struct telephony_service *service);

/**
 * Post a subscription update for a per-SIM method.
 *
 * The payload goes to the slot specific key ("/<method>/<simId>") on both bus
 * names, and additionally to the legacy key ("/<method>") when the slot is the
 * default for the role, so that clients which never learned about simId keep
 * receiving updates for the SIM they care about.
 */
void telephony_service_post_sim_subscription(struct telephony_service *service, const char *method,
                                             int sim_id, enum telephony_sim_role role, jvalue_ref reply_obj);

/* Post a subscription update that is not tied to a specific slot. */
void telephony_service_post_subscription(struct telephony_service *service, const char *method,
                                         jvalue_ref reply_obj);

/**
 * Register the incoming message for a subscription.
 *
 * When the client asked for a specific slot it is added to the slot specific
 * key, otherwise to the legacy key.
 */
bool telephony_service_process_sim_subscription(struct telephony_service *service, LSHandle *handle,
                                                LSMessage *message, const char *method, int sim_id,
                                                bool explicit_sim);

/**
 * Common entry sequence for a per-SIM luna method.
 *
 * Parses the payload, resolves "simId" for the given role, optionally rejects
 * the call when the slot's backend is not up yet, and registers the message for
 * a subscription. On failure it has already answered the client and returns
 * NULL. On success the caller owns both the returned request data and
 * *parsed_obj (which may be jinvalid when the payload was empty).
 */
/**
 * Same as telephony_service_begin_request() but for handlers which already
 * parsed the payload themselves. Does not touch subscriptions.
 */
struct luna_service_req_data* telephony_service_begin_parsed_request(struct telephony_service *service,
                                                                     LSHandle *handle, LSMessage *message,
                                                                     jvalue_ref parsed_obj,
                                                                     enum telephony_sim_role role,
                                                                     bool require_initialized);

struct luna_service_req_data* telephony_service_begin_request(struct telephony_service *service,
                                                              LSHandle *handle, LSMessage *message,
                                                              const char *method,
                                                              enum telephony_sim_role role,
                                                              bool require_initialized,
                                                              bool subscribable,
                                                              jvalue_ref *parsed_obj);

/* Re-post the state of every slot; used after the default SIM assignment changed. */
void telephony_service_repost_sim_list(struct telephony_service *service);

/* Build the simList payload (array of per-slot objects). */
jvalue_ref telephony_service_build_sim_list(struct telephony_service *service);

/* Build the defaultSim payload ({voice, sms, data}). */
jvalue_ref telephony_service_build_default_sims(struct telephony_service *service);

/* Remember the radio power state a slot should come up with after a restart. */
void telephony_service_store_power_state_for_sim(int sim_id, bool power);
void telephony_service_store_airplane_mode(bool airplane_mode);
bool telephony_service_effective_power_state(struct telephony_service *service, int sim_id);

/* Persist and apply a user supplied label for a slot. */
bool telephony_service_set_sim_name(struct telephony_service *service, int sim_id, const char *name);

/* Add "simId" plus the slot's label to a reply object. */
void telephony_service_add_sim_id(jvalue_ref reply_obj, int sim_id);

/* Implemented in telephonyservice_simmgmt.c */
bool _service_airplane_mode_set_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_airplane_mode_query_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_sim_list_query_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_default_sim_query_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_default_sim_set_cb(LSHandle *handle, LSMessage *message, void *user_data);
bool _service_sim_name_set_cb(LSHandle *handle, LSMessage *message, void *user_data);

#endif

// vim:ts=4:sw=4:noexpandtab
