/* @@@LICENSE
*
* Copyright (c) 2013 Simon Busch <morphis@gravedo.de>
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

#ifndef OFONO_VOICECALL_MANAGER_H_
#define OFONO_VOICECALL_MANAGER_H_

#include "ofonobase.h"

enum ofono_voicecall_clir_option {
	OFONO_VOICECALL_CLIR_OPTION_DEFAULT = 0,
	OFONO_VOICECALL_CLIR_OPTION_ENABLED,
	OFONO_VOICECALL_CLIR_OPTION_DISABLED
};

typedef void (*ofono_voicecall_manager_get_calls_cb)(const struct ofono_error *error,
														 GList *calls, void *data);
typedef void (*ofono_voicecall_manager_dial_cb)(const struct ofono_error *error,
												const char *path, void *data);

typedef void (*ofono_voicecall_manager_call_changed_cb)(const char *path, void *data);

struct ofono_voicecall_manager;

struct ofono_voicecall_manager* ofono_voicecall_manager_create(const gchar *path);
void ofono_voicecall_manager_ref(struct ofono_voicecall_manager *vm);
void ofono_voicecall_manager_unref(struct ofono_voicecall_manager *vm);
void ofono_voicecall_manager_free(struct ofono_voicecall_manager *vm);

void ofono_voicecall_manager_register_prop_changed_cb(struct ofono_voicecall_manager *vm,
													   ofono_property_changed_cb cb, void *data);
void ofono_voicecall_manager_register_calls_changed_cb(struct ofono_voicecall_manager *vm,
												  ofono_base_cb cb, void *data);

void ofono_voicecall_manager_register_call_added_cb(struct ofono_voicecall_manager *vm,
													ofono_voicecall_manager_call_changed_cb cb, void *data);
void ofono_voicecall_manager_register_call_removed_cb(struct ofono_voicecall_manager *vm,
													ofono_voicecall_manager_call_changed_cb cb, void *data);

void ofono_voicecall_manager_dial(struct ofono_voicecall_manager *vm,
								  const char *number, enum ofono_voicecall_clir_option clir,
								  ofono_voicecall_manager_dial_cb, void *data);
void ofono_voicecall_manager_transfer(struct ofono_voicecall_manager *vm,
									  ofono_base_result_cb cb, void *data);
void ofono_voicecall_manager_swap_calls(struct ofono_voicecall_manager *vm,
									  ofono_base_result_cb cb, void *data);
void ofono_voicecall_manager_release_and_answer(struct ofono_voicecall_manager *vm,
									  ofono_base_result_cb cb, void *data);
void ofono_voicecall_manager_release_and_swap(struct ofono_voicecall_manager *vm,
									  ofono_base_result_cb cb, void *data);
void ofono_voicecall_manager_hold_and_answer(struct ofono_voicecall_manager *vm,
									  ofono_base_result_cb cb, void *data);
void ofono_voicecall_manager_hangup_all(struct ofono_voicecall_manager *vm,
									  ofono_base_result_cb cb, void *data);
void ofono_voicecall_manager_send_tones(struct ofono_voicecall_manager *vm, const char *tones,
									  ofono_base_result_cb cb, void *data);

void ofono_voicecall_manager_get_calls(struct ofono_voicecall_manager *vm,
										ofono_voicecall_manager_get_calls_cb cb, void *data);
GList* ofono_voicecall_manager_get_emergency_numbers(struct ofono_voicecall_manager *vm);

#endif

// vim:ts=4:sw=4:noexpandtab
