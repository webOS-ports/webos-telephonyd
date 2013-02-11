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

#ifndef OFONO_RADIO_SETTING_H_
#define OFONO_RADIO_SETTINGS_H_

#include <glib.h>

struct ofono_radio_settings;

enum ofono_radio_access_mode {
	OFONO_RADIO_ACCESS_MODE_ANY = 0,
	OFONO_RADIO_ACCESS_MODE_GSM,
	OFONO_RADIO_ACCESS_MODE_UMTS,
	OFONO_RADIO_ACCESS_MODE_LTE,
	OFONO_RADIO_ACCESS_MODE_UNKNOWN
};

struct ofono_radio_settings* ofono_radio_settings_create(const char *path);
void ofono_radio_settings_free(struct ofono_radio_settings *ras);

void ofono_radio_settings_set_technology_preference(struct ofono_radio_settings *ras,
													enum ofono_radio_access_mode mode, ofono_base_result_cb cb,
													void *data);
enum ofono_radio_access_mode ofono_radio_settings_get_technology_preference(struct ofono_radio_settings *ras);

#endif

// vim:ts=4:sw=4:noexpandtab
