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

#ifndef TELEPHONY_DRIVER_H_
#define TELEPHONY_DRIVER_H_

#include <stdbool.h>

struct telephony_service;

struct telephony_error {
	int code;
};

/**
 * Identifies one SIM slot / modem. Slots are numbered from 0 in the order
 * ofono reports the corresponding modems.
 *
 * TELEPHONY_SIM_ID_DEFAULT is what a client gets when it does not pass a
 * "simId" parameter: the service resolves it to the SIM configured as the
 * default for the role the called method belongs to.
 */
#define TELEPHONY_SIM_ID_DEFAULT	(-1)

/* Upper bound on the number of SIM slots we are willing to track. */
#define TELEPHONY_MAX_SIMS		8

/**
 * The role a SIM can be the default for. webOS/LuneOS lets the user pick a
 * different SIM for outgoing calls, outgoing messages and packet data, which
 * mirrors what other multi-SIM platforms do.
 */
enum telephony_sim_role {
	TELEPHONY_SIM_ROLE_VOICE = 0,
	TELEPHONY_SIM_ROLE_SMS,
	TELEPHONY_SIM_ROLE_DATA,
	TELEPHONY_SIM_ROLE_MAX
};

const char* telephony_sim_role_to_string(enum telephony_sim_role role);

enum telephony_error_type {
	TELEPHONY_ERROR_NOT_IMPLEMENTED = 0,
	TELEPHONY_ERROR_INTERNAL,
	TELEPHONY_ERROR_INVALID_ARGUMENT,
	TELEPHONY_ERROR_NOT_AVAILABLE,
	TELEPHONY_ERROR_ALREADY_INPROGRESS,
	TELEPHONY_ERROR_FAIL
};

enum telephony_sim_status {
	TELEPHONY_SIM_STATUS_SIM_NOT_FOUND = 0,
	TELEPHONY_SIM_STATUS_SIM_INVALID,
	TELEPHONY_SIM_STATUS_SIM_READY,
	TELEPHONY_SIM_STATUS_PIN_REQUIRED,
	TELEPHONY_SIM_STATUS_PUK_REQUIRED,
	TELEPHONY_SIM_STATUS_PIN_PERM_BLOCKED,
};

enum telephony_tty_mode {
	TELEPHONY_TTY_MODE_FULL = 0,
};

enum telephony_network_state {
	TELEPHONY_NETWORK_STATE_SERVICE = 0,
	TELEPHONY_NETWORK_STATE_NO_SERVICE,
	TELEPHONY_NETWORK_STATE_LIMITED,
};

enum telephony_network_registration {
	TELEPHONY_NETWORK_REGISTRATION_HOME = 0,
	TELEPHONY_NETWORK_REGISTRATION_ROAM,
	TELEPHONY_NETWORK_REGISTRATION_ROAM_BLINK,
	TELEPHONY_NETWORK_REGISTRATION_SEARCHING,
	TELEPHONY_NETWORK_REGISTRATION_DENIED,
	TELEPHONY_NETWORK_REGISTRATION_NO_SERVICE
};

enum telephony_radio_access_mode {
	/* forces the enum to a signed type so parse failures survive < 0 checks */
	TELEPHONY_RADIO_ACCESS_MODE_INVALID = -1,
	TELEPHONY_RADIO_ACCESS_MODE_ANY = 0,
	TELEPHONY_RADIO_ACCESS_MODE_GSM,
	TELEPHONY_RADIO_ACCESS_MODE_UMTS,
	TELEPHONY_RADIO_ACCESS_MODE_LTE,
	TELEPHONY_RADIO_ACCESS_MODE_UNKNOWN,
};

enum telephony_platform_type {
	TELEPHONY_PLATFORM_TYPE_GSM,
	TELEPHONY_PLATFORM_TYPE_CDMA,
};

const char* telephony_platform_type_to_string(enum telephony_platform_type type);
const char* telephony_sim_status_to_string(enum telephony_sim_status sim_status);
const char* telephony_network_state_to_string(enum telephony_network_state state);
const char* telephony_network_registration_to_string(enum telephony_network_registration netreg);
const char* telephony_radio_access_mode_to_string(enum telephony_radio_access_mode mode);

enum telephony_radio_access_mode telephony_radio_access_mode_from_string(const char *mode);

