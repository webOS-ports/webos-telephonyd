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

struct telephony_service;

const struct telephony_error {
	int code;
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
};

enum telephony_radio_access_mode {
	TELEPHONY_RADIO_ACCESS_MODE_ANY = 0,
	TELEPHONY_RADIO_ACCESS_MODE_GSM,
	TELEPHONY_RADIO_ACCESS_MODE_UMTS,
	TELEPHONY_RADIO_ACCESS_MODE_LTE,
};

enum telephony_platform_type {
	TELEPHONY_PLATFORM_TYPE_GSM,
	TELEPHONY_PLATFORM_TYPE_CDMA,
};

const char* telephony_platform_type_to_string(enum telephony_platform_type type);

struct telephony_network_status {
	enum telephony_network_state state;
	enum telephony_network_registration registration;
	const gchar *name;
	int cause_code;
	bool data_registered;
	/* enum telephony_data_type data_type; */
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
	bool pin_attempts_remaining;
	bool puk_attempts_remaining;
};

struct telephony_network {
	int id;
	const gchar *name;
	enum telephony_radio_access_mode radio_access_mode;
};

typedef void (*telephony_driver_result_cb)(void *data);

typedef int (*telephony_result_cb)(const struct telephony_error* error, void *data);
typedef int (*telephony_power_query_cb)(const struct telephony_error* error, bool power_state, void *data);
typedef int (*telephony_sim_status_query_cb)(const struct telephony_error* error, enum telephony_sim_status status, void *data);
typedef int (*telephony_network_status_query_cb)(const struct telephony_error* error, struct telephony_network_status *status, void *data);
typedef int (*telephony_signal_strength_query_cb)(const struct telephony_error* error, unsigned int rssi, unsigned int max_bars, unsigned int bars, void *data);
typedef int (*telephony_pin_status_query_cb)(const struct telephony_error* error, struct telephony_pin_status, void *data);
typedef int (*telephony_network_list_query_cb)(const struct telephony_error* error, GList *networks, void *data);
typedef int (*telephony_platform_query_cb)(const struct telephony_error* error, struct telephony_platform_info *platform_info, void *data);

struct telephony_driver {
	int (*probe)(struct telephony_service *service);
	void (*remove)(struct telephony_service *service);

	int (*platform_query)(struct telephony_service *service, telephony_platform_query_cb cb, void *data);

	/* power management */
	int (*power_query)(struct telephony_service *service, telephony_power_query_cb cb, void *data);
	int (*power_set)(struct telephony_service *service, bool power, telephony_result_cb cb, void *data);

	/* SIM */
	int (*sim_status_query)(struct telephony_service *service, telephony_sim_status_query_cb, void *data);
	int (*pin1_status_query)(struct telephony_service *service, telephony_pin_status_query_cb cb, void *data);
	int (*pin2_status_query)(struct telephony_service *service, telephony_pin_status_query_cb cb, void *data);
	int (*pin1_verify)(struct telephony_service *service, const gchar *pin, telephony_result_cb cb, void *data);
	int (*pin1_change)(struct telephony_service *service, const gchar *old_pin, const gchar *new_pin, telephony_result_cb cb, void *data);
	int (*pin1_enable)(struct telephony_service *service, const gchar *pin, telephony_result_cb cb, void *data);
	int (*pin1_disable)(struct telephony_service *service, const gchar *pin, telephony_result_cb cb, void *data);
	int (*pin1_unblock)(struct telephony_service *service, const gchar *puk, const gchar *pin, void *data);
	int (*pin2_verify)(struct telephony_service *service, const gchar *pin, telephony_result_cb cb, void *data);
	int (*pin2_change)(struct telephony_service *service, const gchar *old_pin, const gchar *new_pin, telephony_result_cb cb, void *data);
	int (*pin2_unblock)(struct telephony_service *service, const gchar *puk, const gchar *pin, void *data);

	/* network */
	int (*network_status_query)(struct telephony_service *service, telephony_network_status_query_cb cb, void *data);
	int (*signal_strength_query)(struct telephony_service *service, telephony_signal_strength_query_cb cb, void *data);
	int (*network_list_query)(struct telephony_service *service, telephony_network_list_query_cb cb, void *data);
};

#endif

// vim:ts=4:sw=4:noexpandtab
