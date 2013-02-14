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

#ifndef OFONO_VOICECALL_H_
#define OFONO_VOICECALL_H_

#include "ofonobase.h"

struct ofono_voicecall;

enum ofono_voicecall_disconnect_reason {
	OFONO_VOICECALL_DISCONNECT_REASON_LOCAL = 0,
	OFONO_VOICECALL_DISCONNECT_REASON_REMOTE,
	OFONO_VOICECALL_DISCONNECT_REASON_NETWORK
};

enum ofono_voicecall_state {
	OFONO_VOICECALL_STATE_ACTIVE = 0,
	OFONO_VOICECALL_STATE_HELD,
	OFONO_VOICECALL_STATE_DIALING,
	OFONO_VOICECALL_STATE_ALERTING,
	OFONO_VOICECALL_STATE_INCOMING,
	OFONO_VOICECALL_STATE_WAITING,
	OFONO_VOICECALL_STATE_DISCONNECTED
};

struct ofono_voicecall* ofono_voicecall_create(const gchar *path);
void ofono_voicecall_ref(struct ofono_voicecall *call);
void ofono_voicecall_unref(struct ofono_voicecall *call);
void ofono_voicecall_free(struct ofono_voicecall *call);

const char* ofono_voicecall_get_path(struct ofono_voicecall *call);

void ofono_voicecall_register_prop_changed_cb(struct ofono_voicecall *call,
											ofono_property_changed_cb cb, void *data);

void ofono_voicecall_deflect(struct ofono_voicecall *call, const char *number, ofono_base_result_cb cb, void *data);
void ofono_voicecall_hangup(struct ofono_voicecall *call, ofono_base_result_cb cb, void *data);
void ofono_voicecall_answer(struct ofono_voicecall *call, ofono_base_result_cb cb, void *data);

const char* ofono_voicecall_get_line_identification(struct ofono_voicecall *call);
const char* ofono_voicecall_get_incoming_line(struct ofono_voicecall *call);
const char* ofono_voicecall_get_name(struct ofono_voicecall *call);
bool ofono_voicecall_get_mutliparty(struct ofono_voicecall *call);
const char* ofono_voicecall_get_start_time(struct ofono_voicecall *call);
bool ofono_voicecall_get_emergency(struct ofono_voicecall *call);
bool ofono_voicecall_get_remote_held(struct ofono_voicecall *call);
bool ofono_voicecall_get_remote_multiparty(struct ofono_voicecall *call);

#endif

// vim:ts=4:sw=4:noexpandtab