struct telephony_network_status {
	enum telephony_network_state state;
	enum telephony_network_registration registration;
	const gchar *name;
	int cause_code;
	bool data_registered;
	/*
	 * Registered with IMS for voice calls, over any bearer: LTE, NR or
	 * Wi-Fi. A status bar that knows vowifi shows VoWiFi when that is set
	 * and VoLTE otherwise; one that does not shows VoLTE for both, as it
	 * did before telephonyd knew the bearer, since a call does go over IMS.
	 */
	bool volte;
	/* volte, with the IMS registration over Wi-Fi: Wi-Fi calling */
	bool vowifi;
	/* the IMS registration, for a phone-info page */
	bool ims_registered;
	bool ims_voice_capable;
	bool ims_sms_capable;
	/* "wifi", "cellular" or "" while not registered or not known */
	const gchar *ims_bearer;
	/* the Wi-Fi calling preference: "wifi", "cellular", "wifi-only", ""
	 * (unknown), or NULL where the modem has none to report */
	const gchar *ims_wfc_preference;
	/* ofono's name of the registered radio technology: "gsm", "edge",
	 * "umts", "hspa", "lte", "nr" or "" */
	const gchar *technology;
	/* enum telephony_data_type data_type; */
};

/*
 * A serving cell's signal, in the units a phone-information page shows:
 * RSSI, RSRP in dBm, RSRQ and SINR in dB. RSRP, RSRQ and SINR are LTE's
 * RSRP, RSRQ and RS-SNR, or NR's SS-RSRP, SS-RSRQ and SS-SINR. A value the
 * modem did not report, or reported out of its range, is left out.
 */
struct telephony_cell_signal {
	/* "gsm", "wcdma", "lte" or "nr" */
	const gchar *type;
	bool has_rssi;
	int rssi;
	bool has_rsrp;
	int rsrp;
	bool has_rsrq;
	int rsrq;
	bool has_sinr;
	double sinr;
};

struct telephony_platform_info {
	enum telephony_platform_type platform_type;
	const gchar *imei;
	const gchar *carrier;
	int mcc;
	int mnc;
	/* FIXME: whats with "capabilities":{"dataCategory":10} ? */
	const gchar *version;
};

struct telephony_pin_status {
	bool enabled;
	bool required;
	bool puk_required;
	bool perm_blocked;
	bool device_locked;
	int pin_attempts_remaining;
	int puk_attempts_remaining;
};

struct telephony_fdn_status {
	bool enabled;
	bool permanent_block;
};

struct telephony_network {
	int id;
	const gchar *name;
	enum telephony_radio_access_mode radio_access_mode;
};

/**
 * Static-ish description of a single SIM slot, used to enumerate the SIMs
 * present in the device.
 */
struct telephony_sim_info {
	int sim_id;
	bool present;
	const gchar *iccid;
	const gchar *imsi;
	const gchar *msisdn;
	const gchar *operator_name;
	const gchar *modem_path;
	enum telephony_sim_status sim_status;
	bool powered;
};

struct telephony_subscriber_info {
	enum telephony_platform_type platform_type;
	const gchar *imsi;
	const gchar *msisdn;
	const gchar *min;
	const gchar *mdn;
};

typedef void (*telephony_driver_result_cb)(void *data);

typedef int (*telephony_result_cb)(const struct telephony_error* error, void *data);
/* ref identifies the sent message in later status reports, NULL on error */
typedef int (*telephony_sms_sent_cb)(const struct telephony_error* error, const char *ref, void *data);
typedef int (*telephony_power_query_cb)(const struct telephony_error* error, bool power_state, void *data);
typedef int (*telephony_sim_status_query_cb)(const struct telephony_error* error, enum telephony_sim_status status, void *data);
typedef int (*telephony_network_status_query_cb)(const struct telephony_error* error, struct telephony_network_status *status, void *data);
typedef int (*telephony_signal_strength_query_cb)(const struct telephony_error* error, unsigned int bars, void *data);
/* The cells the SIM's modem is registered on, count of them */
typedef int (*telephony_cell_info_query_cb)(const struct telephony_error* error, const struct telephony_cell_signal *cells,
											unsigned int count, void *data);
typedef int (*telephony_pin_status_query_cb)(const struct telephony_error* error, struct telephony_pin_status *status, void *data);
typedef int (*telephony_network_list_query_cb)(const struct telephony_error* error, GList *networks, void *data);
typedef int (*telephony_platform_query_cb)(const struct telephony_error* error, struct telephony_platform_info *platform_info, void *data);
typedef int (*telephony_network_id_query_cb)(const struct telephony_error *error, const char *id, void *data);
typedef int (*telephony_network_selection_mode_query_cb)(const struct telephony_error *error, bool automatic, void *data);
typedef int (*telephony_fdn_status_query_cb)(const struct telephony_error* error, struct telephony_fdn_status *status, void *data);
typedef int (*telephony_rat_query_cb)(const struct telephony_error *error, enum telephony_radio_access_mode mode, void *data);
typedef int (*telephony_sim_info_query_cb)(const struct telephony_error *error, struct telephony_sim_info *info, void *data);
typedef int (*telephony_subscriber_id_query_cb)(const struct telephony_error *error, struct telephony_subscriber_info *info, void *data);

