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

#ifndef OFONO_MODEM_H_
#define OFONO_MODEM_H_

#include <glib.h>

struct ofono_modem;

typedef void (*ofono_modem_result_cb)(gboolean success, void *data);

enum ofono_modem_interface {
	OFONO_MODEM_INTERFACE_ASSISTET_SATELLITE_NAVIGATION = 0,
	OFONO_MODEM_INTERFACE_AUDIO_SETTINGS,
	OFONO_MODEM_INTERFACE_CALL_BARRING,
	OFONO_MODEM_INTERFACE_CALL_FORWARDING,
	OFONO_MODEM_INTERFACE_CALL_METER,
	OFONO_MODEM_INTERFACE_CALL_SETTINGS,
	OFONO_MODEM_INTERFACE_CALL_VOLUME,
	OFONO_MODEM_INTERFACE_CELL_BROADCAST,
	OFONO_MODEM_INTERFACE_HANDSFREE,
	OFONO_MODEM_INTERFACE_LOCATION_REPORTING,
	OFONO_MODEM_INTERFACE_MESSAGE_MANAGER,
	OFONO_MODEM_INTERFACE_MESSAGE_WAITING,
	OFONO_MODEM_INTERFACE_NETWORK_REGISTRATION,
	OFONO_MODEM_INTERFACE_PHONEBOOK,
	OFONO_MODEM_INTERFACE_PUSH_NOTIFICATION,
	OFONO_MODEM_INTERFACE_RADIO_SETTINGS,
	OFONO_MODEM_INTERFACE_SIM_MANAGER,
	OFONO_MODEM_INTERFACE_SMART_MESSAGING,
	OFONO_MODEM_INTERFACE_SIM_TOOLKIT,
	OFONO_MODEM_INTERFACE_SUPPLEMENTARY_SERVICES,
	OFONO_MODEM_INTERFACE_TEXT_TELEPHONY,
	OFONO_MODEM_INTERFACE_VOICE_CALL_MANAGER,
	OFONO_MODEM_INTERFACE_MAX,
};

struct ofono_modem* ofono_modem_create(const gchar *path);
void ofono_modem_ref(struct ofono_modem *modem);
void ofono_modem_unref(struct ofono_modem *modem);
void ofono_modem_free(struct ofono_modem *modem);

const gchar* ofono_modem_get_path(struct ofono_modem *modem);

int ofono_modem_set_powered(struct ofono_modem *modem, gboolean powered, ofono_modem_result_cb cb, void *data);
bool ofono_modem_get_powered(struct ofono_modem *modem);
int ofono_modem_set_online(struct ofono_modem *modem, gboolean online, ofono_modem_result_cb cb, void *data);
bool ofono_modem_get_online(struct ofono_modem *modem);
const gchar* ofono_modem_get_serial(struct ofono_modem *modem);
const gchar* ofono_modem_get_revision(struct ofono_modem *modem);
bool ofono_modem_is_interface_supported(struct ofono_modem *modem, enum ofono_modem_interface interface);

#endif

// vim:ts=4:sw=4:noexpandtab
