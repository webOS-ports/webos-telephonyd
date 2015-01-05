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

#ifndef TELEPHONY_SETTINGS_H_
#define TELEPHONY_SETTINGS_H_

enum telephony_settings_type {
	TELEPHONY_SETTINGS_TYPE_POWER_STATE = 0,
	TELEPHONY_SETTINGS_TYPE_DISABLE_WAN = 1,
};

const char* telephony_settings_load(enum telephony_settings_type type);
bool telephony_settings_store(enum telephony_settings_type type, const char *data);

#endif

// vim:ts=4:sw=4:noexpandtab
