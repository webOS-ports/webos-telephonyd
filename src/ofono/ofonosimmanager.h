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

#ifndef OFONO_SIM_MANAGER_H_
#define OFONO_SIM_MANAGER_H_

#include "ofonobase.h"

struct ofono_sim_manager;

enum ofono_sim_pin {
	OFONO_SIM_PIN_TYPE_NONE = 0,
	OFONO_SIM_PIN_TYPE_PIN,
	OFONO_SIM_PIN_TYPE_PHONE,
	OFONO_SIM_PIN_TYPE_FIRST_PHONE,
	OFONO_SIM_PIN_TYPE_PIN2,
	OFONO_SIM_PIN_TYPE_NETWORK,
	OFONO_SIM_PIN_TYPE_NET_SUB,
	OFONO_SIM_PIN_TYPE_SERVICE,
	OFONO_SIM_PIN_TYPE_CORP,
	OFONO_SIM_PIN_TYPE_PUK,
	OFONO_SIM_PIN_TYPE_FIRST_PHONE_PUK,
	OFONO_SIM_PIN_TYPE_PUK2,
	OFONO_SIM_PIN_TYPE_NETWORK_PUK,
	OFONO_SIM_PIN_TYPE_NET_SUB_PUK,
	OFONO_SIM_PIN_TYPE_SERVICE_PUK,
	OFONO_SIM_PIN_TYPE_CORP_PUK,
	OFONO_SIM_PIN_TYPE_INVALID,
	OFONO_SIM_PIN_TYPE_MAX
};

struct ofono_sim_manager* ofono_sim_manager_create(const gchar *path);
void ofono_sim_manager_ref(struct ofono_sim_manager *sim);
void ofono_sim_manager_unref(struct ofono_sim_manager *sim);
void ofono_sim_manager_free(struct ofono_sim_manager *sim);

void ofono_sim_manager_register_prop_changed_handler(struct ofono_sim_manager *sim, ofono_property_changed_cb cb, void *data);

void ofono_sim_manager_enter_pin(struct ofono_sim_manager *sim, enum ofono_sim_pin type, const gchar *pin,
						ofono_base_result_cb cb, void *data);
void ofono_sim_manager_lock_pin(struct ofono_sim_manager *sim, enum ofono_sim_pin type, const gchar *pin,
						ofono_base_result_cb cb, void *data);
void ofono_sim_manager_unlock_pin(struct ofono_sim_manager *sim, enum ofono_sim_pin type, const gchar *pin,
						ofono_base_result_cb cb, void *data);
void ofono_sim_manager_change_pin(struct ofono_sim_manager *sim, enum ofono_sim_pin type, const gchar *old_pin,
						const gchar *new_pin, ofono_base_result_cb cb, void *data);
void ofono_sim_manager_reset_pin(struct ofono_sim_manager *sim, enum ofono_sim_pin type, const gchar *puk,
						const gchar *new_pin, ofono_base_result_cb cb, void *data);

bool ofono_sim_manager_get_present(struct ofono_sim_manager *sim);
enum ofono_sim_pin ofono_sim_manager_get_pin_required(struct ofono_sim_manager *sim);
int ofono_sim_manager_get_pin_retries(struct ofono_sim_manager *sim, enum ofono_sim_pin pin_type);
bool ofono_sim_manager_is_pin_locked(struct ofono_sim_manager *sim, enum ofono_sim_pin pin_type);
bool ofono_sim_manager_get_fixed_dialing(struct ofono_sim_manager *sim);
const char* ofono_sim_manager_get_mcc(struct ofono_sim_manager *sim);
const char* ofono_sim_manager_get_mnc(struct ofono_sim_manager *sim);

#endif

// vim:ts=4:sw=4:noexpandtab
