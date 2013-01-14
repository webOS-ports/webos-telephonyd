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

#include <glib.h>
#include <lunaprefs.h>
#include <pbnjson.h>

#include "telephonysettings.h"

#define TELEPHONY_LUNA_PREFS_ID		"com.palm.telephony"

static const char *setting_keys[] = {
	"telephonyPowerState",
};

const char* telephony_settings_load(enum telephony_settings_type type)
{
	LPErr lperr = LP_ERR_NONE;
	LPAppHandle handle;
	char *setting_value = NULL;

	lperr = LPAppGetHandle(TELEPHONY_LUNA_PREFS_ID, &handle);
	if (lperr) {
		g_message("Failed to retrieve telephony settings handle");
		return NULL;
	}

	lperr = LPAppCopyValue(handle, setting_keys[type], &setting_value);
	LPAppFreeHandle(handle, false);
	if (lperr) {
		g_message("Failed in executing LPAppCopyValue for %s", setting_keys[type]);
		return NULL;
	}

	return setting_value;
}

bool telephony_settings_store(enum telephony_settings_type type, const char *data)
{
	LPErr lperr = LP_ERR_NONE;
	LPAppHandle handle;

	lperr = LPAppGetHandle(TELEPHONY_LUNA_PREFS_ID, &handle);
	if (lperr) {
		g_message("Failed to retrieve telephony settings handle");
		return false;
	}

	lperr = LPAppSetValue(handle, setting_keys[type], data);
	LPAppFreeHandle(handle, true);
	if (lperr) {
		g_message("Failed to execute LPAppSetValue for %s", setting_keys[type]);
		return false;
	}

	return false;
}

// vim:ts=4:sw=4:noexpandtab