struct telephony_driver {
	int (*probe)(struct telephony_service *service);
	void (*remove)(struct telephony_service *service);

	/**
	 * Every operation below is scoped to a single SIM slot. sim_id is always a
	 * concrete slot index by the time the driver sees it; the service layer
	 * resolves TELEPHONY_SIM_ID_DEFAULT before dispatching.
	 */
	void (*platform_query)(struct telephony_service *service, int sim_id, telephony_platform_query_cb cb, void *data);
	void (*subscriber_id_query)(struct telephony_service *service, int sim_id, telephony_subscriber_id_query_cb cb, void *data);

	/* SIM slot enumeration */
	void (*sim_info_query)(struct telephony_service *service, int sim_id, telephony_sim_info_query_cb cb, void *data);

	/* power management */
	void (*power_query)(struct telephony_service *service, int sim_id, telephony_power_query_cb cb, void *data);
	void (*power_set)(struct telephony_service *service, int sim_id, bool power, telephony_result_cb cb, void *data);

	/* SIM */
	void (*sim_status_query)(struct telephony_service *service, int sim_id, telephony_sim_status_query_cb, void *data);
	void (*pin1_status_query)(struct telephony_service *service, int sim_id, telephony_pin_status_query_cb cb, void *data);
	void (*pin1_verify)(struct telephony_service *service, int sim_id, const gchar *pin, telephony_result_cb cb, void *data);
	void (*pin1_change)(struct telephony_service *service, int sim_id, const gchar *old_pin, const gchar *new_pin, telephony_result_cb cb, void *data);
	void (*pin1_enable)(struct telephony_service *service, int sim_id, const gchar *pin, telephony_result_cb cb, void *data);
	void (*pin1_disable)(struct telephony_service *service, int sim_id, const gchar *pin, telephony_result_cb cb, void *data);
	void (*pin1_unblock)(struct telephony_service *service, int sim_id, const gchar *puk, const gchar *new_pin, telephony_result_cb cb, void *data);
	void (*pin2_status_query)(struct telephony_service *service, int sim_id, telephony_pin_status_query_cb cb, void *data);
	void (*fdn_status_query)(struct telephony_service *service, int sim_id, telephony_fdn_status_query_cb cb, void *data);

	/* network */
	void (*network_status_query)(struct telephony_service *service, int sim_id, telephony_network_status_query_cb cb, void *data);
	void (*signal_strength_query)(struct telephony_service *service, int sim_id, telephony_signal_strength_query_cb cb, void *data);
	void (*cell_info_query)(struct telephony_service *service, int sim_id, telephony_cell_info_query_cb cb, void *data);
	void (*network_list_query)(struct telephony_service *service, int sim_id, telephony_network_list_query_cb cb, void *data);
	void (*network_list_query_cancel)(struct telephony_service *service, int sim_id, telephony_result_cb cb, void *data);
	void (*network_set)(struct telephony_service *service, int sim_id, bool automatic, const char *id, telephony_result_cb cb, void *data);
	void (*network_id_query)(struct telephony_service *service, int sim_id, telephony_network_id_query_cb cb, void *data);
	void (*network_selection_mode_query)(struct telephony_service *service, int sim_id, telephony_network_selection_mode_query_cb cb, void *data);
	void (*rat_query)(struct telephony_service *service, int sim_id, telephony_rat_query_cb cb, void *data);
	void (*rat_set)(struct telephony_service *service, int sim_id, enum telephony_radio_access_mode mode, telephony_result_cb cb, void *data);

	/* call control */
	void (*dial)(struct telephony_service *service, int sim_id, const char *number, bool block_id, telephony_result_cb, void *data);
	void (*answer)(struct telephony_service *service, int sim_id, int id, telephony_result_cb cb, void *data);
	void (*ignore)(struct telephony_service *service, int sim_id, int id, telephony_result_cb cb, void *data);
	void (*hangup)(struct telephony_service *service, int sim_id, int id, telephony_result_cb cb, void *data);

	/* sms */
	/* delivery_report: ask the network for a status report, which comes back
	 * through telephony_service_sms_status_report() with the cb's ref */
	void (*send_sms)(struct telephony_service *service, int sim_id, const char *to, const char *text,
					 bool delivery_report, telephony_sms_sent_cb cb, void *data);

	/* SIM toolkit: start a session with the main menu item at index */
	void (*stk_select_item)(struct telephony_service *service, int sim_id, int index, telephony_result_cb cb, void *data);
};

#endif

// vim:ts=4:sw=4:noexpandtab
